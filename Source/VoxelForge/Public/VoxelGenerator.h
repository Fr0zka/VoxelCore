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

private:
    /** Pick the biome (index into Ctx.Biomes) for a Voronoi site, by its climate. */
    int32 ClassifyBiomeAtSite(float SiteX, float SiteY, const FBiomeContext& Ctx, uint32 SiteHash) const;

    /** The SurfaceWorld heightfield: world XY → terrain surface Z (voxel coords). Pure
     *  per-XY; the part that's evaluated per biome and blended in GetSurfaceDensity. */
    float ComputeSurfaceTerrainZ(float WorldX, float WorldY, const FSurfaceGenerationParams& Params) const;

    /** The SurfaceWorld sky-cap ceiling surface Z at a world XY (also pure per-XY). */
    float ComputeSurfaceCeiling(float WorldX, float WorldY, const FSurfaceGenerationParams& Params) const;

    /** Final SurfaceWorld density from a column's precomputed terrain Z + ceiling: the
     *  cheap per-voxel Z-combine + origin spine + boundary seal + passage carving. The
     *  XY-only work (terrain/ceiling) is done once per column and cached (T1.a). */
    float SurfaceDensityFromColumn(float WorldX, float WorldY, float WorldZ,
                                   float TerrainZ, float CeilSurf,
                                   const FSurfaceGenerationParams& Structural) const;

    /** (Re)build the per-chunk biome cell grid covering chunk (X,Y) footprint + margin. */
    void RebuildBiomeGrid(int32 ChunkX, int32 ChunkY, int32 ChunkZ,
                          const FBiomeContext& Ctx, FChunkBiomeCache& Cache) const;
};
