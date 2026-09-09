// VoxelMarchingCubesMesher.h
// Mesh lisse à partir du champ de densité du générateur.
//
// Principe:
// 1. Chaque cellule du chunk (cube 2x2x2 voxels) lit la densité aux 8 coins.
// 2. Un index 8 bits indique quels coins sont au-dessus du IsoLevel.
// 3. Les tables MC (EdgeTable + TriTable) donnent les triangles à générer.
// 4. On interpole la position du vertex le long de chaque arête traversée.
//
// LOD: Step > 1 échantillonne moins souvent → moins de triangles à distance.

#pragma once

#include "CoreMinimal.h"
#include <atomic>
#include "VoxelTypes.h"   // Pour FVoxelMeshData, CHUNK_SIZE, VOXEL_SIZE, etc.
#include "VoxelGenerator.h"
#include "VoxelMarchingCubesMesher.generated.h"

// A run-scoped, read-only density lattice.  The explorer owns the storage and installs the
// pointer only while its canonical tiles are being meshed.  The lattice includes the one-point
// halo needed by every tile, so each world sample is generated once and copied into both adjacent
// tile scratch grids.  It is deliberately not a UObject or a cache with eviction: ownership,
// lifetime, and iteration order remain explicit and deterministic.
struct FVoxelSharedDensityGrid
{
    FIntVector OriginVoxels = FIntVector::ZeroValue;
    int32 Step = 1;
    int32 Dim = 0;
    TArray<float> Samples;

    void Reset()
    {
        OriginVoxels = FIntVector::ZeroValue;
        Step = 1;
        Dim = 0;
        Samples.Reset();
    }
};

UCLASS(BlueprintType)
class VOXELFORGE_API UVoxelMarchingCubesMesher : public UObject
{
    GENERATED_BODY()

public:
    /**
     * Génère le mesh d'une TUILE de clipmap (voir FVoxelTileKey).
     *
     * @param OriginVoxels - Coin min de la tuile en coords VOXEL (= Tile.OriginVoxels()).
     * @param Step         - Taille de cellule en voxels. La tuile couvre CellsPerAxis*Step voxels.
     * @param CellsPerAxis - Nombre de cellules par axe. Les tuiles GROSSIÈRES en utilisent MOINS
     *                       (gen moins chère, maillage plus grossier au loin) tout en couvrant la
     *                       même étendue (extent = CellsPerAxis*Step). Niveau 0 = CHUNK_SIZE.
     * @param OutCaptureGrid - CAPTURE-DURING-MESHING (optionnel). Si non-null ET CellsPerAxis==CHUNK_SIZE
     *                       (tuile pleine résolution, Step==1<<Level, donc 1:1 avec une cellule du clipmap
     *                       de densité), on y recopie les CHUNK_SIZE³ points intérieurs de la grille de
     *                       densité déjà échantillonnée, quantifiés via VF_QuantizeDensity. Cela évite à
     *                       UVoxelDensityVolume de re-sampler GetDensityAt pour ces cellules (le mesher
     *                       les a déjà calculées). Vidé puis rempli ; reste vide si non éligible.
     * @param BandZMinVox / BandZMaxVox - COUPE DE CONTENU PAR STRATE (optionnel, voxels Z INCLUSIFS,
     *                       cf. UVoxelSettings::StrateContentCutMinLevel) : seules les cellules dont
     *                       l'intervalle Z chevauche la bande sont maillées. Une tuile grossière qui
     *                       chevauche une frontière de strate ne maille que la strate du joueur —
     *                       supprime les trous d'aliasing (le bouchon seal/gap plus fin que Step
     *                       tombait entre deux points du treillis) et le mélange de matériaux entre
     *                       strates. Les cellules maillées restent identiques au bit près (mêmes
     *                       échantillons monde purs). Jamais combiné avec OutCaptureGrid.
     */
    FVoxelMeshData GenerateMesh(FIntVector OriginVoxels, int32 Step = 1, int32 CellsPerAxis = CHUNK_SIZE,
                                TArray<uint8>* OutCaptureGrid = nullptr,
                                int32 BandZMinVox = INT32_MIN, int32 BandZMaxVox = INT32_MAX);

