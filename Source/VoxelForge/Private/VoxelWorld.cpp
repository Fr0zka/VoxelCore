// VoxelWorld.cpp
// Implementation of the voxel world manager

#include "VoxelWorld.h"
#include "VoxelDiffLayer.h"
#include "RealtimeMeshComponent.h"
#include "RealtimeMeshSimple.h"
#include "VoxelMarchingCubesMesher.h"
#include "VoxelStrateDefinition.h"
#include "VoxelBiomeDefinition.h"
#include "VoxelTerrainOpDefinition.h"
#include "VoxelContentManager.h"
#include "VoxelDensityVolume.h"
#include "VoxelStats.h"
// IWYU (FPSemantics = Precise ⇒ plus de PCH partagé) : GetPlayerPosition déréférence le pawn, donc
// APawn doit être COMPLET — `Casts.h` n'en donne qu'une déclaration avant. APlayerController était
// complet par transitivité seulement : on l'inclut explicitement, c'est exactement la fragilité
// qu'on est en train de retirer.
// GetPlayerPosition dereferences the pawn, so APawn must be COMPLETE — Casts.h only forward-declares
// it. APlayerController was complete transitively only; include it explicitly.
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialParameterCollection.h"
#include "Kismet/KismetMaterialLibrary.h"
#include "Engine/VolumeTexture.h"
#include "VoxelAtmosphereManager.h"
#include "DrawDebugHelpers.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Modules/ModuleManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"   // Unreal Insights scopes (Perf 0)

#if WITH_EDITOR
#include "VoxelStrateComposer.h"
#include "VoxelStrateMeasure.h"
#endif

AVoxelWorld::AVoxelWorld()
{
    PrimaryActorTick.bCanEverTick = true;
}

//=============================================================================
// T1.f — build the RMC geometry buffers OFF the game thread.
//=============================================================================
// FRealtimeMeshStreamSet is plain CPU data; the per-vertex/per-triangle builder loop used to run
// in ApplyMeshToTile ON THE GAME THREAD, where it was the dominant streaming cost (game thread
// >6 ms while moving, GPU/draw idle). It touches ONLY the POD MeshData arrays — no UObject, no
// generator — so it's safe on the gen worker. The game thread then just uploads the finished
// streams (CreateSectionGroup). Byte-identical geometry; the only thing that moved is WHERE it runs.
static void BuildTileStreamSet(RealtimeMesh::FRealtimeMeshStreamSet& Streams, const FVoxelMeshData& MeshData)
{
    RealtimeMesh::TRealtimeMeshBuilderLocal<uint32, FPackedNormal, FVector2DHalf, 1> Builder(Streams);
    Builder.EnableTangents();
    Builder.EnableTexCoords();
    Builder.EnableColors();        // masques matériau F6 (palette biome / pente / fondu) — voir le mesher
    Builder.EnablePolyGroups();

    const int32 NumVertices = MeshData.Vertices.Num();
    Builder.ReserveAdditionalVertices(NumVertices);
    for (int32 i = 0; i < NumVertices; i++)
    {
        auto Vertex = Builder.AddVertex((FVector3f)MeshData.Vertices[i]);
        if (MeshData.Normals.IsValidIndex(i))
        {
            Vertex.SetNormalAndTangent((FVector3f)MeshData.Normals[i], FVector3f(1, 0, 0));
        }
        if (MeshData.UVs.IsValidIndex(i))
        {
            Vertex.SetTexCoord(0, (FVector2f)MeshData.UVs[i]);
        }
        if (MeshData.Colors.IsValidIndex(i))
        {
            Vertex.SetColor(MeshData.Colors[i]);
        }
    }

    // F17 — the mesher packs the index buffer as [ground run | sky-cap run] (see
    // FVoxelMeshData::NumCeilingTriangles): polygroup 0 = ground, 1 = sky-cap ceiling.
    // RMC derives one section per contiguous group run (material slot = group index).
    const int32 NumIndices    = MeshData.Triangles.Num();
    const int32 FirstCapIndex = NumIndices - MeshData.NumCeilingTriangles * 3;
    Builder.ReserveAdditionalTriangles(NumIndices / 3);
    for (int32 i = 0; i < NumIndices; i += 3)
    {
        Builder.AddTriangle((uint32)MeshData.Triangles[i],
                            (uint32)MeshData.Triangles[i + 1],
                            (uint32)MeshData.Triangles[i + 2],
                            (i >= FirstCapIndex) ? 1 : 0 /*poly group*/);
    }
}

class FScopedGenerationPause
{
public:
    explicit FScopedGenerationPause(AVoxelWorld* InWorld)
        : World(InWorld)
    {
        if (!World) return;

        World->bGenerationPaused.store(true, std::memory_order_release);

        // The game thread owns this gate; workers only read Generator/Mesher and enqueue results.
        // La barrière est prise sur le thread de jeu ; les workers ne font qu'énumérer et Enqueue.
        const double Deadline = FPlatformTime::Seconds() + 5.0;
        while (World->ActiveTaskCount.load(std::memory_order_relaxed) > 0)
        {
            if (FPlatformTime::Seconds() > Deadline) return;
            FPlatformProcess::Yield();
        }

        if (World->ContentManager && !World->ContentManager->WaitForDecorationTasks(Deadline)) return;

        bAcquired = true;
    }

    ~FScopedGenerationPause()
    {
        if (World)
        {
            World->bGenerationPaused.store(false, std::memory_order_release);
        }
    }

    bool Acquired() const { return bAcquired; }

private:
    AVoxelWorld* World = nullptr;
    bool bAcquired = false;
};

#if WITH_EDITOR
namespace
{
    void VF_SetComposerRuntimeBounds(FVoxelStrateArchetypeParams& Params,
                                     float TopWorldZ, float BottomWorldZ)
    {
        Params.TunnelNetworkParams.StrateTopWorldZ = TopWorldZ;
        Params.TunnelNetworkParams.StrateBottomWorldZ = BottomWorldZ;
        Params.SlabParams.StrateTopWorldZ = TopWorldZ;
        Params.SlabParams.StrateBottomWorldZ = BottomWorldZ;
        Params.MazeParams.StrateTopWorldZ = TopWorldZ;
        Params.MazeParams.StrateBottomWorldZ = BottomWorldZ;
        Params.SurfaceParams.StrateTopWorldZ = TopWorldZ;
        Params.SurfaceParams.StrateBottomWorldZ = BottomWorldZ;
        Params.VerticalShaftParams.StrateTopWorldZ = TopWorldZ;
        Params.VerticalShaftParams.StrateBottomWorldZ = BottomWorldZ;
        Params.FloatingIslandParams.StrateTopWorldZ = TopWorldZ;
        Params.FloatingIslandParams.StrateBottomWorldZ = BottomWorldZ;
    }

    float VF_ComposerBoundarySeal(const FVoxelStrateArchetypeParams& Params,
                                  ECaveGeneratorType Archetype)
    {
        switch (Archetype)
        {
        case ECaveGeneratorType::FlatPlain:
        case ECaveGeneratorType::CrystalChamber:
            return Params.SlabParams.BoundarySealThickness;
        case ECaveGeneratorType::Maze:
            return Params.MazeParams.BoundarySealThickness;
        case ECaveGeneratorType::SurfaceWorld:
            return Params.SurfaceParams.BoundarySealThickness;
        case ECaveGeneratorType::VerticalShafts:
            return Params.VerticalShaftParams.BoundarySealThickness;
        case ECaveGeneratorType::FloatingIslands:
            return Params.FloatingIslandParams.BoundarySealThickness;
        case ECaveGeneratorType::Underwater:
        case ECaveGeneratorType::TunnelNetwork:
        default:
            return Params.TunnelNetworkParams.BoundarySealThickness;
        }
    }

    class FComposerStackDensitySampler final : public IVoxelStrateDensitySampler
    {
    public:
        explicit FComposerStackDensitySampler(const FVoxelOpStack& InStack)
            : Stack(InStack) {}

        float SampleDensity(float WorldX, float WorldY, float WorldZ) const override
        {
            return Stack.EvalMC(WorldX, WorldY, WorldZ);
        }

    private:
        const FVoxelOpStack& Stack;
    };

    bool VF_BuildComposerReferenceStack(
        const FVoxelStrateComposerCandidate& Candidate,
        const UVoxelGenerator& Generator,
        const UVoxelStrateManager& StrateManager,
        int32 TargetStrateIndex,
        int32 TargetTopChunkZ,
        int32 TargetBottomChunkZ,
        FVoxelOpStack& OutStack,
        FVoxelOpContext& OutContext,
        FString& OutError)
    {
        const float TopWorldZ = (float)(TargetTopChunkZ + 1) * CHUNK_SIZE;
        const float BottomWorldZ = (float)TargetBottomChunkZ * CHUNK_SIZE;
        FVoxelStrateArchetypeParams Params = Candidate.ArchetypeParams;
        VF_SetComposerRuntimeBounds(Params, TopWorldZ, BottomWorldZ);

        bool bBuilt = false;
        if (Candidate.Regions.RegionCount > 1)
        {
            FVoxelStrateRegionManifest RegionManifest = Candidate.Regions;
            VF_RekeyStrateRegionManifest(RegionManifest, Candidate.Seed,
                                          TargetStrateIndex);
            RegionManifest.StrateTopWorldZ = TopWorldZ;
            RegionManifest.StrateBottomWorldZ = BottomWorldZ;
            RegionManifest.bHasGlobalStructuralParams = true;
            for (FVoxelStrateRegion& Region : RegionManifest.Regions)
            {
                VF_SetStrateArchetypeRuntimeBounds(Region.ArchetypeParams,
                                                   TopWorldZ, BottomWorldZ);
            }
            bBuilt = VF_BuildStrateRegionStack(
                RegionManifest, Generator.OriginSpineRadius, &StrateManager,
                OutStack, OutContext, &OutError);
        }
        else if (Candidate.bStructureRoll)
        {
            bBuilt = VF_BuildStackFromRecipe(
                Candidate.Recipe, Params, Generator.Seed, Generator.OriginSpineRadius,
                &StrateManager, OutStack, OutContext, &OutError);
        }
        else
        {
            bBuilt = VF_BuildNativeStrateStackForCandidate(
                Candidate.Archetype, Params, Generator.Seed, Generator.OriginSpineRadius,
                Generator.WorldRadiusVoxels, Generator.EdgeSealThickness, &StrateManager,
                OutStack, OutContext);
            if (!bBuilt && OutError.IsEmpty())
            {
                OutError = TEXT("The production native candidate stack could not be materialised.");
            }
        }
        if (!bBuilt)
        {
            return false;
        }

        // The recipe builder deliberately defaults the world-edge context to 0 for offline
        // measurements. A live candidate must use the actual world's edge law, while its
        // vertical bounds and structural seal remain candidate-owned.
        const int32 MidChunkZ = TargetBottomChunkZ
            + (TargetTopChunkZ - TargetBottomChunkZ) / 2;
        OutContext.ChunkCoord = FIntVector(0, 0, MidChunkZ);
        OutContext.Step = 1;
        OutContext.LayoutVersion = StrateManager.GetLayoutVersion();
        OutContext.WorldRadiusVoxels = Generator.WorldRadiusVoxels;
        OutContext.EdgeSealThickness = Generator.EdgeSealThickness;
        OutStack.PrepareChunk(OutContext);
        return true;
    }

    bool VF_ComposerReferenceIsComparable(
        const AVoxelWorld& World,
        const FStrateSlot& TargetSlot,
        ECaveGeneratorType Archetype,
        const FIntVector& RepresentativeChunk,
        FString& OutReason)
    {
        OutReason.Reset();
        if (World.DiffLayer && World.DiffLayer->HasAnyMods())
        {
            OutReason = TEXT("diff-layer-modifications-present");
            return false;
        }

        const FStrateDisturbanceParams Disturbances =
            World.StrateManager->GetDisturbanceParamsForChunk(RepresentativeChunk);
        if (Disturbances.ChasmDensity > 0.0f
            || Disturbances.BridgeDensity > 0.0f
            || Disturbances.RidgeDensity > 0.0f)
        {
            OutReason = TEXT("live-disturbance-post-present");
            return false;
        }

        // The production SurfaceWorld stack may include a generator-owned biome field and
        // per-biome params. The standalone candidate stack intentionally does not invent that
        // context; fall back to the live metric and mark the density check as unavailable.
        if (Archetype == ECaveGeneratorType::SurfaceWorld
            && TargetSlot.Definition != nullptr
            && TargetSlot.Definition->Biomes.Num() > 0)
        {
            OutReason = TEXT("surface-biome-context-present");
            return false;
        }
        return true;
    }

    FVoxelStrateMeasureSettings VF_ComposerMeasureSettings()
    {
        FVoxelStrateMeasureSettings Settings;
        Settings.SampleStep = 4;
        Settings.RadiusInVoxels = 256;
        Settings.CenterXY = FVector2D::ZeroVector;
        Settings.MaxCells = 8000000;
        Settings.MaxRouteRetries = 16;
        Settings.HeadroomCells = 2;
        Settings.InteriorMarginVoxels = -1;
        return Settings;
    }

    bool VF_ComposerDensitySanityCheck(
        const UVoxelGenerator& Generator,
        const FVoxelOpStack& ReferenceStack,
        int32 TargetTopChunkZ,
        int32 TargetBottomChunkZ,
        float& OutMaxDelta,
        int32& OutMismatches,
        int32& OutSamples)
    {
        // Keep all probes inside one representative chunk column where possible. This exercises
        // both the live thread-local refetch and the prepared reference stack without making the
        // check depend on a particular room being present at a hand-picked coordinate.
        static const FIntVector Offsets[] =
        {
            FIntVector(1, 1, -15),
            FIntVector(7, 13, -7),
            FIntVector(17, 3, 1),
            FIntVector(29, 27, 9),
            FIntVector(3, 23, 17),
            FIntVector(19, 19, 25),
            FIntVector(11, 29, 31),
            FIntVector(27, 5, -23),
        };

        const int32 MidChunkZ = TargetBottomChunkZ
            + (TargetTopChunkZ - TargetBottomChunkZ) / 2;
        const int32 BottomWorldZ = TargetBottomChunkZ * CHUNK_SIZE;
        const int32 TopWorldZExclusive = (TargetTopChunkZ + 1) * CHUNK_SIZE;
        const int32 MidWorldZ = MidChunkZ * CHUNK_SIZE + CHUNK_SIZE / 2;

        OutMaxDelta = 0.0f;
        OutMismatches = 0;
        OutSamples = 0;
        for (const FIntVector& Offset : Offsets)
        {
            const float X = (float)Offset.X;
            const float Y = (float)Offset.Y;
            const float Z = (float)FMath::Clamp(
                MidWorldZ + Offset.Z, BottomWorldZ + 1, TopWorldZExclusive - 1);
            const float Live = Generator.GetDensityAt(X, Y, Z);
            const float Reference = ReferenceStack.EvalMC(X, Y, Z);

            uint32 LiveBits = 0;
            uint32 ReferenceBits = 0;
            FMemory::Memcpy(&LiveBits, &Live, sizeof(LiveBits));
            FMemory::Memcpy(&ReferenceBits, &Reference, sizeof(ReferenceBits));
            const bool bBitEqual = LiveBits == ReferenceBits;
            if (!bBitEqual)
            {
                ++OutMismatches;
            }

            const float Delta = (FMath::IsFinite(Live) && FMath::IsFinite(Reference))
                ? FMath::Abs(Live - Reference) : FLT_MAX;
            OutMaxDelta = FMath::Max(OutMaxDelta, Delta);
            ++OutSamples;
        }
        return OutMismatches == 0;
    }
}
#endif

//=============================================================================
// LIVE EDIT — regenerate all chunks when params change in the Details panel
//=============================================================================

void AVoxelWorld::RegenerateAllChunks()
{
    // Bump the generation epoch so in-flight async tasks become stale.
    // ProcessPendingChunks will discard any result with an old epoch.
    GenerationEpoch++;

    // Tear down every tile component, then clear all tile state. Components are GC-safe via
    // actor ownership. T2.c: PARK them instead of destroying — the reload right after this
    // is exactly the burst the pool exists for (overflow past the cap is destroyed).
    const int32 Count = LoadedTiles.Num();
    for (auto& Pair : TileComponents) { if (Pair.Value) ReleaseTileComponent(Pair.Value); }
    TileComponents.Empty();
    LoadedTiles.Empty();

    // Decorations/water are keyed per level-0 chunk — clear them all.
    if (ContentManager) { ContentManager->ClearAll(); }

    // Density volume: bump epoch (drop in-flight fills) + drop data → full refill next Tick.
    if (DensityVolume) { DensityVolume->Reset(); }

    // Clear pending set — stale tasks will be discarded by the epoch check.
    PendingTiles.Empty();
    // Tiles are already destroyed above — drop any deferred-teardown keys so the drain doesn't
    // try to UnloadTile coords that no longer exist.
    PendingUnload.Empty();
    // Re-mesh queues reference now-unloaded tiles — drop them (they'd be skipped anyway).
    DirtyRemeshQueue.Empty();
    BandRemeshQueue.Empty();
    // §9.4 collision-only tracking references destroyed tiles — clear (rebuilt on the next crossing).
    CollisionOnlyTiles.Empty();
    PrevCollisionOnlyTiles.Empty();

    // Reset streaming state so the next Tick rebuilds the desired set and reloads.
    LastUpdateCenter = FIntVector(INT32_MAX, INT32_MAX, INT32_MAX);
    bAllChunksLoaded = false;
    DesiredSorted.Reset();
    DesiredStamped.Reset();
    TransitionHold.Reset();
    TransitionHoldQueue.Reset();
    TransitionHoldCursor = 0;

    // Tick will reload all tiles on the next frame with fresh params.
    UE_LOG(LogTemp, Log, TEXT("[VoxelWorld] RegenerateAllChunks (epoch %u): cleared %d tiles"), GenerationEpoch, Count);
}

