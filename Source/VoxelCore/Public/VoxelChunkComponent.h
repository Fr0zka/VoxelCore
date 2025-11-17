#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "VoxelStructs.h"
#include "VoxelMesher.h"
#include <atomic>
#include "RealtimeMeshComponent.h"
#include "VoxelMaterialSet.h"
#include "VoxelChunkComponent.generated.h"

class UProceduralMeshComponent;
class UVoxelSettings;
class AVoxelWorld;
class URealtimeMeshComponent;


UENUM()
enum class EVoxelChunkState : uint8
{
    Empty,
    Generating,
    Meshing,
    Ready,
    Unloading
};

UENUM()
enum class EVoxelRenderMode : uint8
{
    Voxels,
    Heightfield
};

UCLASS()
class VOXELCORE_API UVoxelChunkComponent : public UActorComponent
{
    GENERATED_BODY()

public:
    void InitializeChunk(
        FVoxelCoord InCoord,
        const UVoxelSettings* InSettings,
        AVoxelWorld* InWorld,
        EVoxelLODLevel InLOD,
        bool bInBuildCollision,
        bool bInUseAO);

    // Requests
    void RequestRemesh();
    void RequestCollisionReapply(bool bNewCollision);

    void CancelPendingTask();
    void UnloadChunk();

    // State/query
    EVoxelChunkState State = EVoxelChunkState::Empty;

    FVoxelCoord              ChunkCoord;
    const UVoxelSettings* Settings = nullptr;
    AVoxelWorld* OwnerWorld = nullptr;

    bool bBuildCollision = false;
    bool bUseAO = true;
    EVoxelLODLevel LOD = EVoxelLODLevel::LOD0;
    int32 LODScaleXY = 1;
    EVoxelRenderMode RenderMode = EVoxelRenderMode::Voxels;

    // Scheduler priority (distance^2), set by world each UpdateChunks
    int32 PriorityDist2 = TNumericLimits<int32>::Max();

    // World apply hooks
    bool IsCancelPending() const { return bCancelPending; }
    void ApplyBuffersToMesh(const FMeshBuffers& Bufs, bool bCollision);
    const FMeshBuffers* GetCachedBuffers() const { return CachedBuffers.Get(); }
    void OnMeshApplied(TUniquePtr<FMeshBuffers>&& AppliedBuffers, bool bWasSeamRemesh);
    void OnCollisionReapplied();

    // Allow world to call (virtual so subclasses can override)
    virtual void DoGeneration();
    virtual void DoMeshing(bool bSeamRemesh);
    bool bSeamRemeshQueued = false;

    struct FCachedNeighborBorders
    {
        TArray<EVoxelBlockID> XNeg, XPos, YNeg, YPos, ZNeg, ZPos;
        bool bHasXNeg = false, bHasXPos = false;
        bool bHasYNeg = false, bHasYPos = false;
        bool bHasZNeg = false, bHasZPos = false;
    };
    bool bNeighborsCacheDirty = true;

    // OPTIMIZATION: Track hash of neighbor borders to skip unnecessary seam remeshes
    uint32 LastNeighborHash = 0;

protected:
    // Data
    // Compact category data for voxels.  Each voxel stores a 2‑bit category:
    // 0 = air, 1 = semi‑solid (e.g. water/leaves), 2 = solid.  This replaces
    // the previous per‑voxel EVoxelBlockID array and significantly reduces
    // memory usage.  During meshing the categories are expanded back into
    // temporary block IDs.
    FCategoryBitset CategoryData;
    FBiomeGrid2D BiomeGrid;
    // Protects CategoryData and HeightData
    FCachedNeighborBorders CachedNeighborBorders;
    bool bNeighborBordersCached = false;

    // Heightfield samples for LOD2.  Unchanged.
    TArray<int32>            HeightData;
    int32 HF_SamplesX = 0;               // LOD2 grid width
    int32 HF_SamplesY = 0;               // LOD2 grid height

    // Cached neighbors (optimization - snapshot once, reuse)
    FChunkNeighbors CachedNeighbors;

    // Components (from world pool)
    UProceduralMeshComponent* PMC = nullptr;
    URealtimeMeshComponent* RMC = nullptr;
    bool bUsingRMC = false;

    // Async control
    FThreadSafeBool bCancelPending = false;
    bool bIsMeshing = false;
    bool bHasAnnouncedReady = false;

    // Cache
    TUniquePtr<FMeshBuffers>  CachedBuffers;

    // Pipeline
    void StartGeneration();               // asks world scheduler
    void OnGenerationComplete();          // GT -> then asks meshing
    void StartMeshing(bool bSeamRemesh);  // asks world scheduler

    void SnapshotNeighbors(FChunkNeighbors& Out) const;

    void CacheNeighborBordersFromWorld();

    void EnsureVoxelMaterial_RMC(URealtimeMeshComponent* inRMC, const UVoxelSettings* inSettings, const UVoxelMaterialSet* MatSet);


    // Components
    void CreateMeshComponent();
    void DestroyMeshComponent();

    // Meshing/apply sequencing to avoid out-of-order GPU results overwriting newer meshes
public:
    int32 BeginMeshingSequence() { return ++MeshingSeqCounter; }
    void MarkAppliedSequence(int32 Seq) { LastAppliedSeq = FMath::Max(LastAppliedSeq, Seq); }
    int32 GetLastAppliedSequence() const { return LastAppliedSeq; }

    // Mesh component access (for visibility culling)
    FORCEINLINE UProceduralMeshComponent* GetPMC() const { return PMC; }
    FORCEINLINE URealtimeMeshComponent* GetRMC() const { return RMC; }
    FORCEINLINE bool IsUsingRMC() const { return bUsingRMC; }

private:
    int32 MeshingSeqCounter = 0;
    int32 LastAppliedSeq = -1;
    UPROPERTY(Transient) TObjectPtr<UMaterialInstanceDynamic> VoxelMID;

};
