#include "VoxelWorld.h"
#include "VoxelChunkComponent.h"
#include "VoxelMacroTileComponent.h"
#include "VoxelMesher.h"
#include "VoxelStats.h"
#include "Kismet/GameplayStatics.h"
#include "ProceduralMeshComponent.h"
#include "RealtimeMeshComponent.h"
#include <RealtimeMeshSimple.h>
#include "VoxelGPUMesher.h"

AVoxelWorld::AVoxelWorld()
{
    RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
    PrimaryActorTick.bCanEverTick = true;
}

void AVoxelWorld::BeginPlay()
{
    Super::BeginPlay();

    // Optional prewarm for mesh component pool
    if (const UVoxelSettings* S = Settings.GetDefaultObject())
    {
        if (S->bEnableMeshComponentPooling && S->MeshComponentPoolPrewarm > 0)
        {
            TArray<UProceduralMeshComponent*> WarmPMCs;
            WarmPMCs.Reserve(S->MeshComponentPoolPrewarm);
            for (int32 i = 0; i < S->MeshComponentPoolPrewarm; ++i)
            {
                WarmPMCs.Add(AcquirePMC());
            }
            for (UProceduralMeshComponent* PMC : WarmPMCs)
            {
                ReleasePMC(PMC);
            }

            if (S->bUseRuntimeMeshComponent)
            {
                TArray<URealtimeMeshComponent*> WarmRMCs;
                WarmRMCs.Reserve(S->MeshComponentPoolPrewarm);
                for (int32 i = 0; i < S->MeshComponentPoolPrewarm; ++i)
                {
                    WarmRMCs.Add(AcquireRMC());
                }
                for (URealtimeMeshComponent* RMC : WarmRMCs)
                {
                    ReleaseRMC(RMC);
                }
            }
        }
    }

    UpdateChunks();
}

void AVoxelWorld::Tick(float DeltaTime)
{
    SCOPE_CYCLE_COUNTER(STAT_VoxelWorldTick);

    Super::Tick(DeltaTime);

    FVoxelGPUMesher::PumpAsyncReadbacks();

    DrainApplyQueue();
    PromoteReadyPendings();

    if (Settings.GetDefaultObject()->bLiveNoisePreview)
    {
        ++NoisePreviewFrameCounter;
        const int32 FramesPerReset = 10; // adjust for desired responsiveness

        if (NoisePreviewFrameCounter >= FramesPerReset)
        {
            // UNLOAD ALL ACTIVE CHUNKS
            for (auto& Pair : ActiveChunks)
            {
                if (Pair.Value)
                {
                    Pair.Value->CancelPendingTask();
                    Pair.Value->UnloadChunk();
                }
            }
            ActiveChunks.Empty();

            // UNLOAD ALL PENDING CHUNKS
            for (auto& Pair : PendingChunks)
            {
                if (Pair.Value)
                {
                    Pair.Value->CancelPendingTask();
                    Pair.Value->UnloadChunk();
                }
            }
            PendingChunks.Empty();
            UpdateChunks();
            NoisePreviewFrameCounter = 0;
        }
    }

    // Update stats
    SET_DWORD_STAT(STAT_VoxelActiveChunks, ActiveChunks.Num());
    SET_DWORD_STAT(STAT_VoxelPendingChunks, PendingChunks.Num());
    SET_DWORD_STAT(STAT_VoxelActiveMacroTiles, ActiveMacro.Num());
    SET_DWORD_STAT(STAT_VoxelGenTasksRunning, ActiveGenTasks);
    SET_DWORD_STAT(STAT_VoxelMeshTasksRunning, ActiveMeshTasks);

    {
        FScopeLock L1(&GenMutex);
        int32 GenQSize = 0;
        for (const auto& Pair : GenWaitByDistance)
            GenQSize += Pair.Value.Num();
        SET_DWORD_STAT(STAT_VoxelGenQueueSize, GenQSize);
    }

    {
        FScopeLock L2(&MeshMutex);
        int32 MeshQSize = 0;
        for (const auto& Pair : MeshWaitByDistance)
            MeshQSize += Pair.Value.Num();
        SET_DWORD_STAT(STAT_VoxelMeshQueueSize, MeshQSize);
    }

    {
        FScopeLock L3(&ApplyQueueMutex);
        SET_DWORD_STAT(STAT_VoxelApplyQueueSize, ApplyQueue.Num());
    }

    const AActor* Player = UGameplayStatics::GetPlayerPawn(this, 0);
    if (!Player) return;

    const FVector PlayerPos = Player->GetActorLocation();
    const FVoxelCoord Center = WorldToChunkCoord(PlayerPos);

    const bool bCenterChanged = (!bHasLastCenter || !(Center == LastCenterChunk));
    if (bCenterChanged || bNeedsMoreSpawning || !bInitialLoadComplete)
    {
        UpdateChunks();
        LastCenterChunk = Center;
        bHasLastCenter = true;
    }
}

