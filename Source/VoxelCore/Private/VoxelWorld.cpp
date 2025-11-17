#include "VoxelWorld.h"
#include "VoxelChunkComponent.h"
#include "VoxelMacroTileComponent.h"
#include "VoxelMesher.h"
#include "VoxelStats.h"
#include "VoxelGPUMesher.h"
#include "VoxelGPUGenerator.h"
#include "VoxelStreamingSourceComponent.h"
#include "Kismet/GameplayStatics.h"
#include "ProceduralMeshComponent.h"
#include "RealtimeMeshComponent.h"
#include "EngineUtils.h"
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

	// ALWAYS call UpdateChunks every frame to update collision/visibility based on player position
	// The function is optimized internally with caching (only rebuilds Desired list when player changes chunks)
	UpdateChunks();

	// Track last center for initial load completion
	const AActor* Player = UGameplayStatics::GetPlayerPawn(this, 0);
	if (Player)
	{
		const FVector PlayerPos = Player->GetActorLocation();
		const FVoxelCoord Center = WorldToChunkCoord(PlayerPos);

		if (!bHasLastCenter || !(Center == LastCenterChunk))
		{
			LastCenterChunk = Center;
			bHasLastCenter = true;
		}
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

	// OPTIMIZATION: Gather items to process (collision prioritized) in single pass
	{
		FScopeLock Lock(&ApplyQueueMutex);
		if (ApplyQueue.Num() == 0)
		{
			return;
		}

		const int32 Budget = MaxMeshAppliesPerTick;

		// OPTIMIZATION: Use TSet for O(1) lookups instead of TArray Contains (was O(n²)!)
		TSet<int32> ProcessedIndices;
		ProcessedIndices.Reserve(Budget);

		// Pass 1: Collision items (priority)
		for (int32 i = 0; i < ApplyQueue.Num() && ToProcess.Num() < Budget; ++i)
		{
			if (ApplyQueue[i].bCollision)
			{
				ToProcess.Add(MoveTemp(ApplyQueue[i]));
				ProcessedIndices.Add(i);
			}
		}

		// Pass 2: Fill remainder with non-collision (using TSet for fast lookup)
		for (int32 i = 0; i < ApplyQueue.Num() && ToProcess.Num() < Budget; ++i)
		{
			if (!ProcessedIndices.Contains(i))  // O(1) hash lookup instead of O(n) linear!
			{
				ToProcess.Add(MoveTemp(ApplyQueue[i]));
				ProcessedIndices.Add(i);
			}
		}

		// OPTIMIZATION: Batch remove using sorted indices (highest first to avoid shifts)
		TArray<int32> ToRemoveIndices = ProcessedIndices.Array();
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
		UE_LOG(LogTemp, Verbose, TEXT("[GenQueue] START (%d,%d,%d) - ActiveGenTasks=%d, QueueSize=%d"),
			Chunk->ChunkCoord.Cx, Chunk->ChunkCoord.Cy, Chunk->ChunkCoord.Cz, ActiveGenTasks, GenWaitByDistance.Num());
		Chunk->DoGeneration();
		return;
	}

	// Slow path: queue by distance-squared (closer chunks prioritized)
	GenWaitByDistance.FindOrAdd(Chunk->PriorityDist2).Add(Chunk);

	// DEBUG: Log queue growth
	static int32 LastQueueSize = 0;
	const int32 CurrentQueueSize = GenWaitByDistance.Num();
	if (CurrentQueueSize > LastQueueSize && CurrentQueueSize % 10 == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("[GenQueue] QUEUED (%d,%d,%d) - ActiveGenTasks=%d, QueueSize=%d (GROWING!)"),
			Chunk->ChunkCoord.Cx, Chunk->ChunkCoord.Cy, Chunk->ChunkCoord.Cz, ActiveGenTasks, CurrentQueueSize);
		LastQueueSize = CurrentQueueSize;
	}
}

