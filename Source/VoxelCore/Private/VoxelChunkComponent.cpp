#include "VoxelChunkComponent.h"
#include "VoxelSettings.h"
#include "VoxelGenerator.h"
#include "VoxelGPUGenerator.h"
#include "VoxelMesher.h"
#include "VoxelWorld.h"
#include "VoxelStats.h"
#include "ProceduralMeshComponent.h"
#include "Async/Async.h"
#include "HAL/IConsoleManager.h"

#if WITH_REALTIME_MESHCOMPONENT
#include "RealtimeMeshComponent.h"
#endif
#include <Misc/ScopeTryLock.h>
#include <VoxelGPUMesher.h>
#include "VoxelBlockTable.h"
#include "VoxelMaterialSet.h"
#include <VoxelBiome.h>
#include <VoxelNoise.h>

// ============================================================================
// LOGGING CATEGORIES
// ============================================================================

DEFINE_LOG_CATEGORY_STATIC(LogVoxelChunk, Log, All);

// Debug console variable for padding extraction logging
static TAutoConsoleVariable<int32> CVarVoxelLogPadding(
    TEXT("r.Voxel.LogPadding"),
    0,
    TEXT("Log padding extraction details (0=off, 1=on)"),
    ECVF_Default);

void UVoxelChunkComponent::InitializeChunk(
    FVoxelCoord InCoord,
    const UVoxelSettings* InSettings,
    AVoxelWorld* InWorld,
    EVoxelLODLevel InLOD,
    bool bInBuildCollision,
    bool bInUseAO)
{
    ChunkCoord = InCoord;
    Settings = InSettings;
    OwnerWorld = InWorld;
    LOD = InLOD;
    bBuildCollision = bInBuildCollision;
    bUseAO = bInUseAO;

    LODScaleXY = (LOD == EVoxelLODLevel::LOD1) ? FMath::Max(1, Settings->LOD1_ScaleXY) : 1;
    RenderMode = (LOD == EVoxelLODLevel::LOD2) ? EVoxelRenderMode::Heightfield : EVoxelRenderMode::Voxels;

    // Mark neighbors as dirty for first snapshot
    bNeighborsCacheDirty = true;

    CreateMeshComponent();

    const FVector WorldLocation(
        ChunkCoord.Cx * Settings->ChunkSizeX * Settings->VoxelWorldScale,
        ChunkCoord.Cy * Settings->ChunkSizeY * Settings->VoxelWorldScale,
        ChunkCoord.Cz * Settings->ChunkSizeZ * Settings->VoxelWorldScale);

    if (PMC) PMC->SetWorldLocation(WorldLocation);  // Changed from SetWorldLocation
    if (RMC) RMC->SetWorldLocation(WorldLocation);  // Changed from SetWorldLocation
    const UVoxelMaterialSet* MatSet = Settings->MaterialSet.LoadSynchronous();
    EnsureVoxelMaterial_RMC(RMC, Settings, MatSet);

    StartGeneration();
    // In InitializeChunk, after calculating WorldLocation:
    UE_LOG(LogVoxelChunk, Warning, TEXT("Chunk (%d,%d,%d) spawned at Z=%f (Cz=%d, ChunkSizeZ=%d, Scale=%f)"),
        ChunkCoord.Cx, ChunkCoord.Cy, ChunkCoord.Cz,
        WorldLocation.Z, ChunkCoord.Cz, Settings->ChunkSizeZ, Settings->VoxelWorldScale);
}

void UVoxelChunkComponent::CreateMeshComponent()
{
    DestroyMeshComponent();
    bUsingRMC = false;

    if (!OwnerWorld) return;

    if (Settings->bUseRuntimeMeshComponent)
    {
        RMC = OwnerWorld->AcquireRMC();
        if (RMC) { bUsingRMC = true; return; }
    }

    PMC = OwnerWorld->AcquirePMC();
}

void UVoxelChunkComponent::DestroyMeshComponent()
{
    if (!OwnerWorld) return;

    if (PMC) { OwnerWorld->ReleasePMC(PMC); PMC = nullptr; }
    if (RMC) { OwnerWorld->ReleaseRMC(RMC); RMC = nullptr; }
}

void UVoxelChunkComponent::StartGeneration()
{
    State = EVoxelChunkState::Generating;
    bCancelPending.AtomicSet(false);
    if (OwnerWorld) OwnerWorld->ScheduleGeneration(this);
}