void AVoxelWorld::EndPlay(const EEndPlayReason::Type Reason)
{
    bShuttingDown = true;

    { FScopeLock L(&ApplyQueueMutex); ApplyQueue.Reset(); }
    //{ FScopeLock L(&MeshMutex);       PendingMesh.Empty(); ActiveMeshTasks = 0; }

    Super::EndPlay(Reason);
}
/* ---------- Apply Queue (collision prioritized) ---------- */

void AVoxelWorld::DrainApplyQueue()
{
    SCOPE_CYCLE_COUNTER(STAT_VoxelDrainApplyQueue);

    TArray<FPendingApply> ToProcess;
    ToProcess.Reserve(MaxMeshAppliesPerTick);

    {
        FScopeLock Lock(&ApplyQueueMutex);
        if (ApplyQueue.Num() == 0) return;

        const int32 Budget = MaxMeshAppliesPerTick;

        // Collect items to process and mark for removal
        TArray<int32> ToRemoveIndices;
        ToRemoveIndices.Reserve(Budget);

        // Pass 1: collision items (priority)
        for (int32 i = 0; i < ApplyQueue.Num() && ToProcess.Num() < Budget; ++i)
        {
            if (ApplyQueue[i].bCollision)
            {
                ToProcess.Add(MoveTemp(ApplyQueue[i]));
                ToRemoveIndices.Add(i);
            }
        }

        // Pass 2: fill remainder with non-collision
        for (int32 i = 0; i < ApplyQueue.Num() && ToProcess.Num() < Budget; ++i)
        {
            if (!ToRemoveIndices.Contains(i))
            {
                ToProcess.Add(MoveTemp(ApplyQueue[i]));
                ToRemoveIndices.Add(i);
            }
        }

        // Batch remove (highest index first to avoid shifts)
        ToRemoveIndices.Sort();
        for (int32 i = ToRemoveIndices.Num() - 1; i >= 0; --i)
        {
            ApplyQueue.RemoveAtSwap(ToRemoveIndices[i], 1, false);
        }
    }

    // Process outside lock
    for (FPendingApply& Item : ToProcess)
    {
        if (UVoxelChunkComponent* C = Item.Chunk.Get())
        {
            if (C->IsCancelPending()) continue;

            // Drop stale applies (out-of-order GPU results)
            if (Item.Sequence < C->GetLastAppliedSequence())
            {
                continue;
            }

            if (Item.OwnedBuffers)
            {
                C->MarkAppliedSequence(Item.Sequence);
                C->ApplyBuffersToMesh(*Item.OwnedBuffers, Item.bCollision);
                C->OnMeshApplied(MoveTemp(Item.OwnedBuffers), Item.bWasSeamRemesh);
            }
            else if (Item.bUseChunkCache)
            {
                if (const FMeshBuffers* Cached = C->GetCachedBuffers())
                {
                    C->MarkAppliedSequence(Item.Sequence);
                    C->ApplyBuffersToMesh(*Cached, Item.bCollision);
                    C->OnCollisionReapplied();
                }
            }
        }
    }
    FVoxelCoord C;
    while (!PendingSeamRemesh.IsEmpty())
    {
        PendingSeamRemesh.Dequeue(C);
        if (UVoxelChunkComponent* Ch = GetChunk(C))
        {
            Ch->bNeighborsCacheDirty = true;
            Ch->RequestRemesh();
        }
    }

}

