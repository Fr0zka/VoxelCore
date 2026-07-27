// VoxelForgeOpStackTunnelTest.cpp
// TunnelNetwork — ÉTAPE A : le squelette SDF, sans les modificateurs de détail.
// TunnelNetwork — STAGE A: the SDF spine, without the detail modifiers.
//
// POURQUOI UN TEST D'UNE PILE INCOMPLÈTE
// `GetDensityWithParams` fait ~1080 lignes et treize modificateurs de détail. Tout porter avant de
// pouvoir rien vérifier, ce serait écrire ~600 lignes non compilées par-dessus ~200 non vérifiées —
// exactement le motif que `AUDIT §P3` documente et que ce refactor a évité six fois de suite.
//
// La sortie : **tous les modificateurs de détail sont pilotés par une amplitude**, et
// `FStrateGenerationParams` les laisse déjà TOUS à zéro par défaut (`BuildParamsFromDefinition` ne
// les fusionne plus globalement — ils viennent d'ops par salle). Une seule exception,
// `SurfaceRoughness = 5`. Les mettre à zéro fait passer l'ORIGINAL par exactement le chemin que
// l'étape A a porté, donc l'étape A est vérifiable AUJOURD'HUI, bit à bit, contre la vraie fonction.
// Même discipline que la passe « défauts puis tous les ops ON » du test de pile de hauteur, prise
// dans l'autre sens.
//
// CE QUE CE TEST NE PROUVE PAS (et le dit) : rien sur les 13 modificateurs, rien sur l'override d'op
// par salle, et rien sur le saut de tuile — `FRoomGraphSource::EffectOverBox` rend `Both`, donc
// aucun verdict n'est prouvable à ce stade. Ces trois manques sont l'étape B et l'étape C.
//
// ⚠️ ÉCHANTILLONNAGE PAR GRAPPES, PAS UNIFORME. Le cache SDF se reconstruit quand la requête sort de
// sa boîte de recherche ; 20 000 points uniformément aléatoires feraient ~20 000 `BuildChunkCache`
// par chemin, et un test qui dure trois minutes est un test qu'on finit par ne plus lancer. On tire
// donc N chunks et M points DANS chacun — ce qui est aussi plus représentatif du vrai motif d'accès
// (un mesher parcourt une tuile, il ne saute pas au hasard).

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Async/ParallelFor.h"
#include "HAL/PlatformMisc.h"

#include "VoxelForgeTestFixture.h"
#include "VoxelDensityOpStack.h"

#include <atomic>

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FVoxelForgeOpStackTunnelTest,
    "VoxelForge.OpStack.TunnelNetworkSpineEquivalence",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

namespace
{
    constexpr int32 NumTunnelChunks  = 24;
    constexpr int32 PointsPerChunk   = 250;
    constexpr int32 NumTunnelSamples = NumTunnelChunks * PointsPerChunk;

    /**
     * Met à zéro tout ce que l'étape A n'a pas encore porté, pour que l'original prenne le même
     * chemin. Tout sauf `SurfaceRoughness` est DÉJÀ à zéro par défaut ; on l'écrit quand même, parce
     * qu'un test qui dépend d'un défaut se casse le jour où le défaut change, et silencieusement.
     */
    void DisableStageBModifiers(FStrateGenerationParams& P)
    {
        P.SurfaceRoughness          = 0.0f;   // le seul non nul par défaut (5.0)
        P.DomainWarpStrength        = 0.0f;
        P.TerraceStepHeight         = 0.0f;
        P.TerraceNoiseDisplacement  = 0.0f;
        P.LayerLineSpacing          = 0.0f;
        P.RibbingSpacing            = 0.0f;
        P.OverhangStrength          = 0.0f;
        P.CliffStrength             = 0.0f;
        P.ScallopStrength           = 0.0f;
        P.ArchDensity               = 0.0f;
        P.ColumnDensity             = 0.0f;   // ⚠️ celui-ci se cuit dans SDFCache.Columns, pas un `if`
        P.DomeDensity               = 0.0f;
        P.PinchDensity              = 0.0f;
        P.FloorBias                 = 0.0f;
    }