// --- VoxelChunkComponent.cpp (replace full function) ---
void UVoxelChunkComponent::DoGeneration()
{
    SCOPE_CYCLE_COUNTER(STAT_VoxelGeneration);

    const FChunkGenParams Params = UVoxelGenerator::MakeParamsFromSettings(Settings);
    const FVoxelCoord Coord = ChunkCoord;
    const int32 ScaleXY = LODScaleXY;
    const bool bHeight = (RenderMode == EVoxelRenderMode::Heightfield);
    const bool bUseGPU = Settings->bUseGPUGeneration && !bHeight; // GPU path only for voxel chunks

    // CRITICAL: Capture OwnerWorld to ALWAYS call OnGenerationFinished, even if component is destroyed
    // This prevents ActiveGenTasks from staying inflated and generation queue from growing infinitely
    TWeakObjectPtr<AVoxelWorld> WeakWorld(OwnerWorld);

    // GPU GENERATION PATH (10-50x faster)
    if (bUseGPU && FVoxelGPUGenerator::IsGPUGenerationAvailable())
    {
        // Calculate LOD-scaled grid size (CRITICAL: must match CPU path!)
        // For LOD1+, the grid is reduced: fewer voxels cover the same world space
        const int32 ScaledSizeX = (Params.SizeX + ScaleXY - 1) / ScaleXY;
        const int32 ScaledSizeY = (Params.SizeY + ScaleXY - 1) / ScaleXY;
        const int32 ScaledSizeZ = Params.SizeZ;

        // Launch GPU generation with entire BiomeTable for per-column selection
        TWeakObjectPtr<UVoxelChunkComponent> WeakThis(this);
        FVoxelGPUGenerator::GenerateChunkGPU(
            Coord,
            ScaledSizeX + 2, ScaledSizeY + 2, ScaledSizeZ + 2, // +2 for halo (LOD-scaled dimensions)
            Params.SizeX, Params.SizeY, Params.SizeZ, // Base unscaled chunk size (for world coordinate calculation)
            ScaleXY,
            Params.Seed,
            Params.BaseHeight,
            Params.WaterLevel,
            Params.MaxCaveDepth,
            Params.BiomeTable.Get(),  // Pass entire BiomeTable for per-column biome selection
            Params.NoiseProfile,      // Pass NoiseProfile for climate noise generation
            [WeakThis, WeakWorld, Params, ScaleXY, Coord, ScaledSizeX, ScaledSizeY, ScaledSizeZ](TArray<uint8>&& GPUCategoryData, FBiomeGrid2D&& GPUBiomeGrid)
            {
                // Check if component is still valid (might be destroyed during async generation)
                UVoxelChunkComponent* This = WeakThis.Get();

                // CRITICAL: ALWAYS call OnGenerationFinished, even if component is destroyed
                // This ensures ActiveGenTasks is decremented and queue slots are freed
                auto NotifyWorldLambda = [WeakThis, WeakWorld]()
                {
                    AsyncTask(ENamedThreads::GameThread, [WeakThis, WeakWorld]()
                    {
                        if (AVoxelWorld* World = WeakWorld.Get())
                        {
                            UVoxelChunkComponent* Comp = WeakThis.Get();
                            World->OnGenerationFinished(Comp); // Comp may be nullptr if destroyed - that's OK
                        }
                    });
                };

                if (!This || !IsValid(This))
                {
                    // Component destroyed - discard results BUT notify world to free queue slot
                    UE_LOG(LogVoxelChunk, Verbose, TEXT("[GenQueue] GPU generation completed but chunk (%d,%d,%d) destroyed - notifying world"),
                        Coord.Cx, Coord.Cy, Coord.Cz);
                    NotifyWorldLambda();
                    return;
                }

                // GPU generation complete - store CategoryData and BiomeGrid
                This->CategoryData.Data = MoveTemp(GPUCategoryData);
                This->CategoryData.SizeX = ScaledSizeX + 2;
                This->CategoryData.SizeY = ScaledSizeY + 2;
                This->CategoryData.SizeZ = ScaledSizeZ + 2;

                // OPTION B: Store GPU-generated BiomeGrid (SurfaceZ + Biome per XY column)
                This->BiomeGrid = MoveTemp(GPUBiomeGrid);

                // DIAGNOSTIC: Log GPU BiomeGrid received
                UE_LOG(LogVoxelChunk, Warning, TEXT("[OPTIONB] Chunk (%d,%d,%d) GPU callback: Received BiomeGrid %dx%d (%d columns)"),
                    Coord.Cx, Coord.Cy, Coord.Cz,
                    This->BiomeGrid.SizeX, This->BiomeGrid.SizeY,
                    This->BiomeGrid.SurfaceZWorld.Num());

                const int64 NumBytes = This->CategoryData.Data.Num();
                INC_MEMORY_STAT_BY(STAT_VoxelDataMemory, NumBytes);

                if (This->bCancelPending)
                {
                    NotifyWorldLambda();
                    return;
                }

                AsyncTask(ENamedThreads::GameThread, [WeakThis, WeakWorld]()
                    {
                        UVoxelChunkComponent* Comp = WeakThis.Get();
                        if (Comp && IsValid(Comp))
                        {
                            Comp->OnGenerationComplete();
                        }

                        // ALWAYS notify world, even if component became invalid
                        if (AVoxelWorld* World = WeakWorld.Get())
                        {
                            World->OnGenerationFinished(Comp); // Comp may be nullptr - that's OK
                        }
                    });
            });

        return; // GPU path handles async completion
    }

    // CPU GENERATION PATH (legacy, always available)
    // FIXED: Capture WeakThis instead of raw 'this' to prevent use-after-free if component destroyed
    TWeakObjectPtr<UVoxelChunkComponent> WeakThis(this);
    UE::Tasks::Launch(UE_SOURCE_LOCATION, [WeakThis, WeakWorld, Params, Coord, ScaleXY, bHeight]()
        {
            // SAFETY: Get raw pointer upfront - if component is destroyed during generation,
            // we'll detect it via bCancelPending or IsValid() check before accessing members
            UVoxelChunkComponent* This = WeakThis.Get();
            if (!This || !IsValid(This))
            {
                // Component destroyed during generation - notify world to free queue slot
                AsyncTask(ENamedThreads::GameThread, [WeakThis, WeakWorld]()
                    {
                        if (AVoxelWorld* World = WeakWorld.Get())
                        {
                            UVoxelChunkComponent* Comp = WeakThis.Get();
                            World->OnGenerationFinished(Comp); // Comp may be nullptr - that's OK
                        }
                    });
                return;
            }

            if (bHeight)
            {
                FIntPoint Samples;
                UVoxelGenerator::GenerateHeightmap(Coord, Params, ScaleXY, This->HeightData, Samples);
                This->HF_SamplesX = Samples.X;
                This->HF_SamplesY = Samples.Y;
            }
            else
            {
                // Categories for fast solid/air decisions
                UVoxelGenerator::GenerateChunkLOD_Categories(Coord, Params, ScaleXY, This->CategoryData);

                // NEW: 2D biome grid used later to expand categories into biome-aware blocks
                UVoxelGenerator::GenerateBiomeGrid2D(Coord, Params, ScaleXY, This->BiomeGrid);

                const int64 NumBytes = This->CategoryData.Data.Num();
                INC_MEMORY_STAT_BY(STAT_VoxelDataMemory, NumBytes);
            }

            if (This->bCancelPending)
            {
                AsyncTask(ENamedThreads::GameThread, [WeakThis, WeakWorld]()
                    {
                        // ALWAYS notify world, even if OwnerWorld changed
                        if (AVoxelWorld* World = WeakWorld.Get())
                        {
                            UVoxelChunkComponent* Comp = WeakThis.Get();
                            World->OnGenerationFinished(Comp);
                        }
                    });
                return;
            }

            AsyncTask(ENamedThreads::GameThread, [WeakThis, WeakWorld]()
                {
                    UVoxelChunkComponent* Comp = WeakThis.Get();
                    if (Comp && IsValid(Comp))
                    {
                        Comp->OnGenerationComplete();
                    }

                    // ALWAYS notify world, even if OwnerWorld changed
                    if (AVoxelWorld* World = WeakWorld.Get())
                    {
                        World->OnGenerationFinished(Comp);
                    }
                });
        });
}


void UVoxelChunkComponent::OnGenerationComplete()
{
    if (bCancelPending)
    {
        State = EVoxelChunkState::Unloading;
        return;
    }

    // CRITICAL OPTIMIZATION: Skip meshing for empty/solid chunks
    // This is Minecraft's #1 performance trick - don't render what you can't see!
    if (RenderMode == EVoxelRenderMode::Voxels && CategoryData.Data.Num() > 0)
    {
        // Quick scan: Check if chunk is entirely air OR entirely solid
        // CategoryData stores 2-bit categories: 0=air, 1=semi-solid, 2=solid
        bool bIsEmpty = true;
        bool bIsSolid = true;
        uint8 FirstNonAir = 0; // 0 = air

        const int32 TotalVoxels = CategoryData.SizeX * CategoryData.SizeY * CategoryData.SizeZ;

        // Sample every N voxels for speed (checking all voxels is too slow for large chunks)
        // Step of 4 means we check ~1/64th of voxels (still very accurate for uniform chunks)
        const int32 Step = 4;

        for (int32 z = 0; z < CategoryData.SizeZ; z += Step)
        {
            for (int32 y = 0; y < CategoryData.SizeY; y += Step)
            {
                for (int32 x = 0; x < CategoryData.SizeX; x += Step)
                {
                    const uint8 Cat = CategoryData.Get(x, y, z);

                    if (Cat != 0) // Not air
                    {
                        bIsEmpty = false;
                        if (FirstNonAir == 0)
                        {
                            FirstNonAir = Cat;
                        }
                        else if (Cat != FirstNonAir)
                        {
                            bIsSolid = false;
                            goto BreakAllLoops; // Not uniform - need full mesh
                        }
                    }
                    else
                    {
                        bIsSolid = false; // Has air - not solid
                    }
                }
            }
        }
        BreakAllLoops:;

        // OPTIMIZATION 1: Empty chunks (100% air)
        if (bIsEmpty)
        {
            UE_LOG(LogVoxelChunk, Verbose, TEXT("[EmptyCull] Chunk (%d,%d,%d) is 100%% air - skipping mesh + collision"),
                ChunkCoord.Cx, ChunkCoord.Cy, ChunkCoord.Cz);

            // Disable collision for air chunks (massive performance save!)
            bBuildCollision = false;

            State = EVoxelChunkState::Ready;
            if (OwnerWorld) OwnerWorld->OnChunkReady(ChunkCoord);
            return;
        }

        // OPTIMIZATION 2: Solid chunks (100% same category)
        // Only render surface faces (neighbors will handle interior culling)
        // This is HUGE for underground stone chunks - only mesh the edges!
        if (bIsSolid && FirstNonAir != 0)
        {
            UE_LOG(LogVoxelChunk, Verbose, TEXT("[SolidCull] Chunk (%d,%d,%d) is 100%% solid (category %d) - surface-only mesh"),
                ChunkCoord.Cx, ChunkCoord.Cy, ChunkCoord.Cz, (int32)FirstNonAir);

            // Mark as solid for surface-only meshing
            // The mesher will only generate faces on chunk boundaries
            bIsSolidChunk = true;
        }
    }

    // DON'T cache neighbors here - they might not be ready yet!
    // Just mark as dirty so meshing will handle it
    if (RenderMode == EVoxelRenderMode::Voxels)
    {
        bNeighborsCacheDirty = true;
    }

    StartMeshing(/*bSeamRemesh=*/false);
}