void AVoxelWorld::PromoteReadyPendings()
{
    SCOPE_CYCLE_COUNTER(STAT_VoxelPromotePendings);

    // Chunks (LOD0/1)
    {
        TArray<FVoxelCoord> ToPromote;
        TArray<FVoxelCoord> ToDrop;

        for (const auto& Pair : PendingChunks)
        {
            const FVoxelCoord& C = Pair.Key;
            UVoxelChunkComponent* P = Pair.Value;
            if (!P) { ToDrop.Add(C); continue; }
            if (P->State == EVoxelChunkState::Ready) ToPromote.Add(C);
        }

        for (const FVoxelCoord& C : ToDrop) PendingChunks.Remove(C);

        for (const FVoxelCoord& C : ToPromote)
        {
            UVoxelChunkComponent* NewC = PendingChunks[C];
            UVoxelChunkComponent* OldC = ActiveChunks.FindRef(C);

            if (OldC)
            {
                OldC->CancelPendingTask();
                OldC->UnloadChunk();
            }
            ActiveChunks.Add(C, NewC);
            PendingChunks.Remove(C);

            OnChunkReady(C); // neighbor seam remesh after promotion
        }
    }

    // Macro-tiles (LOD2)
    {
        TArray<FIntPoint> ToPromote;
        TArray<FIntPoint> ToDrop;

        for (const auto& Pair : PendingMacro)
        {
            const FIntPoint& T = Pair.Key;
            UVoxelMacroTileComponent* P = Pair.Value;
            if (!P) { ToDrop.Add(T); continue; }
            if (P->State == EVoxelChunkState::Ready) ToPromote.Add(T);
        }

        for (const FIntPoint& T : ToDrop) PendingMacro.Remove(T);

        for (const FIntPoint& T : ToPromote)
        {
            UVoxelMacroTileComponent* NewT = PendingMacro[T];
            if (UVoxelMacroTileComponent* OldT = ActiveMacro.FindRef(T))
            {
                OldT->CancelPendingTask();
                OldT->UnloadChunk();
            }
            ActiveMacro.Add(T, NewT);
            PendingMacro.Remove(T);
        }
    }
}

/* ---------- Scheduling (concurrency limiting with priority queue) ---------- */

TWeakObjectPtr<UVoxelChunkComponent> AVoxelWorld::PopClosest(TMap<int32, TArray<TWeakObjectPtr<UVoxelChunkComponent>>>& QueueByDistance)
{
    // Keep popping until we find a valid chunk or the queues are empty
    while (QueueByDistance.Num() > 0)
    {
        // Find minimum distance key with non-empty bucket
        int32 MinDist = TNumericLimits<int32>::Max();
        for (const auto& Pair : QueueByDistance)
        {
            if (Pair.Value.Num() > 0 && Pair.Key < MinDist)
            {
                MinDist = Pair.Key;
            }
        }

        if (MinDist == TNumericLimits<int32>::Max())
        {
            return nullptr;
        }

        TArray<TWeakObjectPtr<UVoxelChunkComponent>>& Bucket = QueueByDistance[MinDist];

        // Pop invalid/stale entries until we find a valid one
        while (Bucket.Num() > 0)
        {
            TWeakObjectPtr<UVoxelChunkComponent> Candidate = Bucket.Pop(false);
            if (UVoxelChunkComponent* C = Candidate.Get())
            {
                // Found a live chunk
                if (Bucket.Num() == 0)
                {
                    QueueByDistance.Remove(MinDist);
                }
                return Candidate;
            }
        }

        // This bucket was empty or all entries were stale
        QueueByDistance.Remove(MinDist);
    }

    return nullptr;
}

void AVoxelWorld::ScheduleGeneration(UVoxelChunkComponent* Chunk)
{
    if (!Chunk) return;
    const UVoxelSettings* S = Settings.GetDefaultObject();
    const int32 MaxGen = (S->MaxConcurrentGenerationTasks > 1) ? S->MaxConcurrentGenerationTasks : 1;

    {
        FScopeLock L(&GenMutex);
        if (ActiveGenTasks < MaxGen)
        {
            ++ActiveGenTasks;
            Chunk->DoGeneration();
            return;
        }

        // Add to priority queue bucket
        GenWaitByDistance.FindOrAdd(Chunk->PriorityDist2).Add(Chunk);
    }
}

void AVoxelWorld::OnGenerationFinished(UVoxelChunkComponent* /*Chunk*/)
{
    const UVoxelSettings* S = Settings.GetDefaultObject();
    const int32 MaxGen = (S->MaxConcurrentGenerationTasks > 1) ? S->MaxConcurrentGenerationTasks : 1;

    TWeakObjectPtr<UVoxelChunkComponent> Next;
    {
        FScopeLock L(&GenMutex);
        ActiveGenTasks = FMath::Max(0, ActiveGenTasks - 1);
        if (ActiveGenTasks < MaxGen)
        {
            Next = PopClosest(GenWaitByDistance);
        }
        if (Next.IsValid())
        {
            ++ActiveGenTasks;
        }
    }
    if (UVoxelChunkComponent* N = Next.Get())
    {
        N->DoGeneration();
    }
}

