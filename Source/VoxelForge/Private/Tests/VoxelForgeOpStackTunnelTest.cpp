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

    /** Pits et cheminées sont à 0 par défaut — or ce sont précisément les deux boucles que `§2`
     *  annonçait comme « le plus retors de toute la décomposition » (coordonnées NON warpées
     *  mélangées au SDF warpé). Les laisser au repos testerait tout sauf le morceau difficile. */
    void EnableTunnelFeatures(FStrateGenerationParams& P)
    {
        P.PitDensity      = 0.55f;
        P.ChimneyDensity  = 0.55f;
        P.VerticalScale   = 1.35f;   // ≠ 1 ⇒ le Z « effectif » diverge du Z monde partout
    }
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

    // rock + roomgraph + carve + worms + 3 structurels. Les 13 modificateurs de détail viendront
    // s'insérer entre le carve et les vers — ce nombre DOIT bouger à l'étape B.
    TestEqual(TEXT("the stage-A tunnel stack is decomposed into 6 ops"), Stack.Num(), 6);

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

    if (NumInCave == 0)
    {
        AddWarning(TEXT("No sample landed in open cave away from the seal bands: the room graph, ")
                   TEXT("the carve, the pits and the worms were never meaningfully exercised, so ")
                   TEXT("the equivalence above mostly compares solid rock to solid rock. Raise ")
                   TEXT("RoomDensity or lower RoomSpacing."));
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
            TEXT("Params-fingerprint check: %d of %d probe points genuinely differ between the two ")
            TEXT("param sets, and %d were served wrong under A/B interleaving. A zero in the FIRST ")
            TEXT("number would mean the check proved nothing -- the two param sets must actually ")
            TEXT("produce different rock for a stale cache to be detectable."),
            NumActuallyDifferent, Probe, NumWrong));

        if (NumActuallyDifferent == 0)
        {
            AddWarning(TEXT("The two param sets produced identical density at every probe point, so ")
                       TEXT("this check cannot distinguish a correct cache from a stale one. Make ")
                       TEXT("P2 differ more."));
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
