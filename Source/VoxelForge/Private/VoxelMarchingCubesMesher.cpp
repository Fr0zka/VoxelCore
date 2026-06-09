// VoxelMarchingCubesMesher.cpp
// Implémentation du marching cubes density-only.

#include "VoxelMarchingCubesMesher.h"
#include "MarchingCubesTables.h"

//=============================================================================
// DENSITY SAMPLING
//=============================================================================

float UVoxelMarchingCubesMesher::GetDensity(const FVoxelChunk& Chunk, int32 X, int32 Y, int32 Z) const
{
    // On n'utilise plus de stockage de blocs — densité demandée directement
    // au générateur, qui produit la valeur pour TOUTE coordonnée monde.
    // Si le générateur manque, le chunk est considéré tout-air (IsoLevel par défaut = 0).
    if (!Generator) return 0.0f;

    const float WorldX = Chunk.ChunkCoord.X * CHUNK_SIZE + X;
    const float WorldY = Chunk.ChunkCoord.Y * CHUNK_SIZE + Y;
    const float WorldZ = Chunk.ChunkCoord.Z * CHUNK_SIZE + Z;
    return Generator->GetDensityAt(WorldX, WorldY, WorldZ);
}

//=============================================================================
// EDGE INTERPOLATION
//=============================================================================

FVector UVoxelMarchingCubesMesher::InterpolateEdge(
    const FVector& P1, const FVector& P2,
    float D1, float D2) const
{
    // Densités quasi-égales → on prend le milieu (évite division par ~0).
    if (FMath::Abs(D2 - D1) < KINDA_SMALL_NUMBER)
    {
        return (P1 + P2) * 0.5f;
    }

    // t = 0 → surface en P1; t = 1 → surface en P2.
    float T = (IsoLevel - D1) / (D2 - D1);
    T = FMath::Clamp(T, 0.0f, 1.0f);
    return P1 + T * (P2 - P1);
}

//=============================================================================
// NORMAL (gradient central de densité)
//=============================================================================

FVector UVoxelMarchingCubesMesher::ComputeGradientNormal(float WorldX, float WorldY, float WorldZ) const
{
    // Convention: densité négative = solide, positive = air.
    // Le gradient pointe solide→air = vers l'extérieur de la surface.
    // Pas de négation à faire.
    const float Dx = Generator->GetDensityAt(WorldX + GradientOffset, WorldY, WorldZ)
                   - Generator->GetDensityAt(WorldX - GradientOffset, WorldY, WorldZ);
    const float Dy = Generator->GetDensityAt(WorldX, WorldY + GradientOffset, WorldZ)
                   - Generator->GetDensityAt(WorldX, WorldY - GradientOffset, WorldZ);
    const float Dz = Generator->GetDensityAt(WorldX, WorldY, WorldZ + GradientOffset)
                   - Generator->GetDensityAt(WorldX, WorldY, WorldZ - GradientOffset);

    FVector Normal(Dx, Dy, Dz);
    Normal.Normalize();

    // Fallback si le gradient est dégénéré (zone plate).
    if (Normal.IsNearlyZero())
    {
        Normal = FVector(0.0f, 0.0f, 1.0f);
    }
    return Normal;
}

//=============================================================================
// MAIN ALGORITHM
//=============================================================================