void AVoxelWorld::RebuildStrates()
{
    {
        FScopedGenerationPause Guard(this);
        if (!Guard.Acquired())
        {
            UE_LOG(LogTemp, Error, TEXT("[VoxelWorld] RebuildStrates: generation pause timed out; no mutation applied."));
            return;
        }

        if (StrateManager && Settings)
        {
            // Re-applies layout + inter-strate gap + passage/spine settings from VoxelSettings.
            StrateManager->Initialize(Settings, Settings->Seed);
        }
        if (AtmosphereManager) AtmosphereManager->Reset();
        if (ContentManager)    ContentManager->ClearAll();
    }

    // Reload all chunks against the rebuilt strate data.
    RegenerateAllChunks();

    UE_LOG(LogTemp, Log, TEXT("[VoxelWorld] RebuildStrates: strate layout + passages rebuilt from settings."));
}

void AVoxelWorld::ValidateDeterminism()
{
    // F2 — window-invariance regression test (§8.4). The density function must return the SAME
    // value for a coordinate no matter which chunk's thread_local caches (SDF rooms, strate
    // memo, biome grid, surface columns, lattice bakes) happen to be warm. Historically THE
    // source of chunk seams — and the invariant every "bit-identical" hot-path refactor claims
    // to preserve. This runs on the game thread, whose caches are isolated from the workers.
    if (!Generator)
    {
        UE_LOG(LogTemp, Warning, TEXT("[VoxelForge] ValidateDeterminism: no Generator — run during PIE."));
        return;
    }

    const FIntVector CenterChunk = WorldToChunkCoord(GetPlayerPosition());

    float MaxRepeatDelta = 0.0f;   // same alignment sampled twice — must be 0 (statelessness)
    float MaxWindowDelta = 0.0f;   // left-warmed vs right-warmed — must be 0 (window invariance)
    FVector WorstP = FVector::ZeroVector;
    int32 Mismatches = 0, Points = 0;

    // Points hugging the X boundary between chunk (CX,CY) and (CX+1,CY): they sit inside BOTH
    // chunks' cache search boxes (box = chunk extent + margin), so either alignment may legally
    // serve them — exactly the cross-window case that seams when an invariant breaks.
    const float BoundaryX = (float)((CenterChunk.X + 1) * CHUNK_SIZE);
    for (int32 iy = 0; iy < 16; ++iy)
    {
        for (int32 iz = 0; iz < 8; ++iz)
        {
            const float Y = (float)(CenterChunk.Y * CHUNK_SIZE) + (float)iy * 2.0f + 0.5f;
            const float Z = (float)(CenterChunk.Z * CHUNK_SIZE) + (float)iz * 4.0f + 0.5f;
            for (const float Side : { -0.5f, 0.5f })   // just left / just right of the boundary
            {
                const float X = BoundaryX + Side;
                ++Points;

                // Warm every cache from the LEFT chunk's middle, sample the point twice.
                Generator->GetDensityAt(BoundaryX - (float)CHUNK_SIZE * 0.5f, Y, Z);
                const float DLeft  = Generator->GetDensityAt(X, Y, Z);
                const float DLeft2 = Generator->GetDensityAt(X, Y, Z);

                // Re-warm from the RIGHT chunk (rebuilds the boxes centred there), resample.
                Generator->GetDensityAt(BoundaryX + (float)CHUNK_SIZE * 0.5f, Y, Z);
                const float DRight = Generator->GetDensityAt(X, Y, Z);

                MaxRepeatDelta = FMath::Max(MaxRepeatDelta, FMath::Abs(DLeft - DLeft2));
                const float WDelta = FMath::Abs(DLeft - DRight);
                if (WDelta > MaxWindowDelta)
                {
                    MaxWindowDelta = WDelta;
                    WorstP = FVector(X, Y, Z);
                }
                if (WDelta > 0.0f) { ++Mismatches; }
            }
        }
    }

    if (MaxWindowDelta == 0.0f && MaxRepeatDelta == 0.0f)
    {
        UE_LOG(LogTemp, Log, TEXT("[VoxelForge] ValidateDeterminism: OK — %d boundary points at chunk (%d,%d,%d), window delta 0, repeat delta 0."),
            Points, CenterChunk.X, CenterChunk.Y, CenterChunk.Z);
    }
    else
    {
        UE_LOG(LogTemp, Error, TEXT("[VoxelForge] ValidateDeterminism: FAIL — %d/%d points mismatch, max window delta %.6f (repeat %.6f) at voxel (%.1f, %.1f, %.1f). Window-invariance regression — see ARCHITECTURE §8.4."),
            Mismatches, Points, MaxWindowDelta, MaxRepeatDelta, WorstP.X, WorstP.Y, WorstP.Z);
    }
}

#if WITH_EDITOR
void AVoxelWorld::ApplyComposerCandidate()
{
    UWorld* World = GetWorld();
    if (World == nullptr || !World->IsPlayInEditor())
    {
        UE_LOG(LogTemp, Warning,
            TEXT("[VoxelWorld] ApplyComposerCandidate: run this button from a PIE world."));
        return;
    }
    if (bShuttingDown.load(std::memory_order_acquire)
        || bGenerationPaused.load(std::memory_order_acquire))
    {
        UE_LOG(LogTemp, Warning,
            TEXT("[VoxelWorld] ApplyComposerCandidate: generation is already stopping or paused."));
        return;
    }
    if (!Settings || !Generator || !StrateManager)
    {
        UE_LOG(LogTemp, Warning,
            TEXT("[VoxelWorld] ApplyComposerCandidate: BeginPlay has not initialized Settings, Generator, and StrateManager."));
        return;
    }

    const TArray<FStrateSlot>& Layout = StrateManager->GetLayout();
    const FStrateSlot* TargetSlot = nullptr;
    for (const FStrateSlot& Slot : Layout)
    {
        if (Slot.StrateIndex == ComposerTargetStrateIndex)
        {
            TargetSlot = &Slot;
            break;
        }
    }
    if (TargetSlot == nullptr || TargetSlot->Definition == nullptr)
    {
        UE_LOG(LogTemp, Warning,
            TEXT("[VoxelWorld] ApplyComposerCandidate: ComposerTargetStrateIndex=%d is not a live layout slot (layout has %d slots)."),
            ComposerTargetStrateIndex, Layout.Num());
        return;
    }

    FVoxelStrateCorpus Corpus;
    FString CorpusReport;
    if (!Corpus.LoadFromAssetRegistry(CorpusReport))
    {
        UE_LOG(LogTemp, Error,
            TEXT("[VoxelWorld] ApplyComposerCandidate: composer corpus is unavailable: %s"),
            *CorpusReport);
        return;
    }

    const FVoxelStrateComposerCandidate Candidate = VF_RollStrateCandidate(
        Corpus, ComposerSeed, ComposerCandidateIndex, bComposerRollStructure);
    if (!Candidate.bValid)
    {
        UE_LOG(LogTemp, Error,
            TEXT("[VoxelWorld] ApplyComposerCandidate: seed=%d index=%d roll failed: %s"),
            ComposerSeed, ComposerCandidateIndex, *Candidate.FailureReason);
        return;
    }

    const int32 TargetTopChunkZ = TargetSlot->TopChunkZ;
    const int32 TargetBottomChunkZ = TargetSlot->BottomChunkZ;
    const float TargetTopWorldZ = (float)(TargetTopChunkZ + 1) * CHUNK_SIZE;
    const float TargetBottomWorldZ = (float)TargetBottomChunkZ * CHUNK_SIZE;
    const FIntVector RepresentativeChunk(
        0, 0, TargetBottomChunkZ + (TargetTopChunkZ - TargetBottomChunkZ) / 2);

    // Materialise before mutating the live manager. A bad recipe or unsupported native family
    // therefore leaves the running world untouched.
    FVoxelStrateArchetypeParams PreflightParams = Candidate.ArchetypeParams;
    VF_SetComposerRuntimeBounds(PreflightParams, TargetTopWorldZ, TargetBottomWorldZ);
    FVoxelOpStack PreflightStack;
    FVoxelOpContext PreflightContext;
    FString PreflightError;
    bool bPreflightBuilt = false;
    FVoxelStrateRegionManifest PreflightRegions;
    if (Candidate.Regions.RegionCount > 1)
    {
        PreflightRegions = Candidate.Regions;
        VF_RekeyStrateRegionManifest(PreflightRegions, Candidate.Seed,
                                      ComposerTargetStrateIndex);
        PreflightRegions.StrateTopWorldZ = TargetTopWorldZ;
        PreflightRegions.StrateBottomWorldZ = TargetBottomWorldZ;
        PreflightRegions.bHasGlobalStructuralParams = true;
        for (FVoxelStrateRegion& Region : PreflightRegions.Regions)
        {
            VF_SetStrateArchetypeRuntimeBounds(Region.ArchetypeParams,
                                               TargetTopWorldZ, TargetBottomWorldZ);
        }
        bPreflightBuilt = VF_BuildStrateRegionStack(
            PreflightRegions, Generator->OriginSpineRadius, StrateManager,
            PreflightStack, PreflightContext, &PreflightError);
    }
    else if (Candidate.bStructureRoll)
    {
        bPreflightBuilt = VF_BuildStackFromRecipe(
            Candidate.Recipe, PreflightParams, Generator->Seed, Generator->OriginSpineRadius,
            StrateManager, PreflightStack, PreflightContext, &PreflightError);
    }
    else
    {
        bPreflightBuilt = VF_BuildNativeStrateStackForCandidate(
            Candidate.Archetype, PreflightParams, Generator->Seed, Generator->OriginSpineRadius,
            Generator->WorldRadiusVoxels, Generator->EdgeSealThickness, StrateManager,
            PreflightStack, PreflightContext);
        if (!bPreflightBuilt)
        {
            PreflightError = TEXT("The production native candidate stack could not be materialised.");
        }
    }
    if (!bPreflightBuilt)
    {
        UE_LOG(LogTemp, Error,
            TEXT("[VoxelWorld] ApplyComposerCandidate: seed=%d index=%d was not applied: %s"),
            ComposerSeed, ComposerCandidateIndex, *PreflightError);
        return;
    }

    FString ApplyError;
    {
        // The manager override is a density-only replacement. It does not call Initialize or
        // GeneratePassages while the layout is half-built: the existing slot Z span, content
        // definition, and passage geometry remain stable. The pause drains current workers before
        // the map is changed, and the subsequent RegenerateAllChunks supplies the epoch bump.
        FScopedGenerationPause Guard(this);
        if (!Guard.Acquired())
        {
            UE_LOG(LogTemp, Error,
                TEXT("[VoxelWorld] ApplyComposerCandidate: generation pause timed out; no mutation applied."));
            return;
        }

        if (!StrateManager->SetComposerOverrideForStrate(
            ComposerTargetStrateIndex, Candidate.Seed, Candidate.Archetype,
            Candidate.ArchetypeParams, Candidate.bStructureRoll,
            Candidate.bStructureRoll ? &Candidate.Recipe : nullptr, ApplyError,
            Candidate.Regions.RegionCount > 1 ? &Candidate.Regions : nullptr))
        {
            UE_LOG(LogTemp, Error,
                TEXT("[VoxelWorld] ApplyComposerCandidate: seed=%d index=%d was not applied: %s"),
                ComposerSeed, ComposerCandidateIndex, *ApplyError);
            return;
        }

        // These are the same game-thread-owned side systems reset by RebuildStrates. The full
        // chunk reset below also resets the density volume and preserves DiffLayer edits.
        if (AtmosphereManager) { AtmosphereManager->Reset(); }
        if (ContentManager)    { ContentManager->ClearAll(); }
    }

    // Reuse the established seed/live-edit invalidation path: GenerationEpoch increments here,
    // stale ProcessQueue results are dropped, and the normal Tick submits a fresh stream around
    // the player. No async worker can observe the half-applied manager state.
    RegenerateAllChunks();

    FVoxelOpStack ReferenceStack;
    FVoxelOpContext ReferenceContext;
    FString ReferenceError;
    const bool bReferenceBuilt = VF_BuildComposerReferenceStack(
        Candidate, *Generator, *StrateManager, ComposerTargetStrateIndex,
        TargetTopChunkZ, TargetBottomChunkZ,
        ReferenceStack, ReferenceContext, ReferenceError);

    FString ComparableReason;
    const bool bReferenceComparable = bReferenceBuilt
        && VF_ComposerReferenceIsComparable(
            *this, *TargetSlot, Candidate.Archetype, RepresentativeChunk, ComparableReason);

    const FVoxelStrateMeasureSettings MeasureSettings = VF_ComposerMeasureSettings();
    FVoxelStrateMetrics Metrics;
    FString MetricSource;
    if (bReferenceComparable)
    {
        FComposerStackDensitySampler Sampler(ReferenceStack);
        Metrics = VF_MeasureStrateWithSampler(
            Sampler, (int32)TargetBottomWorldZ, (int32)TargetTopWorldZ,
            VF_ComposerBoundarySeal(PreflightParams, Candidate.Archetype), MeasureSettings);
        MetricSource = TEXT("candidate-stack");
    }
    else
    {
        // A live diff, disturbance post, or surface biome context makes a standalone stack an
        // unlike oracle. Keep the owner's metric useful by measuring the actual applied generator;
        // the log records why the bit-level oracle was skipped.
        Metrics = VF_MeasureStrate(
            *Generator, *StrateManager, ComposerTargetStrateIndex, MeasureSettings);
        MetricSource = TEXT("live-generator");
    }

    float MaxDensityDelta = 0.0f;
    int32 DensityMismatches = 0;
    int32 DensitySamples = 0;
    bool bDensityMatch = false;
    if (bReferenceComparable)
    {
        bDensityMatch = VF_ComposerDensitySanityCheck(
            *Generator, ReferenceStack, TargetTopChunkZ, TargetBottomChunkZ,
            MaxDensityDelta, DensityMismatches, DensitySamples);
    }

    const FString RecipeText = Candidate.bStructureRoll
        ? VF_FormatStrateStructureRecipe(Candidate.Recipe)
        : TEXT("parameter-roll");
    const FString DeterminismText = !bReferenceBuilt
        ? FString::Printf(TEXT("UNAVAILABLE(%s)"), *ReferenceError)
        : !bReferenceComparable
            ? FString::Printf(TEXT("SKIPPED(%s)"), *ComparableReason)
            : FString::Printf(TEXT("%s/%d"), bDensityMatch ? TEXT("PASS") : TEXT("FAIL"),
                              DensitySamples);
    const float AirFraction = Metrics.bValid ? Metrics.AirFraction : -1.0f;
    const float LargestShare = Metrics.bValid ? Metrics.LargestComponentShare : -1.0f;
    const float WalkableFraction = Metrics.bValid ? Metrics.WalkableFraction : -1.0f;
    const float FeatureScale = Metrics.bValid ? Metrics.MedianFeatureScale : -1.0f;

    UE_LOG(LogTemp, Log,
        TEXT("[VoxelWorld] ComposerCandidate applied seed=%d index=%d target=%d recipe=%s archetype=%s metrics=%s air=%.6f largest=%.6f walkable=%.6f feature=%.3f components=%d determinism=%s max_delta=%.9g mismatches=%d epoch=%u"),
        Candidate.Seed, Candidate.Index, ComposerTargetStrateIndex, *RecipeText,
        VF_GetStrateArchetypeName(Candidate.Archetype), *MetricSource,
        AirFraction, LargestShare, WalkableFraction, FeatureScale,
        Metrics.bValid ? Metrics.NumAirComponents : -1,
        *DeterminismText, MaxDensityDelta, DensityMismatches, GenerationEpoch);
}

void AVoxelWorld::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
    Super::PostEditChangeProperty(PropertyChangedEvent);

    // During PIE with live edit on, regenerate when the actor's own properties change
    // (e.g., Settings reference, bLiveEditStrates toggle, etc.)
    // Data asset edits (strate definitions) are handled separately by OnObjectModifiedInEditor.
    if (bLiveEditStrates && GetWorld() && GetWorld()->IsPlayInEditor())
    {
        RegenerateAllChunks();
    }
}

void AVoxelWorld::OnObjectModifiedInEditor(UObject* ModifiedObject)
{
    // Only react during PIE with live edit enabled
    if (!bLiveEditStrates || !GetWorld() || !GetWorld()->IsPlayInEditor()) return;

    // Only care about strate definition and terrain op definition edits.
    // (This delegate fires for EVERY UObject modification in the editor.)
    bool bIsRelevant = false;
    FString AssetName;

    // Case 1: A strate definition was modified
    if (UVoxelStrateDefinition* ModifiedStrate = Cast<UVoxelStrateDefinition>(ModifiedObject))
    {
        if (!Settings) return;
        AssetName = ModifiedStrate->GetName();

        // Check the strate pool
        for (const TSoftObjectPtr<UVoxelStrateDefinition>& PoolEntry : Settings->StratePool)
        {
            if (PoolEntry.Get() == ModifiedStrate) { bIsRelevant = true; break; }
        }
        // Check fixed strates
        if (!bIsRelevant)
        {
            for (const auto& FixedEntry : Settings->FixedStrates)
            {
                if (FixedEntry.Value.Get() == ModifiedStrate) { bIsRelevant = true; break; }
            }
        }
    }
    // Case 2: A terrain op definition was modified — check if any strate references it
    else if (UVoxelTerrainOpDefinition* ModifiedOp = Cast<UVoxelTerrainOpDefinition>(ModifiedObject))
    {
        if (!Settings || !StrateManager) return;
        AssetName = ModifiedOp->GetName();

        // Check every strate definition's terrain op list
        for (const TSoftObjectPtr<UVoxelStrateDefinition>& PoolEntry : Settings->StratePool)
        {
            UVoxelStrateDefinition* Def = PoolEntry.Get();
            if (!Def) continue;
            for (const FStrateTerrainOpEntry& Entry : Def->TerrainOperations)
            {
                if (Entry.Operation.Get() == ModifiedOp) { bIsRelevant = true; break; }
            }
            if (bIsRelevant) break;
        }
        if (!bIsRelevant)
        {
            for (const auto& FixedEntry : Settings->FixedStrates)
            {
                UVoxelStrateDefinition* Def = FixedEntry.Value.Get();
                if (!Def) continue;
                for (const FStrateTerrainOpEntry& Entry : Def->TerrainOperations)
                {
                    if (Entry.Operation.Get() == ModifiedOp) { bIsRelevant = true; break; }
                }
                if (bIsRelevant) break;
            }
        }
    }

    if (!bIsRelevant) return;

    UE_LOG(LogTemp, Log, TEXT("[VoxelWorld] Live edit: '%s' modified, regenerating..."),
        *AssetName);

    // Re-initialize the strate manager so it picks up the changed definition values,
    // then regenerate all chunks with the updated params.
    {
        FScopedGenerationPause Guard(this);
        if (!Guard.Acquired())
        {
            UE_LOG(LogTemp, Error, TEXT("[VoxelWorld] OnObjectModifiedInEditor: generation pause timed out; no mutation applied."));
            return;
        }

        if (StrateManager)
        {
            StrateManager->Initialize(Settings, Settings->Seed);
        }
        if (Generator)
        {
            Generator->InitializeSettings(Settings);
        }
    }

    RegenerateAllChunks();
}
#endif

