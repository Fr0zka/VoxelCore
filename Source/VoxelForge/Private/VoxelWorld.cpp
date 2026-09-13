// VoxelWorld.cpp
// Implementation of the voxel world manager

#include "VoxelWorld.h"
#include "VoxelDiffLayer.h"
#include "RealtimeMeshComponent.h"
#include "RealtimeMeshSimple.h"
#include "Core/RealtimeMeshCollision.h"
#include "VoxelMarchingCubesMesher.h"
#include "VoxelStrateDefinition.h"
#include "VoxelBiomeDefinition.h"
#include "VoxelTerrainOpDefinition.h"
#include "VoxelContentManager.h"
#include "VoxelDensityVolume.h"
#include "VoxelDensityOpStack.h"
#include "VoxelDensityProfile.h"
#include "VoxelStartupTrace.h"
#include "VoxelStackSampler.h"
#include "VoxelStats.h"
#include "VoxelCaveMorphology.h"
// IWYU (FPSemantics = Precise ⇒ plus de PCH partagé) : GetPlayerPosition déréférence le pawn, donc
// APawn doit être COMPLET — `Casts.h` n'en donne qu'une déclaration avant. APlayerController était
// complet par transitivité seulement : on l'inclut explicitement, c'est exactement la fragilité
// qu'on est en train de retirer.
// GetPlayerPosition dereferences the pawn, so APawn must be COMPLETE — Casts.h only forward-declares
// it. APlayerController was complete transitively only; include it explicitly.
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PawnMovementComponent.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Components/CapsuleComponent.h"
#include "Async/Async.h"
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
#include "Misc/CommandLine.h"
#include "Misc/Paths.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformMisc.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"   // Unreal Insights scopes (Perf 0)

#if WITH_EDITOR
#include "VoxelStrateComposer.h"
#include "VoxelStrateMeasure.h"
#endif

namespace
{
    int32 GVoxelForgeProfileTileGeneration = 0;
    FAutoConsoleVariableRef CVarVoxelForgeProfileTileGeneration(
        TEXT("voxel.ProfileTileGeneration"),
        GVoxelForgeProfileTileGeneration,
        TEXT("Log worker tile generation time with level/step/cell count."));

    int32 GVoxelForgeProfileDensity = 0;
    FAutoConsoleVariableRef CVarVoxelForgeProfileDensity(
        TEXT("voxel.ProfileDensity"),
        GVoxelForgeProfileDensity,
        TEXT("Collect sampled per-density profiling for the game path."));
    int32 GVoxelForgeProfileDensityFull = 0;
    FAutoConsoleVariableRef CVarVoxelForgeProfileDensityFull(
        TEXT("voxel.ProfileDensityFull"),
        GVoxelForgeProfileDensityFull,
        TEXT("Collect full per-operator profiling for the game path."));
    int32 GVoxelForgePerfAttribution = 0;
    FAutoConsoleVariableRef CVarVoxelForgePerfAttribution(
        TEXT("voxel.PerfAttribution"),
        GVoxelForgePerfAttribution,
        TEXT("Collect low-overhead component attribution for the game path."));
    int32 GVoxelForgeSampleStacks = 0;
    FAutoConsoleVariableRef CVarVoxelForgeSampleStacks(
        TEXT("voxel.SampleStacks"),
        GVoxelForgeSampleStacks,
        TEXT("Sample active generation worker stacks at the requested interval in microseconds; 0 is off."));

    // The nested 8^3 proof is useful for focused/offline measurements, but it is opt-in for
    // streaming because every qualifying mixed tile would pay the recursive proof again.
    int32 GVoxelForgeUseBlockEarlyOut = 0;
    FAutoConsoleVariableRef CVarVoxelForgeUseBlockEarlyOut(
        TEXT("voxel.UseBlockEarlyOut"),
        GVoxelForgeUseBlockEarlyOut,
        TEXT("Use the optional nested 8x8x8 mesher classifier in streaming."));

    // Outer classifier benchmark modes: 0 = disabled, 1 = current discarded exact validation.
    int32 GVoxelForgeOuterClassifierMode = 0;
    FAutoConsoleVariableRef CVarVoxelForgeOuterClassifierMode(
        TEXT("voxel.OuterClassifierMode"),
        GVoxelForgeOuterClassifierMode,
        TEXT("Outer tile classifier mode: 0 off, 1 discarded exact validation."));

    // Startup command-line cvars can arrive before the game world has spawned AVoxelWorld (or
    // before its first tiles have become visible). Keep the diagnostic edit pending until Tick has
    // a loaded tile; otherwise a startup measurement silently becomes a no-op or edits the wrong
    // position.
    int32 GVoxelForgePendingTestModification = 0;
    float GVoxelForgePendingModificationRadius = 3.0f;
    float GVoxelForgePendingModificationStrength = 10.0f;
    bool GVoxelForgeTestModificationCommandLineConsumed = false;
    bool GVoxelForgeStreamingBudgetReported = false;
    bool GVoxelForgeGenerationCapHitReported = false;
    int32 GVoxelForgeMaxPendingTilesObserved = 0;
}

AVoxelWorld::AVoxelWorld()
{
    PrimaryActorTick.bCanEverTick = true;
    // Run the collision-readiness check before physics. The pawn receives this actor as a tick
    // prerequisite once it is possessed, so a movement step cannot race the gate on the same frame.
    PrimaryActorTick.TickGroup = TG_PrePhysics;
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
    CollisionReadyTiles.Empty();
    CollisionNotRequiredTiles.Empty();
    CollisionSolidTiles.Empty();
    PendingCollisionCooks.Empty();

    // Decorations/water are keyed per level-0 chunk — clear them all.
    if (ContentManager) { ContentManager->ClearAll(); }

    // Density volume: bump epoch (drop in-flight fills) + drop data → full refill next Tick.
    if (DensityVolume) { DensityVolume->Reset(); }

    // Mark any still-running request obsolete. Keep its PendingTiles slot and token map until the
    // worker/result drains; otherwise a late old result could remove the slot belonging to a new
    // request for the same key. Live-edit callers normally arrive with generation paused (and no
    // active tasks), in which case the stale queue can be discarded immediately.
    for (TPair<FVoxelTileKey, TSharedPtr<FVoxelTileCancellationState, ESPMode::ThreadSafe>>& Pair :
         PendingTileCancellation)
    {
        if (Pair.Value) { Pair.Value->bObsolete.store(true, std::memory_order_release); }
    }
    if (ActiveTaskCount.load(std::memory_order_relaxed) == 0)
    {
        FChunkResult StaleResult;
        while (ProcessQueue.Dequeue(StaleResult)) {}
        PendingTiles.Empty();
        PendingTileCancellation.Empty();
    }
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
    CriticalDesiredTiles.Reset();
    DesiredStamped.Reset();
    ++DesiredEpoch;
    TransitionHold.Reset();
    TransitionHoldQueue.Reset();
    TransitionHoldCursor = 0;

    // Tick will reload all tiles on the next frame with fresh params.
    UE_LOG(LogTemp, Log, TEXT("[VoxelWorld] RegenerateAllChunks (epoch %u): cleared %d tiles"), GenerationEpoch, Count);
}

void AVoxelWorld::RebuildStrates()
{
    bool bLayoutRebuilt = true;
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
            bLayoutRebuilt = StrateManager->Initialize(
                Settings, Settings->GetEffectiveWorldSeed());
        }
        if (AtmosphereManager) AtmosphereManager->Reset();
        if (ContentManager)    ContentManager->ClearAll();
    }

    if (!bLayoutRebuilt)
    {
        UE_LOG(LogTemp, Error,
            TEXT("[VoxelWorld] RebuildStrates refused an invalid season; loaded tiles were not regenerated."));
        return;
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

    FVector PlayerPosition = FVector::ZeroVector;
    if (!TryGetPlayerPosition(PlayerPosition))
    {
        UE_LOG(LogTemp, Warning,
            TEXT("[VoxelForge] ValidateDeterminism: no player pawn — run during PIE after possession."));
        return;
    }
    const FIntVector CenterChunk = WorldToChunkCoord(WorldToLocalCm(PlayerPosition));

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
            StrateManager->Initialize(Settings, Settings->GetEffectiveWorldSeed());
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
    const double EndPlayStartSeconds = FPlatformTime::Seconds();
    bShuttingDown.store(true, std::memory_order_release);
    ReleasePawnCollisionGate();
    if (APawn* Pawn = PawnTickPrerequisite.Get())
    {
        Pawn->RemoveTickPrerequisiteActor(this);
    }
    if (UPawnMovementComponent* PawnMovement = PawnMovementTickPrerequisite.Get())
    {
        PawnMovement->RemoveTickPrerequisiteActor(this);
    }
    PawnTickPrerequisite.Reset();
    PawnMovementTickPrerequisite.Reset();
    UE_LOG(LogTemp, Display, TEXT("[VoxelWorld] EndPlay: shutdown signalled; waiting for %d tasks"),
        ActiveTaskCount.load(std::memory_order_relaxed));

    // Wait for every running task before destroying UObjects. A timeout here is unsafe: a task
    // already inside GenerateTileResult may still be evaluating a cached op stack that points at
    // this world's manager. Log a slow drain, but never tear the world down underneath a reader.
    const double Deadline = FPlatformTime::Seconds() + 3.0;
    bool bReportedSlowDrain = false;
    while (ActiveTaskCount.load(std::memory_order_relaxed) > 0)
    {
        if (!bReportedSlowDrain && FPlatformTime::Seconds() > Deadline)
        {
            UE_LOG(LogTemp, Warning, TEXT("[VoxelWorld] EndPlay: waiting for %d tasks after 3s; UObject teardown is deferred"),
                ActiveTaskCount.load(std::memory_order_relaxed));
            bReportedSlowDrain = true;
        }
        FPlatformProcess::Yield();  // Give CPU to other threads
    }
    // Drain any queued results
    FChunkResult Discard;
    while (ProcessQueue.Dequeue(Discard))
    {
        if (Discard.bObsolete) { ++ObsoleteTileAbortCount; }
    }
    UE_LOG(LogTemp, Display, TEXT("[VoxelWorld] EndPlay: workers drained in %.6fs; obsolete_tile_aborts=%d"),
        FPlatformTime::Seconds() - EndPlayStartSeconds, ObsoleteTileAbortCount);
    if (bSampleStacksStarted)
    {
        FVoxelStackSampler::Get().StopAndWrite();
        bSampleStacksStarted = false;
    }
    VoxelCaveMorphology::LogPlayerFitMemoStats();
    LogStreamingLatencySummary();
    PendingTiles.Empty();
    PendingTileCancellation.Empty();
    PendingUnload.Empty();
    DirtyRemeshQueue.Empty();
    BandRemeshQueue.Empty();
    PendingCollisionCooks.Empty();
    CollisionReadyTiles.Empty();
    CollisionNotRequiredTiles.Empty();
    CollisionSolidTiles.Empty();
    for (const TPair<URealtimeMesh*, FDelegateHandle>& Pair : CollisionBodyUpdatedHandles)
    {
        if (Pair.Key != nullptr)
        {
            Pair.Key->OnCollisionBodyUpdated().Remove(Pair.Value);
        }
    }
    CollisionBodyUpdatedHandles.Empty();

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

    if (VoxelForgeStartupTrace::IsActive())
    {
        VoxelForgeStartupTrace::Finish(TEXT("end_play_before_steady_state"));
    }

    Super::EndPlay(EndPlayReason);
}

