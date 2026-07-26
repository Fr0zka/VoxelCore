// VoxelGenerator.h
// Fournit le champ de densité du monde (positif = solide, négatif = air).
//
// Pipeline density-only: plus de grille de blocs, plus de surface heightfield.
// Le mesher appelle GetDensityAt() par voxel pour reconstruire la géométrie.
// Toute la logique de caves vit dans GetDensityWithParams / GetSlabDensity,
// pilotée par les params de strate récupérés auprès du StrateManager.

#pragma once

#include "CoreMinimal.h"
#include "VoxelTypes.h"
#include "VoxelStrateTypes.h"
#include "VoxelBiomeTypes.h"
#include "VoxelGenerator.generated.h"

// Forward decls (évite les includes transitifs)
class UVoxelSettings;
class UVoxelStrateManager;
class UVoxelDiffLayer;
class UVoxelBiomeDefinition;

//=============================================================================
// LOD-AWARE OCTAVE REDUCTION (T2.b)
//=============================================================================
// At Step=2/4, noise octaves whose wavelength is smaller than the sampling cell
// are pure aliasing cost — they can't shape the coarse isosurface, only shift it
// by sub-cell noise. Réduction d'octaves sur les tuiles lointaines (LOD).
//
// OctaveBias = octaves dropped from PER-VOXEL volumetric noise for the tile
// currently being meshed on THIS thread. Default 0 = full quality; set (via
// TGuardValue) by UVoxelMarchingCubesMesher::GenerateMesh from its Step and the
// opt-in UVoxelSettings::LODOctaveDrop, restored when the tile finishes — so
// game-thread queries, deco snapping and the density-volume fill thread always
// see 0. fBM/Ridged add octaves low→high frequency, so dropping the TAIL keeps
// the coarse shape identical; only sub-cell detail (already unrepresentable at
// that Step) disappears. LOD0 (Step=1) is byte-identical regardless.
//
// Deliberately NOT applied to XY-field noise (heightfield, ceiling, relief,
// moisture): those feed box-validated caches that can outlive a tile task on
// the same thread, and biome/climate must stay LOD-independent.
namespace VoxelGenLOD
{
    // NOT VOXELFORGE_API: MSVC forbids dll-interface on thread_local (C2492). Both users
    // (generator + mesher) are inside this module, so no export is needed anyway.
    extern thread_local int32 OctaveBias;

    // Effective octave count for a per-voxel noise call site.
    // At least 1 octave always survives (the coarse base shape).
    FORCEINLINE int32 Eff(int32 Octaves) { return FMath::Max(1, Octaves - OctaveBias); }
}

//=============================================================================
// TRIVIAL-TILE CLASSIFICATION (T1.d)
//=============================================================================
// Verdict de ClassifyTile pour une tuile AVANT le pré-échantillonnage 33³+ :
// AllSolid / AllAir garantissent que CHAQUE point du treillis du mesher (marge
// ±1 incluse) est du même côté de l'iso ⇒ maillage vide, GenerateMesh est
// sautée. Mixed = "je ne peux pas le prouver" ⇒ génération normale. Un faux
// Mixed coûte juste du CPU ; un faux AllSolid/AllAir ferait un TROU — les
// verdicts ne sont donc émis que sur des bornes exactes (colonnes surface
// échantillonnées au MÊME treillis que le mesher) + gardes conservatives sur
// tout ce qui peut creuser/remplir (spine, passages, disturbances, diff layer).
enum class EVoxelTileClass : uint8
{
    Mixed,      // peut contenir une surface → mesher normalement
    AllSolid,   // chaque échantillon prouvé solide → maillage vide
    AllAir,     // chaque échantillon prouvé air   → maillage vide
};

/**
 * UVoxelGenerator
 *
 * Objet UObject léger — ne stocke pas de données lourdes.
 * Tient juste un pointeur sur les settings (pour le seed) et les services
 * dont il a besoin (StrateManager pour les params par chunk, DiffLayer pour
 * les carves du joueur).
 */
UCLASS(BlueprintType)
class VOXELFORGE_API UVoxelGenerator : public UObject
{
    GENERATED_BODY()

public:
    //=========================================================================
    // SEED (source unique: Settings->Seed)
    //=========================================================================
    // On copie juste le seed au démarrage pour éviter un déréférencement
    // par voxel. Tout le reste vient des params de strate.
    int32 Seed = 0;

    // Radius (voxels) of the guaranteed open vertical landing column carved at world
    // XY (0,0) in every strate — the (0,0) descent spine. Copied from VoxelSettings.
    // 0 disables the spine carve.
    float OriginSpineRadius = 14.0f;

