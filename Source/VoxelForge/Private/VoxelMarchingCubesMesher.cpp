// VoxelMarchingCubesMesher.cpp
// Implémentation du marching cubes density-only.

#include "VoxelMarchingCubesMesher.h"
#include "MarchingCubesTables.h"
#include "VoxelDensityProfile.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

//=============================================================================
// MAIN ALGORITHM
//=============================================================================
// (L'ancien trio GetDensity / InterpolateEdge / ComputeGradientNormal a été retiré :
//  mort depuis T1.b — la grille pré-échantillonnée fournit positions ET gradients.)

FVoxelMeshData UVoxelMarchingCubesMesher::GenerateMesh(FIntVector OriginVoxels, int32 Step, int32 InCellsPerAxis,
                                                       TArray<uint8>* OutCaptureGrid,
                                                       int32 BandZMinVox, int32 BandZMaxVox)
{
    TRACE_CPUPROFILER_EVENT_SCOPE(VoxelForge_MesherGenerateMesh);
    FVoxelMeshData MeshData;
    if (OutCaptureGrid) { OutCaptureGrid->Reset(); }
    if (!Generator) return MeshData;
    VoxelDensityProfile::FScopedTimer MesherProfileTimer(
        VoxelDensityProfile::EBucket::MesherGenerateMesh);
    VoxelDensityProfile::FScopedTimer MesherSetupTimer(
        VoxelDensityProfile::EBucket::MesherOther);

    // Cell size in voxels. No upper clamp: coarse clipmap levels use bigger steps (the EXTENT
    // grows). Coarse tiles also use FEWER cells (InCellsPerAxis) for cheaper gen.
    Step = FMath::Max(1, Step);

    // T2.b — octave bias for THIS tile: drop LODOctaveDrop octaves per Step doubling from
    // the generator's per-voxel volumetric noise (sub-cell octaves can't shape a coarse
    // isosurface). TGuardValue restores 0 on every exit path, so nothing outside this tile
    // (deco snapping, density-volume fill, the next task on this pooled thread) sees a bias.
    const int32 OctaveBias = (LODOctaveDrop > 0 && Step > 1)
        ? LODOctaveDrop * (int32)FMath::FloorLog2((uint32)Step) : 0;
    TGuardValue<int32> OctaveBiasGuard(VoxelGenLOD::OctaveBias, OctaveBias);
    TGuardValue<int32> SampleStepGuard(VoxelGenLOD::SampleStep, Step);

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

    //=========================================================================
    // F17 — CLASSE DE SURFACE (sol vs plafond sky-cap), par vertex → par triangle
    //=========================================================================
    // Le discriminant est SÉMANTIQUE, pas géométrique (fable-idea F17) : un vertex orienté vers
    // le bas est un sky-cap seulement s'il est proche de CeilSurf ; proche de TerrainZ c'est un
    // surplomb de terrain (reste "sol" — l'ancien vote par tuile mettait le matériau ciel sous
    // les surplombs des tuiles majoritairement plafond). Un futur toit de grotte (aussi down-
    // facing, mais SOUS TerrainZ) tombera correctement côté "sol/roche" par la même règle.
    // Coût : GetSurfaceHeightAt (pile XY complète) UNIQUEMENT pour les vertex down-facing,
    // mémoïsé par colonne quantifiée au pas de la grille → ≤ (colonnes touchées) appels ;
    // ~zéro sur une tuile de sol pur, borné par lattice² sur une tuile de cap.
    static thread_local TArray<uint8>              VertexClasses;   // 0 = sol, 1 = sky-cap
    static thread_local TMap<FIntVector, FVector2f> SurfColMemo;    // (Xq,Yq,chunkZ) → (TerrainZ, CeilSurf) ; X=FLT_MAX ⇒ pas SurfaceWorld
    VertexClasses.Reset();
    SurfColMemo.Reset();

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

        // F17 — classe de surface (voir le bloc en tête de fonction). Down-facing seulement :
        // les vertex de sol/falaise (N.Z ≥ -0.1) sont "sol" sans payer la requête colonne.
        uint8 SurfClass = 0;
        if (Normal.Z < -0.1f)
        {
            const float Zv = WorldPos.Z / VOXEL_SIZE;
            const float StepF = (float)Step;
            const FIntVector MemoKey(
                FMath::RoundToInt(WorldPos.X / (VOXEL_SIZE * StepF)),
                FMath::RoundToInt(WorldPos.Y / (VOXEL_SIZE * StepF)),
                FMath::FloorToInt(Zv / (float)CHUNK_SIZE));
            FVector2f* Col = SurfColMemo.Find(MemoKey);
            if (!Col)
            {
                float TerrainZ = 0.0f, CeilSurf = 0.0f;
                // Un vertex du cap vit à la FRONTIÈRE HAUTE de la strate : arrondi/interpolation
                // peuvent le faire flotter dans le chunk gap/seal juste AU-DESSUS (hors
                // SurfaceWorld ⇒ sonde ratée ⇒ faux "sol"). On retente un chunk plus bas.
                const bool bSurf = Generator->GetSurfaceHeightAt(
                        (float)(MemoKey.X * Step), (float)(MemoKey.Y * Step), MemoKey.Z,
                        TerrainZ, CeilSurf)
                    || Generator->GetSurfaceHeightAt(
                        (float)(MemoKey.X * Step), (float)(MemoKey.Y * Step), MemoKey.Z - 1,
                        TerrainZ, CeilSurf);
                Col = &SurfColMemo.Add(MemoKey, bSurf ? FVector2f(TerrainZ, CeilSurf)
                                                      : FVector2f(FLT_MAX, -FLT_MAX));
            }
            // Plus proche du plafond que du terrain ⇒ sky-cap. (Hors SurfaceWorld : sol.)
            // + garde : un vrai vertex de cap est AU niveau du cap (± slop d'interpolation) ;
            // loin AU-DESSUS = plancher/toit de la strate d'à côté atteint via le retry chunkZ-1
            // (strates contiguës) → reste sol.
            if (Col->X != FLT_MAX && FMath::Abs(Zv - Col->Y) < FMath::Abs(Zv - Col->X)
                && Zv <= Col->Y + 2.0f * StepF)
            {
                SurfClass = 1;
            }
        }
        VertexClasses.Add(SurfClass);

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

    // COUPE DE CONTENU PAR STRATE — restreint le maillage (et l'échantillonnage) aux cellules
    // dont l'intervalle Z chevauche la bande [BandZMinVox, BandZMaxVox] (voxels inclusifs).
    // Les tuiles à capture ne sont jamais bandées (niveau 0 — garde-fou ci-dessous).
    if (OutCaptureGrid) { BandZMinVox = INT32_MIN; BandZMaxVox = INT32_MAX; }
    int32 CzLo = 0, CzHi = CellsPerAxis - 1;
    if (BandZMinVox > INT32_MIN || BandZMaxVox < INT32_MAX)
    {
        auto FloorDivI = [](int64 A, int64 B) -> int32
        {
            const int64 Q = A / B;
            return (int32)(Q - (((A % B) != 0 && ((A < 0) != (B < 0))) ? 1 : 0));
        };
        // Cellule c couvre [O.Z + c*Step, O.Z + (c+1)*Step] : première/dernière cellule contenant
        // la borne (un contact purement tangent est exclu — sa traversée serait hors bande).
        CzLo = FMath::Max(CzLo, FloorDivI((int64)BandZMinVox - OriginVoxels.Z, Step));
        CzHi = FMath::Min(CzHi, FloorDivI((int64)BandZMaxVox - OriginVoxels.Z, Step));
        if (CzHi < CzLo) { return MeshData; }          // tuile entièrement hors bande → vide
    }
    const int32 GzLo = CzLo - 1;                       // coins CzLo..CzHi+1, gradients ±1
    const int32 GzHi = CzHi + 2;                       // (== -1..GridDim sans bande)

    MesherSetupTimer.End();

    // Réutilise le tampon entre tuiles (thread_local) : SetNumUninitialized garde la
    // capacité, donc plus de malloc/free de ~170 Ko (35³ floats) par tuile.
    static thread_local TArray<float> DensityGrid;
    DensityGrid.SetNumUninitialized(MDim * MDim * MDim);
    // Bande de strate : seules les rangées Z réellement lues (cellules CzLo..CzHi + marges de
    // gradient) sont échantillonnées — le reste du tampon reste non initialisé et non lu.
    VoxelDensityProfile::FScopedTimer MesherDensityGridTimer(
        VoxelDensityProfile::EBucket::MesherDensityGrid);
    for (int32 gz = GzLo; gz <= GzHi; gz++)
    {
        if (ShouldAbortWork()) return FVoxelMeshData();
        for (int32 gy = -1; gy <= GridDim; gy++)
        {
            if (ShouldAbortWork()) return FVoxelMeshData();
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
    MesherDensityGridTimer.End();

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
    // F17 — les triangles sont émis dans DEUX seaux (sol / sky-cap, vote majoritaire des
    // classes de vertex) puis concaténés sol-puis-cap : RMC exige un run d'indices contigu
    // par polygroup. Géométrie inchangée au bit près — seul l'ORDRE des triangles bouge.
    static thread_local TArray<int32> GroundTris;
    static thread_local TArray<int32> CapTris;
    GroundTris.Reset();
    CapTris.Reset();
    const bool bProfileMesher = VoxelDensityProfile::IsCycleTimingEnabled();
    uint64 CellClassificationCalls = 0;
    uint64 SurfaceCellCalls = 0;
    for (int32 cz = CzLo; cz <= CzHi; cz++)            // bande de strate : cf. CzLo/CzHi plus haut
    {
        if (ShouldAbortWork()) return FVoxelMeshData();
        for (int32 cy = 0; cy < CellsPerAxis; cy++)
        {
            if (ShouldAbortWork()) return FVoxelMeshData();
            for (int32 cx = 0; cx < CellsPerAxis; cx++)
            {
                const bool bSampleCell = bProfileMesher
                    && VoxelDensityProfile::ShouldSample(
                        VoxelDensityProfile::EBucket::MesherCellClassification);
                const uint64 ClassificationStart = bSampleCell
                    ? FPlatformTime::Cycles64() : 0;
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

                const bool bHasSurface = MCEdgeTable[CaseIndex] != 0;
                if (bSampleCell)
                {
                    VoxelDensityProfile::AddSampledMeasurement(
                        VoxelDensityProfile::EBucket::MesherCellClassification,
                        FPlatformTime::Cycles64() - ClassificationStart);
                }
                ++CellClassificationCalls;
                if (!bHasSurface) continue;  // Pas de surface ici → skip
                ++SurfaceCellCalls;

                // PASSE 2 : positions + gradients aux 8 coins (uniquement si surface présente).
                const uint64 GradientStart = bSampleCell
                    ? FPlatformTime::Cycles64() : 0;
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
                if (bSampleCell)
                {
                    VoxelDensityProfile::AddSampledMeasurement(
                        VoxelDensityProfile::EBucket::MesherGradientNormals,
                        FPlatformTime::Cycles64() - GradientStart);
                }

                // Interpolation des positions + normales sur les arêtes traversées. t = point de
                // traversée de l'iso entre les deux coins (clampé, milieu si densités quasi-égales) ;
                // la normale interpole les gradients de coin par le même t.
                const uint64 VertexInterpolationStart = bSampleCell
                    ? FPlatformTime::Cycles64() : 0;
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
                if (bSampleCell)
                {
                    VoxelDensityProfile::AddSampledMeasurement(
                        VoxelDensityProfile::EBucket::MesherVertexInterpolation,
                        FPlatformTime::Cycles64() - VertexInterpolationStart);
                }

                // Génère les triangles avec vertices dédupliqués.
                // Ordre 0, 2, 1 (pas 0, 1, 2) pour le winding attendu par RealtimeMesh.
                const uint64 StreamBuildingStart = bSampleCell
                    ? FPlatformTime::Cycles64() : 0;
                for (int32 i = 0; MCTriTable[CaseIndex][i] != -1; i += 3)
                {
                    const int32 E0 = MCTriTable[CaseIndex][i];
                    const int32 E1 = MCTriTable[CaseIndex][i + 1];
                    const int32 E2 = MCTriTable[CaseIndex][i + 2];
                    const int32 Idx0 = GetOrCreateVertex(EdgeVertices[E0], EdgeNormals[E0]);
                    const int32 Idx1 = GetOrCreateVertex(EdgeVertices[E1], EdgeNormals[E1]);
                    const int32 Idx2 = GetOrCreateVertex(EdgeVertices[E2], EdgeNormals[E2]);

                    // F17 — vote majoritaire (≥ 2 vertex sky-cap ⇒ triangle sky-cap).
                    const int32 CapVotes = (int32)VertexClasses[Idx0]
                                         + (int32)VertexClasses[Idx1]
                                         + (int32)VertexClasses[Idx2];
                    TArray<int32>& Dst = (CapVotes >= 2) ? CapTris : GroundTris;
                    Dst.Add(Idx0);
                    Dst.Add(Idx2);
                    Dst.Add(Idx1);
                }
                if (bSampleCell)
                {
                    VoxelDensityProfile::AddSampledMeasurement(
                        VoxelDensityProfile::EBucket::MesherStreamBuilding,
                        FPlatformTime::Cycles64() - StreamBuildingStart);
                }
            }
        }
    }

    if (bProfileMesher)
    {
        VoxelDensityProfile::AddMeasurement(
            VoxelDensityProfile::EBucket::MesherCellClassification,
            0, CellClassificationCalls);
        VoxelDensityProfile::AddMeasurement(
            VoxelDensityProfile::EBucket::MesherGradientNormals,
            0, SurfaceCellCalls);
        VoxelDensityProfile::AddMeasurement(
            VoxelDensityProfile::EBucket::MesherVertexInterpolation,
            0, SurfaceCellCalls);
        VoxelDensityProfile::AddMeasurement(
            VoxelDensityProfile::EBucket::MesherStreamBuilding,
            0, SurfaceCellCalls);
    }

    VoxelDensityProfile::FScopedTimer MesherFinalisationTimer(
        VoxelDensityProfile::EBucket::MesherOther);

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
    if (bGenerateSkirts && (GroundTris.Num() + CapTris.Num()) > 0)
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

        // F17 — jupes émises PAR SEAU : chaque jupe hérite la classe (donc le polygroup /
        // matériau) de son triangle source ; ajouter les jupes après coup casserait les runs
        // d'indices contigus par groupe qu'exige RMC.
        auto EmitSkirts = [&](TArray<int32>& Tris)
        {
            // On ajoute en itérant : on fige le nombre de triangles de surface et on n'ajoute qu'au-delà.
            const int32 BaseTriNum = Tris.Num();
            for (int32 t = 0; t + 2 < BaseTriNum; t += 3)
            {
                const int32 Tri[3] = { Tris[t], Tris[t + 1], Tris[t + 2] };
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
                    Tris.Add(iA);  Tris.Add(iB);  Tris.Add(iB2);
                    Tris.Add(iA);  Tris.Add(iB2); Tris.Add(iA2);
                    Tris.Add(iA);  Tris.Add(iB2); Tris.Add(iB);
                    Tris.Add(iA);  Tris.Add(iA2); Tris.Add(iB2);
                }
            }
        };
        EmitSkirts(GroundTris);
        EmitSkirts(CapTris);
    }

    // F17 — concatène sol PUIS sky-cap : un run d'indices contigu par polygroup (voir
    // FVoxelMeshData::NumCeilingTriangles). Mêmes triangles qu'avant, seul l'ordre change.
    MeshData.Triangles.Reserve(GroundTris.Num() + CapTris.Num());
    MeshData.Triangles.Append(GroundTris);
    MeshData.Triangles.Append(CapTris);
    MeshData.NumCeilingTriangles = CapTris.Num() / 3;

    return MeshData;
}

