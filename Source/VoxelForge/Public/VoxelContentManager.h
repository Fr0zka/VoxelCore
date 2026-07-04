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
// TWO STREAMING GRIDS (FStrateDecoration::StreamTier, §8.5). To stay flicker-free the stream RADIUS must
// be a property of the grid, not the entry (mixing radii in one grid would re-stream a region in place as
// the player crosses an entry's radius — the old tier system's flicker bug). So there are exactly two
// self-contained region streams, and an entry picks one:
//   • FarGrid  — DecorationRadiusChunks radius + DecorationFarSpacingVoxels (COARSE) grid. Default. Cheap
//                for rare/large props visible everywhere (sparse marching across the full radius).
//   • NearGrid — DecorationNearRadiusChunks radius + DecorationSpacingVoxels (FINE) grid. Dense groundcover
//                near the player only; bounding its radius keeps far-region HISM build + memory small.
// Each grid owns its own region/build/queue-routing state (FDecoGrid) and its own subset of the palette;
// the two never share a HISM, so crossing the near boundary loads/unloads a near region without touching
// the far one (no flicker). A given world XY is covered by a far region always, plus a near region when close.
//
// DETERMINISM: same seed + world ⇒ identical placement. Spawning runs on the game thread.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Queue.h"
#include "VoxelTypes.h"
#include "VoxelStrateTypes.h"   // FStrateDecoration (resolved per dominant biome)
#include "VoxelBiomeTypes.h"    // FBiomeContext (per-column biome resolve on the worker)
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

