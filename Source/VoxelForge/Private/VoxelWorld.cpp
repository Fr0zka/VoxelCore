// VoxelWorld.cpp
// Implementation of the voxel world manager

#include "VoxelWorld.h"
#include "VoxelDiffLayer.h"
#include "RealtimeMeshComponent.h"
#include "RealtimeMeshSimple.h"
#include "VoxelMarchingCubesMesher.h"
#include "VoxelStrateDefinition.h"
#include "VoxelTerrainOpDefinition.h"
#include "VoxelContentManager.h"
#include "VoxelAtmosphereManager.h"
#include "DrawDebugHelpers.h"

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

    // Collect all loaded chunk coords
    TArray<FIntVector> AllCoords;
    ChunkMeshes.GetKeys(AllCoords);

    // Unload every chunk (destroys mesh components + data)
    for (const FIntVector& Coord : AllCoords)
    {
        UnloadChunk(Coord);
    }

    // Clear pending set — stale tasks will be discarded by epoch check
    PendingChunkCoord.Empty();

    // Reset streaming state so the next Tick rebuilds the desired set and reloads.
    LastUpdateCenter = FIntVector(INT32_MAX, INT32_MAX, INT32_MAX);
    bAllChunksLoaded = false;
    DesiredSorted.Reset();
    DesiredSet.Reset();

    // Tick will reload all chunks on the next frame with fresh params.
    UE_LOG(LogTemp, Log, TEXT("[VoxelWorld] RegenerateAllChunks (epoch %u): unloaded %d chunks"), GenerationEpoch, AllCoords.Num());
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
    PendingChunkCoord.Empty();

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

    // Générateur + mesher (UObjects légers)
    Generator = NewObject<UVoxelGenerator>(this);
    Mesher    = NewObject<UVoxelMarchingCubesMesher>(this);

    Generator->InitializeSettings(Settings);
    Mesher->SetGenerator(Generator);

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

    // Content manager — scatters decorations/actors + water per chunk as they stream in.
    ContentManager = NewObject<UVoxelContentManager>(this);
    ContentManager->Initialize(this, StrateManager, Settings->Seed);

    // Atmosphere manager — per-strate fog + ambient + persistent ceiling/floor layers.
    if (bManageAtmosphere && StrateManager)
    {
        AtmosphereManager = NewObject<UVoxelAtmosphereManager>(this);
        AtmosphereManager->Initialize(this, StrateManager);
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
    FVector PlayerLastPos = GetPlayerPosition();

    if ((PlayerLastPos != FVector::ZeroVector)) {
        UpdateChunksAroundPosition(PlayerLastPos);
        if (AtmosphereManager)
        {
            AtmosphereManager->UpdateForPlayer(PlayerLastPos);
        }
    }
    ProcessPendingChunks();

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
        PendingChunkCoord.Remove(DequeuedChunk.ChunkCoord);

        // Discard results from a previous generation epoch (stale).
        if (DequeuedChunk.Epoch != GenerationEpoch)
        {
            continue;
        }

        // Always register the chunk as loaded (even if empty — so we don't re-generate it).
        Chunks.Add(DequeuedChunk.ChunkCoord, DequeuedChunk.Chunk);
        ChunkLODs.Add(DequeuedChunk.ChunkCoord, DequeuedChunk.LODLevel);

        // Empty mesh = all air chunk — nothing to render, but still "loaded".
        if (DequeuedChunk.MeshData.IsEmpty())
        {
            continue;
        }

        // Apply mesh (GPU upload) — this is the expensive part we budget.
        ApplyMeshToChunk(DequeuedChunk.ChunkCoord, DequeuedChunk.MeshData);
        MeshesApplied++;

        if (MeshesApplied >= MaxApplies)
        {
            break;
        }
    }

}


