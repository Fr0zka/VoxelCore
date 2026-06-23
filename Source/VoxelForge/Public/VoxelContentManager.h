// VoxelContentManager.h
// Decoration scatter (distance-based world grid, ASYNC surface march) + aesthetic water surfaces.
//
// TWO INDEPENDENT SUBSYSTEMS:
//
// 1) DECORATIONS — distance-based WORLD GRID (the no-pop system).
//    Decorations are placed on a fixed world XY cell grid (1 cell = 1 chunk footprint) and streamed
//    by DISTANCE from the player, completely decoupled from clipmap tiles / LOD. Each candidate column
//    is ray-marched vertically through the player's strate Z-band via UVoxelGenerator::GetDensityAt and
//    snapped to the real surface crossings. Every placement is a pure hash of (cell, column, crossing,
//    entry, seed) + the density surface snap → a prop sits at the SAME world position no matter which
//    LOD tile meshes the ground under it: NO pop/teleport on LOD swaps.
//
//    THREADING (critical): the column ray-march is EXPENSIVE (many GetDensityAt per column). It runs on
//    a WORKER thread (UE::Tasks, like mesh gen — GetDensityAt is thread-safe), producing a list of spawn
//    commands. Only the SpawnActor/AddInstance (which MUST be game-thread) happens on the game thread,
//    budgeted. The game thread never does decoration density math. Flow (UpdateDecorations each Tick):
//      - recompute desired cell set on player cell-boundary / strate change
//      - LaunchDecoTasks: fire async march tasks (capped by MaxConcurrentDecorationTasks)
//      - ProcessDecoResults: drain finished tasks' results, apply (spawn) budgeted, epoch-guarded
//    Decorations exist ONLY in the player's current strate (march is strate-bounded) → a strate change
//    wipes + rebuilds them, and there is no cross-strate light bleed to cull.
//
// 2) WATER — ONE strate-global ocean plane that follows the player (UpdateWater). Terrain pokes
//    through it, so it reads as water at every LOD / to the horizon with no per-tile gaps. One draw.
//
// RENDERING PATHS / DISTANCE TIERS per entry (FStrateDecoration): non-instanced ActorClass entries with
// MaxLODLevel==0 are near-only (DecorationActorRadiusChunks — pricey actors stay close); InstancedMesh
// (HISM) entries + MaxLODLevel>=1 actor entries are any-distance (DecorationRadiusChunks).
//
// DETERMINISM: same seed + world ⇒ identical placement. Spawning runs on the game thread.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Queue.h"
#include "VoxelTypes.h"
#include "VoxelStrateTypes.h"   // FStrateDecoration (resolved per dominant biome)
#include <atomic>
#include "VoxelContentManager.generated.h"