void AVoxelWorld::ScheduleMeshing(UVoxelChunkComponent* Chunk, bool bSeamRemesh)
{
    if (bShuttingDown || !IsValid(Chunk)) return;
    Chunk->bSeamRemeshQueued = Chunk->bSeamRemeshQueued || bSeamRemesh;

    const UVoxelSettings* S = Settings.GetDefaultObject();
    const int32 MaxMesh = (S->MaxConcurrentMeshingTasks > 1) ? S->MaxConcurrentMeshingTasks : 1;

    {
        FScopeLock L(&MeshMutex);
        if (ActiveMeshTasks < MaxMesh)
        {
            ++ActiveMeshTasks;
            const bool PopFlag = Chunk->bSeamRemeshQueued;
            Chunk->bSeamRemeshQueued = false;
            Chunk->DoMeshing(PopFlag);
            return;
        }

        // Add to priority queue bucket
        MeshWaitByDistance.FindOrAdd(Chunk->PriorityDist2).Add(Chunk);
    }
}

void AVoxelWorld::OnMeshingFinished(UVoxelChunkComponent* /*Chunk*/)
{
    const UVoxelSettings* S = Settings.GetDefaultObject();
    const int32 MaxMesh = (S->MaxConcurrentMeshingTasks > 1) ? S->MaxConcurrentMeshingTasks : 1;

    TWeakObjectPtr<UVoxelChunkComponent> Next;
    bool bSeam = false;

    {
        FScopeLock L(&MeshMutex);
        ActiveMeshTasks = FMath::Max(0, ActiveMeshTasks - 1);
        if (ActiveMeshTasks < MaxMesh)
        {
            Next = PopClosest(MeshWaitByDistance);
        }
        if (Next.IsValid())
        {
            bSeam = Next->bSeamRemeshQueued;
            Next->bSeamRemeshQueued = false;
            ++ActiveMeshTasks;
        }
    }

    if (UVoxelChunkComponent* N = Next.Get())
    {
        N->DoMeshing(bSeam);
    }
}

/* ---------- Mesh component pool ---------- */

UProceduralMeshComponent* AVoxelWorld::AcquirePMC()
{
    UProceduralMeshComponent* PMC = nullptr;
    if (PMCPool.Num() > 0)
    {
        PMC = PMCPool.Pop(false);
    }
    else
    {
        PMC = NewObject<UProceduralMeshComponent>(this, UProceduralMeshComponent::StaticClass());
        PMC->RegisterComponent();
        PMC->AttachToComponent(RootComponent, FAttachmentTransformRules::KeepRelativeTransform);
    }

    PMC->SetVisibility(true, true);
    PMC->SetHiddenInGame(false, true);
    PMC->ClearAllMeshSections();
    PMC->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    return PMC;
}

void AVoxelWorld::ReleasePMC(UProceduralMeshComponent* PMC)
{
    if (!PMC) return;
    PMC->ClearAllMeshSections();
    PMC->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    PMC->SetHiddenInGame(true, true);
    PMC->SetVisibility(false, true);
    PMCPool.Add(PMC);
}

URealtimeMeshComponent* AVoxelWorld::AcquireRMC()
{
    URealtimeMeshComponent* RMC = nullptr;
    if (RMCPool.Num() > 0)
    {
        RMC = RMCPool.Pop(false);
    }
    else
    {
        RMC = NewObject<URealtimeMeshComponent>(this);
        RMC->RegisterComponent();
        RMC->AttachToComponent(RootComponent, FAttachmentTransformRules::KeepRelativeTransform);
    }

    // CRITICAL: Reset transform to origin
    RMC->SetRelativeLocation(FVector::ZeroVector);
    RMC->SetRelativeRotation(FRotator::ZeroRotator);
    RMC->SetRelativeScale3D(FVector::OneVector);

    RMC->SetVisibility(true, true);
    RMC->SetHiddenInGame(false, true);
    RMC->SetCollisionEnabled(ECollisionEnabled::NoCollision);

    // Clear any previous mesh data without recreating shared resources (avoids lock-destruction assert)
    if (URealtimeMesh* MeshAsset = RMC->GetRealtimeMesh())
    {
        MeshAsset->Reset(false);
    }

    return RMC;
}