    /**
     * Pits et cheminées sont à 0 par défaut — or ce sont précisément les deux boucles que `§2`
     * annonçait comme « le plus retors de toute la décomposition » (coordonnées NON warpées
     * mélangées au SDF warpé). Les laisser au repos testerait tout sauf le morceau difficile.
     *
     * ⚠️ ET ON DENSIFIE LE RÉSEAU, corrigé après le premier run vert. Aux défauts
     * (`RoomSpacing = 80`, `RoomDensity = 0.35`) le premier passage a rendu **65 échantillons en
     * grotte sur 6000, soit 1,1 %** : bit-identique, oui, mais en comparant surtout du roc plein à
     * du roc plein, là où le carve, les pits, les cheminées et les vers ne s'exécutent même pas.
     * Le compteur avait été écrit exactement pour dire ça, et il l'a dit ; l'avertissement, lui, ne
     * se déclenchait qu'à ZÉRO — trop tard pour être utile. Les deux sont corrigés ici.
     *
     * Densified after the first green run: 65 of 6000 samples in open cave (1.1%) means the port was
     * mostly compared solid-rock-to-solid-rock. The counter said so; the warning threshold (only at
     * zero) did not. Both fixed.
     */
    void EnableTunnelFeatures(FStrateGenerationParams& P)
    {
        P.RoomSpacing     = 42.0f;   // 80 → 42 : des salles à portée de chaque chunk échantillonné
        P.RoomDensity     = 0.85f;   // 0.35 → 0.85
        P.PitDensity      = 0.55f;
        P.ChimneyDensity  = 0.55f;
        P.VerticalScale   = 1.35f;   // ≠ 1 ⇒ le Z « effectif » diverge du Z monde partout
    }

    /** Fraction minimale d'échantillons devant tomber en grotte ouverte pour que l'équivalence
     *  signifie quelque chose. 10 % est modeste et très au-dessus du 1,1 % observé. */
    constexpr float MinCaveFraction = 0.10f;
}

