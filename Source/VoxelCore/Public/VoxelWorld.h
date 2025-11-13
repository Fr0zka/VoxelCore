#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "VoxelStructs.h"
#include "VoxelSettings.h"
#include "VoxelWorld.generated.h"

class UVoxelChunkComponent;
class UVoxelMacroTileComponent;
class UProceduralMeshComponent;
struct FMeshBuffers;

class URealtimeMeshComponent;

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
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel")
    TSubclassOf<UVoxelSettings> Settings;

    // Max CreateMeshSection applies per tick (collision prioritized)
    UPROPERTY(EditAnywhere, Category = "Voxel|Performance", meta = (ClampMin = "1", ClampMax = "64"))
    int32 MaxMeshAppliesPerTick = 4;

    // Called by chunks on first Ready (not for seam-only remesh)
    void OnChunkReady(const FVoxelCoord& Coord);

    // Lookup active LOD0/LOD1 chunk at coord
    UVoxelChunkComponent* GetChunk(const FVoxelCoord& Coord) const;

    // Worker thread: enqueue a fresh mesh apply (world owns buffers during apply)
    void EnqueueMeshApply(UVoxelChunkComponent* Chunk, FMeshBuffers&& Buffers, bool bCreateCollision, bool bWasSeamRemesh, int32 Sequence);

    // GT: enqueue collision-only reapply using cached buffers
    void EnqueueReapplyUsingCache(UVoxelChunkComponent* Chunk, bool bCreateCollision);

    void MarkChunkForSeamRemesh(const FVoxelCoord& C);

    // === Schedulers (concurrency-limited with priority queues) ===
    void ScheduleGeneration(UVoxelChunkComponent* Chunk);
    void OnGenerationFinished(UVoxelChunkComponent* Chunk);

    void ScheduleMeshing(UVoxelChunkComponent* Chunk, bool bSeamRemesh);
    void OnMeshingFinished(UVoxelChunkComponent* Chunk);

    // === Mesh component pool ===
    UProceduralMeshComponent* AcquirePMC();
    void ReleasePMC(UProceduralMeshComponent* PMC);

    URealtimeMeshComponent* AcquireRMC();
    void ReleaseRMC(URealtimeMeshComponent* RMC);
    FORCEINLINE bool IsShuttingDown() const { return bShuttingDown.Load(); }


private:
    // Active visible LOD0/LOD1 chunks
    TMap<FVoxelCoord, UVoxelChunkComponent*> ActiveChunks;

    // Pending chunks for gated LOD swap (LOD0/1)
    TMap<FVoxelCoord, UVoxelChunkComponent*> PendingChunks;

    // LOD2 macro-tiles (keyed by tile coords)
    TMap<FIntPoint, UVoxelMacroTileComponent*> ActiveMacro;
    TMap<FIntPoint, UVoxelMacroTileComponent*> PendingMacro;

    // Last player center
    bool bHasLastCenter = false;
    FVoxelCoord LastCenterChunk;

    // Apply queue (GT)
    struct FPendingApply
    {
        TWeakObjectPtr<UVoxelChunkComponent> Chunk;
        TUniquePtr<FMeshBuffers> OwnedBuffers; // present for fresh meshes/remeshes
        bool bUseChunkCache = false;           // true for collision-only reapply
        bool bCollision = false;
        bool bWasSeamRemesh = false;
        int32 Sequence = 0;
    };

    FCriticalSection ApplyQueueMutex;
    TArray<FPendingApply> ApplyQueue;
    TQueue< FVoxelCoord>PendingSeamRemesh;

    // Scheduling state with priority queues (distance^2 -> chunks)
    int32 ActiveGenTasks = 0;
    int32 ActiveMeshTasks = 0;
    TMap<int32, TArray<TWeakObjectPtr<UVoxelChunkComponent>>> GenWaitByDistance;
    TMap<int32, TArray<TWeakObjectPtr<UVoxelChunkComponent>>> MeshWaitByDistance;

    FCriticalSection GenMutex;
    FCriticalSection MeshMutex;

    // Mesh component pool
    TArray<UProceduralMeshComponent*> PMCPool;
    TArray<URealtimeMeshComponent*> RMCPool;


private:
    void DrainApplyQueue();      // up to MaxMeshAppliesPerTick (collision first)
    void PromoteReadyPendings(); // gated LOD swap for chunks

    void UpdateChunks();         // create/teardown (chunks + macro-tiles)
    FVoxelCoord WorldToChunkCoord(const FVector& Location) const;

    EVoxelLODLevel PickLOD(int32 Dist2, const UVoxelSettings* S) const;

    // Helper: pop lowest-distance item from priority queue
    static TWeakObjectPtr<UVoxelChunkComponent> PopClosest(TMap<int32, TArray<TWeakObjectPtr<UVoxelChunkComponent>>>& QueueByDistance);

    // Helpers
    static int32 FloorDiv(int32 A, int32 B) { const int32 q = A / B; const int32 r = A % B; return q - ((r != 0) && ((r < 0) != (B < 0))); }
    bool bNeedsMoreSpawning = false;
    int32 ChunksSpawnedThisFrame = 0;
    bool bInitialLoadComplete = false;
    TAtomic<bool> bShuttingDown{ false };
    int32 NoisePreviewFrameCounter = 0;


};
