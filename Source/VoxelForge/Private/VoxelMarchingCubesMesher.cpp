// VoxelMarchingCubesMesher.cpp
// Implémentation du marching cubes density-only.

#include "VoxelMarchingCubesMesher.h"
#include "MarchingCubesTables.h"

//=============================================================================
// MAIN ALGORITHM
//=============================================================================
// (L'ancien trio GetDensity / InterpolateEdge / ComputeGradientNormal a été retiré :
//  mort depuis T1.b — la grille pré-échantillonnée fournit positions ET gradients.)

FVoxelMeshData UVoxelMarchingCubesMesher::GenerateMesh(FIntVector OriginVoxels, int32 Step, int32 InCellsPerAxis,
                                                       TArray<uint8>* OutCaptureGrid)
{
    FVoxelMeshData MeshData;
    if (OutCaptureGrid) { OutCaptureGrid->Reset(); }
    if (!Generator) return MeshData;

    // Cell size in voxels. No upper clamp: coarse clipmap levels use bigger steps (the EXTENT
    // grows). Coarse tiles also use FEWER cells (InCellsPerAxis) for cheaper gen.
    Step = FMath::Max(1, Step);

    // World-cm origin of the tile's min corner (positions are built relative to this).
    const FVector ChunkWorldPos = FVector(OriginVoxels) * VOXEL_SIZE;

    //=========================================================================
    // VERTEX DEDUPLICATION MAP
    //=========================================================================
    // Clé = position quantifiée au 0.01 unité (FIntVector).
    // Valeur = index dans MeshData.Vertices.
    // Les vertices partagés permettent des normales lisses et réduisent le count ~3x.
    // Réutilisé d'une tuile à l'autre (thread_local) : Reset garde les buckets alloués →
    // plus de (ré)allocation de hash-map par tuile (chaque worker a son propre exemplaire).
    static thread_local TMap<FIntVector, int32> VertexMap;
    VertexMap.Reset();

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

        //---------------------------------------------------------------------
        // VERTEX COLOUR — masques pour le matériau triplanar maître (F6).
        //   R = index de palette du biome DOMINANT (0-255) — re-skin par biome.
        //   G = pente (0 = sol/plafond plat, 1 = paroi verticale) — roche sur falaises.
        //   B = poids de fondu de bordure (0 au cœur d'un biome → ~0.5 à la frontière).
        //   A = index de palette du biome VOISIN — le matériau lerp(R,A) par B → bords sans couture.
        // La hauteur/snow-line se déduit de WorldPosition.Z dans le matériau (pas besoin de canal).
        // Coût: une résolution biome par vertex UNIQUE (déduplication) ; nul si la strate n'a pas
        // de biomes (GetBiomeMaterialAt sort en O(1) → palette 0 partout).
        int32 PalD = 0, PalN = 0; float BlendW = 0.0f;
        Generator->GetBiomeMaterialAt(WorldPos.X / VOXEL_SIZE, WorldPos.Y / VOXEL_SIZE,
                                      WorldPos.Z / VOXEL_SIZE, PalD, PalN, BlendW);
        const uint8 R = (uint8)FMath::Clamp(PalD, 0, 255);
        const uint8 A = (uint8)FMath::Clamp(PalN, 0, 255);
        const uint8 G = (uint8)FMath::Clamp(FMath::RoundToInt((1.0f - FMath::Abs((float)Normal.Z)) * 255.0f), 0, 255);
        const uint8 Bc = (uint8)FMath::Clamp(FMath::RoundToInt(BlendW * 255.0f), 0, 255);
        MeshData.Colors.Add(FColor(R, G, Bc, A));

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
    const int32 CellsPerAxis = FMath::Clamp(InCellsPerAxis, 2, CHUNK_SIZE);  // coarse tiles use fewer
    const int32 GridDim      = CellsPerAxis + 1;
    const int32 MDim         = GridDim + 2;            // +1 marge de chaque côté

    // Réutilise le tampon entre tuiles (thread_local) : SetNumUninitialized garde la
    // capacité, donc plus de malloc/free de ~170 Ko (35³ floats) par tuile.
    static thread_local TArray<float> DensityGrid;
    DensityGrid.SetNumUninitialized(MDim * MDim * MDim);
    for (int32 gz = -1; gz <= GridDim; gz++)
    {
        for (int32 gy = -1; gy <= GridDim; gy++)
        {
            for (int32 gx = -1; gx <= GridDim; gx++)
            {
                // World voxel = tile origin + grid offset scaled by the cell size (Step).
                DensityGrid[((gz + 1) * MDim + (gy + 1)) * MDim + (gx + 1)] =
                    Generator->GetDensityAt(
                        OriginVoxels.X + gx * Step,
                        OriginVoxels.Y + gy * Step,
                        OriginVoxels.Z + gz * Step);
            }
        }
    }

    // ── CAPTURE-DURING-MESHING ──
    // Si demandé et que la tuile est pleine résolution (CellsPerAxis==CHUNK_SIZE ⇒ Step==1<<Level,
    // donc chaque point de grille = exactement une cellule du clipmap de densité), on recopie les
    // CHUNK_SIZE³ points INTÉRIEURS (g=0..CHUNK_SIZE-1, on exclut le point frontière +1 — il
    // appartient à la tuile voisine — et l'anneau de marge ±1) dans OutCaptureGrid, quantifiés.
    // UVoxelDensityVolume réutilise ces octets au lieu de re-sampler GetDensityAt. Pure lecture de
    // DensityGrid : la forme de grille, la boucle deux passes, l'anneau de marge et la réutilisation
    // thread_local restent intacts (§8.10). Ordre X→Y→Z (x rapide) = layout attendu par l'ingest.
    if (OutCaptureGrid && CellsPerAxis == CHUNK_SIZE)
    {
        OutCaptureGrid->SetNumUninitialized(CHUNK_SIZE * CHUNK_SIZE * CHUNK_SIZE);
        uint8* Cap = OutCaptureGrid->GetData();
        int32 ci = 0;
        for (int32 gz = 0; gz < CHUNK_SIZE; ++gz)
        for (int32 gy = 0; gy < CHUNK_SIZE; ++gy)
        for (int32 gx = 0; gx < CHUNK_SIZE; ++gx)
        {
            Cap[ci++] = VF_QuantizeDensity(DensityGrid[((gz + 1) * MDim + (gy + 1)) * MDim + (gx + 1)]);
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
                // PASSE 1 : densités aux 8 coins + index de cas MC SEULEMENT.
                // ~70% des cellules d'un chunk sont tout-roc ou tout-air (aucune surface) ;
                // on les rejette ICI, AVANT de payer les 8 positions + 8 gradients (48 lectures
                // grille + maths vectorielles). Sortie bit-identique : positions et gradients ne
                // servent qu'aux cellules réellement traversées par l'isosurface.
                float Densities[8];
                int32 CaseIndex = 0;
                for (int32 i = 0; i < 8; i++)
                {
                    const float D = SampleG(cx + CornerOffsets[i].X,
                                            cy + CornerOffsets[i].Y,
                                            cz + CornerOffsets[i].Z);
                    Densities[i] = D;
                    if (D >= IsoLevel) CaseIndex |= (1 << i);
                }

                if (MCEdgeTable[CaseIndex] == 0) continue;  // Pas de surface ici → skip

                // PASSE 2 : positions + gradients aux 8 coins (uniquement si surface présente).
                FVector Positions[8];
                FVector Gradients[8];
                for (int32 i = 0; i < 8; i++)
                {
                    const int32 GX = cx + CornerOffsets[i].X;
                    const int32 GY = cy + CornerOffsets[i].Y;
                    const int32 GZ = cz + CornerOffsets[i].Z;
                    Positions[i] = ChunkWorldPos
                        + FVector(GX * Step, GY * Step, GZ * Step) * VOXEL_SIZE;
                    Gradients[i] = GradAt(GX, GY, GZ);
                }

                // Interpolation des positions + normales sur les arêtes traversées. t = point de
                // traversée de l'iso entre les deux coins (clampé, milieu si densités quasi-égales) ;
                // la normale interpole les gradients de coin par le même t.
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

    //=========================================================================
    // SKIRTS — boucheurs de fissures aux coutures de LOD (clipmap)
    //=========================================================================
    // Deux tuiles de niveaux voisins maillent à des résolutions différentes : leurs iso-surfaces
    // ne se rejoignent pas parfaitement le long de la face partagée → une fine fissure traversante.
    // On la scelle en suspendant une courte « jupe » (mur) sous chaque arête de surface posée sur
    // l'une des 6 faces externes de la tuile, extrudée VERS LE SOLIDE le long de la normale inversée
    // sur ~une taille de cellule. Là où la surface du voisin est décalée, les jupes des deux tuiles
    // se recouvrent dans la roche et ferment le trou ; ailleurs la jupe est enterrée et invisible.
    // Émis en DOUBLE FACE (deux orientations) pour s'afficher quel que soit le côté caméra / le
    // matériau. Une arête de surface posée sur une face de frontière a sa coordonnée d'axe EXACTE
    // (l'interpolation MC garde fixe l'axe de la face) → comparaison flottante exacte fiable.
    if (bGenerateSkirts && MeshData.Triangles.Num() > 0)
    {
        const float ExtentCm = (float)(CellsPerAxis * Step) * VOXEL_SIZE;
        const float MinX = ChunkWorldPos.X, MinY = ChunkWorldPos.Y, MinZ = ChunkWorldPos.Z;
        const float MaxX = MinX + ExtentCm, MaxY = MinY + ExtentCm, MaxZ = MinZ + ExtentCm;
        const float SkirtDepth = FMath::Max(1.0f, SkirtCells) * (float)Step * VOXEL_SIZE;

        auto OnBoundaryPlane = [&](const FVector& A, const FVector& B) -> bool
        {
            return (A.X == MinX && B.X == MinX) || (A.X == MaxX && B.X == MaxX)
                || (A.Y == MinY && B.Y == MinY) || (A.Y == MaxY && B.Y == MaxY)
                || (A.Z == MinZ && B.Z == MinZ) || (A.Z == MaxZ && B.Z == MaxZ);
        };

        auto AddSkirtVert = [&](const FVector& Pos, const FVector& Nrm, const FColor& Col) -> int32
        {
            const int32 Idx = MeshData.Vertices.Num();
            MeshData.Vertices.Add(Pos);
            MeshData.Normals.Add(Nrm);
            MeshData.UVs.Add(FVector2D(Pos.X / VOXEL_SIZE, Pos.Y / VOXEL_SIZE));
            MeshData.Colors.Add(Col);   // hérite la couleur du vertex source (parallèle aux autres tableaux)
            return Idx;
        };

        // On ajoute en itérant : on fige le nombre de triangles de surface et on n'ajoute qu'au-delà.
        const int32 BaseTriNum = MeshData.Triangles.Num();
        for (int32 t = 0; t + 2 < BaseTriNum; t += 3)
        {
            const int32 Tri[3] = { MeshData.Triangles[t], MeshData.Triangles[t + 1], MeshData.Triangles[t + 2] };
            for (int32 e = 0; e < 3; ++e)
            {
                const int32 iA = Tri[e], iB = Tri[(e + 1) % 3];
                // COPIES par valeur — AddSkirtVert réalloue Vertices/Normals (invaliderait des refs).
                const FVector PA = MeshData.Vertices[iA];
                const FVector PB = MeshData.Vertices[iB];
                if (!OnBoundaryPlane(PA, PB)) continue;

                const FVector NA = MeshData.Normals[iA];
                const FVector NB = MeshData.Normals[iB];
                const FColor  CA = MeshData.Colors[iA];
                const FColor  CB = MeshData.Colors[iB];
                const int32 iA2 = AddSkirtVert(PA - NA * SkirtDepth, NA, CA);
                const int32 iB2 = AddSkirtVert(PB - NB * SkirtDepth, NB, CB);

                // Quad (iA, iB, iB2, iA2) → 2 triangles, émis dans LES DEUX orientations.
                MeshData.Triangles.Add(iA);  MeshData.Triangles.Add(iB);  MeshData.Triangles.Add(iB2);
                MeshData.Triangles.Add(iA);  MeshData.Triangles.Add(iB2); MeshData.Triangles.Add(iA2);
                MeshData.Triangles.Add(iA);  MeshData.Triangles.Add(iB2); MeshData.Triangles.Add(iB);
                MeshData.Triangles.Add(iA);  MeshData.Triangles.Add(iA2); MeshData.Triangles.Add(iB2);
            }
        }
    }

    return MeshData;
}
