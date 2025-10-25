#include "VoxelWorld.h"
#include "VoxelChunkComponent.h"
#include "VoxelMacroTileComponent.h"
#include "VoxelMesher.h"
#include "Kismet/GameplayStatics.h"
#include "ProceduralMeshComponent.h"

#if WITH_RUNTIME_MESHCOMPONENT
#include "RuntimeMeshComponent.h"
#endif
AVoxelWorld::AVoxelWorld()
{
    RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
    PrimaryActorTick.bCanEverTick = true;
}

void AVoxelWorld::BeginPlay()
{
    Super::BeginPlay();
    // Staggered prewarm for mesh component pool (prevents a massive BeginPlay spike)
    if (const UVoxelSettings* S = Settings.GetDefaultObject())
    {
        if (S->bEnableMeshComponentPooling && S->MeshComponentPoolPrewarm > 0)
        {
            const int32 Target = S->MeshComponentPoolPrewarm;
            const int32 PerTick = FMath::Clamp(MaxPMCsPrewarmPerTick, 1, 256);
            int32 Remaining = Target;

            // Pump a few per tick until we reach Target.
            GetWorldTimerManager().SetTimer(
                Handle,
                [this, &Remaining, PerTick]()
                {
                    const int32 ThisTick = FMath::Min(PerTick, Remaining);
                    for (int32 i = 0; i < ThisTick; ++i)
                    {
                        PMCPool.Add(AcquirePMC());
                    }
                       #if WITH_RUNTIME_MESHCOMPONENT
                    for (int32 i = 0; i < ThisTick; ++i)
                    {
                        RMCPool.Add(AcquireRMC());
                    }
                    #endif
                    Remaining -= ThisTick;

                    if (Remaining <= 0)
                    {
                        // Return to pool (hide, clear) once the prewarm finishes
                        for (UProceduralMeshComponent* PMC : PMCPool) { ReleasePMC(PMC); }
                            #if WITH_RUNTIME_MESHCOMPONENT
                            for (URuntimeMeshComponent* RMC : RMCPool) { ReleaseRMC(RMC); }
                            #endif
                        GetWorldTimerManager().ClearTimer(Handle); // stop
                    }
                },
                0.0f,   // delay
                true    // looping each tick
            );
        }
    }

    UpdateChunks(); // will now fill SpawnQueue instead of creating hundreds immediately
}


void AVoxelWorld::Tick(float DeltaTime)
{
    Super::Tick(DeltaTime);
    if (const AActor* P = UGameplayStatics::GetPlayerPawn(this, 0))
    {
        const FVector V = P->GetVelocity();
        const double Speed = V.Size();
        bFreezeFar = (Speed > Settings.GetDefaultObject()->FreezeSpeedThreshold) && (Settings.GetDefaultObject()->FreezeOuterLodRings > 0);
        FrozenFromRing = Settings.GetDefaultObject()->FreezeOuterLodRings;
    }
    else
    {
        bFreezeFar = false;
        FrozenFromRing = 0;
    }
    UpdateDynamicBudgets(DeltaTime);
    DrainApplyQueue();
    PromoteReadyPendings();
    DrainSpawnQueue();  // <-- new: spreads new chunk creation across frames

    const AActor* Player = UGameplayStatics::GetPlayerPawn(this, 0);
    if (!Player) return;

    const FVector PlayerPos = Player->GetActorLocation();
    const FVoxelCoord Center = WorldToChunkCoord(PlayerPos);

    if (!bHasLastCenter || !(Center == LastCenterChunk))
    {
        UpdateChunks();
        LastCenterChunk = Center;
        bHasLastCenter = true;
    }
}
bool AVoxelWorld::ShouldDeferFar(const FVoxelCoord& Center, const FVoxelCoord& C, int32 VisibleRadiusSq) const
{
    if (!bFreezeFar || FrozenFromRing <= 0) return false;
    const int32 dx = C.Cx - Center.Cx;
    const int32 dy = C.Cy - Center.Cy;
    const int32 d2 = dx * dx + dy * dy;
    const int32 R = (int32)FMath::FloorToInt(FMath::Sqrt((float)VisibleRadiusSq));
    const int32 ringFromEdge = R - (int32)FMath::FloorToInt(FMath::Sqrt((float)d2));
    return ringFromEdge < FrozenFromRing; // in the last N rings
}