void AVoxelWorld::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    // Signal all async tasks to bail out ASAP
    bShuttingDown.store(true, std::memory_order_release);

    // Wait for all running tasks to finish before destroying UObjects.
    // Tasks check bShuttingDown and exit early, so this should be fast.
    // Timeout after 3 seconds to avoid hanging the editor.
    const double Deadline = FPlatformTime::Seconds() + 3.0;
    while (ActiveTaskCount.load(std::memory_order_relaxed) > 0)
    {
        if (FPlatformTime::Seconds() > Deadline)
        {
            UE_LOG(LogTemp, Warning, TEXT("[VoxelWorld] EndPlay: %d tasks still running after 3s timeout"),
                ActiveTaskCount.load(std::memory_order_relaxed));
            break;
        }
        FPlatformProcess::Yield();  // Give CPU to other threads
    }

    // Drain any queued results
    FChunkResult Discard;
    while (ProcessQueue.Dequeue(Discard)) {}
    PendingTiles.Empty();
    PendingUnload.Empty();
    DirtyRemeshQueue.Empty();
    BandRemeshQueue.Empty();

    // Stop + drain the decoration march tasks (they read the Generator) before UObject teardown.
    if (ContentManager)
    {
        ContentManager->NotifyShutdown();
    }

    // Stop + drain the density-volume fill tasks (they read the Generator) before UObject teardown.
    if (DensityVolume)
    {
        DensityVolume->NotifyShutdown();
    }

    // Destroy any spawned atmosphere layer actors.
    if (AtmosphereManager)
    {
        AtmosphereManager->Reset();
    }

    // Unbind the data asset monitoring delegate
#if WITH_EDITOR
    if (OnObjectModifiedHandle.IsValid())
    {
        FCoreUObjectDelegates::OnObjectModified.Remove(OnObjectModifiedHandle);
        OnObjectModifiedHandle.Reset();
    }
#endif

    Super::EndPlay(EndPlayReason);
}

void AVoxelWorld::BeginPlay()
{
    Super::BeginPlay();
    bShuttingDown.store(false, std::memory_order_relaxed);

    if (!Settings)
    {
        UE_LOG(LogTemp, Error, TEXT("[VoxelWorld] No Settings assigned — world won't generate."));
        return;
    }

    // Tiles never move once generated, so make the actor root STATIC. RMC already requests the
    // cached static DRAW path (section group DrawType defaults to Static), but a Movable parent
    // forces every child back to Movable — which also defeats Virtual Shadow Map caching (Movable
    // geometry re-renders its shadow every frame; that's the cost that made us turn VSM off). With
    // a Static root + Static tile components, draws cache AND VSM can cache the terrain's shadows,
    // so VSM can be turned back on cheaply. Content decoration HISMs are likewise Static (placed once,
    // never move). Movable children (atmosphere fog/sky, water planes — these follow the player) under a
    // Static root are allowed. NOTE: a Static actor can't be moved in-editor —
    // VoxelWorld is expected to sit at the origin.
    if (USceneComponent* Root = GetRootComponent())
    {
        Root->SetMobility(EComponentMobility::Static);
    }

    // Générateur + mesher (UObjects légers)
    // TRANSLATION SEULEMENT / TRANSLATION ONLY.
    // Tout le systeme raisonne en boites ALIGNEES SUR LES AXES DU MONDE : bornes de tuile, verdicts
    // ClassifyBox, fenetre du clipmap, culling, distance de streaming. Une rotation ou une echelle
    // non unitaire fait qu une tuile cesse d etre alignee sur les axes en espace monde, et TOUTE
    // cette arithmetique devient fausse -- pas degradee : structurellement fausse.
    // The whole system reasons in WORLD-AXIS-ALIGNED boxes: tile bounds, ClassifyBox verdicts, the
    // clipmap window, culling, streaming distance. A rotation or non-unit scale means a tile is no
    // longer axis-aligned in world space and none of that arithmetic survives - it does not degrade,
    // it breaks structurally. Translating the actor IS supported (that is the point of the
    // WorldToLocal* helpers); rotating or scaling it is not.
    {
        const FTransform ActorXf = GetActorTransform();
        if (!ActorXf.GetRotation().Rotator().IsNearlyZero(0.01f) ||
            !ActorXf.GetScale3D().Equals(FVector::OneVector, 0.001f))
        {
            UE_LOG(LogTemp, Error,
                TEXT("[VoxelWorld] Actor has rotation %s / scale %s. VoxelForge supports TRANSLATION ONLY - ")
                TEXT("tile bounds, ClassifyBox verdicts, the clipmap window and culling all assume ")
                TEXT("world-axis-aligned boxes. Reset rotation to 0 and scale to 1."),
                *ActorXf.GetRotation().Rotator().ToString(), *ActorXf.GetScale3D().ToString());
        }
    }

    Generator = NewObject<UVoxelGenerator>(this);
    Mesher    = NewObject<UVoxelMarchingCubesMesher>(this);

    Generator->InitializeSettings(Settings);
    Mesher->SetGenerator(Generator);
    Mesher->bGenerateSkirts = Settings->bGenerateSkirts;
    Mesher->SkirtCells      = Settings->SkirtCells;
    Mesher->LODOctaveDrop   = Settings->LODOctaveDrop;   // T2.b — 0 = off

    // Système de strates — piloté par le pool et les fixed entries dans Settings.
    if (Settings->StratePool.Num() > 0)
    {
        StrateManager = NewObject<UVoxelStrateManager>(this);
        StrateManager->Initialize(Settings, Settings->Seed);
        Generator->SetStrateManager(StrateManager);
        UE_LOG(LogTemp, Log, TEXT("[VoxelWorld] Strate system initialized with %d strates"),
            StrateManager->GetNumStrates());
    }

    // Diff layer — stocke les modifications du joueur par dessus la densité
    // procédurale. Créé systématiquement (coût nul tant qu'il n'y a pas d'édit).
    DiffLayer = NewObject<UVoxelDiffLayer>(this);
    DiffLayer->SetBudget(Settings->MaxModifications, Settings->MaxBrushRadius, Settings->MaxTotalVolume);
    Generator->SetDiffLayer(DiffLayer);

    // Content manager — distance-based decoration grid (no LOD pop) + level-0 water planes.
    ContentManager = NewObject<UVoxelContentManager>(this);
    ContentManager->Initialize(this, StrateManager, Generator, Settings, Settings->Seed);

    // Atmosphere manager — per-strate fog + ambient + persistent ceiling/floor layers.
    if (bManageAtmosphere && StrateManager)
    {
        AtmosphereManager = NewObject<UVoxelAtmosphereManager>(this);
        AtmosphereManager->Initialize(this, StrateManager, Generator);
    }

    // Density volume — player-centred clipmap streamed to the GPU for mini-sun raymarched shadows.
    if (Settings->bEnableDensityVolume)
    {
        DensityVolume = NewObject<UVoxelDensityVolume>(this);
        DensityVolume->Initialize(this, Generator, Settings);
    }

#if WITH_EDITOR
    // Listen for data asset edits during PIE so live edit can detect
    // strate definition changes (PostEditChangeProperty only fires for
    // properties on this actor itself, not on referenced data assets).
    OnObjectModifiedHandle = FCoreUObjectDelegates::OnObjectModified.AddUObject(
        this, &AVoxelWorld::OnObjectModifiedInEditor);
#endif
}

void AVoxelWorld::Tick(float DeltaTime)
{
    Super::Tick(DeltaTime);
    TRACE_CPUPROFILER_EVENT_SCOPE(VoxelForge_Tick);   // game-thread streaming orchestration breakdown
    FVector PlayerLastPos = GetPlayerPosition();

    if ((PlayerLastPos != FVector::ZeroVector)) {
        UpdateChunksAroundPosition(PlayerLastPos);
        if (AtmosphereManager)
        {
            AtmosphereManager->UpdateForPlayer(PlayerLastPos);
        }
        if (ContentManager)
        {
            // Distance-based decoration streaming (no LOD pop). Cheap no-op unless the player crosses
            // a decoration cell boundary or changes strate; otherwise just drains the spawn budget.
            { TRACE_CPUPROFILER_EVENT_SCOPE(VoxelForge_UpdateDecorations); ContentManager->UpdateDecorations(PlayerLastPos); }
            // Rare hash-lattice landmarks (the "mini-suns") — cheap at any radius (scales with count, not area).
            // Landmarks now cover F7 set-pieces too (AnchorMode HashLattice/PassageMouth + exclusion).
            { TRACE_CPUPROFILER_EVENT_SCOPE(VoxelForge_UpdateLandmarks); ContentManager->UpdateLandmarks(PlayerLastPos); }
            // One strate-global ocean plane following the player (water at every LOD, to the horizon).
            { TRACE_CPUPROFILER_EVENT_SCOPE(VoxelForge_UpdateWater); ContentManager->UpdateWater(PlayerLastPos); }
        }
        if (DensityVolume)
        {
            // Density clipmap for mini-sun shadows: recentre + queue/launch/drain worker fills.
            // Cheap unless the player crossed a level-0 cell boundary or a carve dirtied cells.
            TRACE_CPUPROFILER_EVENT_SCOPE(VoxelForge_UpdateDensityVolume);
            DensityVolume->Update(PlayerLastPos);
            // Push the clipmap transform + nearest orb to the terrain MIDs (the material's shadow march).
            UpdateTerrainMaterialParams();
        }
        // Bounded-directional mini-sun lighting: stream the nearest orbs into the Light Function MPC.
        // Independent of the density volume (self-guards on OrbLightMPC); this is the replacement path.
        UpdateOrbLightMPC();
    }
    ProcessPendingChunks();
    ProcessUnloadQueue();

#if ENABLE_DRAW_DEBUG
    // Density-volume overlay (step 1a): cyan boxes for solid level-0 cells near the player.
    if (DensityVolume) { DensityVolume->DebugDraw(); }

    // Inter-strate passage overlay (cyan path, green=upper / red=lower endpoints).
    // Points are in voxel coords → world units (×VOXEL_SIZE) → actor space.
    if (bDebugDrawPassages && StrateManager)
    {
        const FTransform Xf = GetActorTransform();
        auto ToWorld = [&](const FVector& VoxelPt) { return Xf.TransformPosition(VoxelPt * VOXEL_SIZE); };
        auto DrawSeg = [&](const FVector& A, const FVector& B)
        {
            DrawDebugLine(GetWorld(), ToWorld(A), ToWorld(B), FColor::Cyan, false, -1.0f, 0, 30.0f);
        };

        for (const FVoxelPassage& P : StrateManager->GetPassages())
        {
            if (P.ControlPoints.Num() >= 2)
            {
                for (int32 j = 0; j < P.ControlPoints.Num() - 1; ++j)
                    DrawSeg(P.ControlPoints[j], P.ControlPoints[j + 1]);
            }
            else
            {
                DrawSeg(P.UpperPoint, P.LowerPoint);
            }

            DrawDebugSphere(GetWorld(), ToWorld(P.UpperPoint), P.Radius * VOXEL_SIZE, 12, FColor::Green, false, -1.0f, 0, 4.0f);
            DrawDebugSphere(GetWorld(), ToWorld(P.LowerPoint), P.Radius * VOXEL_SIZE, 12, FColor::Red,   false, -1.0f, 0, 4.0f);
        }
    }
#endif
}

//=============================================================================
// ESPACE MONDE <-> ESPACE ACTEUR / WORLD <-> ACTOR SPACE
//=============================================================================
// Le champ de densite est en espace ACTEUR. Ces trois fonctions sont la SEULE frontiere
// autorisee entre les coordonnees Unreal et les coordonnees voxel (cf. VoxelWorld.h).
// The density field is in ACTOR space. These three are the ONLY sanctioned boundary between
// Unreal coordinates and voxel coordinates (see VoxelWorld.h).

FVector AVoxelWorld::WorldToLocalCm(FVector WorldPos) const
{
    return GetActorTransform().InverseTransformPosition(WorldPos);
}

FVector AVoxelWorld::WorldToLocalVoxel(FVector WorldPos) const
{
    return GetActorTransform().InverseTransformPosition(WorldPos) / VOXEL_SIZE;
}

FVector AVoxelWorld::LocalVoxelToWorld(FVector VoxelPos) const
{
    return GetActorTransform().TransformPosition(VoxelPos * VOXEL_SIZE);
}

FVector AVoxelWorld::GetPlayerPosition() const
{
    // This one is tricky with Unreal's API, so I'll give you more help:
    APlayerController* PC = GetWorld()->GetFirstPlayerController();
    if (PC && PC->GetPawn())
    {
        return PC->GetPawn()->GetActorLocation();
    }
    return FVector::ZeroVector;
}

void AVoxelWorld::ProcessPendingChunks()
{
    TRACE_CPUPROFILER_EVENT_SCOPE(VoxelForge_ProcessPending);
    // This runs on the game thread, called from Tick.
    //
    // STEPS:
    // 1. Try to dequeue a result from ProcessQueue
    //    TQueue has a Dequeue(OutItem) method that returns true if it got something
    //
    // 2. If we got a result:
    //    a. Store the chunk data in our Chunks map
    //    b. Apply the mesh so it becomes visible
    //    c. Remove the coord from PendingChunkCoord (it's no longer "in progress")
    //
    // 3. You can process multiple results per frame with a while loop,
    //    or limit to a few per frame to avoid stutters (e.g., max 4 per tick)
    //
    // TOOLS:
    // - ProcessQueue.Dequeue(Result) — returns bool, fills Result if true
    // - Chunks.Add(Key, Value)
    // - ApplyMeshToChunk(ChunkCoord, MeshData)
    // - PendingChunkCoord.Remove(ChunkCoord)
    // Drain the process queue up to the per-frame budget.
    // This prevents stutters from applying too many meshes in one frame.
    // Budget limits how many VISIBLE mesh applies we do per frame (GPU upload cost).
    // Empty meshes and stale results are free to drain — don't count them.
    const int32 MaxApplies = Settings ? Settings->MaxMeshAppliesPerFrame : 4;
    int32 MeshesApplied = 0;
    FChunkResult DequeuedChunk;
    while (ProcessQueue.Dequeue(DequeuedChunk))
    {
        PendingTiles.Remove(DequeuedChunk.Tile);

        // ApplyTileResult does epoch check, mark-loaded, capture ingest, empty-release / mesh upload.
        // Only a real (visible) upload counts against the per-frame budget — stale/empty drain free.
        if (ApplyTileResult(DequeuedChunk))
        {
            if (++MeshesApplied >= MaxApplies)
            {
                break;
            }
        }
    }

}

// Game-thread apply for one gen result. Shared by ProcessPendingChunks (async drain) and
// SyncRemeshTile (synchronous carve). Returns true iff a visible mesh was uploaded (budget).
bool AVoxelWorld::ApplyTileResult(FChunkResult& Result)
{
    // Discard results from a previous generation epoch (stale).
    if (Result.Epoch != GenerationEpoch)
    {
        return false;
    }

    // Mark the tile loaded (even if empty — so we don't re-submit it).
    LoadedTiles.Add(Result.Tile);

    // Une tuile en vol n'est JAMAIS annulée : si le desired set a bougé pendant sa gen, elle
    // arrive ici hors desired — le delta cull ne re-scanne plus tout, donc on l'inscrit en
    // TransitionHold pour qu'elle soit re-considérée au prochain crossing (ou au settled cull).
    if (!IsDesired(Result.Tile)) { AddToTransitionHold(Result.Tile); }

    // CAPTURE-DURING-MESHING: hand the mesher's captured density grid to the clipmap BEFORE the
    // empty-tile early-out — all-air / all-solid tiles are exactly the uniform cells the volume
    // needs, and they carry a valid CaptureGrid even though they render nothing.
    if (DensityVolume && Result.CaptureGrid.Num() > 0)
    {
        DensityVolume->IngestTileCapture(Result.Tile.Coord, MoveTemp(Result.CaptureGrid));
    }

    // Empty mesh = all-air tile — nothing to render, but still "loaded".
    if (Result.bEmpty || !Result.Streams)
    {
        // Une RE-GEN (BandRemeshQueue / RemeshDirtyChunks) peut passer de "contenu" à "vide" :
        // bande déplacée hors de la tuile, ou skip cellule-plus-haute-que-la-bande après un
        // changement de strate (LoadTile). L'ancien composant doit tomber, sinon sa vieille
        // géométrie (l'autre strate !) reste affichée. Première gen vide : Find rate, no-op.
        if (URealtimeMeshComponent** OldComp = TileComponents.Find(Result.Tile))
        {
            if (*OldComp) { ReleaseTileComponent(*OldComp); }
            TileComponents.Remove(Result.Tile);
        }
        return false;
    }

    // Apply mesh (GPU upload). The vertex/index buffers were already built (T1.f, on the worker for
    // the async path or inline for the sync carve path); the game thread only uploads them here.
    ApplyMeshToTile(Result);
    return true;
}