class UVoxelStrateManager;
class UVoxelStrateDefinition;
class UVoxelGenerator;
class UVoxelSettings;
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
     *  Generator supplies GetDensityAt (surface snapping) + GetDominantBiomeAt (per-biome content).
     *  Settings supplies the decoration grid tunables (radii / spacing / march / budget). */
    void Initialize(AActor* InOwner, UVoxelStrateManager* InStrateManager,
                    UVoxelGenerator* InGenerator, UVoxelSettings* InSettings, int32 InSeed);

    /** Update the seed used for placement hashing (season reset). */
    void SetSeed(int32 InSeed) { Seed = InSeed; }

    //--- WATER (single strate-global ocean plane, follows the player) ---------
    /** Maintain ONE large water plane at the player strate's water level, centred on the player
     *  (snapped to a coarse grid). Terrain pokes through it, so it reads as water at EVERY LOD and
     *  out to the horizon with no per-tile gaps (the old per-tile planes only appeared on tiles that
     *  had terrain geometry in the water band → gaps over deep water + level-0 only). One draw, cheap.
     *  Call every Tick. Uses the strate's WaterMaterial (per-biome override not applied to the global
     *  plane). */
    void UpdateWater(const FVector& PlayerWorldPos);

    //--- DECORATIONS (distance-based world grid, async march) ----------------
    /** Stream decoration cells around the player: recompute the desired set on cell/strate change,
     *  launch async march tasks (capped), and apply finished results budgeted. Call every Tick. */
    void UpdateDecorations(const FVector& PlayerWorldPos);

    /** Destroy all spawned content (decorations + water). Regenerate / season reset. Bumps the deco
     *  epoch so any in-flight march tasks' results are discarded. */
    void ClearAll();

    virtual void BeginDestroy() override;   // flag shutdown so in-flight march tasks don't touch us

    /** Flag shutdown + block until in-flight march tasks drain. Call from AVoxelWorld::EndPlay BEFORE
     *  UObject teardown (worker tasks read the Generator). */
    void NotifyShutdown();

    //--- async-task plumbing (public so the worker lambda can reach them) -----
    /** One placement decided off-thread; spawned on the game thread from FDecoCellResult::Entries. */
    struct FDecoSpawn
    {
        int32      EntryIdx = 0;
        bool       bInstanced = false;
        FTransform Xf = FTransform::Identity;
    };
    /** A finished cell march: a snapshot of the resolved decoration list + the spawn commands. */
    struct FDecoCellResult
    {
        FIntPoint Cell    = FIntPoint::ZeroValue;
        uint32    BuildId = 0;                // identity of the region build this cell belongs to
        TArray<FStrateDecoration> Entries;   // snapshot the game thread spawns from (by EntryIdx)
        TArray<FDecoSpawn>        Spawns;
    };