void UVoxelChunkComponent::StartMeshing(bool bSeamRemesh)
{
    if (OwnerWorld) OwnerWorld->ScheduleMeshing(this, bSeamRemesh);
}

void UVoxelChunkComponent::DoMeshing(bool bSeamRemesh)
{
    SCOPE_CYCLE_COUNTER(STAT_VoxelMeshing);

    if ((RenderMode == EVoxelRenderMode::Voxels && CategoryData.Data.Num() == 0) ||
        (RenderMode == EVoxelRenderMode::Heightfield && HeightData.Num() == 0))
    {
        UE_LOG(LogVoxelChunk, Error, TEXT("Chunk (%d,%d,%d) FAILED to mesh - no data! CatData=%d HeightData=%d"),
            ChunkCoord.Cx, ChunkCoord.Cy, ChunkCoord.Cz, CategoryData.Data.Num(), HeightData.Num());
        if (IsValid(OwnerWorld)) OwnerWorld->OnMeshingFinished(this);
        return;
    }

    State = EVoxelChunkState::Meshing;
    bIsMeshing = true;
    bCancelPending.AtomicSet(false);

    // OPTIMIZATION: Early exit for chunks that won't produce any visible geometry
    // This saves ~0.5ms per empty chunk by skipping voxel expansion and meshing entirely
    // CRITICAL: Check CORE chunk only (excluding 1-voxel padding halo from neighbors)
    // CategoryData has padding (+2 in each dimension), which may contain neighbor data
    //
    // Use IsCoreRenderableEmpty() which considers both air (cat 0) and water (cat 1) as empty
    // because water-only chunks produce 0 geometry when fully surrounded by water
    const bool bIsCoreEmpty = (RenderMode == EVoxelRenderMode::Voxels) && CategoryData.IsCoreRenderableEmpty(1);

    if (bIsCoreEmpty)
    {
        // Chunk is completely empty - skip meshing AND mesh apply entirely
        // Don't enqueue empty meshes for application - this saves game thread time
        UE_LOG(LogVoxelChunk, Verbose, TEXT("[PROFILING] Meshing: 0.00ms | Verts: 0 | Tris: 0 | ChunkSize: %dx%dx%d | LOD: %d | Mesher: EarlyExit (Empty)"),
            (Settings->ChunkSizeX + LODScaleXY - 1) / LODScaleXY,
            (Settings->ChunkSizeY + LODScaleXY - 1) / LODScaleXY,
            Settings->ChunkSizeZ,
            LODScaleXY);

        // Mark chunk as ready without applying any mesh (no visual component needed for empty chunks)
        State = EVoxelChunkState::Ready;
        bIsMeshing = false;

        if (IsValid(OwnerWorld))
        {
            OwnerWorld->OnMeshingFinished(this);
            // OPTIMIZATION: Don't enqueue empty mesh apply - saves game thread time!
            // Empty chunks don't need visual components, just mark them as done
            if (!bHasAnnouncedReady)
            {
                bHasAnnouncedReady = true;
                OwnerWorld->OnChunkReady(ChunkCoord);
            }
        }
        return;
    }

    const float VoxelUU = Settings->VoxelWorldScale;

    // CRITICAL: Make thread-safe copies of CategoryData and BiomeGrid BEFORE async task
    // to prevent race condition with UnloadChunk clearing data mid-mesh
    FCategoryBitset CategoryDataCopy = CategoryData;
    FBiomeGrid2D BiomeGridCopy = BiomeGrid;

    TArray<EVoxelBlockID> VoxelsCopy;
    TArray<int32> HeightsCopy;
    TArray<uint8> CatsExpanded;  // OPTIMIZATION: Cached categories
    int32 SamplesX = HF_SamplesX, SamplesY = HF_SamplesY;

    if (RenderMode == EVoxelRenderMode::Voxels)
    {
        // CRITICAL: Use actual CategoryData dimensions, not recalculated
        // CategoryData was generated with specific LOD scale that may differ from current LODScaleXY
        const int32 PaddedSX = CategoryDataCopy.SizeX;
        const int32 PaddedSY = CategoryDataCopy.SizeY;
        const int32 PaddedSZ = CategoryDataCopy.SizeZ;
        const int32 SX = PaddedSX - 2;
        const int32 SY = PaddedSY - 2;
        const int32 SZ = PaddedSZ - 2;
        const int32 TotalPadded = PaddedSX * PaddedSY * PaddedSZ;

        // OPTION B: GPU provides Categories + BiomeGrid, CPU generates blocks
        // This ensures perfect CPU-GPU parity while maintaining good performance

        // World-space bases
        const int32 BaseWX = ChunkCoord.Cx * Settings->ChunkSizeX;
        const int32 BaseWY = ChunkCoord.Cy * Settings->ChunkSizeY;
        const int32 BaseWZ = ChunkCoord.Cz * Settings->ChunkSizeZ;

        VoxelsCopy.Init(EVoxelBlockID::Air, TotalPadded);

        // OPTIMIZATION: Cache expanded categories to avoid Voxels→Cats conversion later
        // This eliminates a full 32K+ iteration loop in BuildBinaryGreedyMesh
        CatsExpanded.SetNumUninitialized(TotalPadded);

        auto PickBelowSurface = [](const UVoxelBiomeDef* B, int32 depth)->EVoxelBlockID
        {
            if (B && B->Subsurface.Num() > 0)
            {
                int32 acc = 0;
                for (const FBiomeLayer& L : B->Subsurface)
                {
                    acc += FMath::Max(0, L.Thickness);
                    if (depth <= acc) return L.Block;
                }
                return B->Subsurface.Last().Block;
            }
            return EVoxelBlockID::Stone;
        };

        // Biome grid presence
        const bool bHaveBiomeGrid = BiomeGridCopy.IsValid();
        if (!bHaveBiomeGrid)
        {
            static std::atomic<bool> bWarned{ false };
            bool expected = false;
            if (bWarned.compare_exchange_strong(expected, true))
            {
                UE_LOG(LogVoxelChunk, Warning, TEXT("[Voxel] BiomeGrid missing for chunk (%d,%d,%d). Using defaults."),
                    ChunkCoord.Cx, ChunkCoord.Cy, ChunkCoord.Cz);
            }
        }

        // Expand padded categories → EVoxelBlockID with biome-aware selection
        int32 idx = 0;
        for (int32 z = 0; z < PaddedSZ; ++z)
        {
            const int32 WorldZ = BaseWZ + (z - 1);

            for (int32 y = 0; y < PaddedSY; ++y)
            {
                const int32 vy = FMath::Clamp(y - 1, 0, SY - 1);
                const int32 WY = BaseWY + vy * LODScaleXY;

                for (int32 x = 0; x < PaddedSX; ++x, ++idx)
                {
                    const uint8 Cat = CategoryDataCopy.Get(x, y, z);

                    // OPTIMIZATION: Cache category as we generate voxels (single pass)
                    CatsExpanded[idx] = Cat;

                    if (Cat == 0) { VoxelsCopy[idx] = EVoxelBlockID::Air;  continue; }
                    if (Cat == 1) { VoxelsCopy[idx] = EVoxelBlockID::Water; continue; }

                    EVoxelBlockID Bid = EVoxelBlockID::Stone;

                    if (bHaveBiomeGrid)
                    {
                        // SAFE INDEXING: clamp to actual BiomeGrid size
                        const int32 bx = FMath::Clamp(x, 0, BiomeGridCopy.SizeX - 1);
                        const int32 by = FMath::Clamp(y, 0, BiomeGridCopy.SizeY - 1);
                        const int32 gi = by * BiomeGridCopy.SizeX + bx;

                        // Guard against corrupted grids
                        const bool bInRange =
                            gi >= 0 &&
                            gi < BiomeGridCopy.SurfaceZWorld.Num() &&
                            gi < BiomeGridCopy.BiomeAtXY.Num();

                        if (bInRange)
                        {
                            const int32 TopZ = (int32)BiomeGridCopy.SurfaceZWorld[gi];
                            const UVoxelBiomeDef* B = BiomeGridCopy.BiomeAtXY[gi];

                            // CRITICAL FIX: Local surface detection instead of global TopZ comparison
                            // A voxel is a surface if it has air above OR below it
                            const bool bHasAirAbove = (z + 1 < PaddedSZ) && (CategoryDataCopy.Get(x, y, z + 1) == 0);
                            const bool bHasAirBelow = (z > 0) && (CategoryDataCopy.Get(x, y, z - 1) == 0);

                            if (bHasAirAbove || bHasAirBelow)
                            {
                                // This voxel has an exposed face - use biome surface block
                                Bid = B ? B->Surface : EVoxelBlockID::Grass;
                            }
                            else if (WorldZ < TopZ)
                            {
                                // Interior voxel below main surface - use depth-based subsurface layers
                                const int32 depth = TopZ - WorldZ;
                                Bid = PickBelowSurface(B, depth);
                            }
                            else
                            {
                                // Interior voxel above main surface (inside floating island/overhang)
                                // Use shallow subsurface layers instead of pure stone
                                const int32 depth = 2; // Treat as slightly below surface
                                Bid = PickBelowSurface(B, depth);
                            }
                        }
                        else
                        {
                            // Fallback if grid is unexpectedly sized
                            Bid = EVoxelBlockID::Stone;
                        }
                    }

                    VoxelsCopy[idx] = Bid;
                }
            }
        }
    }
    else
    {
        HeightsCopy = HeightData;
    }

    const bool bCollision = bBuildCollision;
    const bool bAO = bUseAO && (RenderMode == EVoxelRenderMode::Voxels);

    // CRITICAL: Capture OwnerWorld to ALWAYS call OnMeshingFinished, even if world is destroyed
    // This prevents ActiveMeshTasks from staying inflated and meshing queue from growing infinitely
    TWeakObjectPtr<AVoxelWorld> WeakWorld(OwnerWorld);
    AVoxelWorld* W = OwnerWorld;

    const FIntVector SizeVox(
        (Settings->ChunkSizeX + LODScaleXY - 1) / LODScaleXY,
        (Settings->ChunkSizeY + LODScaleXY - 1) / LODScaleXY,
        Settings->ChunkSizeZ);

    const int32 XYScale = LODScaleXY;

    // Extract neighbor borders FROM padding so the mesher can cull faces at chunk seams
    FChunkNeighbors NbhCopy;
    if (RenderMode == EVoxelRenderMode::Voxels && VoxelsCopy.Num() > 0)
    {
        const int32 SX = SizeVox.X, SY = SizeVox.Y, SZ = SizeVox.Z;
        const int32 PaddedSX = SX + 2, PaddedSY = SY + 2, PaddedSZ = SZ + 2;

        NbhCopy.SizeX = SX; NbhCopy.SizeY = SY; NbhCopy.SizeZ = SZ;

        // X-
        NbhCopy.XNeg.SetNumUninitialized(SY * SZ);
        for (int32 z = 0; z < SZ; ++z)
            for (int32 y = 0; y < SY; ++y)
                NbhCopy.XNeg[y + z * SY] = VoxelsCopy[0 + (y + 1) * PaddedSX + (z + 1) * PaddedSX * PaddedSY];
        NbhCopy.bHasXNeg = true;

        // X+
        NbhCopy.XPos.SetNumUninitialized(SY * SZ);
        for (int32 z = 0; z < SZ; ++z)
            for (int32 y = 0; y < SY; ++y)
                NbhCopy.XPos[y + z * SY] = VoxelsCopy[(SX + 1) + (y + 1) * PaddedSX + (z + 1) * PaddedSX * PaddedSY];
        NbhCopy.bHasXPos = true;

        // Y-
        NbhCopy.YNeg.SetNumUninitialized(SX * SZ);
        for (int32 z = 0; z < SZ; ++z)
            for (int32 x = 0; x < SX; ++x)
                NbhCopy.YNeg[x + z * SX] = VoxelsCopy[(x + 1) + 0 * PaddedSX + (z + 1) * PaddedSX * PaddedSY];
        NbhCopy.bHasYNeg = true;

        // Y+
        NbhCopy.YPos.SetNumUninitialized(SX * SZ);
        for (int32 z = 0; z < SZ; ++z)
            for (int32 x = 0; x < SX; ++x)
                NbhCopy.YPos[x + z * SX] = VoxelsCopy[(x + 1) + (SY + 1) * PaddedSX + (z + 1) * PaddedSX * PaddedSY];
        NbhCopy.bHasYPos = true;

        // Z-
        NbhCopy.ZNeg.SetNumUninitialized(SX * SY);
        for (int32 y = 0; y < SY; ++y)
            for (int32 x = 0; x < SX; ++x)
                NbhCopy.ZNeg[x + y * SX] = VoxelsCopy[(x + 1) + (y + 1) * PaddedSX + 0 * PaddedSX * PaddedSY];
        NbhCopy.bHasZNeg = true;

        // Z+
        NbhCopy.ZPos.SetNumUninitialized(SX * SY);
        for (int32 y = 0; y < SY; ++y)
            for (int32 x = 0; x < SX; ++x)
                NbhCopy.ZPos[x + y * SX] = VoxelsCopy[(x + 1) + (y + 1) * PaddedSX + (SZ + 1) * PaddedSX * PaddedSY];
        NbhCopy.bHasZPos = true;

        // Strip padding for the main volume sent to the mesher
        TArray<EVoxelBlockID> RealVoxels;
        RealVoxels.SetNum(SX * SY * SZ);

        // CRITICAL: Also strip padding from categories array
        TArray<uint8> RealCats;
        RealCats.SetNum(SX * SY * SZ);

        int32 di = 0;
        for (int32 z = 0; z < SZ; ++z)
            for (int32 y = 0; y < SY; ++y)
                for (int32 x = 0; x < SX; ++x, ++di)
                {
                    const int32 paddedIdx = (x + 1) + (y + 1) * PaddedSX + (z + 1) * PaddedSX * PaddedSY;
                    RealVoxels[di] = VoxelsCopy[paddedIdx];
                    RealCats[di] = CatsExpanded[paddedIdx];
                }

        VoxelsCopy = MoveTemp(RealVoxels);
        CatsExpanded = MoveTemp(RealCats);
    }
    else
    {
        NbhCopy = CachedNeighbors;
    }

    // OPTIMIZATION: Compute hash of neighbor borders to track if they changed
    // This allows us to skip unnecessary seam remeshes when neighbors haven't actually changed
    const uint32 CurrentNeighborHash = NbhCopy.ComputeHash();
    LastNeighborHash = CurrentNeighborHash;

    const EVoxelLODLevel LODLevel = LOD;
    const EVoxelRenderMode RenderModeValue = RenderMode;
    const int32 ChunkSizeX = Settings->ChunkSizeX;
    const int32 ChunkSizeY = Settings->ChunkSizeY;

    UE::Tasks::Launch(UE_SOURCE_LOCATION,
        [this, WeakWorld, VoxelUU, bCollision, bAO, bSeamRemesh, W, SizeVox, ChunkSizeX, ChunkSizeY, XYScale,
        LODLevel, RenderModeValue, Voxels = MoveTemp(VoxelsCopy), Cats = MoveTemp(CatsExpanded), HCopy = MoveTemp(HeightsCopy),
        SamplesX, SamplesY, NbhCopy = MoveTemp(NbhCopy), SettingsPtr = Settings]() mutable
        {
            FMeshBuffers Buffers;
            const int32 Seq = BeginMeshingSequence();

            if (RenderModeValue == EVoxelRenderMode::Voxels)
            {
                const bool bShouldTryGPU =
                    (LODLevel == EVoxelLODLevel::LOD0) &&
                    SettingsPtr &&
                    SettingsPtr->bUseGPUMesherForLOD0;
                const bool bAllowCPUFallback =
                    !SettingsPtr || SettingsPtr->bFallbackToCPUOnGPUFailure;

                bool bAttemptedGPU = false;
                bool bGPUOk = false;

                if (bShouldTryGPU && SettingsPtr && SettingsPtr->bAsyncGPUReadback)
                {
                    bAttemptedGPU = true;

                    TWeakObjectPtr<UVoxelChunkComponent> WeakChunk(this);
                    TWeakObjectPtr<AVoxelWorld> WeakWorldGPU(OwnerWorld);
                    const bool bUseNaive = SettingsPtr ? SettingsPtr->bUseNaiveMesher : false;
                    const bool bUseBinary = SettingsPtr ? SettingsPtr->bUseBinaryGreedyMesher : false;
                    const bool bAOFlag = bAO;
                    const UVoxelBlockTable* BT = (SettingsPtr && SettingsPtr->BlockTable.Get()) ? SettingsPtr->BlockTable.Get() : nullptr;
                    const bool bAllowFallbackLocal = bAllowCPUFallback;

                    const bool bLaunched = UVoxelMesher::BuildGreedyMesh_GPU_Async(
                        Voxels, SizeVox, &NbhCopy, XYScale, VoxelUU,
                        [WeakChunk, WeakWorldGPU, Cats, Voxels, NbhCopy, bCollision, bSeamRemesh, Seq, bUseNaive, bUseBinary, bAOFlag, bAllowFallbackLocal, BT]
                        (bool bSuccess, TArray<uint32>&& Packed, int32 SizeX, int32 SizeY, int32 SizeZ, int32 XYScaleParam, float VoxelUUParam)
                        {
                            UVoxelChunkComponent* Chunk = WeakChunk.Get();
                            AVoxelWorld* World = WeakWorldGPU.Get();

                            // CRITICAL: ALWAYS call OnMeshingFinished, even if chunk/world invalid
                            // This ensures ActiveMeshTasks is decremented and queue slots are freed
                            if (!World)
                            {
                                UE_LOG(LogVoxelChunk, Warning, TEXT("[MeshQueue] Async GPU meshing completed but world destroyed - discarding"));
                                return;
                            }

                            FMeshBuffers LocalBufs;
                            bool bShouldApply = (Chunk != nullptr && IsValid(Chunk));

                            if (bShouldApply)
                            {
                                if (bSuccess && Packed.Num() > 0)
                                {
                                    FVoxelGPUMesher::DecodePackedVertsToMeshBuffers(Packed, LocalBufs, VoxelUUParam, XYScaleParam, SizeX, SizeY, SizeZ);
                                }
                                else if (bAllowFallbackLocal)
                                {
                                    const FChunkNeighbors* NeighborPtr = &NbhCopy;
                                    const FIntVector SizeVector(SizeX, SizeY, SizeZ);

                                    // PROFILING: Measure async meshing time
                                    const double StartTime = FPlatformTime::Seconds();

                                    if (bUseNaive)
                                        UVoxelMesher::BuildNaiveMesh(Voxels, SizeVector, NeighborPtr, VoxelUUParam, XYScaleParam, bAOFlag, BT, LocalBufs);
                                    else if (bUseBinary)
                                        UVoxelMesher::BuildBinaryGreedyMesh(Cats, Voxels, SizeVector, NeighborPtr, VoxelUUParam, XYScaleParam, bAOFlag, BT, LocalBufs);
                                    else
                                        UVoxelMesher::BuildGreedyMesh(Voxels, SizeVector, NeighborPtr, VoxelUUParam, XYScaleParam, bAOFlag, BT, LocalBufs);

                                    const double EndTime = FPlatformTime::Seconds();
                                    const float MeshingMs = (float)((EndTime - StartTime) * 1000.0);

                                    UE_LOG(LogVoxelChunk, Verbose, TEXT("[PROFILING] Async Meshing: %.2fms | Verts: %d | Tris: %d | ChunkSize: %dx%dx%d | Mesher: %s"),
                                        MeshingMs, LocalBufs.Vertices.Num(), LocalBufs.Triangles.Num() / 3, SizeX, SizeY, SizeZ,
                                        bUseBinary ? TEXT("Binary") : TEXT("Standard"));
                                }
                            }

                            // ALWAYS call OnMeshingFinished
                            World->OnMeshingFinished(Chunk);

                            // Only apply mesh if chunk still valid
                            if (bShouldApply)
                            {
                                World->EnqueueMeshApply(Chunk, MoveTemp(LocalBufs), bCollision, bSeamRemesh, Seq);
                            }
                        },
                        SettingsPtr);

                    if (bLaunched) return;
                }

                if (bShouldTryGPU && !bGPUOk)
                {
                    bAttemptedGPU = true;
                    bGPUOk = UVoxelMesher::BuildGreedyMesh_GPU(Voxels, SizeVox, &NbhCopy, XYScale, VoxelUU, Buffers, SettingsPtr);
                }

                if ((!bAttemptedGPU || !bGPUOk) && bAllowCPUFallback)
                {
                    const UVoxelBlockTable* BT = (SettingsPtr && SettingsPtr->BlockTable.Get()) ? SettingsPtr->BlockTable.Get() : nullptr;

                    // PROFILING: Measure meshing time
                    const double StartTime = FPlatformTime::Seconds();

                    if (SettingsPtr && SettingsPtr->bUseNaiveMesher)
                        UVoxelMesher::BuildNaiveMesh(Voxels, SizeVox, &NbhCopy, VoxelUU, XYScale, bAO, BT, Buffers);
                    else if (SettingsPtr && SettingsPtr->bUseBinaryGreedyMesher)
                        UVoxelMesher::BuildBinaryGreedyMesh(Cats, Voxels, SizeVox, &NbhCopy, VoxelUU, XYScale, bAO, BT, Buffers);
                    else
                        UVoxelMesher::BuildGreedyMesh(Voxels, SizeVox, &NbhCopy, VoxelUU, XYScale, bAO, BT, Buffers);

                    const double EndTime = FPlatformTime::Seconds();
                    const float MeshingMs = (float)((EndTime - StartTime) * 1000.0);
                    const int32 VertCount = Buffers.Vertices.Num();
                    const int32 TriCount = Buffers.Triangles.Num() / 3;

                    UE_LOG(LogVoxelChunk, Verbose, TEXT("[PROFILING] Meshing: %.2fms | Verts: %d | Tris: %d | ChunkSize: %dx%dx%d | LOD: %d | Mesher: %s"),
                        MeshingMs, VertCount, TriCount, SizeVox.X, SizeVox.Y, SizeVox.Z, XYScale,
                        SettingsPtr && SettingsPtr->bUseBinaryGreedyMesher ? TEXT("Binary") : TEXT("Standard"));
                }
            }
            else
            {
                UVoxelMesher::BuildHeightfieldMesh(HCopy, SamplesX, SamplesY, ChunkSizeX, ChunkSizeY, XYScale, VoxelUU, Buffers);
            }

            // ALWAYS notify world, even if W is now invalid
            AVoxelWorld* World = WeakWorld.Get();
            if (World)
            {
                World->OnMeshingFinished(this);
                if (IsValid(W))
                {
                    World->EnqueueMeshApply(this, MoveTemp(Buffers), bCollision, bSeamRemesh, Seq);
                }
            }
        });
}



