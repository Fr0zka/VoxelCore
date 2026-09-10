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
    // The TLS variables themselves stay module-local because MSVC forbids exporting thread_local
    // data (C2492).  These tiny accessors let the editor explorer build a shared LOD grid without
    // linking directly to the storage in the game module.
    extern thread_local int32 OctaveBias;

    // Sampling step of the tile currently being meshed on this worker.  GetDensityAt itself has
    // no Step parameter because callers outside the mesher are point queries; this TLS is only a
    // read-only hint for caches that may safely widen their spatial window for coarse tiles.
    extern thread_local int32 SampleStep;

    VOXELFORGE_API int32 GetThreadOctaveBias();
    VOXELFORGE_API void SetThreadOctaveBias(int32 Value);
    VOXELFORGE_API int32 GetThreadSampleStep();
    VOXELFORGE_API void SetThreadSampleStep(int32 Value);

    // Effective octave count for a per-voxel noise call site.
    // At least 1 octave always survives (the coarse base shape).
    FORCEINLINE int32 Eff(int32 Octaves) { return FMath::Max(1, Octaves - OctaveBias); }
}

// NOTE: EVoxelTileClass (le verdict T1.d) a déménagé dans VoxelTypes.h — inclus ci-dessus —
// pour que VoxelDensityOp.h puisse le partager sans dépendre d'un header UCLASS.
// EVoxelTileClass (the T1.d verdict) moved to VoxelTypes.h, included above.

/**
 * UVoxelGenerator
 *
 * Objet UObject léger — ne stocke pas de données lourdes.
 * Tient les paramètres globaux copiés depuis les settings (seed, spine, bord XY) et les services
 * dont il a besoin (StrateManager pour les params par chunk, DiffLayer pour
 * les carves du joueur).
 */
UCLASS(BlueprintType)
class VOXELFORGE_API UVoxelGenerator : public UObject
{
    GENERATED_BODY()

public:
    UVoxelGenerator();

    //=========================================================================
    // SEED (source unique: Settings->Seed)
    //=========================================================================
    // Les paramètres globaux sont copiés au démarrage pour éviter un déréférencement par voxel.
    // Les formes créatives viennent ensuite des params de strate.
    int32 Seed = 0;

    // Authored radius (voxels) of the guaranteed flat-floored landing room at world XY (0,0) in
    // every strate — the old “spine” setting now sizes a room and never creates a vertical bore.
    // 0 disables the origin landing rooms. Copied from VoxelSettings.
    float OriginSpineRadius = 14.0f;

    // Radius (voxels) of the bounded world in actor-space XY, measured from (0,0).
    // 0 keeps the legacy unbounded behavior and is a true no-op for the edge seal.
    float WorldRadiusVoxels = 8192.0f;

