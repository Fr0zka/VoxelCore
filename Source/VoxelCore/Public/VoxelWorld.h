#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "VoxelStructs.h"
#include "VoxelSettings.h"
#include "VoxelChunkBucket.h"
#include "VoxelWorld.generated.h"

class UVoxelChunkComponent;
class UVoxelMacroTileComponent;
class UProceduralMeshComponent;
class URealtimeMeshComponent;
class UVoxelStreamingSourceComponent;
struct FMeshBuffers;

/**
 * AVoxelWorld: Main orchestrator for voxel terrain streaming.
 *
 * **Responsibilities:**
 * - Chunk lifecycle management (spawn, LOD selection, teardown)
 * - Progressive loading with per-frame spawn budgets
 * - Task scheduling with concurrency limits and priority queues
 * - Mesh apply queue with collision prioritization
 * - Mesh component pooling for performance
 *
 * **LOD System:**
 * - LOD0: Full resolution chunks (16×16×64 default)
 * - LOD1: Coarsened XY chunks (same vertical resolution)
 * - LOD2: Macro-tiles (heightfield impostors)
 *
 * **Chunk States:**
 * - Pending: Loading in background (generation/meshing in progress)
 * - Active: Visible and rendered
 * - Transition: Pending → Active via PromoteReadyPendings()
 *
 * **Threading Model:**
 * - GameThread: Tick(), apply queue, chunk management
 * - TaskGraph: Generation and meshing tasks (concurrency-limited)
 * - RenderThread: GPU mesh readbacks
 */
UCLASS()
class VOXELCORE_API AVoxelWorld : public AActor
{
	GENERATED_BODY()

public:
	AVoxelWorld();

protected:
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaTime) override;
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;

public:
	// ==================== CONFIGURATION ====================

	/** Voxel world settings (chunk size, LOD radii, generation params, etc.) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel")
	TSubclassOf<UVoxelSettings> Settings;

	/** Max mesh apply operations per tick (collision prioritized over visual-only)
	 * CRITICAL: This was a SEVERE bottleneck at 4 (taking 79 seconds to load 19k chunks!)
	 * For 40uu voxels (6.25x more chunks), need MUCH higher throughput.
	 *
	 * Recommended values:
	 * - 128: Aggressive loading (5800X3D + 7900XTX can handle it!)
	 * - 64: Balanced (good for most high-end PCs)
	 * - 32: Conservative (original "fixed" value, still too low for 40uu)
	 *
	 * With batching ON, individual chunk applies are cheap (just caching buffers).
	 * The real work happens in bucket rebuilds, so we can push this MUCH higher.
	 */
	UPROPERTY(EditAnywhere, Category = "Voxel|Performance", meta = (ClampMin = "1", ClampMax = "512"))
	int32 MaxMeshAppliesPerTick = 128;

	// ==================== PUBLIC API ====================

	/**
	 * Called by chunks when they first become Ready (not for seam-only remesh).
	 * Marks all 6 neighboring chunks as dirty for seamless edge remeshing.
	 */
	void OnChunkReady(const FVoxelCoord& Coord);

	/** Lookup active LOD0/LOD1 chunk at given coordinate (nullptr if not active). */
	UVoxelChunkComponent* GetChunk(const FVoxelCoord& Coord) const;

	/**
	 * Worker thread: enqueue fresh mesh apply (world owns buffers during apply).
	 * @param Chunk Target chunk component
	 * @param Buffers Mesh data (moved, owned by world until applied)
	 * @param bCreateCollision Whether to build collision mesh
	 * @param bWasSeamRemesh True if this is a seam-only remesh (not initial generation)
	 * @param Sequence Apply sequence number (for out-of-order GPU result handling)
	 */
	void EnqueueMeshApply(UVoxelChunkComponent* Chunk, FMeshBuffers&& Buffers, bool bCreateCollision, bool bWasSeamRemesh, int32 Sequence);

	/**
	 * GameThread: enqueue collision-only reapply using chunk's cached buffers.
	 * Used when collision policy changes without regenerating mesh.
	 */
	void EnqueueReapplyUsingCache(UVoxelChunkComponent* Chunk, bool bCreateCollision);

	/** Mark chunk for deferred seam remesh (processed during DrainApplyQueue). */
	void MarkChunkForSeamRemesh(const FVoxelCoord& Coord);

	// ==================== TASK SCHEDULING (CONCURRENCY-LIMITED) ====================

	/**
	 * Schedule chunk generation with concurrency limit and distance-based priority.
	 * Runs immediately if slots available, otherwise queues by distance.
	 */
	void ScheduleGeneration(UVoxelChunkComponent* Chunk);

	/** Called when generation finishes - launches next queued task if available. */
	void OnGenerationFinished(UVoxelChunkComponent* Chunk);

	/**
	 * Schedule chunk meshing with concurrency limit and distance-based priority.
	 * @param bSeamRemesh True if this is a seam-only remesh (reuses generation data)
	 */
	void ScheduleMeshing(UVoxelChunkComponent* Chunk, bool bSeamRemesh);

	/** Called when meshing finishes - launches next queued task if available. */
	void OnMeshingFinished(UVoxelChunkComponent* Chunk);

	// ==================== MESH COMPONENT POOLING ====================

	/** Acquire a ProceduralMeshComponent from pool (creates new if pool empty). */
	UProceduralMeshComponent* AcquirePMC(bool bForBucket = false);

	/** Return ProceduralMeshComponent to pool (clears data, hides). */
	void ReleasePMC(UProceduralMeshComponent* PMC);

	/** Acquire a RealtimeMeshComponent from pool (creates new if pool empty). */
	URealtimeMeshComponent* AcquireRMC(bool bForBucket = false);

	/** Return RealtimeMeshComponent to pool (clears data, hides). */
	void ReleaseRMC(URealtimeMeshComponent* RMC);

	/** Check if world is shutting down (used by chunks to cancel async operations). */
	FORCEINLINE bool IsShuttingDown() const { return bShuttingDown.Load(); }