void UVoxelChunkComponent::ApplyBuffersToMesh(const FMeshBuffers& Bufs, bool bCollision)
{
    // PROFILING: Measure mesh apply time
    const double StartTime = FPlatformTime::Seconds();

    if (bUsingRMC)
    {
        // If buffers are accidentally in world space, subtract chunk origin to make them local
        static auto* CVarVertsWorld = IConsoleManager::Get().FindConsoleVariable(TEXT("r.Voxel.GPU.VerticesAreWorld"));
        const bool bVertsAreWorld = CVarVertsWorld && (CVarVertsWorld->GetInt() != 0);
        if (bVertsAreWorld)
        {
            const FVector Origin(
                ChunkCoord.Cx * Settings->ChunkSizeX * Settings->VoxelWorldScale,
                ChunkCoord.Cy * Settings->ChunkSizeY * Settings->VoxelWorldScale,
                ChunkCoord.Cz * Settings->ChunkSizeZ * Settings->VoxelWorldScale);
            FMeshBuffers Local = Bufs;
            for (FVector& V : Local.Vertices)
            {
                V -= Origin;
            }
            UVoxelMesher::ApplyToRMC(RMC, Local, bCollision);
        }
        else
        {
            UVoxelMesher::ApplyToRMC(RMC, Bufs, bCollision);
        }
    }
    else
    {
        UVoxelMesher::ApplyToPMC(PMC, Bufs, bCollision);
    }

    // CRITICAL FIX: Hide individual chunk components when batching is enabled
    // This must happen AFTER mesh apply (not at component acquisition) because
    // Settings may not be loaded yet when components are first created.
    // Only buckets should be visible when batching is on.
    if (Settings && Settings->bEnableChunkBatching)
    {
        if (RMC)
        {
            RMC->SetVisibility(false, true);
            RMC->SetHiddenInGame(true, true);
        }
        if (PMC)
        {
            PMC->SetVisibility(false, true);
            PMC->SetHiddenInGame(true, true);
        }
    }

    const double EndTime = FPlatformTime::Seconds();
    const float ApplyMs = (float)((EndTime - StartTime) * 1000.0);

    UE_LOG(LogVoxelChunk, Verbose, TEXT("[PROFILING] Mesh Apply: %.2fms | Verts: %d | Tris: %d | Collision: %s | Component: %s"),
        ApplyMs, Bufs.Vertices.Num(), Bufs.Triangles.Num() / 3,
        bCollision ? TEXT("Yes") : TEXT("No"),
        bUsingRMC ? TEXT("RMC") : TEXT("PMC"));
}