void AVoxelWorld::ReleaseRMC(URealtimeMeshComponent* RMC)
{
    if (!RMC) return;

    // Clear mesh data before pooling (do not recreate shared resources while locked)
    if (URealtimeMesh* MeshAsset = RMC->GetRealtimeMesh())
    {
        MeshAsset->Reset(false);
    }

    RMC->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    RMC->SetHiddenInGame(true, true);
    RMC->SetVisibility(false, true);

    // Reset transform when returning to pool
    RMC->SetRelativeLocation(FVector::ZeroVector);

    RMCPool.Add(RMC);
}

/* ---------- World update (chunks + macro-tiles) ---------- */

FVoxelCoord AVoxelWorld::WorldToChunkCoord(const FVector& Location) const
{
    const UVoxelSettings* S = Settings.GetDefaultObject();
    const float VoxelUU = S->VoxelWorldScale;

    const float ChunkWorldSizeX = S->ChunkSizeX * VoxelUU;
    const float ChunkWorldSizeY = S->ChunkSizeY * VoxelUU;
    const float ChunkWorldSizeZ = S->ChunkSizeZ * VoxelUU;

    // Compute the chunk coordinate along each axis.  We use FloorToInt to
    // ensure negative locations map correctly to negative chunk indices.  The
    // Z coordinate is computed similarly to X/Y so that vertical chunks are
    // loaded above and below the player when ViewDistanceChunksZ > 0.
    const int32 Cx = FMath::FloorToInt(Location.X / ChunkWorldSizeX);
    const int32 Cy = FMath::FloorToInt(Location.Y / ChunkWorldSizeY);
    const int32 Cz = FMath::FloorToInt(Location.Z / ChunkWorldSizeZ);
    return FVoxelCoord(Cx, Cy, Cz);
}

EVoxelLODLevel AVoxelWorld::PickLOD(int32 Dist2, const UVoxelSettings* S) const
{
    const int32 R0 = S->LOD0_Radius;
    const int32 R1 = S->LOD1_Radius;
    const int32 R2 = S->LOD2_Radius;

    const int32 R0_2 = R0 * R0;
    const int32 R1_2 = R1 * R1;
    const int32 R2_2 = R2 * R2;

    if (Dist2 <= R0_2) return EVoxelLODLevel::LOD0;
    if (Dist2 <= R1_2) return EVoxelLODLevel::LOD1;
    if (Dist2 <= R2_2) return EVoxelLODLevel::LOD2;
    return EVoxelLODLevel::LOD2;
}

    // VoxelWorld.cpp - Modified UpdateChunks with progressive loading