    //=========================================================================
    // SERVICES (injectés par AVoxelWorld::BeginPlay)
    //=========================================================================

    // Manager des strates — fournit les params de cave par chunk.
    // Peut être nullptr: dans ce cas on utilise des params par défaut.
    UPROPERTY()
    const UVoxelStrateManager* StrateManager = nullptr;

    // Diff layer des modifications joueur (carve/fill).
    // Peut être nullptr: dans ce cas pas de modifs appliquées.
    UPROPERTY()
    const UVoxelDiffLayer* DiffLayer = nullptr;

    void SetStrateManager(const UVoxelStrateManager* InManager) { StrateManager = InManager; }
    void SetDiffLayer(const UVoxelDiffLayer* InDiffLayer)       { DiffLayer = InDiffLayer; }

    // Copie le seed depuis les settings. Appelé au démarrage et lors d'un ChangeSeed.
    void InitializeSettings(const UVoxelSettings* Settings);

    //=========================================================================
    // API DE DENSITÉ
    //=========================================================================

    /**
     * Densité combinée (strates + diff du joueur).
     *
     * Convention de sortie (MC): négatif = solide, positif = air.
     * C'est ce que le marching cubes attend comme champ scalaire.
     *
     * @param WorldX, WorldY, WorldZ - coordonnées monde en voxels (pas cm)
     */
    float GetDensityAt(float WorldX, float WorldY, float WorldZ) const;

    /**
     * Densité pour une strate TunnelNetwork (rooms + tunnels + worm noise).
     * Utilisée en interne par GetDensityAt quand la strate est de ce type.
     * Exposée pour permettre des tests isolés avec des params custom.
     */
    float GetDensityWithParams(float WorldX, float WorldY, float WorldZ,
                               const FStrateGenerationParams& Params) const;

    /**
     * Densité pour une strate Slab (FlatPlain / CrystalChamber).
     * Vide horizontal entre un sol bruité et un plafond bruité.
     */
    float GetSlabDensity(float WorldX, float WorldY, float WorldZ,
                         const FSlabGenerationParams& Params) const;

    /**
     * Densité pour une strate Maze — couloirs étroits sur un treillis 3D déterministe.
     * Pas de cache: évalué par voxel sur les quelques cellules voisines.
     */
    float GetMazeDensity(float WorldX, float WorldY, float WorldZ,
                         const FMazeGenerationParams& Params) const;

    /**
     * Densité pour une strate SurfaceWorld — terrain à ciel ouvert (collines,
     * montagnes, plages) sous un plafond solide, avec nappe d'eau optionnelle.
     *
     * Biome output-blend: the heightfield is evaluated with ParamsD (the voxel's dominant
     * biome) and, when NeighborWeight > 0, also with ParamsN (its nearest neighbour); the
     * two surface HEIGHTS are lerped. This stays seamless across ANY param difference
     * (frequencies included). With no biomes, pass ParamsD == ParamsN and weight 0 →
     * bit-identical to the single-param terrain. Structural fields (Z bounds, seal, base,
     * water level) must be equal in both (forced from the strate).
     */
    float GetSurfaceDensity(float WorldX, float WorldY, float WorldZ,
                            const FSurfaceGenerationParams& ParamsD,
                            const FSurfaceGenerationParams& ParamsN,
                            float NeighborWeight) const;

    /**
     * Densité pour une strate VerticalShafts — puits verticaux pleine hauteur,
     * vires horizontales, et connecteurs horizontaux occasionnels.
     */
    float GetVerticalShaftDensity(float WorldX, float WorldY, float WorldZ,
                                  const FVerticalShaftParams& Params) const;

    /**
     * Densité pour une strate FloatingIslands — masses de terre suspendues dans
     * un grand vide ouvert (îles flottantes), sommets aplatis, dessous rugueux.
     */
    float GetFloatingIslandDensity(float WorldX, float WorldY, float WorldZ,
                                   const FFloatingIslandParams& Params) const;

    //=========================================================================
    // CLIMATE & BIOME FIELDS  (pure XY, deterministic, window-invariant)
    //=========================================================================

    /**
     * Relief ("elevation") field at a world XY → [0,1], contrast-shaped & smoothed.
     * The single source of truth for the relief map M: SurfaceWorld terrain and the
     * biome climate map both call this, so they agree when given the same freq/contrast.
     */
    float SampleRelief(float WorldX, float WorldY, float Frequency, float Contrast) const;