//=============================================================================
// F18 — FEUILLE DE CHAMP LOINTAIN (voir le doc de la déclaration dans le .h)
//=============================================================================
// Deux grilles déplacées (sol + cap) au lieu d'un marching cubes 3D. Conventions IDENTIQUES à
// GenerateMesh : positions monde cm, UVs planaires (voxels), masques F6 par GetBiomeMaterialAt,
// jupes double-face extrudées le long de −N par seau, indices sol‖cap + NumCeilingTriangles.
// Winding : le chemin MC émet (0,2,1) des tables Bourke ⇒ une surface orientée +Z s'écrit
// (x,y) → (x,y+1) → (x+1,y) ; le cap (orienté −Z) inverse.
FVoxelMeshData UVoxelMarchingCubesMesher::GenerateSheetMesh(FIntVector OriginVoxels, int32 StepXY,
                                                            int32 CellsXY, int32 StrateChunkZ,
                                                            int32 HoleMinXVox, int32 HoleMinYVox,
                                                            int32 HoleMaxXVox, int32 HoleMaxYVox)
{
    FVoxelMeshData MeshData;
    if (!Generator) return MeshData;
    StepXY  = FMath::Max(1, StepXY);
    CellsXY = FMath::Clamp(CellsXY, 2, 512);

    // TROU XY — cellule [x0, x0+Step]×[y0, y0+Step] entièrement dans le rectangle (Max exclusif)
    // ⇒ sautée : la zone est couverte par les coquilles MC (voir VoxelWorld.h SheetHole*).
    auto CellInHole = [&](int32 cx, int32 cy) -> bool
    {
        const int32 X0 = OriginVoxels.X + cx * StepXY;
        const int32 Y0 = OriginVoxels.Y + cy * StepXY;
        return X0 >= HoleMinXVox && X0 + StepXY <= HoleMaxXVox
            && Y0 >= HoleMinYVox && Y0 + StepXY <= HoleMaxYVox;
    };

    const int32 GridDim = CellsXY + 1;   // points de grille par axe
    const int32 MDim    = GridDim + 2;   // + anneau de marge ±1 (normales continues entre feuilles)

    // Early-out : la validité SurfaceWorld ne dépend que de ChunkZ (les strates sont des couches
    // horizontales) — une sonde suffit. La sentinelle par colonne plus bas reste par prudence.
    {
        float T0 = 0.0f, C0 = 0.0f;
        if (!Generator->GetSurfaceHeightAt((float)OriginVoxels.X, (float)OriginVoxels.Y,
                                           StrateChunkZ, T0, C0))
        {
            return MeshData;   // pas une strate SurfaceWorld → feuille vide
        }
    }

    // Colonnes (TerrainZ, CeilSurf) en Z-voxel monde absolu, marge incluse. X==FLT_MAX ⇒ invalide.
    static thread_local TArray<FVector2f> SheetCols;
    SheetCols.SetNumUninitialized(MDim * MDim);
    for (int32 gy = -1; gy <= GridDim; ++gy)
    {
        if (ShouldAbortWork()) return FVoxelMeshData();
        for (int32 gx = -1; gx <= GridDim; ++gx)
        {
            float Tz = 0.0f, Cz = 0.0f;
            const bool bOk = Generator->GetSurfaceHeightAt(
                (float)(OriginVoxels.X + gx * StepXY), (float)(OriginVoxels.Y + gy * StepXY),
                StrateChunkZ, Tz, Cz);
            SheetCols[(gy + 1) * MDim + (gx + 1)] = bOk ? FVector2f(Tz, Cz)
                                                        : FVector2f(FLT_MAX, -FLT_MAX);
        }
    }
    auto Col = [&](int32 gx, int32 gy) -> const FVector2f&
    {
        return SheetCols[(gy + 1) * MDim + (gx + 1)];
    };

    // Index de vertex par point de grille (−1 = pas créé), sol et cap séparés — le partage de
    // vertex est structurel (grille régulière), pas besoin de map de déduplication.
    static thread_local TArray<int32> GroundIdx;
    static thread_local TArray<int32> CapIdx;
    GroundIdx.Init(-1, GridDim * GridDim);
    CapIdx.Init(-1, GridDim * GridDim);

    auto MakeVert = [&](int32 gx, int32 gy, bool bCap) -> int32
    {
        int32& Slot = (bCap ? CapIdx : GroundIdx)[gy * GridDim + gx];
        if (Slot >= 0) return Slot;

        const FVector2f C = Col(gx, gy);
        const float H = bCap ? C.Y : C.X;
        // Pente par différences centrales sur la grille de hauteurs (voxels/voxels — l'échelle cm
        // se simplifie). Voisin invalide ⇒ composante plate (ne devrait pas arriver : cf. early-out).
        auto HAt = [&](int32 x, int32 y) -> float
        {
            const FVector2f& N = Col(x, y);
            return (N.X == FLT_MAX) ? H : (bCap ? N.Y : N.X);
        };
        const float Sx = (HAt(gx + 1, gy) - HAt(gx - 1, gy)) / (2.0f * (float)StepXY);
        const float Sy = (HAt(gx, gy + 1) - HAt(gx, gy - 1)) / (2.0f * (float)StepXY);
        // Sol : air au-dessus ⇒ normale vers le haut. Cap : l'air (l'intérieur de la strate) est
        // EN-DESSOUS ⇒ normale vers le bas (−∇(z − C) = (Cx, Cy, −1)).
        FVector Normal = bCap ? FVector(Sx, Sy, -1.0f) : FVector(-Sx, -Sy, 1.0f);
        if (!Normal.Normalize())
        {
            Normal = FVector(0.0f, 0.0f, bCap ? -1.0f : 1.0f);
        }

        const float Xv = (float)(OriginVoxels.X + gx * StepXY);
        const float Yv = (float)(OriginVoxels.Y + gy * StepXY);
        const FVector WorldPos(Xv * VOXEL_SIZE, Yv * VOXEL_SIZE, H * VOXEL_SIZE);

        Slot = MeshData.Vertices.Num();
        MeshData.Vertices.Add(WorldPos);
        MeshData.Normals.Add(Normal);
        MeshData.UVs.Add(FVector2D(Xv, Yv));   // == WorldPos.XY / VOXEL_SIZE (convention MC)

        // Mêmes masques F6 que GenerateMesh (palette biome dominant/voisin, pente, fondu).
        int32 PalD = 0, PalN = 0; float BlendW = 0.0f;
        Generator->GetBiomeMaterialAt(Xv, Yv, H, PalD, PalN, BlendW);
        const uint8 Rr = (uint8)FMath::Clamp(PalD, 0, 255);
        const uint8 Aa = (uint8)FMath::Clamp(PalN, 0, 255);
        const uint8 Gg = (uint8)FMath::Clamp(FMath::RoundToInt((1.0f - FMath::Abs((float)Normal.Z)) * 255.0f), 0, 255);
        const uint8 Bb = (uint8)FMath::Clamp(FMath::RoundToInt(BlendW * 255.0f), 0, 255);
        MeshData.Colors.Add(FColor(Rr, Gg, Bb, Aa));

        return Slot;
    };

    static thread_local TArray<int32> SheetGroundTris;
    static thread_local TArray<int32> SheetCapTris;
    SheetGroundTris.Reset();
    SheetCapTris.Reset();

    for (int32 cy = 0; cy < CellsXY; ++cy)
    {
        if (ShouldAbortWork()) return FVoxelMeshData();
        for (int32 cx = 0; cx < CellsXY; ++cx)
        {
            if (CellInHole(cx, cy)) continue;   // couverte par les coquilles MC proches
            if (Col(cx, cy).X == FLT_MAX || Col(cx + 1, cy).X == FLT_MAX ||
                Col(cx, cy + 1).X == FLT_MAX || Col(cx + 1, cy + 1).X == FLT_MAX)
            {
                continue;
            }

            // SOL (polygroup 0) — orienté +Z.
            const int32 g00 = MakeVert(cx,     cy,     false);
            const int32 g10 = MakeVert(cx + 1, cy,     false);
            const int32 g01 = MakeVert(cx,     cy + 1, false);
            const int32 g11 = MakeVert(cx + 1, cy + 1, false);
            SheetGroundTris.Add(g00); SheetGroundTris.Add(g01); SheetGroundTris.Add(g10);
            SheetGroundTris.Add(g10); SheetGroundTris.Add(g01); SheetGroundTris.Add(g11);

            // CAP (polygroup 1) — orienté −Z ⇒ winding inversé.
            const int32 c00 = MakeVert(cx,     cy,     true);
            const int32 c10 = MakeVert(cx + 1, cy,     true);
            const int32 c01 = MakeVert(cx,     cy + 1, true);
            const int32 c11 = MakeVert(cx + 1, cy + 1, true);
            SheetCapTris.Add(c00); SheetCapTris.Add(c10); SheetCapTris.Add(c01);
            SheetCapTris.Add(c10); SheetCapTris.Add(c11); SheetCapTris.Add(c01);
        }
    }

    // JUPES périmètre — mêmes règles que le chemin MC : extrusion le long de −N (vers le solide :
    // bas pour le sol, HAUT pour le cap), double face, émises PAR SEAU (héritent le polygroup).
    // Entre feuilles voisines les coins coïncident (mêmes échantillons monde) → pas de fissure ;
    // la jupe couvre surtout la couture feuille ↔ anneau MC et les décalages de niveau.
    if (bGenerateSkirts && (SheetGroundTris.Num() + SheetCapTris.Num()) > 0)
    {
        const float SkirtDepth = FMath::Max(1.0f, SkirtCells) * (float)StepXY * VOXEL_SIZE;

        auto EmitEdgeSkirt = [&](int32 ax, int32 ay, int32 bx, int32 by, bool bCap, TArray<int32>& Tris)
        {
            if (Col(ax, ay).X == FLT_MAX || Col(bx, by).X == FLT_MAX) return;
            const int32 iA = MakeVert(ax, ay, bCap);
            const int32 iB = MakeVert(bx, by, bCap);
            // COPIES par valeur — les Add ci-dessous réallouent Vertices/Normals/Colors.
            const FVector PA = MeshData.Vertices[iA], PB = MeshData.Vertices[iB];
            const FVector NA = MeshData.Normals[iA],  NB = MeshData.Normals[iB];
            const FColor  CA = MeshData.Colors[iA],   CB = MeshData.Colors[iB];

            const int32 iA2 = MeshData.Vertices.Num();
            MeshData.Vertices.Add(PA - NA * SkirtDepth);
            MeshData.Normals.Add(NA);
            MeshData.UVs.Add(FVector2D(PA.X / VOXEL_SIZE, PA.Y / VOXEL_SIZE));
            MeshData.Colors.Add(CA);
            const int32 iB2 = MeshData.Vertices.Num();
            MeshData.Vertices.Add(PB - NB * SkirtDepth);
            MeshData.Normals.Add(NB);
            MeshData.UVs.Add(FVector2D(PB.X / VOXEL_SIZE, PB.Y / VOXEL_SIZE));
            MeshData.Colors.Add(CB);

            Tris.Add(iA); Tris.Add(iB);  Tris.Add(iB2);
            Tris.Add(iA); Tris.Add(iB2); Tris.Add(iA2);
            Tris.Add(iA); Tris.Add(iB2); Tris.Add(iB);
            Tris.Add(iA); Tris.Add(iA2); Tris.Add(iB2);
        };

        // Une jupe n'est émise que si la cellule de bord adjacente a réellement été maillée
        // (pas dans le trou XY) — sinon mur flottant sans surface.
        for (int32 c = 0; c < CellsXY; ++c)
        {
            if (ShouldAbortWork()) return FVoxelMeshData();
            for (int32 Pass = 0; Pass < 2; ++Pass)
            {
                const bool bCap = (Pass == 1);
                TArray<int32>& Tris = bCap ? SheetCapTris : SheetGroundTris;
                if (!CellInHole(c, 0))
                    EmitEdgeSkirt(c, 0,       c + 1, 0,       bCap, Tris);   // bord Y-min
                if (!CellInHole(c, CellsXY - 1))
                    EmitEdgeSkirt(c, CellsXY, c + 1, CellsXY, bCap, Tris);   // bord Y-max
                if (!CellInHole(0, c))
                    EmitEdgeSkirt(0,       c, 0,       c + 1, bCap, Tris);   // bord X-min
                if (!CellInHole(CellsXY - 1, c))
                    EmitEdgeSkirt(CellsXY, c, CellsXY, c + 1, bCap, Tris);   // bord X-max
            }
        }
    }

    // Concatène sol PUIS cap : un run d'indices contigu par polygroup (contrat F17/BuildTileStreamSet).
    MeshData.Triangles.Reserve(SheetGroundTris.Num() + SheetCapTris.Num());
    MeshData.Triangles.Append(SheetGroundTris);
    MeshData.Triangles.Append(SheetCapTris);
    MeshData.NumCeilingTriangles = SheetCapTris.Num() / 3;

    return MeshData;
}
