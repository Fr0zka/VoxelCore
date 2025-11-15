#include "VoxelWorld.h"
#include "VoxelChunkComponent.h"
#include "VoxelMacroTileComponent.h"
#include "VoxelMesher.h"
#include "VoxelStats.h"
#include "VoxelGPUMesher.h"
#include "VoxelGPUGenerator.h"
#include "Kismet/GameplayStatics.h"
#include "ProceduralMeshComponent.h"
#include "RealtimeMeshComponent.h"
#include <RealtimeMeshSimple.h>

// ============================================================================
// LIFECYCLE
// ============================================================================

AVoxelWorld::AVoxelWorld()
{
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	PrimaryActorTick.bCanEverTick = true;
}

void AVoxelWorld::BeginPlay()
{
	Super::BeginPlay();

	// Prewarm mesh component pool (reduces allocation hitches during gameplay)
	if (const UVoxelSettings* S = Settings.GetDefaultObject())
	{
		if (S->bEnableMeshComponentPooling && S->MeshComponentPoolPrewarm > 0)
		{
			// Prewarm ProceduralMeshComponent pool
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

			// Prewarm RealtimeMeshComponent pool (if enabled)
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

	// Initial chunk loading
	UpdateChunks();
}

void AVoxelWorld::Tick(float DeltaTime)
{
	SCOPE_CYCLE_COUNTER(STAT_VoxelWorldTick);
	Super::Tick(DeltaTime);

	// Pump GPU async readbacks (decode GPU mesher results)
	FVoxelGPUMesher::PumpAsyncReadbacks();

	// Pump GPU generation jobs (decode GPU generation results)
	FVoxelGPUGenerator::TickGPUGenerationJobs();

	// Apply mesh updates to visual/collision components
	DrainApplyQueue();

	// Promote Ready chunks from Pending to Active
	PromoteReadyPendings();

	// Live noise preview: reload all chunks periodically
	if (Settings.GetDefaultObject()->bLiveNoisePreview)
	{
		++NoisePreviewFrameCounter;
		const int32 FramesPerReset = 10; // Adjust for desired responsiveness

		if (NoisePreviewFrameCounter >= FramesPerReset)
		{
			// Unload all active chunks
			for (auto& Pair : ActiveChunks)
			{
				if (Pair.Value)
				{
					Pair.Value->CancelPendingTask();
					Pair.Value->UnloadChunk();
				}
			}
			ActiveChunks.Empty();

			// Unload all pending chunks
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

	// OPTIMIZATION: Update stats less frequently (every 10 frames instead of every frame)
	// Stats are for profiling, don't need real-time precision
	static int32 StatsFrameCounter = 0;
	if (++StatsFrameCounter >= 10)
	{
		StatsFrameCounter = 0;

		SET_DWORD_STAT(STAT_VoxelActiveChunks, ActiveChunks.Num());
		SET_DWORD_STAT(STAT_VoxelPendingChunks, PendingChunks.Num());
		SET_DWORD_STAT(STAT_VoxelActiveMacroTiles, ActiveMacro.Num());
		SET_DWORD_STAT(STAT_VoxelGenTasksRunning, ActiveGenTasks);
		SET_DWORD_STAT(STAT_VoxelMeshTasksRunning, ActiveMeshTasks);

		{
			FScopeLock L1(&GenMutex);
			int32 GenQSize = 0;
			for (const auto& Pair : GenWaitByDistance)
			{
				GenQSize += Pair.Value.Num();
			}
			SET_DWORD_STAT(STAT_VoxelGenQueueSize, GenQSize);
		}

		{
			FScopeLock L2(&MeshMutex);
			int32 MeshQSize = 0;
			for (const auto& Pair : MeshWaitByDistance)
			{
				MeshQSize += Pair.Value.Num();
			}
			SET_DWORD_STAT(STAT_VoxelMeshQueueSize, MeshQSize);
		}

		{
			FScopeLock L3(&ApplyQueueMutex);
			SET_DWORD_STAT(STAT_VoxelApplyQueueSize, ApplyQueue.Num());
		}
	}

	// Update chunks based on player movement
	const AActor* Player = UGameplayStatics::GetPlayerPawn(this, 0);
	if (!Player)
	{
		return;
	}

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

	// Clear apply queue to prevent further processing
	{
		FScopeLock Lock(&ApplyQueueMutex);
		ApplyQueue.Reset();
	}

	Super::EndPlay(Reason);
}

// ============================================================================
// APPLY QUEUE (COLLISION PRIORITIZED)
// ============================================================================

void AVoxelWorld::DrainApplyQueue()
{
	SCOPE_CYCLE_COUNTER(STAT_VoxelDrainApplyQueue);

	TArray<FPendingApply> ToProcess;
	ToProcess.Reserve(MaxMeshAppliesPerTick);

	// Gather items to process (collision prioritized)
	{
		FScopeLock Lock(&ApplyQueueMutex);
		if (ApplyQueue.Num() == 0)
		{
			return;
		}

		const int32 Budget = MaxMeshAppliesPerTick;
		TArray<int32> ToRemoveIndices;
		ToRemoveIndices.Reserve(Budget);

		// Pass 1: Collision items (priority)
		for (int32 i = 0; i < ApplyQueue.Num() && ToProcess.Num() < Budget; ++i)
		{
			if (ApplyQueue[i].bCollision)
			{
				ToProcess.Add(MoveTemp(ApplyQueue[i]));
				ToRemoveIndices.Add(i);
			}
		}

		// Pass 2: Fill remainder with non-collision
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
		if (UVoxelChunkComponent* Chunk = Item.Chunk.Get())
		{
			if (Chunk->IsCancelPending())
			{
				continue;
			}

			// Drop stale applies (out-of-order GPU results)
			if (Item.Sequence < Chunk->GetLastAppliedSequence())
			{
				continue;
			}

			if (Item.OwnedBuffers)
			{
				// Fresh mesh or seam remesh
				Chunk->MarkAppliedSequence(Item.Sequence);
				Chunk->ApplyBuffersToMesh(*Item.OwnedBuffers, Item.bCollision);
				Chunk->OnMeshApplied(MoveTemp(Item.OwnedBuffers), Item.bWasSeamRemesh);
			}
			else if (Item.bUseChunkCache)
			{
				// Collision-only reapply using cached buffers
				if (const FMeshBuffers* Cached = Chunk->GetCachedBuffers())
				{
					Chunk->MarkAppliedSequence(Item.Sequence);
					Chunk->ApplyBuffersToMesh(*Cached, Item.bCollision);
					Chunk->OnCollisionReapplied();
				}
			}
		}
	}

	// Process pending seam remesh requests
	FVoxelCoord Coord;
	while (!PendingSeamRemesh.IsEmpty())
	{
		PendingSeamRemesh.Dequeue(Coord);
		if (UVoxelChunkComponent* Chunk = GetChunk(Coord))
		{
			Chunk->bNeighborsCacheDirty = true;
			Chunk->RequestRemesh();
		}
	}
}

void AVoxelWorld::PromoteReadyPendings()
{
	SCOPE_CYCLE_COUNTER(STAT_VoxelPromotePendings);

	// Promote chunks (LOD0/1)
	{
		TArray<FVoxelCoord> ToPromote;
		TArray<FVoxelCoord> ToDrop;

		for (const auto& Pair : PendingChunks)
		{
			const FVoxelCoord& Coord = Pair.Key;
			UVoxelChunkComponent* Pending = Pair.Value;

			if (!Pending)
			{
				ToDrop.Add(Coord);
				continue;
			}

			if (Pending->State == EVoxelChunkState::Ready)
			{
				ToPromote.Add(Coord);
			}
		}

		for (const FVoxelCoord& Coord : ToDrop)
		{
			PendingChunks.Remove(Coord);
		}

		for (const FVoxelCoord& Coord : ToPromote)
		{
			UVoxelChunkComponent* NewChunk = PendingChunks[Coord];
			UVoxelChunkComponent* OldChunk = ActiveChunks.FindRef(Coord);

			if (OldChunk)
			{
				OldChunk->CancelPendingTask();
				OldChunk->UnloadChunk();
			}

			ActiveChunks.Add(Coord, NewChunk);
			PendingChunks.Remove(Coord);

			// Trigger neighbor seam remesh after promotion
			OnChunkReady(Coord);
		}
	}

	// Promote macro-tiles (LOD2)
	{
		TArray<FIntPoint> ToPromote;
		TArray<FIntPoint> ToDrop;

		for (const auto& Pair : PendingMacro)
		{
			const FIntPoint& Tile = Pair.Key;
			UVoxelMacroTileComponent* Pending = Pair.Value;

			if (!Pending)
			{
				ToDrop.Add(Tile);
				continue;
			}

			if (Pending->State == EVoxelChunkState::Ready)
			{
				ToPromote.Add(Tile);
			}
		}

		for (const FIntPoint& Tile : ToDrop)
		{
			PendingMacro.Remove(Tile);
		}

		for (const FIntPoint& Tile : ToPromote)
		{
			UVoxelMacroTileComponent* NewTile = PendingMacro[Tile];
			UVoxelMacroTileComponent* OldTile = ActiveMacro.FindRef(Tile);

			if (OldTile)
			{
				OldTile->CancelPendingTask();
				OldTile->UnloadChunk();
			}

			ActiveMacro.Add(Tile, NewTile);
			PendingMacro.Remove(Tile);
		}
	}
}

// ============================================================================
// TASK SCHEDULING (CONCURRENCY-LIMITED WITH PRIORITY QUEUES)
// ============================================================================

TWeakObjectPtr<UVoxelChunkComponent> AVoxelWorld::PopClosest(TMap<int32, TArray<TWeakObjectPtr<UVoxelChunkComponent>>>& QueueByDistance)
{
	// Priority queue implementation: buckets keyed by squared distance
	// Pops closest chunk first, automatically skipping invalid/stale entries
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
			return nullptr; // All queues empty
		}

		TArray<TWeakObjectPtr<UVoxelChunkComponent>>& Bucket = QueueByDistance[MinDist];

		// Pop invalid/stale entries until we find a valid one
		while (Bucket.Num() > 0)
		{
			TWeakObjectPtr<UVoxelChunkComponent> Candidate = Bucket.Pop(false);
			if (UVoxelChunkComponent* C = Candidate.Get())
			{
				// Found a live chunk - remove bucket if now empty
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

	FScopeLock Lock(&GenMutex);

	// Fast path: slot available, run immediately
	if (ActiveGenTasks < MaxGen)
	{
		++ActiveGenTasks;
		Chunk->DoGeneration();
		return;
	}

	// Slow path: queue by distance-squared (closer chunks prioritized)
	GenWaitByDistance.FindOrAdd(Chunk->PriorityDist2).Add(Chunk);
}

void AVoxelWorld::OnGenerationFinished(UVoxelChunkComponent* /*Chunk*/)
{
	const UVoxelSettings* S = Settings.GetDefaultObject();
	const int32 MaxGen = (S->MaxConcurrentGenerationTasks > 1) ? S->MaxConcurrentGenerationTasks : 1;

	// Pop next closest chunk from priority queue (if available)
	TWeakObjectPtr<UVoxelChunkComponent> Next;
	{
		FScopeLock Lock(&GenMutex);

		// Decrement active count
		ActiveGenTasks = FMath::Max(0, ActiveGenTasks - 1);

		// Try to pop next task if we have capacity
		if (ActiveGenTasks < MaxGen)
		{
			Next = PopClosest(GenWaitByDistance);
		}

		// Reserve slot if we found a valid task
		if (Next.IsValid())
		{
			++ActiveGenTasks;
		}
	}

	// Launch task outside lock (DoGeneration may enqueue callbacks)
	if (UVoxelChunkComponent* N = Next.Get())
	{
		N->DoGeneration();
	}
}

void AVoxelWorld::ScheduleMeshing(UVoxelChunkComponent* Chunk, bool bSeamRemesh)
{
	if (bShuttingDown || !IsValid(Chunk)) return;

	// Track seam remesh flag (cumulative OR - if any request was seam-only)
	Chunk->bSeamRemeshQueued = Chunk->bSeamRemeshQueued || bSeamRemesh;

	const UVoxelSettings* S = Settings.GetDefaultObject();
	const int32 MaxMesh = (S->MaxConcurrentMeshingTasks > 1) ? S->MaxConcurrentMeshingTasks : 1;

	FScopeLock Lock(&MeshMutex);

	// Fast path: slot available, run immediately
	if (ActiveMeshTasks < MaxMesh)
	{
		++ActiveMeshTasks;

		// Pop seam flag and launch
		const bool bIsSeamRemesh = Chunk->bSeamRemeshQueued;
		Chunk->bSeamRemeshQueued = false;

		Chunk->DoMeshing(bIsSeamRemesh);
		return;
	}

	// Slow path: queue by distance-squared (closer chunks prioritized)
	// Note: Chunk may already be in queue (e.g., initial mesh + seam remesh)
	// This is fine - we just update the seam flag and it will be processed once
	MeshWaitByDistance.FindOrAdd(Chunk->PriorityDist2).Add(Chunk);
}

void AVoxelWorld::OnMeshingFinished(UVoxelChunkComponent* /*Chunk*/)
{
	const UVoxelSettings* S = Settings.GetDefaultObject();
	const int32 MaxMesh = (S->MaxConcurrentMeshingTasks > 1) ? S->MaxConcurrentMeshingTasks : 1;

	// Pop next closest chunk from priority queue (if available)
	TWeakObjectPtr<UVoxelChunkComponent> Next;
	bool bSeamRemesh = false;

	{
		FScopeLock Lock(&MeshMutex);

		// Decrement active count
		ActiveMeshTasks = FMath::Max(0, ActiveMeshTasks - 1);

		// Try to pop next task if we have capacity
		if (ActiveMeshTasks < MaxMesh)
		{
			Next = PopClosest(MeshWaitByDistance);
		}

		// Reserve slot and extract seam flag if we found a valid task
		if (Next.IsValid())
		{
			bSeamRemesh = Next->bSeamRemeshQueued;
			Next->bSeamRemeshQueued = false;
			++ActiveMeshTasks;
		}
	}

	// Launch task outside lock (DoMeshing may enqueue callbacks)
	if (UVoxelChunkComponent* N = Next.Get())
	{
		N->DoMeshing(bSeamRemesh);
	}
}

// ============================================================================
// MESH COMPONENT POOLING
// ============================================================================

UProceduralMeshComponent* AVoxelWorld::AcquirePMC()
{
	UProceduralMeshComponent* PMC = nullptr;

	// Try to reuse from pool first
	if (PMCPool.Num() > 0)
	{
		PMC = PMCPool.Pop(false);
	}
	else
	{
		// Pool empty - create new component
		PMC = NewObject<UProceduralMeshComponent>(this, UProceduralMeshComponent::StaticClass());
		PMC->RegisterComponent();
		PMC->AttachToComponent(RootComponent, FAttachmentTransformRules::KeepRelativeTransform);
	}

	// Reset component state for reuse
	PMC->SetVisibility(true, true);
	PMC->SetHiddenInGame(false, true);
	PMC->ClearAllMeshSections();
	PMC->SetCollisionEnabled(ECollisionEnabled::NoCollision);

	return PMC;
}

void AVoxelWorld::ReleasePMC(UProceduralMeshComponent* PMC)
{
	if (!PMC) return;

	// Clear data and hide component before returning to pool
	PMC->ClearAllMeshSections();
	PMC->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	PMC->SetHiddenInGame(true, true);
	PMC->SetVisibility(false, true);

	PMCPool.Add(PMC);
}

URealtimeMeshComponent* AVoxelWorld::AcquireRMC()
{
	URealtimeMeshComponent* RMC = nullptr;

	// Try to reuse from pool first
	if (RMCPool.Num() > 0)
	{
		RMC = RMCPool.Pop(false);
	}
	else
	{
		// Pool empty - create new component
		RMC = NewObject<URealtimeMeshComponent>(this);
		RMC->RegisterComponent();
		RMC->AttachToComponent(RootComponent, FAttachmentTransformRules::KeepRelativeTransform);
	}

	// CRITICAL: Reset transform to origin (prevents accumulated transform drift)
	RMC->SetRelativeLocation(FVector::ZeroVector);
	RMC->SetRelativeRotation(FRotator::ZeroRotator);
	RMC->SetRelativeScale3D(FVector::OneVector);

	// Reset component state for reuse
	RMC->SetVisibility(true, true);
	RMC->SetHiddenInGame(false, true);
	RMC->SetCollisionEnabled(ECollisionEnabled::NoCollision);

	// Clear any previous mesh data without recreating shared resources
	// (avoids lock-destruction assert when pooling)
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

	// Hide and disable collision
	RMC->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	RMC->SetHiddenInGame(true, true);
	RMC->SetVisibility(false, true);

	// Reset transform when returning to pool (prevents transform accumulation)
	RMC->SetRelativeLocation(FVector::ZeroVector);

	RMCPool.Add(RMC);
}

// ============================================================================
// CHUNK MANAGEMENT (PROGRESSIVE LOADING)
// ============================================================================

void AVoxelWorld::UpdateChunks()
{
	SCOPE_CYCLE_COUNTER(STAT_VoxelUpdateChunks);

	const UVoxelSettings* S = Settings.GetDefaultObject();
	if (!S) return;

	const AActor* Player = UGameplayStatics::GetPlayerPawn(this, 0);
	if (!Player) return;

	const FVector PlayerLocation = Player->GetActorLocation();
	const FVoxelCoord CenterChunk = WorldToChunkCoord(PlayerLocation);

	// Compute view ranges
	const int32 R2 = (S->LOD2_Radius > 0) ? S->LOD2_Radius : S->ViewDistanceChunks;
	const int32 R2Sq = R2 * R2;
	const int32 Rz = FMath::Max(0, S->ViewDistanceChunksZ);

	// OPTIMIZATION: Reuse static arrays to avoid allocations per frame
	static TArray<TPair<FVoxelCoord, int32>> Desired;
	static TSet<FVoxelCoord> Visible;
	static FVoxelCoord LastDesiredCenter = FVoxelCoord(INT32_MAX, INT32_MAX, INT32_MAX);  // Cache center

	// OPTIMIZATION: Only rebuild Desired list when player moves to different chunk
	// This eliminates expensive nested loops + sort when player stays in same chunk
	const bool bCenterChanged = !(CenterChunk == LastDesiredCenter);

	if (bCenterChanged || Desired.Num() == 0)
	{
		Desired.Reset(0);  // Keep capacity, just clear count

		const int32 EstimatedSize = (2 * R2 + 1) * (2 * R2 + 1) * (2 * Rz + 1);

		// OPTIMIZATION: Only reserve if current capacity is insufficient (TArray only)
		if (Desired.GetSlack() < EstimatedSize)
		{
			Desired.Reserve(EstimatedSize);
		}

		// Build desired list around player's current vertical chunk
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

		// OPTIMIZATION: Simplified sort comparator - just distance-squared (primary) and Z-priority (secondary)
		// Removed expensive Manhattan distance calculation - not critical for chunk spawning order
		const int32 Czc = CenterChunk.Cz;
		Desired.Sort([Czc](const TPair<FVoxelCoord, int32>& A, const TPair<FVoxelCoord, int32>& B)
		{
			// Primary: distance-squared (closest first)
			if (A.Value != B.Value) return A.Value < B.Value;

			// Secondary: prefer same vertical level (reduces vertical pop-in)
			const int32 Az = FMath::Abs(A.Key.Cz - Czc);
			const int32 Bz = FMath::Abs(B.Key.Cz - Czc);
			return Az < Bz;
		});

		LastDesiredCenter = CenterChunk;
	}

	// OPTIMIZATION: Build Visible set from Desired list (fast - just hash insertions from existing list)
	Visible.Reset();
	Visible.Reserve(Desired.Num());
	for (const auto& Pair : Desired)
	{
		Visible.Add(Pair.Key);
	}

	// Collision and AO radii
	const int32 CollisionR = S->CollisionViewDistance;
	const int32 CollisionR2 = CollisionR * CollisionR;
	const int32 AOR = S->AORadiusChunks;
	const int32 AOR2 = AOR * AOR;
	const int32 CollisionDropR2 = (CollisionR + 1) * (CollisionR + 1); // Hysteresis for collision drop

	TMap<FIntPoint, int32> VisibleTilesMinD2; // LOD2 macro-tiles

	// PROGRESSIVE LOADING: Reset spawn counter each frame
	ChunksSpawnedThisFrame = 0;
	const int32 SpawnBudget = FMath::Max(1, S->MaxChunksSpawnPerFrame);
	bNeedsMoreSpawning = false;

	int32 MissingChunks = 0; // Track how many chunks are not yet spawned

	// Process ALL desired chunks
	for (const auto& Pair : Desired)
	{
		const FVoxelCoord& C = Pair.Key;
		const int32 d2 = Pair.Value;
		Visible.Add(C);

		const EVoxelLODLevel DesiredLOD = PickLOD(d2, S);

		// LOD2 macro-tile handling (heightfield impostors)
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
				continue; // Skip regular chunk processing for LOD2
			}
		}

		const bool bDesiredCollision0 = (d2 <= CollisionR2);
		const bool bDesiredAO = (d2 <= AOR2);

		UVoxelChunkComponent* Active = ActiveChunks.FindRef(C);
		UVoxelChunkComponent* Pending = PendingChunks.FindRef(C);

		// Update priority distance if chunk exists
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

			// Spawn new chunk component
			UVoxelChunkComponent* Chunk = NewObject<UVoxelChunkComponent>(this);
			Chunk->RegisterComponent();
			AddInstanceComponent(Chunk);
			Chunk->PriorityDist2 = d2;

			Chunk->InitializeChunk(C, S, this, DesiredLOD, bDesiredCollision0, bDesiredAO);
			ActiveChunks.Add(C, Chunk);
			continue;
		}

		// EXISTING CHUNK: Update settings (collision, AO, LOD transitions)
		if (Active)
		{
			// Collision hysteresis: keep collision enabled for one extra ring
			bool bDesiredCollision = bDesiredCollision0;
			if (!bDesiredCollision && Active->bBuildCollision && d2 <= CollisionDropR2)
			{
				bDesiredCollision = true;
			}

			// Same LOD: check for policy changes (collision/AO)
			if (Active->LOD == DesiredLOD)
			{
				const bool bPolicyChanged =
					(Active->bBuildCollision != bDesiredCollision) ||
					(Active->bUseAO != bDesiredAO);

				if (bPolicyChanged)
				{
					const bool AOChanged = (Active->bUseAO != bDesiredAO);
					Active->bUseAO = bDesiredAO;

					// Collision policy changed: reapply collision using cached mesh
					if (Active->bBuildCollision != bDesiredCollision)
					{
						Active->RequestCollisionReapply(bDesiredCollision);
					}

					// AO policy changed: full remesh required
					if (AOChanged)
					{
						Active->RequestRemesh();
					}
				}
			}
			else
			{
				// LOD changed: spawn Pending replacement
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
					// Pending already exists - update its settings
					Pending->bBuildCollision = bDesiredCollision0;
					Pending->bUseAO = bDesiredAO;
					Pending->PriorityDist2 = d2;
				}
			}
		}
		else if (Pending)
		{
			// Update pending chunk settings
			Pending->bBuildCollision = bDesiredCollision0;
			Pending->bUseAO = bDesiredAO;
			Pending->PriorityDist2 = d2;
		}
	}

	// Track initial load progress
	if (MissingChunks > 0 && ChunksSpawnedThisFrame >= SpawnBudget)
	{
		bNeedsMoreSpawning = true;
	}
	else if (MissingChunks == 0)
	{
		bNeedsMoreSpawning = false;
	}

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

	// Spawn/update macro-tiles (LOD2)
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

		// OPTIMIZATION: Single-pass macro-tile unloading (reuse static array)
		static TArray<FIntPoint> ToRemoveTiles;
		ToRemoveTiles.Reset(0);

		// Unload invisible active macro-tiles
		for (const auto& Pair : ActiveMacro)
		{
			if (!VisibleTilesMinD2.Contains(Pair.Key))
			{
				if (UVoxelMacroTileComponent* C = Pair.Value)
				{
					C->CancelPendingTask();
					C->UnloadChunk();
					C->DestroyComponent();
				}
				ToRemoveTiles.Add(Pair.Key);
			}
		}
		for (const FIntPoint& T : ToRemoveTiles)
		{
			ActiveMacro.Remove(T);
		}

		// Unload invisible pending macro-tiles
		ToRemoveTiles.Reset(0);
		for (const auto& Pair : PendingMacro)
		{
			if (!VisibleTilesMinD2.Contains(Pair.Key))
			{
				if (UVoxelMacroTileComponent* C = Pair.Value)
				{
					C->CancelPendingTask();
					C->UnloadChunk();
					C->DestroyComponent();
				}
				ToRemoveTiles.Add(Pair.Key);
			}
		}
		for (const FIntPoint& T : ToRemoveTiles)
		{
			PendingMacro.Remove(T);
		}
	}

	// OPTIMIZATION: Single-pass chunk unloading (build removal list while unloading)
	// Reuse static array to avoid allocations
	static TArray<FVoxelCoord> ToRemoveChunks;
	ToRemoveChunks.Reset(0);

	// Unload invisible active chunks
	for (const auto& Pair : ActiveChunks)
	{
		if (!Visible.Contains(Pair.Key))
		{
			if (UVoxelChunkComponent* Chunk = Pair.Value)
			{
				Chunk->CancelPendingTask();
				Chunk->UnloadChunk();
				Chunk->DestroyComponent();
			}
			ToRemoveChunks.Add(Pair.Key);
		}
	}
	for (const FVoxelCoord& C : ToRemoveChunks)
	{
		ActiveChunks.Remove(C);
	}

	// Unload invisible pending chunks
	ToRemoveChunks.Reset(0);
	for (const auto& Pair : PendingChunks)
	{
		if (!Visible.Contains(Pair.Key))
		{
			if (UVoxelChunkComponent* Chunk = Pair.Value)
			{
				Chunk->CancelPendingTask();
				Chunk->UnloadChunk();
				Chunk->DestroyComponent();
			}
			ToRemoveChunks.Add(Pair.Key);
		}
	}
	for (const FVoxelCoord& C : ToRemoveChunks)
	{
		PendingChunks.Remove(C);
	}
}

