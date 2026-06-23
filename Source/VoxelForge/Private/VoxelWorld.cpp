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
#include "VoxelAtmosphereManager.h"
#include "DrawDebugHelpers.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Modules/ModuleManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"   // Unreal Insights scopes (Perf 0)

AVoxelWorld::AVoxelWorld()
{
    PrimaryActorTick.bCanEverTick = true;
}

//=============================================================================
// LIVE EDIT — regenerate all chunks when params change in the Details panel
//=============================================================================

void AVoxelWorld::RegenerateAllChunks()
{
    // Bump the generation epoch so in-flight async tasks become stale.
    // ProcessPendingChunks will discard any result with an old epoch.
    GenerationEpoch++;

    // Tear down every tile component, then clear all tile state. Components are GC-safe via
    // actor ownership; destroying them here is immediate.
    const int32 Count = LoadedTiles.Num();
    for (auto& Pair : TileComponents) { if (Pair.Value) Pair.Value->DestroyComponent(); }
    TileComponents.Empty();
    LoadedTiles.Empty();

    // Decorations/water are keyed per level-0 chunk — clear them all.
    if (ContentManager) { ContentManager->ClearAll(); }

    // Clear pending set — stale tasks will be discarded by the epoch check.
    PendingTiles.Empty();
    // Tiles are already destroyed above — drop any deferred-teardown keys so the drain doesn't
    // try to UnloadTile coords that no longer exist.
    PendingUnload.Empty();

    // Reset streaming state so the next Tick rebuilds the desired set and reloads.
    LastUpdateCenter = FIntVector(INT32_MAX, INT32_MAX, INT32_MAX);
    bAllChunksLoaded = false;
    DesiredSorted.Reset();
    DesiredSet.Reset();

    // Tick will reload all tiles on the next frame with fresh params.
    UE_LOG(LogTemp, Log, TEXT("[VoxelWorld] RegenerateAllChunks (epoch %u): cleared %d tiles"), GenerationEpoch, Count);
}

void AVoxelWorld::RebuildStrates()
{
    if (StrateManager && Settings)
    {
        // Re-applies layout + inter-strate gap + passage/spine settings from VoxelSettings.
        StrateManager->Initialize(Settings, Settings->Seed);
    }
    if (AtmosphereManager) AtmosphereManager->Reset();
    if (ContentManager)    ContentManager->ClearAll();

    // Reload all chunks against the rebuilt strate data.
    RegenerateAllChunks();

    UE_LOG(LogTemp, Log, TEXT("[VoxelWorld] RebuildStrates: strate layout + passages rebuilt from settings."));
}

#if WITH_EDITOR
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
    if (StrateManager)
    {
        StrateManager->Initialize(Settings, Settings->Seed);
    }
    if (Generator)
    {
        Generator->InitializeSettings(Settings);
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

    // Stop + drain the decoration march tasks (they read the Generator) before UObject teardown.
    if (ContentManager)
    {
        ContentManager->NotifyShutdown();
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
    Generator = NewObject<UVoxelGenerator>(this);
    Mesher    = NewObject<UVoxelMarchingCubesMesher>(this);

    Generator->InitializeSettings(Settings);
    Mesher->SetGenerator(Generator);
    Mesher->bGenerateSkirts = Settings->bGenerateSkirts;
    Mesher->SkirtCells      = Settings->SkirtCells;

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
            // One strate-global ocean plane following the player (water at every LOD, to the horizon).
            { TRACE_CPUPROFILER_EVENT_SCOPE(VoxelForge_UpdateWater); ContentManager->UpdateWater(PlayerLastPos); }
        }
    }
    ProcessPendingChunks();
    ProcessUnloadQueue();