private:
	// ==================== CHUNK STORAGE ====================

	/** Active visible LOD0/LOD1 chunks (promoted from Pending when Ready). */
	TMap<FVoxelCoord, UVoxelChunkComponent*> ActiveChunks;

	/** Pending chunks awaiting promotion (generation/meshing in progress). */
	TMap<FVoxelCoord, UVoxelChunkComponent*> PendingChunks;

	/** Active LOD2 macro-tiles (keyed by tile coordinates). */
	TMap<FIntPoint, UVoxelMacroTileComponent*> ActiveMacro;

	/** Pending macro-tiles awaiting promotion. */
	TMap<FIntPoint, UVoxelMacroTileComponent*> PendingMacro;

	// ==================== STREAMING SOURCE TRACKING ====================

	/** Cached list of active streaming sources (updated each frame). */
	TArray<UVoxelStreamingSourceComponent*> StreamingSources;

	/** Last known center chunk (for detecting movement). */
	bool bHasLastCenter = false;
	FVoxelCoord LastCenterChunk;

	// ==================== APPLY QUEUE (GAMETHREAD) ====================

	/**
	 * Pending mesh apply operation.
	 * Contains either owned buffers (fresh mesh) or uses chunk's cached buffers.
	 */
	struct FPendingApply
	{
		TWeakObjectPtr<UVoxelChunkComponent> Chunk;
		TUniquePtr<FMeshBuffers> OwnedBuffers; ///< Present for fresh meshes/remeshes
		bool bUseChunkCache = false;           ///< True for collision-only reapply
		bool bCollision = false;
		bool bWasSeamRemesh = false;
		int32 Sequence = 0;
	};

	FCriticalSection ApplyQueueMutex;
	TArray<FPendingApply> ApplyQueue;
	TQueue<FVoxelCoord> PendingSeamRemesh;

	// ==================== TASK SCHEDULING STATE ====================

	/** Current number of active generation tasks. */
	int32 ActiveGenTasks = 0;

	/** Current number of active meshing tasks. */
	int32 ActiveMeshTasks = 0;

	/** Generation wait queue (buckets by squared distance for priority). */
	TMap<int32, TArray<TWeakObjectPtr<UVoxelChunkComponent>>> GenWaitByDistance;

	/** Meshing wait queue (buckets by squared distance for priority). */
	TMap<int32, TArray<TWeakObjectPtr<UVoxelChunkComponent>>> MeshWaitByDistance;

	FCriticalSection GenMutex;
	FCriticalSection MeshMutex;

	// ==================== MESH COMPONENT POOL ====================

	TArray<UProceduralMeshComponent*> PMCPool;
	TArray<URealtimeMeshComponent*> RMCPool;

	// ==================== CHUNK BATCHING SYSTEM ====================

	/** Spatial buckets (only used when bEnableChunkBatching = true) */
	TMap<FIntVector, FVoxelChunkBucket*> ChunkBuckets;

	/** Bucket manager helper */
	FVoxelBucketManager BucketManager;