    /**
     * Moisture field at a world XY → [0,1]. The second climate axis for biome placement.
     */
    float SampleMoisture(float WorldX, float WorldY, float Frequency) const;

    /**
     * Resolve the biome at a world XY: dominant cell + nearest neighbour + blend weight.
     * Warped Voronoi over a jittered grid; each cell's biome is chosen by its site's
     * (relief, moisture) against the context's climate boxes. PURE function of (XY, seed,
     * Ctx) — no chunk-window dependence (CODEMAP §8.4). Returns an empty sample when the
     * context has no biomes. Used by the 2D preview bake and (next) the density path.
     */
    FBiomeSample SampleBiomeAt(float WorldX, float WorldY, const FBiomeContext& Ctx) const;

    /**
     * Hot-path biome resolve: the dominant + neighbour biome and blend weight at a world
     * XY for the given strate slice. Uses (and lazily rebuilds) Cache — a box-validated
     * per-chunk grid — so the noise-heavy cell classification happens once per chunk, not
     * per voxel. Bit-identical to SampleBiomeAt, so the baked preview matches the terrain.
     */
    FBiomeSample ResolveBiomeSampleAt(float WorldX, float WorldY, int32 ChunkZ,
                                      const FBiomeContext& Ctx, FChunkBiomeCache& Cache) const;

    /**
     * Dominant biome ASSET at a world XY for the given strate slice (chunk Z), or nullptr
     * when the strate has no biomes / the point is outside the strate range. Game-thread
     * helper for content + atmosphere selection — uses the same field as the density path.
     */
    const UVoxelBiomeDefinition* GetDominantBiomeAt(float WorldX, float WorldY, int32 ChunkZ) const;

    /**
     * Rich biome probe at a world XY for a strate slice (ChunkZ): dominant + neighbour biome assets,
     * climate fields, border blend weight, and the dominant biome's EFFECTIVE decoration count. Mirrors
     * exactly what the decoration scatter resolves per column, so it is a faithful "what's under here?"
     * diagnostic (DominantDecorationCount == 0 explains an empty biome region). Game-thread, uncached.
     */
    void QueryBiomeAt(float WorldX, float WorldY, int32 ChunkZ, FVoxelBiomeQuery& Out) const;

    /**
     * Per-vertex material data for the master triplanar palette material (F6). Resolves the
     * biome field at a world XY/Z and returns the dominant + neighbour MaterialPaletteIndex
     * and the border blend weight (0 deep in a cell → 0.5 at the border). The mesher packs
     * these into vertex colour so one material re-skins terrain per biome and cross-fades
     * across biome borders. No biomes ⇒ Dominant=Neighbour=0, Weight=0 (default palette).
     * Thread-safe: own per-chunk thread_local context + box cache (clustered tile queries
     * stay warm). Window-invariant (ResolveBiomeSampleAt is bit-identical to SampleBiomeAt).
     */
    void GetBiomeMaterialAt(float WorldX, float WorldY, float WorldZ,
                            int32& OutDominantPalette, int32& OutNeighborPalette,
                            float& OutBlendWeight) const;

    /**
     * F7 AWARE PLACEMENT: evaluate an entry's relational placement conditions at a candidate voxel XY.
     * True = ALL conditions pass (AND); empty list = true (zero cost). Pure query of the analytic fields
     * (relief/moisture/biome-border) — deterministic + worker-safe. The caller passes the strate's
     * already-resolved biome context (freq/contrast + Voronoi map), so no re-resolve. Also the shared core
     * of the future quest FindFeature locator (same predicate, run as an outward search).
     */
    bool EvaluateTerrainConditions(const TArray<FTerrainCondition>& Conditions,
                                   float WorldX, float WorldY, const FBiomeContext& BiomeCtx) const;

    /**
     * SurfaceWorld HEIGHT ORACLE: the terrain surface Z + sky-cap ceiling Z (voxel coords) at a world
     * XY for the given strate slice (ChunkZ), WITHOUT ray-marching the density column. Returns false
     * (outs untouched) when the chunk is NOT a SurfaceWorld heightfield — callers fall back to marching.
     * Shares the density path's surface helpers, so a decoration snapped to OutTerrainZ sits exactly on
     * the rendered ground. Does NOT include passage/spine/seal carving — verify with one GetDensityAt at
     * the result if a column might be carved. Thread-safe (own per-chunk thread_local cache).
     */
    bool GetSurfaceHeightAt(float WorldX, float WorldY, int32 ChunkZ,
                            float& OutTerrainZ, float& OutCeilSurf) const;