// Same-frame level-0 re-mesh on the game thread (see header). Mirrors LoadTile's level-0 parameters
// (Cells = CHUNK_SIZE, Step = 1) + the strate content band; skips density-volume capture (the volume
// is refilled from the diff via MarkDirtyVoxelBox in RemeshDirtyChunks).
void AVoxelWorld::SyncRemeshTile(const FVoxelTileKey& Tile)
{
    if (!Generator || !Mesher || ShouldAbortWork()) return;

    const FIntVector OriginVoxels = Tile.OriginVoxels();
    const int32 Cells = CHUNK_SIZE;   // level 0 is always full-res (level 0 < FullResClipLevels)
    const int32 Step  = 1;            // Extent(=CHUNK_SIZE) / Cells

    // STRATE CONTENT CUT — identical to LoadTile (Tile.Level >= CutMin; for a level-0 tile inside the
    // player strate the clamp is a no-op, but keep it bit-identical to the async path). Too-coarse
    // skip never fires at Step 1.
    int32 BandVoxLo = INT32_MIN, BandVoxHi = INT32_MAX;
    int32 BandChunkLo = MIN_int32, BandChunkHi = MAX_int32;
    const int32 CutMin = Settings ? Settings->StrateContentCutMinLevel : 9;
    if (Tile.Level >= CutMin && MeshBandChunkLo != MIN_int32)
    {
        BandChunkLo = MeshBandChunkLo;
        BandChunkHi = MeshBandChunkHi;
        BandVoxLo   = MeshBandChunkLo * CHUNK_SIZE;
        BandVoxHi   = (MeshBandChunkHi + 1) * CHUNK_SIZE - 1;
    }

    FChunkResult Result;
    GenerateTileResult(Tile, OriginVoxels, Step, Cells, GenerationEpoch, /*bWantCapture*/ false,
                       BandVoxLo, BandVoxHi, BandChunkLo, BandChunkHi,
                       /*bSheetTile*/ false, /*SheetChunkZ*/ 0,
                       /*Hole*/ 0, 0, 0, 0, Result);   // hole unused (not a sheet tile)

    ApplyTileResult(Result);
}

void AVoxelWorld::ProcessUnloadQueue()
{
    TRACE_CPUPROFILER_EVENT_SCOPE(VoxelForge_ProcessUnload);
    // Budgeted teardown: destroy at most a few tiles' components + content actors per frame, so a
    // fast traversal's whole-shell cull (dozens of UnloadTile in one frame) doesn't spike the game
    // thread. The budget auto-scales with the backlog (PendingUnload/4) so it never falls far behind.
    if (PendingUnload.Num() == 0) return;

    // Drain the floor budget, scaling up with the backlog so we never fall far behind, but capped
    // at 4× the floor so a huge backlog (extreme speed) can't itself become a one-frame spike — the
    // excess just lingers a few more frames (it's all behind the player, out of view).
    const int32 Floor = Settings ? FMath::Max(1, Settings->MaxUnloadsPerFrame) : 6;
    int32 DestroyBudget = FMath::Clamp(PendingUnload.Num() / 4, Floor, Floor * 4);

    TArray<FVoxelTileKey> Dequeued;   // removed from the queue this frame (destroyed OR cancelled)
    for (const FVoxelTileKey& T : PendingUnload)
    {
        if (IsDesired(T))
        {
            // Re-desired before its turn (player reversed) — keep it; it's still loaded, just drop
            // it from the queue. Doesn't count against the destroy budget.
            Dequeued.Add(T);
            continue;
        }
        UnloadTile(T);
        Dequeued.Add(T);
        if (--DestroyBudget <= 0) break;
    }
    for (const FVoxelTileKey& T : Dequeued) PendingUnload.Remove(T);
}

// Integer floor-division (correct for negatives), scalar + vector.
static FORCEINLINE int32 VF_FloorDiv(int32 V, int32 D)
{
    return V >= 0 ? (V / D) : -(((-V) + D - 1) / D);
}
static FORCEINLINE FIntVector VF_FloorDiv(const FIntVector& V, int32 D)
{
    return FIntVector(VF_FloorDiv(V.X, D), VF_FloorDiv(V.Y, D), VF_FloorDiv(V.Z, D));
}

// RENDER DISTANCE — rayon (en tuiles niveau-MaxLevel) de la coquille EXTERNE : ClipRadius, élargi
// si `RenderDistanceChunks` demande une portée horizontale au-delà du naturel R·2^MaxLevel. Partagé
// par BuildDesiredTiles (le desired set) et IsTileInClipRange (le même horizon pour le cull).
static FORCEINLINE int32 VF_OuterShellRadius(const UVoxelSettings* Settings, int32 R, int32 MaxLevel)
{
    const int32 Dist = Settings ? Settings->RenderDistanceChunks : 0;
    if (Dist <= 0) return R;
    return FMath::Max(R, (Dist + (1 << MaxLevel) - 1) >> MaxLevel);   // ceil(Dist / 2^MaxLevel)
}

// F18 — la coquille LA PLUS EXTERNE : niveau + rayon. Sans anneau feuille = (MaxLevel, rayon
// render-distance). Avec (`bFarSheetRing` et distance > portée naturelle) = l'anneau FEUILLE :
// niveau MaxLevel + FarSheetSpanLevels (une feuille couvre 2^span empreintes MC par axe), rayon
// re-dérivé à ce niveau. Partagé par BuildDesiredTiles et IsTileInClipRange (même horizon).
static FORCEINLINE void VF_OuterShell(const UVoxelSettings* Settings, int32 R, int32 MaxLevel,
                                      int32& OutLevel, int32& OutRadius)
{
    OutLevel  = MaxLevel;
    OutRadius = VF_OuterShellRadius(Settings, R, MaxLevel);
    if (Settings && Settings->bFarSheetRing && OutRadius > R)
    {
        OutLevel  = MaxLevel + FMath::Clamp(Settings->FarSheetSpanLevels, 1, 4);
        OutRadius = FMath::Max(1, (Settings->RenderDistanceChunks + (1 << OutLevel) - 1) >> OutLevel);
    }
}

void AVoxelWorld::BuildDesiredTiles(const FIntVector& Center, TArray<FVoxelTileKey>& OutLeavers)
{
    TRACE_CPUPROFILER_EVENT_SCOPE(VoxelForge_BuildDesiredTiles);
    DesiredSorted.Reset();
    OutLeavers.Reset();
    CollisionOnlyTiles.Reset();   // §9.4 — rebuilt by AddAnchorDesiredTiles below
    ++DesiredStamp;   // les upserts ci-dessous marquent le crossing courant

    const int32 R        = Settings ? FMath::Max(1, Settings->ClipRadius) : 3;
    const int32 MaxLevel = Settings ? FMath::Clamp(Settings->MaxClipLevel, 0, 8) : 4;

    // RENDER DISTANCE (`RenderDistanceChunks`) : la coquille EXTERNE continue au-delà du rayon
    // naturel jusqu'à couvrir la distance demandée — en tuiles MC niveau-MaxClipLevel, ou (F18,
    // `bFarSheetRing`) en tuiles FEUILLE plus grandes (niveau MaxLevel+span, deux heightfields
    // sol/cap au lieu de marching cubes — cf. GenerateSheetMesh). IsTileInClipRange partage
    // VF_OuterShell pour que le cull voie le même horizon.
    const int32 ROuter = VF_OuterShellRadius(Settings, R, MaxLevel);
    int32 SheetLevel = MaxLevel, RSheet = ROuter;
    VF_OuterShell(Settings, R, MaxLevel, SheetLevel, RSheet);
    const bool bSheetRing = SheetLevel > MaxLevel;

    // Strate-aware VERTICAL band (in level-0 chunk-Z). Without this the clipmap generates the
    // occluded volume above/below (sealed strates are light-tight, §8.7) AND the full underground
    // depth — a huge column of invisible solid rock = the gen lag. So clamp the vertical reach to
    // the player's strate ± margin (open-ceiling strates extend UP to the sky-cap). The clipmap
    // stays full-reach HORIZONTALLY (the horizon) but limited vertically. ZLo/ZHi span ⇒ no clamp.
    int32 ZLo = MIN_int32, ZHi = MAX_int32;
    if (Settings && Settings->bClampViewToStrate && StrateManager)
    {
        const int32 Margin = Settings->StrateViewMarginChunks;
        ZLo = Center.Z - Settings->ViewDistanceDown;
        ZHi = Center.Z + Settings->ViewDistanceUp;
        int32 StrTopZ = 0, StrBotZ = 0;
        if (StrateManager->GetStrateChunkZBounds(Center.Z, StrTopZ, StrBotZ))
        {
            const ECaveGeneratorType GenType = StrateManager->GetGeneratorTypeForChunk(Center);
            const bool bOpen = (GenType == ECaveGeneratorType::SurfaceWorld
                             || GenType == ECaveGeneratorType::FloatingIslands);
            ZLo = FMath::Max(ZLo, StrBotZ - Margin);
            ZHi = bOpen ? (StrTopZ + Margin) : FMath::Min(ZHi, StrTopZ + Margin);
        }
        // else: in the bedrock gap → keep the player window (see both sides while descending).
    }

    // Concentric shells: level 0 near the player, each coarser level a 2× larger shell beyond.
    // A level-L tile is dropped if it's fully covered by the finer (L-1) level's box — that's
    // the inner hole, so the shells tile space without big gaps.
    for (int32 L = 0; L <= MaxLevel; ++L)
    {
        const int32 Pow   = 1 << L;                       // level-L tile = 2^L chunks
        const FIntVector CL = VF_FloorDiv(Center, Pow);   // player's level-L tile coord
        const FIntVector CF = (L > 0) ? VF_FloorDiv(Center, Pow >> 1) : FIntVector::ZeroValue;

        // Rayon de CE niveau : R partout, sauf la coquille externe (render distance) — qui, si
        // l'anneau FEUILLE est actif (F18), est émise à part plus bas (le niveau MaxLevel reste
        // alors à R). Le test "covered by finer" garde R (le niveau plus fin n'est jamais étendu).
        const int32 RL = (L == MaxLevel && !bSheetRing) ? ROuter : R;

        // Anneau étendu : le balayage naïf serait (2·RL+1)³ — on restreint dz à la fenêtre de la
        // clamp verticale AVANT la boucle (mêmes tuiles retenues : le `continue` Z ci-dessous
        // rejetterait tout le reste). Sentinelles MIN/MAX (pas de clamp) ⇒ balayage plein.
        int32 DzMin = -RL, DzMax = RL;
        if (RL > R && ZLo != MIN_int32)
        {
            DzMin = FMath::Max(DzMin, VF_FloorDiv(ZLo, Pow) - CL.Z);
            DzMax = FMath::Min(DzMax, VF_FloorDiv(ZHi, Pow) - CL.Z);
        }

        for (int32 dz = DzMin; dz <= DzMax; ++dz)
        for (int32 dy = -RL; dy <= RL; ++dy)
        for (int32 dx = -RL; dx <= RL; ++dx)
        {
            const FIntVector T = CL + FIntVector(dx, dy, dz);

            // Strate-aware vertical clamp: drop tiles whose level-0 chunk-Z footprint doesn't
            // overlap [ZLo, ZHi] (the occluded strate above/below / deep underground).
            const int32 TZLo = T.Z << L;
            const int32 TZHi = ((T.Z + 1) << L) - 1;
            if (TZHi < ZLo || TZLo > ZHi) continue;

            if (L > 0)
            {
                // Covered by the finer level iff the level-(L-1) tiles 2T..2T+1 (per axis)
                // all lie inside the finer box [CF-R, CF+R].
                const bool bCovered =
                    (2 * T.X     >= CF.X - R) && (2 * T.X + 1 <= CF.X + R) &&
                    (2 * T.Y     >= CF.Y - R) && (2 * T.Y + 1 <= CF.Y + R) &&
                    (2 * T.Z     >= CF.Z - R) && (2 * T.Z + 1 <= CF.Z + R);
                if (bCovered) continue;
            }
            const FVoxelTileKey Key(T, L);
            DesiredSorted.Add(Key);
            DesiredStamped.FindOrAdd(Key) = DesiredStamp;
        }
    }

    // F18 — ANNEAU FEUILLE : la portée render-distance est couverte par des tuiles feuille
    // (niveau SheetLevel > MaxLevel, LoadTile route niveau > MaxClipLevel vers GenerateSheetMesh).
    // Trou intérieur = la boîte MC niveau-MaxLevel (rayon R), pas le niveau SheetLevel−1.
    if (bSheetRing)
    {
        const int32 SPow = 1 << SheetLevel;
        const FIntVector CS = VF_FloorDiv(Center, SPow);
        const FIntVector CM = VF_FloorDiv(Center, 1 << MaxLevel);
        const int32 K = SheetLevel - MaxLevel;   // 1 feuille = 2^K tuiles MC par axe

        int32 DzMin = -RSheet, DzMax = RSheet;
        if (ZLo != MIN_int32)
        {
            DzMin = FMath::Max(DzMin, VF_FloorDiv(ZLo, SPow) - CS.Z);
            DzMax = FMath::Min(DzMax, VF_FloorDiv(ZHi, SPow) - CS.Z);
        }

        for (int32 dz = DzMin; dz <= DzMax; ++dz)
        for (int32 dy = -RSheet; dy <= RSheet; ++dy)
        for (int32 dx = -RSheet; dx <= RSheet; ++dx)
        {
            const FIntVector T = CS + FIntVector(dx, dy, dz);
            const int32 TZLo = T.Z << SheetLevel;
            const int32 TZHi = ((T.Z + 1) << SheetLevel) - 1;
            if (TZHi < ZLo || TZLo > ZHi) continue;

            // Couverte par la boîte MC (empreinte entièrement dans [CM−R, CM+R] au niveau MaxLevel).
            const bool bCovered =
                ((T.X << K) >= CM.X - R) && ((((T.X + 1) << K) - 1) <= CM.X + R) &&
                ((T.Y << K) >= CM.Y - R) && ((((T.Y + 1) << K) - 1) <= CM.Y + R) &&
                ((T.Z << K) >= CM.Z - R) && ((((T.Z + 1) << K) - 1) <= CM.Z + R);
            if (bCovered) continue;

            const FVoxelTileKey Key(T, SheetLevel);
            DesiredSorted.Add(Key);
            DesiredStamped.FindOrAdd(Key) = DesiredStamp;
        }
    }

    // Streaming anchors (AI / remote players, §9.3): fold each one's small level-0 box into the SAME
    // desired set BEFORE the leaver sweep, so the delta cull releases an anchor's tiles automatically
    // once it moves away or is unregistered. No-op (zero cost) when there are no anchors.
    AddAnchorDesiredTiles();

    // Balayage UNIQUE de la map : les entrées à stamp périmé viennent de quitter le desired set —
    // ce sont les seuls candidats au cull de ce crossing (avec la TransitionHold). On les retire
    // ici même (RemoveCurrent est sûr en itérant), la map reste donc == desired set courant.
    for (auto It = DesiredStamped.CreateIterator(); It; ++It)
    {
        if (It.Value() != DesiredStamp)
        {
            OutLeavers.Add(It.Key());
            It.RemoveCurrent();
        }
    }

    // Nearest-first (by tile-centre distance to the player), so the closest tiles stream first.
    const FVector PlayerVoxel = (FVector(Center) + FVector(0.5f, 0.5f, 0.5f)) * (float)CHUNK_SIZE;
    DesiredSorted.Sort([&PlayerVoxel](const FVoxelTileKey& A, const FVoxelTileKey& B)
    {
        const FVector CA = A.CenterCm() / VOXEL_SIZE;
        const FVector CB = B.CenterCm() / VOXEL_SIZE;
        return FVector::DistSquared(CA, PlayerVoxel) < FVector::DistSquared(CB, PlayerVoxel);
    });
}

// Fold every registered anchor's small level-0 box into the current desired set (§9.3). Runs inside
// BuildDesiredTiles after the player clipmap + sheet ring, keyed on the same DesiredStamp so the leaver
// sweep + delta cull handle anchor tiles leaving. Level-0 only (collision lives on level-0 tiles); empty
// tiles in the box are ~free (the trivial-tile reject skips gen). Dedup vs the player clipmap by stamp.
// §9.4: a tile the player clipmap did NOT stamp (bNew) that only a CollisionOnly anchor wants goes into
// CollisionOnlyTiles → hidden at apply. A FullVisual anchor (or the clipmap) forces it rendered.
void AVoxelWorld::AddAnchorDesiredTiles()
{
    for (const FVoxelStreamingAnchor& Anchor : StreamingAnchors)
    {
        if (!Anchor.Actor.IsValid()) continue;   // dead ptr — pruned in UpdateChunksAroundPosition
        const FIntVector AC = Anchor.LastChunk;   // set this Tick by the move-detection pass
        const int32 RXY = FMath::Clamp(Anchor.XYRadiusChunks, 0, 4);   // guard the box small
        const int32 RZLo = FMath::Clamp(Anchor.ZBelowChunks, 0, 4);
        const int32 RZHi = FMath::Clamp(Anchor.ZAboveChunks, 0, 4);
        const bool bColl = (Anchor.Policy == EVoxelAnchorPolicy::CollisionOnly);
        for (int32 dz = -RZLo; dz <= RZHi; ++dz)
        for (int32 dy = -RXY;  dy <= RXY;  ++dy)
        for (int32 dx = -RXY;  dx <= RXY;  ++dx)
        {
            const FVoxelTileKey Key(AC + FIntVector(dx, dy, dz), 0);
            uint32& S = DesiredStamped.FindOrAdd(Key);
            const bool bNew = (S != DesiredStamp);   // false ⇒ already desired (clipmap / earlier anchor)
            if (bNew)
            {
                S = DesiredStamp;
                DesiredSorted.Add(Key);
            }
            if (bColl)
            {
                // Collision-only only if NOTHING full-visual claimed this exact level-0 tile this
                // crossing (bNew). If the clipmap or a FullVisual anchor stamped it first, leave it rendered.
                if (bNew) { CollisionOnlyTiles.Add(Key); }
            }
            else
            {
                CollisionOnlyTiles.Remove(Key);   // FullVisual anchor → force rendered (undo a prior coll mark)
            }
        }
    }
}

// §9.4 — apply visibility flips to ALREADY-LOADED tiles when a tile changed render↔collision-only this
// crossing (player walked toward/away from a CollisionOnly cluster). Bounded by the collision-only set
// (small); a no-op when there are no CollisionOnly anchors. Newly-loaded tiles get their state at apply
// (ApplyMeshToTile reads CollisionOnlyTiles). Called after BuildDesiredTiles rebuilt CollisionOnlyTiles.
void AVoxelWorld::ReconcileAnchorTileVisibility()
{
    if (CollisionOnlyTiles.Num() == 0 && PrevCollisionOnlyTiles.Num() == 0) return;   // fast path

    // Became collision-only → hide (if loaded).
    for (const FVoxelTileKey& Key : CollisionOnlyTiles)
    {
        if (!PrevCollisionOnlyTiles.Contains(Key))
        {
            if (URealtimeMeshComponent* Comp = TileComponents.FindRef(Key)) { Comp->SetVisibility(false); }
        }
    }
    // Stopped being collision-only → show, but only if still desired (else it's a leaver being culled —
    // don't flash it visible on its way out).
    for (const FVoxelTileKey& Key : PrevCollisionOnlyTiles)
    {
        if (!CollisionOnlyTiles.Contains(Key) && IsDesired(Key))
        {
            if (URealtimeMeshComponent* Comp = TileComponents.FindRef(Key)) { Comp->SetVisibility(true); }
        }
    }
    PrevCollisionOnlyTiles = CollisionOnlyTiles;
}