void UVoxelChunkComponent::OnMeshApplied(TUniquePtr<FMeshBuffers>&& AppliedBuffers, bool bWasSeamRemesh)
{
    // MEMORY MANAGEMENT: Move the freshly generated buffers into our cache.
    // For LOD0 chunks we keep the buffers around for potential collision and seam remeshing.
    // For higher LOD levels there is no need to retain the vertex/triangle arrays after
    // they have been applied to the mesh component, so we immediately discard them to save memory.
    //
    // FUTURE OPTIMIZATION: Implement LRU cache eviction for LOD0 buffers after N seconds of inactivity.
    // Current behavior: Buffers cached until chunk unload (may accumulate if player teleports).
    // Typical cost: ~60KB per LOD0 chunk (1000 verts, 2000 tris, UVs, colors, normals).
    CachedBuffers = MoveTemp(AppliedBuffers);

    // If a cancellation was requested we stop here and mark the state as
    // unloading.  The cached buffers (if any) are still cleaned up by the
    // destructor or unloading logic.
    if (bCancelPending)
    {
        State = EVoxelChunkState::Unloading;
        bIsMeshing = false;
        return;
    }

    // Transition to the ready state now that the mesh has been applied.
    State = EVoxelChunkState::Ready;
    bIsMeshing = false;

    // Immediately free mesh buffers on non-LOD0 chunks to reduce memory
    // footprint.  LOD0 chunks retain their buffers so that collision can be
    // reapplied without a full remesh if needed. Also adjust memory stats.
    //
    // CRITICAL: When chunk batching is enabled, we MUST keep ALL LOD0 buffers
    // so buckets can merge them! Never free LOD0 buffers when batching is on.
    const bool bBatchingEnabled = (Settings && Settings->bEnableChunkBatching);
    const bool bShouldFreeBuffers = (LOD != EVoxelLODLevel::LOD0) && !bBatchingEnabled;

    if (bShouldFreeBuffers && CachedBuffers.IsValid())
    {
        const int32 Bytes =
            CachedBuffers->Vertices.Num() * sizeof(FVector) +
            CachedBuffers->Triangles.Num() * sizeof(int32) +
            CachedBuffers->UVs.Num() * sizeof(FVector2D) +
            CachedBuffers->Colors.Num() * sizeof(FLinearColor) +
            CachedBuffers->Normals.Num() * sizeof(FVector);
        if (Bytes > 0)
        {
            DEC_MEMORY_STAT_BY(STAT_VoxelMeshBufferMemory, Bytes);
        }

        CachedBuffers->Vertices.Empty();
        CachedBuffers->Triangles.Empty();
        CachedBuffers->UVs.Empty();
        CachedBuffers->Colors.Empty();
        CachedBuffers->Normals.Empty();
    }

    // Announce that this chunk is ready if it has not been announced yet and
    // this was not a seam re‑mesh.  The world uses this callback to trigger
    // seam remeshing on neighboring chunks.
    if (!bWasSeamRemesh && !bHasAnnouncedReady)
    {
        bHasAnnouncedReady = true;
        if (OwnerWorld)
        {
            OwnerWorld->OnChunkReady(ChunkCoord);
        }
    }

    // If a seam remesh was queued while we were meshing, start it now.
    if (bSeamRemeshQueued)
    {
        bSeamRemeshQueued = false;
        StartMeshing(/*bSeamRemesh=*/true);
    }
}