private:
	// ==================== INTERNAL HELPERS ====================

	/** Gather all active streaming sources in the world (called each frame). */
	void GatherStreamingSources();

	/**
	 * Drain apply queue up to MaxMeshAppliesPerTick (collision first).
	 * Processes pending seam remesh requests.
	 */
	void DrainApplyQueue();

	/** Promote chunks from Pending to Active when they reach Ready state. */
	void PromoteReadyPendings();

	/**
	 * Main chunk management loop - creates/tears down chunks and macro-tiles.
	 * Implements progressive loading with per-frame spawn budget.
	 */
	void UpdateChunks();

	/** Build list of desired chunks sorted by priority (distance + frustum). */
	void BuildDesiredChunkList(
		const FVoxelCoord& CenterChunk,
		const FVector& PlayerLocation,
		const FVector& PlayerForward,
		const FVector& PlayerForwardXY,
		const UVoxelSettings* Settings,
		const UVoxelStreamingSourceComponent* PrimarySource,
		int32 R2, int32 R2Sq, int32 Rz,
		float ChunkWorldSizeX, float ChunkWorldSizeY, float ChunkWorldSizeZ,
		bool bApplyFrustumPriority, float CosHalfHorizontalFOV, float CosHalfVerticalFOV,
		bool bDiskLoading, bool bFrustumVertical);

	/** Rebuild visibility cache from desired chunk list. */
	void RebuildVisibilityCache();

	/** Update chunk mesh visibility based on frustum/occlusion culling. */
	void UpdateChunkVisibility(
		UVoxelChunkComponent* Active,
		const FVoxelCoord& ChunkCoord,
		const FVector& PlayerLocation,
		const FVector& PlayerForward,
		const FVector& PlayerForwardXY,
		const UVoxelStreamingSourceComponent* PrimarySource,
		float ChunkWorldSizeX, float ChunkWorldSizeY, float ChunkWorldSizeZ,
		bool bApplyFrustumPriority, float CosHalfHorizontalFOV, float CosHalfVerticalFOV,
		bool bFrustumVertical);

	/** Process desired chunks (spawn new, update settings, handle LOD transitions). */
	void ProcessDesiredChunks(
		const FVoxelCoord& CenterChunk,
		const FVector& PlayerLocation,
		const FVector& PlayerForward,
		const FVector& PlayerForwardXY,
		const UVoxelSettings* Settings,
		const UVoxelStreamingSourceComponent* PrimarySource,
		int32 CollisionR, int32 CollisionR2, int32 CollisionDropR2,
		int32 AOR2, int32 SpawnBudget,
		float ChunkWorldSizeX, float ChunkWorldSizeY, float ChunkWorldSizeZ,
		bool bApplyFrustumPriority, float CosHalfHorizontalFOV, float CosHalfVerticalFOV,
		bool bFrustumVertical, bool bRotationChanged,
		TMap<FIntPoint, int32>& OutVisibleTilesMinD2,
		int32& OutMissingChunks);

	/** Update LOD2 macro-tiles (spawn, update, unload). */
	void UpdateMacroTiles(
		const UVoxelSettings* Settings,
		const TMap<FIntPoint, int32>& VisibleTilesMinD2);

	/** Unload chunks that are no longer visible. */
	void UnloadInvisibleChunks();

	// ==================== CHUNK BATCHING HELPERS ====================

	/** Get or create bucket for given chunk coordinate */
	struct FVoxelChunkBucket* GetOrCreateBucket(const FVoxelCoord& ChunkCoord);

	/** Mark bucket dirty when chunk changes */
	void MarkBucketDirty(const FVoxelCoord& ChunkCoord);

	/** Rebuild all dirty buckets (merge chunk meshes) */
	void RebuildDirtyBuckets();

	/** Remove chunk from its bucket */
	void RemoveChunkFromBucket(const FVoxelCoord& ChunkCoord);

	/** Rebuild single bucket mesh (merge all contained chunks) */
	void RebuildBucketMesh(struct FVoxelChunkBucket* Bucket);

	/** Convert world position to chunk coordinate. */
	FVoxelCoord WorldToChunkCoord(const FVector& Location) const;

	/** Select LOD level based on squared distance from player. */
	EVoxelLODLevel PickLOD(int32 Dist2, const UVoxelSettings* S) const;

	/**
	 * Pop closest chunk from priority queue (pops lowest-distance bucket).
	 * Skips invalid/stale entries automatically.
	 */
	static TWeakObjectPtr<UVoxelChunkComponent> PopClosest(TMap<int32, TArray<TWeakObjectPtr<UVoxelChunkComponent>>>& QueueByDistance);

	/** Floor division for tile coordinate computation (handles negative correctly). */
	static int32 FloorDiv(int32 A, int32 B)
	{
		const int32 q = A / B;
		const int32 r = A % B;
		return q - ((r != 0) && ((r < 0) != (B < 0)));
	}

	// ==================== PROGRESSIVE LOADING STATE ====================

	/** True if more chunks need spawning (hit budget this frame). */
	bool bNeedsMoreSpawning = false;

	/** Number of chunks spawned this frame (limited by budget). */
	int32 ChunksSpawnedThisFrame = 0;

	/** True once all initial chunks have been spawned. */
	bool bInitialLoadComplete = false;

	/** True during EndPlay (used by chunks to cancel async ops). */
	TAtomic<bool> bShuttingDown{ false };

	/** Frame counter for live noise preview (reloads chunks periodically). */
	int32 NoisePreviewFrameCounter = 0;

	// ==================== CHUNK UPDATE CACHE (OPTIMIZATION) ====================
	// These were previously static variables (caused memory leak + multi-world bugs).
	// Now instance members to properly scope lifetime and support multiple worlds.

	/** Cached list of desired chunks with priority (rebuilt when player moves/rotates). */
	TArray<TPair<FVoxelCoord, float>> CachedDesiredChunks;

	/** Cached set of visible chunks (used for fast lookup during unload pass). */
	TSet<FVoxelCoord> CachedVisibleChunks;

	/** Last center chunk coordinate used for cache (for detecting movement). */
	FVoxelCoord CachedLastDesiredCenter = FVoxelCoord(INT32_MAX, INT32_MAX, INT32_MAX);

	/** Last player yaw rotation used for cache (for detecting rotation changes). */
	float CachedLastPlayerYaw = 0.0f;

	/** True if visibility cache is still valid this frame. */
	bool bVisibilityCacheValid = false;

	/** Temporary array for macro-tile removal (reused to avoid allocations). */
	TArray<FIntPoint> TempRemoveTiles;

	/** Temporary array for chunk removal (reused to avoid allocations). */
	TArray<FVoxelCoord> TempRemoveChunks;
};