void AVoxelWorld::RegisterStreamingAnchor(AActor* Actor, EVoxelAnchorPolicy Policy,
                                          int32 XYRadiusChunks, int32 ZBelowChunks, int32 ZAboveChunks)
{
    if (!Actor) return;
    for (FVoxelStreamingAnchor& Existing : StreamingAnchors)
    {
        if (Existing.Actor.Get() == Actor)   // already registered → update policy/box in place
        {
            Existing.Policy         = Policy;
            Existing.XYRadiusChunks = XYRadiusChunks;
            Existing.ZBelowChunks   = ZBelowChunks;
            Existing.ZAboveChunks   = ZAboveChunks;
            bForceDesiredRebuild    = true;
            return;
        }
    }
    FVoxelStreamingAnchor A;
    A.Actor          = Actor;
    A.Policy         = Policy;
    A.XYRadiusChunks = XYRadiusChunks;
    A.ZBelowChunks   = ZBelowChunks;
    A.ZAboveChunks   = ZAboveChunks;
    // LastChunk stays at its sentinel → next Tick's move detection sets it + triggers the rebuild.
    StreamingAnchors.Add(A);
    bForceDesiredRebuild = true;
}

void AVoxelWorld::UnregisterStreamingAnchor(AActor* Actor)
{
    if (!Actor) return;
    for (int32 i = StreamingAnchors.Num() - 1; i >= 0; --i)
    {
        if (StreamingAnchors[i].Actor.Get() == Actor)
        {
            StreamingAnchors.RemoveAtSwap(i);
            bForceDesiredRebuild = true;   // rebuild next Tick so its now-unwanted tiles become leavers
        }
    }
}

bool AVoxelWorld::IsTileInClipRange(const FVoxelTileKey& Tile, const FIntVector& Center) const
{
    // In range = the tile's centre falls within the OUTERMOST shell (VF_OuterShell — the
    // render-distance/sheet-ring level+radius, same as BuildDesiredTiles). A loaded-but-not-
    // desired tile in range is mid-LOD-transition (wait for its replacement); one out of range
    // has left the view entirely (cull immediately).
    const int32 R        = Settings ? FMath::Max(1, Settings->ClipRadius) : 3;
    const int32 MaxLevelBase = Settings ? FMath::Clamp(Settings->MaxClipLevel, 0, 8) : 4;
    int32 MaxLevel = MaxLevelBase, ROuter = R;
    VF_OuterShell(Settings, R, MaxLevelBase, MaxLevel, ROuter);
    const int32 PowMax   = 1 << MaxLevel;
    const FIntVector CMax = VF_FloorDiv(Center, PowMax);

    const FVector CV = Tile.CenterCm() / VOXEL_SIZE;   // tile centre in voxels
    const int32 SizeMax = CHUNK_SIZE * PowMax;
    const FIntVector TMax(
        VF_FloorDiv(FMath::FloorToInt(CV.X), SizeMax),
        VF_FloorDiv(FMath::FloorToInt(CV.Y), SizeMax),
        VF_FloorDiv(FMath::FloorToInt(CV.Z), SizeMax));

    return FMath::Abs(TMax.X - CMax.X) <= ROuter
        && FMath::Abs(TMax.Y - CMax.Y) <= ROuter
        && FMath::Abs(TMax.Z - CMax.Z) <= ROuter;
}

int32 AVoxelWorld::GetMaxConcurrentTasks() const
{
    // T2.d — the asset value, capped to the spare LOGICAL cores. BackgroundNormal priority
    // (see LoadTile) already stops gen from starving the frame; this cap stops a flat 16 from
    // thrashing context switches on small CPUs where 16 > the machine's spare parallelism.
    const int32 Asset = Settings ? Settings->MaxConcurrentTasks : 16;
    const int32 SpareCores = FMath::Max(2, FPlatformMisc::NumberOfCoresIncludingHyperthreads() - 2);
    return FMath::Clamp(Asset, 1, SpareCores);
}

void AVoxelWorld::UpdateChunksAroundPosition(const FVector& CenterPosition)
{
    TRACE_CPUPROFILER_EVENT_SCOPE(VoxelForge_UpdateChunks);
    const int32 MaxTasks = GetMaxConcurrentTasks();

    const FIntVector CenterChunk = WorldToChunkCoord(WorldToLocalCm(CenterPosition));  // player's level-0 tile
    CurrentCenterChunk = CenterChunk;

    // Streaming anchors (AI / remote players, §9.3): prune dead ones + detect chunk crossings so the
    // desired set rebuilds when an anchor moves (its box of collision tiles follows it). Cheap: a few
    // WorldToChunkCoord per anchor, and empty (no anchors) is a zero-iteration loop.
    bool bAnchorsMoved = false;
    for (int32 i = StreamingAnchors.Num() - 1; i >= 0; --i)
    {
        AActor* A = StreamingAnchors[i].Actor.Get();
        if (!A)
        {
            StreamingAnchors.RemoveAtSwap(i);   // destroyed → drop it; its tiles must be culled
            bAnchorsMoved = true;
            continue;
        }
        const FIntVector AC = WorldToChunkCoord(WorldToLocalCm(A->GetActorLocation()));
        if (AC != StreamingAnchors[i].LastChunk)
        {
            StreamingAnchors[i].LastChunk = AC;
            bAnchorsMoved = true;
        }
    }

    //=========================================================================
    // Rebuild the desired tile set when the player crosses a level-0 tile boundary — or when a
    // streaming anchor moved / (un)registered (bAnchorsMoved / bForceDesiredRebuild).
    //=========================================================================
    if (CenterChunk != LastUpdateCenter || bAnchorsMoved || bForceDesiredRebuild)
    {
        LastUpdateCenter = CenterChunk;
        bAllChunksLoaded = false;
        bForceDesiredRebuild = false;   // consumed

        // DELTA CULL: BuildDesiredTiles renvoie les LEAVERS (désirées au crossing précédent, plus
        // maintenant). Seuls candidats au cull : ces leavers + la TransitionHold (retenues des
        // crossings passés). Fini le re-scan de TOUTES les tuiles chargées à chaque crossing —
        // c'était le spike CullTiles ~1.6 ms/crossing (trace 2026-07-05). Les tuiles en vol qui
        // finissent hors desired sont capturées à l'apply (ProcessPendingChunks → TransitionHold),
        // et le settled cull (tout chargé) reste le filet de sécurité plein-scan.
        TArray<FVoxelTileKey> Leavers;
        BuildDesiredTiles(CenterChunk, Leavers);

        // §9.4 — toggle visibility on already-loaded tiles that flipped render↔collision-only this
        // crossing (a CollisionOnly cluster the player just walked toward/away from). No-op w/o anchors.
        ReconcileAnchorTileVisibility();

        // STRATE CONTENT CUT — the band coarse tiles are meshed to = the player strate's EXACT
        // chunk-Z bounds (no margin: that's the view clamp's job — selection vs content). In the
        // inter-strate gap: no band (full tiles, you can see both sides through the descent).
        // On band change (strate transition), re-queue the loaded coarse tiles whose mesh depends
        // on it — fully inside BOTH bands ⇒ identical either way; fully outside both ⇒ empty
        // either way; everything else re-gens in place via BandRemeshQueue (no visual pop).
        {
            const int32 CutMin = Settings ? Settings->StrateContentCutMinLevel : 9;
            // F18 — l'anneau feuille dépend aussi de la bande (sa strate de référence) : on l'arme
            // dès que les feuilles sont actives, même si la coupe de contenu MC est désactivée.
            const bool bSheetsWantBand = Settings && Settings->bFarSheetRing
                                      && Settings->RenderDistanceChunks > 0;
            const int32 TopMC = Settings ? FMath::Clamp(Settings->MaxClipLevel, 0, 8) : 4;
            int32 NewLo = MIN_int32, NewHi = MAX_int32;
            if ((CutMin <= 8 || bSheetsWantBand) && StrateManager)
            {
                int32 StrTopZ = 0, StrBotZ = 0;
                if (StrateManager->GetStrateChunkZBounds(CenterChunk.Z, StrTopZ, StrBotZ))
                {
                    NewLo = StrBotZ;
                    NewHi = StrTopZ;
                }
            }
            if (NewLo != MeshBandChunkLo || NewHi != MeshBandChunkHi)
            {
                // Diagnostic volontairement VISIBLE : si cette ligne n'apparaît JAMAIS dans
                // l'Output Log, la coupe de contenu ne s'est jamais armée (bounds de strate
                // introuvables pour la position du PION → tout se maille plein, comme avant).
                UE_LOG(LogTemp, Warning,
                    TEXT("[VoxelWorld] Strate content band -> chunks [%d..%d] (was [%d..%d]), CutMinLevel=%d, pawn chunk Z=%d"),
                    NewLo, NewHi, MeshBandChunkLo, MeshBandChunkHi, CutMin, CenterChunk.Z);
                for (const FVoxelTileKey& T : LoadedTiles)
                {
                    // Feuilles (niveau > MaxClipLevel) : toujours dépendantes de la bande.
                    if (T.Level < CutMin && T.Level <= TopMC) continue;
                    const int32 CLo = T.Coord.Z << T.Level;
                    const int32 CHi = ((T.Coord.Z + 1) << T.Level) - 1;
                    const bool bInOld  = CLo >= MeshBandChunkLo && CHi <= MeshBandChunkHi;
                    const bool bInNew  = CLo >= NewLo && CHi <= NewHi;
                    const bool bOutOld = CHi < MeshBandChunkLo || CLo > MeshBandChunkHi;
                    const bool bOutNew = CHi < NewLo || CLo > NewHi;
                    if ((bInOld && bInNew) || (bOutOld && bOutNew)) continue;   // même contenu
                    if (!IsDesired(T)) continue;                                // sera cull, pas re-gen
                    BandRemeshQueue.Add(T);
                }
                MeshBandChunkLo = NewLo;
                MeshBandChunkHi = NewHi;
                bAllChunksLoaded = false;   // le drain de BandRemeshQueue vit dans le bloc submit
            }
        }

        // F18 — TROU XY de l'anneau feuille (voir VoxelWorld.h) : boîte MC niveau-MaxClipLevel
        // autour du joueur, rétrécie d'UNE tuile — le raccord feuille↔anneau MC garde une tuile
        // MC pleine de recouvrement (même pas d'échantillonnage des deux côtés → discret), et en
        // avançant, les tuiles MC de la zone nouvellement découpée étaient déjà desired au
        // crossing précédent (chargées avant que le trou ne les découvre). Changement (crossing
        // de tuile MaxClipLevel, ~tous les 2^L chunks) ⇒ re-queue des feuilles chevauchant
        // l'ancien OU le nouveau trou.
        {
            int32 NewMinX = MAX_int32, NewMinY = MAX_int32;
            int32 NewMaxX = MIN_int32, NewMaxY = MIN_int32;
            const int32 TopMCLvl = Settings ? FMath::Clamp(Settings->MaxClipLevel, 0, 8) : 4;
            if (Settings && Settings->bFarSheetRing && Settings->RenderDistanceChunks > 0)
            {
                const int32 RClip  = FMath::Max(1, Settings->ClipRadius);
                const int32 Shrink = FMath::Max(0, RClip - 1);
                const int32 ExtM   = CHUNK_SIZE << TopMCLvl;                      // tuile MaxLevel en voxels
                const FIntVector CM = VF_FloorDiv(CenterChunk, 1 << TopMCLvl);    // tuile MaxLevel du joueur
                NewMinX = (CM.X - Shrink) * ExtM;
                NewMinY = (CM.Y - Shrink) * ExtM;
                NewMaxX = (CM.X + Shrink + 1) * ExtM;   // EXCLUSIF
                NewMaxY = (CM.Y + Shrink + 1) * ExtM;
            }
            if (NewMinX != SheetHoleMinXVox || NewMinY != SheetHoleMinYVox
                || NewMaxX != SheetHoleMaxXVox || NewMaxY != SheetHoleMaxYVox)
            {
                for (const FVoxelTileKey& T : LoadedTiles)
                {
                    if (T.Level <= TopMCLvl) continue;   // seules les feuilles portent le trou
                    const int32 Ext   = CHUNK_SIZE << T.Level;
                    const int32 TMinX = T.Coord.X * Ext, TMinY = T.Coord.Y * Ext;
                    const bool bOldOv = TMinX < SheetHoleMaxXVox && TMinX + Ext > SheetHoleMinXVox
                                     && TMinY < SheetHoleMaxYVox && TMinY + Ext > SheetHoleMinYVox;
                    const bool bNewOv = TMinX < NewMaxX && TMinX + Ext > NewMinX
                                     && TMinY < NewMaxY && TMinY + Ext > NewMinY;
                    if ((bOldOv || bNewOv) && IsDesired(T)) BandRemeshQueue.Add(T);
                }
                SheetHoleMinXVox = NewMinX; SheetHoleMinYVox = NewMinY;
                SheetHoleMaxXVox = NewMaxX; SheetHoleMaxYVox = NewMaxY;
                bAllChunksLoaded = false;
            }
        }

        // Cull rules — STRICT LOAD-BEFORE-UNLOAD so crossing a shell boundary NEVER leaves a hole:
        //  - out of clip range → left the view entirely, no replacement coming → cull now.
        //  - in range (mid-LOD-transition) → cull ONLY once EVERY desired tile that overlaps its
        //    footprint is loaded (a coarse tile is replaced by several finer tiles — the whole
        //    covering set must be in before it drops). Otherwise it goes to TransitionHold.
        {
            TRACE_CPUPROFILER_EVENT_SCOPE(VoxelForge_CullTiles);

            auto FootprintsOverlap = [](const FVoxelTileKey& A, const FVoxelTileKey& B) -> bool
            {
                const int32 ea = A.ExtentVoxels(), eb = B.ExtentVoxels();
                const FIntVector aMin = A.OriginVoxels(), bMin = B.OriginVoxels();
                return aMin.X < bMin.X + eb && bMin.X < aMin.X + ea
                    && aMin.Y < bMin.Y + eb && bMin.Y < aMin.Y + ea
                    && aMin.Z < bMin.Z + eb && bMin.Z < aMin.Z + ea;
            };
            // "Every covering desired tile loaded" ⟺ "no unloaded desired tile overlaps T".
            // Built LAZILY: only needed if an in-range transition candidate actually exists.
            TArray<FVoxelTileKey> DesiredPending;
            bool bPendingBuilt = false;
            auto EnsurePending = [&]()
            {
                if (bPendingBuilt) return;
                bPendingBuilt = true;
                for (const FVoxelTileKey& D : DesiredSorted)
                {
                    if (!LoadedTiles.Contains(D)) DesiredPending.Add(D);
                }
            };
            auto ReplacementsReady = [&](const FVoxelTileKey& T) -> bool
            {
                for (const FVoxelTileKey& D : DesiredPending)
                {
                    if (FootprintsOverlap(T, D)) return false;   // a covering tile isn't ready → keep T
                }
                return true;
            };

            // true = la tuile doit être RETENUE (transition en attente de ses remplaçants) ;
            // false = rien à retenir (cullée, re-désirée, ou jamais chargée).
            auto NeedsHold = [&](const FVoxelTileKey& T) -> bool
            {
                if (IsDesired(T)) { return false; }                                    // re-désirée
                if (!LoadedTiles.Contains(T) && !TileComponents.Contains(T))
                {
                    return false;   // jamais chargée / rien d'appliqué → rien à cull
                }
                if (!IsTileInClipRange(T, CenterChunk))   // left the view → cull now
                {
                    PendingUnload.Add(T);
                    return false;
                }
                // In-range transition. Quand le backlog est gros (sprint), sauter le test de
                // recouvrement et RETENIR est la direction hole-safe ; le settled cull ramassera.
                EnsurePending();
                if (DesiredPending.Num() <= 48 && ReplacementsReady(T))
                {
                    PendingUnload.Add(T);
                    return false;
                }
                return true;
            };

            for (const FVoxelTileKey& T : Leavers)
            {
                if (NeedsHold(T)) { AddToTransitionHold(T); }
            }

            // Hold : re-évaluation à BUDGET tournant. Re-scanner toute la hold par crossing
            // redevient le vieux scan O(loaded) dès que le streaming ne settle jamais (mesuré
            // 2.47 ms/crossing packagé) ; retenir plus longtemps est hole-safe, chaque tuile
            // repasse sous le curseur en quelques crossings.
            int32 HoldBudget = FMath::Min(TransitionHoldQueue.Num(), 256);
            while (HoldBudget > 0 && TransitionHoldQueue.Num() > 0)
            {
                if (TransitionHoldCursor >= TransitionHoldQueue.Num()) { TransitionHoldCursor = 0; }
                const FVoxelTileKey T = TransitionHoldQueue[TransitionHoldCursor];
                if (!TransitionHold.Contains(T))
                {
                    // Clé périmée (déchargée / settled-cullée) — retrait paresseux, ne consomme
                    // pas le budget (la queue rétrécit ⇒ la boucle termine).
                    TransitionHoldQueue.RemoveAtSwap(TransitionHoldCursor);
                    continue;
                }
                --HoldBudget;
                if (!NeedsHold(T))
                {
                    TransitionHold.Remove(T);
                    TransitionHoldQueue.RemoveAtSwap(TransitionHoldCursor);
                }
                else
                {
                    ++TransitionHoldCursor;
                }
            }
            // Teardown différé — ProcessUnloadQueue étale les destructions sur plusieurs frames.
        }
    }

    //=========================================================================
    // Submit pending work (budgeted, nearest-first). Once everything desired is loaded,
    // do the "settled" cull of the deferred LOD-transition tiles, then go idle.
    //=========================================================================
    if (!bAllChunksLoaded)
    {
        TRACE_CPUPROFILER_EVENT_SCOPE(VoxelForge_SubmitTiles);
        int32 Submitted = 0;

        // DIG RESPONSIVENESS — drain player-carve re-meshes FIRST (ahead of streaming + band) at
        // BackgroundHigh, so a dig gets the task budget before any streaming gen. A tile that's still
        // in flight is KEPT queued (retried next frame) so its stale pre-carve result is corrected.
        for (auto It = DirtyRemeshQueue.CreateIterator(); It; ++It)
        {
            if (PendingTiles.Num() >= MaxTasks) break;
            const FVoxelTileKey T = *It;
            if (!LoadedTiles.Contains(T)) { It.RemoveCurrent(); continue; }  // unloaded — drop
            if (PendingTiles.Contains(T)) { continue; }                     // in flight — retry after it lands
            It.RemoveCurrent();
            LoadTile(T, /*bHighPriority*/ true);
            ++Submitted;
        }

        for (const FVoxelTileKey& T : DesiredSorted)
        {
            if (PendingTiles.Num() >= MaxTasks) break;
            if (PendingTiles.Contains(T)) continue;   // in flight
            if (LoadedTiles.Contains(T)) continue;    // already loaded (level is in the key — no LOD remesh)
            LoadTile(T);
            ++Submitted;
        }

        // Bande de strate changée : re-gen budgétée des tuiles grossières concernées (le vieux
        // mesh reste visible jusqu'au résultat — même schéma que RemeshDirtyChunks).
        for (auto It = BandRemeshQueue.CreateIterator(); It; ++It)
        {
            if (PendingTiles.Num() >= MaxTasks) break;
            const FVoxelTileKey T = *It;
            It.RemoveCurrent();
            if (PendingTiles.Contains(T) || !LoadedTiles.Contains(T)) continue;
            LoadTile(T);
            ++Submitted;
        }

        if (Submitted == 0 && PendingTiles.Num() == 0 && BandRemeshQueue.Num() == 0 && DirtyRemeshQueue.Num() == 0)
        {
            // Everything desired is loaded → load-before-unload is satisfied: drop the
            // deferred (in-range, not-desired) transition tiles now. No holes. Full scan —
            // rare (once per settle), and the safety net behind the delta cull above.
            for (const auto& Pair : TileComponents)
                if (!IsDesired(Pair.Key)) PendingUnload.Add(Pair.Key);
            for (const FVoxelTileKey& T : LoadedTiles)
                if (!IsDesired(T) && !TileComponents.Contains(T)) PendingUnload.Add(T);
            // Teardown is drained by ProcessUnloadQueue (budgeted) — single spike-free path.

            bAllChunksLoaded = true;
        }
    }
}