// An active mini-sun light orb (a placed FStrateLandmark with bIsLightOrb). The terrain material marches
// the density volume toward the nearest of these for raymarched shadows. Plain struct (not reflected);
// distances are in WORLD cm (already converted from the landmark's voxel units). See FStrateLandmark.
struct FVoxelActiveOrb
{
    FVector      WorldPos = FVector::ZeroVector;
    FLinearColor Color = FLinearColor::White;
    float        Intensity = 1.0f;
    float        RadiusWorld = 400.0f;        // cm
    float        FalloffWorld = 50000.0f;     // cm
    float        MaxShadowDistWorld = 25000.0f; // cm
};

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

    //--- LANDMARKS (rare large objects on a coarse hash lattice — the "mini-suns") -----------
    /** Stream rare landmark objects around the player. Unlike decorations, these sit on a COARSE hash
     *  lattice (cell = `FStrateLandmark::SpacingChunks` chunks), so cost scales with the number of
     *  landmarks in range, not the area — a huge StreamRadiusChunks stays cheap (no per-chunk enumeration,
     *  no freeze). Synchronous game-thread placement (a surface-find runs only when a NEW lattice cell
     *  enters range; there are very few). Deterministic (hash of cell+entry+seed) → pop-free. Call every
     *  Tick. Strate-bounded like decorations (wiped + rebuilt on strate change). */
    void UpdateLandmarks(const FVector& PlayerWorldPos);

    /** Collect the currently-placed mini-sun light orbs (landmarks with bIsLightOrb). Cheap — iterates
     *  the small LandmarkInstances map. AVoxelWorld picks the nearest to feed the terrain material's
     *  raymarched shadows. */
    void GetActiveOrbs(TArray<FVoxelActiveOrb>& OutOrbs) const;

    /** Destroy all spawned content (decorations + water). Regenerate / season reset. Bumps the deco
     *  epoch so any in-flight march tasks' results are discarded. */
    void ClearAll();

    /** DIAGNOSTIC: report the decoration streaming state of the region covering an actor-LOCAL XY, so a
     *  line-trace probe can tell apart a render drop / an empty march / a stuck build / a never-requested
     *  region for a visibly-bare patch. LocalPos is in actor-local cm (the caller undoes the actor xf).
     *  Game-thread only (reads DecoRegions / RegionBuilds). */
    void QueryDecoDebugAt(const FVector& LocalPos, bool& bApplied, int32& InstanceCount,
                          bool& bBuilding, int32& CellsAccounted, int32& CellsTotal,
                          int32& LiveMarchSpawns, int32& InstancesInCell) const;

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
        EDecoStreamTier Grid = EDecoStreamTier::Far;  // which grid (Near/Far) this result routes back to
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
        // Cells already counted toward completion. A cell can spawn TWO worker tasks (a stale cell from a
        // discarded build re-enters range, gets re-enqueued, and launches again once its first task frees
        // the InFlightCells slot). Accounting per-cell here (not a blind --CellsRemaining) makes completion
        // IDEMPOTENT so the second task can't double-decrement and apply the region before every cell has
        // actually reported — which left a permanent empty chunk until a regen re-marched it.
        TSet<FIntPoint> AccountedCells;
    };

    // All per-grid streaming state, instantiated once per tier (NearGrid / FarGrid). Each grid is a fully
    // self-contained region stream: its own loaded regions, in-progress builds, launch/in-flight queues,
    // completed list, build-id counter, palette subset, and (radius, spacing) config. The two grids never
    // share a HISM, so they load/unload independently with no cross-tier flicker (see the file header).
    struct FDecoGrid
    {
        EDecoStreamTier Tier = EDecoStreamTier::Far;   // identity (stamped on results so they route back here)
        int32 Radius  = 6;   // stream radius in cells (= chunks)
        int32 Spacing = 4;   // march column spacing in voxels (fine for Near, coarse for Far)

        TMap<FIntPoint, FDecoRegionContent> Regions;     // loaded regions
        TMap<FIntPoint, FDecoRegionBuild>   Builds;      // regions being marched
        TArray<FIntPoint> PendingLaunch;                 // cells awaiting a march task (nearest-first)
        TSet<FIntPoint>   InFlightCells;                 // cells with a task in flight
        TArray<FIntPoint> Completed;                     // regions whose last cell landed, awaiting apply
        uint32 NextBuildId = 1;                          // monotonic build id (per grid)

        // Palette subset for THIS tier, rebuilt each update. Entries[i] is owned by context-biome
        // EntryBiome[i] (-1 = strate fallback, always matches). EntryIdx in a result indexes this snapshot.
        TArray<FStrateDecoration> Entries;
        TArray<int32>             EntryBiome;
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

        // Strate biome field (PODs only → worker-safe). Empty ⇒ biomes disabled for this strate. The
        // worker resolves the dominant biome PER COLUMN (ResolveBiomeSampleAt) so decoration borders
        // follow the warped-Voronoi field instead of snapping to the chunk-footprint cell grid (§8.5).
        FBiomeContext BiomeCtx;
    };

    // One spawned landmark (rare hash-lattice object). Weak — the owner actor keeps it alive. BOTH null
    // means the cell was evaluated but placed nothing (gate failed) — kept so we don't re-evaluate it.
    struct FLandmarkInstance
    {
        TWeakObjectPtr<AActor>               Actor;       // set when the entry uses ActorClass
        TWeakObjectPtr<UStaticMeshComponent> Component;   // set when the entry uses InstancedMesh

        // Mini-sun light orb data (set in SpawnLandmarkInstance when the landmark has bIsLightOrb). The
        // terrain material consumes the nearest active orb for raymarched shadows (see GetActiveOrbs).
        bool         bIsOrb = false;
        FVoxelActiveOrb Orb;
    };

    /** WORKER-THREAD surface find → fills OutSpawns for one cell. SurfaceWorld uses the height oracle
     *  (cheap, O(1)/column); other archetypes ray-march the density column. No UObject access except
     *  Generator (thread-safe). Determinism-critical. Resolves the dominant biome PER COLUMN
     *  (ResolveBiomeSampleAt via Ctx.BiomeCtx) and rolls only the entries that biome owns — EntryBiome[i]
     *  is the context-biome index for Entries[i] (-1 = strate fallback, always matches). */
    static void BuildCellSpawns(const UVoxelGenerator* Gen, const FTransform& OwnerXf,
                                const FIntPoint& Cell, const FDecoContext& Ctx,
                                const TArray<FStrateDecoration>& Entries,
                                const TArray<int32>& EntryBiome, uint32 InSeed,
                                int32 Spacing, float Step, int32 MaxCrossings, float ColumnDepth,
                                TArray<FDecoSpawn>& OutSpawns);

    // Each step operates on ONE grid (G = NearGrid or FarGrid). LaunchDecoTasks throttles against the
    // COMBINED in-flight count (OtherInFlight = the other grid's in-flight cells) so the two grids share
    // one concurrency budget. ProcessDecoResults drains the shared result queue, routing each result to its
    // grid by FDecoCellResult::Grid, then applies both grids' completed regions under one frame budget.
    void LaunchDecoTasks(FDecoGrid& G, const FIntPoint& PlayerCell, int32 OtherInFlight, int32 MaxConc);
    void ProcessDecoResults(const FIntPoint& PlayerCell);
    void MergeCellResult(FDecoGrid& G, const FDecoCellResult& Result);   // fold one cell's spawns into its region build
    void MarkCellDone(FDecoGrid& G, const FIntPoint& Region, const FIntPoint& Cell, uint32 BuildId);  // idempotent per-cell accounting
    void ApplyRegion(FDecoGrid& G, const FIntPoint& Region, FDecoRegionBuild& Build);
    void RebuildDesiredCells(FDecoGrid& G, const FIntPoint& PlayerCell);
    void ClearDecorationRegion(FDecoGrid& G, const FIntPoint& Region);
    void ClearAllDecorations();
    void DrainDecoResults();                          // discard every queued march result
    static void ResetGridBuildState(FDecoGrid& G);    // drop builds/queues (loaded regions untouched)
    // Region size in cells, clamped (>=1). Cell↔region math lives in file-static helpers in the .cpp.
    int32 RegionSize() const;

    //--- LANDMARKS (hash-lattice rare objects) -------------------------------------------------
    // Evaluate ONE lattice cell's landmark: biome/surface/slope/water gates, then spawn the actor/mesh.
    // Leaves Out empty (null) when the cell is "evaluated but nothing placed" so it is never re-evaluated
    // while it stays in range. H = the cell's existence hash (drives jitter/rotation/scale determinism).
    void SpawnLandmarkInstance(const FStrateLandmark& L, uint32 H, const FDecoContext& Ctx,
                               const FTransform& OwnerXf, AActor* OwnerActor,
                               float LocalX, float LocalY, float Step, float ColDepth, FLandmarkInstance& Out);
    void DestroyLandmarkInstance(FLandmarkInstance& Inst);
    void ClearAllLandmarks();
    // Single-column surface find for a landmark (voxel XY): SurfaceWorld → height oracle, else ray-march the
    // strate band for the first crossing whose orientation matches Surf. Fills Z (voxel) + outward world normal.
    static bool FindLandmarkColumn(const UVoxelGenerator* Gen, const FTransform& OwnerXf,
                                   const FDecoContext& Ctx, float VX, float VY, ESurfaceType Surf,
                                   float Step, float ColDepth, float& OutZ, FVector& OutNormal);


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

    // The two streaming grids. Each owns its loaded regions, in-progress builds, launch/in-flight queues,
    // completed list, build-id counter, palette subset, and (radius, spacing) config — see FDecoGrid. The
    // (radius, spacing) are refreshed from VoxelSettings each update; the regions are NOT UPROPERTYs (weak
    // ptrs inside; the owner actor keeps the spawned components alive).
    FDecoGrid NearGrid;
    FDecoGrid FarGrid;

    // Worker tasks enqueue here (Mpsc: many workers, one game-thread consumer). SHARED across both grids;
    // each result carries its FDecoCellResult::Grid so ProcessDecoResults routes it to the right grid.
    TQueue<FDecoCellResult, EQueueMode::Mpsc> DecoResults;

    // Spawned landmarks, keyed by FIntVector(latticeCellX, latticeCellY, entryIndex) — FIntVector already
    // hashes, so no custom key type is needed. An entry with both ptrs null = "evaluated, nothing placed"
    // (kept until the cell leaves range so the surface-find isn't repeated). Strate-bounded.
    TMap<FIntVector, FLandmarkInstance> LandmarkInstances;
    int32 LastLandmarkStrate = INT32_MIN;   // strate change → wipe + rebuild landmarks

    // Set in BeginDestroy; worker tasks check it before touching us.
    std::atomic<bool> bShuttingDown{false};

    // Streaming state. INT_MIN sentinels force a full rebuild on the first update / after ClearAll.
    FIntPoint LastDecoCell    = FIntPoint(INT32_MIN, INT32_MIN);
    int32     LastStrateIndex = INT32_MIN;

    // Shared strate context for the current update (recomputed each UpdateDecorations; the launch step
    // copies the PODs into each task). Same for both grids — a strate is a horizontal slab.
    FDecoContext CurrentCtx;

    // The decoration palette is built ONCE per update (a strate's biome field is XY-global, so the flat
    // list is the same for every cell — only the per-COLUMN biome pick varies) and PARTITIONED by tier into
    // NearGrid.Entries / FarGrid.Entries (with parallel EntryBiome). Each list is the concatenation of every
    // biome's decoration entries of that tier (or the strate's when a biome has none / biomes are disabled).

    // Single strate-global ocean plane, repositioned to follow the player (see UpdateWater).
    UPROPERTY()
    UStaticMeshComponent* WaterPlane = nullptr;

    // Cached state so UpdateWater is a cheap no-op when neither the water level nor the snapped
    // player cell changed since last frame.
    float     LastWaterZ    = -FLT_MAX;                          // strate water level (voxel Z)
    FIntPoint LastWaterCell = FIntPoint(INT32_MIN, INT32_MIN);   // snapped XY grid cell
};