FVoxelMeshData UVoxelMarchingCubesMesher::GenerateMesh(const FVoxelChunk& Chunk, int32 Step)
{
    FVoxelMeshData MeshData;

    // Step valide = puissance de 2 dans [1, 4]
    Step = FMath::Clamp(Step, 1, 4);

    const FVector ChunkWorldPos = Chunk.GetWorldPosition();

    //=========================================================================
    // VERTEX DEDUPLICATION MAP
    //=========================================================================
    // Clé = position quantifiée au 0.01 unité (FIntVector).
    // Valeur = index dans MeshData.Vertices.
    // Les vertices partagés permettent des normales lisses et réduisent le count ~3x.
    TMap<FIntVector, int32> VertexMap;

    auto GetOrCreateVertex = [&](const FVector& WorldPos) -> int32
    {
        const FIntVector Key(
            FMath::RoundToInt(WorldPos.X * 100.0f),
            FMath::RoundToInt(WorldPos.Y * 100.0f),
            FMath::RoundToInt(WorldPos.Z * 100.0f)
        );

        if (int32* Existing = VertexMap.Find(Key))
        {
            return *Existing;
        }

        const int32 NewIndex = MeshData.Vertices.Num();
        VertexMap.Add(Key, NewIndex);

        MeshData.Vertices.Add(WorldPos);

        // Normale par gradient de densité (shading lisse).
        if (Generator)
        {
            const float VoxelX = WorldPos.X / VOXEL_SIZE;
            const float VoxelY = WorldPos.Y / VOXEL_SIZE;
            const float VoxelZ = WorldPos.Z / VOXEL_SIZE;
            MeshData.Normals.Add(ComputeGradientNormal(VoxelX, VoxelY, VoxelZ));
        }
        else
        {
            MeshData.Normals.Add(FVector(0.0f, 0.0f, 1.0f));
        }

        // UVs planaires — le triplanar mapping se fait dans le matériau.
        MeshData.UVs.Add(FVector2D(WorldPos.X / VOXEL_SIZE, WorldPos.Y / VOXEL_SIZE));

        return NewIndex;
    };

    //=========================================================================
    // CORNER + EDGE TABLES
    //=========================================================================
    //       4-------5
    //      /|      /|
    //     / |     / |
    //    7-------6  |
    //    |  0----|--1
    //    | /     | /
    //    |/      |/
    //    3-------2
    static const FIntVector CornerOffsets[8] = {
        {0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0},
        {0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1},
    };

    static const int32 EdgeCorners[12][2] = {
        {0, 1}, {1, 2}, {2, 3}, {3, 0},  // Bas (0-3)
        {4, 5}, {5, 6}, {6, 7}, {7, 4},  // Haut (4-7)
        {0, 4}, {1, 5}, {2, 6}, {3, 7},  // Verticales (8-11)
    };

    //=========================================================================
    // PRÉ-CALCUL DE LA GRILLE DE DENSITÉ
    //=========================================================================
    // Les cellules adjacentes partagent leurs coins : en échantillonnant par
    // cellule (8 coins) on appelle GetDensityAt ~8× de trop pour chaque point.
    // On échantillonne donc chaque point de grille UNE SEULE FOIS dans un tableau
    // plat, puis le balayage des cellules y lit ses coins. GetDensityAt est une
    // fonction pure de la coordonnée monde, donc le maillage est identique au bit
    // près — c'est juste ~7× moins d'appels au LOD0 (33³ au lieu de 32³×8).
    //
    // CellsPerAxis = nombre de cellules par axe ; GridDim = points de grille (+1).
    // Le point de grille (gx,gy,gz) correspond au voxel monde (gx,gy,gz)*Step.
    // Ordre de remplissage z→y→x : garde le cache SDF (search-box) bien chaud.
    const int32 CellsPerAxis = CHUNK_SIZE / Step;
    const int32 GridDim      = CellsPerAxis + 1;

    TArray<float> DensityGrid;
    DensityGrid.SetNumUninitialized(GridDim * GridDim * GridDim);
    for (int32 gz = 0; gz < GridDim; gz++)
    {
        for (int32 gy = 0; gy < GridDim; gy++)
        {
            for (int32 gx = 0; gx < GridDim; gx++)
            {
                DensityGrid[(gz * GridDim + gy) * GridDim + gx] =
                    GetDensity(Chunk, gx * Step, gy * Step, gz * Step);
            }
        }
    }

    //=========================================================================
    // ITÉRATION SUR LES CELLULES
    //=========================================================================
    // On itère sur les CELLULES (indices de grille), pas sur les voxels monde.
    // Coin i de la cellule = point de grille (cx+ox, cy+oy, cz+oz) ; coord voxel
    // monde = ce point × Step. LOD0 Step=1 → full res ; LOD1 Step=2 → ~4× moins.
    for (int32 cz = 0; cz < CellsPerAxis; cz++)
    {
        for (int32 cy = 0; cy < CellsPerAxis; cy++)
        {
            for (int32 cx = 0; cx < CellsPerAxis; cx++)
            {
                // Densités + positions aux 8 coins (lues dans la grille pré-calculée)
                float Densities[8];
                FVector Positions[8];

                for (int32 i = 0; i < 8; i++)
                {
                    const int32 GX = cx + CornerOffsets[i].X;
                    const int32 GY = cy + CornerOffsets[i].Y;
                    const int32 GZ = cz + CornerOffsets[i].Z;

                    Densities[i] = DensityGrid[(GZ * GridDim + GY) * GridDim + GX];
                    Positions[i] = ChunkWorldPos
                        + FVector(GX * Step, GY * Step, GZ * Step) * VOXEL_SIZE;
                }

                // Index de cas MC (8 bits, un par coin)
                int32 CaseIndex = 0;
                for (int32 i = 0; i < 8; i++)
                {
                    if (Densities[i] >= IsoLevel)
                    {
                        CaseIndex |= (1 << i);
                    }
                }

                if (MCEdgeTable[CaseIndex] == 0) continue;  // Pas de surface ici

                // Interpolation des positions sur les arêtes traversées
                FVector EdgeVertices[12];
                for (int32 i = 0; i < 12; i++)
                {
                    if (MCEdgeTable[CaseIndex] & (1 << i))
                    {
                        const int32 A = EdgeCorners[i][0];
                        const int32 B = EdgeCorners[i][1];
                        EdgeVertices[i] = InterpolateEdge(
                            Positions[A], Positions[B],
                            Densities[A], Densities[B]
                        );
                    }
                }

                // Génère les triangles avec vertices dédupliqués.
                // Ordre 0, 2, 1 (pas 0, 1, 2) pour le winding attendu par RealtimeMesh.
                for (int32 i = 0; MCTriTable[CaseIndex][i] != -1; i += 3)
                {
                    const int32 Idx0 = GetOrCreateVertex(EdgeVertices[MCTriTable[CaseIndex][i]]);
                    const int32 Idx1 = GetOrCreateVertex(EdgeVertices[MCTriTable[CaseIndex][i + 1]]);
                    const int32 Idx2 = GetOrCreateVertex(EdgeVertices[MCTriTable[CaseIndex][i + 2]]);

                    MeshData.Triangles.Add(Idx0);
                    MeshData.Triangles.Add(Idx2);
                    MeshData.Triangles.Add(Idx1);
                }
            }
        }
    }

    return MeshData;
}