void AVoxelWorld::LoadTile(const FVoxelTileKey& Tile, bool bHighPriority)
{
    if (PendingTiles.Contains(Tile)) return;

    const int32 MaxTasks = GetMaxConcurrentTasks();   // T2.d — core-clamped
    if (PendingTiles.Num() >= MaxTasks)
    {
        return;  // Budget full — wait for a task to finish.
    }
    PendingTiles.Add(Tile);

    const FIntVector OriginVoxels = Tile.OriginVoxels();   // min corner, voxel coords

    // Coarse levels mesh with FEWER cells → much cheaper gen (blockier far field, which is far
    // away + skirts hide the seams). Near levels (< FullResClipLevels) stay full CHUNK_SIZE cells.
    // Extent stays CHUNK_SIZE<<Level (so the shell tiling is unchanged) — only the cell count and
    // step change: Step = Extent / Cells.
    const int32 FullRes = Settings ? FMath::Max(1, Settings->FullResClipLevels) : 2;
    const int32 Extent  = CHUNK_SIZE << Tile.Level;

    // F18 — tuile FEUILLE : BuildDesiredTiles n'émet des clés au-delà de MaxClipLevel que pour
    // l'anneau feuille (render distance) — elles se maillent en deux heightfields (GenerateSheetMesh),
    // pas en marching cubes. Densité d'échantillonnage = celle de l'anneau MC niveau-MaxClipLevel
    // (le nombre de cellules grandit avec la feuille, plafonné à 128/axe — au-delà le pas grossit).
    const int32 TopMC = Settings ? FMath::Clamp(Settings->MaxClipLevel, 0, 8) : 4;
    const bool bSheetTile = Tile.Level > TopMC;

    int32 Cells = (Tile.Level < FullRes)
        ? CHUNK_SIZE
        : (Settings ? FMath::Clamp(Settings->CoarseTileCells, 4, CHUNK_SIZE) : 16);
    if (bSheetTile)
    {
        const int32 StepMC = FMath::Max(1, (CHUNK_SIZE << TopMC) / Cells);
        Cells = FMath::Clamp(Extent / StepMC, 4, 128);
    }
    const int32 Step    = FMath::Max(1, Extent / Cells);
    const uint32 TaskEpoch = GenerationEpoch;

    // CAPTURE-DURING-MESHING: only level-0 full-res tiles map 1:1 onto a density-clipmap level
    // (Step == 1<<Level, Cells == CHUNK_SIZE). When the density volume is active, ask the mesher to
    // emit the captured R8 grid so the volume reuses it instead of re-sampling GetDensityAt. Gated to
    // tiles the volume can actually consume (its shadow window is much smaller than the streaming
    // ring) — the rest shouldn't pay the quantize + 32 KB queue payload for a grid it would refuse.
    const bool bWantCapture = (Tile.Level == 0) && (Cells == CHUNK_SIZE)
        && DensityVolume != nullptr && Settings && Settings->bEnableDensityVolume
        && DensityVolume->IsTileCaptureUseful(Tile.Coord);

    // STRATE CONTENT CUT — coarse tiles mesh only the player-strate band (see the band update in
    // UpdateChunksAroundPosition + UVoxelSettings::StrateContentCutMinLevel). Chunk band → voxels
    // (inclusive). Fine tiles / no band (gap, feature off) mesh full.
    int32 BandVoxLo = INT32_MIN, BandVoxHi = INT32_MAX;
    int32 BandChunkLo = MIN_int32, BandChunkHi = MAX_int32;
    int32 SheetChunkZ = 0;
    if (bSheetTile)
    {
        // F18 — la feuille a besoin de la STRATE de référence (les deux heightfields sont ceux de
        // la strate du joueur) : pas de bande armée (gap inter-strates, ou bounds introuvables)
        // ⇒ rien à mailler, tuile vide (re-queue automatique via BandRemeshQueue en atterrissant).
        if (MeshBandChunkLo == MIN_int32)
        {
            FChunkResult Empty;
            Empty.Tile  = Tile;
            Empty.Epoch = TaskEpoch;
            ProcessQueue.Enqueue(MoveTemp(Empty));
            return;
        }
        BandChunkLo = MeshBandChunkLo;   // pour la résolution matériaux sol/cap dans ApplyMeshToTile
        BandChunkHi = MeshBandChunkHi;
        SheetChunkZ = MeshBandChunkLo + (MeshBandChunkHi - MeshBandChunkLo) / 2;   // chunk au cœur de la strate
    }
    // F18 — trou XY courant (zone couverte par les coquilles MC, découpée des feuilles).
    const int32 HoleMinX = SheetHoleMinXVox, HoleMinY = SheetHoleMinYVox;
    const int32 HoleMaxX = SheetHoleMaxXVox, HoleMaxY = SheetHoleMaxYVox;
    const int32 CutMin = Settings ? Settings->StrateContentCutMinLevel : 9;
    if (!bSheetTile && Tile.Level >= CutMin && MeshBandChunkLo != MIN_int32)
    {
        BandChunkLo = MeshBandChunkLo;
        BandChunkHi = MeshBandChunkHi;
        BandVoxLo   = MeshBandChunkLo * CHUNK_SIZE;
        BandVoxHi   = (MeshBandChunkHi + 1) * CHUNK_SIZE - 1;

        // Résidu ultra-grossier (niveaux ≥7) : la coupe est à la granularité de la CELLULE. Si
        // UNE cellule (Step voxels de haut) est plus haute que la bande entière, toute cellule
        // qui chevauche la bande échantillonne quand même les airs des DEUX strates (mêmes trous
        // et mélanges de matériaux qu'avant la coupe) — la tuile ne peut rendre que des artefacts
        // ⇒ on n'émet RIEN. Résultat vide via ProcessQueue (bookkeeping normal : PendingTiles,
        // LoadedTiles, epoch) ; jamais figé — le changement de bande re-queue via BandRemeshQueue.
        if (Step > (BandChunkHi - BandChunkLo + 1) * CHUNK_SIZE)
        {
            FChunkResult Empty;
            Empty.Tile        = Tile;
            Empty.Epoch       = TaskEpoch;
            Empty.BandChunkLo = BandChunkLo;
            Empty.BandChunkHi = BandChunkHi;
            ProcessQueue.Enqueue(MoveTemp(Empty));
            return;
        }
    }

    ActiveTaskCount.fetch_add(1, std::memory_order_relaxed);

    // BackgroundNormal priority: gen runs on background workers that YIELD to foreground
    // (game/render-thread) tasks. Without this, raising MaxConcurrentTasks past the spare
    // core count saturates the scheduler and starves the frame (the "over 12 = lag" symptom).
    // At background priority the frame keeps its cores; gen just fills in around it.
    // DIG RESPONSIVENESS: player carves launch at BackgroundHigh (bHighPriority) — still a background
    // worker (yields to the frame, keeps the invariant) but jumps AHEAD of all pending streaming gen,
    // so a dig is never queued behind a shell of streaming tasks.
    const UE::Tasks::ETaskPriority TaskPriority = bHighPriority
        ? UE::Tasks::ETaskPriority::BackgroundHigh
        : UE::Tasks::ETaskPriority::BackgroundNormal;
    UE::Tasks::Launch(TEXT("ChunkGen"), [this, Tile, OriginVoxels, Step, Cells, TaskEpoch, bWantCapture,
                                         BandVoxLo, BandVoxHi, BandChunkLo, BandChunkHi,
                                         bSheetTile, SheetChunkZ, HoleMinX, HoleMinY, HoleMaxX, HoleMaxY]()
    {
        // RAII: decrement the counter on every exit path.
        struct FTaskGuard
        {
            std::atomic<int32>& Counter;
            ~FTaskGuard() { Counter.fetch_sub(1, std::memory_order_relaxed); }
        } Guard{ActiveTaskCount};

        if (ShouldAbortWork()) return;

        FChunkResult Result;
        GenerateTileResult(Tile, OriginVoxels, Step, Cells, TaskEpoch, bWantCapture,
                           BandVoxLo, BandVoxHi, BandChunkLo, BandChunkHi,
                           bSheetTile, SheetChunkZ, HoleMinX, HoleMinY, HoleMaxX, HoleMaxY, Result);

        if (!ShouldAbortWork())
        {
            ProcessQueue.Enqueue(MoveTemp(Result));   // move: don't copy the geometry payload
        }
    }, TaskPriority);
}

// Worker-side gen for one tile (shared by the async ChunkGen task and the synchronous carve path).
// READS Generator/Mesher only — safe on a worker or the game thread. Fills Result; no enqueue.
void AVoxelWorld::GenerateTileResult(const FVoxelTileKey& Tile, const FIntVector& OriginVoxels,
                                     int32 Step, int32 Cells, uint32 Epoch, bool bWantCapture,
                                     int32 BandVoxLo, int32 BandVoxHi, int32 BandChunkLo, int32 BandChunkHi,
                                     bool bSheetTile, int32 SheetChunkZ,
                                     int32 HoleMinX, int32 HoleMinY, int32 HoleMaxX, int32 HoleMaxY,
                                     FChunkResult& Result)
{
    Result.Tile  = Tile;
    Result.Epoch = Epoch;
    Result.BandChunkLo = BandChunkLo;   // strate content cut (MIN/MAX = uncut)
    Result.BandChunkHi = BandChunkHi;

    // T1.d — TRIVIAL-TILE REJECT: ~84 % des tuiles générées sortaient vides (tout-roc /
    // tout-air) en payant quand même le pré-échantillonnage complet. Le classifieur prouve
    // (bornes exactes sur le treillis du mesher + gardes conservatives) qu'une tuile est
    // uniforme → on saute GenerateMesh, Result reste bEmpty. Mixed = génération normale.
    // Les tuiles à capture (density volume) génèrent toujours : le volume veut la grille
    // même pour les cellules uniformes, et ces tuiles sont rares (fenêtre d'ombre).
    // (Gate IsoLevel == 0 : les verdicts du classifieur supposent l'iso MC à zéro exactement.)
    bool bTrivialEmpty = false;
    if (!bSheetTile && !bWantCapture && Generator && Mesher && Mesher->IsoLevel == 0.0f)
    {
        TRACE_CPUPROFILER_EVENT_SCOPE(VoxelForge_ClassifyTile);
        INC_DWORD_STAT(STAT_VoxelForgeTilesClassified);
        const EVoxelTileClass Verdict = Generator->ClassifyTile(OriginVoxels, Step, Cells);
        if (Verdict == EVoxelTileClass::AllSolid)
        {
            INC_DWORD_STAT(STAT_VoxelForgeTilesSkippedAllSolid);
        }
        else if (Verdict == EVoxelTileClass::AllAir)
        {
            INC_DWORD_STAT(STAT_VoxelForgeTilesSkippedAllAir);
        }
        bTrivialEmpty = (Verdict != EVoxelTileClass::Mixed);
    }

    // F18 — feuille : deux heightfields sol/cap échantillonnés par colonne (pas de marching
    // cubes, pas de classifieur — la classe de surface est vraie par construction).
    // `TilesMeshed` peut dépasser `TilesClassified` : les tuiles qui ratent cette porte sont
    // maillées sans classification. / `TilesMeshed` may exceed `TilesClassified`: tiles that
    // fail this gate are meshed without classification.
    FVoxelMeshData MeshData;
    if (!bTrivialEmpty)
    {
        TRACE_CPUPROFILER_EVENT_SCOPE(VoxelForge_GenerateMesh);
        MeshData = bSheetTile
            ? Mesher->GenerateSheetMesh(OriginVoxels, Step, Cells, SheetChunkZ,
                                        HoleMinX, HoleMinY, HoleMaxX, HoleMaxY)
            : Mesher->GenerateMesh(OriginVoxels, Step, Cells,
                                   bWantCapture ? &Result.CaptureGrid : nullptr,
                                   BandVoxLo, BandVoxHi);
        INC_DWORD_STAT(STAT_VoxelForgeTilesMeshed);
    }

    // T1.f — build the RMC geometry buffers HERE (worker), not on the game thread. Empty/all-air
    // tiles carry no streams (Result.bEmpty stays true) → no component on apply.
    if (!MeshData.IsEmpty())
    {
        TRACE_CPUPROFILER_EVENT_SCOPE(VoxelForge_BuildStreams);
        Result.Streams = MakeShared<RealtimeMesh::FRealtimeMeshStreamSet>();
        BuildTileStreamSet(*Result.Streams, MeshData);
        Result.bEmpty = false;

        // F17 — the mesher classified every triangle semantically (sky-cap vs ground, per
        // vertex against the column's TerrainZ/CeilSurf) and packed them as two contiguous
        // polygroup runs. Here we only record which sections exist for the apply path.
        // (Replaces the whole-tile normal VOTE, which painted mixed coarse tiles — terrain
        // AND cap in one tile — entirely with the winner's material.)
        const int32 NumTris = MeshData.Triangles.Num() / 3;
        Result.bHasCeilingTris = MeshData.NumCeilingTriangles > 0;
        Result.bHasGroundTris  = NumTris > MeshData.NumCeilingTriangles;
    }
}

// Section-group key shared by every tile component ("the" tile geometry group).
static FRealtimeMeshSectionGroupKey VoxelTileGroupKey()
{
    return FRealtimeMeshSectionGroupKey::Create(FRealtimeMeshLODKey(0), FName("Tile"));
}

//=============================================================================
// TILE COMPONENT POOL (T2.c)
//=============================================================================
// Recycler les composants de tuile au lieu de les détruire/recréer.

URealtimeMeshComponent* AVoxelWorld::AcquireTileComponent()
{
    // Reuse a parked component when one is available — skips NewObject + RegisterComponent
    // (and the full proxy teardown/GC of a destroy) during fast travel & regen bursts.
    while (TileComponentPool.Num() > 0)
    {
        URealtimeMeshComponent* Pooled = TileComponentPool.Pop();
        if (IsValid(Pooled))
        {
            Pooled->SetVisibility(true);
            return Pooled;
        }
    }

    URealtimeMeshComponent* MeshComp = NewObject<URealtimeMeshComponent>(this);
    // Generated once, never moves → Static so RMC's cached static draw path + VSM shadow
    // caching apply (see the root SetMobility note in BeginPlay). Must be set before register.
    // Re-mesh on carve recreates the section-group proxy (RMC's Static path already does this),
    // which is fine for an infrequent action.
    MeshComp->SetMobility(EComponentMobility::Static);
    MeshComp->SetGenerateOverlapEvents(false);   // chunks use raycasts, not overlaps
    MeshComp->SetCanEverAffectNavigation(false);
    MeshComp->RegisterComponent();
    MeshComp->AttachToComponent(GetRootComponent(), FAttachmentTransformRules::KeepRelativeTransform);
    return MeshComp;
}

void AVoxelWorld::ReleaseTileComponent(URealtimeMeshComponent* Comp)
{
    if (!IsValid(Comp)) { return; }

    if (TileComponentPool.Num() >= MaxPooledTileComponents)
    {
        Comp->DestroyComponent();
        return;
    }

    // Strip the tile's geometry NOW, not at reuse: removing the section group drops its
    // sections and their cooked collision, so a parked (hidden) component can't be collided
    // with and its render memory is released while it waits. The mesh OBJECT is kept — reuse
    // goes through the same RemoveSectionGroup/CreateSectionGroup path as a re-mesh.
    if (URealtimeMeshSimple* RTMesh = Comp->GetRealtimeMeshAs<URealtimeMeshSimple>())
    {
        RTMesh->RemoveSectionGroup(VoxelTileGroupKey());
    }
    Comp->SetVisibility(false);
    TileComponentPool.Add(Comp);
}

void AVoxelWorld::UnloadTile(const FVoxelTileKey& Tile)
{
    // Water + decorations are no longer tile-bound (water is one player-following ocean plane via
    // UpdateWater; decorations stream by distance via UpdateDecorations) — nothing to clear per tile.
    if (URealtimeMeshComponent** Comp = TileComponents.Find(Tile))
    {
        if (*Comp) { ReleaseTileComponent(*Comp); }   // T2.c — park, don't destroy
        TileComponents.Remove(Tile);
    }
    LoadedTiles.Remove(Tile);
    PendingTiles.Remove(Tile);
    TransitionHold.Remove(Tile);   // couvre aussi le settled cull (qui ne tient pas la hold à jour)
}