void AVoxelWorld::UpdateChunksAroundPosition(const FVector& CenterPosition)
{
    //
    // TOOLS:
    // - WorldToChunkCoord(Position) - convert world pos to chunk coord
    // - Chunks.Contains(Coord) - check if chunk exists
    // - TArray<FIntVector> to build a list
    // - Chunks is a TMap you can iterate with: for (auto& Pair : Chunks)
    //   where Pair.Key is the coordinate and Pair.Value is the chunk
    const int32 ViewXY = Settings->ViewDistanceXY;
    const int32 ViewUp = Settings->ViewDistanceUp;
    const int32 ViewDown = Settings->ViewDistanceDown;
    const int32 MaxTasks = Settings ? Settings->MaxConcurrentTasks : 16;

    const FIntVector CenterChunk = WorldToChunkCoord(CenterPosition);
    CurrentCenterChunk = CenterChunk;

    //=========================================================================
    // ONLY rebuild the desired set when the player crosses a chunk boundary.
    // Standing still (or moving within a chunk) costs nothing here.
    //=========================================================================
    if (CenterChunk != LastUpdateCenter)
    {
        LastUpdateCenter = CenterChunk;
        bAllChunksLoaded = false;

        DesiredSorted.Reset();
        DesiredSet.Reset();

        for (int32 OffsetX = -ViewXY; OffsetX <= ViewXY; OffsetX++)
        for (int32 OffsetY = -ViewXY; OffsetY <= ViewXY; OffsetY++)
        for (int32 OffsetZ = -ViewDown; OffsetZ <= ViewUp; OffsetZ++)
        {
            const FIntVector C = CenterChunk + FIntVector(OffsetX, OffsetY, OffsetZ);
            if (IsChunkInRange(C, CenterChunk))
            {
                DesiredSorted.Add(C);
                DesiredSet.Add(C);
            }
        }

        // Nearest-first so the closest chunks stream in before distant ones.
        DesiredSorted.Sort([&CenterChunk](const FIntVector& A, const FIntVector& B)
        {
            const FIntVector DA = A - CenterChunk;
            const FIntVector DB = B - CenterChunk;
            return (DA.X * DA.X + DA.Y * DA.Y + DA.Z * DA.Z)
                 < (DB.X * DB.X + DB.Y * DB.Y + DB.Z * DB.Z);
        });

        // Cull pass — O(loaded) thanks to the TSet (was O(loaded × desired)).
        TArray<FIntVector> ChunksToRemove;
        for (const auto& Pair : Chunks)
        {
            if (!DesiredSet.Contains(Pair.Key))
            {
                ChunksToRemove.Add(Pair.Key);
            }
        }
        for (const FIntVector& C : ChunksToRemove)
        {
            UnloadChunk(C);
        }

        // LOD reconciliation for chunks that stayed in view is handled by the persistent
        // budgeted loop below — NOT as a one-shot here. Doing it once on the boundary-cross
        // frame stranded any chunk skipped by a full task budget at its stale LOD; the
        // persistent loop retries on later frames until every chunk sits at its target LOD.
    }

    //=========================================================================
    // Submit pending work (budgeted). Runs each frame until everything desired is
    // streamed in AT ITS TARGET LOD, then goes idle until the player moves again.
    // Handles BOTH missing chunks (load) and loaded-but-wrong-LOD chunks (hot-swap
    // re-mesh) in one persistent, budget-retried pass — so nothing is stranded.
    //=========================================================================
    if (!bAllChunksLoaded)
    {
        int32 Submitted = 0;
        for (const FIntVector& C : DesiredSorted)
        {
            // Budget full → stop submitting this frame. Pending > 0 keeps us out of the
            // idle state below, so we resume next frame (nearest-first, so closest first).
            if (PendingChunkCoord.Num() >= MaxTasks) break;
            if (PendingChunkCoord.Contains(C)) continue;  // already in flight

            bool bNeedsWork;
            if (Chunks.Contains(C))
            {
                // Loaded — re-mesh only if its LOD no longer matches the current center.
                // Hot-swap (LoadChunk, never unload-first): the old mesh stays visible
                // until the new one lands, so no hole/pop during the swap.
                const int32 DesiredLOD = GetLODForChunk(C, CurrentCenterChunk);
                const int32* CurrentLOD = ChunkLODs.Find(C);
                bNeedsWork = (CurrentLOD && *CurrentLOD != DesiredLOD);
            }
            else
            {
                bNeedsWork = true;  // not loaded yet
            }
            if (!bNeedsWork) continue;

            LoadChunk(C);
            ++Submitted;
        }

        // Idle only after a FULL scan submitted nothing and nothing is in flight — i.e.
        // every desired chunk is loaded at its target LOD. (We only break early when the
        // budget is full, which leaves Pending > 0, so this can't fire prematurely.)
        if (Submitted == 0 && PendingChunkCoord.Num() == 0)
        {
            bAllChunksLoaded = true;
        }
    }
}

