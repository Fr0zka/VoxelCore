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

    // Normale fournie par l'appelant (gradient lu dans la grille de densité, T1.b) —
    // plus d'échantillonnage de densité par vertex. RawNormal pointe solide→air ; on la
    // normalise ici (fallback up si dégénérée).
    auto GetOrCreateVertex = [&](const FVector& WorldPos, const FVector& RawNormal) -> int32
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

        FVector Normal = RawNormal;
        if (!Normal.Normalize())
        {
            Normal = FVector(0.0f, 0.0f, 1.0f);  // dégénéré (zone plate)
        }
        MeshData.Normals.Add(Normal);

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
    // On échantillonne avec un anneau de marge de 1 point de chaque côté (indices -1..GridDim)
    // pour pouvoir calculer les normales par GRADIENT DE GRILLE (T1.b) — différences centrales
    // sur la grille au lieu de 6 appels densité frais par vertex. La marge utilise les mêmes
    // échantillons monde purs qu'un chunk voisin, donc les normales restent continues aux bords
    // de chunk (pas de couture de shading). La géométrie est inchangée au bit près (mêmes
    // positions d'arête) ; seules les normales changent.
    const int32 CellsPerAxis = CHUNK_SIZE / Step;
    const int32 GridDim      = CellsPerAxis + 1;
    const int32 MDim         = GridDim + 2;            // +1 marge de chaque côté

    TArray<float> DensityGrid;
    DensityGrid.SetNumUninitialized(MDim * MDim * MDim);
    for (int32 gz = -1; gz <= GridDim; gz++)
    {
        for (int32 gy = -1; gy <= GridDim; gy++)
        {
            for (int32 gx = -1; gx <= GridDim; gx++)
            {
                DensityGrid[((gz + 1) * MDim + (gy + 1)) * MDim + (gx + 1)] =
                    GetDensity(Chunk, gx * Step, gy * Step, gz * Step);
            }
        }
    }

    // Lecture grille (avec offset de marge) + gradient central depuis la grille.
    auto SampleG = [&](int32 gx, int32 gy, int32 gz) -> float
    {
        return DensityGrid[((gz + 1) * MDim + (gy + 1)) * MDim + (gx + 1)];
    };
    auto GradAt = [&](int32 gx, int32 gy, int32 gz) -> FVector
    {
        // Densité négative=solide, positive=air → le gradient pointe vers l'air (sortant).
        return FVector(
            SampleG(gx + 1, gy, gz) - SampleG(gx - 1, gy, gz),
            SampleG(gx, gy + 1, gz) - SampleG(gx, gy - 1, gz),
            SampleG(gx, gy, gz + 1) - SampleG(gx, gy, gz - 1));
    };

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
                // Densités + positions + gradients aux 8 coins (lus dans la grille).
                float Densities[8];
                FVector Positions[8];
                FVector Gradients[8];

                for (int32 i = 0; i < 8; i++)
                {
                    const int32 GX = cx + CornerOffsets[i].X;
                    const int32 GY = cy + CornerOffsets[i].Y;
                    const int32 GZ = cz + CornerOffsets[i].Z;

                    Densities[i]  = SampleG(GX, GY, GZ);
                    Positions[i]  = ChunkWorldPos
                        + FVector(GX * Step, GY * Step, GZ * Step) * VOXEL_SIZE;
                    Gradients[i]  = GradAt(GX, GY, GZ);
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

                // Interpolation des positions + normales sur les arêtes traversées. Le t est
                // calculé exactement comme InterpolateEdge → positions bit-identiques (topologie
                // inchangée) ; la normale interpole les gradients de coin par le même t.
                FVector EdgeVertices[12];
                FVector EdgeNormals[12];
                for (int32 i = 0; i < 12; i++)
                {
                    if (MCEdgeTable[CaseIndex] & (1 << i))
                    {
                        const int32 A = EdgeCorners[i][0];
                        const int32 B = EdgeCorners[i][1];
                        const float D1 = Densities[A], D2 = Densities[B];
                        const float T = (FMath::Abs(D2 - D1) < KINDA_SMALL_NUMBER)
                            ? 0.5f
                            : FMath::Clamp((IsoLevel - D1) / (D2 - D1), 0.0f, 1.0f);
                        EdgeVertices[i] = Positions[A] + T * (Positions[B] - Positions[A]);
                        EdgeNormals[i]  = Gradients[A] + T * (Gradients[B] - Gradients[A]);
                    }
                }

                // Génère les triangles avec vertices dédupliqués.
                // Ordre 0, 2, 1 (pas 0, 1, 2) pour le winding attendu par RealtimeMesh.
                for (int32 i = 0; MCTriTable[CaseIndex][i] != -1; i += 3)
                {
                    const int32 E0 = MCTriTable[CaseIndex][i];
                    const int32 E1 = MCTriTable[CaseIndex][i + 1];
                    const int32 E2 = MCTriTable[CaseIndex][i + 2];
                    const int32 Idx0 = GetOrCreateVertex(EdgeVertices[E0], EdgeNormals[E0]);
                    const int32 Idx1 = GetOrCreateVertex(EdgeVertices[E1], EdgeNormals[E1]);
                    const int32 Idx2 = GetOrCreateVertex(EdgeVertices[E2], EdgeNormals[E2]);

                    MeshData.Triangles.Add(Idx0);
                    MeshData.Triangles.Add(Idx2);
                    MeshData.Triangles.Add(Idx1);
                }
            }
        }
    }

    return MeshData;
}