void AVoxelWorld::ApplyMeshToTile(FChunkResult& Result)
{
    TRACE_CPUPROFILER_EVENT_SCOPE(VoxelForge_ApplyMeshToChunk);

    // Streams are pre-built on the worker (T1.f) and guaranteed non-empty by the caller
    // (ProcessPendingChunks skips empty tiles). This path is game-thread-CHEAP: material lookup +
    // component get/create + the upload + per-section config. No per-vertex work here.
    const FVoxelTileKey& Tile = Result.Tile;
    RealtimeMesh::FRealtimeMeshStreamSet& Streams = *Result.Streams;
    const bool bHasGroundTris  = Result.bHasGroundTris;
    const bool bHasCeilingTris = Result.bHasCeilingTris;
    const bool bLevel0 = (Tile.Level == 0);

    // F17 — materials per POLYGROUP, not per tile. The mesher classified each triangle
    // semantically (sky-cap = down-facing near the column's CeilSurf; terrain overhangs and
    // future cave roofs stay ground) and packed two contiguous runs → RMC creates one section
    // per non-empty group. Ground (group 0): strate override else global default. Sky-cap
    // (group 1): the strate's CeilingMaterial when set (the rocky "night sky" overhead reads
    // flat/bright otherwise, since it casts no shadow) else same as ground. A coarse tile
    // spanning BOTH surfaces now renders both correctly (the old whole-tile vote painted the
    // loser with the winner's material — Jahni's "terrain and ceiling become one" artifact).
    UMaterialInterface* GroundMaterial = Settings ? Settings->VoxelMaterial : nullptr;
    UMaterialInterface* CeilingMaterial = nullptr;
    if (StrateManager)
    {
        // F17 — a coarse tile is 2^level CHUNKS TALL: its raw min/max corners can sit in a
        // NEIGHBOUR strate (or the inter-strate gap → null) that the mesh doesn't even contain
        // (strate content cut). Clamp the lookup Zs into the band the tile was MESHED with, then
        // resolve: ground at the (clamped) BOTTOM chunk, sky-cap at the (clamped) TOP chunk —
        // the cap is by definition the topmost surface in the tile (mid as a gap fallback).
        // This was the "far cap renders with the ground material" residue: the min-corner lookup
        // missed the surface strate entirely on tall far tiles.
        const FIntVector MinChunk = Tile.Coord * (1 << Tile.Level);     // level-0-equivalent min corner
        const int32 TileChunks = 1 << Tile.Level;
        const int32 ZLoC = FMath::Max(MinChunk.Z, Result.BandChunkLo);
        const int32 ZHiC = FMath::Min(MinChunk.Z + TileChunks - 1, Result.BandChunkHi);
        if (UVoxelStrateDefinition* StrateDef =
                StrateManager->GetStrateForChunk(FIntVector(MinChunk.X, MinChunk.Y, ZLoC)))
        {
            if (StrateDef->OverrideMaterial) { GroundMaterial = StrateDef->OverrideMaterial; }
            if (StrateDef->CeilingMaterial)  { CeilingMaterial = StrateDef->CeilingMaterial; }
        }
        UVoxelStrateDefinition* CapDef =
            StrateManager->GetStrateForChunk(FIntVector(MinChunk.X, MinChunk.Y, ZHiC));
        if (!CapDef || !CapDef->CeilingMaterial)
        {
            CapDef = StrateManager->GetStrateForChunk(FIntVector(MinChunk.X, MinChunk.Y, (ZLoC + ZHiC) / 2));
        }
        if (CapDef && CapDef->CeilingMaterial) { CeilingMaterial = CapDef->CeilingMaterial; }
    }
    if (!CeilingMaterial) { CeilingMaterial = GroundMaterial; }

    // Mini-sun shadows: route the resolved base materials through a shared MID that binds the
    // density-volume textures + per-frame shadow params (the material marches them for raymarched
    // orb shadows). One MID per base material, so tiles of a base still share one material.
    if (DensityVolume && Settings && Settings->bEnableDensityVolume)
    {
        if (UMaterialInstanceDynamic* MID = GetOrCreateTerrainMID(GroundMaterial))  { GroundMaterial = MID; }
        if (UMaterialInstanceDynamic* MID = GetOrCreateTerrainMID(CeilingMaterial)) { CeilingMaterial = MID; }
    }

    // The geometry stream set was built on the worker (BuildTileStreamSet, T1.f); we just upload it.
    // Vertices are world-space; the component sits at the actor origin.

    // One component per tile — the clipmap keeps the total tile count low (~1-2k), so this is
    // cheap on the game thread (no batching needed). Collision + content are level-0 only.
    // T2.c: the component comes from the pool when one is parked (see AcquireTileComponent).
    URealtimeMeshComponent* MeshComp = TileComponents.FindRef(Tile);
    if (!MeshComp)
    {
        MeshComp = AcquireTileComponent();
        TileComponents.Add(Tile, MeshComp);
    }

    // §9.4 RENDER-SKIP — a tile only a CollisionOnly anchor wants (not the player clipmap) cooks its
    // collision below but is hidden (no draw / VSM). Set every apply (overrides the pool's default-
    // visible state); ReconcileAnchorTileVisibility handles later flips on already-loaded tiles.
    MeshComp->SetVisibility(!CollisionOnlyTiles.Contains(Tile));

    // Reuse the component's existing mesh object when it has one (pooled component or carve
    // re-mesh) — InitializeRealtimeMesh allocates a brand-new URealtimeMesh EVERY call, so
    // calling it unconditionally (as before) orphaned one mesh object per re-apply to the GC.
    // The RemoveSectionGroup below does the actual geometry clearing on reuse.
    URealtimeMeshSimple* RTMesh = MeshComp->GetRealtimeMeshAs<URealtimeMeshSimple>();
    if (!RTMesh) { RTMesh = MeshComp->InitializeRealtimeMesh<URealtimeMeshSimple>(); }
    if (!RTMesh) { return; }
    // Shadow casting: far (level >= 2) tiles never cast; the sky-cap SECTION never casts either
    // — otherwise the high rock ceiling shadows the entire terrain below it. F17: shadow is now
    // PER SECTION, so a mixed tile keeps its ground shadow while its cap stays shadowless.
    const bool bCastShadow = (Tile.Level <= 1);
    MeshComp->SetCastShadow(bCastShadow);

    const FRealtimeMeshSectionGroupKey GroupKey = VoxelTileGroupKey();
    RTMesh->RemoveSectionGroup(GroupKey);                    // clear old geometry on re-mesh
                                                             // (no-op on a fresh/pooled mesh)
    RTMesh->SetupMaterialSlot(0, "Main",   GroundMaterial);
    RTMesh->SetupMaterialSlot(1, "SkyCap", CeilingMaterial);
    RTMesh->CreateSectionGroup(GroupKey, MoveTemp(Streams));

    // RMC casts shadows PER SECTION (FRealtimeMeshSectionConfig::bCastsShadow, default true) — the
    // component-level UPrimitiveComponent::CastShadow is NOT honored by the RMC proxy, so the real
    // shadow lever is the section flag. RMC auto-created one section per non-empty polygroup above
    // (default config already maps material slot = polygroup index); only config sections that
    // exist — the bHas* flags come from the worker. Collision at level 0 only (T1.c), both groups.
    if (bHasGroundTris)
    {
        FRealtimeMeshSectionConfig GroundConfig(0);
        GroundConfig.bCastsShadow = bCastShadow;
        RTMesh->UpdateSectionConfig(
            FRealtimeMeshSectionKey::CreateForPolyGroup(GroupKey, 0),
            GroundConfig, /*bShouldCreateCollision*/ bLevel0);
    }
    if (bHasCeilingTris)
    {
        FRealtimeMeshSectionConfig CapConfig(1);
        CapConfig.bCastsShadow = false;                      // the cap never casts (see above)
        RTMesh->UpdateSectionConfig(
            FRealtimeMeshSectionKey::CreateForPolyGroup(GroupKey, 1),
            CapConfig, /*bShouldCreateCollision*/ bLevel0);
    }

    // Water is no longer spawned per tile — it's a single player-following ocean plane (UpdateWater,
    // driven from Tick), so it renders at every LOD and to the horizon with no per-tile gaps.
}

//=============================================================================
// STRATE QUERIES
//=============================================================================

int32 AVoxelWorld::GetStrateAtPosition(FVector WorldPosition) const
{
    if (!StrateManager) return -1;

    // GetStrateIndex veut des cm ACTEUR-LOCAUX (il convertit cm -> voxels lui-meme).
    // GetStrateIndex wants actor-LOCAL cm (it does the cm -> voxel conversion itself).
    return StrateManager->GetStrateIndex(WorldToLocalCm(WorldPosition).Z);
}

FVoxelBiomeQuery AVoxelWorld::GetBiomeAtWorldLocation(FVector WorldLocation) const
{
    FVoxelBiomeQuery Out;
    if (!Generator) return Out;

    // Bring the world point into actor-LOCAL voxel space — the SAME transform the decoration scatter
    // applies (UpdateDecorations), so the probe agrees with where props actually land.
    const FVector Local = GetActorTransform().InverseTransformPosition(WorldLocation);
    const float VX = Local.X / VOXEL_SIZE;
    const float VY = Local.Y / VOXEL_SIZE;
    const int32 ChunkZ = FMath::FloorToInt((Local.Z / VOXEL_SIZE) / (float)CHUNK_SIZE);

    Generator->QueryBiomeAt(VX, VY, ChunkZ, Out);

    // Decoration streaming state of the region under this point — discriminates a render drop / empty
    // march / stuck build / never-requested region for a visibly-bare patch (see FVoxelBiomeQuery).
    if (ContentManager)
    {
        ContentManager->QueryDecoDebugAt(Local, Out.bDecoRegionApplied, Out.DecoAppliedInstances,
                                         Out.bDecoRegionBuilding, Out.DecoCellsAccounted, Out.DecoCellsTotal,
                                         Out.DecoLiveMarchSpawns, Out.DecoInstancesInCell);
    }
    return Out;
}

bool AVoxelWorld::GetVoxelSurfaceHeightAt(FVector WorldLocation, float& OutSurfaceWorldZ, float& OutCeilingWorldZ) const
{
    OutSurfaceWorldZ = WorldLocation.Z;   // sensible fallback: unchanged Z
    OutCeilingWorldZ = WorldLocation.Z;
    if (!Generator) return false;

    // Undo the actor transform → voxel space (same convention as GetBiomeAtWorldLocation / the deco scatter).
    const FVector Local = GetActorTransform().InverseTransformPosition(WorldLocation);
    const float VX = Local.X / VOXEL_SIZE;
    const float VY = Local.Y / VOXEL_SIZE;
    const int32 ChunkZ = FMath::FloorToInt((Local.Z / VOXEL_SIZE) / (float)CHUNK_SIZE);

    float TerrainVZ, CeilVZ;
    if (!Generator->GetSurfaceHeightAt(VX, VY, ChunkZ, TerrainVZ, CeilVZ)) return false;   // not a heightfield

    // Voxel Z → actor-local cm → world, keeping the query's XY so a tilted/scaled actor stays consistent.
    OutSurfaceWorldZ = GetActorTransform().TransformPosition(FVector(Local.X, Local.Y, TerrainVZ * VOXEL_SIZE)).Z;
    OutCeilingWorldZ = GetActorTransform().TransformPosition(FVector(Local.X, Local.Y, CeilVZ    * VOXEL_SIZE)).Z;
    return true;
}

//=============================================================================
// TERRAIN MODIFICATION — player carving & filling
//=============================================================================

// All brush entry points below build an FVoxelModification and funnel through ApplyModification
// (diff layer + re-mesh). Strength sign convention: NEGATIVE = carve (air), POSITIVE = fill (solid).

void AVoxelWorld::CarveAtPosition(FVector Position, float Radius, float Strength)
{
    FVoxelModification Mod;
    Mod.Center = WorldToLocalVoxel(Position);    // world cm → voxel space
    Mod.Radius = Radius;
    Mod.Strength = -FMath::Abs(Strength);  // force negative for carving
    ApplyModification(Mod);
}

void AVoxelWorld::FillAtPosition(FVector Position, float Radius, float Strength)
{
    FVoxelModification Mod;
    Mod.Center = WorldToLocalVoxel(Position);
    Mod.Radius = Radius;
    Mod.Strength = FMath::Abs(Strength);   // force positive for filling
    ApplyModification(Mod);
}

void AVoxelWorld::ApplyModification(const FVoxelModification& Modification)
{
    if (!DiffLayer) return;
    TArray<FIntVector> AffectedChunks = DiffLayer->ApplyModification(Modification);

    // INSTANT DIG FEEL — synchronously re-mesh the level-0 tile the brush CENTRE sits in, so the hole
    // appears THIS frame right where the player is looking. Neighbour tiles (brush edge) re-mesh async
    // and prioritised (RemeshDirtyChunks → DirtyRemeshQueue @ BackgroundHigh), a frame or two behind —
    // imperceptible. One full-res tile gen on the game thread; only for the common case.
    // SKIP the sync when the centre tile is already mid-gen (an async task owns it): syncing would race
    // the in-flight stale result (which lands with no hole and would clobber ours). Instead let it flow
    // through the async queue, which now KEEPS in-flight tiles queued and re-gens them once the stale
    // result lands. The centre tile must be loaded to remesh in place (else it streams in with the diff).
    const FVoxelTileKey CenterTile(WorldToChunkCoord(Modification.Center * VOXEL_SIZE), 0);
    bool bSyncedCenter = false;
    if (AffectedChunks.Contains(CenterTile.Coord)
        && LoadedTiles.Contains(CenterTile)
        && !PendingTiles.Contains(CenterTile))
    {
        SyncRemeshTile(CenterTile);
        bSyncedCenter = true;
    }

    RemeshDirtyChunks(AffectedChunks, bSyncedCenter ? &CenterTile : nullptr);

    // Remove decorations inside the modified volume so grass doesn't float over a dug hole (or bury under a
    // fill). Instant + flicker-free (only the affected instances go); the placer already skips carved columns
    // on any future rebuild. Center/Radius are in voxels → world cm. Box/capsule use their bounding sphere.
    if (ContentManager && AffectedChunks.Num() > 0)
    {
        const FVector WorldCenter = Modification.Center * VOXEL_SIZE;   // Center was WorldPos/VOXEL_SIZE
        ContentManager->RemoveDecorationsInSphere(WorldCenter, Modification.Radius * VOXEL_SIZE);
    }
}

void AVoxelWorld::CarveBox(FVector Position, FVector ExtentVoxels, float Strength)
{
    FVoxelModification Mod;
    Mod.Shape = EVoxelBrushShape::Box;
    Mod.Center = WorldToLocalVoxel(Position);
    Mod.BoxExtent = ExtentVoxels;
    Mod.Radius = ExtentVoxels.GetMax();        // budget proxy
    Mod.Strength = -FMath::Abs(Strength);       // carve
    ApplyModification(Mod);
}

void AVoxelWorld::FillBox(FVector Position, FVector ExtentVoxels, float Strength)
{
    FVoxelModification Mod;
    Mod.Shape = EVoxelBrushShape::Box;
    Mod.Center = WorldToLocalVoxel(Position);
    Mod.BoxExtent = ExtentVoxels;
    Mod.Radius = ExtentVoxels.GetMax();
    Mod.Strength = FMath::Abs(Strength);        // fill
    ApplyModification(Mod);
}

void AVoxelWorld::CarveCapsule(FVector WorldA, FVector WorldB, float RadiusVoxels, float Strength)
{
    FVoxelModification Mod;
    Mod.Shape = EVoxelBrushShape::Capsule;
    Mod.Center = WorldToLocalVoxel(WorldA);
    Mod.CapsuleEnd = WorldToLocalVoxel(WorldB);
    Mod.Radius = RadiusVoxels;
    Mod.Strength = -FMath::Abs(Strength);
    ApplyModification(Mod);
}

void AVoxelWorld::FillCapsule(FVector WorldA, FVector WorldB, float RadiusVoxels, float Strength)
{
    FVoxelModification Mod;
    Mod.Shape = EVoxelBrushShape::Capsule;
    Mod.Center = WorldToLocalVoxel(WorldA);
    Mod.CapsuleEnd = WorldToLocalVoxel(WorldB);
    Mod.Radius = RadiusVoxels;
    Mod.Strength = FMath::Abs(Strength);
    ApplyModification(Mod);
}

void AVoxelWorld::EditorCarveSphere()
{
    if (!DiffLayer)
    {
        UE_LOG(LogTemp, Warning, TEXT("[VoxelWorld] EditorCarveSphere: no DiffLayer (start PIE first)."));
        return;
    }
    CarveAtPosition(EditorBrushCenter, EditorBrushRadius, EditorBrushStrength);
}

void AVoxelWorld::EditorFillSphere()
{
    if (!DiffLayer)
    {
        UE_LOG(LogTemp, Warning, TEXT("[VoxelWorld] EditorFillSphere: no DiffLayer (start PIE first)."));
        return;
    }
    FillAtPosition(EditorBrushCenter, EditorBrushRadius, EditorBrushStrength);
}

//=============================================================================
// BIOME MAP PREVIEW — bake the XY biome field to Saved/BiomePreview.png
//=============================================================================

