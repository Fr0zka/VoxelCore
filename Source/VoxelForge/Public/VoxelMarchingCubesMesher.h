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
#include "VoxelTypes.h"   // Pour FVoxelMeshData, CHUNK_SIZE, VOXEL_SIZE, etc.
#include "VoxelGenerator.h"
#include "VoxelMarchingCubesMesher.generated.h"

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
     */
    FVoxelMeshData GenerateMesh(FIntVector OriginVoxels, int32 Step = 1, int32 CellsPerAxis = CHUNK_SIZE,
                                TArray<uint8>* OutCaptureGrid = nullptr);

    //=========================================================================
    // SERVICES (injectés par AVoxelWorld)
    //=========================================================================

    // Le générateur fournit la densité par voxel.
    UPROPERTY()
    const UVoxelGenerator* Generator = nullptr;

    void SetGenerator(const UVoxelGenerator* InGenerator) { Generator = InGenerator; }

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
};