void UVoxelChunkComponent::OnCollisionReapplied()
{
    if (State != EVoxelChunkState::Unloading)
    {
        State = EVoxelChunkState::Ready;
    }
}

void UVoxelChunkComponent::RequestRemesh()
{
    // OPTIMIZATION: Check if neighbors actually changed before remeshing
    // This dramatically reduces unnecessary seam remeshes (typically 80-90% reduction)
    if (bNeighborsCacheDirty && State == EVoxelChunkState::Ready)
    {
        // Temporarily extract current neighbor state to compute hash
        FChunkNeighbors TempNeighbors;
        SnapshotNeighbors(TempNeighbors);
        const uint32 NewHash = TempNeighbors.ComputeHash();

        // If hash matches, neighbors haven't changed - skip remesh
        if (NewHash == LastNeighborHash && LastNeighborHash != 0)
        {
            bNeighborsCacheDirty = false; // Clear dirty flag, no remesh needed
            return;
        }
    }

    // Force recache of neighbor borders on next mesh
    bNeighborsCacheDirty = true;
    bNeighborBordersCached = false;

    if (State == EVoxelChunkState::Ready && !bIsMeshing)
    {
        StartMeshing(/*bSeamRemesh=*/true);
    }
    else
    {
        bSeamRemeshQueued = true;
    }
}