bool FVoxelForgeOpStackTunnelTest::RunTest(const FString& Parameters)
{
    using namespace VoxelForgeTest;

    FTestWorld World;
    World.Build();
    if (!World.IsValid())
    {
        AddError(World.WhyInvalid());
        return false;
    }

    const UVoxelGenerator* Gen = World.Generator.Get();

    int32 TopVoxelZ = 0, BottomVoxelZ = 0;
    if (!World.GetSlotVoxelZRange(FTestWorld::SlotTunnelNetwork, TopVoxelZ, BottomVoxelZ))
    {
        AddError(TEXT("The fixture layout has no TunnelNetwork slot. Check FTestWorld::Build's ")
                 TEXT("Archetypes[] against FTestWorld::SlotTunnelNetwork."));
        return false;
    }

    const int32 MidChunkZ = ((TopVoxelZ + BottomVoxelZ) / 2) / CHUNK_SIZE;
    FStrateGenerationParams P = World.StrateManager->GetGenerationParams(FIntVector(0, 0, MidChunkZ));

    if (P.StrateTopWorldZ - P.StrateBottomWorldZ <= 0.0f)
    {
        AddError(TEXT("The TunnelNetwork strate has degenerate Z bounds."));
        return false;
    }

    DisableStageBModifiers(P);
    EnableTunnelFeatures(P);

    FVoxelOpStack Stack;
    VoxelDensityOps::BuildTunnelNetworkStack(Stack, P, World.Settings->Seed,
                                             Gen->OriginSpineRadius, World.StrateManager.Get());

    // rock + roomgraph + carve + worms + 3 structurels = 7. (Le premier run a dit 7 contre un 6
    // attendu : faute d'arithmétique dans l'attente, pas dans la pile — 4 + 3, comme les îles.)
    // Les 13 modificateurs de détail viendront s'insérer entre le carve et les vers, donc ce nombre
    // DOIT bouger à l'étape B.
    TestEqual(TEXT("the stage-A tunnel stack is decomposed into 7 ops"), Stack.Num(), 7);

    FVoxelOpContext Ctx;
    Ctx.Seed               = (uint32)World.Settings->Seed;
    Ctx.LayoutVersion      = World.StrateManager->GetLayoutVersion();
    Ctx.StrateTopWorldZ    = P.StrateTopWorldZ;
    Ctx.StrateBottomWorldZ = P.StrateBottomWorldZ;
    Stack.PrepareChunk(Ctx);

    // Grappes : N chunks, M points dans chacun. Voir l'en-tête — un tirage uniforme ferait
    // reconstruire le cache SDF à presque chaque point, sur les DEUX chemins.
    TArray<FVector> Points;
    Points.Reserve(NumTunnelSamples);
    {
        FRandomStream Rng(1080601);
        const int32 ChunkZ0 = BottomVoxelZ / CHUNK_SIZE;
        const int32 ChunkZ1 = FMath::Max(ChunkZ0, (TopVoxelZ / CHUNK_SIZE) - 1);
        for (int32 c = 0; c < NumTunnelChunks; ++c)
        {
            const int32 CX = Rng.RandRange(-3, 3);
            const int32 CY = Rng.RandRange(-3, 3);
            const int32 CZ = Rng.RandRange(ChunkZ0, ChunkZ1);
            for (int32 i = 0; i < PointsPerChunk; ++i)
            {
                Points.Add(FVector(
                    (float)(CX * CHUNK_SIZE + Rng.RandRange(0, CHUNK_SIZE - 1)),
                    (float)(CY * CHUNK_SIZE + Rng.RandRange(0, CHUNK_SIZE - 1)),
                    (float)FMath::Clamp(CZ * CHUNK_SIZE + Rng.RandRange(0, CHUNK_SIZE - 1),
                                        BottomVoxelZ, TopVoxelZ)));
            }
        }
    }

    //=========================================================================
    // 1. ÉQUIVALENCE
    //=========================================================================
    const float InnerBot = P.StrateBottomWorldZ + P.BoundarySealThickness;
    const float InnerTop = P.StrateTopWorldZ    - P.BoundarySealThickness;

    int32 NumDiff = 0, NumSideDisagree = 0, WorstIdx = -1;
    int32 NumInCave = 0, NumInRock = 0;
    float WorstDelta = 0.0f;

    for (int32 i = 0; i < NumTunnelSamples; ++i)
    {
        const float X = (float)Points[i].X, Y = (float)Points[i].Y, Z = (float)Points[i].Z;

        const float Old = Gen->GetDensityWithParams(X, Y, Z, P);
        const float New = Stack.EvalMC(X, Y, Z);

        const bool bInterior = (Z > InnerBot && Z < InnerTop);
        if (bInterior && Old >= 0.0f) { ++NumInCave; }   // air loin des seals ⇒ salle/tunnel/ver
        if (bInterior && Old <  0.0f) { ++NumInRock; }

        if (!BitEqual(Old, New))
        {
            ++NumDiff;
            const float D = FMath::Abs(Old - New);
            if (D > WorstDelta) { WorstDelta = D; WorstIdx = i; }
        }
        if ((Old >= 0.0f) != (New >= 0.0f)) { ++NumSideDisagree; }
    }

    if (NumDiff == 0)
    {
        AddInfo(FString::Printf(
            TEXT("TunnelNetwork STAGE A: bit-identical across %d samples in %d chunks (%d in open ")
            TEXT("cave, %d in rock, away from the seal bands). Exercised: vertical scale (1.35, so ")
            TEXT("effective Z differs from world Z everywhere), cave warp, the room/tunnel SDF via ")
            TEXT("the SHARED BuildChunkCache, pits and chimneys at UNWARPED coords, the carve with ")
            TEXT("its floored divisor, and the worm carve with its network mask. NOT covered: the ")
            TEXT("13 detail modifiers, the per-room op override, and any tile verdict."),
            NumTunnelSamples, NumTunnelChunks, NumInCave, NumInRock));

        AddInfo(FString::Printf(
            TEXT("Cave coverage: %.1f%% of samples are in open cave (floor %.0f%%). This is the ")
            TEXT("number that says whether the equivalence MEANS anything -- the carve, the pits, ")
            TEXT("the chimneys and the worm carve only execute near the network, so a run dominated ")
            TEXT("by deep rock would be green while proving almost nothing."),
            100.0f * (float)NumInCave / (float)NumTunnelSamples, 100.0f * MinCaveFraction));
    }
    else
    {
        AddError(FString::Printf(
            TEXT("TunnelNetwork STAGE A: %d of %d samples differ (largest |delta| %.9g at ")
            TEXT("(%.0f, %.0f, %.0f)); %d cross the isosurface. Check, in order: the carve's ")
            TEXT("MinDivisor (TunnelNetwork floors Blend*2 at 1.0 and the other archetypes do NOT ")
            TEXT("-- getting this wrong only shows up when SDFBlendRadius*2 < 1), then EffectiveZ ")
            TEXT("(VerticalScale must divide BEFORE the warp and the worms, and must NOT touch pit ")
            TEXT("or chimney Z), then the pit/chimney loops reading UNWARPED coords while the room ")
            TEXT("SDF reads warped ones, then the SDF cache key (it now includes a params ")
            TEXT("fingerprint the original lacks -- that can cost a rebuild, never a wrong room), ")
            TEXT("then the worm early-out on N1 >= threshold."),
            NumDiff, NumTunnelSamples, WorstDelta,
            WorstIdx >= 0 ? Points[WorstIdx].X : 0.0f,
            WorstIdx >= 0 ? Points[WorstIdx].Y : 0.0f,
            WorstIdx >= 0 ? Points[WorstIdx].Z : 0.0f,
            NumSideDisagree));
    }

    TestEqual(TEXT("no sample lands on the opposite side of the isosurface"), NumSideDisagree, 0);

    // ⚠️ SEUIL EN FRACTION, PAS « > 0 ». La version « == 0 » de ce garde-fou a laissé passer un run
    // à 1,1 % en silence. Un test qui ne se plaint qu'au zéro absolu ne mesure pas la couverture,
    // il constate seulement qu'elle n'est pas vide.
    if ((float)NumInCave < MinCaveFraction * (float)NumTunnelSamples)
    {
        AddWarning(FString::Printf(
            TEXT("Only %.1f%% of samples landed in open cave (want >= %.0f%%): the carve, the pits, ")
            TEXT("the chimneys and the worm carve were barely exercised, so the equivalence above ")
            TEXT("mostly compares solid rock to solid rock. Lower RoomSpacing or raise RoomDensity ")
            TEXT("in EnableTunnelFeatures."),
            100.0f * (float)NumInCave / (float)NumTunnelSamples, 100.0f * MinCaveFraction));
    }

    //=========================================================================
    // 2. INVARIANCE DE FENÊTRE — le test qui compte le plus sur cet archétype
    //=========================================================================
    // `BuildChunkCache` porte la discipline à deux régions de `ARCHITECTURE §8.4` : c'est LE endroit
    // du plugin où un cache mal clé produit une couture visible entre deux tuiles. La pile ajoute sa
    // propre clé par-dessus (boîte + strate + seed + empreinte de params + version de layout), donc
    // c'est cette clé-là que ce bloc met à l'épreuve : mêmes points, ordre mélangé, N threads.
    {
        std::atomic<int32> Impure{ 0 };
        const int32 NumBlocks = FMath::Max(4, FMath::Min(16, FPlatformMisc::NumberOfCores()));

        TArray<float> Ref;
        Ref.SetNumUninitialized(NumTunnelSamples);
        for (int32 i = 0; i < NumTunnelSamples; ++i)
        {
            Ref[i] = Stack.EvalMC((float)Points[i].X, (float)Points[i].Y, (float)Points[i].Z);
        }

        ParallelFor(NumBlocks, [&](int32 Block)
        {
            TArray<int32> LocalOrder;
            BuildShuffledOrder(NumTunnelSamples, 4400 + Block, LocalOrder);
            for (const int32 i : LocalOrder)
            {
                const float V = Stack.EvalMC((float)Points[i].X, (float)Points[i].Y, (float)Points[i].Z);
                if (!BitEqual(V, Ref[i])) { Impure.fetch_add(1, std::memory_order_relaxed); }
            }
        });

        TestEqual(TEXT("the tunnel stack is window-invariant across order and threads"),
                  Impure.load(), 0);
    }

    //=========================================================================
    // 3. LE CACHE NE PEUT PAS SERVIR LES PARAMS DU VOISIN
    //=========================================================================
    // La régression d'overhang du 2026-07-27 : deux piles dans la MÊME strate, au MÊME seed, ne
    // différant QUE par des params, partageaient un cache `thread_local` dont la clé ignorait les
    // params — et la seconde lisait les salles de la première. La pile clé donc aussi sur une
    // empreinte CRC des params. Ce bloc le vérifie en ALTERNANT A, B, A, B au même point, le motif
    // qui fait mentir une clé incomplète.
    //
    // ⚠️⚠️ ON NE COMPARE **PAS** À L'ORIGINAL ICI, ET C'EST LE POINT LE PLUS IMPORTANT DE CE TEST.
    // `GetDensityWithParams` clé son cache sur (boîte XY, strate, seed) — **sans les params**. En
    // alternance il rendrait donc, pour B, les salles de A : l'original ÉCHOUERAIT ce contrôle. Le
    // comparer à lui ici ne mesurerait pas mon opérateur, ça mesurerait son bug. On compare donc
    // chaque pile à ELLE-MÊME évaluée seule — un oracle qui ne partage pas le défaut testé.
    //
    // ⚠️ ET CE N'EST PEUT-ÊTRE PAS QU'UN ARTEFACT DE TEST — à vérifier, pas à croire. En production
    // `GetGenerationParams` MÉLANGE les params entre strates voisines (transitions Gradient), donc
    // deux chunks de Z différents dans la même strate peuvent avoir des params différents, avec la
    // même boîte XY, le même index de strate et le même seed ⇒ aucune reconstruction. Si c'est
    // exact, un worker qui descend une bande de transition sert les salles du chunk précédent.
    // Noté dans `AUDIT §C2` comme SUSPECTÉ, avec le test qui le confirmerait — pas comme prouvé.
    //
    // We compare each stack to ITSELF evaluated alone, not to the original: the original keys its
    // SDF cache without the params and would fail this check, so comparing against it would measure
    // its bug rather than this operator.
    {
        FStrateGenerationParams P2 = P;
        P2.RoomSpacing = P.RoomSpacing * 0.6f;    // une autre disposition de salles
        P2.RoomDensity = FMath::Min(P.RoomDensity * 1.7f, 1.0f);

        FVoxelOpStack Stack2;
        VoxelDensityOps::BuildTunnelNetworkStack(Stack2, P2, World.Settings->Seed,
                                                 Gen->OriginSpineRadius, World.StrateManager.Get());
        Stack2.PrepareChunk(Ctx);

        // Chaque pile compte 2 reconstructions de cache par point en alternance (elles partagent le
        // `thread_local`), donc on reste modeste sur le nombre de sondes : `BuildChunkCache` est la
        // fonction la plus chère du plugin.
        const int32 Probe = FMath::Min(400, NumTunnelSamples);

        TArray<float> SoloA, SoloB;
        SoloA.SetNumUninitialized(Probe);
        SoloB.SetNumUninitialized(Probe);
        for (int32 i = 0; i < Probe; ++i)
        {
            SoloA[i] = Stack.EvalMC((float)Points[i].X, (float)Points[i].Y, (float)Points[i].Z);
        }
        for (int32 i = 0; i < Probe; ++i)
        {
            SoloB[i] = Stack2.EvalMC((float)Points[i].X, (float)Points[i].Y, (float)Points[i].Z);
        }

        int32 NumWrong = 0, NumActuallyDifferent = 0;
        for (int32 i = 0; i < Probe; ++i)
        {
            const float X = (float)Points[i].X, Y = (float)Points[i].Y, Z = (float)Points[i].Z;
            const float GotA = Stack.EvalMC(X, Y, Z);
            const float GotB = Stack2.EvalMC(X, Y, Z);

            if (!BitEqual(GotA, SoloA[i]) || !BitEqual(GotB, SoloB[i])) { ++NumWrong; }
            if (!BitEqual(SoloA[i], SoloB[i])) { ++NumActuallyDifferent; }
        }

        TestEqual(TEXT("two tunnel stacks with different params never serve each other's rooms"),
                  NumWrong, 0);

        AddInfo(FString::Printf(
            TEXT("Params-fingerprint check: %d of %d probe points (%.1f%%) genuinely differ between ")
            TEXT("the two param sets, and %d were served wrong under A/B interleaving. The FIRST ")
            TEXT("number is the check's real strength: only those points could ever reveal a stale ")
            TEXT("cache, so it is a count of how many times the question was actually asked."),
            NumActuallyDifferent, Probe, 100.0f * (float)NumActuallyDifferent / (float)Probe,
            NumWrong));

        // Le premier run a donné 3 sur 400 (0,75 %) : 397 sondes ne pouvaient RIEN distinguer. Même
        // correction que la couverture de grotte — un seuil en fraction, pas « non nul ».
        if ((float)NumActuallyDifferent < 0.05f * (float)Probe)
        {
            AddWarning(FString::Printf(
                TEXT("Only %d of %d probe points differ between the two param sets (want >= 5%%), so ")
                TEXT("this check asked its question %d times, not %d. A stale cache would go ")
                TEXT("unnoticed at every other point. Make P2 differ more, or probe nearer the ")
                TEXT("network."),
                NumActuallyDifferent, Probe, NumActuallyDifferent, Probe));
        }
    }

    //=========================================================================
    // 3b. LES PITS ET LES CHEMINÉES ONT-ILS RÉELLEMENT CONTRIBUÉ ?
    //=========================================================================
    // `§2` désigne ces deux boucles comme « le plus retors de toute la décomposition » : elles
    // écrivent le MÊME canal SDF que le graphe de salles mais à des coordonnées NON warpées. Les
    // activer dans les params ne prouve pas qu'elles ont changé quoi que ce soit — un test peut très
    // bien être vert avec `SDFCache.Pits` vide.
    //
    // On le MESURE : la même pile avec `PitDensity = ChimneyDensity = 0`, et on compte les points où
    // la densité bouge. Zéro ⇒ les deux boucles n'ont rien fait et le morceau difficile n'est pas
    // couvert, quelle que soit la couleur du test.
    //
    // Enabling a feature in the params is not evidence it fired. This measures it.
    {
        FStrateGenerationParams PNoShafts = P;
        PNoShafts.PitDensity     = 0.0f;
        PNoShafts.ChimneyDensity = 0.0f;

        FVoxelOpStack StackNoShafts;
        VoxelDensityOps::BuildTunnelNetworkStack(StackNoShafts, PNoShafts, World.Settings->Seed,
                                                 Gen->OriginSpineRadius, World.StrateManager.Get());
        StackNoShafts.PrepareChunk(Ctx);

        const int32 Probe = FMath::Min(1500, NumTunnelSamples);

        // Deux passes SOLO (pas d'alternance) : on ne teste pas une clé de cache ici, seulement une
        // contribution — et alterner ferait reconstruire le cache SDF à chaque point pour rien.
        TArray<float> WithShafts;
        WithShafts.SetNumUninitialized(Probe);
        for (int32 i = 0; i < Probe; ++i)
        {
            WithShafts[i] = Stack.EvalMC((float)Points[i].X, (float)Points[i].Y, (float)Points[i].Z);
        }

        int32 NumMoved = 0;
        for (int32 i = 0; i < Probe; ++i)
        {
            const float V = StackNoShafts.EvalMC((float)Points[i].X, (float)Points[i].Y,
                                                 (float)Points[i].Z);
            if (!BitEqual(V, WithShafts[i])) { ++NumMoved; }
        }

        AddInfo(FString::Printf(
            TEXT("Pit/chimney contribution: %d of %d probe points move when PitDensity and ")
            TEXT("ChimneyDensity are zeroed. These are the two loops OPSTACK-DECOMPOSITION 2 calls ")
            TEXT("the fiddliest thing in the decomposition (unwarped coords SmoothMin'd into the ")
            TEXT("warped room SDF), so this number is the difference between covering them and ")
            TEXT("merely having switched them on."),
            NumMoved, Probe));

        if (NumMoved == 0)
        {
            AddWarning(TEXT("Zeroing PitDensity and ChimneyDensity changed nothing, so those two ")
                       TEXT("loops never contributed a single voxel and the equivalence says ")
                       TEXT("NOTHING about them. Most likely BuildChunkCache baked no pits at this ")
                       TEXT("RoomSpacing/strate height -- check SDFCache.Pits, not the params."));
        }
    }

    //=========================================================================
    // 4. LE VERDICT DE BOÎTE — attendu NUL, et c'est le point
    //=========================================================================
    {
        int32 NumProved = 0, NumMixed = 0;
        FRandomStream Rng(97531);
        for (int32 t = 0; t < 40; ++t)
        {
            const int32 Step = 1, Cells = 8;
            const int32 Extent = Step * Cells;
            const FIntVector Origin(
                Rng.RandRange(-4, 4) * Extent,
                Rng.RandRange(-4, 4) * Extent,
                FMath::Clamp(Rng.RandRange(BottomVoxelZ / Extent, TopVoxelZ / Extent), -4096, 4096) * Extent);
            const int32 GridDim = Cells + 1;
            const FBox Box(
                FVector(Origin.X - Step, Origin.Y - Step, Origin.Z - Step),
                FVector(Origin.X + GridDim * Step, Origin.Y + GridDim * Step, Origin.Z + GridDim * Step));

            if (Stack.ClassifyBox(Box, Ctx) == EVoxelTileClass::Mixed) { ++NumMixed; }
            else { ++NumProved; }
        }

        AddInfo(FString::Printf(
            TEXT("Box verdicts over 40 TunnelNetwork tiles: %d proved, %d Mixed. %d proved is the ")
            TEXT("EXPECTED result at stage A and not a defect: the room source answers Both (its ")
            TEXT("bounds live in the SDF cache, which it would have to build for the queried box), ")
            TEXT("and the worm source answers CarveOnly EVERYWHERE because a fielded noise carve ")
            TEXT("has no spatial bound at all. Recovering these needs the numeric amplitude cap in ")
            TEXT("OPSTACK-DECOMPOSITION 0.2 -- the largest single perf item in the whole plan, and ")
            TEXT("the reason this archetype currently skips zero tiles."),
            NumProved, NumMixed, NumProved));

        TestEqual(TEXT("stage A emits no unsound verdict (it emits none at all)"), NumProved, 0);
    }

    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