void AVoxelWorld::UpdateChunks()
    {
        SCOPE_CYCLE_COUNTER(STAT_VoxelUpdateChunks);

        const UVoxelSettings* S = Settings.GetDefaultObject();
        if (!S) return;

        const AActor* Player = UGameplayStatics::GetPlayerPawn(this, 0);
        if (!Player) return;

        const FVector PlayerLocation = Player->GetActorLocation();
        const FVoxelCoord CenterChunk = WorldToChunkCoord(PlayerLocation);

        const int32 R2 = (S->LOD2_Radius > 0) ? S->LOD2_Radius : S->ViewDistanceChunks;
        const int32 R2Sq = R2 * R2;
        const int32 Rz = FMath::Max(0, S->ViewDistanceChunksZ);

        static TArray<TPair<FVoxelCoord, int32>> Desired;
        static TSet<FVoxelCoord> Visible;

        Desired.Reset(0);
        Visible.Reset();

        const int32 EstimatedSize = (2 * R2 + 1) * (2 * R2 + 1) * (2 * Rz + 1);
        Desired.Reserve(EstimatedSize);
        Visible.Reserve(EstimatedSize);

        // Build desired list around the player's current vertical chunk
        const int32 VerticalCenter = CenterChunk.Cz;

        for (int32 dx = -R2; dx <= R2; ++dx)
        {
            for (int32 dy = -R2; dy <= R2; ++dy)
            {
                for (int32 dz = -Rz; dz <= Rz; ++dz)
                {
                    const int32 d2 = dx * dx + dy * dy + dz * dz;
                    if (!S->bDiskShapedLoading || d2 <= R2Sq)
                    {
                        Desired.Emplace(FVoxelCoord(CenterChunk.Cx + dx, CenterChunk.Cy + dy, VerticalCenter + dz), d2);
                    }
                }
            }
        }

        // Sort by distance - closest first, then prefer same-height (small |dz|), then smaller |dx|+|dy|
        const int32 Cxc = CenterChunk.Cx;
        const int32 Cyc = CenterChunk.Cy;
        const int32 Czc = CenterChunk.Cz;
        Desired.Sort([Cxc, Cyc, Czc](const TPair<FVoxelCoord, int32>& A, const TPair<FVoxelCoord, int32>& B)
            {
                if (A.Value != B.Value) return A.Value < B.Value;
                const int32 Az = FMath::Abs(A.Key.Cz - Czc);
                const int32 Bz = FMath::Abs(B.Key.Cz - Czc);
                if (Az != Bz) return Az < Bz;
                const int32 Axy = FMath::Abs(A.Key.Cx - Cxc) + FMath::Abs(A.Key.Cy - Cyc);
                const int32 Bxy = FMath::Abs(B.Key.Cx - Cxc) + FMath::Abs(B.Key.Cy - Cyc);
                return Axy < Bxy;
            });

        const int32 CollisionR = S->CollisionViewDistance;
        const int32 CollisionR2 = CollisionR * CollisionR;
        const int32 AOR = S->AORadiusChunks;
        const int32 AOR2 = AOR * AOR;
        const int32 CollisionDropR2 = (CollisionR + 1) * (CollisionR + 1);

        TMap<FIntPoint, int32> VisibleTilesMinD2;

        // CRITICAL: Reset spawn counter each frame
        ChunksSpawnedThisFrame = 0;
        const int32 SpawnBudget = FMath::Max(1, S->MaxChunksSpawnPerFrame);
        bNeedsMoreSpawning = false;

        // Track missing chunks
        int32 MissingChunks = 0;

        // Process ALL desired chunks
        for (const auto& Pair : Desired)
        {
            const FVoxelCoord& C = Pair.Key;
            const int32 d2 = Pair.Value;
            Visible.Add(C);

            const EVoxelLODLevel DesiredLOD = PickLOD(d2, S);

            if (DesiredLOD == EVoxelLODLevel::LOD2 && S->bUseLOD2MacroTiles)
            {
                if (C.Cz == CenterChunk.Cz)
                {
                    const int32 M = (S->LOD2_MacroTileSize > 2) ? S->LOD2_MacroTileSize : 2;
                    const int32 Tx = FloorDiv(C.Cx, M);
                    const int32 Ty = FloorDiv(C.Cy, M);
                    const FIntPoint Tile(Tx, Ty);

                    int32& MinD2 = VisibleTilesMinD2.FindOrAdd(Tile, d2);
                    MinD2 = FMath::Min(MinD2, d2);
                    continue;
                }
            }

            const bool bDesiredCollision0 = (d2 <= CollisionR2);
            const bool bDesiredAO = (d2 <= AOR2);

            UVoxelChunkComponent* Active = ActiveChunks.FindRef(C);
            UVoxelChunkComponent* Pending = PendingChunks.FindRef(C);

            if (Active)  Active->PriorityDist2 = d2;
            if (Pending) Pending->PriorityDist2 = d2;

            // NEW CHUNK: Progressive spawning with budget
            if (!Active && !Pending)
            {
                ++MissingChunks;

                // BUDGET: Only spawn up to SpawnBudget new chunks per frame
                if (ChunksSpawnedThisFrame >= SpawnBudget)
                {
                    bNeedsMoreSpawning = true;
                    continue; // Skip this chunk this frame, will spawn next frame
                }

                ++ChunksSpawnedThisFrame;

                UVoxelChunkComponent* Chunk = NewObject<UVoxelChunkComponent>(this);
                Chunk->RegisterComponent();
                AddInstanceComponent(Chunk);
                Chunk->PriorityDist2 = d2;

                Chunk->InitializeChunk(C, S, this, DesiredLOD, bDesiredCollision0, bDesiredAO);
                ActiveChunks.Add(C, Chunk);
                continue;
            }

            // EXISTING CHUNK: Always process updates
            if (Active)
            {
                bool bDesiredCollision = bDesiredCollision0;
                if (!bDesiredCollision && Active->bBuildCollision && d2 <= CollisionDropR2)
                {
                    bDesiredCollision = true;
                }

                if (Active->LOD == DesiredLOD)
                {
                    const bool bPolicyChanged =
                        (Active->bBuildCollision != bDesiredCollision) ||
                        (Active->bUseAO != bDesiredAO);

                    if (bPolicyChanged)
                    {
                        const bool AOChanged = (Active->bUseAO != bDesiredAO);
                        Active->bUseAO = bDesiredAO;

                        if (Active->bBuildCollision != bDesiredCollision)
                        {
                            Active->RequestCollisionReapply(bDesiredCollision);
                        }
                        if (AOChanged)
                        {
                            Active->RequestRemesh();
                        }
                    }
                }
                else
                {
                    if (!Pending)
                    {
                        UVoxelChunkComponent* NewP = NewObject<UVoxelChunkComponent>(this);
                        NewP->RegisterComponent();
                        AddInstanceComponent(NewP);
                        NewP->PriorityDist2 = d2;

                        NewP->InitializeChunk(C, S, this, DesiredLOD, bDesiredCollision0, bDesiredAO);
                        PendingChunks.Add(C, NewP);
                    }
                    else
                    {
                        Pending->bBuildCollision = bDesiredCollision0;
                        Pending->bUseAO = bDesiredAO;
                        Pending->PriorityDist2 = d2;
                    }
                }
            }
            else if (Pending)
            {
                Pending->bBuildCollision = bDesiredCollision0;
                Pending->bUseAO = bDesiredAO;
                Pending->PriorityDist2 = d2;
            }
        }

        if (MissingChunks > 0 && ChunksSpawnedThisFrame >= SpawnBudget)
        {
            bNeedsMoreSpawning = true;
        }
        else if (MissingChunks == 0)
        {
            bNeedsMoreSpawning = false;
        }

        // Track if initial load is complete
        if (!bInitialLoadComplete)
        {
            if (MissingChunks == 0)
            {
                bNeedsMoreSpawning = false;
                bInitialLoadComplete = true;
                UE_LOG(LogTemp, Warning, TEXT("=== INITIAL CHUNK LOAD COMPLETE ==="));
                UE_LOG(LogTemp, Warning, TEXT("Total chunks loaded: %d"), ActiveChunks.Num() + PendingChunks.Num());
            }
            else
            {
                // Log progress during initial load
                static int32 LogCounter = 0;
                if (++LogCounter >= 60) // Every 60 frames (~1 second)
                {
                    LogCounter = 0;
                    const int32 TotalNeeded = Desired.Num();
                    const int32 CurrentLoaded = ActiveChunks.Num() + PendingChunks.Num();
                    const float Progress = (float)CurrentLoaded / (float)TotalNeeded * 100.0f;
                    UE_LOG(LogTemp, Warning, TEXT("Loading chunks: %d/%d (%.1f%%) - Missing: %d, Spawned this frame: %d (Budget %d)"),
                        CurrentLoaded, TotalNeeded, Progress, MissingChunks, ChunksSpawnedThisFrame, SpawnBudget);
                }
            }
        }

        // ... rest of UpdateChunks (macro tiles, unloading) stays the same ...

        // Spawn/update macro-tiles
        if (S->bUseLOD2MacroTiles)
        {
            for (const TPair<FIntPoint, int32>& TPairMin : VisibleTilesMinD2)
            {
                const FIntPoint& Tile = TPairMin.Key;
                const int32 d2 = TPairMin.Value;

                UVoxelMacroTileComponent* ActiveT = ActiveMacro.FindRef(Tile);
                UVoxelMacroTileComponent* PendingT = PendingMacro.FindRef(Tile);

                if (ActiveT)  ActiveT->PriorityDist2 = d2;
                if (PendingT) PendingT->PriorityDist2 = d2;

                if (!ActiveT && !PendingT)
                {
                    auto* TileComp = NewObject<UVoxelMacroTileComponent>(this);
                    TileComp->RegisterComponent();
                    AddInstanceComponent(TileComp);
                    TileComp->PriorityDist2 = d2;

                    TileComp->InitializeMacroTile(Tile, S, this);
                    ActiveMacro.Add(Tile, TileComp);
                }
            }

            // Unload invisible macro-tiles
            TArray<FIntPoint> ToRemoveActive, ToRemovePending;
            for (const auto& Pair : ActiveMacro)
            {
                if (!VisibleTilesMinD2.Contains(Pair.Key))
                {
                    if (Pair.Value) Pair.Value->CancelPendingTask();
                    ToRemoveActive.Add(Pair.Key);
                }
            }
            for (const auto& Pair : PendingMacro)
            {
                if (!VisibleTilesMinD2.Contains(Pair.Key))
                {
                    if (Pair.Value) Pair.Value->CancelPendingTask();
                    ToRemovePending.Add(Pair.Key);
                }
            }

            for (const FIntPoint& T : ToRemoveActive)
            {
                if (UVoxelMacroTileComponent* C = ActiveMacro[T])
                {
                    C->CancelPendingTask();
                    C->UnloadChunk();
                }
                ActiveMacro.Remove(T);
            }
            for (const FIntPoint& T : ToRemovePending)
            {
                if (UVoxelMacroTileComponent* C = PendingMacro[T])
                {
                    C->CancelPendingTask();
                    C->UnloadChunk();
                }
                PendingMacro.Remove(T);
            }
        }

        // Unload invisible chunks
        TArray<FVoxelCoord> ToRemoveA, ToRemoveP;

        for (const auto& Pair : ActiveChunks)
        {
            if (!Visible.Contains(Pair.Key))
            {
                if (UVoxelChunkComponent* C = Pair.Value) { C->CancelPendingTask(); }
                ToRemoveA.Add(Pair.Key);
            }
        }
        for (const auto& Pair : PendingChunks)
        {
            if (!Visible.Contains(Pair.Key))
            {
                if (UVoxelChunkComponent* C = Pair.Value) { C->CancelPendingTask(); }
                ToRemoveP.Add(Pair.Key);
            }
        }

        for (const FVoxelCoord& C : ToRemoveA)
        {
            if (UVoxelChunkComponent* Chunk = ActiveChunks[C])
            {
                Chunk->CancelPendingTask();
                Chunk->UnloadChunk();
            }
            ActiveChunks.Remove(C);
        }
        for (const FVoxelCoord& C : ToRemoveP)
        {
            if (UVoxelChunkComponent* Chunk = PendingChunks[C])
            {
                Chunk->CancelPendingTask();
                Chunk->UnloadChunk();
            }
            PendingChunks.Remove(C);
        }
    }

