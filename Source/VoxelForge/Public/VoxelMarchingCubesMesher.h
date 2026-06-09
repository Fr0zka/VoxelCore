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
#include "VoxelChunk.h"
#include "VoxelGenerator.h"
#include "VoxelMarchingCubesMesher.generated.h"

UCLASS(BlueprintType)
class VOXELFORGE_API UVoxelMarchingCubesMesher : public UObject
{
    GENERATED_BODY()

public:
    /**
     * Génère le mesh d'un chunk.
     *
     * @param Chunk - Le chunk à mesher (on n'utilise que ChunkCoord pour
     *                calculer les positions monde; la densité vient du générateur).
     * @param Step  - Pas d'échantillonnage (1=LOD0, 2=LOD1, 4=LOD2).
     */
    FVoxelMeshData GenerateMesh(const FVoxelChunk& Chunk, int32 Step = 1);

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

    // Distance d'échantillonnage (en voxels) pour calculer la normale par
    // différence centrée du gradient. Plus petit = plus détaillé mais bruité.
    float GradientOffset = 1.0f;

protected:
    //=========================================================================
    // DENSITY + NORMAL SAMPLING
    //=========================================================================

    // Lit la densité à une position locale (via le générateur en coords monde).
    float GetDensity(const FVoxelChunk& Chunk, int32 X, int32 Y, int32 Z) const;

    // Normale lissée: gradient central du champ de densité (pointe solide→air).
    FVector ComputeGradientNormal(float WorldX, float WorldY, float WorldZ) const;

    // Interpolation linéaire le long d'une arête: trouve où la surface
    // traverse entre P1 (densité D1) et P2 (densité D2).
    FVector InterpolateEdge(const FVector& P1, const FVector& P2, float D1, float D2) const;
};