private:
    // Per-REGION spawned content (weak — actors live in the level, components owned by the owner actor).
    // A region groups RxR cells (DecorationRegionSizeCells); ALL placements in the region share ONE HISM
    // per mesh, so the render thread walks ~R^2 fewer components. Regions load/unload as a unit, so
    // clearing is a plain DestroyComponent — no per-instance RemoveInstances index remapping.
    struct FDecoRegionContent
    {
        TArray<TWeakObjectPtr<AActor>> Actors;
        TArray<TWeakObjectPtr<UHierarchicalInstancedStaticMeshComponent>> Instances;
    };

    // Instanced transforms for one mesh, accumulated across all of a region's cells → one batched HISM.
    struct FRegionMeshBucket
    {
        FStrateDecoration  Deco;      // representative entry (mesh + HISM render tuning: cull/shadow/scale)
        TArray<FTransform> Xforms;
    };
    // A non-instanced actor placement, spawned when the region is applied.
    struct FRegionActorSpawn
    {
        TSubclassOf<AActor> ActorClass;
        FTransform          Xf = FTransform::Identity;
    };
    // A region being assembled: cell march results merged in as they land; APPLIED (HISMs built, actors
    // spawned) only once every cell has reported, so the whole region is one batched build per mesh.
    struct FDecoRegionBuild
    {
        TMap<TWeakObjectPtr<UStaticMesh>, FRegionMeshBucket> MeshBuckets;
        TArray<FRegionActorSpawn> ActorSpawns;
        int32  CellsRemaining = 0;    // cells still to account for before this region can apply
        uint32 BuildId        = 0;    // unique, monotonic — stale in-flight cell results fail to match
    };

    // Constant per-update strate context (a strate is a horizontal slab → same for every cell). Carries
    // only PODs/Z-bounds so it is safe to copy into a worker task (no UObject deref on the worker).
    struct FDecoContext
    {
        const UVoxelStrateDefinition* Def = nullptr;   // game-thread only (biome/decoration resolve)
        int32  RepChunkZ      = 0;          // representative chunk-Z for biome lookups
        float  TopVoxelZ      = 0.0f;       // strate band, voxel coords (march range)
        float  BottomVoxelZ   = 0.0f;
        float  WaterLocalZ    = -FLT_MAX;   // water surface, actor-local cm (-FLT_MAX = no water)
        bool   bHasWater      = false;
        bool   bSurfaceWorld  = false;      // heightfield archetype → use the GetSurfaceHeightAt oracle
    };

    /** WORKER-THREAD surface find → fills OutSpawns for one cell. SurfaceWorld uses the height oracle
     *  (cheap, O(1)/column); other archetypes ray-march the density column. No UObject access except
     *  Generator (thread-safe). Determinism-critical. */
    static void BuildCellSpawns(const UVoxelGenerator* Gen, const FTransform& OwnerXf,
                                const FIntPoint& Cell, const FDecoContext& Ctx,
                                const TArray<FStrateDecoration>& Entries, uint32 InSeed,
                                int32 Spacing, float Step, int32 MaxCrossings, float ColumnDepth,
                                TArray<FDecoSpawn>& OutSpawns);

    void LaunchDecoTasks(const FIntPoint& PlayerCell);
    void ProcessDecoResults(const FIntPoint& PlayerCell, int32 FarR);
    void MergeCellResult(const FDecoCellResult& Result);   // fold one cell's spawns into its region build
    void MarkCellDone(const FIntPoint& Region, uint32 BuildId);  // decrement region's remaining-cell count
    void ApplyRegion(const FIntPoint& Region, FDecoRegionBuild& Build);
    void RebuildDesiredCells(const FIntPoint& PlayerCell);
    void ClearDecorationRegion(const FIntPoint& Region);
    void ClearAllDecorations();
    // Region size in cells, clamped (>=1). Cell↔region math lives in file-static helpers in the .cpp.
    int32 RegionSize() const;


    TWeakObjectPtr<AActor> Owner;

    UPROPERTY()
    UVoxelStrateManager* StrateManager = nullptr;

    UPROPERTY()
    UVoxelGenerator* Generator = nullptr;

    UPROPERTY()
    UVoxelSettings* Settings = nullptr;

    int32 Seed = 0;

    // Engine unit plane (/Engine/BasicShapes/Plane) reused for every water surface.
    UPROPERTY()
    UStaticMesh* PlaneMesh = nullptr;

    // Loaded decoration regions (FIntPoint = region XY). One HISM per mesh per region. Not a UPROPERTY
    // (weak ptrs inside; the owner actor keeps the components alive).
    TMap<FIntPoint, FDecoRegionContent> DecoRegions;

    // Regions currently being marched cell-by-cell; merged here until every cell reports, then applied.
    TMap<FIntPoint, FDecoRegionBuild> RegionBuilds;

    // Cells that are desired but need a march task launched (nearest-first).
    TArray<FIntPoint> PendingLaunch;
    // Cells with a march task in flight (awaiting a result).
    TSet<FIntPoint> InFlightCells;

    // Worker tasks enqueue here (Mpsc: many workers, one game-thread consumer).
    TQueue<FDecoCellResult, EQueueMode::Mpsc> DecoResults;
    // Regions whose last cell just landed, awaiting budgeted game-thread apply (HISM build + actor spawn).
    TArray<FIntPoint> CompletedRegions;

    // Monotonic id stamped on each region build + the cell tasks it launches. A cell result merges only
    // if its BuildId still matches the live build for that region → a region that was cleared and later
    // re-marched (same coords, new BuildId) never absorbs a stale in-flight cell from its prior life.
    uint32 NextBuildId = 1;

    // Set in BeginDestroy; worker tasks check it before touching us.
    std::atomic<bool> bShuttingDown{false};

    // Streaming state. INT_MIN sentinels force a full rebuild on the first update / after ClearAll.
    FIntPoint LastDecoCell    = FIntPoint(INT32_MIN, INT32_MIN);
    int32     LastStrateIndex = INT32_MIN;

    // Shared strate context for the current update (recomputed each UpdateDecorations; the launch step
    // copies the PODs into each task).
    FDecoContext CurrentCtx;

    // Single strate-global ocean plane, repositioned to follow the player (see UpdateWater).
    UPROPERTY()
    UStaticMeshComponent* WaterPlane = nullptr;

    // Cached state so UpdateWater is a cheap no-op when neither the water level nor the snapped
    // player cell changed since last frame.
    float     LastWaterZ    = -FLT_MAX;                          // strate water level (voxel Z)
    FIntPoint LastWaterCell = FIntPoint(INT32_MIN, INT32_MIN);   // snapped XY grid cell
};
