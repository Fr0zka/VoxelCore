// VoxelContentManager.h
// Per-chunk content population: deterministic decoration/actor scatter from the
// strate content pools, plus self-contained aesthetic water surfaces.
//
// LIFECYCLE (driven by AVoxelWorld on the GAME THREAD):
//   - PopulateChunk(coord, meshdata) after a chunk's mesh is applied
//   - ClearChunk(coord) when a chunk unloads / is re-meshed
//   - ClearAll() on regenerate / season reset
//
// DETERMINISM: every placement decision is a pure hash of (chunk, surface index,
// entry index, seed), so the same world re-populates identically. Spawning itself
// must run on the game thread (UWorld::SpawnActor), which it does — ApplyMeshToChunk
// is game-thread.

#pragma once

#include "CoreMinimal.h"
#include "VoxelTypes.h"
#include "VoxelContentManager.generated.h"

class UVoxelStrateManager;
class UVoxelStrateDefinition;
class UStaticMesh;
class UStaticMeshComponent;

UCLASS()
class VOXELFORGE_API UVoxelContentManager : public UObject
{
    GENERATED_BODY()

public:
    /** Wire up services. Owner is the AVoxelWorld actor that owns spawned content. */
    void Initialize(AActor* InOwner, UVoxelStrateManager* InStrateManager, int32 InSeed);

    /** Update the seed used for placement hashing (season reset). */
    void SetSeed(int32 InSeed) { Seed = InSeed; }

    /** Populate decorations + water for a chunk. Clears any previous content first.
     *  Decorations are only scattered at LOD 0 (near chunks); water is placed at any LOD. */
    void PopulateChunk(const FIntVector& ChunkCoord, const FVoxelMeshData& MeshData, int32 LODLevel = 0);

    /** Destroy all spawned content (actors + water plane) for one chunk. */
    void ClearChunk(const FIntVector& ChunkCoord);

    /** Destroy all spawned content for every chunk. */
    void ClearAll();

private:
    void SpawnDecorations(const FIntVector& ChunkCoord, const FVoxelMeshData& MeshData,
                          const UVoxelStrateDefinition* Def, TArray<TWeakObjectPtr<AActor>>& Out);
    void SpawnWater(const FIntVector& ChunkCoord, const UVoxelStrateDefinition* Def);

    TWeakObjectPtr<AActor> Owner;

    UPROPERTY()
    UVoxelStrateManager* StrateManager = nullptr;

    int32 Seed = 0;

    // Engine unit plane (/Engine/BasicShapes/Plane) reused for every water surface.
    UPROPERTY()
    UStaticMesh* PlaneMesh = nullptr;

    // Spawned decoration/ambient actors per chunk (weak — they live in the level).
    TMap<FIntVector, TArray<TWeakObjectPtr<AActor>>> SpawnedActors;

    // Water surface component per chunk.
    UPROPERTY()
    TMap<FIntVector, UStaticMeshComponent*> WaterPlanes;
};