    /**
     * T1.d — classification conservative d'une tuile AVANT le pré-échantillonnage du mesher.
     * (OriginVoxels, Step, CellsPerAxis) = les MÊMES arguments que GenerateMesh ; le verdict
     * porte sur le treillis exact que le mesher échantillonnerait (marge ±1 incluse).
     *
     * v1 : ne prouve que les chunks GAP (bedrock) et les strates SurfaceWorld — colonnes
     * terrain/plafond évaluées par le MÊME ComputeSurfaceColumn que le chemin densité (donc
     * bit-identiques), bandes de seal solides, gardes spine/passages/disturbances/diff.
     * Tout autre archétype (intérieur de caves) ⇒ Mixed. Worker-safe (lecture seule +
     * caches thread_local partagés avec GetDensityAt — un verdict Mixed laisse les colonnes
     * chaudes pour la génération qui suit).
     */
    EVoxelTileClass ClassifyTile(const FIntVector& OriginVoxels, int32 Step, int32 CellsPerAxis) const;

private:
    /** Pick the biome (index into Ctx.Biomes) for a Voronoi site, by its climate. */
    int32 ClassifyBiomeAtSite(float SiteX, float SiteY, const FBiomeContext& Ctx, uint32 SiteHash) const;

    /** The SurfaceWorld heightfield: world XY → terrain surface Z (voxel coords). Pure
     *  per-XY; the part that's evaluated per biome and blended in GetSurfaceDensity. */
    float ComputeSurfaceTerrainZ(float WorldX, float WorldY, const FSurfaceGenerationParams& Params) const;

    /** F20 — the RAW structural heightfield (continents + mountains + detail), BEFORE any
     *  terrain op (cliff/terrace/layer-lines/beach). Ops in ComputeSurfaceTerrainZ build on
     *  this; Cliff re-samples it at an XY offset for a cheap analytic slope. OutM = the relief
     *  "mountainous-ness" [0,1], reused to relief-condition the ops. */
    float SampleSurfaceStructuralZ(float WorldX, float WorldY,
                                   const FSurfaceGenerationParams& Params, float& OutM) const;

    /** The SurfaceWorld sky-cap ceiling surface Z at a world XY (also pure per-XY). */
    float ComputeSurfaceCeiling(float WorldX, float WorldY, const FSurfaceGenerationParams& Params) const;

    /** Resolve a chunk's SurfaceWorld params (strate base + per-biome overrides, structural fields
     *  forced from the strate). Shared by GetDensityAt's per-chunk cache and the GetSurfaceHeightAt
     *  oracle so both produce the SAME surface. */
    void ResolveSurfaceChunkParams(const FIntVector& ChunkCoord,
                                   FSurfaceGenerationParams& OutSurface, FBiomeContext& OutBiomeCtx,
                                   TArray<FSurfaceGenerationParams>& OutBiomeParams) const;

    /** Biome-blended terrain Z + sky-cap ceiling Z for one column (the XY-only surface field). Shared
     *  by the density column cache (T1.a) and the oracle. F20 phase 2 overhang, resolved per column:
     *  `OutOverhangAmp` = strength·slope-gate (0 = off), `(OutDirX,OutDirY)` = unit UPHILL gradient dir. */
    void ComputeSurfaceColumn(float WorldX, float WorldY, int32 ChunkZ,
                              const FSurfaceGenerationParams& BaseSurface, const FBiomeContext& BiomeCtx,
                              const TArray<FSurfaceGenerationParams>& BiomeParams, FChunkBiomeCache& BiomeCache,
                              float& OutTerrainZ, float& OutCeilSurf,
                              float& OutOverhangAmp, float& OutDirX, float& OutDirY) const;

    /** Final SurfaceWorld density from a column's precomputed terrain Z + ceiling: the cheap per-voxel
     *  Z-combine + F20 overhang shelf (warped-terrain union, uphill dir) + origin spine + seal + passages.
     *  The XY-only work (terrain/ceiling/overhang amp+dir) is done once per column and cached (T1.a). */
    float SurfaceDensityFromColumn(float WorldX, float WorldY, float WorldZ,
                                   float TerrainZ, float CeilSurf,
                                   float OverhangAmp, float DirX, float DirY,
                                   const FSurfaceGenerationParams& Structural) const;

    /** (Re)build the per-chunk biome cell grid covering chunk (X,Y) footprint + margin. */
    void RebuildBiomeGrid(int32 ChunkX, int32 ChunkY, int32 ChunkZ,
                          const FBiomeContext& Ctx, FChunkBiomeCache& Cache) const;
};