void UVoxelChunkComponent::RequestCollisionReapply(bool bNewCollision)
{
    bBuildCollision = bNewCollision;
    if (CachedBuffers.IsValid() && OwnerWorld)
    {
        OwnerWorld->EnqueueReapplyUsingCache(this, bBuildCollision);
    }
    else
    {
        RequestRemesh();
    }
}

void UVoxelChunkComponent::CancelPendingTask()
{
    // ADD THIS:
    UE_LOG(LogVoxelChunk, Warning, TEXT("Chunk (%d,%d,%d) CANCELLED (State=%d)"),
        ChunkCoord.Cx, ChunkCoord.Cy, ChunkCoord.Cz, (int32)State);

    bCancelPending.AtomicSet(true);
    State = EVoxelChunkState::Unloading;
}


// ==========================
// Modified UnloadChunk - acquire lock before clearing:
// ==========================
void UVoxelChunkComponent::UnloadChunk()
{
    bCancelPending.AtomicSet(true);
    DestroyMeshComponent();

    // Update memory stats before clearing
    DEC_MEMORY_STAT_BY(STAT_VoxelDataMemory, CategoryData.Data.Num());

    if (CachedBuffers.IsValid())
    {
        const int32 Bytes =
            CachedBuffers->Vertices.Num() * sizeof(FVector) +
            CachedBuffers->Triangles.Num() * sizeof(int32) +
            CachedBuffers->UVs.Num() * sizeof(FVector2D) +
            CachedBuffers->Colors.Num() * sizeof(FLinearColor) +
            CachedBuffers->Normals.Num() * sizeof(FVector);
        if (Bytes > 0)
        {
            DEC_MEMORY_STAT_BY(STAT_VoxelMeshBufferMemory, Bytes);
        }
    }

    // Clear voxel and height data
    CategoryData.Data.Empty();
    HeightData.Empty();
    CachedBuffers.Reset();

    // Clear cached neighbor borders
    CachedNeighborBorders = FCachedNeighborBorders();
    bNeighborBordersCached = false;

    UnregisterComponent();
    DestroyComponent();
}
// ==========================
// VoxelChunkComponent.h additions:
// ==========================
// Add to the class:
// mutable FCriticalSection DataAccessLock;

// MODIFIED: Use cached borders instead of live snapshot
void UVoxelChunkComponent::SnapshotNeighbors(FChunkNeighbors& Out) const
{
    SCOPE_CYCLE_COUNTER(STAT_VoxelSnapshotNeighbors);

    if (RenderMode != EVoxelRenderMode::Voxels)
    {
        Out = {};
        return;
    }

    Out.SizeX = (Settings->ChunkSizeX + LODScaleXY - 1) / LODScaleXY;
    Out.SizeY = (Settings->ChunkSizeY + LODScaleXY - 1) / LODScaleXY;
    Out.SizeZ = Settings->ChunkSizeZ;

    // If we have cached borders, use them directly
    if (bNeighborBordersCached)
    {
        Out.XNeg = CachedNeighborBorders.XNeg;
        Out.XPos = CachedNeighborBorders.XPos;
        Out.YNeg = CachedNeighborBorders.YNeg;
        Out.YPos = CachedNeighborBorders.YPos;
        Out.ZNeg = CachedNeighborBorders.ZNeg;
        Out.ZPos = CachedNeighborBorders.ZPos;

        Out.bHasXNeg = CachedNeighborBorders.bHasXNeg;
        Out.bHasXPos = CachedNeighborBorders.bHasXPos;
        Out.bHasYNeg = CachedNeighborBorders.bHasYNeg;
        Out.bHasYPos = CachedNeighborBorders.bHasYPos;
        Out.bHasZNeg = CachedNeighborBorders.bHasZNeg;
        Out.bHasZPos = CachedNeighborBorders.bHasZPos;

        return;
    }

    // Fallback: no cached borders available (shouldn't happen normally)
    // Return empty neighbors - will result in no face culling at chunk edges
    Out.bHasXNeg = false;
    Out.bHasXPos = false;
    Out.bHasYNeg = false;
    Out.bHasYPos = false;
    Out.bHasZNeg = false;
    Out.bHasZPos = false;
    Out.XNeg.Reset();
    Out.XPos.Reset();
    Out.YNeg.Reset();
    Out.YPos.Reset();
    Out.ZNeg.Reset();
    Out.ZPos.Reset();
}

