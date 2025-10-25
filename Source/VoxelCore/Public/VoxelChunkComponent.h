#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "VoxelStructs.h"
#include "VoxelMesher.h"
#include "VoxelChunkComponent.generated.h"

class UProceduralMeshComponent;
class UVoxelSettings;
class AVoxelWorld;

#if WITH_RUNTIME_MESHCOMPONENT
class URuntimeMeshComponent;
#endif

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


    // --- Mesh section state (keeps apply cheap) ---
public:
    int32 MeshSectionIndex = 0;
    bool  bSectionCreated = false;          // did we call CreateMeshSection yet?
    bool  bSectionHasCollision = false;     // was the section created with collision?
    int32 LastVertCount = 0;
    int32 LastIndexCount = 0;
    uint32 LastIndexHash = 0;   // NEW


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

protected:
    // Data
    TArray<EVoxelBlockID>    VoxelData;   // LOD0/1
    TArray<int32>            HeightData;  // LOD2
    int32 HF_SamplesX = 0;               // LOD2 grid width
    int32 HF_SamplesY = 0;               // LOD2 grid height

    // Components (from world pool)
    UProceduralMeshComponent* PMC = nullptr;
#if WITH_RUNTIME_MESHCOMPONENT
    URuntimeMeshComponent* RMC = nullptr;
#endif
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

    // Components
    void CreateMeshComponent();
    void DestroyMeshComponent();
    // In UVoxelChunkComponent (private or protected)
    FCompactVoxelData Compact; // used for LOD0 and LOD1 when sparse is enabled

    // Converts current dense VoxelData → Compact (air not stored). Safe to call many times.
    void ConvertDenseToCompact(bool bForLOD0);

    // Edits
    bool SetVoxelCompact(int32 X, int32 Y, int32 Z, uint16 Id); // returns true if changed
    bool ClearVoxelCompact(int32 X, int32 Y, int32 Z);          // returns true if changed

};