#if ENABLE_DRAW_DEBUG
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
            else if (P.bHasMidPoint)
            {
                DrawSeg(P.UpperPoint, P.MidPoint);
                DrawSeg(P.MidPoint, P.LowerPoint);
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

int32 AVoxelWorld::GetLODForChunk(const FIntVector& ChunkCoord, const FIntVector& CenterChunk) const
{
    // Chebyshev distance (max of absolute differences on each axis)
    // This gives a cubic LOD zone instead of spherical — simpler and
    // matches how chunks are loaded (cubic view distance).
    FIntVector Delta = ChunkCoord - CenterChunk;
    int32 Distance = FMath::Max3(
        FMath::Abs(Delta.X),
        FMath::Abs(Delta.Y),
        FMath::Abs(Delta.Z)
    );

    if (Distance <= Settings->LOD0Distance)
    {
        return 0;  // Full resolution
    }
    else if (Distance <= Settings->LOD1Distance)
    {
        return 1;  // Half resolution
    }
    else
    {
        return 2;  // Quarter resolution
    }
}

int32 AVoxelWorld::LODToStep(int32 LODLevel)
{
    // LOD0 → 1, LOD1 → 2, LOD2 → 4
    // Using bit shift: 1 << LODLevel
    return 1 << FMath::Clamp(LODLevel, 0, 2);
}

bool AVoxelWorld::IsChunkInRange(const FIntVector& ChunkCoord, const FIntVector& CenterChunk) const
{
    const int32 ViewXY = Settings->ViewDistanceXY;
    const int32 ViewUp = Settings->ViewDistanceUp;
    const int32 ViewDown = Settings->ViewDistanceDown;
    FIntVector Range = ChunkCoord - CenterChunk;

    if ((FMath::Abs(Range.X) <= ViewXY) and (FMath::Abs(Range.Y) <= ViewXY)) {
        if (Range.Z > 0)
        {
            if (FMath::Abs(Range.Z) <= ViewUp)
            {
                return true;
            }
        }
        else
        {
            if (FMath::Abs(Range.Z) <= ViewDown)
            {
                return true;
            }
        }
    }
    return false;
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

        // Discard results from a previous generation epoch (stale).
        if (DequeuedChunk.Epoch != GenerationEpoch)
        {
            continue;
        }

        // Mark the tile loaded (even if empty — so we don't re-submit it).
        LoadedTiles.Add(DequeuedChunk.Tile);

        // Empty mesh = all-air tile — nothing to render, but still "loaded".
        if (DequeuedChunk.MeshData.IsEmpty())
        {
            continue;
        }

        // Apply mesh (GPU upload) — this is the expensive part we budget.
        ApplyMeshToTile(DequeuedChunk.Tile, DequeuedChunk.MeshData);
        MeshesApplied++;

        if (MeshesApplied >= MaxApplies)
        {
            break;
        }
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
        if (DesiredSet.Contains(T))
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

void AVoxelWorld::BuildDesiredTiles(const FIntVector& Center)
{
    TRACE_CPUPROFILER_EVENT_SCOPE(VoxelForge_BuildDesiredTiles);
    DesiredSorted.Reset();
    DesiredSet.Reset();

    const int32 R        = Settings ? FMath::Max(1, Settings->ClipRadius) : 3;
    const int32 MaxLevel = Settings ? FMath::Clamp(Settings->MaxClipLevel, 0, 8) : 4;

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

        for (int32 dz = -R; dz <= R; ++dz)
        for (int32 dy = -R; dy <= R; ++dy)
        for (int32 dx = -R; dx <= R; ++dx)
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
            DesiredSet.Add(Key);
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

bool AVoxelWorld::IsTileInClipRange(const FVoxelTileKey& Tile, const FIntVector& Center) const
{
    // In range = the tile's centre falls within the OUTERMOST shell (level MaxLevel ± R). A
    // loaded-but-not-desired tile in range is mid-LOD-transition (wait for its replacement);
    // one out of range has left the view entirely (cull immediately).
    const int32 R        = Settings ? FMath::Max(1, Settings->ClipRadius) : 3;
    const int32 MaxLevel = Settings ? FMath::Clamp(Settings->MaxClipLevel, 0, 8) : 4;
    const int32 PowMax   = 1 << MaxLevel;
    const FIntVector CMax = VF_FloorDiv(Center, PowMax);

    const FVector CV = Tile.CenterCm() / VOXEL_SIZE;   // tile centre in voxels
    const int32 SizeMax = CHUNK_SIZE * PowMax;
    const FIntVector TMax(
        VF_FloorDiv(FMath::FloorToInt(CV.X), SizeMax),
        VF_FloorDiv(FMath::FloorToInt(CV.Y), SizeMax),
        VF_FloorDiv(FMath::FloorToInt(CV.Z), SizeMax));

    return FMath::Abs(TMax.X - CMax.X) <= R
        && FMath::Abs(TMax.Y - CMax.Y) <= R
        && FMath::Abs(TMax.Z - CMax.Z) <= R;
}

void AVoxelWorld::UpdateChunksAroundPosition(const FVector& CenterPosition)
{
    TRACE_CPUPROFILER_EVENT_SCOPE(VoxelForge_UpdateChunks);
    const int32 MaxTasks = Settings ? Settings->MaxConcurrentTasks : 16;

    const FIntVector CenterChunk = WorldToChunkCoord(CenterPosition);  // player's level-0 tile
    CurrentCenterChunk = CenterChunk;

    //=========================================================================
    // Rebuild the desired tile set only when the player crosses a level-0 tile boundary.
    //=========================================================================
    if (CenterChunk != LastUpdateCenter)
    {
        LastUpdateCenter = CenterChunk;
        bAllChunksLoaded = false;

        BuildDesiredTiles(CenterChunk);

        // Cull loaded tiles that are no longer desired — STRICT LOAD-BEFORE-UNLOAD so crossing a
        // shell boundary NEVER leaves a hole and we never drop a tile before its replacement exists:
        //  - out of clip range → left the view entirely, no replacement coming → cull now.
        //  - in range (mid-LOD-transition) → cull ONLY once EVERY desired tile that overlaps its
        //    footprint is loaded. A coarse tile is replaced by several finer tiles; the old
        //    center-owner check culled it as soon as the ONE tile over its centre was ready, so the
        //    not-yet-ready edges flashed a hole. Checking the whole covering set fixes that: the old
        //    tile stays at its current resolution until the better mesh is fully in, then drops.
        auto FootprintsOverlap = [](const FVoxelTileKey& A, const FVoxelTileKey& B) -> bool
        {
            const int32 ea = A.ExtentVoxels(), eb = B.ExtentVoxels();
            const FIntVector aMin = A.OriginVoxels(), bMin = B.OriginVoxels();
            return aMin.X < bMin.X + eb && bMin.X < aMin.X + ea
                && aMin.Y < bMin.Y + eb && bMin.Y < aMin.Y + ea
                && aMin.Z < bMin.Z + eb && bMin.Z < aMin.Z + ea;
        };
        // Only a desired tile that ISN'T loaded yet can block a cull (an old tile must stay until its
        // replacement is in). That set is small — just the few newly-needed tiles this crossing — so
        // build it ONCE and test each candidate against it (was: scan ALL of DesiredSorted per tile,
        // an O(loaded×desired) game-thread spike when fast movement turns many tiles non-desired).
        // "Every covering desired tile loaded" ⟺ "no unloaded desired tile overlaps T" — equivalent.
        TArray<FVoxelTileKey> DesiredPending;
        for (const FVoxelTileKey& D : DesiredSorted)
        {
            if (!LoadedTiles.Contains(D)) DesiredPending.Add(D);
        }
        auto ReplacementsReady = [&](const FVoxelTileKey& T) -> bool
        {
            for (const FVoxelTileKey& D : DesiredPending)
            {
                if (FootprintsOverlap(T, D)) return false;   // a covering tile isn't ready → keep T
            }
            return true;
        };

        // ReplacementsReady is O(DesiredPending); calling it for every loaded tile is O(loaded × pending),
        // which goes QUADRATIC exactly when streaming falls behind (sprinting → DesiredPending balloons) —
        // the measured 53 ms/cross CullTiles spike and a death spiral (behind → stall → further behind).
        // KEEPING a transition tile longer is always hole-safe (the conservative direction), so when pending
        // is large we skip the overlap test and just keep in-range transition tiles; the settled cull (once
        // bAllChunksLoaded) + ProcessUnloadQueue reclaim them when streaming catches up. Out-of-range tiles
        // still cull unconditionally (bounds memory). This caps the cull at O(loaded) and breaks the spiral.
        const bool bDoOverlapCull = DesiredPending.Num() <= 48;

        TArray<FVoxelTileKey> ToRemove;
        auto Consider = [&](const FVoxelTileKey& T)
        {
            if (DesiredSet.Contains(T)) return;
            // T comes from TileComponents keys then LoadedTiles-not-in-TileComponents → never twice.
            if (!IsTileInClipRange(T, CenterChunk)) { ToRemove.Add(T); return; }   // left the view → cull now
            if (bDoOverlapCull && ReplacementsReady(T)) ToRemove.Add(T);           // transition → cull when safe
        };
        {
            TRACE_CPUPROFILER_EVENT_SCOPE(VoxelForge_CullTiles);
            for (const auto& Pair : TileComponents) Consider(Pair.Key);
            for (const FVoxelTileKey& T : LoadedTiles) { if (!TileComponents.Contains(T)) Consider(T); }
            // Defer the actual teardown — ProcessUnloadQueue spreads it across frames so a fast
            // traversal's whole-shell cull doesn't destroy dozens of components + actors in one frame.
            for (const FVoxelTileKey& T : ToRemove) PendingUnload.Add(T);
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
        for (const FVoxelTileKey& T : DesiredSorted)
        {
            if (PendingTiles.Num() >= MaxTasks) break;
            if (PendingTiles.Contains(T)) continue;   // in flight
            if (LoadedTiles.Contains(T)) continue;    // already loaded (level is in the key — no LOD remesh)
            LoadTile(T);
            ++Submitted;
        }

        if (Submitted == 0 && PendingTiles.Num() == 0)
        {
            // Everything desired is loaded → load-before-unload is satisfied: drop the
            // deferred (in-range, not-desired) transition tiles now. No holes.
            for (const auto& Pair : TileComponents)
                if (!DesiredSet.Contains(Pair.Key)) PendingUnload.Add(Pair.Key);
            for (const FVoxelTileKey& T : LoadedTiles)
                if (!DesiredSet.Contains(T) && !TileComponents.Contains(T)) PendingUnload.Add(T);
            // Teardown is drained by ProcessUnloadQueue (budgeted) — single spike-free path.

            bAllChunksLoaded = true;
        }
    }
}

void AVoxelWorld::LoadTile(const FVoxelTileKey& Tile)
{
    if (PendingTiles.Contains(Tile)) return;

    const int32 MaxTasks = Settings ? Settings->MaxConcurrentTasks : 16;
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
    const int32 Cells   = (Tile.Level < FullRes)
        ? CHUNK_SIZE
        : (Settings ? FMath::Clamp(Settings->CoarseTileCells, 4, CHUNK_SIZE) : 16);
    const int32 Extent  = CHUNK_SIZE << Tile.Level;
    const int32 Step    = FMath::Max(1, Extent / Cells);
    const uint32 TaskEpoch = GenerationEpoch;

    ActiveTaskCount.fetch_add(1, std::memory_order_relaxed);

    // BackgroundNormal priority: gen runs on background workers that YIELD to foreground
    // (game/render-thread) tasks. Without this, raising MaxConcurrentTasks past the spare
    // core count saturates the scheduler and starves the frame (the "over 12 = lag" symptom).
    // At background priority the frame keeps its cores; gen just fills in around it.
    UE::Tasks::Launch(TEXT("ChunkGen"), [this, Tile, OriginVoxels, Step, Cells, TaskEpoch]()
    {
        // RAII: decrement the counter on every exit path.
        struct FTaskGuard
        {
            std::atomic<int32>& Counter;
            ~FTaskGuard() { Counter.fetch_sub(1, std::memory_order_relaxed); }
        } Guard{ActiveTaskCount};

        if (bShuttingDown.load(std::memory_order_relaxed)) return;

        FChunkResult Result;
        Result.Tile  = Tile;
        Result.Epoch = TaskEpoch;
        {
            TRACE_CPUPROFILER_EVENT_SCOPE(VoxelForge_GenerateMesh);
            Result.MeshData = Mesher->GenerateMesh(OriginVoxels, Step, Cells);
        }

        if (!bShuttingDown.load(std::memory_order_relaxed))
        {
            ProcessQueue.Enqueue(Result);
        }
    }, UE::Tasks::ETaskPriority::BackgroundNormal);
}

void AVoxelWorld::UnloadTile(const FVoxelTileKey& Tile)
{
    // Water + decorations are no longer tile-bound (water is one player-following ocean plane via
    // UpdateWater; decorations stream by distance via UpdateDecorations) — nothing to clear per tile.
    if (URealtimeMeshComponent** Comp = TileComponents.Find(Tile))
    {
        if (*Comp) { (*Comp)->DestroyComponent(); }
        TileComponents.Remove(Tile);
    }
    LoadedTiles.Remove(Tile);
    PendingTiles.Remove(Tile);
}

void AVoxelWorld::ApplyMeshToTile(const FVoxelTileKey& Tile, const FVoxelMeshData& MeshData)
{
    TRACE_CPUPROFILER_EVENT_SCOPE(VoxelForge_ApplyMeshToChunk);

    if (MeshData.IsEmpty())
    {
        // Became empty (e.g. fully carved) — drop any prior component for this tile.
        if (URealtimeMeshComponent** C = TileComponents.Find(Tile))
        {
            if (*C) { (*C)->DestroyComponent(); }
            TileComponents.Remove(Tile);
        }
        return;
    }

    const bool bLevel0 = (Tile.Level == 0);

    // SKY-CAP CEILING classification (computed once, used for BOTH shadow + material). A tile is a
    // ceiling tile if its centre sits in the upper half of the open span between the ground surface and
    // the cap (the tile grid keeps ceiling tiles separate from ground tiles — they're far apart in Z).
    // The oracle is O(1) and returns false for non-surface strates, so this is cheap at every level.
    bool bIsCeiling = false;
    if (Generator)
    {
        const int32 VoxelsPerTile = CHUNK_SIZE << Tile.Level;
        const FIntVector MinVoxel = Tile.Coord * VoxelsPerTile;
        const float CenterX = (float)MinVoxel.X + VoxelsPerTile * 0.5f;
        const float CenterY = (float)MinVoxel.Y + VoxelsPerTile * 0.5f;
        const float CenterZ = (float)MinVoxel.Z + VoxelsPerTile * 0.5f;
        const int32 CenterChunkZ = FMath::FloorToInt(CenterZ / (float)CHUNK_SIZE);

        float TerrainZ = 0.0f, CeilSurf = 0.0f;
        if (Generator->GetSurfaceHeightAt(CenterX, CenterY, CenterChunkZ, TerrainZ, CeilSurf))
        {
            const float Mid = (TerrainZ + CeilSurf) * 0.5f;
            bIsCeiling = (CenterZ > Mid);
        }
    }

    // Material: strate override (by the tile's min-corner chunk coord) else the global default. Ceiling
    // tiles take the strate's CeilingMaterial when set (the rocky "night sky" overhead reads flat/bright
    // otherwise, since it casts no shadow).
    UMaterialInterface* ChunkMaterial = Settings ? Settings->VoxelMaterial : nullptr;
    if (StrateManager)
    {
        const FIntVector ChunkCoord = Tile.Coord * (1 << Tile.Level);   // level-0-equivalent min corner
        if (UVoxelStrateDefinition* StrateDef = StrateManager->GetStrateForChunk(ChunkCoord))
        {
            if (StrateDef->OverrideMaterial) { ChunkMaterial = StrateDef->OverrideMaterial; }
            if (bIsCeiling && StrateDef->CeilingMaterial) { ChunkMaterial = StrateDef->CeilingMaterial; }
        }
    }

    // Build the geometry stream set. Vertices are world-space; the component sits at the actor origin.
    RealtimeMesh::FRealtimeMeshStreamSet Streams;
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

        const int32 NumIndices = MeshData.Triangles.Num();
        Builder.ReserveAdditionalTriangles(NumIndices / 3);
        for (int32 i = 0; i < NumIndices; i += 3)
        {
            Builder.AddTriangle((uint32)MeshData.Triangles[i],
                                (uint32)MeshData.Triangles[i + 1],
                                (uint32)MeshData.Triangles[i + 2], 0 /*poly group*/);
        }
    }

    // One component per tile — the clipmap keeps the total tile count low (~1-2k), so this is
    // cheap on the game thread (no batching needed). Collision + content are level-0 only.
    URealtimeMeshComponent* MeshComp = TileComponents.FindRef(Tile);
    if (!MeshComp)
    {
        MeshComp = NewObject<URealtimeMeshComponent>(this);
        // Generated once, never moves → Static so RMC's cached static draw path + VSM shadow
        // caching apply (see the root SetMobility note in BeginPlay). Must be set before register.
        // Re-mesh on carve recreates the section-group proxy (RMC's Static path already does this),
        // which is fine for an infrequent action.
        MeshComp->SetMobility(EComponentMobility::Static);
        MeshComp->SetGenerateOverlapEvents(false);   // chunks use raycasts, not overlaps
        MeshComp->SetCanEverAffectNavigation(false);
        MeshComp->RegisterComponent();
        MeshComp->AttachToComponent(GetRootComponent(), FAttachmentTransformRules::KeepRelativeTransform);
        TileComponents.Add(Tile, MeshComp);
    }

    URealtimeMeshSimple* RTMesh = MeshComp->InitializeRealtimeMesh<URealtimeMeshSimple>();
    if (!RTMesh) { return; }
    // Shadow casting: far (level >= 2) tiles never cast; the SurfaceWorld SKY-CAP CEILING never casts
    // either — otherwise the high rock ceiling shadows the entire terrain below it (one mesh, so we
    // can't split it). bIsCeiling was classified above via the O(1) oracle.
    const bool bCastShadow = (Tile.Level <= 1) && !bIsCeiling;
    MeshComp->SetCastShadow(bCastShadow);

    const FRealtimeMeshSectionGroupKey GroupKey =
        FRealtimeMeshSectionGroupKey::Create(FRealtimeMeshLODKey(0), FName("Tile"));
    RTMesh->RemoveSectionGroup(GroupKey);                    // clear old geometry on re-mesh
    RTMesh->SetupMaterialSlot(0, "Main", ChunkMaterial);
    RTMesh->CreateSectionGroup(GroupKey, MoveTemp(Streams));

    FRealtimeMeshSectionConfig SectionConfig(0);
    // RMC casts shadows PER SECTION (FRealtimeMeshSectionConfig::bCastsShadow, default true) — the
    // component-level UPrimitiveComponent::CastShadow is NOT honored by the RMC proxy. So the real
    // shadow lever is here: drive the section flag from the same decision (far tiles + sky-cap ceiling
    // → no cast). SetCastShadow above is kept only to keep the component flag consistent.
    SectionConfig.bCastsShadow = bCastShadow;
    RTMesh->UpdateSectionConfig(
        FRealtimeMeshSectionKey::CreateForPolyGroup(GroupKey, 0),
        SectionConfig, /*bShouldCreateCollision*/ bLevel0);  // collision at level 0 only (T1.c)

    // Water is no longer spawned per tile — it's a single player-following ocean plane (UpdateWater,
    // driven from Tick), so it renders at every LOD and to the horizon with no per-tile gaps.
}

//=============================================================================
// STRATE QUERIES
//=============================================================================

int32 AVoxelWorld::GetStrateAtPosition(FVector WorldPosition) const
{
    if (!StrateManager) return -1;

    // GetStrateIndex expects Unreal world units — it converts internally.
    return StrateManager->GetStrateIndex(WorldPosition.Z);
}

//=============================================================================
// TERRAIN MODIFICATION — player carving & filling
//=============================================================================

void AVoxelWorld::CarveAtPosition(FVector Position, float Radius, float Strength)
{
    if (!DiffLayer) return;

    // Convert world position (Unreal units) to voxel space.
    // VOXEL_SIZE = 25 in VoxelForge, so divide by it.
    const FVector VoxelPos = Position / VOXEL_SIZE;

    // Carve = negative strength (subtracts density → creates air)
    FVoxelModification Mod;
    Mod.Center = VoxelPos;
    Mod.Radius = Radius;
    Mod.Strength = -FMath::Abs(Strength);  // Force negative for carving

    TArray<FIntVector> AffectedChunks = DiffLayer->ApplyModification(Mod);
    RemeshDirtyChunks(AffectedChunks);
}

void AVoxelWorld::FillAtPosition(FVector Position, float Radius, float Strength)
{
    if (!DiffLayer) return;

    // Convert world position to voxel space
    const FVector VoxelPos = Position / VOXEL_SIZE;

    // Fill = positive strength (adds density → creates solid)
    FVoxelModification Mod;
    Mod.Center = VoxelPos;
    Mod.Radius = Radius;
    Mod.Strength = FMath::Abs(Strength);  // Force positive for filling

    TArray<FIntVector> AffectedChunks = DiffLayer->ApplyModification(Mod);
    RemeshDirtyChunks(AffectedChunks);
}

void AVoxelWorld::ApplyModification(const FVoxelModification& Modification)
{
    if (!DiffLayer) return;
    TArray<FIntVector> AffectedChunks = DiffLayer->ApplyModification(Modification);
    RemeshDirtyChunks(AffectedChunks);
}

void AVoxelWorld::CarveBox(FVector Position, FVector ExtentVoxels, float Strength)
{
    FVoxelModification Mod;
    Mod.Shape = EVoxelBrushShape::Box;
    Mod.Center = Position / VOXEL_SIZE;
    Mod.BoxExtent = ExtentVoxels;
    Mod.Radius = ExtentVoxels.GetMax();        // budget proxy
    Mod.Strength = -FMath::Abs(Strength);       // carve
    ApplyModification(Mod);
}

void AVoxelWorld::FillBox(FVector Position, FVector ExtentVoxels, float Strength)
{
    FVoxelModification Mod;
    Mod.Shape = EVoxelBrushShape::Box;
    Mod.Center = Position / VOXEL_SIZE;
    Mod.BoxExtent = ExtentVoxels;
    Mod.Radius = ExtentVoxels.GetMax();
    Mod.Strength = FMath::Abs(Strength);        // fill
    ApplyModification(Mod);
}

void AVoxelWorld::CarveCapsule(FVector WorldA, FVector WorldB, float RadiusVoxels, float Strength)
{
    FVoxelModification Mod;
    Mod.Shape = EVoxelBrushShape::Capsule;
    Mod.Center = WorldA / VOXEL_SIZE;
    Mod.CapsuleEnd = WorldB / VOXEL_SIZE;
    Mod.Radius = RadiusVoxels;
    Mod.Strength = -FMath::Abs(Strength);
    ApplyModification(Mod);
}

void AVoxelWorld::FillCapsule(FVector WorldA, FVector WorldB, float RadiusVoxels, float Strength)
{
    FVoxelModification Mod;
    Mod.Shape = EVoxelBrushShape::Capsule;
    Mod.Center = WorldA / VOXEL_SIZE;
    Mod.CapsuleEnd = WorldB / VOXEL_SIZE;
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

void AVoxelWorld::RemeshDirtyChunks(const TArray<FIntVector>& DirtyCoords)
{
    // For each affected chunk that's currently loaded, re-queue it for
    // async generation + meshing. The old mesh stays visible until the
    // new result arrives in ProcessPendingChunks, so no visual pop.
    //
    // Chunks that aren't loaded are ignored — when they eventually load
    // through normal streaming, they'll include the diff layer automatically.

    // Edits only affect LEVEL-0 tiles (collision + visible detail are full-res near the player;
    // coarse far tiles sample too sparsely to show small carves, and pick up the diff naturally
    // when they next stream). Re-queue the loaded level-0 tile for each dirty coord — LoadTile
    // re-runs gen (density includes the DiffLayer via GetDensityAt) and ProcessPendingChunks
    // updates the existing component in place (old mesh stays visible until then, no pop).
    const int32 MaxTasks = Settings ? Settings->MaxConcurrentTasks : 16;
    for (const FIntVector& Coord : DirtyCoords)
    {
        const FVoxelTileKey Tile(Coord, 0);
        if (!LoadedTiles.Contains(Tile)) continue;     // only re-mesh loaded full-res tiles
        if (PendingTiles.Contains(Tile)) continue;     // already queued
        if (PendingTiles.Num() >= MaxTasks) break;     // task budget
        LoadTile(Tile);
    }

    UE_LOG(LogTemp, Verbose, TEXT("[VoxelWorld] RemeshDirtyChunks: %d coords, %d pending"),
        DirtyCoords.Num(), PendingTiles.Num());
}