void AVoxelWorld::LoadChunk(const FIntVector& ChunkCoord)
{
    if (PendingChunkCoord.Contains(ChunkCoord)) return;

    const int32 MaxTasks = Settings ? Settings->MaxConcurrentTasks : 16;
    if (PendingChunkCoord.Num() >= MaxTasks)
    {
        return;  // Budget plein — on attend qu'une tâche finisse
    }
    PendingChunkCoord.Add(ChunkCoord);

    const int32 LODLevel = GetLODForChunk(ChunkCoord, CurrentCenterChunk);
    const int32 Step = LODToStep(LODLevel);

    // Epoch capturé pour détecter les résultats périmés (après un RegenerateAll)
    const uint32 TaskEpoch = GenerationEpoch;

    ActiveTaskCount.fetch_add(1, std::memory_order_relaxed);

    UE::Tasks::Launch(TEXT("ChunkGen"), [this, ChunkCoord, Step, LODLevel, TaskEpoch]()
    {
        // RAII: décrément du compteur quel que soit le chemin de sortie
        struct FTaskGuard
        {
            std::atomic<int32>& Counter;
            ~FTaskGuard() { Counter.fetch_sub(1, std::memory_order_relaxed); }
        } Guard{ActiveTaskCount};

        if (bShuttingDown.load(std::memory_order_relaxed)) return;

        // Le mesher lit la densité directement depuis Generator — pas de
        // "generate chunk" préliminaire: le chunk est juste un wrapper de coord.
        const FVoxelChunk Chunk(ChunkCoord);

        FChunkResult Result;
        Result.ChunkCoord = ChunkCoord;
        Result.Chunk      = Chunk;
        Result.LODLevel   = LODLevel;
        Result.Epoch      = TaskEpoch;
        Result.MeshData   = Mesher->GenerateMesh(Chunk, Step);

        if (!bShuttingDown.load(std::memory_order_relaxed))
        {
            ProcessQueue.Enqueue(Result);
        }
    });
}

void AVoxelWorld::UnloadChunk(const FIntVector& ChunkCoord)
{
    // Destroy any spawned content (decorations + water) before the mesh goes.
    if (ContentManager)
    {
        ContentManager->ClearChunk(ChunkCoord);
    }

    if (ChunkMeshes.Contains(ChunkCoord)) {
        ChunkMeshes[ChunkCoord]->DestroyComponent();
        ChunkMeshes.Remove(ChunkCoord);
        Chunks.Remove(ChunkCoord);
        ChunkLODs.Remove(ChunkCoord);
    }
}