void AVoxelWorld::UpdateDynamicBudgets(float DeltaTime)
{
    // Smooth the observed frame time
    const UVoxelSettings* S = Settings.GetDefaultObject();
    const double FrameMs = FMath::Max(DeltaTime * 1000.0, 0.01);
    SmoothedFrameMs = FMath::Lerp(SmoothedFrameMs, FrameMs, 0.10); // 10% EMA

    // Smooth per-system costs using your existing rolling stats if available
    // Fallback to simple EMAs fed from measured sections in OnGenerationFinished/OnMeshingFinished
    ExpAvgGenMs = FMath::Clamp(FMath::Lerp(ExpAvgGenMs, ExpAvgGenMs, 0.0), 0.05, 8.0);
    ExpAvgMeshMs = FMath::Clamp(FMath::Lerp(ExpAvgMeshMs, ExpAvgMeshMs, 0.0), 0.05, 8.0);

    // Available time windows this frame
    const double BudgetGenMs = S->TargetFrameMs * FMath::Clamp(S->BudgetForGen, 0.05f, 0.5f);
    const double BudgetMeshMs = S->TargetFrameMs * FMath::Clamp(S->BudgetForMesh, 0.05f, 0.5f);

    // If frametime is high, back off harder
    const double Backoff = (SmoothedFrameMs > S->BackoffFrameMs) ? 0.5 : 1.0;

    // Convert ms budgets to integer task counts using EMAs as cost hints
    auto ToTasks = [&](double budgetMs, double avgMs, int32 hardCap, int32 minCap)
        {
            if (avgMs <= 0.01) return minCap;
            const int32 byCost = (int32)FMath::Clamp(FMath::FloorToInt(budgetMs / avgMs), (int32)minCap, (int32)hardCap);
            const int32 byBackoff = (int32)FMath::FloorToInt(byCost * Backoff);
            return FMath::Clamp(byBackoff, minCap, hardCap);
        };

    const int32 MinGen = 1;
    const int32 MinMesh = 1;

    // Start from user-configured maxima to respect designer intent
    const int32 HardGen = FMath::Min(S->MaxGenHardCap, FMath::Max(1, S ? S->MaxConcurrentGenerationTasks : 2));
    const int32 HardMesh = FMath::Min(S->MaxMeshHardCap, FMath::Max(1, S ? S->MaxConcurrentMeshingTasks : 2));

    DynMaxGen = ToTasks(BudgetGenMs, ExpAvgGenMs, HardGen, MinGen);
    DynMaxMesh = ToTasks(BudgetMeshMs, ExpAvgMeshMs, HardMesh, MinMesh);
}

/* ---------- Apply Queue (collision prioritized) ---------- */