void AVoxelWorld::BakeBiomePreview()
{
    if (!BiomePreviewStrate)
    {
        UE_LOG(LogTemp, Warning, TEXT("[VoxelWorld] BakeBiomePreview: assign BiomePreviewStrate first."));
        return;
    }
    if (BiomePreviewChannel == EBiomePreviewChannel::Biome && BiomePreviewStrate->Biomes.Num() == 0)
    {
        UE_LOG(LogTemp, Warning, TEXT("[VoxelWorld] BakeBiomePreview: '%s' has no Biomes — bake Relief/Moisture instead, or add biomes."),
            *BiomePreviewStrate->GetName());
        return;
    }

    // Transient generator so this works in the editor without PIE.
    UVoxelGenerator* Gen = NewObject<UVoxelGenerator>(this);
    Gen->Seed = Settings ? Settings->Seed : 0;

    // Flatten the strate's biomes into a context (mirrors StrateManager::GetBiomeContextForChunk).
    FBiomeContext Ctx;
    Ctx.Map = BiomePreviewStrate->BiomeMapParams;
    for (int32 i = 0; i < BiomePreviewStrate->Biomes.Num(); ++i)
    {
        const UVoxelBiomeDefinition* B = BiomePreviewStrate->Biomes[i];
        if (!B) continue;
        FBiomeResolved R;
        R.Index       = i;
        R.ReliefMin   = B->ReliefMin;   R.ReliefMax   = B->ReliefMax;
        R.MoistureMin = B->MoistureMin; R.MoistureMax = B->MoistureMax;
        R.DebugColor  = B->DebugColor.ToFColor(true);
        Ctx.Biomes.Add(R);
    }

    const int32 Res    = FMath::Clamp(BiomePreviewResolution, 64, 2048);
    const float Size   = FMath::Max(BiomePreviewWorldSize, 1.0f);
    const float Step   = Size / (float)Res;
    const float OriginX = BiomePreviewCenter.X - Size * 0.5f;
    const float OriginY = BiomePreviewCenter.Y - Size * 0.5f;

    TArray<FColor> Pixels;
    Pixels.SetNumUninitialized(Res * Res);

    for (int32 py = 0; py < Res; ++py)
    for (int32 px = 0; px < Res; ++px)
    {
        const float wx = OriginX + (px + 0.5f) * Step;
        const float wy = OriginY + (py + 0.5f) * Step;

        FColor C = FColor::Black;
        switch (BiomePreviewChannel)
        {
        case EBiomePreviewChannel::Relief:
        {
            const float r = Gen->SampleRelief(wx, wy, Ctx.Map.ReliefFrequency, Ctx.Map.ReliefContrast);
            const uint8 v = (uint8)FMath::Clamp(r * 255.0f, 0.0f, 255.0f);
            C = FColor(v, v, v);
            break;
        }
        case EBiomePreviewChannel::Moisture:
        {
            const float m = Gen->SampleMoisture(wx, wy, Ctx.Map.MoistureFrequency);
            const uint8 v = (uint8)FMath::Clamp(m * 255.0f, 0.0f, 255.0f);
            C = FColor(0, v, (uint8)(255 - v));   // dry=blue → wet=green
            break;
        }
        default: // Biome
        {
            const FBiomeSample S = Gen->SampleBiomeAt(wx, wy, Ctx);
            if (Ctx.Biomes.IsValidIndex(S.DominantIndex))
            {
                C = Ctx.Biomes[S.DominantIndex].DebugColor;
                if (S.NeighborWeight > 0.0f && Ctx.Biomes.IsValidIndex(S.NeighborIndex))
                {
                    const FLinearColor A(C);
                    const FLinearColor Bn(Ctx.Biomes[S.NeighborIndex].DebugColor);
                    C = FLinearColor::LerpUsingHSV(A, Bn, S.NeighborWeight).ToFColor(true);
                }
            }
            break;
        }
        }
        C.A = 255;
        Pixels[py * Res + px] = C;
    }

    // Encode PNG and write to Saved/.
    IImageWrapperModule& Module = FModuleManager::LoadModuleChecked<IImageWrapperModule>(FName("ImageWrapper"));
    const TSharedPtr<IImageWrapper> Wrapper = Module.CreateImageWrapper(EImageFormat::PNG);
    if (!Wrapper.IsValid() ||
        !Wrapper->SetRaw(Pixels.GetData(), (int64)Pixels.Num() * sizeof(FColor), Res, Res, ERGBFormat::BGRA, 8))
    {
        UE_LOG(LogTemp, Error, TEXT("[VoxelWorld] BakeBiomePreview: failed to encode image."));
        return;
    }

    const TArray64<uint8>& Png = Wrapper->GetCompressed(100);
    const FString Path = FPaths::ProjectSavedDir() / TEXT("BiomePreview.png");
    if (FFileHelper::SaveArrayToFile(Png, *Path))
    {
        UE_LOG(LogTemp, Display, TEXT("[VoxelWorld] Biome preview (%dx%d, %s) saved to %s"),
            Res, Res, *UEnum::GetValueAsString(BiomePreviewChannel), *FPaths::ConvertRelativePathToFull(Path));
    }
    else
    {
        UE_LOG(LogTemp, Error, TEXT("[VoxelWorld] BakeBiomePreview: failed to write %s"), *Path);
    }
}

void AVoxelWorld::ClearAllModifications()
{
    if (!DiffLayer) return;

    DiffLayer->Clear();

    // Regenerate all loaded chunks to restore procedural terrain
    RegenerateAllChunks();
}

//=============================================================================
// SEED / SEASON MANAGEMENT
//=============================================================================

void AVoxelWorld::ChangeSeed(int32 NewSeed)
{
    if (!Settings)
    {
        UE_LOG(LogTemp, Error, TEXT("[VoxelWorld] ChangeSeed failed — no Settings assigned"));
        return;
    }

    const int32 OldSeed = Settings->Seed;
    const int32 OldSeason = Settings->CurrentSeason;

    {
        FScopedGenerationPause Guard(this);
        if (!Guard.Acquired())
        {
            UE_LOG(LogTemp, Error, TEXT("[VoxelWorld] ChangeSeed: generation pause timed out; no mutation applied."));
            return;
        }

        // 1. Update seed in Settings (the authoritative source)
        Settings->Seed = NewSeed;

        // 2. Increment season counter
        Settings->CurrentSeason++;

        // 3. Push new seed to Generator
        if (Generator)
        {
            Generator->InitializeSettings(Settings);
        }

        // 4. Rebuild strate layout with the new seed.
        //    Strate assignments and passages all change.
        if (StrateManager)
        {
            StrateManager->Initialize(Settings, NewSeed);
        }

        // 5. Clear all player modifications — carvings from the old world are meaningless
        if (DiffLayer)
        {
            DiffLayer->Clear();
        }

        // 5b. Update content placement seed so the new world scatters differently.
        if (ContentManager)
        {
            ContentManager->SetSeed(NewSeed);
            ContentManager->ClearAll();
        }

        // 5c. Reset atmosphere — strate layout changed, re-apply on next Tick.
        if (AtmosphereManager)
        {
            AtmosphereManager->Reset();
        }
    }

    // 6. Unload all existing chunks and let Tick reload them with new generation
    RegenerateAllChunks();

    UE_LOG(LogTemp, Log,
        TEXT("[VoxelWorld] Seed changed: %d -> %d (Season %d -> %d). All chunks regenerated, mods cleared."),
        OldSeed, NewSeed, OldSeason, Settings->CurrentSeason);
}

int32 AVoxelWorld::GetCurrentSeed() const
{
    return Settings ? Settings->Seed : 0;
}

int32 AVoxelWorld::GetCurrentSeason() const
{
    return Settings ? Settings->CurrentSeason : 0;
}

//=============================================================================
// REMESH DIRTY CHUNKS — re-queue affected chunks after terrain modification
//=============================================================================

void AVoxelWorld::RemeshDirtyChunks(const TArray<FIntVector>& DirtyCoords, const FVoxelTileKey* ExcludeTile)
{
    // Edits only affect LEVEL-0 tiles (collision + visible detail are full-res near the player;
    // coarse far tiles sample too sparsely to show small carves, and pick up the diff naturally
    // when they next stream). Queue each loaded level-0 dirty tile onto DirtyRemeshQueue — drained
    // FIRST in the submit loop and launched at BackgroundHigh (ahead of streaming), so a dig never
    // waits behind a shell of streaming tasks. LoadTile re-runs gen (density includes the DiffLayer
    // via GetDensityAt) and ProcessPendingChunks updates the existing component in place (old mesh
    // stays visible until then, no pop).
    //
    // Tiles that are currently mid-gen are STILL queued (not skipped): their in-flight result was
    // sampled BEFORE this carve, so it lands with no hole — keeping the tile queued re-gens it once
    // that stale result drains. (The old inline path dropped both over-budget and in-flight tiles,
    // which is why a dig could show up a beat late or not until the player moved.)
    for (const FIntVector& Coord : DirtyCoords)
    {
        const FVoxelTileKey Tile(Coord, 0);
        if (ExcludeTile && Tile == *ExcludeTile) continue;  // handled synchronously this frame
        if (!LoadedTiles.Contains(Tile)) continue;          // only re-mesh loaded full-res tiles
        DirtyRemeshQueue.Add(Tile);
    }
    if (DirtyRemeshQueue.Num() > 0)
    {
        // Wake the submit loop even if the streaming set had settled (idle player digging).
        bAllChunksLoaded = false;
    }

    // Density volume: refill the clipmap cells overlapping each carved chunk so the shadow march
    // sees the edit (GetDensityAt includes the diff layer). Cheap + local; covers all carve shapes.
    if (DensityVolume)
    {
        for (const FIntVector& Coord : DirtyCoords)
        {
            const FIntVector MinV = Coord * CHUNK_SIZE;
            const FIntVector MaxV = MinV + FIntVector(CHUNK_SIZE, CHUNK_SIZE, CHUNK_SIZE);
            DensityVolume->MarkDirtyVoxelBox(MinV, MaxV);
        }
    }

    UE_LOG(LogTemp, Verbose, TEXT("[VoxelWorld] RemeshDirtyChunks: %d coords, %d pending"),
        DirtyCoords.Num(), PendingTiles.Num());
}

UVolumeTexture* AVoxelWorld::GetDensityVolumeTexture(int32 Level) const
{
    return DensityVolume ? DensityVolume->GetLevelTexture(Level) : nullptr;
}

//=============================================================================
// TERRAIN MATERIAL — density-volume / orb shadow params (MID-driven, see ApplyMeshToTile)
//=============================================================================

UMaterialInstanceDynamic* AVoxelWorld::GetOrCreateTerrainMID(UMaterialInterface* Base)
{
    if (!Base) return nullptr;
    if (TObjectPtr<UMaterialInstanceDynamic>* Found = TerrainMIDs.Find(Base))
    {
        return Found->Get();
    }
    UMaterialInstanceDynamic* MID = UMaterialInstanceDynamic::Create(Base, this);
    if (MID)
    {
        TerrainMIDs.Add(Base, MID);
        SetVolumeParamsOnMID(MID);   // seed with the current frame's params
    }
    return MID;
}

void AVoxelWorld::SetVolumeParamsOnMID(UMaterialInstanceDynamic* MID) const
{
    if (!MID) return;
    // Static FNames — this runs per MID on every param change; no per-call FName construction.
    static const FName VolPNames[10] = {
        FName("VolP0"), FName("VolP1"), FName("VolP2"), FName("VolP3"), FName("VolP4"),
        FName("VolP5"), FName("VolP6"), FName("VolP7"), FName("VolP8"), FName("VolP9") };
    static const FName VolTexNames[3] = { FName("VolTex0"), FName("VolTex1"), FName("VolTex2") };

    const FLinearColor* TVPs[10] = { &TVP0, &TVP1, &TVP2, &TVP3, &TVP4, &TVP5, &TVP6, &TVP7, &TVP8, &TVP9 };
    for (int32 i = 0; i < 10; ++i) { MID->SetVectorParameterValue(VolPNames[i], *TVPs[i]); }
    if (DensityVolume)
    {
        for (int32 L = 0; L < 3; ++L)
        {
            if (UVolumeTexture* T = DensityVolume->GetLevelTexture(L)) { MID->SetTextureParameterValue(VolTexNames[L], T); }
        }
    }
}

void AVoxelWorld::UpdateTerrainMaterialParams()
{
    if (!DensityVolume || !Settings || !Settings->bEnableDensityVolume) return;

    // --- Per-level clipmap transforms (L0 = finest/near; L1-2 = coarser for shadow REACH) ---
    // For each level the material maps WorldPos → RelPos = WorldPos - WindowOrigin → cellF = RelPos/Cell →
    // toroidal UVW = frac((OriginMod + cellF + 0.5)/Res). OriginMod = OriginCells mod Res precomputed here
    // so the shader never touches the large absolute cell coord (no float precision loss). The shader
    // derives each level's cell size from L0's (cell_L = L0Cell * 2^L); Res is shared across levels.
    const FTransform Xf = GetActorTransform();
    int32 Res = 0;
    bool bHave = false;
    float CellWorldSize = VOXEL_SIZE;   // L0 cm per cell

    FLinearColor OriginC[3] = { FLinearColor::Black, FLinearColor::Black, FLinearColor::Black };
    FLinearColor ModC[3]    = { FLinearColor::Black, FLinearColor::Black, FLinearColor::Black };
    for (int32 L = 0; L < 3; ++L)
    {
        FIntVector OriginCells(0, 0, 0);
        float StepF = 1.0f;
        int32 LRes = 0;
        if (DensityVolume->GetLevelShaderParams(L, OriginCells, StepF, LRes) && LRes > 0)
        {
            const FVector OriginLocalCm = FVector(OriginCells.X, OriginCells.Y, OriginCells.Z) * (StepF * VOXEL_SIZE);
            const FVector OW = Xf.TransformPosition(OriginLocalCm);
            auto Mod = [LRes](int32 v) { const int32 m = v % LRes; return (float)((m < 0) ? m + LRes : m); };
            OriginC[L] = FLinearColor(OW.X, OW.Y, OW.Z, 0.0f);
            ModC[L]    = FLinearColor(Mod(OriginCells.X), Mod(OriginCells.Y), Mod(OriginCells.Z), 0.0f);
            if (L == 0) { bHave = true; Res = LRes; CellWorldSize = VOXEL_SIZE * StepF; }
        }
    }
    // Track whether anything actually changed — the push below enqueues render-thread updates per MID,
    // so on the (common) idle frames where the window didn't scroll and the orb didn't change, skip it.
    bool bDirty = false;
    auto SetTVP = [&bDirty](FLinearColor& Dst, const FLinearColor& V)
    {
        if (Dst != V) { Dst = V; bDirty = true; }
    };
    SetTVP(TVP0, OriginC[0]);  SetTVP(TVP1, ModC[0]);
    SetTVP(TVP6, OriginC[1]);  SetTVP(TVP7, ModC[1]);
    SetTVP(TVP8, OriginC[2]);  SetTVP(TVP9, ModC[2]);

    // --- Nearest active orb ---
    FVoxelActiveOrb Best;
    bool bHaveOrb = false;
    if (ContentManager)
    {
        TArray<FVoxelActiveOrb> Orbs;
        ContentManager->GetActiveOrbs(Orbs);
        if (Orbs.Num() > 0)
        {
            const FVector P = GetPlayerPosition();
            float BestD = FLT_MAX;
            for (const FVoxelActiveOrb& O : Orbs)
            {
                const float D = FVector::DistSquared(O.WorldPos, P);
                if (D < BestD) { BestD = D; Best = O; bHaveOrb = true; }
            }
        }
    }

    // All data lives in .xyz (a Vector Parameter only delivers float3 into a Custom node). Intensity is
    // premultiplied into the colour; Res / CellWorldSize / Enable go in TVP5.
    const float Enable = (bHave && bHaveOrb && Res > 0) ? 1.0f : 0.0f;
    const float Steps = (float)FMath::Clamp(Settings->DensityVolumeMarchSteps, 4, 256);
    SetTVP(TVP2, FLinearColor(Best.WorldPos.X, Best.WorldPos.Y, Best.WorldPos.Z, 0.0f));
    SetTVP(TVP3, FLinearColor(Best.Color.R * Best.Intensity, Best.Color.G * Best.Intensity, Best.Color.B * Best.Intensity, 0.0f));
    SetTVP(TVP4, FLinearColor(Best.MaxShadowDistWorld, Best.FalloffWorld, Steps, 0.0f));
    SetTVP(TVP5, FLinearColor((float)FMath::Max(Res, 0), CellWorldSize, Enable, 0.0f));

    // Re-push if the L0 texture object itself was recreated (resolution change) even when the packed
    // params happen to be identical — otherwise the MIDs would keep sampling the dropped texture.
    UVolumeTexture* Tex0 = DensityVolume->GetLevelTexture(0);
    if (LastBoundVolTex0.Get() != Tex0) { LastBoundVolTex0 = Tex0; bDirty = true; }
    if (!bDirty) return;

    // Push to every terrain MID (new MIDs are seeded on creation in GetOrCreateTerrainMID).
    for (TPair<TObjectPtr<UMaterialInterface>, TObjectPtr<UMaterialInstanceDynamic>>& Pair : TerrainMIDs)
    {
        SetVolumeParamsOnMID(Pair.Value.Get());
    }
}

void AVoxelWorld::UpdateOrbLightMPC()
{
    if (!OrbLightMPC || !ContentManager) return;

    TArray<FVoxelActiveOrb> Orbs;
    ContentManager->GetActiveOrbs(Orbs);

    // Nearest-first so Orb0..3 are the 4 closest orbs (the Light Function unions their pools; 4 is
    // plenty since only nearby pools are visible and the player sits inside one or two at a time).
    const FVector P = GetPlayerPosition();
    Orbs.Sort([&P](const FVoxelActiveOrb& A, const FVoxelActiveOrb& B)
    {
        return FVector::DistSquared(A.WorldPos, P) < FVector::DistSquared(B.WorldPos, P);
    });

    static const FName OrbNames[4] = { FName("Orb0"), FName("Orb1"), FName("Orb2"), FName("Orb3") };
    for (int32 i = 0; i < 4; ++i)
    {
        // (x,y,z) = orb WORLD position, .w = reach radius in cm (FalloffWorld = how far the pool
        // extends). Unused slots = all-zero → radius 0 → the mask yields no pool for them.
        FLinearColor V(0.f, 0.f, 0.f, 0.f);
        if (i < Orbs.Num())
        {
            const FVoxelActiveOrb& O = Orbs[i];
            V = FLinearColor((float)O.WorldPos.X, (float)O.WorldPos.Y, (float)O.WorldPos.Z, O.FalloffWorld);
        }
        // ALWAYS write, even when unchanged. A skip-if-identical cache was tried here and BROKE the
        // lighting: the MPC's world INSTANCE can be reset/recreated behind our back (PIE init order,
        // asset recompile), and a cached skip then leaves it holding defaults forever. The per-frame
        // rewrite is what makes the collection self-healing — and 4 vector writes cost nothing.
        UKismetMaterialLibrary::SetVectorParameterValue(this, OrbLightMPC, OrbNames[i], V);
    }
}