FVoxelCoord AVoxelWorld::WorldToChunkCoord(const FVector& Location) const
{
	const UVoxelSettings* S = Settings.GetDefaultObject();
	const float VoxelUU = S->VoxelWorldScale;

	const float ChunkWorldSizeX = S->ChunkSizeX * VoxelUU;
	const float ChunkWorldSizeY = S->ChunkSizeY * VoxelUU;
	const float ChunkWorldSizeZ = S->ChunkSizeZ * VoxelUU;

	// Compute chunk coordinate along each axis using FloorToInt to handle negative locations
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

// ============================================================================
// PUBLIC APIs
// ============================================================================

UVoxelChunkComponent* AVoxelWorld::GetChunk(const FVoxelCoord& Coord) const
{
	if (UVoxelChunkComponent* const* Found = ActiveChunks.Find(Coord)) return *Found;
	return nullptr;
}

void AVoxelWorld::OnChunkReady(const FVoxelCoord& Coord)
{
	// Mark all 6 neighboring chunks as dirty for seamless edge remeshing
	static const FVoxelCoord Neighbors[6] = {
		{-1,0,0}, {1,0,0}, {0,-1,0}, {0,1,0}, {0,0,-1}, {0,0,1}
	};

	for (const FVoxelCoord& D : Neighbors)
	{
		if (UVoxelChunkComponent* N = GetChunk({ Coord.Cx + D.Cx, Coord.Cy + D.Cy, Coord.Cz + D.Cz }))
		{
			if (N->RenderMode == EVoxelRenderMode::Voxels)
			{
				// CRITICAL: Mark ALL voxel neighbors as dirty, not just Ready ones
				// This ensures chunks that are still generating/meshing will use the
				// correct border data from this chunk when they eventually mesh
				N->bNeighborsCacheDirty = true;

				// If neighbor is already Ready, trigger an immediate seam remesh
				if (N->State == EVoxelChunkState::Ready)
				{
					N->RequestRemesh(); // Rebuild seam faces now
				}
			}
		}
	}
}

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
	Item.bWasSeamRemesh = true; // Collision reapply counts as seam-only

	FScopeLock Lock(&ApplyQueueMutex);
	ApplyQueue.Add(MoveTemp(Item));
}

void AVoxelWorld::MarkChunkForSeamRemesh(const FVoxelCoord& C)
{
	PendingSeamRemesh.Enqueue(C);
}

