// VoxelContentManager.h
// Per-chunk content population: deterministic decoration/actor scatter from the
// strate content pools, plus self-contained aesthetic water surfaces.
//
// LIFECYCLE (driven by AVoxelWorld on the GAME THREAD):
//   - PopulateChunk(coord, meshdata) after a chunk's mesh is applied
//   - ClearChunk(coord) when a chunk unloads / is re-meshed
//   - ClearAll() on regenerate / season reset
//   - SetActiveStrate(playerStrate) each Tick (no-op unless the strate changed)
//
// DETERMINISM: every placement decision is a pure hash of (chunk, surface index,
// entry index, seed), so the same world re-populates identically. Spawning itself
// must run on the game thread (UWorld::SpawnActor), which it does — ApplyMeshToChunk
// is game-thread.
//
// RENDERING PATHS per decoration entry (FStrateDecoration):
//   - ActorClass     → real actors (lights, logic, interaction). Keep MaxLODLevel=0.
//   - InstancedMesh  → HISM instances batched per chunk: no tick, no per-actor cost,
//                      engine-culled. Safe to allow at LOD 1-2 so visual props don't
//                      pop out with LOD0 (emissive materials still glow at distance).
//
// STRATE LIGHT CULLING: a light in another strate can never legitimately reach the
// player (seals + bedrock gap are solid), but a shadowless light BLEEDS through rock
// and a shadowed one pays full cost to render black. So light components on spawned
// decoration actors are only visible while the player is in the SAME strate — the
// analytical form of occlusion culling, computed once per strate change.

#pragma once

#include "CoreMinimal.h"
#include "VoxelTypes.h"
#include "VoxelStrateTypes.h"   // FStrateDecoration (resolved per dominant biome)
#include "VoxelContentManager.generated.h"

class UVoxelStrateManager;
class UVoxelStrateDefinition;
class UVoxelGenerator;
class UStaticMesh;
class UStaticMeshComponent;
class UHierarchicalInstancedStaticMeshComponent;
class UMaterialInterface;

UCLASS()
class VOXELFORGE_API UVoxelContentManager : public UObject
{
    GENERATED_BODY()

public:
    /** Wire up services. Owner is the AVoxelWorld actor that owns spawned content.
     *  Generator supplies the dominant-biome query for per-biome content selection. */
    void Initialize(AActor* InOwner, UVoxelStrateManager* InStrateManager,
                    UVoxelGenerator* InGenerator, int32 InSeed);

    /** Update the seed used for placement hashing (season reset). */
    void SetSeed(int32 InSeed) { Seed = InSeed; }

    /** Populate decorations + water for a chunk. Clears any previous content first.
     *  Each decoration entry spawns only while LOD <= its MaxLODLevel; water at any LOD. */
    void PopulateChunk(const FIntVector& ChunkCoord, const FVoxelMeshData& MeshData, int32 LODLevel = 0);

    /** Destroy all spawned content (actors + instances + water plane) for one chunk. */
    void ClearChunk(const FIntVector& ChunkCoord);

    /** Destroy all spawned content for every chunk. */
    void ClearAll();

    /** Player strate changed → toggle decoration lights (strict: lights only live in the
     *  player's strate). Cheap no-op while the strate stays the same — call every Tick. */
    void SetActiveStrate(int32 PlayerStrateIndex);

private:
    void SpawnDecorations(const FIntVector& ChunkCoord, const FVoxelMeshData& MeshData,
                          const TArray<FStrateDecoration>& Decorations, int32 LODLevel,
                          TArray<TWeakObjectPtr<AActor>>& Out);
    void SpawnWater(const FIntVector& ChunkCoord, const UVoxelStrateDefinition* Def,
                    UMaterialInterface* WaterMaterial);

    /** Strate index a chunk belongs to (center-Z lookup). */
    int32 GetChunkStrateIndex(const FIntVector& ChunkCoord) const;

    /** Show/hide every light component on a decoration actor. */
    static void SetActorLightsEnabled(AActor* Actor, bool bEnabled);

    TWeakObjectPtr<AActor> Owner;

    UPROPERTY()
    UVoxelStrateManager* StrateManager = nullptr;

    UPROPERTY()
    UVoxelGenerator* Generator = nullptr;

    int32 Seed = 0;

    // Player's current strate (INT32_MIN until first SetActiveStrate). Lights on spawned
    // actors are enabled iff their chunk's strate matches this.
    int32 ActiveStrateIndex = INT32_MIN;

    // Engine unit plane (/Engine/BasicShapes/Plane) reused for every water surface.
    UPROPERTY()
    UStaticMesh* PlaneMesh = nullptr;

    // Spawned decoration/ambient actors per chunk (weak — they live in the level).
    TMap<FIntVector, TArray<TWeakObjectPtr<AActor>>> SpawnedActors;

    // HISM components per chunk (weak — registered components are owned by the actor).
    TMap<FIntVector, TArray<TWeakObjectPtr<UHierarchicalInstancedStaticMeshComponent>>> ChunkInstances;

    // Water surface component per chunk.
    UPROPERTY()
    TMap<FIntVector, UStaticMeshComponent*> WaterPlanes;
};