    // Width (voxels) of the smooth solid ramp at the XY world edge.
    float EdgeSealThickness = 64.0f;

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
     *
     * ⚠️ `ParamsFingerprint` ET `LayoutVersion` SONT OBLIGATOIRES, ET C'EST LE CORRECTIF
     * D'`AUDIT §C2` (2026-07-28). Le cache SDF interne est clé sur (boîte XY, strate, seed) et
     * PAS sur les params. Or `GetGenerationParams` BLENDE les params à l'intérieur d'une même
     * strate — `Alpha` dépend du chunk Z en mode `Gradient` (le DÉFAUT, avec
     * `TransitionBlendChunks = 2`) et du chunk XY en plus en mode `Interleaved`. Deux chunks de la
     * même strate, même seed, donc même clé, mais des params DIFFÉRENTS : le worker évalue le
     * deuxième chunk qu'il construit contre les salles du premier. Et comme *quel* chunk vient en
     * premier dépend de l'ordre des workers, **deux pairs divergent depuis la même seed** — ce que
     * `OPSTACK-PLAN §2.6.1` interdit explicitement.
     *
     * Pourquoi une empreinte PASSÉE plutôt qu'un `MemCrc32` calculé ici : ce serait ~300 octets de
     * CRC PAR VOXEL sur le chemin le plus chaud du plugin. L'appelant la calcule UNE fois par
     * chunk, là où le mémo de params vit déjà (`CP_*`), donc le coût par voxel est exactement deux
     * comparaisons d'entiers. Pas de valeur par défaut : un appelant qui oublie doit ne pas
     * compiler, pas hériter silencieusement du trou (la discipline de `FVoxelOpContext`).
     *
     * ⚠️ POUR LES TESTS : passez `FCrc::MemCrc32(&Params, sizeof(Params))`. Un oracle qui partage
     * le défaut qu'il teste ne prouve rien — c'est précisément ce que la note de
     * `VoxelForgeOpStackTunnelTest.cpp` (contrôle 3) décrivait comme le trou de l'original.
     *
     * The SDF cache key had neither the params nor anything that determines them, while the params
     * are blended per chunk INSIDE a strate — so a worker could evaluate one chunk against another
     * chunk's rooms, and which came first depends on worker order. Passing a once-per-chunk
     * fingerprint keeps the fix off the per-voxel path. No default: forgetting it must not compile.
     */
    float GetDensityWithParams(float WorldX, float WorldY, float WorldZ,
                               const FStrateGenerationParams& Params,
                               uint32 ParamsFingerprint, uint32 LayoutVersion,
                               bool bApplyLegacyStructuralPosts = true,
                               bool bApplyTunnelSupportFloor = true) const;

    /**
     * Densité pour une strate Slab (FlatPlain / CrystalChamber).
     * Vide horizontal entre un sol bruité et un plafond bruité.
     */
    float GetSlabDensity(float WorldX, float WorldY, float WorldZ,
                         const FSlabGenerationParams& Params) const;

    /**
      * Densité pour une strate Maze — couloirs sur un treillis 3D déterministe, avec les décisions
      * parent/boucle reconstruites par cellule dans un cache thread-local; le hot path ne parcourt
      * que les capsules déjà émises.
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
     * La chaîne de hauteur complète de SurfaceWorld : structural → cliff → terrace → layer lines →
     * plage. Rend une ALTITUDE monde en voxels, pas une densité.
     *
     * PUBLIQUE pour la même raison que `GetSlabDensity` / `GetMazeDensity` : permettre un test
     * isolé. C'est la référence de `VoxelForge.OpStack.SurfaceHeightEquivalence`, qui compare la
     * pile d'opérateurs de hauteur (`VoxelHeightOp.h`) à cette fonction point par point.
     * Public so the height-op stack can be measured against it — same reason as GetSlabDensity.
     */
    float ComputeSurfaceTerrainZ(float WorldX, float WorldY, const FSurfaceGenerationParams& Params) const;

    /**
     * La colonne de surface : terrain Z, plafond, et le gate d'OVERHANG résolu par colonne
     * (amplitude + direction amont). PUBLIQUES toutes deux pour la même raison que ci-dessus :
     * c'est le seul chemin qui calcule l'overhang — `GetSurfaceDensity` passe `OverhangAmp = 0` —
     * donc c'est la seule référence possible pour `FOverhangShelfMod`.
     * Public because this is the ONLY path that computes the overhang (GetSurfaceDensity passes 0),
     * so it is the only possible reference for the ported op.
     */
    void ComputeSurfaceColumn(float WorldX, float WorldY, int32 ChunkZ,
        const FSurfaceGenerationParams& BaseSurface, const FBiomeContext& BiomeCtx,
        const TArray<FSurfaceGenerationParams>& BiomeParams, FChunkBiomeCache& BiomeCache,
        float& OutTerrainZ, float& OutCeilSurf,
        float& OutOverhangAmp, float& OutDirX, float& OutDirY) const;