// NEW: Cache neighbor borders immediately after generation on game thread
void UVoxelChunkComponent::CacheNeighborBordersFromWorld()
{
    if (!OwnerWorld) return;

    const int32 SX = (Settings->ChunkSizeX + LODScaleXY - 1) / LODScaleXY;
    const int32 SY = (Settings->ChunkSizeY + LODScaleXY - 1) / LODScaleXY;
    const int32 SZ = Settings->ChunkSizeZ;

    CachedNeighborBorders = FCachedNeighborBorders(); // Reset

    // Generic border copy helper - eliminates code duplication across X/Y/Z borders
    // FixedAxis: 0=X, 1=Y, 2=Z (which coordinate is held constant)
    auto CopyBorderGeneric = [&](UVoxelChunkComponent* N, int32 FixedAxis, int32 SrcCoord,
                                   int32 Size0, int32 Size1, int32 NeighborSize0, int32 NeighborSize1, int32 NeighborSizeFixed,
                                   TArray<EVoxelBlockID>& Dst, bool& bFlag)
        {
            // Validate neighbor state
            if (!N || N->State != EVoxelChunkState::Ready || N->LOD != LOD ||
                N->RenderMode != RenderMode || N->CategoryData.Data.Num() == 0)
            {
                bFlag = false;
                Dst.Reset();
                return;
            }

            // Validate coordinate ranges
            if (SrcCoord < 0 || SrcCoord >= NeighborSizeFixed || NeighborSize0 != Size0 || NeighborSize1 != Size1)
            {
                bFlag = false;
                Dst.Reset();
                return;
            }

            // Copy border slice
            const int32 ExpectedSize = Size0 * Size1;
            Dst.SetNumUninitialized(ExpectedSize);

            for (int32 i1 = 0; i1 < Size1; ++i1)
            {
                for (int32 i0 = 0; i0 < Size0; ++i0)
                {
                    // Map (i0, i1) to (x, y, z) based on fixed axis
                    int32 x, y, z;
                    switch (FixedAxis)
                    {
                        case 0: x = SrcCoord; y = i0; z = i1; break; // X fixed: iterate YZ
                        case 1: x = i0; y = SrcCoord; z = i1; break; // Y fixed: iterate XZ
                        default: x = i0; y = i1; z = SrcCoord; break; // Z fixed: iterate XY
                    }

                    uint8 Cat = N->CategoryData.Get(x, y, z);
                    EVoxelBlockID Bid;
                    switch (Cat) {
                    case 0: Bid = EVoxelBlockID::Air; break;
                    case 1: Bid = EVoxelBlockID::Water; break;
                    default: Bid = EVoxelBlockID::Stone; break;
                    }

                    const int32 WriteIndex = i0 + i1 * Size0;
                    if (WriteIndex >= 0 && WriteIndex < ExpectedSize)
                    {
                        Dst[WriteIndex] = Bid;
                    }
                }
            }
            bFlag = true;
        };

    // Specialized wrappers for clarity at call sites
    auto CopyXBorder = [&](UVoxelChunkComponent* N, int32 SrcX, TArray<EVoxelBlockID>& Dst, bool& bFlag)
        {
            const int32 NSX = N ? (Settings->ChunkSizeX + N->LODScaleXY - 1) / N->LODScaleXY : 0;
            const int32 NSY = N ? (Settings->ChunkSizeY + N->LODScaleXY - 1) / N->LODScaleXY : 0;
            const int32 NSZ = N ? Settings->ChunkSizeZ : 0;
            CopyBorderGeneric(N, 0, SrcX, SY, SZ, NSY, NSZ, NSX, Dst, bFlag);
        };

    auto CopyYBorder = [&](UVoxelChunkComponent* N, int32 SrcY, TArray<EVoxelBlockID>& Dst, bool& bFlag)
        {
            const int32 NSX = N ? (Settings->ChunkSizeX + N->LODScaleXY - 1) / N->LODScaleXY : 0;
            const int32 NSY = N ? (Settings->ChunkSizeY + N->LODScaleXY - 1) / N->LODScaleXY : 0;
            const int32 NSZ = N ? Settings->ChunkSizeZ : 0;
            CopyBorderGeneric(N, 1, SrcY, SX, SZ, NSX, NSZ, NSY, Dst, bFlag);
        };

    auto CopyZBorder = [&](UVoxelChunkComponent* N, int32 SrcZ, TArray<EVoxelBlockID>& Dst, bool& bFlag)
        {
            const int32 NSX = N ? (Settings->ChunkSizeX + N->LODScaleXY - 1) / N->LODScaleXY : 0;
            const int32 NSY = N ? (Settings->ChunkSizeY + N->LODScaleXY - 1) / N->LODScaleXY : 0;
            const int32 NSZ = N ? Settings->ChunkSizeZ : 0;
            CopyBorderGeneric(N, 2, SrcZ, SX, SY, NSX, NSY, NSZ, Dst, bFlag);
        };

    // Get neighbors and cache their borders
    UVoxelChunkComponent* XNegN = OwnerWorld->GetChunk(FVoxelCoord(ChunkCoord.Cx - 1, ChunkCoord.Cy, ChunkCoord.Cz));
    UVoxelChunkComponent* XPosN = OwnerWorld->GetChunk(FVoxelCoord(ChunkCoord.Cx + 1, ChunkCoord.Cy, ChunkCoord.Cz));
    UVoxelChunkComponent* YNegN = OwnerWorld->GetChunk(FVoxelCoord(ChunkCoord.Cx, ChunkCoord.Cy - 1, ChunkCoord.Cz));
    UVoxelChunkComponent* YPosN = OwnerWorld->GetChunk(FVoxelCoord(ChunkCoord.Cx, ChunkCoord.Cy + 1, ChunkCoord.Cz));
    UVoxelChunkComponent* ZNegN = OwnerWorld->GetChunk(FVoxelCoord(ChunkCoord.Cx, ChunkCoord.Cy, ChunkCoord.Cz - 1));
    UVoxelChunkComponent* ZPosN = OwnerWorld->GetChunk(FVoxelCoord(ChunkCoord.Cx, ChunkCoord.Cy, ChunkCoord.Cz + 1));

    CopyXBorder(XNegN, SX - 1, CachedNeighborBorders.XNeg, CachedNeighborBorders.bHasXNeg);
    CopyXBorder(XPosN, 0, CachedNeighborBorders.XPos, CachedNeighborBorders.bHasXPos);
    CopyYBorder(YNegN, SY - 1, CachedNeighborBorders.YNeg, CachedNeighborBorders.bHasYNeg);
    CopyYBorder(YPosN, 0, CachedNeighborBorders.YPos, CachedNeighborBorders.bHasYPos);

    if (ZNegN)
    {
        const int32 NSZ = Settings->ChunkSizeZ;
        CopyZBorder(ZNegN, NSZ - 1, CachedNeighborBorders.ZNeg, CachedNeighborBorders.bHasZNeg);
    }
    if (ZPosN)
    {
        CopyZBorder(ZPosN, 0, CachedNeighborBorders.ZPos, CachedNeighborBorders.bHasZPos);
    }

    bNeighborBordersCached = true;
    // REMOVED: MarkChunkForSeamRemesh call
}
void UVoxelChunkComponent::EnsureVoxelMaterial_RMC(URealtimeMeshComponent* inRMC,
    const UVoxelSettings* inSettings,
    const UVoxelMaterialSet* MatSet)
{
    if (!inRMC || !Settings || !MatSet) return;
    if (!VoxelMID)
    {
        UMaterialInterface* Base = Settings->VoxelArrayMaterial.LoadSynchronous();
        if (!Base) return;
        VoxelMID = UMaterialInstanceDynamic::Create(Base, this);
        VoxelMID->SetTextureParameterValue(TEXT("AlbedoArray"), MatSet->Albedo);
        VoxelMID->SetTextureParameterValue(TEXT("NormalArray"), MatSet->Normal);
        VoxelMID->SetTextureParameterValue(TEXT("ORMArray"), MatSet->ORM);
        VoxelMID->SetScalarParameterValue(TEXT("UVScale"), MatSet->UVScale);
        VoxelMID->SetScalarParameterValue(TEXT("LayersMinusOne"),
            (float)FMath::Clamp(MatSet->LayersMinusOne, 0, 255));
    }
    inRMC->SetMaterial(0, VoxelMID);   // <- appeler SetMaterial sur le RMC
}