void AVoxelWorld::DrainApplyQueue()
{
    TRACE_CPUPROFILER_EVENT_SCOPE(Voxel_DrainApplyQueue);
    SCOPE_CYCLE_COUNTER(STAT_Voxel_DrainApplyQueue);

    // --- Adaptive budget based on rolling per-apply time ---
    // (Add the members shown below to your AVoxelWorld.h)
    const int32 UserCap = FMath::Clamp(MaxMeshAppliesPerTick, 1, 64);
    const double SafeMsPerF = TargetGTMsForApplies;                  // e.g. 6.0
    const double AvgMs = FMath::Max(AvgApplyMs, 0.10);          // avoid div by 0
    const int32 DynBudget = FMath::Clamp((int32)FMath::FloorToInt(SafeMsPerF / AvgMs), 1, 64);
    int32 Budget = FMath::Min(UserCap, DynBudget);

    const int32 CollisionBudget = 1; // hard cap: at most one collision apply per frame

    // Pull up to Budget items (prefer collision first up to CollisionBudget)
    TArray<FPendingApply> Local;
    {
        FScopeLock Lock(&ApplyQueueMutex);
        if (ApplyQueue.Num() == 0) return;

        int32 Taken = 0;
        int32 TakenCollision = 0;

        // Pass 1: collision (up to CollisionBudget)
        for (int32 i = 0; i < ApplyQueue.Num() && Taken < Budget && TakenCollision < CollisionBudget; ++i)
        {
            if (ApplyQueue[i].bCollision)
            {
                Local.Add(MoveTemp(ApplyQueue[i]));
                ApplyQueue.RemoveAt(i, 1, /*bAllowShrinking=*/false);
                --i; ++Taken; ++TakenCollision;
            }
        }

        // Pass 2: non-collision
        for (int32 i = 0; i < ApplyQueue.Num() && Taken < Budget; ++i)
        {
            if (!ApplyQueue[i].bCollision)
            {
                Local.Add(MoveTemp(ApplyQueue[i]));
                ApplyQueue.RemoveAt(i, 1, /*bAllowShrinking=*/false);
                --i; ++Taken;
            }
        }

        // Pass 3: anything (if still room)
        for (int32 i = 0; i < ApplyQueue.Num() && Taken < Budget; ++i)
        {
            Local.Add(MoveTemp(ApplyQueue[i]));
            ApplyQueue.RemoveAt(i, 1, /*bAllowShrinking=*/false);
            --i; ++Taken;
        }
    }

    // Apply on GT (we're in Tick)
    for (FPendingApply& Item : Local)
    {
        TRACE_CPUPROFILER_EVENT_SCOPE(Voxel_ApplyOne);
        SCOPE_CYCLE_COUNTER(STAT_Voxel_ApplyOne);

        const double t0 = FPlatformTime::Seconds();

        if (UVoxelChunkComponent* C = Item.Chunk.Get())
        {
            if (!C->IsCancelPending())
            {
                if (Item.OwnedBuffers)
                {
                    C->ApplyBuffersToMesh(*Item.OwnedBuffers, Item.bCollision);
                    C->OnMeshApplied(MoveTemp(Item.OwnedBuffers), Item.bWasSeamRemesh);
                }
                else if (Item.bUseChunkCache)
                {
                    if (const FMeshBuffers* Cached = C->GetCachedBuffers())
                    {
                        C->ApplyBuffersToMesh(*Cached, Item.bCollision);
                        C->OnCollisionReapplied();
                    }
                }
            }
        }

        // Update rolling average with the *actual apply cost*, not the selection cost.
        const double dtMs = (FPlatformTime::Seconds() - t0) * 1000.0;
        AvgApplyMs = FMath::Lerp(AvgApplyMs, dtMs, ApplyEMA);
    }
}



