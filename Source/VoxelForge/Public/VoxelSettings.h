// VoxelSettings.h
// Settings du plugin VoxelForge.
//
// Data asset unique (assigné sur AVoxelWorld) qui regroupe tous les tuning
// du monde: seed, streaming, LOD, pool de strates, budgets de carving.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "VoxelStrateDefinition.h"
#include "VoxelSettings.generated.h"

// IWYU : pointeur seulement (VoxelMaterial). / Pointer-only use.
class UMaterialInterface;
class UVoxelSeasonAsset;

UCLASS(BlueprintType)
class UVoxelSettings : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:

    /** Season seed/radius replace the mutable authored values only when Season is assigned. */
    int32 GetEffectiveWorldSeed() const;
    float GetEffectiveOriginSpineRadius() const;
    float GetEffectiveWorldRadiusVoxels() const;
    int32 GetEffectiveSeasonNumber() const;

	//=========================================================================
	// STREAMING (distance de vue)
	//=========================================================================

	// En CHUNKS (CHUNK_SIZE=32). Couverture linéaire = ViewDistanceXY × 32 × 25 cm.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel|Streaming")
	int32 ViewDistanceXY = 16;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel|Streaming")
	int32 ViewDistanceUp = 5;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel|Streaming")
	int32 ViewDistanceDown = 5;

	// Nombre max de tâches de génération/mesh en parallèle.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel|Streaming")
	int32 MaxConcurrentTasks = 16;

	// Nombre max de meshes appliqués (upload GPU) par frame. Seuls les vrais applies
	// comptent (chunks vides/périmés se vident gratuitement). À 32³ chaque apply est léger
	// (~0.1 ms) — tunable en live sur l'asset : montez (8-16) si le remplissage traîne,
	// baissez si l'apparition des chunks fait des à-coups.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel|Streaming")
	int32 MaxMeshAppliesPerFrame = 4;

	// Max tile teardowns (component destroy + content-actor Destroy) per frame. A fast traversal
	// culls a whole shell of tiles at once; doing every destroy in one frame is a game-thread spike
	// ("stuff torn down behind you"). This spreads it. The drain auto-scales up if the backlog grows
	// (PendingUnload/4) so it never falls far behind — raise the floor if teardown lags at high speed.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel|Streaming")
	int32 MaxUnloadsPerFrame = 6;

	// Strate-aware VERTICAL streaming: don't load chunks into a strate you can't see.
	// Seals + the inter-strate bedrock gap are light-tight (§8.7), so the strate above/below
	// the one you're in is fully occluded — loading it just wastes scene primitives (the
	// game-thread cost). When ON, the vertical view is clamped to the player's strate ±
	// StrateViewMarginChunks. In the bedrock GAP itself the view is NOT clamped (you can see
	// both sides through the descent passage there), so the next strate streams in ahead.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel|Streaming")
	bool bClampViewToStrate = true;

	// Chunks of vertical look-ahead PAST the strate seal (each side) when bClampViewToStrate
	// is on: keeps the descent tunnel / immediate transition loaded and acts as the trailing
	// buffer of the strate you're leaving (it sheds naturally as you descend out of view).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel|Streaming", meta = (ClampMin = "0"))
	int32 StrateViewMarginChunks = 3;

	// STRATE CONTENT CUT (F17 separation) — a level-L tile is 2^L chunks TALL and can straddle a
	// strate boundary: at coarse Steps the thin seal/gap solid between two strates' airs falls
	// between lattice points (⇒ holes into the neighbour strate at far LOD) and one tile mixes
	// both strates' materials. From this clip level UP, the mesher only meshes cells inside the
	// PLAYER's strate Z-band (the other strates are sealed/enclosed ⇒ invisible from here anyway);
	// loaded coarse tiles re-queue automatically when the band changes (strate transition).
	// Default 0 = cut at EVERY level (tested verdict 2026-07-05: level 0/1 straddler tiles were
	// the visible mixers — a higher floor left them mixing and looked like "no improvement").
	// Raise only if the descent/passage transition needs full tiles near the player. 9 = off.
	// At ultra-coarse levels where one CELL is taller than the band itself, the tile is skipped
	// entirely (see LoadTile) — cell-granular cutting there could only render garbage.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel|Streaming", meta = (ClampMin = "0", ClampMax = "9"))
	int32 StrateContentCutMinLevel = 0;

	// Open-world SKY reach: the sky-cap ceiling of an open strate (SurfaceWorld / FloatingIslands)
	// is FAR, so the ceiling BAND is streamed across a wider horizontal radius = ViewDistanceXY ×
	// this, so the sky reaches toward the horizon instead of being a patch over the player's head.
	// Only the ceiling band (top CeilingBandChunks of the strate) gets the wide radius — the empty
	// air below it stays at base XY. Cost grows ~ multiplier² (more far chunks/draws; gen is cheap
	// and batching keeps the game-thread cost low). 1 = off. Live-tunable on the asset — push to 4
	// if the draw budget allows; a proper to-the-horizon view (terrain too) is the chunk-LOD clipmap.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel|Streaming", meta = (ClampMin = "1"))
	int32 CeilingViewMultiplier = 2;

	// How many chunks down from the strate top get the wide CeilingViewMultiplier radius (covers
	// the sky-cap slab). Bigger = no gaps if the ceiling surface dips, but more far chunks.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel|Streaming", meta = (ClampMin = "1"))
	int32 CeilingBandChunks = 4;

	//=========================================================================
	// CLIPMAP (chunked-LOD streaming — supersedes the ViewDistance box above)
	//=========================================================================
	// Streaming loads concentric shells of tiles: level 0 = full-res chunks near the player,
	// each coarser level doubles tile size (and reach). Total tile/draw/gen count stays ~flat
	// regardless of how far you see — so this is the "see to the horizon cheaply" system.
	//   Total reach ≈ ClipRadius × CHUNK_SIZE × 2^MaxClipLevel voxels.
	//   Near full-res reach ≈ ClipRadius × CHUNK_SIZE voxels.

	// Half-extent (in tiles, per axis) of EACH level's shell. Bigger = more tiles per level
	// (more detail / overlap) but more components. 3 → a 7×7×7 box per level (minus the inner
	// hole filled by the finer level).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel|Clipmap", meta = (ClampMin = "1"))
	int32 ClipRadius = 3;

	// Coarsest LOD level. 0 = full-res only (no clipmap). Each level up doubles tile size &
	// reach at ~constant cost — raise this to see much farther for cheap. (Level L step = 2^L.)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel|Clipmap", meta = (ClampMin = "0", ClampMax = "8"))
	int32 MaxClipLevel = 4;

	// How many near levels mesh at FULL resolution (CHUNK_SIZE cells). Levels at or above this
	// use the cheaper CoarseTileCells → MUCH faster gen for the far field (it's far, so the
	// extra blockiness is hidden, and skirts cover the seams). 2 = levels 0,1 crisp; set to 1
	// for the fastest gen (slightly harder LOD0→1 transition).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel|Clipmap", meta = (ClampMin = "1"))
	int32 FullResClipLevels = 2;

	// Cells per axis for COARSE tiles (levels >= FullResClipLevels). Lower = cheaper gen, blockier
	// far terrain. 16 = ~⅛ the gen samples of a full 32-cell tile; 8 = ~1/64. Tile EXTENT is
	// unchanged (the shells still tile) — only the mesh resolution within a far tile drops.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel|Clipmap", meta = (ClampMin = "4", ClampMax = "32"))
	int32 CoarseTileCells = 16;

	// RENDER DISTANCE — custom horizontal reach, in CHUNKS (1 chunk = 8 m; 128 ≈ 1 km, 768 ≈ 6 km).
	// When > 0 and farther than the natural clipmap reach (ClipRadius × 2^MaxClipLevel chunks), the
	// OUTERMOST shell keeps generating level-MaxClipLevel tiles outward until it covers this
	// distance. Pick MaxClipLevel = the coarsest level that still renders strates correctly (one
	// cell must fit inside a strate band — level ≥7 blanks via the too-coarse skip) and buy the
	// remaining horizon here. Cost: the extra ring is all same-level tiles — tile/draw/gen count
	// grows with (distance / 2^MaxClipLevel)², so each MaxClipLevel step down quadruples the ring.
	// 0 = off (natural reach, byte-identical streaming).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel|Clipmap", meta = (ClampMin = "0"))
	int32 RenderDistanceChunks = 0;

	// F18 — the render-distance ring streams per-surface SHEETS instead of MC tiles: in an open
	// strate the far field is exactly two heightfields (TerrainZ + sky-cap CeilSurf, both already
	// computed per column), so each far tile becomes two displaced grids (ground polygroup 0 /
	// cap polygroup 1 — same materials), ~3-6× cheaper to generate and far fewer components (one
	// sheet spans 2^FarSheetSpanLevels MC-tile footprints per axis). Non-open strates produce
	// empty sheets (their far ring was enclosed rock anyway). Carved features (passages, chasms,
	// spine) don't show at sheet distance. Needs the strate band armed (StrateContentCutMinLevel
	// active); in the inter-strate gap the sheet ring blanks until you land. Off = the ring stays
	// MC tiles at level MaxClipLevel (pre-F18 behaviour). Only matters when RenderDistanceChunks > 0.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel|Clipmap")
	bool bFarSheetRing = true;

	// Sheet tile size = MaxClipLevel + this many levels (2 → one sheet covers 4×4 MC-tile
	// footprints → ~16× fewer far components). Sampling density stays that of the MaxClipLevel
	// MC ring (cell count grows instead), capped at 128 cells/axis (beyond, cells coarsen).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel|Clipmap", meta = (ClampMin = "1", ClampMax = "4", EditCondition = "bFarSheetRing"))
	int32 FarSheetSpanLevels = 2;

	// SKIRTS — seal the thin cracks where neighbouring clipmap shells (different resolutions) meet.
	// A short wall is extruded into the solid from each surface edge on the tile's outer faces.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel|Clipmap")
	bool bGenerateSkirts = true;

	// Skirt depth in tile CELLS (× the tile's step). ~2 covers the gap to a one-level-coarser
	// neighbour (2× cell). Raise if cracks still show; lower if skirts peek out on convex edges.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel|Clipmap", meta = (ClampMin = "0.5", ClampMax = "8.0"))
	float SkirtCells = 2.0f;

	// T2.b — LOD-aware octave reduction. Octaves DROPPED from per-voxel volumetric noise
	// (roughness, worms displacement, slab/maze/shaft/island detail) per Step doubling on
	// coarse tiles: a Step=4 tile drops 2×this. Sub-cell octaves can't shape a coarse
	// isosurface — they only cost CPU — so 1 shaves 30-50% off far-tile gen for a sub-cell
	// isosurface shift (skirts already stitch bigger LOD seams). LOD0 is NEVER affected.
	// 0 = off (every LOD samples full octaves — byte-identical to before this setting).
	// Réduction d'octaves sur les tuiles lointaines ; 0 = désactivé, LOD0 jamais touché.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel|Clipmap", meta = (ClampMin = "0", ClampMax = "3"))
	int32 LODOctaveDrop = 0;

	//==========================================================================
	// WORLD BOUNDS (bounded gigantic world in actor-space XY)
	//==========================================================================
	// Radius (voxels) of the bounded world in XY, measured from actor-space (0,0).
	// 0 = unbounded (legacy behaviour, no edge seal).
	//
	// ⚠️ DEFAULTS TO 0 ON PURPOSE, and it is not the value the GDD wants.
	// Existing DA_Settings / BP_VoxelSettings assets predate this property, so they do NOT serialize
	// it — they silently inherit whatever C++ default is written here. A non-zero default would
	// therefore wall in every existing world the next time the editor opens, with nothing in the
	// asset to show why. The GDD calls for "bounded, but gigantic": ~8192 voxels (2.05 km) is the
	// intended production value. SET IT IN THE ASSET, deliberately, once.
	//
	// FR : 0 par défaut EXPRÈS — les assets existants n'ont pas cette propriété et héritent
	// silencieusement de la valeur C++. La valeur de production visée (~8192) se règle dans l'asset.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel|World Bounds", meta = (ClampMin = "0.0"))
	float WorldRadiusVoxels = 0.0f;

	// Solid ramp thickness at the radial world edge, in voxels.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel|World Bounds", meta = (ClampMin = "0.0"))
	float EdgeSealThickness = 64.0f;

	//=========================================================================
	// CONTENT — distance-based decoration grid (no LOD pop)
	//=========================================================================
	// Decorations are placed on a fixed WORLD XY cell grid (1 cell = 1 chunk footprint) and streamed
	// by DISTANCE from the player, independent of which clipmap tile (LOD) currently meshes the ground
	// under them. Each candidate column is ray-marched through the player's strate via the generator's
	// density field and snapped to the real surface — so a given prop keeps the SAME world position at
	// every LOD (no teleport/pop on tile swaps). Decorations exist only in the player's current strate.

	// FAR-tier stream radius in cells (= chunks): how far FStrateDecoration entries set to EDecoStreamTier::Far
	// (the default — trees, landmarks, rare props) stream out. Bigger = props visible farther + more
	// spawn/march cost (but the far grid is COARSE — see DecorationFarSpacingVoxels — so far cost is cheap).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel|Content", meta = (ClampMin = "1"))
	int32 DecorationRadiusChunks = 6;

	// NEAR-tier stream radius in cells (= chunks): how far EDecoStreamTier::Near entries (dense groundcover
	// like grass) stream out. Keep this SHORT — near entries use the FINE grid (DecorationSpacingVoxels), so
	// their cost is the steep one; bounding their radius keeps the far-region HISM build + memory small.
	// (Repurposes the old vestigial DecorationActorRadiusChunks; same default.)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel|Content", meta = (ClampMin = "1"))
	int32 DecorationNearRadiusChunks = 3;

	// Decoration cells are grouped into REGIONS of RxR cells, and ALL placements in a region share ONE
	// HISM per mesh (instead of one HISM per cell per mesh). Regions load/unload as a unit, so clearing
	// stays a plain DestroyComponent — no per-instance index remapping. This is the render-thread lever:
	// component count (which InitViews walks every frame) drops by ~R^2. R=4 → ~16 regions in a radius-6
	// disk vs 169 cells (~10x fewer components). Bigger R = fewer components but coarser pop-in + heavier
	// one-shot cluster-tree build per region (apply is budgeted, so the build is amortised). MUST be >= 1.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel|Content", meta = (ClampMin = "1", ClampMax = "16"))
	int32 DecorationRegionSizeCells = 4;

	// NEAR-tier column spacing (in voxels) within a cell — the FINE grid. MUST divide CHUNK_SIZE (32):
	// 4 → 8×8=64 columns/cell. Smaller = denser placement potential + more march cost. SpawnDensity rolls
	// per column-crossing. Used by EDecoStreamTier::Near entries (and is the legacy single-grid spacing).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel|Content", meta = (ClampMin = "1", ClampMax = "32"))
	int32 DecorationSpacingVoxels = 4;

	// FAR-tier column spacing (in voxels) within a cell — the COARSE grid. MUST divide CHUNK_SIZE (32):
	// 16 → 2×2=4 columns/cell (16× fewer worker ray-marches than a spacing-4 grid). This is the lever that
	// makes a RARE prop visible at every distance cheap: the far grid samples sparsely, so supporting a
	// low-SpawnDensity landmark across the full radius costs a fraction of the fine grid. DEFAULTS to the
	// fine value (4) so existing worlds are byte-identical until you raise it; bump to 8–16 for cheap far props.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel|Content", meta = (ClampMin = "1", ClampMax = "32"))
	int32 DecorationFarSpacingVoxels = 4;

	// COARSE vertical march step (in voxels) when searching a column for surface crossings. The crossing
	// Z is then bisection-refined, so accuracy is independent of this — raise it (4-8) to cut the scan
	// cost (the column is ray-marched on a WORKER thread, but a smaller step still means more samples).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel|Content", meta = (ClampMin = "1", ClampMax = "8"))
	int32 DecorationMarchStepVoxels = 2;

	// Max surface crossings placed per column (caps cave columns that pierce many floors/ceilings;
	// surface worlds have 1). Bounds worst-case march/spawn cost.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel|Content", meta = (ClampMin = "1", ClampMax = "16"))
	int32 DecorationMaxCrossingsPerColumn = 4;

	// Stop marching a column after this many voxels of CONTIGUOUS SOLID once it has already entered the
	// open space — i.e. cap how far into bedrock BELOW the floor (or below a cavern) we keep scanning.
	// The top cap/seal and the air above the ground are always marched first (this only trims the dead
	// rock underneath), so surface worlds still get their ground and caves still find layered floors.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel|Content", meta = (ClampMin = "8"))
	int32 DecorationColumnDepthVoxels = 160;

	// Budget: how many completed decoration REGIONS to APPLY (game-thread HISM build + SpawnActor) per
	// frame. A region is applied once ALL its cells have finished marching, in one batched AddInstances
	// per mesh; this throttles that burst. The expensive ray-march runs async on workers. (Named "Cells"
	// for asset back-compat — the apply unit is now a region of DecorationRegionSizeCells^2 cells.)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel|Content", meta = (ClampMin = "1"))
	int32 MaxDecorationCellsPerFrame = 2;

	// Max decoration ray-march tasks in flight at once. Keeps decoration marching from crowding the
	// mesh-gen workers (which are the streaming bottleneck). 0 disables decorations entirely.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel|Content", meta = (ClampMin = "0"))
	int32 MaxConcurrentDecorationTasks = 4;

	//=========================================================================
	// LIGHTING — DENSITY VOLUME (mini-sun raymarched shadows)
	//=========================================================================
	// A player-centred CLIPMAP of the density field, uploaded to the GPU so the terrain
	// material can RAYMARCH it toward the "mini-sun" orbs → from-the-orb, crisp, dynamic
	// shadows under FORWARD rendering (Lumen/DF off the table). The volume is the load-bearing
	// prerequisite: density is CPU-only (GetDensityAt), so we stream it onto the GPU here.
	// Concentric levels: level 0 = full-res near the player (step 1), each level up doubles the
	// sampling step & reach (fine near / coarse far — exactly what shadow rays want). Filled on
	// WORKER threads (re-evaluating GetDensityAt → deterministic, carves auto-picked-up), with
	// toroidal incremental refill on movement and localized refill on carve. See VoxelDensityVolume.

	// Master switch. OFF = no volume built, no fill tasks, no GPU cost (terrain unlit by orbs).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel|Lighting")
	bool bEnableDensityVolume = true;

	// Per-axis resolution of EACH clip level (cells). Memory per level ≈ Res³ bytes (R8). 128 →
	// ~2 MB/level; 192 → ~7 MB; 256 → ~16 MB. Higher = crisper near shadows + bigger startup fill.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel|Lighting", meta = (ClampMin = "32", ClampMax = "256"))
	int32 DensityVolumeResolution = 128;

	// Number of concentric clip levels. Level L samples every (1<<L) voxels and covers
	// Res·(1<<L) voxels. 3 levels at Res=128 → near 32 m (full-res) out to ~128 m (coarse).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel|Lighting", meta = (ClampMin = "1", ClampMax = "5"))
	int32 DensityVolumeLevels = 3;

	// DEPRECATED / unused: the volume fill no longer runs on the shared UE::Tasks pool (where it
	// starved behind mesh-gen). It now runs on ONE dedicated thread off the pool, so there's no task
	// budget to cap. Kept only so existing saved assets don't error; safe to ignore.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel|Lighting", meta = (ClampMin = "1", ClampMax = "16"))
	int32 DensityVolumeMaxTasks = 4;

	// A fill box is split into Z-slabs of at most this many cells per task, so no single task is
	// huge (a full level refill on startup/teleport fans out across workers). Lower = more, smaller
	// tasks (better parallelism / latency); higher = fewer, fatter tasks.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel|Lighting", meta = (ClampMin = "1", ClampMax = "64"))
	int32 DensityVolumeFillSlabCells = 8;

	// Per-pixel shadow-march step count toward the orb (the terrain material reads this). More = crisper
	// occlusion at grazing angles but higher GPU cost. 64 is a sane start; tune against the look/cost.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel|Lighting", meta = (ClampMin = "4", ClampMax = "256"))
	int32 DensityVolumeMarchSteps = 64;

	// Upload the clipmap to GPU R8 volume textures (so the terrain material can march it). OFF = the
	// CPU volume still streams (debug-draw works) but nothing reaches the GPU — the safe fallback if
	// the runtime Texture3D RHI path misbehaves on a given engine build.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel|Lighting")
	bool bDensityVolumeGPUUpload = true;

	// DEBUG (step 1a verification, no GPU): draw small boxes for SOLID cells of level 0 within
	// DensityVolumeDebugRadiusCells of the player, so you can confirm the volume holds terrain-shaped
	// solidity, follows you, and updates on carve — BEFORE the GPU upload + material march land.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel|Lighting|Debug")
	bool bDebugDrawDensityVolume = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel|Lighting|Debug", meta = (ClampMin = "1", ClampMax = "32"))
	int32 DensityVolumeDebugRadiusCells = 6;

	//=========================================================================
	// RENDERING
	//=========================================================================

	// Matériau global par défaut (peut être override par strate).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel|Rendering")
	UMaterialInterface* VoxelMaterial;

	//=========================================================================
	// STRATES
	//=========================================================================

	// Seed du monde. Pilote toute la génération procédurale.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel|Strates")
	int32 Seed = 0;

	// Optional reviewed/cooked season. When set, its manifest owns layout, seed, bounds and
	// density recipes; the authored StratePool/FixedStrates path remains untouched when unset.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Voxel|Strates")
	TSoftObjectPtr<UVoxelSeasonAsset> Season;

	// Numéro de saison (incrementé par ChangeSeed). Purement informatif côté gameplay.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel|Strates")
	int32 CurrentSeason = 1;

	// Pool de définitions de strates disponibles pour l'assignation aléatoire.
	// Chaque définition a sa propre hauteur — les strates ne sont PAS uniformes.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel|Strates")
	TArray<TSoftObjectPtr<UVoxelStrateDefinition>> StratePool;

	// Strates fixées: {index → définition}. Ex: {5, CrystalCaverns} =
	// la strate 5 est toujours CrystalCaverns. Les autres sont tirées du pool.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel|Strates")
	TMap<int32, TSoftObjectPtr<UVoxelStrateDefinition>> FixedStrates;

	// Nombre total de strates à générer vers le bas.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel|Strates", meta = (ClampMin = "1"))
	int32 TotalStrates = 10;

	// Vertical SEPARATION between consecutive strates, in chunks of solid bedrock.
	// 0 = strates touch (just the seals between them). Higher = a thick rock layer
	// between each biome that the player must dig through at (0,0), and that the
	// auto-carved passages tunnel across.
	//   0 → adjacent layers (default) · 1-2 → a real bedrock band · 4+ → deep separation
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel|Strates", meta = (ClampMin = "0", ClampMax = "16"))
	int32 InterStrateGapChunks = 0;

	//=========================================================================
	// SPINE & INTER-STRATE CONNECTIONS  (the (0,0) descent axis)
	//=========================================================================

    // Authored radius (voxels) of the guaranteed (0,0) landing room in EVERY strate, regardless
    // of archetype. The §6.5 body-sized formulas turn it into a flat-floored room; this setting
    // no longer bores a vertical column. 0 = disabled. The room is top-anchored inside each
    // strate, and the seals remain solid until a progression passage is opened.
    //   10-14 → compact landing room (default) · 20+ → broad landing room
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel|Spine", meta = (ClampMin = "0.0"))
	float OriginSpineRadius = 14.0f;

	// Auto-open the very top seal at (0,0) so the world starts with a hole to the
	// surface (the entry shaft). Lower boundaries are NOT auto-opened — the player
	// digs those. Disable for a fully sealed world the player must breach from above.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel|Spine")
	bool bOpenSurfaceEntry = true;

	// NOTE: inter-strate tunnel count + shape (style, width, length, placement, worming,
	// spiral, cascade) are now PER-STRATE — see UVoxelStrateDefinition::PassageConfig.
	// Each strate controls its own descent tunnels to the layer below it.

	//=========================================================================
	// CARVING (modifications du joueur)
	//=========================================================================

	// Nombre max d'opérations de modification par saison. 0 = illimité.
	//   500  = édition légère, 2000 = mining modéré, 0 = créatif.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel|Carving",
		meta = (ClampMin = "0"))
	int32 MaxModifications = 0;

	// Rayon max d'un brush (en voxels). Empêche les édits géants.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel|Carving",
		meta = (ClampMin = "1.0"))
	float MaxBrushRadius = 15.0f;

	// Volume total maximum (somme des volumes de brush). 0 = illimité.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel|Carving",
		meta = (ClampMin = "0.0"))
	float MaxTotalVolume = 0.0f;
};