void AVoxelWorld::OnGenerationFinished(UVoxelChunkComponent* Chunk)
{
	const UVoxelSettings* S = Settings.GetDefaultObject();
	const int32 MaxGen = (S->MaxConcurrentGenerationTasks > 1) ? S->MaxConcurrentGenerationTasks : 1;

	// Pop next closest chunk from priority queue (if available)
	TWeakObjectPtr<UVoxelChunkComponent> Next;
	int32 NewActiveCount = 0;
	int32 QueueSize = 0;
	{
		FScopeLock Lock(&GenMutex);

		// Decrement active count
		ActiveGenTasks = FMath::Max(0, ActiveGenTasks - 1);
		NewActiveCount = ActiveGenTasks;
		QueueSize = GenWaitByDistance.Num();

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

	// DEBUG: Log completion
	if (Chunk)
	{
		UE_LOG(LogTemp, Verbose, TEXT("[GenQueue] FINISH (%d,%d,%d) - ActiveGenTasks=%d, QueueSize=%d"),
			Chunk->ChunkCoord.Cx, Chunk->ChunkCoord.Cy, Chunk->ChunkCoord.Cz, NewActiveCount, QueueSize);
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("[GenQueue] FINISH (NULL chunk!) - ActiveGenTasks=%d, QueueSize=%d"),
			NewActiveCount, QueueSize);
	}

	// Launch task outside lock (DoGeneration may enqueue callbacks)
	if (UVoxelChunkComponent* N = Next.Get())
	{
		UE_LOG(LogTemp, Verbose, TEXT("[GenQueue] START (%d,%d,%d) - ActiveGenTasks=%d, QueueSize=%d"),
			N->ChunkCoord.Cx, N->ChunkCoord.Cy, N->ChunkCoord.Cz, ActiveGenTasks, GenWaitByDistance.Num());
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
// STREAMING SOURCE MANAGEMENT
// ============================================================================

void AVoxelWorld::GatherStreamingSources()
{
	StreamingSources.Reset();

	// Scan world for all active streaming source components
	UWorld* World = GetWorld();
	if (!World) return;

	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (!Actor) continue;

		// Find all streaming source components on this actor
		TArray<UVoxelStreamingSourceComponent*> Components;
		Actor->GetComponents<UVoxelStreamingSourceComponent>(Components);

		for (UVoxelStreamingSourceComponent* Source : Components)
		{
			if (Source && Source->IsActiveSource())
			{
				StreamingSources.Add(Source);
			}
		}
	}

	// Log when streaming sources change (helps debugging)
	static int32 LastCount = -1;
	if (StreamingSources.Num() != LastCount)
	{
		UE_LOG(LogTemp, Log, TEXT("[VoxelWorld] Found %d active streaming source(s)"), StreamingSources.Num());
		for (UVoxelStreamingSourceComponent* Source : StreamingSources)
		{
			UE_LOG(LogTemp, Log, TEXT("  - %s (Player=%d, Priority=%.2f, ViewDist=%d)"),
				*Source->GetOwner()->GetName(),
				Source->bIsPlayerSource ? 1 : 0,
				Source->PriorityWeight,
				Source->ViewDistanceChunks);
		}
		LastCount = StreamingSources.Num();
	}
}

// ============================================================================
// CHUNK MANAGEMENT (PROGRESSIVE LOADING)
// ============================================================================

void AVoxelWorld::UpdateChunks()
{
	SCOPE_CYCLE_COUNTER(STAT_VoxelUpdateChunks);

	const UVoxelSettings* S = Settings.GetDefaultObject();
	if (!S) return;

	// Gather all active streaming sources
	GatherStreamingSources();

	// Fallback to GetPlayerPawn if no streaming sources found (backwards compatibility)
	UVoxelStreamingSourceComponent* PrimarySource = nullptr;
	const AActor* PrimaryActor = nullptr;

	if (StreamingSources.Num() > 0)
	{
		// Use first streaming source as primary (TODO: support multiple sources)
		PrimarySource = StreamingSources[0];
		PrimaryActor = PrimarySource->GetOwner();
	}
	else
	{
		// Backwards compatibility: Use player pawn if no streaming sources
		PrimaryActor = UGameplayStatics::GetPlayerPawn(this, 0);
		if (!PrimaryActor) return;
	}

	const FVector PlayerLocation = PrimarySource ? PrimarySource->GetSourceLocation() : PrimaryActor->GetActorLocation();
	const FVoxelCoord CenterChunk = WorldToChunkCoord(PlayerLocation);

	// DEBUG: Log player position and center chunk every 60 frames
	static int32 DebugFrameCounter = 0;
	if (++DebugFrameCounter >= 60)
	{
		DebugFrameCounter = 0;
		UE_LOG(LogTemp, Warning, TEXT("[VoxelWorld] Player at World(%.1f,%.1f,%.1f) Chunk(%d,%d,%d) - CollisionR=%d, Using StreamingSource=%s"),
			PlayerLocation.X, PlayerLocation.Y, PlayerLocation.Z,
			CenterChunk.Cx, CenterChunk.Cy, CenterChunk.Cz,
			PrimarySource ? PrimarySource->CollisionRadius : S->CollisionViewDistance,
			PrimarySource ? TEXT("YES") : TEXT("NO"));
	}

	// Get view direction for frustum priority
	const FRotator PlayerRotation = PrimarySource ? PrimarySource->GetSourceRotation() : PrimaryActor->GetActorRotation();
	const FVector PlayerForward = PrimarySource ? PrimarySource->GetForwardVector() : PlayerRotation.Vector();

	// Compute view ranges from streaming source (or fallback to global settings)
	const int32 R2 = PrimarySource ? PrimarySource->ViewDistanceChunks :
		((S->LOD2_Radius > 0) ? S->LOD2_Radius : S->ViewDistanceChunks);
	const int32 R2Sq = R2 * R2;
	const int32 Rz = PrimarySource ? (PrimarySource->ViewDistanceChunksZ > 0 ? PrimarySource->ViewDistanceChunksZ : R2) :
		FMath::Max(0, S->ViewDistanceChunksZ);

	// Frustum priority settings
	const bool bUseFrustumPriority = PrimarySource ? PrimarySource->bUseFrustumPriority : S->bUseFrustumBasedGeneration;
	const float FrustumHFOV = PrimarySource ? PrimarySource->FrustumHorizontalFOV : S->FrustumHorizontalFOV;
	const bool bFrustumVertical = PrimarySource ? PrimarySource->bFrustumPriorityVertical : S->bFrustumCullVertical;
	const float FrustumVFOV = PrimarySource ? PrimarySource->FrustumVerticalFOV : S->FrustumVerticalFOV;
	const bool bDiskLoading = PrimarySource ? PrimarySource->bDiskShapedLoading : S->bDiskShapedLoading;

	// Cache chunk world size for visibility culling (needed outside rebuild block)
	const float VoxelUU = S->VoxelWorldScale;
	const float ChunkWorldSizeX = S->ChunkSizeX * VoxelUU;
	const float ChunkWorldSizeY = S->ChunkSizeY * VoxelUU;
	const float ChunkWorldSizeZ = S->ChunkSizeZ * VoxelUU;

	// Precompute frustum parameters for visibility culling (needed outside rebuild block)
	const bool bApplyFrustumPriority = bUseFrustumPriority && FrustumHFOV < 360.0f;
	const float CosHalfHorizontalFOV = bApplyFrustumPriority ? FMath::Cos(FMath::DegreesToRadians(FrustumHFOV * 0.5f)) : -1.0f;
	const float CosHalfVerticalFOV = (bApplyFrustumPriority && bFrustumVertical)
		? FMath::Cos(FMath::DegreesToRadians(FrustumVFOV * 0.5f)) : -1.0f;
	const FVector PlayerForwardXY = FVector(PlayerForward.X, PlayerForward.Y, 0.0f).GetSafeNormal();

	// OPTIMIZATION: Cache both Desired list (with priority) and Visible set to avoid rebuilding every frame
	static TArray<TPair<FVoxelCoord, float>> Desired; // Coord → Priority (higher = more important)
	static TSet<FVoxelCoord> Visible;
	static FVoxelCoord LastDesiredCenter = FVoxelCoord(INT32_MAX, INT32_MAX, INT32_MAX);
	static float LastPlayerYaw = 0.0f;
	static bool bVisibleCached = false;

	// OPTIMIZATION: Only rebuild when player moves chunks OR rotates horizontally (ignore pitch for jumping)
	const bool bCenterChanged = !(CenterChunk == LastDesiredCenter);
	const float CurrentYaw = PlayerRotation.Yaw;
	const float YawDelta = FMath::Abs(FRotator::NormalizeAxis(CurrentYaw - LastPlayerYaw));
	const bool bRotationChanged = bUseFrustumPriority && (YawDelta > 10.0f); // 10° threshold, horizontal only
	const bool bNeedsRebuild = bCenterChanged || bRotationChanged;

	if (bNeedsRebuild || Desired.Num() == 0)
	{
		Desired.Reset(0);

		const int32 EstimatedSize = (2 * R2 + 1) * (2 * R2 + 1) * (2 * Rz + 1);

		if (Desired.GetSlack() < EstimatedSize)
		{
			Desired.Reserve(EstimatedSize);
		}

		const int32 VerticalCenter = CenterChunk.Cz;

		// NOTE: Frustum and chunk size variables are now declared outside the rebuild block
		// so they're accessible for visibility culling later

		for (int32 dx = -R2; dx <= R2; ++dx)
		{
			for (int32 dy = -R2; dy <= R2; ++dy)
			{
				for (int32 dz = -Rz; dz <= Rz; ++dz)
				{
					const int32 d2 = dx * dx + dy * dy + dz * dz;
					if (!bDiskLoading || d2 <= R2Sq)
					{
						const FVoxelCoord ChunkCoord(CenterChunk.Cx + dx, CenterChunk.Cy + dy, VerticalCenter + dz);

						// Calculate base priority (inverse distance - closer = higher priority)
						// Priority range: [0.0 ... 1.0] where 1.0 = at player position, 0.0 = max distance
						const float Distance = FMath::Sqrt(static_cast<float>(d2));
						const float MaxDistance = FMath::Sqrt(static_cast<float>(R2Sq + Rz * Rz));
						float Priority = 1.0f - (Distance / (MaxDistance + 1.0f));

						// OPTIMIZATION: Frustum PRIORITY BOOST (not hard-culling)
						// Chunks in frustum get 2x priority (loaded first), chunks outside get normal priority (loaded later)
						if (bApplyFrustumPriority)
						{
							// Calculate chunk center in world space
							const FVector ChunkWorldPos(
								ChunkCoord.Cx * ChunkWorldSizeX + ChunkWorldSizeX * 0.5f,
								ChunkCoord.Cy * ChunkWorldSizeY + ChunkWorldSizeY * 0.5f,
								ChunkCoord.Cz * ChunkWorldSizeZ + ChunkWorldSizeZ * 0.5f
							);

							const FVector ToChunk = ChunkWorldPos - PlayerLocation;
							const float DistanceSq = ToChunk.SizeSquared();

							// Close chunks always get max priority (no frustum check needed)
							const float CloseChunkThresholdSq = FMath::Max(ChunkWorldSizeX, FMath::Max(ChunkWorldSizeY, ChunkWorldSizeZ));
							const float CloseChunkThreshold = CloseChunkThresholdSq * CloseChunkThresholdSq * 4.0f;

							if (DistanceSq > CloseChunkThreshold)
							{
								const FVector ToChunkDir = ToChunk.GetSafeNormal();

								// Horizontal frustum check (XY plane)
								const FVector ToChunkXY = FVector(ToChunkDir.X, ToChunkDir.Y, 0.0f).GetSafeNormal();
								const float DotHorizontal = FVector::DotProduct(PlayerForwardXY, ToChunkXY);

								bool bInFrustum = (DotHorizontal >= CosHalfHorizontalFOV);

								// Vertical frustum check (optional)
								if (bInFrustum && bFrustumVertical && CosHalfVerticalFOV > -1.0f)
								{
									const float DotVertical = FVector::DotProduct(PlayerForward, ToChunkDir);
									bInFrustum = (DotVertical >= CosHalfVerticalFOV);
								}

								// 2x priority boost for chunks in frustum
								if (bInFrustum)
								{
									Priority *= 2.0f;
								}
							}
							else
							{
								// Very close chunks always get 2x priority
								Priority *= 2.0f;
							}
						}

						Desired.Emplace(ChunkCoord, Priority);
					}
				}
			}
		}

		// Sort by PRIORITY (higher = load first), then by vertical distance
		const int32 Czc = CenterChunk.Cz;
		Desired.Sort([Czc](const TPair<FVoxelCoord, float>& A, const TPair<FVoxelCoord, float>& B)
		{
			// Higher priority first (reverse sort on priority)
			if (!FMath::IsNearlyEqual(A.Value, B.Value, 0.001f))
			{
				return A.Value > B.Value; // Higher priority = earlier in list
			}

			// If priorities are equal, prefer chunks closer to player's vertical position
			const int32 Az = FMath::Abs(A.Key.Cz - Czc);
			const int32 Bz = FMath::Abs(B.Key.Cz - Czc);
			return Az < Bz;
		});

		LastDesiredCenter = CenterChunk;
		LastPlayerYaw = CurrentYaw;  // Cache yaw to detect horizontal camera rotation
		bVisibleCached = false;  // Invalidate Visible cache when Desired changes
	}

	// OPTIMIZATION: Cache Visible set too (only rebuild if Desired changed)
	// Eliminates 3000+ hash insertions every frame during chunk loading
	if (!bVisibleCached)
	{
		Visible.Reset();
		Visible.Reserve(Desired.Num());
		for (const auto& Pair : Desired)
		{
			Visible.Add(Pair.Key);
		}
		bVisibleCached = true;
	}

	// Collision and AO radii (use streaming source settings if available)
	const int32 CollisionR = PrimarySource ? PrimarySource->CollisionRadius : S->CollisionViewDistance;
	const int32 CollisionR2 = CollisionR * CollisionR;
	const int32 AOR = PrimarySource ? PrimarySource->AORadius : S->AORadiusChunks;
	const int32 AOR2 = AOR * AOR;
	const int32 CollisionDropR2 = (CollisionR + 1) * (CollisionR + 1); // Hysteresis for collision drop

	TMap<FIntPoint, int32> VisibleTilesMinD2; // LOD2 macro-tiles

	// PROGRESSIVE LOADING: Reset spawn counter each frame
	ChunksSpawnedThisFrame = 0;
	const int32 SpawnBudget = FMath::Max(1, S->MaxChunksSpawnPerFrame);

	int32 MissingChunks = 0; // Track how many chunks are not yet spawned

	// MEGA OPTIMIZATION: When player hasn't moved or rotated, skip the entire update loop
	// All chunks are already correct, so no need to check every chunk's settings
	// CRITICAL: Check bNeedsMoreSpawning BEFORE resetting it, to preserve previous frame's state
	const bool bPlayerStandingStill = !bCenterChanged && !bRotationChanged;
	const bool bAllChunksLoaded = bInitialLoadComplete && !bNeedsMoreSpawning;

	if (bPlayerStandingStill && bAllChunksLoaded)
	{
		// Player standing still, all chunks loaded - nothing to do!
		// Skip expensive chunk iteration entirely
		return;
	}

	// Now reset for this frame's processing
	bNeedsMoreSpawning = false;

	// OPTIMIZATION: Process ALL desired chunks (sorted by priority)
	for (const auto& Pair : Desired)
	{
		const FVoxelCoord& C = Pair.Key;
		// NOTE: Pair.Value is now priority (float), not distance! Recalculate distance from chunk coord
		const int32 dx = C.Cx - CenterChunk.Cx;
		const int32 dy = C.Cy - CenterChunk.Cy;
		const int32 dz = C.Cz - CenterChunk.Cz;
		const int32 d2 = dx * dx + dy * dy + dz * dz;

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

		// OPTIMIZATION: Single lookup per map instead of two FindRef calls
		UVoxelChunkComponent* Active = ActiveChunks.FindRef(C);
		UVoxelChunkComponent* Pending = PendingChunks.FindRef(C);

		// VISIBILITY CULLING: Only update visibility when player rotates (saves massive CPU time)
		// CRITICAL OPTIMIZATION: Skip expensive visibility calculations unless player rotated
		// This reduces UpdateChunks from 3.5ms to <0.5ms when all chunks loaded and player standing still
		const bool bNeedsVisibilityUpdate = bRotationChanged &&
		                                     Active &&
		                                     PrimarySource &&
		                                     PrimarySource->bIsPlayerSource;

		if (bNeedsVisibilityUpdate)
		{
			// Calculate if chunk is in frustum (reuse earlier frustum logic)
			bool bShouldBeVisible = true; // Default: visible

			// Calculate chunk center in world space (used by both frustum and occlusion checks)
			const FVector ChunkWorldPos(
				C.Cx * ChunkWorldSizeX + ChunkWorldSizeX * 0.5f,
				C.Cy * ChunkWorldSizeY + ChunkWorldSizeY * 0.5f,
				C.Cz * ChunkWorldSizeZ + ChunkWorldSizeZ * 0.5f
			);

			const FVector ToChunk = ChunkWorldPos - PlayerLocation;
			const float DistanceSq = ToChunk.SizeSquared();

			// FRUSTUM CULLING
			if (bApplyFrustumPriority && FrustumHFOV < 360.0f)
			{
				// Close chunks always visible
				const float CloseChunkThresholdSq = FMath::Max(ChunkWorldSizeX, FMath::Max(ChunkWorldSizeY, ChunkWorldSizeZ));
				const float CloseChunkThreshold = CloseChunkThresholdSq * CloseChunkThresholdSq * 4.0f;

				if (DistanceSq > CloseChunkThreshold)
				{
					const FVector ToChunkDir = ToChunk.GetSafeNormal();

					// Horizontal frustum check
					const FVector ToChunkXY = FVector(ToChunkDir.X, ToChunkDir.Y, 0.0f).GetSafeNormal();
					const float DotHorizontal = FVector::DotProduct(PlayerForwardXY, ToChunkXY);

					bShouldBeVisible = (DotHorizontal >= CosHalfHorizontalFOV);

					// Vertical frustum check (optional)
					if (bShouldBeVisible && bFrustumVertical && CosHalfVerticalFOV > -1.0f)
					{
						const float DotVertical = FVector::DotProduct(PlayerForward, ToChunkDir);
						bShouldBeVisible = (DotVertical >= CosHalfVerticalFOV);
					}
				}
			}

			// OCCLUSION CULLING (simple height-based check)
			if (bShouldBeVisible && PrimarySource->bEnableOcclusionCulling)
			{
				const int32 OcclusionMinDistSq = PrimarySource->OcclusionMinDistance * PrimarySource->OcclusionMinDistance;

				// Only occlude chunks beyond minimum distance
				if (d2 > OcclusionMinDistSq)
				{
					// Simple occlusion: Hide chunks significantly below player height and far away
					// This catches chunks on the other side of mountains/hills
					const float ChunkTopZ = ChunkWorldPos.Z + (ChunkWorldSizeZ * 0.5f);
					const float PlayerZ = PlayerLocation.Z;

					// If chunk's top is more than 2 chunk heights below player, likely occluded
					const float HeightDifference = PlayerZ - ChunkTopZ;
					if (HeightDifference > ChunkWorldSizeZ * 2.0f)
					{
						bShouldBeVisible = false;
						// TODO: More sophisticated occlusion with terrain raycasting
					}
				}
			}

			// Apply visibility to mesh component
			if (Active->IsUsingRMC() && Active->GetRMC())
			{
				Active->GetRMC()->SetVisibility(bShouldBeVisible, true);
				Active->GetRMC()->SetHiddenInGame(!bShouldBeVisible, true);
			}
			else if (Active->GetPMC())
			{
				Active->GetPMC()->SetVisibility(bShouldBeVisible, true);
				Active->GetPMC()->SetHiddenInGame(!bShouldBeVisible, true);
			}
		}

		// OPTIMIZATION: Early exit if chunk exists and already correct
		// Update priority distance if chunk exists
		if (Active)
		{
			Active->PriorityDist2 = d2;

			// OPTIMIZATION: Early exit if this chunk is already correct (common case during generation)
			// Check if LOD, collision, and AO settings are already correct
			bool bDesiredCollision = bDesiredCollision0;
			if (!bDesiredCollision && Active->bBuildCollision && d2 <= CollisionDropR2)
			{
				bDesiredCollision = true;
			}

			const bool bSettingsMatch = (Active->LOD == DesiredLOD) &&
			                            (Active->bBuildCollision == bDesiredCollision) &&
			                            (Active->bUseAO == bDesiredAO);

			if (bSettingsMatch && !Pending)
			{
				continue;  // Chunk is already correct, skip expensive processing
			}
		}

		if (Pending)
		{
			Pending->PriorityDist2 = d2;
		}

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

			// DEBUG: Log collision settings for chunks near player
			if (d2 <= 4) // Log for chunks within 2-chunk radius
			{
				UE_LOG(LogTemp, Log, TEXT("[VoxelWorld] Spawned chunk (%d,%d,%d) - d2=%d, Collision=%s (CollisionR=%d, CollisionR2=%d)"),
					C.Cx, C.Cy, C.Cz, d2, bDesiredCollision0 ? TEXT("YES") : TEXT("NO"), CollisionR, CollisionR2);
			}

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
						// DEBUG: Log collision changes
						if (d2 <= 16) // Log for chunks within 4-chunk radius
						{
							UE_LOG(LogTemp, Warning, TEXT("[VoxelWorld] UPDATING collision for chunk (%d,%d,%d) - d2=%d, Old=%s, New=%s (CollisionR=%d)"),
								C.Cx, C.Cy, C.Cz, d2,
								Active->bBuildCollision ? TEXT("YES") : TEXT("NO"),
								bDesiredCollision ? TEXT("YES") : TEXT("NO"),
								CollisionR);
						}
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