void AVoxelWorld::ApplyMeshToChunk(const FIntVector& ChunkCoord, const FVoxelMeshData& MeshData)
{
    //==========================================================================
    // STEP 1: EARLY EXIT IF NO MESH DATA
    //==========================================================================
    // If the chunk is all air (empty), there's nothing to render.
    // MeshData.IsEmpty() returns true if Vertices array has 0 elements.
    if (MeshData.IsEmpty())
    {
        return;
    }

    //==========================================================================
    // STEP 2: GET OR CREATE THE MESH COMPONENT
    //==========================================================================
    // Each chunk needs a URealtimeMeshComponent to be visible in the world.
    // We store these in the ChunkMeshes map, keyed by chunk coordinate.
    URealtimeMeshComponent* MeshComp = nullptr;

    // Check if we already have a mesh component for this chunk
    if (ChunkMeshes.Contains(ChunkCoord))
    {
        // Reuse existing component (chunk is being updated, not created)
        MeshComp = ChunkMeshes[ChunkCoord];
    }
    else
    {
        // Create a brand new mesh component for this chunk
        // NewObject<T>(Outer) creates a new UObject of type T
        // 'this' (AVoxelWorld) is the "outer" - it owns this component
        MeshComp = NewObject<URealtimeMeshComponent>(this);

        // RegisterComponent() tells Unreal "this component is ready to use"
        // Without this, the component won't tick, render, or do anything
        MeshComp->RegisterComponent();

        // Attach to our actor so it moves with us and inherits our transform
        MeshComp->AttachToComponent(GetRootComponent(), FAttachmentTransformRules::KeepRelativeTransform);

        // Store in our map so we can find it later
        ChunkMeshes.Add(ChunkCoord, MeshComp);
    }

    // Safety check
    if (!MeshComp)
    {
        return;
    }

    //==========================================================================
    // STEP 3: INITIALIZE THE REALTIMEMESH ASSET
    //==========================================================================
    // Component = "the renderer", MeshAsset = "the data"
    // InitializeRealtimeMesh creates or returns the asset
    URealtimeMeshSimple* RTMesh = MeshComp->InitializeRealtimeMesh<URealtimeMeshSimple>();
    if (!RTMesh)
    {
        return;
    }

    //==========================================================================
    // STEP 4: CREATE KEYS FOR THE MESH STRUCTURE
    //==========================================================================
    // RealtimeMesh hierarchy: Mesh -> LODs -> SectionGroups -> Sections
    //
    // LOD = Level of Detail (LOD0 = highest detail, closest to camera)
    // SectionGroup = A group of mesh sections (one per chunk)
    // Section = Actual triangle data with a material

    const FRealtimeMeshLODKey LOD0(0);  // Highest detail

    // GroupKey = identifies our section group
    const FRealtimeMeshSectionGroupKey GroupKey =
        FRealtimeMeshSectionGroupKey::Create(LOD0, FName("ChunkMesh"));

    // SectionKey = identifies the mesh section within group
    const FRealtimeMeshSectionKey SectionKey =
        FRealtimeMeshSectionKey::CreateForPolyGroup(GroupKey, 0);

    //==========================================================================
    // STEP 5: REMOVE OLD GEOMETRY
    //==========================================================================
    // Clear previous data before adding new (important for chunk updates)
    RTMesh->RemoveSectionGroup(GroupKey);

    //==========================================================================
    // STEP 6: CREATE THE MESH BUILDER
    //==========================================================================
    // StreamSet = container for vertex data, indices, normals, UVs, etc.
    // Builder = helper to easily add vertices and triangles
    //
    // Template: <IndexType, NormalType, UVType, NumUVChannels>
    RealtimeMesh::FRealtimeMeshStreamSet Streams;
    RealtimeMesh::TRealtimeMeshBuilderLocal<uint32, FPackedNormal, FVector2DHalf, 1> Builder(Streams);

    // Enable data streams we'll use
    Builder.EnableTangents();    // Pour le normal mapping
    Builder.EnableTexCoords();   // Pour les UVs
    Builder.EnablePolyGroups();  // Pour l'assignation du matériau

    //==========================================================================
    // STEP 7: ADD ALL VERTICES
    //==========================================================================

    const int32 NumVertices = MeshData.Vertices.Num();
    Builder.ReserveAdditionalVertices(NumVertices);  // Pre-allocate for speed

    for (int32 i = 0; i < NumVertices; i++)
    {
        const FVector3f Position = (FVector3f)MeshData.Vertices[i];
        auto Vertex = Builder.AddVertex(Position);

        if (MeshData.Normals.IsValidIndex(i))
        {
            const FVector3f Normal = (FVector3f)MeshData.Normals[i];
            const FVector3f Tangent = FVector3f(1, 0, 0);
            Vertex.SetNormalAndTangent(Normal, Tangent);
        }

        if (MeshData.UVs.IsValidIndex(i))
        {
            Vertex.SetTexCoord(0, (FVector2f)MeshData.UVs[i]);
        }
    }

    //==========================================================================
    // STEP 8: ADD ALL TRIANGLES
    //==========================================================================
    // Each triangle = 3 vertex indices
    // Our quads = 2 triangles = 6 indices each
    const int32 NumIndices = MeshData.Triangles.Num();
    Builder.ReserveAdditionalTriangles(NumIndices / 3);

    for (int32 i = 0; i < NumIndices; i += 3)
    {
        Builder.AddTriangle(
            (uint32)MeshData.Triangles[i],
            (uint32)MeshData.Triangles[i + 1],
            (uint32)MeshData.Triangles[i + 2],
            0  // PolyGroup 0 = material slot 0
        );
    }

    //==========================================================================
    // STEP 9: UPLOAD TO GPU
    //==========================================================================
    // MoveTemp = transfer ownership efficiently (no copy)
    RTMesh->CreateSectionGroup(GroupKey, MoveTemp(Streams));

    //==========================================================================
    // STEP 10: CONFIGURE SECTION
    //==========================================================================
    FRealtimeMeshSectionConfig SectionConfig(0);  // Material slot 0
    SectionConfig.bIsVisible = true;

    // Pick the material: use the strate's override if available, else the global default.
    UMaterialInterface* ChunkMaterial = Settings->VoxelMaterial;
    if (StrateManager)
    {
        if (UVoxelStrateDefinition* StrateDef = StrateManager->GetStrateForChunk(ChunkCoord))
        {
            if (StrateDef->OverrideMaterial)
            {
                ChunkMaterial = StrateDef->OverrideMaterial;
            }
        }
    }

    RTMesh->SetupMaterialSlot(0, "Main", ChunkMaterial);
    RTMesh->UpdateSectionConfig(SectionKey, SectionConfig, true);

    //==========================================================================
    // STEP 11: POPULATE CONTENT (decorations + water)
    //==========================================================================
    // Runs on the game thread (ProcessPendingChunks calls us here), so spawning
    // actors is safe. PopulateChunk clears any prior content for this coord first,
    // so re-meshing after a carve refreshes the scatter cleanly.
    if (ContentManager)
    {
        ContentManager->PopulateChunk(ChunkCoord, MeshData, ChunkLODs.FindRef(ChunkCoord));
    }
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

    for (const FIntVector& Coord : DirtyCoords)
    {
        // Only remesh chunks that are actually loaded
        if (!Chunks.Contains(Coord)) continue;

        // Skip if already queued for generation (avoid double-submit)
        if (PendingChunkCoord.Contains(Coord)) continue;

        // Check concurrent task budget
        const int32 MaxTasks = Settings ? Settings->MaxConcurrentTasks : 16;
        if (PendingChunkCoord.Num() >= MaxTasks) break;

        PendingChunkCoord.Add(Coord);

        // Use existing LOD for this chunk (it hasn't moved, just modified)
        const int32* CurrentLOD = ChunkLODs.Find(Coord);
        const int32 LODLevel = CurrentLOD ? *CurrentLOD : 0;
        const int32 Step = LODToStep(LODLevel);
        const uint32 TaskEpoch = GenerationEpoch;

        ActiveTaskCount.fetch_add(1, std::memory_order_relaxed);

        UE::Tasks::Launch(TEXT("ChunkRemesh"), [this, Coord, Step, LODLevel, TaskEpoch]()
        {
            struct FTaskGuard
            {
                std::atomic<int32>& Counter;
                ~FTaskGuard() { Counter.fetch_sub(1, std::memory_order_relaxed); }
            } Guard{ActiveTaskCount};

            if (bShuttingDown.load(std::memory_order_relaxed)) return;

            // Re-mesh: la densité inclut automatiquement le DiffLayer
            // (carves du joueur) puisque le générateur le consulte dans GetDensityAt.
            const FVoxelChunk Chunk(Coord);

            FChunkResult Result;
            Result.ChunkCoord = Coord;
            Result.Chunk      = Chunk;
            Result.LODLevel   = LODLevel;
            Result.Epoch      = TaskEpoch;
            Result.MeshData   = Mesher->GenerateMesh(Chunk, Step);

            if (!bShuttingDown.load(std::memory_order_relaxed))
            {
                ProcessQueue.Enqueue(Result);
            }
        });
    }

    UE_LOG(LogTemp, Verbose, TEXT("[VoxelWorld] RemeshDirtyChunks: %d coords, %d queued"),
        DirtyCoords.Num(), PendingChunkCoord.Num());
}