void AVoxelWorld::BeginPlay()
{
    VoxelForgeStartupTrace::BeginFromCommandLine();
    VoxelForgeStartupTrace::FStageScope StartupTraceStage(TEXT("BeginPlay"));
    Super::BeginPlay();
    VoxelCaveMorphology::ConfigurePlayerFitMemoFromCommandLine();
    VoxelCaveMorphology::ResetPlayerFitMemoStats();
    bShuttingDown.store(false, std::memory_order_relaxed);
    LOD0ReadySamples.Reset();
    ObsoleteTileAbortCount = 0;
    PeakObservedPawnSpeedCmPerSecond = 0.0f;
    TotalWorkerGenerationCycles.store(0, std::memory_order_relaxed);
    TotalWorkerGenerationTasks.store(0, std::memory_order_relaxed);
    TotalObsoleteWorkerCycles.store(0, std::memory_order_relaxed);
    TotalObsoleteWorkerTasks.store(0, std::memory_order_relaxed);
    TotalValidationDensityCalls.store(0, std::memory_order_relaxed);
    TotalMesherDensityCalls.store(0, std::memory_order_relaxed);
    for (int32 LOD = 0; LOD < TrackedClassifierLODCount; ++LOD)
    {
        OuterClassifierCallsByLOD[LOD].store(0, std::memory_order_relaxed);
        for (int32 Verdict = 0; Verdict < 3; ++Verdict)
        {
            OuterClassifierVerdictsByLOD[LOD][Verdict].store(0, std::memory_order_relaxed);
        }
    }
    ConfigureHeadlessStreamingTest();

    // Some headless harnesses load module CVars after the engine's startup-CVar pass. Read the
    // profiling switches explicitly as well, so a reported game profile cannot silently run with
    // the default mode because of startup ordering.
    int32 CommandLineProfile = GVoxelForgeProfileDensity;
    int32 CommandLineProfileFull = GVoxelForgeProfileDensityFull;
    int32 CommandLinePerfAttribution = GVoxelForgePerfAttribution;
    int32 CommandLineProfileTileGeneration = GVoxelForgeProfileTileGeneration;
    int32 CommandLineUseBlockEarlyOut = GVoxelForgeUseBlockEarlyOut;
    int32 CommandLineOuterClassifierMode = GVoxelForgeOuterClassifierMode;
    int32 CommandLineSampleStacks = GVoxelForgeSampleStacks;
    if (FParse::Value(FCommandLine::Get(), TEXT("voxel.ProfileDensity="), CommandLineProfile))
    {
        GVoxelForgeProfileDensity = CommandLineProfile;
    }
    if (FParse::Value(FCommandLine::Get(), TEXT("voxel.ProfileDensityFull="), CommandLineProfileFull))
    {
        GVoxelForgeProfileDensityFull = CommandLineProfileFull;
    }
    if (FParse::Value(FCommandLine::Get(), TEXT("voxel.PerfAttribution="), CommandLinePerfAttribution))
    {
        GVoxelForgePerfAttribution = CommandLinePerfAttribution;
    }
    if (FParse::Value(
            FCommandLine::Get(),
            TEXT("voxel.ProfileTileGeneration="),
            CommandLineProfileTileGeneration))
    {
        GVoxelForgeProfileTileGeneration = CommandLineProfileTileGeneration;
    }
    if (FParse::Value(FCommandLine::Get(), TEXT("voxel.UseBlockEarlyOut="), CommandLineUseBlockEarlyOut))
    {
        GVoxelForgeUseBlockEarlyOut = CommandLineUseBlockEarlyOut;
    }
    if (FParse::Value(FCommandLine::Get(), TEXT("voxel.OuterClassifierMode="), CommandLineOuterClassifierMode))
    {
        GVoxelForgeOuterClassifierMode = CommandLineOuterClassifierMode;
    }
    if (FParse::Value(FCommandLine::Get(), TEXT("voxel.SampleStacks="), CommandLineSampleStacks))
    {
        GVoxelForgeSampleStacks = CommandLineSampleStacks;
    }
    GVoxelForgeProfileTileGeneration = GVoxelForgeProfileTileGeneration != 0 ? 1 : 0;
    GVoxelForgeUseBlockEarlyOut = GVoxelForgeUseBlockEarlyOut != 0 ? 1 : 0;
    GVoxelForgeOuterClassifierMode = FMath::Clamp(GVoxelForgeOuterClassifierMode, 0, 1);
    GVoxelForgeSampleStacks = FMath::Clamp(
        GVoxelForgeSampleStacks,
        0,
        static_cast<int32>(FVoxelStackSampler::MaxIntervalUs));

    if (GVoxelForgePerfAttribution != 0)
    {
        VoxelDensityProfile::SetMode(
            VoxelDensityProfile::EMode::Attribution,
            VoxelDensityProfile::DefaultSampleInterval);
        UE_LOG(LogTemp, Display,
            TEXT("[VoxelForgeDensityProfile] game_mode=attribution sample_interval=%u"),
            VoxelDensityProfile::DefaultSampleInterval);
    }
    else if (GVoxelForgeProfileDensityFull != 0)
    {
        VoxelDensityProfile::SetMode(
            VoxelDensityProfile::EMode::Full,
            VoxelDensityProfile::DefaultSampleInterval);
        UE_LOG(LogTemp, Display,
            TEXT("[VoxelForgeDensityProfile] game_mode=full sample_interval=%u"),
            VoxelDensityProfile::DefaultSampleInterval);
    }
    else if (GVoxelForgeProfileDensity != 0)
    {
        VoxelDensityProfile::SetMode(
            VoxelDensityProfile::EMode::Sampled,
            VoxelDensityProfile::DefaultSampleInterval);
        UE_LOG(LogTemp, Display,
            TEXT("[VoxelForgeDensityProfile] game_mode=sampled sample_interval=%u"),
            VoxelDensityProfile::DefaultSampleInterval);
    }

    if (!Settings)
    {
        UE_LOG(LogTemp, Error, TEXT("[VoxelWorld] No Settings assigned — world won't generate."));
        return;
    }

    if (VoxelForgeStartupTrace::IsActive())
    {
        FString SettingsPath = Settings->GetPathName();
        SettingsPath.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
        SettingsPath.ReplaceInline(TEXT("\""), TEXT("\\\""));
        VoxelForgeStartupTrace::RecordEvent(TEXT("settings_resolved"), FString::Printf(
            TEXT("\"asset\":\"%s\",\"seed\":%d,\"total_strates\":%d,\"inter_strate_gap_chunks\":%d,"
                 "\"strate_pool\":%d,\"max_clip_level\":%d,\"clip_radius\":%d,"
                 "\"render_distance_chunks\":%d,\"far_sheet_ring\":%d,\"far_sheet_span_levels\":%d,"
                 "\"enable_density_volume\":%d,\"density_volume_resolution\":%d,\"density_volume_levels\":%d,"
                 "\"density_volume_max_tasks\":%d,\"density_volume_gpu_upload\":%d"),
            *SettingsPath,
            Settings->GetEffectiveWorldSeed(), Settings->TotalStrates,
            Settings->InterStrateGapChunks, Settings->StratePool.Num(), Settings->MaxClipLevel,
            Settings->ClipRadius, Settings->RenderDistanceChunks,
            Settings->bFarSheetRing ? 1 : 0, Settings->FarSheetSpanLevels,
            Settings->bEnableDensityVolume ? 1 : 0, Settings->DensityVolumeResolution,
            Settings->DensityVolumeLevels, Settings->DensityVolumeMaxTasks,
            Settings->bDensityVolumeGPUUpload ? 1 : 0));
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
    Mesher->SetShutdownFlag(&bShuttingDown);
    Mesher->bGenerateSkirts = Settings->bGenerateSkirts;
    Mesher->SkirtCells      = Settings->SkirtCells;
    Mesher->LODOctaveDrop   = Settings->LODOctaveDrop;   // T2.b — 0 = off
    Mesher->bUseBlockEarlyOut = GVoxelForgeUseBlockEarlyOut != 0;

    UE_LOG(LogTemp, Display,
        TEXT("[VoxelForgeClassifierConfig] outer_mode=%d nested_block_early_out=%d"),
        GVoxelForgeOuterClassifierMode, Mesher->bUseBlockEarlyOut ? 1 : 0);

    VoxelForgeStartupTrace::RecordEvent(TEXT("generator_mesher_constructed"), FString::Printf(
        TEXT("\"generator\":1,\"mesher\":1,\"generate_skirts\":%d,\"full_res_clip_levels\":%d,\"coarse_tile_cells\":%d"),
        Settings->bGenerateSkirts ? 1 : 0,
        Settings->FullResClipLevels, Settings->CoarseTileCells));

    // Système de strates — a cooked season takes precedence; otherwise the authored pool path is
    // exactly the legacy one.
    if (!Settings->Season.IsNull() || Settings->StratePool.Num() > 0)
    {
        StrateManager = NewObject<UVoxelStrateManager>(this);
        if (!StrateManager->Initialize(Settings, Settings->GetEffectiveWorldSeed()))
        {
            UE_LOG(LogTemp, Error,
                TEXT("[VoxelWorld] World generation disabled because its assigned season is invalid or incomplete."));
            bShuttingDown.store(true, std::memory_order_release);
            SetActorTickEnabled(false);
            return;
        }
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
    ContentManager->Initialize(
        this, StrateManager, Generator, Settings, Settings->GetEffectiveWorldSeed());

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

    VoxelForgeStartupTrace::RecordEvent(TEXT("density_volume_resolved"), FString::Printf(
        TEXT("\"enabled\":%d,\"created\":%d,\"gpu_upload\":%d,\"resolution\":%d,\"levels\":%d"),
        Settings->bEnableDensityVolume ? 1 : 0, DensityVolume != nullptr ? 1 : 0,
        Settings->bDensityVolumeGPUUpload ? 1 : 0,
        Settings->DensityVolumeResolution, Settings->DensityVolumeLevels));

#if WITH_EDITOR
    // Listen for data asset edits during PIE so live edit can detect
    // strate definition changes (PostEditChangeProperty only fires for
    // properties on this actor itself, not on referenced data assets).
    OnObjectModifiedHandle = FCoreUObjectDelegates::OnObjectModified.AddUObject(
        this, &AVoxelWorld::OnObjectModifiedInEditor);
#endif

    // A possessed pawn may already exist when BeginPlay runs. Engage the same pre-physics gate
    // immediately; Tick repeats the check for late possession and for pawn replacement.
    FVector InitialPlayerPosition = FVector::ZeroVector;
    APawn* InitialPawn = nullptr;
    if (TryGetPlayerPosition(InitialPlayerPosition, &InitialPawn))
    {
        UpdatePawnCollisionGate(InitialPawn, InitialPlayerPosition, 0.0f);
    }

    if (GVoxelForgeSampleStacks > 0)
    {
        const FString SamplerOutputDirectory = FPaths::Combine(
            FPaths::ProjectDir(), TEXT("Plugins/VoxelForge/Saved"));
        bSampleStacksStarted = FVoxelStackSampler::Get().Start(
            static_cast<uint32>(GVoxelForgeSampleStacks),
            SamplerOutputDirectory,
            TEXT("game"));
        if (!bSampleStacksStarted)
        {
            UE_LOG(LogTemp, Error,
                TEXT("[VoxelWorld] stack sampler requested but could not be started."));
        }
    }
}

void AVoxelWorld::ConfigureHeadlessStreamingTest()
{
    bHeadlessStreamingTestMovement = false;
    bHeadlessStreamingTestExitRequested = false;
    HeadlessStreamingTestMoveAttempts = 0;
    HeadlessStreamingTestDistanceCm = 0.0;
    HeadlessStreamingTestSpeedCmPerSecond = 0.0f;
    HeadlessStreamingTestStartSeconds = 0.0f;
    HeadlessStreamingTestExitSeconds = 0.0f;
    HeadlessStreamingTestDirection = FVector::XAxisVector;
    HeadlessStreamingTestLastActualPosition = FVector::ZeroVector;
    HeadlessStreamingTestBeginSeconds = 0.0;
    HeadlessStreamingTestLastElapsedSeconds = 0.0;

    const TCHAR* CommandLine = FCommandLine::Get();
    int32 MoveValue = 0;
    const bool bMoveRequested =
        (FParse::Value(CommandLine, TEXT("voxel.TestMove="), MoveValue) && MoveValue != 0)
        || FParse::Param(CommandLine, TEXT("voxel.TestMove"));
    float Speed = 800.0f;
    float StartSeconds = 20.0f;
    float ExitSeconds = 0.0f;
    FParse::Value(CommandLine, TEXT("voxel.TestMoveSpeedCmPerSecond="), Speed);
    FParse::Value(CommandLine, TEXT("voxel.TestMoveStartSeconds="), StartSeconds);
    FParse::Value(CommandLine, TEXT("voxel.TestExitSeconds="), ExitSeconds);

    if (bMoveRequested)
    {
        FString DirectionText;
        if (FParse::Value(CommandLine, TEXT("voxel.TestMoveDirection="), DirectionText))
        {
            TArray<FString> Components;
            DirectionText.ParseIntoArray(Components, TEXT(","), true);
            if (Components.Num() == 3)
            {
                HeadlessStreamingTestDirection = FVector(
                    FCString::Atof(*Components[0]),
                    FCString::Atof(*Components[1]),
                    FCString::Atof(*Components[2]));
            }
        }
        HeadlessStreamingTestDirection = HeadlessStreamingTestDirection.GetSafeNormal();
        if (HeadlessStreamingTestDirection.IsNearlyZero())
        {
            HeadlessStreamingTestDirection = FVector::XAxisVector;
        }
        HeadlessStreamingTestSpeedCmPerSecond = FMath::Max(Speed, 0.0f);
        HeadlessStreamingTestStartSeconds = FMath::Max(StartSeconds, 0.0f);
        bHeadlessStreamingTestMovement = HeadlessStreamingTestSpeedCmPerSecond > 0.0f;
    }

    HeadlessStreamingTestExitSeconds = FMath::Max(ExitSeconds, 0.0f);
    if (bHeadlessStreamingTestMovement || HeadlessStreamingTestExitSeconds > 0.0f)
    {
        HeadlessStreamingTestBeginSeconds = FPlatformTime::Seconds();
        UE_LOG(LogTemp, Display,
            TEXT("[VoxelForgeStreamingTest] clean=1 movement=%d speed_cm_s=%.3f start_s=%.3f "
                 "exit_s=%.3f direction=(%.3f,%.3f,%.3f)"),
            bHeadlessStreamingTestMovement ? 1 : 0,
            HeadlessStreamingTestSpeedCmPerSecond,
            HeadlessStreamingTestStartSeconds,
            HeadlessStreamingTestExitSeconds,
            HeadlessStreamingTestDirection.X,
            HeadlessStreamingTestDirection.Y,
            HeadlessStreamingTestDirection.Z);
    }
}

void AVoxelWorld::AdvanceHeadlessStreamingTest(
    FVector& InOutPlayerPosition, FVector& InOutPlayerHeading, APawn* PlayerPawn)
{
    if (!bHeadlessStreamingTestMovement || !PlayerPawn || HeadlessStreamingTestBeginSeconds <= 0.0)
    {
        return;
    }

    const double ElapsedSeconds = FPlatformTime::Seconds() - HeadlessStreamingTestBeginSeconds;
    if (ElapsedSeconds < static_cast<double>(HeadlessStreamingTestStartSeconds))
    {
        return;
    }

    // The headless game runs uncapped, so simulation DeltaTime is not a stable movement clock. Use
    // wall time for this explicit harness path; both A/B runs then cover exactly the same distance
    // regardless of how much work the scheduler is doing per frame.
    const double PreviousElapsedSeconds = FMath::Max(
        HeadlessStreamingTestLastElapsedSeconds,
        static_cast<double>(HeadlessStreamingTestStartSeconds));
    const double MoveSeconds = FMath::Max(0.0, ElapsedSeconds - PreviousElapsedSeconds);
    HeadlessStreamingTestLastElapsedSeconds = ElapsedSeconds;
    if (MoveSeconds <= 0.0)
    {
        return;
    }

    const FVector NewPosition = InOutPlayerPosition
        + HeadlessStreamingTestDirection
        * HeadlessStreamingTestSpeedCmPerSecond
        * static_cast<float>(MoveSeconds);
    const bool bMoved = PlayerPawn->SetActorLocation(
        NewPosition, false, nullptr, ETeleportType::TeleportPhysics);
    ++HeadlessStreamingTestMoveAttempts;
    if (bMoved)
    {
        const FVector ActualPosition = PlayerPawn->GetActorLocation();
        HeadlessStreamingTestDistanceCm += FVector::Dist(InOutPlayerPosition, ActualPosition);
        HeadlessStreamingTestLastActualPosition = ActualPosition;
        InOutPlayerPosition = ActualPosition;
        InOutPlayerHeading = HeadlessStreamingTestDirection * HeadlessStreamingTestSpeedCmPerSecond;

        // The explicit -nullrhi harness moves the pawn from this world's pre-physics tick. Keep
        // the template character movement component from applying its own zero-input/gravity step
        // afterward and undoing the synthetic teleport before the next scheduler observation.
        if (ACharacter* Character = Cast<ACharacter>(PlayerPawn))
        {
            if (UCharacterMovementComponent* CharacterMovement = Character->GetCharacterMovement())
            {
                CharacterMovement->StopMovementImmediately();
                CharacterMovement->SetMovementMode(MOVE_None);
            }
        }
    }
}

void AVoxelWorld::MaybeFinishHeadlessStreamingTest()
{
    if (bHeadlessStreamingTestExitRequested || HeadlessStreamingTestExitSeconds <= 0.0f
        || HeadlessStreamingTestBeginSeconds <= 0.0)
    {
        return;
    }

    if (FPlatformTime::Seconds() - HeadlessStreamingTestBeginSeconds
        < static_cast<double>(HeadlessStreamingTestExitSeconds))
    {
        return;
    }

    bHeadlessStreamingTestExitRequested = true;
    UE_LOG(LogTemp, Display,
        TEXT("[VoxelForgeStreamingTest] complete; moves=%d distance_m=%.6f last_position=(%.1f,%.1f,%.1f); requesting graceful exit after clean session"),
        HeadlessStreamingTestMoveAttempts,
        HeadlessStreamingTestDistanceCm / 100.0,
        HeadlessStreamingTestLastActualPosition.X,
        HeadlessStreamingTestLastActualPosition.Y,
        HeadlessStreamingTestLastActualPosition.Z);
    FPlatformMisc::RequestExit(false, TEXT("VoxelForge headless streaming test complete"));
}

void AVoxelWorld::Tick(float DeltaTime)
{
    Super::Tick(DeltaTime);
    TRACE_CPUPROFILER_EVENT_SCOPE(VoxelForge_Tick);   // game-thread streaming orchestration breakdown
    FVector PlayerLastPos = FVector::ZeroVector;
    APawn* PlayerPawn = nullptr;
    const bool bHasPlayer = TryGetPlayerPosition(PlayerLastPos, &PlayerPawn);

    if (bHasPlayer) {
        // Capture velocity before the collision gate can deliberately zero it while the support
        // tile is cooking. The scheduler uses this as the movement heading for its next-tile
        // critical prefix; a stationary pawn falls back to actor forward in BuildDesiredTiles.
        FVector PlayerHeading = PlayerPawn ? PlayerPawn->GetVelocity() : FVector::ZeroVector;
        AdvanceHeadlessStreamingTest(PlayerLastPos, PlayerHeading, PlayerPawn);
        PeakObservedPawnSpeedCmPerSecond = FMath::Max(
            PeakObservedPawnSpeedCmPerSecond, PlayerHeading.Size2D());
        UpdatePawnCollisionGate(PlayerPawn, PlayerLastPos, DeltaTime);
        UpdateChunksAroundPosition(PlayerLastPos, PlayerPawn, PlayerHeading);
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
    else
    {
        ReleasePawnCollisionGate();
        if (APawn* PreviousPawn = PawnTickPrerequisite.Get())
        {
            PreviousPawn->RemoveTickPrerequisiteActor(this);
        }
        if (UPawnMovementComponent* PreviousMovement = PawnMovementTickPrerequisite.Get())
        {
            PreviousMovement->RemoveTickPrerequisiteActor(this);
        }
        PawnTickPrerequisite.Reset();
        PawnMovementTickPrerequisite.Reset();
    }
    // The diagnostic harness uses an explicit command-line token rather than a console command:
    // Unreal processes startup ExecCmds before this runtime module's console registrations are
    // guaranteed to exist. Reading the command line here is order-independent, and the actual edit
    // still waits for a loaded tile and goes through the ordinary modification path below.
    if (!GVoxelForgeTestModificationCommandLineConsumed)
    {
        GVoxelForgeTestModificationCommandLineConsumed = true;
        const FString CommandLine(FCommandLine::Get());
        if (CommandLine.Contains(TEXT("-voxel.TestModification=1"), ESearchCase::IgnoreCase))
        {
            GVoxelForgePendingModificationRadius = 3.0f;
            GVoxelForgePendingModificationStrength = 10.0f;
            UE_LOG(LogTemp, Display,
                TEXT("[VoxelForgeTestModification] queued radius=%.2f strength=%.2f"),
                GVoxelForgePendingModificationRadius,
                GVoxelForgePendingModificationStrength);
            GVoxelForgePendingTestModification = 1;
        }
    }
    ProcessPendingChunks();

    // The startup trace stops at the same point the streaming policy declares itself settled:
    // every desired key is loaded, no generation task remains, and the game-thread result drain
    // has run for the frame. This is intentionally after ProcessPendingChunks so request-to-apply
    // includes the final submission; the separate collision_ready events carry cook latency.
    if (VoxelForgeStartupTrace::IsActive()
        && bAllChunksLoaded
        && PendingTiles.Num() == 0
        && bStartupTraceDesiredRecorded
        && PendingCollisionCooks.Num() == 0
        // The desired set being applied is not the player-ready point: the feet tile must have
        // received its completed LOD0 RMC body first. If no pawn exists, there is no gate to wait
        // for and the headless trace keeps its old completion behavior.
        && (!bHasPlayer || IsPlayerSupportCollisionReady(PlayerPawn, PlayerLastPos)))
    {
        TMap<int32, int32> SteadyLevelCounts;
        TMap<int32, FIntVector> SteadyLevelMins;
        TMap<int32, FIntVector> SteadyLevelMaxs;
        for (const FVoxelTileKey& Key : DesiredSorted)
        {
            ++SteadyLevelCounts.FindOrAdd(Key.Level);
            FIntVector& Min = SteadyLevelMins.FindOrAdd(Key.Level, FIntVector(MAX_int32, MAX_int32, MAX_int32));
            FIntVector& Max = SteadyLevelMaxs.FindOrAdd(Key.Level, FIntVector(MIN_int32, MIN_int32, MIN_int32));
            Min.X = FMath::Min(Min.X, Key.Coord.X);
            Min.Y = FMath::Min(Min.Y, Key.Coord.Y);
            Min.Z = FMath::Min(Min.Z, Key.Coord.Z);
            Max.X = FMath::Max(Max.X, Key.Coord.X);
            Max.Y = FMath::Max(Max.Y, Key.Coord.Y);
            Max.Z = FMath::Max(Max.Z, Key.Coord.Z);
        }
        VoxelForgeStartupTrace::RecordEvent(TEXT("steady_state"), FString::Printf(
             TEXT("\"desired_tiles\":%d,\"loaded_tiles\":%d,\"collision_ready_tiles\":%d,"
                  "\"collision_not_required_tiles\":%d,\"pending_collision_cooks\":%d,"
                  "\"pawn_collision_gate_engaged\":%d,"
                  "\"obsolete_tile_aborts\":%d,\"desired_epoch\":%u,"
                 "\"pending_tiles\":%d,\"pending_unload\":%d,\"generation_epoch\":%u"),
            DesiredSorted.Num(), LoadedTiles.Num(), CollisionReadyTiles.Num(),
            CollisionNotRequiredTiles.Num(), PendingCollisionCooks.Num(),
            bPawnCollisionGateEngaged ? 1 : 0,
            ObsoleteTileAbortCount, DesiredEpoch,
            PendingTiles.Num(), PendingUnload.Num(), GenerationEpoch));
        TArray<int32> SteadyLevels;
        SteadyLevelCounts.GetKeys(SteadyLevels);
        SteadyLevels.Sort();
        for (const int32 Level : SteadyLevels)
        {
            const FIntVector Min = SteadyLevelMins.FindChecked(Level);
            const FIntVector Max = SteadyLevelMaxs.FindChecked(Level);
            const int32 ExtentVoxels = CHUNK_SIZE << Level;
            const double TileExtentMetres = static_cast<double>(ExtentVoxels) * VOXEL_SIZE / 100.0;
            const double SpanX = static_cast<double>(Max.X - Min.X + 1) * TileExtentMetres;
            const double SpanY = static_cast<double>(Max.Y - Min.Y + 1) * TileExtentMetres;
            const double SpanZ = static_cast<double>(Max.Z - Min.Z + 1) * TileExtentMetres;
            VoxelForgeStartupTrace::RecordEvent(TEXT("steady_desired_level"), FString::Printf(
                TEXT("\"level\":%d,\"tiles\":%d,\"min_coord\":[%d,%d,%d],\"max_coord\":[%d,%d,%d],"
                     "\"tile_extent_m\":%s,\"coverage_extent_m\":[%s,%s,%s],\"sheet\":%d"),
                Level, SteadyLevelCounts.FindChecked(Level),
                Min.X, Min.Y, Min.Z, Max.X, Max.Y, Max.Z,
                *FString::SanitizeFloat(TileExtentMetres),
                *FString::SanitizeFloat(SpanX), *FString::SanitizeFloat(SpanY), *FString::SanitizeFloat(SpanZ),
                Settings && Level > Settings->MaxClipLevel ? 1 : 0));
        }
        VoxelForgeStartupTrace::Finish(TEXT("desired_set_satisfied_and_queue_drained"));
    }

    // Complete the one-shot diagnostic request only after the player's level-0 centre tile is
    // loaded and idle. Waiting for any coarse tile would take the async-neighbour branch and would
    // not measure the normal synchronous centre remesh that makes a local edit feel expensive.
    // No pawn is a distinct state from a pawn at (0,0,0); keep the diagnostic fallback for
    // headless/editor use without making the world-origin pawn skip streaming.
    FVector ModificationPosition = bHasPlayer ? PlayerLastPos : GetActorLocation();
    const FVoxelTileKey ModificationCenterTile(
        WorldToChunkCoord(WorldToLocalCm(ModificationPosition)), 0);
    if (GVoxelForgePendingTestModification != 0
        && DiffLayer
        && LoadedTiles.Contains(ModificationCenterTile)
        && !PendingTiles.Contains(ModificationCenterTile))
    {
        GVoxelForgePendingTestModification = 0;
        UE_LOG(LogTemp, Display,
            TEXT("[VoxelForgeTestModification] applying position=(%.2f,%.2f,%.2f) radius=%.2f strength=%.2f"),
            ModificationPosition.X, ModificationPosition.Y, ModificationPosition.Z,
            GVoxelForgePendingModificationRadius, GVoxelForgePendingModificationStrength);
        CarveAtPosition(
            ModificationPosition,
            GVoxelForgePendingModificationRadius,
            GVoxelForgePendingModificationStrength);
    }
    ProcessUnloadQueue();
    MaybeFinishHeadlessStreamingTest();

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

bool AVoxelWorld::TryGetPlayerPosition(FVector& OutPosition, APawn** OutPawn) const
{
    OutPosition = FVector::ZeroVector;
    if (OutPawn != nullptr)
    {
        *OutPawn = nullptr;
    }

    UWorld* World = GetWorld();
    APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
    APawn* Pawn = PC ? PC->GetPawn() : nullptr;
    if (!IsValid(Pawn))
    {
        return false;
    }

    const FVector Position = Pawn->GetActorLocation();
    if (!FMath::IsFinite(Position.X) || !FMath::IsFinite(Position.Y)
        || !FMath::IsFinite(Position.Z))
    {
        return false;
    }

    OutPosition = Position;
    if (OutPawn != nullptr)
    {
        *OutPawn = Pawn;
    }
    return true;
}

FVector AVoxelWorld::GetPlayerPosition() const
{
    FVector Position = FVector::ZeroVector;
    TryGetPlayerPosition(Position);
    return Position;
}

bool AVoxelWorld::GetPlayerSupportTile(APawn* Pawn, const FVector& PlayerPosition,
                                        FVoxelTileKey& OutSupportTile) const
{
    if (!IsValid(Pawn))
    {
        return false;
    }

    float HalfHeightCm = 0.0f;
    if (const ACharacter* Character = Cast<ACharacter>(Pawn))
    {
        if (const UCapsuleComponent* Capsule = Character->GetCapsuleComponent())
        {
            HalfHeightCm = Capsule->GetScaledCapsuleHalfHeight();
        }
    }
    else
    {
        // APawn is not required to be a character. Use its colliding component bounds as a
        // conservative feet estimate so custom pawn movement still gets the same safety gate.
        FVector BoundsOrigin = FVector::ZeroVector;
        FVector BoundsExtent = FVector::ZeroVector;
        Pawn->GetActorBounds(/*bOnlyCollidingComponents*/ true, BoundsOrigin, BoundsExtent);
        HalfHeightCm = FMath::Max(0.0f, BoundsExtent.Z);
    }
    if (!FMath::IsFinite(HalfHeightCm))
    {
        return false;
    }

    // Probe one centimetre below the feet. At an exact chunk boundary this selects the tile
    // containing the solid surface below, rather than the empty tile immediately above it.
    FVector LocalFeet = WorldToLocalCm(PlayerPosition);
    LocalFeet.Z -= HalfHeightCm + 1.0f;
    OutSupportTile = FVoxelTileKey(WorldToChunkCoord(LocalFeet), 0);
    return true;
}

bool AVoxelWorld::IsTileCollisionReady(const FVoxelTileKey& Tile) const
{
    if (Tile.Level != 0 || !CollisionReadyTiles.Contains(Tile) || !LoadedTiles.Contains(Tile))
    {
        return false;
    }

    URealtimeMeshComponent* MeshComp = TileComponents.FindRef(Tile);
    if (!IsValid(MeshComp))
    {
        return false;
    }

    URealtimeMeshSimple* Mesh = MeshComp->GetRealtimeMeshAs<URealtimeMeshSimple>();
    return Mesh != nullptr && Mesh->GetBodySetup() != nullptr;
}

bool AVoxelWorld::IsCollisionReadyFromSupportTile(
    const FVoxelTileKey& InitialTile, FVoxelTileKey* OutSupportTile) const
{
    FVoxelTileKey CandidateTile = InitialTile;
    const int32 Direction = CollisionSolidTiles.Contains(InitialTile) ? 1 : -1;

    // A pawn above an all-air/empty band searches down. If its probe lands inside an all-solid
    // tile (including an exact chunk-boundary case), search up instead: the MC boundary can be
    // emitted by the first non-solid tile above it. Never cross an unresolved tile.
    constexpr int32 MaxSupportSearchTiles = 64;
    for (int32 Depth = 0; Depth < MaxSupportSearchTiles; ++Depth)
    {
        if (IsTileCollisionReady(CandidateTile))
        {
            if (OutSupportTile != nullptr)
            {
                *OutSupportTile = CandidateTile;
            }
            return true;
        }
        if (!CollisionNotRequiredTiles.Contains(CandidateTile))
        {
            if (OutSupportTile != nullptr)
            {
                *OutSupportTile = CandidateTile;
            }
            return false;
        }
        CandidateTile.Coord.Z += Direction;
    }
    if (OutSupportTile != nullptr)
    {
        *OutSupportTile = CandidateTile;
    }
    return false;
}

bool AVoxelWorld::IsPlayerSupportCollisionReady(APawn* Pawn, const FVector& PlayerPosition,
                                                 FVoxelTileKey* OutSupportTile) const
{
    FVoxelTileKey CandidateTile;
    if (!GetPlayerSupportTile(Pawn, PlayerPosition, CandidateTile))
    {
        return false;
    }

    return IsCollisionReadyFromSupportTile(CandidateTile, OutSupportTile);
}

void AVoxelWorld::UpdatePawnCollisionGate(APawn* Pawn, const FVector& PlayerPosition,
                                           float DeltaTime)
{
    if (!IsValid(Pawn) || bShuttingDown.load(std::memory_order_relaxed))
    {
        return;
    }

    // Tick prerequisites are kept even while the gate is open. That makes the next movement
    // decision happen before pawn physics, including the first frame after a tile-boundary crossing.
    if (PawnTickPrerequisite.Get() != Pawn)
    {
        if (APawn* PreviousPawn = PawnTickPrerequisite.Get())
        {
            PreviousPawn->RemoveTickPrerequisiteActor(this);
        }
        if (UPawnMovementComponent* PreviousMovement = PawnMovementTickPrerequisite.Get())
        {
            PreviousMovement->RemoveTickPrerequisiteActor(this);
        }
        PawnTickPrerequisite = Pawn;
        Pawn->AddTickPrerequisiteActor(this);
        bPawnGateUnsupportedReported = false;
    }

    // The actor prerequisite covers the pawn tick; attach the same prerequisite directly to its
    // movement component because CharacterMovementComponent has its own component tick function.
    // Re-check this even for the same pawn: possession/setup code can replace the component late.
    UPawnMovementComponent* CurrentMovement = Pawn->GetMovementComponent();
    if (PawnMovementTickPrerequisite.Get() != CurrentMovement)
    {
        if (UPawnMovementComponent* PreviousMovement = PawnMovementTickPrerequisite.Get())
        {
            PreviousMovement->RemoveTickPrerequisiteActor(this);
        }
        if (CurrentMovement != nullptr)
        {
            CurrentMovement->AddTickPrerequisiteActor(this);
        }
        PawnMovementTickPrerequisite = CurrentMovement;
    }

    if (bPawnCollisionGateEngaged && CollisionGatedPawn.Get() == Pawn
        && CollisionGatedCharacterMovement.Get() != CurrentMovement
        && CollisionGatedPawnMovement.Get() != CurrentMovement)
    {
        // The component was replaced while the pawn was stopped. Restore the old component (if it
        // still exists) and let the normal path engage the replacement against the same readiness
        // decision below.
        ReleasePawnCollisionGate();
    }

    if (bPawnCollisionGateEngaged && CollisionGatedPawn.Get() != Pawn)
    {
        ReleasePawnCollisionGate();
    }

    FVoxelTileKey CurrentSupportTile;
    const bool bCurrentSupportReady = IsPlayerSupportCollisionReady(
        Pawn, PlayerPosition, &CurrentSupportTile);
    bool bCanRelease = bCurrentSupportReady;
    bool bWaitingForPredictedSupport = false;
    FVoxelTileKey PredictedSupportTile;

    // Check the movement path ahead while the gate is open. Without this, a pawn could leave a
    // ready tile on this frame and enter an un-cooked tile before the next world tick observes it.
    if (bCurrentSupportReady && DeltaTime > 0.0f)
    {
        const FVector Velocity = Pawn->GetVelocity();
        if (!Velocity.IsNearlyZero(1.0f))
        {
            constexpr float SupportProbeSpacingCm = CHUNK_SIZE * VOXEL_SIZE * 0.5f;
            constexpr int32 MaxPredictedSupportProbes = 64;
            const int32 NumPredictedProbes = FMath::Clamp(
                FMath::CeilToInt(Velocity.Size() * DeltaTime / SupportProbeSpacingCm),
                1, MaxPredictedSupportProbes);
            for (int32 ProbeIndex = 1; ProbeIndex <= NumPredictedProbes; ++ProbeIndex)
            {
                const float Alpha = static_cast<float>(ProbeIndex)
                    / static_cast<float>(NumPredictedProbes);
                const FVector PredictedPosition = PlayerPosition + Velocity * DeltaTime * Alpha;
                const bool bHavePredictedSupport = GetPlayerSupportTile(
                    Pawn, PredictedPosition, PredictedSupportTile);
                const bool bPredictedSupportReady = bHavePredictedSupport
                    && IsPlayerSupportCollisionReady(
                        Pawn, PredictedPosition, &PredictedSupportTile);
                if (!bPredictedSupportReady)
                {
                    bCanRelease = false;
                    bWaitingForPredictedSupport = bHavePredictedSupport;
                    break;
                }
            }
        }
    }

    // Engage() stops velocity, so the blocked tile must be remembered across subsequent ticks;
    // otherwise a gate caused by a predicted crossing would release immediately on the next tick.
    if (bPawnCollisionGateEngaged && bPawnGateWaitingForPredictedSupport)
    {
        if (!IsCollisionReadyFromSupportTile(PawnGatePredictedSupportTile,
                                              &PredictedSupportTile))
        {
            bCanRelease = false;
            bWaitingForPredictedSupport = true;
        }
    }

    if (bCanRelease)
    {
        bPawnGateWaitingForPredictedSupport = false;
        ReleasePawnCollisionGate();
        return;
    }

    if (bWaitingForPredictedSupport)
    {
        bPawnGateWaitingForPredictedSupport = true;
        PawnGatePredictedSupportTile = PredictedSupportTile;
    }
    else if (!bCurrentSupportReady)
    {
        bPawnGateWaitingForPredictedSupport = false;
    }

    if (!bPawnCollisionGateEngaged)
    {
        UE_LOG(LogTemp, Display,
            TEXT("[VoxelForgeCollisionGate] waiting pawn=%s current_support=(%d,%d,%d) ready=%d "
                 "predicted_wait=%d"),
            *Pawn->GetName(),
            CurrentSupportTile.Coord.X, CurrentSupportTile.Coord.Y, CurrentSupportTile.Coord.Z,
            bCurrentSupportReady ? 1 : 0,
            bWaitingForPredictedSupport ? 1 : 0);
    }
    EngagePawnCollisionGate(Pawn);
}

void AVoxelWorld::EngagePawnCollisionGate(APawn* Pawn)
{
    if (!IsValid(Pawn))
    {
        return;
    }
    if (bPawnCollisionGateEngaged)
    {
        return;
    }

    CollisionGatedPawn = Pawn;

    if (ACharacter* Character = Cast<ACharacter>(Pawn))
    {
        if (UCharacterMovementComponent* CharacterMovement = Character->GetCharacterMovement())
        {
            CollisionGatedCharacterMovement = CharacterMovement;
            SavedCharacterMovementMode = static_cast<uint8>(CharacterMovement->MovementMode);
            SavedCharacterCustomMovementMode = CharacterMovement->CustomMovementMode;
            bSavedCharacterMovementMode = true;

            CharacterMovement->StopMovementImmediately();
            CharacterMovement->SetMovementMode(MOVE_None);
            bPawnCollisionGateEngaged = true;
            return;
        }
    }

    if (UPawnMovementComponent* PawnMovement = Pawn->GetMovementComponent())
    {
        CollisionGatedPawnMovement = PawnMovement;
        bSavedPawnMovementActive = PawnMovement->IsActive();
        PawnMovement->StopMovementImmediately();
        PawnMovement->Deactivate();
        bPawnCollisionGateEngaged = true;
        return;
    }

    if (!bPawnGateUnsupportedReported)
    {
        UE_LOG(LogTemp, Warning,
            TEXT("[VoxelForgeCollisionGate] pawn=%s has no movement component; cannot gate it "
                 "while support collision cooks"),
            *Pawn->GetName());
        bPawnGateUnsupportedReported = true;
    }
}

void AVoxelWorld::ReleasePawnCollisionGate()
{
    if (!bPawnCollisionGateEngaged)
    {
        CollisionGatedPawn.Reset();
        CollisionGatedPawnMovement.Reset();
        CollisionGatedCharacterMovement.Reset();
        bSavedCharacterMovementMode = false;
        bSavedPawnMovementActive = false;
        bPawnGateWaitingForPredictedSupport = false;
        return;
    }

    if (UCharacterMovementComponent* CharacterMovement = CollisionGatedCharacterMovement.Get())
    {
        if (bSavedCharacterMovementMode)
        {
            CharacterMovement->SetMovementMode(
                static_cast<EMovementMode>(SavedCharacterMovementMode),
                SavedCharacterCustomMovementMode);
        }
    }
    else if (UPawnMovementComponent* PawnMovement = CollisionGatedPawnMovement.Get())
    {
        if (bSavedPawnMovementActive)
        {
            PawnMovement->Activate(true);
        }
        else
        {
            PawnMovement->Deactivate();
        }
    }

    if (APawn* Pawn = CollisionGatedPawn.Get())
    {
        UE_LOG(LogTemp, Display, TEXT("[VoxelForgeCollisionGate] released pawn=%s"), *Pawn->GetName());
    }
    CollisionGatedPawn.Reset();
    CollisionGatedPawnMovement.Reset();
    CollisionGatedCharacterMovement.Reset();
    bSavedCharacterMovementMode = false;
    bSavedPawnMovementActive = false;
    bPawnCollisionGateEngaged = false;
    bPawnGateWaitingForPredictedSupport = false;
}

void AVoxelWorld::BindRealtimeMeshCollisionEvent(URealtimeMesh* Mesh)
{
    if (Mesh == nullptr || CollisionBodyUpdatedHandles.Contains(Mesh))
    {
        return;
    }
    CollisionBodyUpdatedHandles.Add(
        Mesh,
        Mesh->OnCollisionBodyUpdated().AddUObject(
            this, &AVoxelWorld::HandleRealtimeMeshCollisionBodyUpdated));
}

void AVoxelWorld::UnbindRealtimeMeshCollisionEvent(URealtimeMesh* Mesh)
{
    if (Mesh == nullptr)
    {
        return;
    }
    if (FDelegateHandle* Handle = CollisionBodyUpdatedHandles.Find(Mesh))
    {
        Mesh->OnCollisionBodyUpdated().Remove(*Handle);
        CollisionBodyUpdatedHandles.Remove(Mesh);
    }
}

void AVoxelWorld::HandleRealtimeMeshCollisionBodyUpdated(URealtimeMesh* Mesh, UBodySetup* BodySetup)
{
    // RMC dispatches this event on the game thread after installing BodySetup. Keep the future as
    // the per-request authority, but use the native delegate as an independent completion witness
    // and diagnostic boundary (it has no tile/request serial of its own).
    if (!IsInGameThread())
    {
        return;
    }
    for (TPair<FVoxelTileKey, FPendingCollisionCook>& Pair : PendingCollisionCooks)
    {
        FPendingCollisionCook& Pending = Pair.Value;
        if (Pending.Mesh.Get() == Mesh && BodySetup != nullptr)
        {
            Pending.bBodyUpdatedEventSeen = true;
            Pending.BodyUpdatedEventCycles = FPlatformTime::Cycles64();
            return;
        }
    }
}

void AVoxelWorld::RecordLOD0ReadySample(
    uint64 RequestStartCycles, uint64 GenerationStartCycles, uint64 GenerationEndCycles,
    uint64 ApplyStartCycles, uint64 ReadyCycles, uint64 CollisionSubmittedCycles)
{
    if (RequestStartCycles == 0 || ReadyCycles < RequestStartCycles)
    {
        return;
    }

    const auto SecondsBetween = [](uint64 Later, uint64 Earlier) -> double
    {
        return Later >= Earlier ? FPlatformTime::ToSeconds64(Later - Earlier) : 0.0;
    };

    FStreamingLatencySample Sample;
    Sample.RequestToReadySeconds = SecondsBetween(ReadyCycles, RequestStartCycles);
    Sample.GenerationSeconds = (GenerationStartCycles != 0 && GenerationEndCycles >= GenerationStartCycles)
        ? SecondsBetween(GenerationEndCycles, GenerationStartCycles) : 0.0;

    // Queue wait is deliberately only the time before generation plus the result wait before the
    // game-thread apply. Mesh upload and the RMC collision cook stay in request-to-ready but are not
    // mislabeled as scheduler time.
    const double WorkerQueueSeconds = GenerationStartCycles != 0
        ? SecondsBetween(GenerationStartCycles, RequestStartCycles) : 0.0;
    const double ResultQueueSeconds = GenerationEndCycles != 0 && ApplyStartCycles != 0
        ? SecondsBetween(ApplyStartCycles, GenerationEndCycles)
        : (GenerationStartCycles == 0 ? SecondsBetween(ApplyStartCycles, RequestStartCycles) : 0.0);
    Sample.QueueWaitSeconds = WorkerQueueSeconds + ResultQueueSeconds;
    Sample.CollisionCookSeconds = CollisionSubmittedCycles != 0
        ? SecondsBetween(ReadyCycles, CollisionSubmittedCycles) : 0.0;
    LOD0ReadySamples.Add(Sample);
}

void AVoxelWorld::LogStreamingLatencySummary() const
{
    TArray<double> RequestToReady;
    TArray<double> QueueWait;
    TArray<double> Generation;
    TArray<double> CollisionCook;
    RequestToReady.Reserve(LOD0ReadySamples.Num());
    QueueWait.Reserve(LOD0ReadySamples.Num());
    Generation.Reserve(LOD0ReadySamples.Num());
    CollisionCook.Reserve(LOD0ReadySamples.Num());
    for (const FStreamingLatencySample& Sample : LOD0ReadySamples)
    {
        RequestToReady.Add(Sample.RequestToReadySeconds);
        QueueWait.Add(Sample.QueueWaitSeconds);
        Generation.Add(Sample.GenerationSeconds);
        CollisionCook.Add(Sample.CollisionCookSeconds);
    }

    const auto Percentile = [](TArray<double> Values, double Fraction) -> double
    {
        if (Values.Num() == 0) return 0.0;
        Values.Sort();
        const int32 Index = FMath::Clamp(
            FMath::CeilToInt(Fraction * static_cast<double>(Values.Num() - 1)),
            0, Values.Num() - 1);
        return Values[Index];
    };
    const auto Maximum = [](const TArray<double>& Values) -> double
    {
        double Result = 0.0;
        for (const double Value : Values) { Result = FMath::Max(Result, Value); }
        return Result;
    };

    const double RequestP95 = Percentile(RequestToReady, 0.95);
    const double TravelP95Metres = RequestP95
        * static_cast<double>(PeakObservedPawnSpeedCmPerSecond) / 100.0;
    FString ClassifierVerdictSummary;
    for (int32 LOD = 0; LOD < TrackedClassifierLODCount; ++LOD)
    {
        if (LOD > 0)
        {
            ClassifierVerdictSummary += TEXT(";");
        }
        ClassifierVerdictSummary += FString::Printf(
            TEXT("l%d[c=%llu,m=%llu,s=%llu,a=%llu]"),
            LOD,
            static_cast<unsigned long long>(OuterClassifierCallsByLOD[LOD].load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(OuterClassifierVerdictsByLOD[LOD][0].load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(OuterClassifierVerdictsByLOD[LOD][1].load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(OuterClassifierVerdictsByLOD[LOD][2].load(std::memory_order_relaxed)));
    }
    const uint64 WorkerGenerationCycles =
        TotalWorkerGenerationCycles.load(std::memory_order_relaxed);
    const uint64 ObsoleteWorkerCycles =
        TotalObsoleteWorkerCycles.load(std::memory_order_relaxed);
    const bool bDiagnosticsEnabled = GVoxelForgeProfileTileGeneration != 0
        || VoxelForgeStartupTrace::IsActive()
        || VoxelDensityProfile::GetMode() != VoxelDensityProfile::EMode::Disabled;
    UE_LOG(LogTemp, Display,
        TEXT("[VoxelForgeStreamingLatency] mode=%s diagnostics=%s lod0_ready_samples=%d "
             "request_to_ready_s[p50=%.6f p95=%.6f max=%.6f] "
             "queue_wait_s[p50=%.6f p95=%.6f max=%.6f] "
             "generation_s[p50=%.6f p95=%.6f max=%.6f] "
             "collision_cook_s[p50=%.6f p95=%.6f max=%.6f] "
             "peak_speed_cm_s=%.3f p95_travel_m=%.6f obsolete_tile_aborts=%d "
             "outer_classifier_mode=%d nested_block_early_out=%d "
             "applied_tiles=%llu applied_visible_tiles=%llu applied_triangles=%llu "
             "worker_generation_tasks=%llu worker_generation_sum_s=%.6f "
             "validation_density_calls=%llu mesher_density_calls=%llu "
             "obsolete_worker_tasks=%llu obsolete_worker_sum_s=%.6f "
             "outer_classifier_verdicts=%s"),
        bDiagnosticsEnabled ? TEXT("profiled") : TEXT("clean"),
        bDiagnosticsEnabled ? TEXT("on") : TEXT("off"),
        LOD0ReadySamples.Num(),
        Percentile(RequestToReady, 0.50), RequestP95, Maximum(RequestToReady),
        Percentile(QueueWait, 0.50), Percentile(QueueWait, 0.95), Maximum(QueueWait),
        Percentile(Generation, 0.50), Percentile(Generation, 0.95), Maximum(Generation),
        Percentile(CollisionCook, 0.50), Percentile(CollisionCook, 0.95), Maximum(CollisionCook),
         PeakObservedPawnSpeedCmPerSecond, TravelP95Metres, ObsoleteTileAbortCount,
         GVoxelForgeOuterClassifierMode, GVoxelForgeUseBlockEarlyOut,
         static_cast<unsigned long long>(AppliedTileCount),
         static_cast<unsigned long long>(AppliedVisibleTileCount),
         static_cast<unsigned long long>(AppliedTriangleCount),
         static_cast<unsigned long long>(TotalWorkerGenerationTasks.load(std::memory_order_relaxed)),
        FPlatformTime::ToSeconds64(WorkerGenerationCycles),
        static_cast<unsigned long long>(TotalValidationDensityCalls.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(TotalMesherDensityCalls.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(TotalObsoleteWorkerTasks.load(std::memory_order_relaxed)),
        FPlatformTime::ToSeconds64(ObsoleteWorkerCycles),
        *ClassifierVerdictSummary);
}

void AVoxelWorld::HandleTileCollisionCookComplete(
    const FVoxelTileKey& Tile, uint64 SubmissionId, URealtimeMesh* Mesh, uint8 Result)
{
    if (!IsInGameThread())
    {
        TWeakObjectPtr<AVoxelWorld> WeakWorld(this);
        AsyncTask(ENamedThreads::GameThread,
            [WeakWorld, Tile, SubmissionId, Mesh, Result]()
            {
                if (AVoxelWorld* World = WeakWorld.Get())
                {
                    World->HandleTileCollisionCookComplete(
                        Tile, SubmissionId, Mesh, Result);
                }
            });
        return;
    }

    if (bShuttingDown.load(std::memory_order_relaxed))
    {
        return;
    }

    FPendingCollisionCook* Pending = PendingCollisionCooks.Find(Tile);
    URealtimeMesh* CompletedMesh = Pending ? Pending->Mesh.Get() : nullptr;
    if (Pending == nullptr || Pending->SubmissionId != SubmissionId
        || CompletedMesh == nullptr || CompletedMesh != Mesh)
    {
        // A remesh, unload, or pool reuse superseded this future. It must not change the newer
        // tile state, even if RMC finishes the old cook after that transition.
        return;
    }

    const uint64 MeasuredMeshSubmittedCycles = Pending->MeshSubmittedCycles;
    const uint64 MeasuredCollisionSubmittedCycles = Pending->CollisionSubmittedCycles;
    const bool bBodyUpdatedEventSeen = Pending->bBodyUpdatedEventSeen;
    const uint64 BodyUpdatedEventCycles = Pending->BodyUpdatedEventCycles;
    const uint64 MeasuredRequestStartCycles = Pending->RequestStartCycles;
    const uint64 MeasuredGenerationStartCycles = Pending->GenerationStartCycles;
    const uint64 MeasuredGenerationEndCycles = Pending->GenerationEndCycles;
    const uint64 MeasuredApplyStartCycles = Pending->ApplyStartCycles;
    const bool bRecordStreamingLatency = Pending->bRecordStreamingLatency;
    PendingCollisionCooks.Remove(Tile);

    const ERealtimeMeshCollisionUpdateResult CollisionResult =
        static_cast<ERealtimeMeshCollisionUpdateResult>(Result);
    const bool bBodyInstalled = CollisionResult == ERealtimeMeshCollisionUpdateResult::Updated
        && CompletedMesh->GetBodySetup() != nullptr;
    if (bBodyInstalled)
    {
        CollisionReadyTiles.Add(Tile);
    }
    else
    {
        CollisionReadyTiles.Remove(Tile);
        // A failed/ignored current cook is not a loaded guarantee. Leaving the tile unloaded makes
        // the normal submit loop retry it while the pawn remains gated.
        LoadedTiles.Remove(Tile);
        bAllChunksLoaded = false;
    }

    const uint64 ReadyCycles = FPlatformTime::Cycles64();
    if (bBodyInstalled && Tile.Level == 0 && bRecordStreamingLatency)
    {
        RecordLOD0ReadySample(MeasuredRequestStartCycles, MeasuredGenerationStartCycles,
                              MeasuredGenerationEndCycles, MeasuredApplyStartCycles,
                              ReadyCycles, MeasuredCollisionSubmittedCycles);
    }
    const double MeshToCookSeconds = FPlatformTime::ToSeconds64(
        ReadyCycles - MeasuredMeshSubmittedCycles);
    const double CollisionRequestToCookSeconds = FPlatformTime::ToSeconds64(
        ReadyCycles - MeasuredCollisionSubmittedCycles);
    const double BodyEventToFutureSeconds = bBodyUpdatedEventSeen
        && ReadyCycles >= BodyUpdatedEventCycles
        ? FPlatformTime::ToSeconds64(ReadyCycles - BodyUpdatedEventCycles) : -1.0;
    const TCHAR* ResultName = TEXT("Unknown");
    switch (CollisionResult)
    {
        case ERealtimeMeshCollisionUpdateResult::Updated: ResultName = TEXT("Updated"); break;
        case ERealtimeMeshCollisionUpdateResult::Ignored: ResultName = TEXT("Ignored"); break;
        case ERealtimeMeshCollisionUpdateResult::Error:   ResultName = TEXT("Error");   break;
        default: break;
    }
    if (GVoxelForgeProfileTileGeneration != 0 || VoxelForgeStartupTrace::IsActive()
        || !bBodyInstalled)
    {
        UE_LOG(LogTemp, Display,
            TEXT("[VoxelForgeCollisionReady] tile=(%d,%d,%d) level=%d result=%s body_setup=%d "
                 "ready=%d body_event=%d mesh_submit_to_cook=%.6f "
                 "collision_request_to_cook=%.6f body_event_to_future=%.6f"),
            Tile.Coord.X, Tile.Coord.Y, Tile.Coord.Z, Tile.Level, ResultName,
            CompletedMesh->GetBodySetup() != nullptr ? 1 : 0,
            bBodyInstalled ? 1 : 0,
            bBodyUpdatedEventSeen ? 1 : 0,
            MeshToCookSeconds, CollisionRequestToCookSeconds, BodyEventToFutureSeconds);
    }
    if (VoxelForgeStartupTrace::IsActive())
    {
        VoxelForgeStartupTrace::RecordEvent(TEXT("collision_ready"), FString::Printf(
            TEXT("\"tile\":[%d,%d,%d],\"level\":%d,\"result\":\"%s\",\"body_setup\":%d,"
                 "\"ready\":%d,\"body_update_event\":%d,"
                 "\"mesh_submit_to_cook_s\":%s,\"collision_request_to_cook_s\":%s,"
                 "\"body_event_to_future_s\":%s"),
            Tile.Coord.X, Tile.Coord.Y, Tile.Coord.Z, Tile.Level, ResultName,
            CompletedMesh->GetBodySetup() != nullptr ? 1 : 0, bBodyInstalled ? 1 : 0,
            bBodyUpdatedEventSeen ? 1 : 0,
            *FString::SanitizeFloat(MeshToCookSeconds),
            *FString::SanitizeFloat(CollisionRequestToCookSeconds),
            *FString::SanitizeFloat(BodyEventToFutureSeconds)));
    }

    // A completion can occur between world ticks. Refresh immediately so the pawn is released on
    // the same game-thread turn once its feet tile is truly collidable.
    FVector PlayerPosition = FVector::ZeroVector;
    APawn* PlayerPawn = nullptr;
    if (TryGetPlayerPosition(PlayerPosition, &PlayerPawn))
    {
        UpdatePawnCollisionGate(PlayerPawn, PlayerPosition, 0.0f);
    }
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
    const bool bProfileApply = GVoxelForgeProfileTileGeneration != 0;
    const uint64 DrainStartCycles = FPlatformTime::Cycles64();
    const int32 MaxApplies = FMath::Max(
        Settings ? Settings->MaxMeshAppliesPerFrame : 4, 1);
    const float ApplyBudgetMilliseconds = Settings
        ? FMath::Max(Settings->MaxMeshApplyMilliseconds, 0.0f)
        : 2.0f;
    const double ApplyBudgetSeconds =
        static_cast<double>(ApplyBudgetMilliseconds) * 0.001;
    int32 MeshesApplied = 0;
    int32 ResultsDrained = 0;
    bool bTimeBudgetHit = false;
    FChunkResult DequeuedChunk;
    while (ProcessQueue.Dequeue(DequeuedChunk))
    {
        ++ResultsDrained;
        const TSharedPtr<FVoxelTileCancellationState, ESPMode::ThreadSafe> Cancellation =
            PendingTileCancellation.FindRef(DequeuedChunk.Tile);
        const bool bRequestObsolete = DequeuedChunk.bObsolete
            || (Cancellation.IsValid()
                && Cancellation->bObsolete.load(std::memory_order_acquire));
        PendingTileCancellation.Remove(DequeuedChunk.Tile);
        PendingTiles.Remove(DequeuedChunk.Tile);

        // A worker can observe shutdown after entering the mesher. Do not turn that partial
        // result into an all-air tile: it must remain eligible for a future generation epoch.
        // Read the token here as well as on the worker: cancellation can race the final worker
        // check after it has already enqueued a complete result. This also drops a stale result
        // if the tile leaves and re-enters the desired set before the queue is drained.
        if (DequeuedChunk.bAborted || bRequestObsolete)
        {
            if (bRequestObsolete)
            {
                ++ObsoleteTileAbortCount;
            }
            continue;
        }

        // ApplyTileResult does epoch check, capture ingest, empty-release / mesh submission, and
        // only then records the tile as loaded. Only a real (visible) upload counts against the
        // per-frame budget — stale/empty drains are free.
        if (ApplyTileResult(DequeuedChunk))
        {
            ++MeshesApplied;
        }

        // Time-box the entire game-thread drain, not only visible uploads. Empty releases and
        // stale-result cleanup normally cost nothing, but a pooled component teardown or a
        // backend callback can still be the one result that produces the player's hitch.
        if (ApplyBudgetSeconds > 0.0
            && FPlatformTime::ToSeconds64(FPlatformTime::Cycles64() - DrainStartCycles)
                >= ApplyBudgetSeconds)
        {
            bTimeBudgetHit = true;
            break;
        }
        if (MeshesApplied >= MaxApplies)
        {
            break;
        }
    }

    if (bProfileApply && ResultsDrained > 0)
    {
        UE_LOG(LogTemp, Display,
            TEXT("[VoxelForgeApplyFrame] drained=%d meshes=%d elapsed=%.6f "
                 "max_meshes=%d budget_ms=%.3f budget_hit=%d"),
            ResultsDrained,
            MeshesApplied,
            FPlatformTime::ToSeconds64(FPlatformTime::Cycles64() - DrainStartCycles),
            MaxApplies,
            ApplyBudgetMilliseconds,
            bTimeBudgetHit ? 1 : 0);
    }
}

// Game-thread apply for one gen result. Shared by ProcessPendingChunks (async drain) and
// SyncRemeshTile (synchronous carve). Returns true iff a visible mesh was uploaded (budget).
bool AVoxelWorld::ApplyTileResult(FChunkResult& Result)
{
    // Discard results from a previous generation epoch (stale), or for a tile that has since left
    // the desired set. ProcessPendingChunks also reads the per-request token before removing it,
    // closing the race where the worker enqueues just after CancelObsoleteTileWork flips the flag.
    if (Result.bAborted || Result.Epoch != GenerationEpoch
        || !IsDesired(Result.Tile))
    {
        return false;
    }

    // Applied is emitted after the game-thread submission, not when the worker result is dequeued.
    // It deliberately does NOT mean collision-ready: RMC's cook future records that later boundary
    // in HandleTileCollisionCookComplete.
    const bool bResultEmpty = Result.bEmpty || !Result.Streams;
    const bool bTraceTile = VoxelForgeStartupTrace::IsActive();
    const uint64 ApplyStartCycles = FPlatformTime::Cycles64();
    Result.ApplyStartCycles = ApplyStartCycles;
    const auto LogTileApplied = [&]()
    {
        ++AppliedTileCount;
        if (!bResultEmpty)
        {
            ++AppliedVisibleTileCount;
            AppliedTriangleCount += static_cast<uint64>(FMath::Max(0, Result.NumTriangles));
        }
        if ((GVoxelForgeProfileTileGeneration != 0 || bTraceTile)
            && Result.RequestStartCycles != 0)
        {
            const uint64 AppliedCycles = FPlatformTime::Cycles64();
            const bool bHaveWorkerTiming = Result.GenerationStartCycles != 0
                && Result.GenerationEndCycles >= Result.GenerationStartCycles;
            const double RequestToApply = FPlatformTime::ToSeconds64(
                AppliedCycles - Result.RequestStartCycles);
            const double ApplySeconds = ApplyStartCycles != 0
                ? FPlatformTime::ToSeconds64(AppliedCycles - ApplyStartCycles) : 0.0;
            const double GenerationSeconds = bHaveWorkerTiming
                ? FPlatformTime::ToSeconds64(
                    Result.GenerationEndCycles - Result.GenerationStartCycles) : 0.0;
            const double WorkerQueueSeconds = bHaveWorkerTiming
                ? FPlatformTime::ToSeconds64(
                    Result.GenerationStartCycles - Result.RequestStartCycles) : 0.0;
            const double ResultQueueSeconds = bHaveWorkerTiming
                ? FMath::Max(0.0, RequestToApply - WorkerQueueSeconds
                    - GenerationSeconds - ApplySeconds)
                : FMath::Max(0.0, RequestToApply - ApplySeconds);
            if (GVoxelForgeProfileTileGeneration != 0)
            {
                UE_LOG(LogTemp, Display,
                    TEXT("[VoxelForgeTileApplied] tile=(%d,%d,%d) level=%d empty=%d "
                         "request_to_apply=%.6f worker_queue=%.6f generation=%.6f "
                         "result_queue=%.6f apply=%.6f"),
                    Result.Tile.Coord.X, Result.Tile.Coord.Y, Result.Tile.Coord.Z,
                    Result.Tile.Level, bResultEmpty ? 1 : 0,
                    RequestToApply, WorkerQueueSeconds, GenerationSeconds,
                    ResultQueueSeconds, ApplySeconds);
            }
            if (bTraceTile)
            {
                VoxelForgeStartupTrace::FTileSample Sample;
                Sample.TileX = Result.Tile.Coord.X;
                Sample.TileY = Result.Tile.Coord.Y;
                Sample.TileZ = Result.Tile.Coord.Z;
                Sample.Level = Result.Tile.Level;
                Sample.Verdict = Result.ClassifyVerdict;
                Sample.bEmpty = bResultEmpty;
                Sample.Triangles = Result.NumTriangles;
                Sample.RequestToApplySeconds = RequestToApply;
                Sample.WorkerQueueSeconds = WorkerQueueSeconds;
                Sample.ResultQueueSeconds = ResultQueueSeconds;
                Sample.QueueWaitSeconds = WorkerQueueSeconds + ResultQueueSeconds;
                Sample.GenerationSeconds = GenerationSeconds;
                Sample.ClassifySeconds = Result.ClassifySeconds;
                Sample.MeshSeconds = Result.MeshSeconds;
                Sample.StreamSeconds = Result.StreamSeconds;
                Sample.ApplySeconds = ApplySeconds;
                VoxelForgeStartupTrace::RecordTile(Sample);
            }
        }
    };

    // CAPTURE-DURING-MESHING: hand the mesher's captured density grid to the clipmap BEFORE the
    // empty-tile early-out — all-air / all-solid tiles are exactly the uniform cells the volume
    // needs, and they carry a valid CaptureGrid even though they render nothing.
    if (DensityVolume && Result.CaptureGrid.Num() > 0)
    {
        DensityVolume->IngestTileCapture(Result.Tile.Coord, MoveTemp(Result.CaptureGrid));
    }

    // Empty mesh = no-surface tile — nothing to render, but still "loaded".
    if (Result.bEmpty || !Result.Streams)
    {
        // An empty re-gen invalidates any previous body and any completion future for the old
        // geometry. Empty tiles are loaded bookkeeping, never collision-ready.
        CollisionReadyTiles.Remove(Result.Tile);
        CollisionSolidTiles.Remove(Result.Tile);
        if (Result.Tile.Level == 0)
        {
            CollisionNotRequiredTiles.Add(Result.Tile);
            if (Result.ClassifyVerdict == static_cast<int32>(EVoxelTileClass::AllSolid))
            {
                CollisionSolidTiles.Add(Result.Tile);
            }
        }
        PendingCollisionCooks.Remove(Result.Tile);
        // Une RE-GEN (BandRemeshQueue / RemeshDirtyChunks) peut passer de "contenu" à "vide" :
        // bande déplacée hors de la tuile, ou skip cellule-plus-haute-que-la-bande après un
        // changement de strate (LoadTile). L'ancien composant doit tomber, sinon sa vieille
        // géométrie (l'autre strate !) reste affichée. Première gen vide : Find rate, no-op.
        const uint64 EmptyReleaseStartCycles = GVoxelForgeProfileTileGeneration != 0
            ? FPlatformTime::Cycles64() : 0;
        bool bReleasedComponent = false;
        if (URealtimeMeshComponent** OldComp = TileComponents.Find(Result.Tile))
        {
            if (*OldComp)
            {
                ReleaseTileComponent(*OldComp);
                bReleasedComponent = true;
            }
            TileComponents.Remove(Result.Tile);
        }
        if (GVoxelForgeProfileTileGeneration != 0)
        {
            UE_LOG(LogTemp, Display,
                TEXT("[VoxelForgeApplyProfile] level=%d empty=1 component_release=%d total=%.6f"),
                Result.Tile.Level,
                bReleasedComponent ? 1 : 0,
                FPlatformTime::ToSeconds64(
                FPlatformTime::Cycles64() - EmptyReleaseStartCycles));
        }
        LoadedTiles.Add(Result.Tile);
        LogTileApplied();
        if (Result.Tile.Level == 0 && Result.bRecordStreamingLatency)
        {
            RecordLOD0ReadySample(Result.RequestStartCycles, Result.GenerationStartCycles,
                                   Result.GenerationEndCycles, Result.ApplyStartCycles,
                                   FPlatformTime::Cycles64(), 0);
        }
        return false;
    }

    // Apply mesh (GPU upload). The vertex/index buffers were already built (T1.f, on the worker for
    // the async path or inline for the sync carve path); the game thread only uploads them here.
    const bool bApplied = ApplyMeshToTile(Result);
    if (!bApplied)
    {
        // A component/mesh initialisation failure is recoverable. Do not poison LoadedTiles: the
        // ordinary submit loop will retry this desired tile on a later frame.
        CollisionReadyTiles.Remove(Result.Tile);
        CollisionNotRequiredTiles.Remove(Result.Tile);
        CollisionSolidTiles.Remove(Result.Tile);
        PendingCollisionCooks.Remove(Result.Tile);
        LoadedTiles.Remove(Result.Tile);
        bAllChunksLoaded = false;
        LogTileApplied();
        return false;
    }
    LoadedTiles.Add(Result.Tile);
    LogTileApplied();
    return true;
}

// Same-frame level-0 re-mesh on the game thread (see header). Mirrors LoadTile's level-0 parameters
// (Cells = CHUNK_SIZE, Step = 1) + the strate content band; skips density-volume capture (the volume
// is refilled from the diff via MarkDirtyVoxelBox in RemeshDirtyChunks).
void AVoxelWorld::SyncRemeshTile(const FVoxelTileKey& Tile)
{
    if (!Generator || !Mesher || ShouldAbortWork()) return;

    const bool bProfileModification = GVoxelForgeProfileTileGeneration != 0;
    const uint64 SyncStartCycles = bProfileModification ? FPlatformTime::Cycles64() : 0;

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
    Result.DesiredEpoch = DesiredEpoch;
    Result.bRecordStreamingLatency = false;
    Result.RequestStartCycles = bProfileModification
        ? FPlatformTime::Cycles64() : 0;
    const uint64 GenerateStartCycles = bProfileModification ? FPlatformTime::Cycles64() : 0;
    Result.GenerationStartCycles = GenerateStartCycles;
    GenerateTileResult(Tile, OriginVoxels, Step, Cells, GenerationEpoch, /*bWantCapture*/ false,
                       BandVoxLo, BandVoxHi, BandChunkLo, BandChunkHi,
                       /*bSheetTile*/ false, /*SheetChunkZ*/ 0,
                        /*Hole*/ 0, 0, 0, 0, Result, nullptr);   // hole unused (not a sheet tile)
    Result.GenerationEndCycles = GenerateStartCycles != 0
        ? FPlatformTime::Cycles64() : 0;

    const double GenerateSeconds = bProfileModification
        ? FPlatformTime::ToSeconds64(FPlatformTime::Cycles64() - GenerateStartCycles)
        : 0.0;
    const uint64 ApplyStartCycles = bProfileModification ? FPlatformTime::Cycles64() : 0;
    const bool bResultEmpty = Result.bEmpty || !Result.Streams;
    ApplyTileResult(Result);
    if (bProfileModification)
    {
        UE_LOG(LogTemp, Display,
            TEXT("[VoxelForgeSyncRemeshProfile] tile=(%d,%d,%d) empty=%d generation=%.6f "
                 "apply=%.6f total=%.6f"),
            Tile.Coord.X, Tile.Coord.Y, Tile.Coord.Z,
            bResultEmpty ? 1 : 0,
            GenerateSeconds,
            FPlatformTime::ToSeconds64(FPlatformTime::Cycles64() - ApplyStartCycles),
            FPlatformTime::ToSeconds64(FPlatformTime::Cycles64() - SyncStartCycles));
    }
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

void AVoxelWorld::BuildDesiredTiles(const FIntVector& Center, const FVector& PlayerPosition,
                                    APawn* PlayerPawn, const FVector& PlayerHeading,
                                    TArray<FVoxelTileKey>& OutLeavers)
{
    TRACE_CPUPROFILER_EVENT_SCOPE(VoxelForge_BuildDesiredTiles);
    DesiredSorted.Reset();
    CriticalDesiredTiles.Reset();
    OutLeavers.Reset();
    CollisionOnlyTiles.Reset();   // §9.4 — rebuilt by AddAnchorDesiredTiles below
    ++DesiredStamp;   // les upserts ci-dessous marquent le crossing courant
    ++DesiredEpoch;

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

    // Cancel work that no longer belongs to this desired set before scheduling replacements. A
    // worker owns no mutable world state; it only observes its request token in GenerateTileResult.
    CancelObsoleteTileWork();

    // Nearest-first (by tile-centre distance to the TRUE pawn position), so a large tile's
    // artificial chunk-centre approximation cannot put work behind the player. The explicit key
    // tie-break keeps equal-distance ordering deterministic across runs.
    const FVector PlayerVoxel = WorldToLocalCm(PlayerPosition) / VOXEL_SIZE;
    DesiredSorted.Sort([&PlayerVoxel](const FVoxelTileKey& A, const FVoxelTileKey& B)
    {
        const FVector CA = A.CenterCm() / VOXEL_SIZE;
        const FVector CB = B.CenterCm() / VOXEL_SIZE;
        const float DA = FVector::DistSquared(CA, PlayerVoxel);
        const float DB = FVector::DistSquared(CB, PlayerVoxel);
        if (DA != DB) { return DA < DB; }
        if (A.Level != B.Level) { return A.Level < B.Level; }
        if (A.Coord.X != B.Coord.X) { return A.Coord.X < B.Coord.X; }
        if (A.Coord.Y != B.Coord.Y) { return A.Coord.Y < B.Coord.Y; }
        return A.Coord.Z < B.Coord.Z;
    });

    // Absolute floor priority. The support tile is the level-0 tile under the pawn's feet — the
    // one that can catch a falling pawn — followed by the adjacent tile in the dominant movement
    // direction. Keep the actor-centre tile as a fallback for capsule/offset edge cases. Only keys
    // in the current desired set are promoted, so this never expands the streaming footprint.
    const FVoxelTileKey PlayerTile(
        WorldToChunkCoord(WorldToLocalCm(PlayerPosition)), 0);
    FVoxelTileKey OccupiedTile = PlayerTile;
    FVoxelTileKey SupportTile;
    if (GetPlayerSupportTile(PlayerPawn, PlayerPosition, SupportTile))
    {
        OccupiedTile = SupportTile;
    }

    auto AddCriticalTile = [this](const FVoxelTileKey& Key)
    {
        if (IsDesired(Key) && !CriticalDesiredTiles.Contains(Key))
        {
            CriticalDesiredTiles.Add(Key);
        }
    };
    AddCriticalTile(OccupiedTile);

    FVector Heading = PlayerHeading;
    if (Heading.IsNearlyZero() && IsValid(PlayerPawn))
    {
        Heading = PlayerPawn->GetActorForwardVector();
    }
    Heading = GetActorTransform().InverseTransformVectorNoScale(Heading);
    const float AbsX = FMath::Abs(Heading.X);
    const float AbsY = FMath::Abs(Heading.Y);
    const float AbsZ = FMath::Abs(Heading.Z);
    FIntVector HeadingStep = FIntVector::ZeroValue;
    if (AbsX > KINDA_SMALL_NUMBER || AbsY > KINDA_SMALL_NUMBER || AbsZ > KINDA_SMALL_NUMBER)
    {
        if (AbsX >= AbsY && AbsX >= AbsZ)
        {
            HeadingStep.X = Heading.X >= 0.0f ? 1 : -1;
        }
        else if (AbsY >= AbsZ)
        {
            HeadingStep.Y = Heading.Y >= 0.0f ? 1 : -1;
        }
        else
        {
            HeadingStep.Z = Heading.Z >= 0.0f ? 1 : -1;
        }
    }
    if (HeadingStep != FIntVector::ZeroValue)
    {
        FVoxelTileKey NextTile = OccupiedTile;
        NextTile.Coord += HeadingStep;
        AddCriticalTile(NextTile);
    }
    AddCriticalTile(PlayerTile);

    // Insert in reverse so CriticalDesiredTiles[0] remains the occupied floor, [1] the heading
    // tile, and all ordinary distance-sorted work follows both.
    for (int32 Index = CriticalDesiredTiles.Num() - 1; Index >= 0; --Index)
    {
        const int32 ExistingIndex = DesiredSorted.Find(CriticalDesiredTiles[Index]);
        if (ExistingIndex > 0)
        {
            const FVoxelTileKey Key = DesiredSorted[ExistingIndex];
            DesiredSorted.RemoveAt(ExistingIndex, 1, EAllowShrinking::No);
            DesiredSorted.Insert(Key, 0);
        }
    }
}

void AVoxelWorld::CancelObsoleteTileWork()
{
    for (const FVoxelTileKey& Tile : PendingTiles)
    {
        if (IsDesired(Tile))
        {
            continue;
        }
        if (TSharedPtr<FVoxelTileCancellationState, ESPMode::ThreadSafe>* Cancellation =
                PendingTileCancellation.Find(Tile))
        {
            (*Cancellation)->bObsolete.store(true, std::memory_order_release);
        }
    }
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
    // T2.d — both sides of the cap are authored in Voxel|Streaming. BackgroundNormal priority
    // (see LoadTile) already stops gen from starving the frame; this cap stops a flat task count
    // from thrashing context switches on small CPUs where it exceeds spare parallelism.
    const int32 Asset = Settings ? Settings->MaxConcurrentTasks : 16;
    const int32 LogicalCores = FMath::Max(
        1, FPlatformMisc::NumberOfCoresIncludingHyperthreads());
    const int32 AutomaticCap = FMath::Max(2, LogicalCores - 2);
    const int32 AuthoredCoreCap = Settings ? Settings->MaxGenerationWorkerCores : 0;
    const int32 CoreCap = AuthoredCoreCap > 0
        ? FMath::Clamp(AuthoredCoreCap, 1, LogicalCores)
        : AutomaticCap;
    return FMath::Clamp(Asset, 1, CoreCap);
}

void AVoxelWorld::UpdateChunksAroundPosition(const FVector& CenterPosition, APawn* PlayerPawn,
                                             const FVector& PlayerHeading)
{
    TRACE_CPUPROFILER_EVENT_SCOPE(VoxelForge_UpdateChunks);
    const int32 MaxTasks = GetMaxConcurrentTasks();
    if (GVoxelForgeProfileTileGeneration != 0
        && !GVoxelForgeStreamingBudgetReported)
    {
        const int32 LogicalCores = FMath::Max(
            1, FPlatformMisc::NumberOfCoresIncludingHyperthreads());
        const int32 AutomaticCap = FMath::Max(2, LogicalCores - 2);
        const int32 AssetCap = Settings ? Settings->MaxConcurrentTasks : 16;
        const int32 AuthoredCoreCap = Settings ? Settings->MaxGenerationWorkerCores : 0;
        UE_LOG(LogTemp, Display,
            TEXT("[VoxelForgeStreamingBudget] logical_cores=%d automatic_core_cap=%d "
                 "authored_core_cap=%d asset_task_cap=%d effective_generation_cap=%d "
                 "max_mesh_applies=%d max_apply_ms=%.3f max_unloads=%d"),
            LogicalCores,
            AutomaticCap,
            AuthoredCoreCap,
            AssetCap,
            MaxTasks,
            Settings ? Settings->MaxMeshAppliesPerFrame : 4,
            Settings ? Settings->MaxMeshApplyMilliseconds : 2.0f,
            Settings ? Settings->MaxUnloadsPerFrame : 6);
        GVoxelForgeStreamingBudgetReported = true;
    }

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

        // DELTA CULL: BuildDesiredTiles returns the LEAVERS (desired on the previous crossing, but
        // not now). Only those leavers + TransitionHold (retained across prior crossings) enter
        // the cull. Pending work that leaves desired is marked obsolete before replacement submit;
        // its aborted result is drained and dropped, while the settled cull remains the full-scan
        // safety net.
        TArray<FVoxelTileKey> Leavers;
        const bool bTraceFirstDesired = VoxelForgeStartupTrace::IsActive()
            && !bStartupTraceDesiredRecorded;
        if (bTraceFirstDesired)
        {
            VoxelForgeStartupTrace::StageBegin(TEXT("BuildDesiredTiles.first"));
        }
        BuildDesiredTiles(CenterChunk, CenterPosition, PlayerPawn, PlayerHeading, Leavers);
        if (bTraceFirstDesired)
        {
            VoxelForgeStartupTrace::StageEnd(TEXT("BuildDesiredTiles.first"));

            struct FDesiredLevelSummary
            {
                int32 Level = -1;
                int32 Count = 0;
                FIntVector Min = FIntVector(MAX_int32, MAX_int32, MAX_int32);
                FIntVector Max = FIntVector(MIN_int32, MIN_int32, MIN_int32);
            };
            TArray<FDesiredLevelSummary> LevelSummaries;
            for (const FVoxelTileKey& Key : DesiredSorted)
            {
                int32 SummaryIndex = INDEX_NONE;
                for (int32 Index = 0; Index < LevelSummaries.Num(); ++Index)
                {
                    if (LevelSummaries[Index].Level == Key.Level)
                    {
                        SummaryIndex = Index;
                        break;
                    }
                }
                if (SummaryIndex == INDEX_NONE)
                {
                    SummaryIndex = LevelSummaries.AddDefaulted();
                    LevelSummaries[SummaryIndex].Level = Key.Level;
                }
                FDesiredLevelSummary& Summary = LevelSummaries[SummaryIndex];
                ++Summary.Count;
                Summary.Min.X = FMath::Min(Summary.Min.X, Key.Coord.X);
                Summary.Min.Y = FMath::Min(Summary.Min.Y, Key.Coord.Y);
                Summary.Min.Z = FMath::Min(Summary.Min.Z, Key.Coord.Z);
                Summary.Max.X = FMath::Max(Summary.Max.X, Key.Coord.X);
                Summary.Max.Y = FMath::Max(Summary.Max.Y, Key.Coord.Y);
                Summary.Max.Z = FMath::Max(Summary.Max.Z, Key.Coord.Z);
            }
            LevelSummaries.Sort([](const FDesiredLevelSummary& A, const FDesiredLevelSummary& B)
            {
                return A.Level < B.Level;
            });

            VoxelForgeStartupTrace::RecordEvent(TEXT("desired_set"), FString::Printf(
                TEXT("\"center_chunk\":[%d,%d,%d],\"total\":%d,\"levels\":%d,\"leavers\":%d,"
                     "\"collision_only\":%d"),
                CenterChunk.X, CenterChunk.Y, CenterChunk.Z, DesiredSorted.Num(),
                LevelSummaries.Num(), Leavers.Num(), CollisionOnlyTiles.Num()));
            for (const FDesiredLevelSummary& Summary : LevelSummaries)
            {
                const int32 ExtentVoxels = CHUNK_SIZE << Summary.Level;
                const double TileExtentMetres = static_cast<double>(ExtentVoxels) * VOXEL_SIZE / 100.0;
                const double SpanX = static_cast<double>(Summary.Max.X - Summary.Min.X + 1) * TileExtentMetres;
                const double SpanY = static_cast<double>(Summary.Max.Y - Summary.Min.Y + 1) * TileExtentMetres;
                const double SpanZ = static_cast<double>(Summary.Max.Z - Summary.Min.Z + 1) * TileExtentMetres;
                VoxelForgeStartupTrace::RecordEvent(TEXT("desired_level"), FString::Printf(
                    TEXT("\"level\":%d,\"tiles\":%d,\"min_coord\":[%d,%d,%d],\"max_coord\":[%d,%d,%d],"
                         "\"tile_extent_m\":%s,\"coverage_extent_m\":[%s,%s,%s],\"sheet\":%d"),
                    Summary.Level, Summary.Count,
                    Summary.Min.X, Summary.Min.Y, Summary.Min.Z,
                    Summary.Max.X, Summary.Max.Y, Summary.Max.Z,
                    *FString::SanitizeFloat(TileExtentMetres),
                    *FString::SanitizeFloat(SpanX), *FString::SanitizeFloat(SpanY), *FString::SanitizeFloat(SpanZ),
                    Settings && Summary.Level > Settings->MaxClipLevel ? 1 : 0));
            }
            bStartupTraceDesiredRecorded = true;
        }

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
    // Submit pending work (budgeted, critical floor prefix then nearest-first). Once everything desired is loaded,
    // do the "settled" cull of the deferred LOD-transition tiles, then go idle.
    //=========================================================================
    if (!bAllChunksLoaded)
    {
        TRACE_CPUPROFILER_EVENT_SCOPE(VoxelForge_SubmitTiles);
        int32 Submitted = 0;

        // FLOOR RESPONSIVENESS — the occupied support tile and the next tile along the pawn heading
        // are an absolute prefix, ahead of dirty remeshes, band work, and ordinary clipmap work.
        // They run at BackgroundHigh so the pawn's floor gets the first available worker.
        for (const FVoxelTileKey& T : CriticalDesiredTiles)
        {
            if (PendingTiles.Num() >= MaxTasks) break;
            if (PendingTiles.Contains(T) || LoadedTiles.Contains(T)) continue;
            LoadTile(T, /*bHighPriority*/ true);
            ++Submitted;
        }

        // DIG RESPONSIVENESS — drain player-carve re-meshes next at BackgroundHigh. A tile that's
        // still in flight is KEPT queued (retried next frame) so its stale pre-carve result is corrected.
        for (auto It = DirtyRemeshQueue.CreateIterator(); It; ++It)
        {
            if (PendingTiles.Num() >= MaxTasks) break;
            const FVoxelTileKey T = *It;
            if (!LoadedTiles.Contains(T)) { It.RemoveCurrent(); continue; }  // unloaded — drop
            if (!IsDesired(T)) { It.RemoveCurrent(); continue; }             // obsolete — drop
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
            if (!IsDesired(T) || PendingTiles.Contains(T) || !LoadedTiles.Contains(T)) continue;
            LoadTile(T);
            ++Submitted;
        }

        if (GVoxelForgeProfileTileGeneration != 0
            && PendingTiles.Num() > GVoxelForgeMaxPendingTilesObserved)
        {
            GVoxelForgeMaxPendingTilesObserved = PendingTiles.Num();
            UE_LOG(LogTemp, Display,
                TEXT("[VoxelForgeStreamingBudget] pending_max=%d effective=%d submitted=%d"),
                GVoxelForgeMaxPendingTilesObserved, MaxTasks, Submitted);
        }
        if (GVoxelForgeProfileTileGeneration != 0
            && PendingTiles.Num() >= MaxTasks
            && !GVoxelForgeGenerationCapHitReported)
        {
            UE_LOG(LogTemp, Display,
                TEXT("[VoxelForgeStreamingBudget] generation_cap_hit=1 pending=%d effective=%d"),
                PendingTiles.Num(), MaxTasks);
            GVoxelForgeGenerationCapHitReported = true;
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
        if (GVoxelForgeProfileTileGeneration != 0
            && !GVoxelForgeGenerationCapHitReported)
        {
            UE_LOG(LogTemp, Display,
                TEXT("[VoxelForgeStreamingBudget] generation_cap_hit=1 pending=%d effective=%d"),
                PendingTiles.Num(), MaxTasks);
            GVoxelForgeGenerationCapHitReported = true;
        }
        return;  // Budget full — wait for a task to finish.
    }
    PendingTiles.Add(Tile);
    const uint64 RequestStartCycles = FPlatformTime::Cycles64();

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
    const uint32 TaskDesiredEpoch = DesiredEpoch;

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
            Empty.DesiredEpoch = TaskDesiredEpoch;
            Empty.RequestStartCycles = RequestStartCycles;
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
            Empty.DesiredEpoch = TaskDesiredEpoch;
            Empty.RequestStartCycles = RequestStartCycles;
            Empty.BandChunkLo = BandChunkLo;
            Empty.BandChunkHi = BandChunkHi;
            ProcessQueue.Enqueue(MoveTemp(Empty));
            return;
        }
    }

    const TSharedPtr<FVoxelTileCancellationState, ESPMode::ThreadSafe> Cancellation =
        MakeShared<FVoxelTileCancellationState, ESPMode::ThreadSafe>();
    PendingTileCancellation.Add(Tile, Cancellation);
    ActiveTaskCount.fetch_add(1, std::memory_order_relaxed);

    // BackgroundNormal priority: gen runs on background workers that YIELD to foreground
    // (game/render-thread) tasks. Without this, raising MaxConcurrentTasks past the spare
    // core count saturates the scheduler and starves the frame (the "over 12 = lag" symptom).
    // At background priority the frame keeps its cores; gen just fills in around it.
    // FLOOR/DIG RESPONSIVENESS: critical support requests and player carves launch at BackgroundHigh
    // (bHighPriority) — still a background worker (yields to the frame, keeps the invariant) but
    // jumps AHEAD of ordinary pending streaming gen.
    const UE::Tasks::ETaskPriority TaskPriority = bHighPriority
        ? UE::Tasks::ETaskPriority::BackgroundHigh
        : UE::Tasks::ETaskPriority::BackgroundNormal;
    UE::Tasks::Launch(TEXT("ChunkGen"), [this, Tile, OriginVoxels, Step, Cells, TaskEpoch, bWantCapture,
                                          BandVoxLo, BandVoxHi, BandChunkLo, BandChunkHi,
                                          bSheetTile, SheetChunkZ, HoleMinX, HoleMinY, HoleMaxX, HoleMaxY,
                                          RequestStartCycles, TaskDesiredEpoch, Cancellation]()
    {
        // RAII: decrement the counter on every exit path.
        struct FTaskGuard
        {
            std::atomic<int32>& Counter;
            ~FTaskGuard() { Counter.fetch_sub(1, std::memory_order_relaxed); }
        } Guard{ActiveTaskCount};

        auto EnqueueObsoleteResult = [&]()
        {
            if (bShuttingDown.load(std::memory_order_relaxed)
                || bGenerationPaused.load(std::memory_order_relaxed))
            {
                return;
            }
            FChunkResult Canceled;
            Canceled.Tile = Tile;
            Canceled.Epoch = TaskEpoch;
            Canceled.DesiredEpoch = TaskDesiredEpoch;
            Canceled.RequestStartCycles = RequestStartCycles;
            Canceled.bAborted = true;
            Canceled.bObsolete = true;
            ProcessQueue.Enqueue(MoveTemp(Canceled));
        };

        if (ShouldAbortWork()) return;
        if (Cancellation->bObsolete.load(std::memory_order_relaxed))
        {
            EnqueueObsoleteResult();
            return;
        }

        FScopedVoxelStackRegistration StackRegistration(Tile.Level);
        FChunkResult Result;
        Result.DesiredEpoch = TaskDesiredEpoch;
        const uint64 GenerationStartCycles = FPlatformTime::Cycles64();
        Result.GenerationStartCycles = GenerationStartCycles;
        GenerateTileResult(Tile, OriginVoxels, Step, Cells, TaskEpoch, bWantCapture,
                           BandVoxLo, BandVoxHi, BandChunkLo, BandChunkHi,
                           bSheetTile, SheetChunkZ, HoleMinX, HoleMinY, HoleMaxX, HoleMaxY,
                           Result, &Cancellation->bObsolete);
        Result.GenerationEndCycles = GenerationStartCycles != 0
            ? FPlatformTime::Cycles64() : 0;
        Result.RequestStartCycles = RequestStartCycles;

        const uint64 WorkerGenerationCycles = Result.GenerationEndCycles >= GenerationStartCycles
            ? Result.GenerationEndCycles - GenerationStartCycles : 0;
        TotalWorkerGenerationTasks.fetch_add(1, std::memory_order_relaxed);
        TotalWorkerGenerationCycles.fetch_add(WorkerGenerationCycles, std::memory_order_relaxed);
        const bool bObsoleteWork = Cancellation->bObsolete.load(std::memory_order_relaxed)
            || Result.bObsolete;
        if (bObsoleteWork)
        {
            TotalObsoleteWorkerTasks.fetch_add(1, std::memory_order_relaxed);
            TotalObsoleteWorkerCycles.fetch_add(WorkerGenerationCycles, std::memory_order_relaxed);
        }

        if (bObsoleteWork || Result.bAborted)
        {
            if (!ShouldAbortWork())
            {
                EnqueueObsoleteResult();
            }
            return;
        }

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
                                      FChunkResult& Result,
                                      const std::atomic<bool>* ObsoleteFlag)
{
    const bool bLogTileProfile = GVoxelForgeProfileTileGeneration != 0;
    const bool bMeasureTile = bLogTileProfile
        || VoxelDensityProfile::GetMode() == VoxelDensityProfile::EMode::Attribution
        || VoxelForgeStartupTrace::IsActive();
    const double TileStartSeconds = bMeasureTile ? FPlatformTime::Seconds() : 0.0;
    double ClassifySeconds = 0.0;
    double MeshSeconds = 0.0;
    double StreamSeconds = 0.0;
    int32 ClassifyVerdict = -1; // Mixed = 0, AllSolid = 1, AllAir = 2
    FVoxelTileClassificationStats ClassifierStats;
    const bool bProfileOps = bLogTileProfile
        && VoxelDensityProfile::GetMode() == VoxelDensityProfile::EMode::Full;
    const VoxelDensityProfile::FSnapshot TileProfileStart = bMeasureTile
        ? VoxelDensityProfile::SnapshotCurrentThread()
        : VoxelDensityProfile::FSnapshot();
    auto EmitTileProfile = [&]()
    {
        if (bLogTileProfile)
        {
            const VoxelDensityProfile::FSnapshot TileProfileEnd = bMeasureTile
                ? VoxelDensityProfile::SnapshotCurrentThread()
                : VoxelDensityProfile::FSnapshot();
            const auto CounterDelta = [&](VoxelDensityProfile::ECounter Counter) -> uint64
            {
                const int32 Index = static_cast<int32>(Counter);
                return bMeasureTile && TileProfileEnd.Counters[Index] >= TileProfileStart.Counters[Index]
                    ? TileProfileEnd.Counters[Index] - TileProfileStart.Counters[Index] : 0;
            };
            UE_LOG(LogTemp, Display,
                TEXT("[VoxelForgeTileProfile] tile=(%d,%d,%d) level=%d step=%d cells=%d sheet=%d aborted=%d empty=%d "
                     "verdict=%d classify=%.6f mesh=%.6f streams=%.6f seconds=%.6f "
                     "refine=%u stack=%u core_samples=%u final_samples=%u core_hits=%u final_hits=%u "
                     "core_leaves=%u final_leaves=%u tail_queries=%u tail_eval=%u "
                     "stack_work=%.6f core_work=%.6f final_work=%.6f tail_work=%.6f "
                     "room_work=%.6f room_exact=%.6f cache_work=%.6f "
                     "rooms=%d tunnels=%d joins=%d pits=%d chimneys=%d "
                     "whole_mixed=%u whole_solid=%u whole_air=%u final_nodes=%u "
                     "split_nodes=%u max_depth=%u block_builds=%llu block_samples=%llu "
                     "block_ops=%llu block_active=%llu block_pruned=%llu cache_builds=%llu "
                     "cache_generator=%llu cache_tunnel_core=%llu cache_op_shared=%llu "
                     "cache_op_local=%llu cache_classifier_shared=%llu cache_classifier_local=%llu "
                     "cache_unknown=%llu room_candidates=%llu room_evaluated=%llu "
                     "tunnel_candidates=%llu tunnel_evaluated=%llu tunnel_core_candidates=%llu "
                     "tunnel_core_evaluated=%llu support_column_candidates=%llu"),
                Tile.Coord.X, Tile.Coord.Y, Tile.Coord.Z, Tile.Level, Step, Cells,
                bSheetTile ? 1 : 0, Result.bAborted ? 1 : 0,
                Result.bEmpty ? 1 : 0, ClassifyVerdict, ClassifySeconds, MeshSeconds, StreamSeconds,
                FPlatformTime::Seconds() - TileStartSeconds,
                ClassifierStats.RefineNodes, ClassifierStats.StackBoxCalls,
                ClassifierStats.ExactCoreSamples, ClassifierStats.ExactFinalSamples,
                ClassifierStats.ExactCoreCacheHits, ClassifierStats.ExactFinalCacheHits,
                ClassifierStats.ExactCoreLeaves, ClassifierStats.ExactFinalLeaves,
                ClassifierStats.RoomTailQueries, ClassifierStats.RoomTailEvaluated,
                FPlatformTime::ToSeconds64(ClassifierStats.StackBoxCycles),
                FPlatformTime::ToSeconds64(ClassifierStats.ExactCoreCycles),
                FPlatformTime::ToSeconds64(ClassifierStats.ExactFinalCycles),
                FPlatformTime::ToSeconds64(ClassifierStats.RoomTailCycles),
                FPlatformTime::ToSeconds64(ClassifierStats.RoomPropagateCycles),
                FPlatformTime::ToSeconds64(ClassifierStats.RoomExactPrimitiveCycles),
                FPlatformTime::ToSeconds64(ClassifierStats.RoomCacheWindowCycles),
                ClassifierStats.RoomNumRooms, ClassifierStats.RoomNumTunnels,
                ClassifierStats.RoomNumRoomFloorJoins, ClassifierStats.RoomNumPits,
                ClassifierStats.RoomNumChimneys,
                ClassifierStats.WholeMixedNodes, ClassifierStats.WholeSolidNodes,
                ClassifierStats.WholeAirNodes, ClassifierStats.NeedsFinalFieldNodes,
                ClassifierStats.SplitNodes, ClassifierStats.MaxRefinementDepth,
                static_cast<unsigned long long>(CounterDelta(VoxelDensityProfile::ECounter::OpBlockBuilds)),
                static_cast<unsigned long long>(CounterDelta(VoxelDensityProfile::ECounter::OpBlockSamples)),
                static_cast<unsigned long long>(CounterDelta(VoxelDensityProfile::ECounter::OpBlockOperators)),
                static_cast<unsigned long long>(CounterDelta(VoxelDensityProfile::ECounter::OpBlockActiveOperators)),
                static_cast<unsigned long long>(CounterDelta(VoxelDensityProfile::ECounter::OpBlockPrunedOperators)),
                static_cast<unsigned long long>(CounterDelta(VoxelDensityProfile::ECounter::SdfCacheBuild)),
                static_cast<unsigned long long>(CounterDelta(VoxelDensityProfile::ECounter::RoomGraphBuildGeneratorTile)),
                static_cast<unsigned long long>(CounterDelta(VoxelDensityProfile::ECounter::RoomGraphBuildGeneratorTunnelCore)),
                static_cast<unsigned long long>(CounterDelta(VoxelDensityProfile::ECounter::RoomGraphBuildOpShared)),
                static_cast<unsigned long long>(CounterDelta(VoxelDensityProfile::ECounter::RoomGraphBuildOpLocal)),
                static_cast<unsigned long long>(CounterDelta(VoxelDensityProfile::ECounter::RoomGraphBuildClassifierShared)),
                static_cast<unsigned long long>(CounterDelta(VoxelDensityProfile::ECounter::RoomGraphBuildClassifierLocal)),
                static_cast<unsigned long long>(CounterDelta(VoxelDensityProfile::ECounter::RoomGraphBuildUnknown)),
                static_cast<unsigned long long>(CounterDelta(VoxelDensityProfile::ECounter::CaveRoomCandidates)),
                static_cast<unsigned long long>(CounterDelta(VoxelDensityProfile::ECounter::CaveRoomEvaluated)),
                static_cast<unsigned long long>(CounterDelta(VoxelDensityProfile::ECounter::CaveTunnelCandidates)),
                static_cast<unsigned long long>(CounterDelta(VoxelDensityProfile::ECounter::CaveTunnelEvaluated)),
                static_cast<unsigned long long>(CounterDelta(VoxelDensityProfile::ECounter::TunnelCoreCandidates)),
                static_cast<unsigned long long>(CounterDelta(VoxelDensityProfile::ECounter::TunnelCoreEvaluated)),
                static_cast<unsigned long long>(CounterDelta(VoxelDensityProfile::ECounter::TunnelSupportColumnCandidates)));

            if (bProfileOps)
            {
                static const VoxelDensityProfile::EBucket GameOpBuckets[] = {
                    VoxelDensityProfile::EBucket::RoomGraphSource,
                    VoxelDensityProfile::EBucket::SdfCarve,
                    VoxelDensityProfile::EBucket::CaveRoughnessMod,
                    VoxelDensityProfile::EBucket::CaveTerraceMod,
                    VoxelDensityProfile::EBucket::LayerLineMod,
                    VoxelDensityProfile::EBucket::RibbingMod,
                    VoxelDensityProfile::EBucket::CaveOverhangMod,
                    VoxelDensityProfile::EBucket::CaveCliffMod,
                    VoxelDensityProfile::EBucket::ScallopMod,
                    VoxelDensityProfile::EBucket::CaveArchMod,
                    VoxelDensityProfile::EBucket::RoomColumnMod,
                    VoxelDensityProfile::EBucket::DomeMod,
                    VoxelDensityProfile::EBucket::PinchMod,
                    VoxelDensityProfile::EBucket::FloorBiasMod,
                    VoxelDensityProfile::EBucket::WormFieldSource,
                    VoxelDensityProfile::EBucket::OriginSpineOp,
                    VoxelDensityProfile::EBucket::BoundarySealOp,
                    VoxelDensityProfile::EBucket::PassageCarveOp,
                    VoxelDensityProfile::EBucket::XYEdgeSealOp,
                };
                for (const VoxelDensityProfile::EBucket Bucket : GameOpBuckets)
                {
                    const int32 Index = static_cast<int32>(Bucket);
                    const uint64 Calls = TileProfileEnd.Calls[Index] >= TileProfileStart.Calls[Index]
                        ? TileProfileEnd.Calls[Index] - TileProfileStart.Calls[Index] : 0;
                    const uint64 Cycles = TileProfileEnd.WallCycles[Index] >= TileProfileStart.WallCycles[Index]
                        ? TileProfileEnd.WallCycles[Index] - TileProfileStart.WallCycles[Index] : 0;
                    if (Calls == 0) { continue; }
                    UE_LOG(LogTemp, Display,
                        TEXT("[VoxelForgeGameOpProfile] tile=(%d,%d,%d) level=%d op=%s calls=%llu total_us=%.3f us_per_call=%.6f"),
                        Tile.Coord.X, Tile.Coord.Y, Tile.Coord.Z, Tile.Level,
                        VoxelDensityProfile::BucketName(Bucket),
                        static_cast<unsigned long long>(Calls),
                        FPlatformTime::ToSeconds64(Cycles) * 1.0e6,
                        FPlatformTime::ToSeconds64(Cycles) * 1.0e6 / static_cast<double>(Calls));
                }
            }
        }
    };

    Result.Tile  = Tile;
    Result.Epoch = Epoch;
    Result.bAborted = false;
    Result.bObsolete = false;
    Result.bEmpty = true;
    Result.ClassifyVerdict = -1;
    Result.ClassifySeconds = 0.0;
    Result.MeshSeconds = 0.0;
    Result.StreamSeconds = 0.0;
    Result.NumTriangles = 0;
    Result.ValidationDensityCalls = 0;
    Result.MesherDensityCalls = 0;
    Result.Streams.Reset();
    Result.CaptureGrid.Reset();
    Result.BandChunkLo = BandChunkLo;   // strate content cut (MIN/MAX = uncut)
    Result.BandChunkHi = BandChunkHi;

    // Shared by async workers and the synchronous carve path. An interrupted result is empty but
    // explicitly discarded, never an all-air tile that gets marked loaded.
    auto AbortResult = [&]()
    {
        Result.bAborted = true;
        Result.bObsolete = ObsoleteFlag != nullptr
            && ObsoleteFlag->load(std::memory_order_relaxed);
        Result.bEmpty = true;
        Result.Streams.Reset();
        Result.CaptureGrid.Reset();
    };
    if (ShouldAbortWork(ObsoleteFlag))
    {
        AbortResult();
        EmitTileProfile();
        return;
    }

    // T1.d — TRIVIAL-TILE REJECT: ~84 % des tuiles générées sortaient vides (tout-roc /
    // tout-air) en payant quand même le pré-échantillonnage complet. Le classifieur prouve
    // (bornes exactes sur le treillis du mesher + gardes conservatives) qu'une tuile est
    // uniforme → on saute GenerateMesh, Result reste bEmpty. Mixed = génération normale.
    // Les tuiles à capture (density volume) génèrent toujours : le volume veut la grille
    // même pour les cellules uniformes, et ces tuiles sont rares (fenêtre d'ombre).
    // (Gate IsoLevel == 0 : les verdicts du classifieur supposent l'iso MC à zéro exactement.)
    bool bTrivialEmpty = false;
    const int32 OuterClassifierMode = FMath::Clamp(GVoxelForgeOuterClassifierMode, 0, 1);
    if (OuterClassifierMode != 0
        && !bSheetTile && !bWantCapture && Generator && Mesher && Mesher->IsoLevel == 0.0f)
    {
        TRACE_CPUPROFILER_EVENT_SCOPE(VoxelForge_ClassifyTile);
        INC_DWORD_STAT(STAT_VoxelForgeTilesClassified);
        const double ClassifyStartSeconds = bMeasureTile ? FPlatformTime::Seconds() : 0.0;
        const EVoxelTileClass Verdict = Generator->ClassifyTile(
            OriginVoxels, Step, Cells, bMeasureTile ? &ClassifierStats : nullptr);

        // ITEM 6 — the outer classifier is a proof candidate, not the final skip decision. The
        // mesher's block path validates candidates against the exact MC vertex lattice before it
        // skips anything; whole-tile verdicts need the same fence. Sample the core vertices
        // g=0..Cells (the halo is normal-only) with the mesher's LOD TLS, so a coarse tile is
        // checked against the field the mesher would actually consume.
        EVoxelTileClass ValidatedVerdict = Verdict;
        bool bValidationAborted = false;
        if (Verdict != EVoxelTileClass::Mixed)
        {
            const int32 ValidationStep = FMath::Max(1, Step);
            const int32 ValidationCells = FMath::Clamp(Cells, 2, CHUNK_SIZE);
            const int32 ValidationOctaveBias = (Mesher->LODOctaveDrop > 0 && ValidationStep > 1)
                ? Mesher->LODOctaveDrop
                    * static_cast<int32>(FMath::FloorLog2(static_cast<uint32>(ValidationStep)))
                : 0;
            TGuardValue<int32> ValidationOctaveBiasGuard(
                VoxelGenLOD::OctaveBias, ValidationOctaveBias);
            TGuardValue<int32> ValidationSampleStepGuard(
                VoxelGenLOD::SampleStep, ValidationStep);

            bool bExactUniform = true;
            for (int32 Z = 0; Z <= ValidationCells
                && bExactUniform && !bValidationAborted; ++Z)
            {
                if (ShouldAbortWork(ObsoleteFlag))
                {
                    bValidationAborted = true;
                    break;
                }
                for (int32 Y = 0; Y <= ValidationCells
                    && bExactUniform && !bValidationAborted; ++Y)
                {
                    for (int32 X = 0; X <= ValidationCells; ++X)
                    {
                        if (ShouldAbortWork(ObsoleteFlag))
                        {
                            bValidationAborted = true;
                            break;
                        }
                        const float Density = Generator->GetDensityAt(
                            OriginVoxels.X + X * ValidationStep,
                            OriginVoxels.Y + Y * ValidationStep,
                            OriginVoxels.Z + Z * ValidationStep);
                        ++Result.ValidationDensityCalls;
                        const bool bSampleAgrees = FMath::IsFinite(Density) && Density != 0.0f
                            && (Verdict != EVoxelTileClass::AllSolid || Density < 0.0f)
                            && (Verdict != EVoxelTileClass::AllAir || Density > 0.0f);
                        if (!bSampleAgrees)
                        {
                            bExactUniform = false;
                            break;
                        }
                    }
                }
            }
            TotalValidationDensityCalls.fetch_add(
                static_cast<uint64>(Result.ValidationDensityCalls), std::memory_order_relaxed);
            if (!bExactUniform)
            {
                // A false Mixed costs meshing; a false uniform verdict removes both render and
                // collision geometry. Fall back to the safe path whenever one exact vertex disagrees.
                ValidatedVerdict = EVoxelTileClass::Mixed;
            }
        }
        if (bValidationAborted)
        {
            AbortResult();
            EmitTileProfile();
            return;
        }
        ClassifySeconds = bMeasureTile ? FPlatformTime::Seconds() - ClassifyStartSeconds : 0.0;
        ClassifyVerdict = static_cast<int32>(ValidatedVerdict);
        Result.ClassifyVerdict = ClassifyVerdict;
        const int32 TrackedLOD = FMath::Clamp(Tile.Level, 0, TrackedClassifierLODCount - 1);
        OuterClassifierCallsByLOD[TrackedLOD].fetch_add(1, std::memory_order_relaxed);
        if (ClassifyVerdict >= 0 && ClassifyVerdict < 3)
        {
            OuterClassifierVerdictsByLOD[TrackedLOD][ClassifyVerdict].fetch_add(
                1, std::memory_order_relaxed);
        }
        if (ShouldAbortWork(ObsoleteFlag))
        {
            AbortResult();
            EmitTileProfile();
            return;
        }
        if (ValidatedVerdict == EVoxelTileClass::AllSolid)
        {
            INC_DWORD_STAT(STAT_VoxelForgeTilesSkippedAllSolid);
        }
        else if (ValidatedVerdict == EVoxelTileClass::AllAir)
        {
            INC_DWORD_STAT(STAT_VoxelForgeTilesSkippedAllAir);
        }
        bTrivialEmpty = (ValidatedVerdict != EVoxelTileClass::Mixed);
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
        const double MeshStartSeconds = bMeasureTile ? FPlatformTime::Seconds() : 0.0;
        MeshData = bSheetTile
            ? Mesher->GenerateSheetMesh(OriginVoxels, Step, Cells, SheetChunkZ,
                                        HoleMinX, HoleMinY, HoleMaxX, HoleMaxY)
            : Mesher->GenerateMesh(OriginVoxels, Step, Cells,
                                   bWantCapture ? &Result.CaptureGrid : nullptr,
                                   BandVoxLo, BandVoxHi,
                                   &Result.MesherDensityCalls);
        TotalMesherDensityCalls.fetch_add(
            static_cast<uint64>(Result.MesherDensityCalls), std::memory_order_relaxed);
        MeshSeconds = bMeasureTile ? FPlatformTime::Seconds() - MeshStartSeconds : 0.0;
        INC_DWORD_STAT(STAT_VoxelForgeTilesMeshed);
        if (ShouldAbortWork(ObsoleteFlag))
        {
            AbortResult();
            EmitTileProfile();
            return;
        }
    }

    // T1.f — build the RMC geometry buffers HERE (worker), not on the game thread. Empty/all-air
    // tiles carry no streams (Result.bEmpty stays true) → no component on apply.
    if (!MeshData.IsEmpty())
    {
        TRACE_CPUPROFILER_EVENT_SCOPE(VoxelForge_BuildStreams);
        const double StreamStartSeconds = bMeasureTile ? FPlatformTime::Seconds() : 0.0;
        Result.Streams = MakeShared<RealtimeMesh::FRealtimeMeshStreamSet>();
        VoxelDensityProfile::FScopedTimer RuntimeStreamTimer(
            VoxelDensityProfile::EBucket::RuntimeStreamBuilding);
        BuildTileStreamSet(*Result.Streams, MeshData);
        StreamSeconds = bMeasureTile ? FPlatformTime::Seconds() - StreamStartSeconds : 0.0;
        if (ShouldAbortWork(ObsoleteFlag))
        {
            AbortResult();
            EmitTileProfile();
            return;
        }
        Result.bEmpty = false;

        // F17 — the mesher classified every triangle semantically (sky-cap vs ground, per
        // vertex against the column's TerrainZ/CeilSurf) and packed them as two contiguous
        // polygroup runs. Here we only record which sections exist for the apply path.
        // (Replaces the whole-tile normal VOTE, which painted mixed coarse tiles — terrain
        // AND cap in one tile — entirely with the winner's material.)
        const int32 NumTris = MeshData.Triangles.Num() / 3;
        Result.NumTriangles = NumTris;
        Result.bHasCeilingTris = MeshData.NumCeilingTriangles > 0;
        Result.bHasGroundTris  = NumTris > MeshData.NumCeilingTriangles;
    }

    Result.ClassifySeconds = ClassifySeconds;
    Result.MeshSeconds = MeshSeconds;
    Result.StreamSeconds = StreamSeconds;
    EmitTileProfile();
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

URealtimeMeshComponent* AVoxelWorld::AcquireTileComponent(
    double* OutCreationSeconds,
    double* OutRegistrationSeconds,
    bool* bOutCreated)
{
    if (OutCreationSeconds != nullptr) { *OutCreationSeconds = 0.0; }
    if (OutRegistrationSeconds != nullptr) { *OutRegistrationSeconds = 0.0; }
    if (bOutCreated != nullptr) { *bOutCreated = false; }

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

    const bool bMeasureComponentPhases = OutCreationSeconds != nullptr
        || OutRegistrationSeconds != nullptr;
    const uint64 CreationStartCycles = bMeasureComponentPhases
        ? FPlatformTime::Cycles64() : 0;
    URealtimeMeshComponent* MeshComp = NewObject<URealtimeMeshComponent>(this);
    // Generated once, never moves → Static so RMC's cached static draw path + VSM shadow
    // caching apply (see the root SetMobility note in BeginPlay). Must be set before register.
    // Re-mesh on carve recreates the section-group proxy (RMC's Static path already does this),
    // which is fine for an infrequent action.
    MeshComp->SetMobility(EComponentMobility::Static);
    MeshComp->SetGenerateOverlapEvents(false);   // chunks use raycasts, not overlaps
    MeshComp->SetCanEverAffectNavigation(false);
    if (OutCreationSeconds != nullptr)
    {
        *OutCreationSeconds = FPlatformTime::ToSeconds64(
            FPlatformTime::Cycles64() - CreationStartCycles);
    }
    const uint64 RegistrationStartCycles = bMeasureComponentPhases
        ? FPlatformTime::Cycles64() : 0;
    MeshComp->RegisterComponent();
    MeshComp->AttachToComponent(GetRootComponent(), FAttachmentTransformRules::KeepRelativeTransform);
    if (OutRegistrationSeconds != nullptr)
    {
        *OutRegistrationSeconds = FPlatformTime::ToSeconds64(
            FPlatformTime::Cycles64() - RegistrationStartCycles);
    }
    if (bOutCreated != nullptr) { *bOutCreated = true; }
    return MeshComp;
}

void AVoxelWorld::ReleaseTileComponent(URealtimeMeshComponent* Comp)
{
    if (!IsValid(Comp)) { return; }

    // A pooled component may still have its previous BodySetup while RMC processes the asynchronous
    // RemoveSectionGroup collision update. Disable the component first so that stale geometry can
    // never act as a collision barrier for another tile while it is parked.
    Comp->SetCollisionEnabled(ECollisionEnabled::NoCollision);

    if (TileComponentPool.Num() >= MaxPooledTileComponents)
    {
        if (URealtimeMeshSimple* RTMesh = Comp->GetRealtimeMeshAs<URealtimeMeshSimple>())
        {
            UnbindRealtimeMeshCollisionEvent(RTMesh);
        }
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
    // RMC futures are not cancellable from the world side. Remove this tile's state first so a
    // late completion from the old component cannot resurrect readiness after pool reuse.
    // Generation work is different: mark it obsolete and let its normal aborted result drain so
    // PendingTiles cannot be removed on behalf of an older request after a replacement is queued.
    if (TSharedPtr<FVoxelTileCancellationState, ESPMode::ThreadSafe>* Cancellation =
            PendingTileCancellation.Find(Tile))
    {
        (*Cancellation)->bObsolete.store(true, std::memory_order_release);
    }
    CollisionReadyTiles.Remove(Tile);
    CollisionNotRequiredTiles.Remove(Tile);
    CollisionSolidTiles.Remove(Tile);
    PendingCollisionCooks.Remove(Tile);
    // Water + decorations are no longer tile-bound (water is one player-following ocean plane via
    // UpdateWater; decorations stream by distance via UpdateDecorations) — nothing to clear per tile.
    if (URealtimeMeshComponent** Comp = TileComponents.Find(Tile))
    {
        if (*Comp) { ReleaseTileComponent(*Comp); }   // T2.c — park, don't destroy
        TileComponents.Remove(Tile);
    }
    LoadedTiles.Remove(Tile);
    TransitionHold.Remove(Tile);   // couvre aussi le settled cull (qui ne tient pas la hold à jour)
}

bool AVoxelWorld::ApplyMeshToTile(FChunkResult& Result)
{
    TRACE_CPUPROFILER_EVENT_SCOPE(VoxelForge_ApplyMeshToChunk);

    const bool bProfileApply = GVoxelForgeProfileTileGeneration != 0;
    const uint64 ApplyStartCycles = bProfileApply ? FPlatformTime::Cycles64() : 0;

    // Streams are pre-built on the worker (T1.f) and guaranteed non-empty by the caller
    // (ProcessPendingChunks skips empty tiles). This path is game-thread-CHEAP: material lookup +
    // component get/create + the upload + per-section config. No per-vertex work here.
    const FVoxelTileKey& Tile = Result.Tile;
    RealtimeMesh::FRealtimeMeshStreamSet& Streams = *Result.Streams;
    const bool bHasGroundTris  = Result.bHasGroundTris;
    const bool bHasCeilingTris = Result.bHasCeilingTris;
    const bool bLevel0 = (Tile.Level == 0);

    // A new mesh submission replaces the previous collision body. Invalidate readiness before any
    // RMC mutation; the old completion future remains alive but its serial/map entry is gone.
    CollisionReadyTiles.Remove(Tile);
    CollisionNotRequiredTiles.Remove(Tile);
    CollisionSolidTiles.Remove(Tile);
    PendingCollisionCooks.Remove(Tile);

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
    const double MaterialSeconds = bProfileApply
        ? FPlatformTime::ToSeconds64(FPlatformTime::Cycles64() - ApplyStartCycles)
        : 0.0;

    // The geometry stream set was built on the worker (BuildTileStreamSet, T1.f); we just upload it.
    // Vertices are world-space; the component sits at the actor origin.

    // One component per tile — the clipmap keeps the total tile count low (~1-2k), so this is
    // cheap on the game thread (no batching needed). Collision + content are level-0 only.
    // T2.c: the component comes from the pool when one is parked (see AcquireTileComponent).
    URealtimeMeshComponent* MeshComp = TileComponents.FindRef(Tile);
    const bool bComponentAlreadyPresent = MeshComp != nullptr;
    const uint64 ComponentStartCycles = bProfileApply ? FPlatformTime::Cycles64() : 0;
    double ComponentCreationSeconds = 0.0;
    double ComponentRegistrationSeconds = 0.0;
    bool bComponentCreated = false;
    if (!MeshComp)
    {
        MeshComp = AcquireTileComponent(
            bProfileApply ? &ComponentCreationSeconds : nullptr,
            bProfileApply ? &ComponentRegistrationSeconds : nullptr,
            bProfileApply ? &bComponentCreated : nullptr);
        TileComponents.Add(Tile, MeshComp);
    }
    if (!IsValid(MeshComp))
    {
        TileComponents.Remove(Tile);
        if (bProfileApply)
        {
            UE_LOG(LogTemp, Warning,
                TEXT("[VoxelForgeApplyProfile] level=%d empty=0 failed_component_acquire=1 total=%.6f"),
                Tile.Level,
                FPlatformTime::ToSeconds64(FPlatformTime::Cycles64() - ApplyStartCycles));
        }
        return false;
    }
    const double ComponentSeconds = bProfileApply
        ? FPlatformTime::ToSeconds64(FPlatformTime::Cycles64() - ComponentStartCycles)
        : 0.0;

    // §9.4 RENDER-SKIP — a tile only a CollisionOnly anchor wants (not the player clipmap) cooks its
    // collision below but is hidden (no draw / VSM). Set every apply (overrides the pool's default-
    // visible state); ReconcileAnchorTileVisibility handles later flips on already-loaded tiles.
    MeshComp->SetVisibility(!CollisionOnlyTiles.Contains(Tile));
    MeshComp->SetCollisionEnabled(
        bLevel0 ? ECollisionEnabled::QueryAndPhysics : ECollisionEnabled::NoCollision);

    // Reuse the component's existing mesh object when it has one (pooled component or carve
    // re-mesh) — InitializeRealtimeMesh allocates a brand-new URealtimeMesh EVERY call, so
    // calling it unconditionally (as before) orphaned one mesh object per re-apply to the GC.
    // The RemoveSectionGroup below does the actual geometry clearing on reuse.
    URealtimeMeshSimple* RTMesh = MeshComp->GetRealtimeMeshAs<URealtimeMeshSimple>();
    const uint64 MeshInitStartCycles = bProfileApply ? FPlatformTime::Cycles64() : 0;
    if (!RTMesh) { RTMesh = MeshComp->InitializeRealtimeMesh<URealtimeMeshSimple>(); }
    const double MeshInitSeconds = bProfileApply
        ? FPlatformTime::ToSeconds64(FPlatformTime::Cycles64() - MeshInitStartCycles)
        : 0.0;
    if (!RTMesh)
    {
        if (bProfileApply)
        {
            UE_LOG(LogTemp, Warning,
                TEXT("[VoxelForgeApplyProfile] level=%d empty=0 failed_mesh_init=1 total=%.6f"),
                Tile.Level,
                FPlatformTime::ToSeconds64(FPlatformTime::Cycles64() - ApplyStartCycles));
        }
        TileComponents.Remove(Tile);
        ReleaseTileComponent(MeshComp);
        return false;
    }
    BindRealtimeMeshCollisionEvent(RTMesh);
    // Shadow casting: far (level >= 2) tiles never cast; the sky-cap SECTION never casts either
    // — otherwise the high rock ceiling shadows the entire terrain below it. F17: shadow is now
    // PER SECTION, so a mixed tile keeps its ground shadow while its cap stays shadowless.
    const bool bCastShadow = (Tile.Level <= 1);
    MeshComp->SetCastShadow(bCastShadow);

    const FRealtimeMeshSectionGroupKey GroupKey = VoxelTileGroupKey();
    const uint64 RemoveGroupStartCycles = bProfileApply ? FPlatformTime::Cycles64() : 0;
    RTMesh->RemoveSectionGroup(GroupKey);                    // clear old geometry on re-mesh
                                                             // (no-op on a fresh/pooled mesh)
    const double RemoveGroupSeconds = bProfileApply
        ? FPlatformTime::ToSeconds64(FPlatformTime::Cycles64() - RemoveGroupStartCycles)
        : 0.0;
    const uint64 MaterialSlotStartCycles = bProfileApply ? FPlatformTime::Cycles64() : 0;
    RTMesh->SetupMaterialSlot(0, "Main",   GroundMaterial);
    RTMesh->SetupMaterialSlot(1, "SkyCap", CeilingMaterial);
    const double MaterialSlotSeconds = bProfileApply
        ? FPlatformTime::ToSeconds64(FPlatformTime::Cycles64() - MaterialSlotStartCycles)
        : 0.0;
    // RMC owns both the CPU stream-to-buffer conversion and the render-resource upload behind
    // CreateSectionGroup. Keep this as one phase: it is the exact plugin/RMC boundary we can
    // budget without modifying RealtimeMeshComponent.
    const uint64 StreamUploadStartCycles = bProfileApply ? FPlatformTime::Cycles64() : 0;
    RTMesh->CreateSectionGroup(GroupKey, MoveTemp(Streams));
    const uint64 MeshSubmittedCycles = FPlatformTime::Cycles64();
    const double StreamUploadSeconds = bProfileApply
        ? FPlatformTime::ToSeconds64(FPlatformTime::Cycles64() - StreamUploadStartCycles)
        : 0.0;

    // RMC casts shadows PER SECTION (FRealtimeMeshSectionConfig::bCastsShadow, default true) — the
    // component-level UPrimitiveComponent::CastShadow is NOT honored by the RMC proxy, so the real
    // shadow lever is the section flag. RMC auto-created one section per non-empty polygroup above
    // (default config already maps material slot = polygroup index); only config sections that
    // exist — the bHas* flags come from the worker. Collision at level 0 only (T1.c), both groups.
    const uint64 CollisionConfigStartCycles = bProfileApply ? FPlatformTime::Cycles64() : 0;
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
    const double CollisionConfigSeconds = bProfileApply
        ? FPlatformTime::ToSeconds64(FPlatformTime::Cycles64() - CollisionConfigStartCycles)
        : 0.0;

    if (bLevel0)
    {
        // RMC's collision future is fulfilled only after its async cook has been applied to the
        // mesh BodySetup. This is the per-apply completion boundary; no submission-time guess is
        // promoted to CollisionReadyTiles.
        const uint64 SubmissionId = ++NextCollisionSubmissionId;
        const FRealtimeMeshCollisionConfiguration CollisionConfig = RTMesh->GetCollisionConfig();
        const uint64 CollisionSubmittedCycles = FPlatformTime::Cycles64();

        FPendingCollisionCook Pending;
        Pending.Mesh = RTMesh;
        Pending.SubmissionId = SubmissionId;
        Pending.MeshSubmittedCycles = MeshSubmittedCycles;
        Pending.CollisionSubmittedCycles = CollisionSubmittedCycles;
        Pending.RequestStartCycles = Result.RequestStartCycles;
        Pending.GenerationStartCycles = Result.GenerationStartCycles;
        Pending.GenerationEndCycles = Result.GenerationEndCycles;
        Pending.ApplyStartCycles = Result.ApplyStartCycles;
        Pending.bRecordStreamingLatency = Result.bRecordStreamingLatency;
        PendingCollisionCooks.Add(Tile, Pending);

        // Register the serial before asking RMC to dirty collision. The current implementation
        // defers the future to end-of-frame, but this ordering also keeps an already-fulfilled
        // future or an inline body-update event from racing the per-tile record.
        TFuture<ERealtimeMeshCollisionUpdateResult> CollisionFuture =
            RTMesh->SetCollisionConfig(CollisionConfig);

        TWeakObjectPtr<AVoxelWorld> WeakWorld(this);
        TWeakObjectPtr<URealtimeMesh> WeakMesh(RTMesh);
        CollisionFuture.Next(
            [WeakWorld, WeakMesh, Tile, SubmissionId](ERealtimeMeshCollisionUpdateResult CollisionResult)
            {
                if (AVoxelWorld* World = WeakWorld.Get())
                {
                    World->HandleTileCollisionCookComplete(
                        Tile, SubmissionId, WeakMesh.Get(), static_cast<uint8>(CollisionResult));
                }
            });
    }

    if (bProfileApply)
    {
        const double TotalSeconds = FPlatformTime::ToSeconds64(
            FPlatformTime::Cycles64() - ApplyStartCycles);
        UE_LOG(LogTemp, Display,
            TEXT("[VoxelForgeApplyProfile] level=%d empty=0 created=%d pooled=%d "
                 "material=%.6f component=%.6f component_create=%.6f "
                 "component_register=%.6f mesh_init=%.6f remove_group=%.6f "
                 "material_slots=%.6f stream_upload=%.6f create_section_group=%.6f "
                 "collision_config=%.6f total=%.6f"),
            Tile.Level,
            bComponentCreated ? 1 : 0,
            (!bComponentAlreadyPresent && !bComponentCreated) ? 1 : 0,
            MaterialSeconds,
            ComponentSeconds,
            ComponentCreationSeconds,
            ComponentRegistrationSeconds,
            MeshInitSeconds,
            RemoveGroupSeconds,
            MaterialSlotSeconds,
            StreamUploadSeconds,
            StreamUploadSeconds,
            CollisionConfigSeconds,
            TotalSeconds);
    }

    // Water is no longer spawned per tile — it's a single player-following ocean plane (UpdateWater,
    // driven from Tick), so it renders at every LOD and to the horizon with no per-tile gaps.
    return true;
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
    const bool bProfileModification = GVoxelForgeProfileTileGeneration != 0;
    const uint64 ModificationStartCycles = bProfileModification ? FPlatformTime::Cycles64() : 0;
    const uint64 DiffStartCycles = bProfileModification ? FPlatformTime::Cycles64() : 0;
    TArray<FIntVector> AffectedChunks = DiffLayer->ApplyModification(Modification);
    const double DiffSeconds = bProfileModification
        ? FPlatformTime::ToSeconds64(FPlatformTime::Cycles64() - DiffStartCycles)
        : 0.0;

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
        const uint64 SyncStartCycles = bProfileModification ? FPlatformTime::Cycles64() : 0;
        SyncRemeshTile(CenterTile);
        if (bProfileModification)
        {
            UE_LOG(LogTemp, Display,
                TEXT("[VoxelForgeModificationProfile] center_sync=%.6f"),
                FPlatformTime::ToSeconds64(FPlatformTime::Cycles64() - SyncStartCycles));
        }
        bSyncedCenter = true;
    }

    const uint64 QueueStartCycles = bProfileModification ? FPlatformTime::Cycles64() : 0;
    RemeshDirtyChunks(AffectedChunks, bSyncedCenter ? &CenterTile : nullptr);
    if (bProfileModification)
    {
        UE_LOG(LogTemp, Display,
            TEXT("[VoxelForgeModificationProfile] affected=%d diff=%.6f queue=%.6f total=%.6f "
                 "center_synced=%d"),
            AffectedChunks.Num(),
            DiffSeconds,
            FPlatformTime::ToSeconds64(FPlatformTime::Cycles64() - QueueStartCycles),
            FPlatformTime::ToSeconds64(FPlatformTime::Cycles64() - ModificationStartCycles),
            bSyncedCenter ? 1 : 0);
    }

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
    if (!Settings->Season.IsNull())
    {
        UE_LOG(LogTemp, Error,
            TEXT("[VoxelWorld] ChangeSeed rejected: a cooked Season owns the seed. Assign another season asset instead."));
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
    return Settings ? Settings->GetEffectiveWorldSeed() : 0;
}

int32 AVoxelWorld::GetCurrentSeason() const
{
    return Settings ? Settings->GetEffectiveSeasonNumber() : 0;
}

FString AVoxelWorld::GetCurrentSeasonContentHash() const
{
    return StrateManager ? StrateManager->GetSeasonContentHash() : FString();
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