    /**
     * F18 — FEUILLE de champ lointain (anneau render-distance, cf. UVoxelSettings::bFarSheetRing).
     * Dans une strate ouverte (SurfaceWorld) le champ lointain est exactement DEUX heightfields —
     * TerrainZ (sol) et CeilSurf (plafond sky-cap), déjà calculés par colonne par l'oracle
     * GetSurfaceHeightAt. On construit donc deux grilles déplacées régulières au lieu d'un marching
     * cubes 3D : sol = polygroup 0, cap = polygroup 1 (classes vraies PAR CONSTRUCTION — pas de vote,
     * pas de sonde de classification), mêmes conventions que GenerateMesh (positions monde cm, UVs
     * planaires, masques couleur F6 biome/pente/fondu, normales du gradient de hauteur, jupes
     * périmètre par seau, run d'indices sol‖cap + NumCeilingTriangles).
     *
     * @param OriginVoxels  - Coin min de la tuile (voxels). Seul XY est utilisé (les hauteurs sont absolues).
     * @param StepXY        - Pas d'échantillonnage XY en voxels (aligné sur l'anneau MC pour la continuité).
     * @param CellsXY       - Cellules par axe XY (extent = CellsXY × StepXY).
     * @param StrateChunkZ  - Chunk Z DANS la strate de référence (le cœur de la bande) — identifie la
     *                        strate dont on maille sol+cap. Hors SurfaceWorld ⇒ mesh vide.
     * @param HoleMin/MaxX/YVox - TROU XY (voxels, Max EXCLUSIF ; sentinelles MAX/MIN = pas de trou) :
     *                        les cellules ENTIÈREMENT dans ce rectangle (la zone couverte par les
     *                        coquilles MC autour du joueur) sont sautées — sinon la feuille recouvre
     *                        le terrain proche avec son échantillonnage grossier. Les cellules à
     *                        cheval restent (anneau de recouvrement au raccord) ; pas de jupe sur
     *                        les bords du trou (le terrain MC remplit derrière).
     * Non couvert (accepté, cf. fable-idea F18) : passages/spine/chasms creusés (le heightfield pur ne
     * les contient pas), diff layer — invisibles à distance de feuille, l'anneau MC proche les garde.
     */
    FVoxelMeshData GenerateSheetMesh(FIntVector OriginVoxels, int32 StepXY, int32 CellsXY,
                                     int32 StrateChunkZ,
                                     int32 HoleMinXVox = INT32_MAX, int32 HoleMinYVox = INT32_MAX,
                                     int32 HoleMaxXVox = INT32_MIN, int32 HoleMaxYVox = INT32_MIN);

    //=========================================================================
    // SERVICES (injectés par AVoxelWorld)
    //=========================================================================

    // Le générateur fournit la densité par voxel.
    UPROPERTY()
    const UVoxelGenerator* Generator = nullptr;

    void SetGenerator(const UVoxelGenerator* InGenerator) { Generator = InGenerator; }

    // AVoxelWorld installs its bShuttingDown flag here. The commandlet path leaves it null.
    // This is intentionally read-only: workers may observe shutdown, but never mutate world state.
    void SetShutdownFlag(const std::atomic<bool>* InShutdownFlag) { ShutdownFlag = InShutdownFlag; }

    // Optional run-scoped shared lattice.  The caller must keep it alive and immutable until all
    // GenerateMesh calls finish.  A malformed/incomplete lattice is ignored and falls back to
    // the canonical generator path rather than changing geometry.
    void SetSharedDensityGrid(const FVoxelSharedDensityGrid* InGrid) { SharedDensityGrid = InGrid; }

private:
    FORCEINLINE bool ShouldAbortWork() const
    {
        return ShutdownFlag && ShutdownFlag->load(std::memory_order_relaxed);
    }

    const std::atomic<bool>* ShutdownFlag = nullptr;
    const FVoxelSharedDensityGrid* SharedDensityGrid = nullptr;

public:

    //=========================================================================
    // SETTINGS
    //=========================================================================

    // Seuil de l'isosurface dans le champ de densité.
    // Convention MC: densité < IsoLevel = solide, >= = air.
    float IsoLevel = 0.0f;

    // SKIRTS — bouchent les fissures aux frontières de tuiles entre niveaux de clipmap voisins
    // (résolutions différentes → les iso-surfaces ne se rejoignent pas exactement). Une jupe
    // (mur court) est extrudée vers le solide depuis chaque arête de surface posée sur une des 6
    // faces externes de la tuile. Voir GenerateMesh.
    bool  bGenerateSkirts = true;
    // Profondeur de la jupe, en CELLULES de la tuile (× Step × VOXEL_SIZE). ~2 cellules couvrent
    // l'écart vers un voisin un niveau plus grossier (cellule 2×). Monter si des fissures persistent.
    float SkirtCells = 2.0f;

    // Conservative 8x8x8 block classifier. Uniform blocks skip the marching-cubes cell loop;
    // when every relevant block has the same verdict, the density grid is skipped as well.
    // ClassifyTile is proof-only, so no samples, resolution, or geometry are removed.
    // The tile classifier is the cheap, whole-tile proof. Keep the optional 8^3-block refinement
    // available for focused diagnostics, but leave it disabled in streaming: repeating a proof on
    // every mixed tile can cost more than the density grid it is meant to avoid.
    bool bUseBlockEarlyOut = false;

    // T2.b — LOD-aware octave reduction (opt-in, copied from UVoxelSettings::LODOctaveDrop).
    // Octaves dropped from per-voxel volumetric noise PER Step doubling: a tile at Step=S
    // drops LODOctaveDrop * log2(S) octaves (see VoxelGenLOD in VoxelGenerator.h).
    // 0 (default) = off — every LOD samples full octaves, byte-identical to before.
    // Réduction d'octaves sur les tuiles grossières ; 0 = désactivé.
    int32 LODOctaveDrop = 0;
};