UVoxelChunkComponent* AVoxelWorld::GetChunk(const FVoxelCoord& Coord) const
{
    if (UVoxelChunkComponent* const* Found = ActiveChunks.Find(Coord)) return *Found;
    return nullptr;
}

void AVoxelWorld::OnChunkReady(const FVoxelCoord& Coord)
{
    static const FVoxelCoord Nbh[6] = {
        {-1,0,0},{1,0,0},{0,-1,0},{0,1,0},{0,0,-1},{0,0,1} };
    for (const FVoxelCoord& D : Nbh)
    {
        if (UVoxelChunkComponent* N = GetChunk({ Coord.Cx + D.Cx, Coord.Cy + D.Cy, Coord.Cz + D.Cz }))
        {
            if (N->RenderMode == EVoxelRenderMode::Voxels)
            {
                // CRITICAL FIX: Mark ALL voxel neighbors as dirty, not just Ready ones.
                // This ensures chunks that are still generating/meshing will use the
                // correct border data from this chunk when they eventually mesh.
                N->bNeighborsCacheDirty = true;

                // If neighbor is already Ready, trigger an immediate seam remesh
                if (N->State == EVoxelChunkState::Ready)
                {
                    N->RequestRemesh();  // rebuild seam faces now
                }
            }
        }
    }
}