    /** Le combine par voxel : colonne → densité, overhang compris, puis le post structurel. */
    float SurfaceDensityFromColumn(float WorldX, float WorldY, float WorldZ,
                                   float TerrainZ, float CeilSurf,
                                   float OverhangAmp, float DirX, float DirY,
                                   const FSurfaceGenerationParams& S) const;

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
     * GAP (bedrock), hors-layout (air constant) et SurfaceWorld plient leurs hypothèses par Z ;
     * SurfaceWorld évalue ses colonnes terrain/plafond avec le MÊME ComputeSurfaceColumn que le
     * chemin densité. Un unique slot cave opt-in peut ajouter le verdict conservatif de sa pile,
     * sous gardes d'archétype et de params bit-identiques. Spine/passages/disturbances/diff restent
     * des gardes conservatrices. Worker-safe (lecture seule + caches thread_local partagés avec
     * GetDensityAt — un verdict Mixed laisse les colonnes chaudes pour la génération qui suit).
     */
    EVoxelTileClass ClassifyTile(const FIntVector& OriginVoxels, int32 Step,
                                 int32 CellsPerAxis) const;
    EVoxelTileClass ClassifyTile(const FIntVector& OriginVoxels, int32 Step,
                                 int32 CellsPerAxis,
                                 struct FVoxelTileClassificationStats* OutStats) const;

private:
    /** Identité process-unique du propriétaire des caches `CP_*` thread_local.
     *  Process-unique owner identity for the `CP_*` thread-local cache key. */
    uint64 DensityCacheOwnerId = 0;

    /** Pick the biome (index into Ctx.Biomes) for a Voronoi site, by its climate. */
    int32 ClassifyBiomeAtSite(float SiteX, float SiteY, const FBiomeContext& Ctx, uint32 SiteHash) const;

    // ComputeSurfaceTerrainZ a été DÉPLACÉE en `public` (voir plus haut) pour que
    // VoxelForge.OpStack.SurfaceHeightEquivalence puisse s'y comparer. Une seule déclaration.
    // Moved to public above so the height-stack test can compare against it. One declaration only.

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

    // ComputeSurfaceColumn et SurfaceDensityFromColumn ont été DÉPLACÉES en `public` (voir plus
    // haut) : c'est le seul chemin qui calcule l'overhang, donc la seule référence possible pour
    // VoxelForge.OpStack.SurfaceHeightEquivalence. Une seule déclaration chacune.
    // Moved to public above — the only path that computes the overhang, hence the only oracle.

    /** (Re)build the per-chunk biome cell grid covering chunk (X,Y) footprint + margin. */
    void RebuildBiomeGrid(int32 ChunkX, int32 ChunkY, int32 ChunkZ,
                          const FBiomeContext& Ctx, FChunkBiomeCache& Cache) const;
};

/**
 * Optional diagnostics for one ClassifyTile call.  This is populated only when the caller asks
 * for it (the streaming profiler does); it is never consulted by the proof and cannot affect a
 * verdict.  Keeping the counters beside the classifier lets the game profile distinguish cache,
 * interval refinement, and exact-lattice work without sampling a different code path.
 */
struct FVoxelTileClassificationStats
{
    uint32 RefineNodes = 0;
    uint32 StackBoxCalls = 0;
    uint32 WholeMixedNodes = 0;
    uint32 WholeSolidNodes = 0;
    uint32 WholeAirNodes = 0;
    uint32 NeedsFinalFieldNodes = 0;
    uint32 SplitNodes = 0;
    uint32 MaxRefinementDepth = 0;
    uint32 ExactCoreSamples = 0;
    uint32 ExactFinalSamples = 0;
    uint32 ExactCoreCacheHits = 0;
    uint32 ExactFinalCacheHits = 0;
    uint32 ExactCoreLeaves = 0;
    uint32 ExactFinalLeaves = 0;
    uint64 StackBoxCycles = 0;
    uint64 ExactCoreCycles = 0;
    uint64 ExactFinalCycles = 0;
    uint32 RoomTailQueries = 0;
    uint32 RoomTailEvaluated = 0;
    uint64 RoomTailCycles = 0;
    uint64 RoomPropagateCycles = 0;
    uint64 RoomExactPrimitiveCycles = 0;
    uint64 RoomCacheWindowCycles = 0;
    int32 RoomNumRooms = 0;
    int32 RoomNumTunnels = 0;
    int32 RoomNumRoomFloorJoins = 0;
    int32 RoomNumPits = 0;
    int32 RoomNumChimneys = 0;
};
