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

UCLASS(BlueprintType)
class UVoxelSettings : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:

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
	// LOD
	//=========================================================================

	// Distance en chunks pour LOD0 (pleine résolution, step=1).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel|LOD")
	int32 LOD0Distance = 4;

	// Distance en chunks pour LOD1 (demi-résolution, step=2). Au-delà → LOD2 (quart-rés,
	// step=4). LOD2 = le plus lointain ; ces chunks ne projettent PLUS d'ombre (cf.
	// ApplyMeshToChunk) → rapprocher LOD0/LOD1 pousse plus de chunks dans la bande
	// LOD2 sans-ombre = moins de draws (levier fps gratuit, à doser visuellement).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel|LOD")
	int32 LOD1Distance = 8;

	//=========================================================================
	// CLIPMAP (chunked-LOD streaming — supersedes the ViewDistance/LOD box above)
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

	// SKIRTS — seal the thin cracks where neighbouring clipmap shells (different resolutions) meet.
	// A short wall is extruded into the solid from each surface edge on the tile's outer faces.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel|Clipmap")
	bool bGenerateSkirts = true;

	// Skirt depth in tile CELLS (× the tile's step). ~2 covers the gap to a one-level-coarser
	// neighbour (2× cell). Raise if cracks still show; lower if skirts peek out on convex edges.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel|Clipmap", meta = (ClampMin = "0.5", ClampMax = "8.0"))
	float SkirtCells = 2.0f;

	// LEGACY / WATER ONLY. Decorations no longer ride clipmap tiles (see Voxel|Content below —
	// they stream on a fixed world grid by distance, so they don't pop on LOD swaps). This now only
	// bounds the tile level at which the level-0 WATER plane is considered (water is level-0 anyway,
	// so its practical effect is nil). Left in place; safe to ignore.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel|Clipmap", meta = (ClampMin = "0", ClampMax = "8"))
	int32 ContentMaxLevel = 2;

	//=========================================================================
	// CONTENT — distance-based decoration grid (no LOD pop)
	//=========================================================================
	// Decorations are placed on a fixed WORLD XY cell grid (1 cell = 1 chunk footprint) and streamed
	// by DISTANCE from the player, independent of which clipmap tile (LOD) currently meshes the ground
	// under them. Each candidate column is ray-marched through the player's strate via the generator's
	// density field and snapped to the real surface — so a given prop keeps the SAME world position at
	// every LOD (no teleport/pop on tile swaps). Decorations exist only in the player's current strate.

	// Far stream radius in cells (= chunks) for "any-distance" entries (instanced/HISM visual props,
	// and actor entries with MaxLODLevel >= 1). Bigger = props visible farther + more spawn/march cost.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel|Content", meta = (ClampMin = "1"))
	int32 DecorationRadiusChunks = 6;

	// Decoration cells are grouped into REGIONS of RxR cells, and ALL placements in a region share ONE
	// HISM per mesh (instead of one HISM per cell per mesh). Regions load/unload as a unit, so clearing
	// stays a plain DestroyComponent — no per-instance index remapping. This is the render-thread lever:
	// component count (which InitViews walks every frame) drops by ~R^2. R=4 → ~16 regions in a radius-6
	// disk vs 169 cells (~10x fewer components). Bigger R = fewer components but coarser pop-in + heavier
	// one-shot cluster-tree build per region (apply is budgeted, so the build is amortised). MUST be >= 1.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel|Content", meta = (ClampMin = "1", ClampMax = "16"))
	int32 DecorationRegionSizeCells = 4;

	// LEGACY / UNUSED. The near/far tier system was removed (it re-streamed cells at the tier boundary
	// as the player moved → decoration flicker). All entries now stream within DecorationRadiusChunks and
	// a loaded cell is never re-streamed in place. Kept only to avoid breaking the asset; safe to ignore.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel|Content", meta = (ClampMin = "1"))
	int32 DecorationActorRadiusChunks = 3;

	// Spacing (in voxels) of candidate columns within a cell. MUST divide CHUNK_SIZE (32): 4 → 8×8=64
	// columns/cell. Smaller = denser placement potential + more march cost. SpawnDensity then rolls per
	// column-crossing (NOTE: this changes the meaning of SpawnDensity vs the old per-vertex scatter —
	// expect to re-tune decoration densities once).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel|Content", meta = (ClampMin = "1", ClampMax = "32"))
	int32 DecorationSpacingVoxels = 4;

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

	// Radius (voxels) of the guaranteed open vertical "landing" column carved at
	// world XY (0,0) in EVERY strate, regardless of archetype. This is the prepared
	// space the player digs THROUGH the seal into when descending. 0 = disabled.
	//   10-14 → cozy shaft (default) · 20+ → wide landing chamber
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