/* ---------- Apply queue APIs ---------- */

void AVoxelWorld::EnqueueMeshApply(UVoxelChunkComponent* Chunk, FMeshBuffers&& Buffers, bool bCreateCollision, bool bWasSeamRemesh, int32 Sequence)
{
    if (bShuttingDown || !IsValid(Chunk)) return;
    FPendingApply Item;
    Item.Chunk = Chunk;
    Item.OwnedBuffers = MakeUnique<FMeshBuffers>(MoveTemp(Buffers));
    Item.bUseChunkCache = false;
    Item.bCollision = bCreateCollision;
    Item.bWasSeamRemesh = bWasSeamRemesh;
    Item.Sequence = Sequence;
    FScopeLock Lock(&ApplyQueueMutex);
    ApplyQueue.Add(MoveTemp(Item));
}

void AVoxelWorld::EnqueueReapplyUsingCache(UVoxelChunkComponent* Chunk, bool bCreateCollision)
{
    FPendingApply Item;
    Item.Chunk = Chunk;
    Item.bUseChunkCache = true;
    Item.bCollision = bCreateCollision;
    Item.bWasSeamRemesh = true;

    FScopeLock Lock(&ApplyQueueMutex);
    ApplyQueue.Add(MoveTemp(Item));
}
void AVoxelWorld::MarkChunkForSeamRemesh(const FVoxelCoord& C)
{
    PendingSeamRemesh.Enqueue(C);
}