void AVoxelWorld::PromoteReadyPendings()
{
    TRACE_CPUPROFILER_EVENT_SCOPE(Voxel_PromotePendings);
    SCOPE_CYCLE_COUNTER(STAT_Voxel_Promote);
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

/* ---------- Scheduling (concurrency limiting) ---------- */

TWeakObjectPtr<UVoxelChunkComponent> AVoxelWorld::PopClosest(TArray<TWeakObjectPtr<UVoxelChunkComponent>>& List)
{
    int32 BestIdx = INDEX_NONE;
    int32 BestD2 = TNumericLimits<int32>::Max();

    for (int32 i = 0; i < List.Num(); ++i)
    {
        if (UVoxelChunkComponent* C = List[i].Get())
        {
            if (C->PriorityDist2 < BestD2)
            {
                BestD2 = C->PriorityDist2;
                BestIdx = i;
            }
        }
    }
    if (BestIdx == INDEX_NONE) return nullptr;

    TWeakObjectPtr<UVoxelChunkComponent> Out = List[BestIdx];
    List.RemoveAtSwap(BestIdx, 1, false);
    return Out;
}

void AVoxelWorld::ScheduleGeneration(UVoxelChunkComponent* Chunk)
{
    if (!Chunk) return;
    const UVoxelSettings* S = Settings.GetDefaultObject();
    const int32 MaxGen = GetMaxGenTasks();
    {
        FScopeLock L(&GenMutex);
        if (ActiveGenTasks < MaxGen)
        {
            ++ActiveGenTasks;
            Chunk->DoGeneration();
            return;
        }
        GenWait.Add(Chunk);
    }
}

void AVoxelWorld::OnGenerationFinished(UVoxelChunkComponent* /*Chunk*/)
{
    const UVoxelSettings* S = Settings.GetDefaultObject();
    const int32 MaxGen = GetMaxGenTasks();

    TWeakObjectPtr<UVoxelChunkComponent> Next;
    {
        FScopeLock L(&GenMutex);
        ActiveGenTasks = FMath::Max(0, ActiveGenTasks - 1);
        if (ActiveGenTasks < MaxGen && GenWait.Num() > 0)
        {
            Next = PopClosest(GenWait);
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
    if (!Chunk) return;
    Chunk->bSeamRemeshQueued = Chunk->bSeamRemeshQueued || bSeamRemesh;

    const UVoxelSettings* S = Settings.GetDefaultObject();
    const int32 MaxMesh = GetMaxMeshTasks();

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
        MeshWait.Add(Chunk);
    }
}

void AVoxelWorld::OnMeshingFinished(UVoxelChunkComponent* /*Chunk*/)
{
    const UVoxelSettings* S = Settings.GetDefaultObject();
    const int32 MaxMesh = GetMaxMeshTasks();

    TWeakObjectPtr<UVoxelChunkComponent> Next;
    bool bSeam = false;

    {
        FScopeLock L(&MeshMutex);
        ActiveMeshTasks = FMath::Max(0, ActiveMeshTasks - 1);
        if (ActiveMeshTasks < MaxMesh && MeshWait.Num() > 0)
        {
            Next = PopClosest(MeshWait);
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

#if WITH_RUNTIME_MESHCOMPONENT
URuntimeMeshComponent* AVoxelWorld::AcquireRMC()
{
    URuntimeMeshComponent* RMC = nullptr;
    if (RMCPool.Num() > 0)
    {
        RMC = RMCPool.Pop(false);
    }
    else
    {
        RMC = NewObject<URuntimeMeshComponent>(this);
        RMC->RegisterComponent();
        RMC->AttachToComponent(RootComponent, FAttachmentTransformRules::KeepRelativeTransform);
    }

    RMC->SetVisibility(true, true);
    RMC->SetHiddenInGame(false, true);
    RMC->ClearAllMeshSections();
    RMC->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    return RMC;
}

void AVoxelWorld::ReleaseRMC(URuntimeMeshComponent* RMC)
{
    if (!RMC) return;
    RMC->ClearAllMeshSections();
    RMC->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    RMC->SetHiddenInGame(true, true);
    RMC->SetVisibility(false, true);
    RMCPool.Add(RMC);
}
#endif

/* ---------- World update (chunks + macro-tiles) ---------- */

FVoxelCoord AVoxelWorld::WorldToChunkCoord(const FVector& Location) const
{
    const UVoxelSettings* S = Settings.GetDefaultObject();
    const float VoxelUU = S->VoxelWorldScale;

    const float ChunkWorldSizeX = S->ChunkSizeX * VoxelUU;
    const float ChunkWorldSizeY = S->ChunkSizeY * VoxelUU;

    return FVoxelCoord(
        FMath::FloorToInt(Location.X / ChunkWorldSizeX),
        FMath::FloorToInt(Location.Y / ChunkWorldSizeY),
        0
    );
}

EVoxelLODLevel AVoxelWorld::PickLOD(int32 Dist2, const UVoxelSettings* S) const
{
    const int32 R0 = S->LOD0_Radius;
    const int32 R1 = S->LOD1_Radius;
    const int32 R2 = (S->LOD2_Radius > 0) ? S->LOD2_Radius : S->ViewDistanceChunks;

    const int32 R0_2 = R0 * R0;
    const int32 R1_2 = R1 * R1;
    const int32 R2_2 = R2 * R2;

    if (Dist2 <= R0_2) return EVoxelLODLevel::LOD0;
    if (Dist2 <= R1_2) return EVoxelLODLevel::LOD1;
    if (Dist2 <= R2_2) return EVoxelLODLevel::LOD2;
    return EVoxelLODLevel::LOD2;
}

void AVoxelWorld::UpdateChunks()
{
    const UVoxelSettings* S = Settings.GetDefaultObject();
    if (!S) return;

    const AActor* Player = UGameplayStatics::GetPlayerPawn(this, 0);
    if (!Player) return;

    const FVector PlayerLocation = Player->GetActorLocation();
    const FVoxelCoord CenterChunk = WorldToChunkCoord(PlayerLocation);

    const int32 R2 = (S->LOD2_Radius > 0) ? S->LOD2_Radius : S->ViewDistanceChunks;
    const int32 R2Sq = R2 * R2;

    static TArray<TPair<FVoxelCoord, int32>> Desired;  Desired.Reset();
    static TSet<FVoxelCoord>                Visible;  Visible.Reset();

    Desired.Reserve((2 * R2 + 1) * (2 * R2 + 1));

    // Fill desired coords (disk if enabled)
    for (int32 dx = -R2; dx <= R2; ++dx)
        for (int32 dy = -R2; dy <= R2; ++dy)
        {
            const int32 d2 = dx * dx + dy * dy;
            if (!S->bDiskShapedLoading || d2 <= R2Sq)
            {
                Desired.Emplace(FVoxelCoord(CenterChunk.Cx + dx, CenterChunk.Cy + dy, 0), d2);
            }
        }

    Visible.Reserve(Desired.Num());

    const int32 CollisionR = S->CollisionViewDistance;
    const int32 CollisionR2 = CollisionR * CollisionR;
    const int32 AOR = S->AORadiusChunks;
    const int32 AOR2 = AOR * AOR;
    const int32 CollisionDropR2 = (CollisionR + 1) * (CollisionR + 1);

    // Track which macro-tiles are visible (if enabled) and their min dist^2 for scheduling
    TMap<FIntPoint, int32> VisibleTilesMinD2;

    for (const auto& Pair : Desired)
    {
        const FVoxelCoord& C = Pair.Key;
        const int32 d2 = Pair.Value;

        Visible.Add(C);

        const EVoxelLODLevel DesiredLOD = PickLOD(d2, S);

        // LOD2 macro-tile path
        if (DesiredLOD == EVoxelLODLevel::LOD2 && S->bUseLOD2MacroTiles)
        {
            const int32 M = FMath::Max(2, S->LOD2_MacroTileSize);
            const int32 Tx = FloorDiv(C.Cx, M);
            const int32 Ty = FloorDiv(C.Cy, M);
            const FIntPoint Tile(Tx, Ty);

            // If frozen, still mark as visible so it is not unloaded; just skip work
            int32& MinD2 = VisibleTilesMinD2.FindOrAdd(Tile, d2);
            MinD2 = FMath::Min(MinD2, d2);

            if (ShouldDeferFar(CenterChunk, C, R2Sq))
            {
                continue; // keep alive, but do no spawn/remesh work this frame
            }

            continue; // do NOT spawn a per-chunk LOD2; the tile will handle it
        }

        // LOD0/LOD1 path (same as before)
        const bool bDesiredCollision0 = (d2 <= CollisionR2);
        const bool bDesiredAO = (d2 <= AOR2);

        UVoxelChunkComponent* Active = ActiveChunks.FindRef(C);
        UVoxelChunkComponent* Pending = PendingChunks.FindRef(C);

        if (Active)  Active->PriorityDist2 = d2;
        if (Pending) Pending->PriorityDist2 = d2;

        if (!Active && !Pending)
        {
            FPendingSpawn P;
            P.Coord = C;
            P.Dist2 = d2;
            P.LOD = DesiredLOD;
            P.bCollision = bDesiredCollision0;
            P.bAO = bDesiredAO;
            SpawnQueue.Add(MoveTemp(P));
            continue; // actual creation happens in DrainSpawnQueue() with a per-frame cap
        }

        if (Active)
        {
            // Collision hysteresis (keep 1 chunk longer)
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
                continue;
            }

            // Tiles are always LOD2; no pending swap needed
        }

        // Unload macro-tiles that are no longer visible
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

    // Unload chunks that left visibility
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
    static const FVoxelCoord Neigh[4] = {
        FVoxelCoord(-1,0,0), FVoxelCoord(1,0,0),
        FVoxelCoord(0,-1,0), FVoxelCoord(0,1,0)
    };
    for (const FVoxelCoord& D : Neigh)
    {
        const FVoxelCoord N(Coord.Cx + D.Cx, Coord.Cy + D.Cy, Coord.Cz);
        if (UVoxelChunkComponent* Chunk = GetChunk(N))
        {
            if (Chunk->State == EVoxelChunkState::Ready && Chunk->RenderMode == EVoxelRenderMode::Voxels)
            {
                Chunk->RequestRemesh();
            }
        }
    }
}

/* ---------- Apply queue APIs ---------- */

void AVoxelWorld::EnqueueMeshApply(UVoxelChunkComponent* Chunk, FMeshBuffers&& Buffers, bool bCreateCollision, bool bWasSeamRemesh)
{
    FPendingApply Item;
    Item.Chunk = Chunk;
    Item.OwnedBuffers = MakeUnique<FMeshBuffers>(MoveTemp(Buffers));
    Item.bUseChunkCache = false;
    Item.bCollision = bCreateCollision;
    Item.bWasSeamRemesh = bWasSeamRemesh;

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
void AVoxelWorld::DrainSpawnQueue()
{
    int32 Budget = FMath::Clamp(MaxChunkSpawnsPerTick, 1, 256);
    int32 Spawned = 0;

    // Spawn closest first (lower Dist2 first)
    SpawnQueue.Sort([](const FPendingSpawn& A, const FPendingSpawn& B) { return A.Dist2 < B.Dist2; });

    while (Spawned < Budget && SpawnQueue.Num() > 0)
    {
        FPendingSpawn Item = SpawnQueue[0];
        SpawnQueue.RemoveAtSwap(0, 1, false);

        // Safety: if chunk already exists/pending, skip
        if (ActiveChunks.Contains(Item.Coord) || PendingChunks.Contains(Item.Coord))
        {
            continue;
        }

        // Create the chunk component now (GT)
        UVoxelChunkComponent* Chunk = NewObject<UVoxelChunkComponent>(this);
        Chunk->RegisterComponent();
        AddInstanceComponent(Chunk);
        Chunk->PriorityDist2 = Item.Dist2;

        Chunk->InitializeChunk(Item.Coord, Settings.GetDefaultObject(), this,
            Item.LOD, Item.bCollision, Item.bAO);

        ActiveChunks.Add(Item.Coord, Chunk);
        ++Spawned;
    }
}
const TArray<int32>& AVoxelWorld::GetSharedGridIB(int32 SamplesX, int32 SamplesY)
{
    TRACE_CPUPROFILER_EVENT_SCOPE(Voxel_SharedGridIB);

    // Lazy cache keyed by (SamplesX,SamplesY)
    static TMap<FIntPoint, TArray<int32>> Cache;

    const FIntPoint Key(SamplesX, SamplesY);
    if (TArray<int32>* Found = Cache.Find(Key))
    {
        return *Found;
    }

    TArray<int32> IB;
    IB.Reserve((SamplesX - 1) * (SamplesY - 1) * 6);

    // Same outward winding as voxel quads
    for (int32 y = 0; y < SamplesY - 1; ++y)
    {
        for (int32 x = 0; x < SamplesX - 1; ++x)
        {
            const int32 v00 = x + y * SamplesX;
            const int32 v10 = v00 + 1;
            const int32 v01 = v00 + SamplesX;
            const int32 v11 = v01 + 1;

            IB.Add(v00); IB.Add(v11); IB.Add(v10);
            IB.Add(v00); IB.Add(v01); IB.Add(v11);
        }
    }

    return Cache.Add(Key, MoveTemp(IB));
}
