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
// ---- Voxel profiling stat group ----
DECLARE_STATS_GROUP(TEXT("Voxel"), STATGROUP_Voxel, STATCAT_Advanced);
DECLARE_CYCLE_STAT(TEXT("VW DrainApplyQueue"), STAT_Voxel_DrainApplyQueue, STATGROUP_Voxel);
DECLARE_CYCLE_STAT(TEXT("VW ApplyOne"), STAT_Voxel_ApplyOne, STATGROUP_Voxel);
DECLARE_CYCLE_STAT(TEXT("VW PromotePendings"), STAT_Voxel_Promote, STATGROUP_Voxel);
DECLARE_CYCLE_STAT(TEXT("Chunk ApplyBuffersToMesh"), STAT_Voxel_Chunk_ApplyMesh, STATGROUP_Voxel);
DECLARE_CYCLE_STAT(TEXT("Chunk OnMeshApplied"), STAT_Voxel_Chunk_OnApplied, STATGROUP_Voxel);
DECLARE_CYCLE_STAT(TEXT("Mesher Greedy (CPU)"), STAT_Voxel_GreedyCPU, STATGROUP_Voxel);
DECLARE_CYCLE_STAT(TEXT("Mesher Greedy Fast LOD 1(CPU)"), STAT_Voxel_GreedyMesh_FastLOD1, STATGROUP_Voxel);



#if WITH_RUNTIME_MESHCOMPONENT
class URuntimeMeshComponent;
#endif

UCLASS()
class VOXELCORE_API AVoxelWorld : public AActor
{
    GENERATED_BODY()

public:
    AVoxelWorld();

protected:
    virtual void BeginPlay() override;
    virtual void Tick(float DeltaTime) override;

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
    void EnqueueMeshApply(UVoxelChunkComponent* Chunk, FMeshBuffers&& Buffers, bool bCreateCollision, bool bWasSeamRemesh);

    // GT: enqueue collision-only reapply using cached buffers
    void EnqueueReapplyUsingCache(UVoxelChunkComponent* Chunk, bool bCreateCollision);

    void DrainSpawnQueue();

    // === Schedulers (concurrency-limited) ===
    void ScheduleGeneration(UVoxelChunkComponent* Chunk);
    void OnGenerationFinished(UVoxelChunkComponent* Chunk);

    void ScheduleMeshing(UVoxelChunkComponent* Chunk, bool bSeamRemesh);
    void OnMeshingFinished(UVoxelChunkComponent* Chunk);

    // === Mesh component pool ===
    UProceduralMeshComponent* AcquirePMC();
    void ReleasePMC(UProceduralMeshComponent* PMC);

#if WITH_RUNTIME_MESHCOMPONENT
    URuntimeMeshComponent* AcquireRMC();
    void ReleaseRMC(URuntimeMeshComponent* RMC);
#endif
    const TArray<int32>& GetSharedGridIB(int32 SamplesX, int32 SamplesY);


private:
    FTimerHandle Handle;
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
    double AvgApplyMs = 0.5;        // rolling average per apply
    double TargetGTMsForApplies = 6.0; // aim to spend this many ms/frame on applies
    float  ApplyEMA = 0.2f;         // smoothing factor
    struct FPendingApply
    {
        TWeakObjectPtr<UVoxelChunkComponent> Chunk;
        TUniquePtr<FMeshBuffers> OwnedBuffers; // present for fresh meshes/remeshes
        bool bUseChunkCache = false;           // true for collision-only reapply
        bool bCollision = false;
        bool bWasSeamRemesh = false;
    };
    // ----- Smooth bootstrap / spawn budgeting -----
    struct FPendingSpawn
    {
        FVoxelCoord Coord;
        int32       Dist2 = 0;
        EVoxelLODLevel LOD = EVoxelLODLevel::LOD0;
        bool        bCollision = false;
        bool        bAO = false;
    };

    TArray<FPendingSpawn> SpawnQueue;     // chunks we want to spawn, but not this frame
    int32 MaxChunkSpawnsPerTick = 12;     // how many *new* chunk components to create per frame
    int32 MaxPMCsPrewarmPerTick = 32;     // how many PMCs to prewarm per frame (instead of 512 at once)

    FCriticalSection ApplyQueueMutex;
    TArray<FPendingApply> ApplyQueue;

    // Scheduling state
    int32 ActiveGenTasks = 0;
    int32 ActiveMeshTasks = 0;
    TArray<TWeakObjectPtr<UVoxelChunkComponent>> GenWait;
    TArray<TWeakObjectPtr<UVoxelChunkComponent>> MeshWait;

    FCriticalSection GenMutex;
    FCriticalSection MeshMutex;

    // Mesh component pool
    TArray<UProceduralMeshComponent*> PMCPool;
#if WITH_RUNTIME_MESHCOMPONENT
    TArray<URuntimeMeshComponent*> RMCPool;
#endif

private:
    void DrainApplyQueue();      // up to MaxMeshAppliesPerTick (collision first)
    void PromoteReadyPendings(); // gated LOD swap for chunks

    void UpdateChunks();         // create/teardown (chunks + macro-tiles)
    FVoxelCoord WorldToChunkCoord(const FVector& Location) const;

    EVoxelLODLevel PickLOD(int32 Dist2, const UVoxelSettings* S) const;

    // Helper: pop lowest-distance item safely (returns pointer and removes it)
    static TWeakObjectPtr<UVoxelChunkComponent> PopClosest(TArray<TWeakObjectPtr<UVoxelChunkComponent>>& List);

    // Helpers
    static int32 FloorDiv(int32 A, int32 B) { const int32 q = A / B; const int32 r = A % B; return q - ((r != 0) && ((r < 0) != (B < 0))); }

    double SmoothedFrameMs = 16.0;
    int32 DynMaxGen = 2;
    int32 DynMaxMesh = 2;

    // Rolling averages
    double ExpAvgGenMs = 1.0;
    double ExpAvgMeshMs = 1.0;
    // Accessors replace reading Settings directly
    FORCEINLINE int32 GetMaxGenTasks() const { return FMath::Clamp(DynMaxGen, 1, Settings.GetDefaultObject()->MaxMeshHardCap); }
    FORCEINLINE int32 GetMaxMeshTasks() const { return FMath::Clamp(DynMaxMesh, 1, Settings.GetDefaultObject()->MaxMeshHardCap); }

    // Called each Tick
    void UpdateDynamicBudgets(float DeltaTime);
    bool bFreezeFar = false;
    int32 FrozenFromRing = 0; // ring index counted from outermost inward

    bool ShouldDeferFar(const FVoxelCoord& Center, const FVoxelCoord& C, int32 VisibleRadiusSq) const;
};
