// VoxelForgeOpStackMazeTest.cpp
// PHASE 1, LE TEST QUI COMPTE — la pile d'opérateurs Maze contre GetMazeDensity.
// PHASE 1'S LOAD-BEARING TEST — the Maze operator stack against GetMazeDensity.
//
// CE QUE LA PHASE 1 DEVAIT PROUVER / WHAT PHASE 1 HAD TO PROVE
// Le déclencheur d'arrêt de `OPSTACK-PLAN §4` : **« est-ce que la séparation source / modifier tombe
// naturellement du code existant ? »** Réponse mesurée : oui. Maze se décompose en sept opérateurs
// sans contorsion, le SDF est reproduit BIT POUR BIT, et aucun échantillon ne change de côté de
// l'isosurface.
//
// ─────────────────────────────────────────────────────────────────────────────────────────
// ⚠️ LE PLANCHER ULP — lire ceci avant de « corriger » un écart résiduel
// ─────────────────────────────────────────────────────────────────────────────────────────
// La pile reproduit `GetMazeDensity` à ~1-2 ULP près sur ~2 % des échantillons (ceux qui tombent
// dans la coquille de blend du SDF, où `Blend - Sdf` annule catastrophiquement et amplifie le
// dernier arrondi). **Zéro échantillon ne traverse l'isosurface**, donc pas un triangle ne bouge.
//
// L'origine exacte de ce dernier arrondi n'a PAS été identifiée, après six cycles de build et cinq
// hypothèses toutes réfutées par la mesure (aller-retour FVector · fenêtre de rugosité · `/fp:fast`
// entre unités de compilation · contexte d'inlining · constante de compilation vs donnée
// d'exécution). Ce qui EST établi par la mesure :
//
//   • le SDF est bit-identique sur 126/126 des écarts — le treillis, les hashs, l'ensemble d'arêtes
//     et `VoxelSDF::Capsule` sont donc exacts ;
//   • l'écart naît entièrement dans la conversion SDF→densité, au dernier arrondi ;
//   • il est DÉTERMINISTE (mêmes échantillons, même delta, même coordonnée à chaque run) ;
//   • il ne dépend ni de l'unité de compilation, ni de l'inlining, ni du modèle flottant.
//
// **Décision (Jahni, 2026-07-27) : on l'accepte et on avance.** Aucune décision du projet ne dépend
// de la réponse, et la chasse coûtait plus que l'information. Consigné comme point ouvert dans
// `AUDIT-2026-07.md §C10`.
//
// ⚠️ LA RÈGLE QUI EN DÉCOULE, ELLE, EST IMPORTANTE :
// **ne jamais faire tourner les deux chemins (switch d'archétype et pile d'opérateurs) dans le même
// monde, et ne jamais comparer leurs sorties pour égalité.** Ce n'est PAS un risque de désync entre
// clients — dans un même binaire le champ est prouvé pur (`VoxelForge.Determinism.DensityPurity`,
// bit-identique entre threads et ordres de requête) et tous les pairs exécutent le même chemin. Mais
// une strate à moitié migrée produirait une couture. Le vrai sujet multijoueur est ailleurs :
// `AUDIT §C9` (le défaut FP d'UBT diffère selon la toolchain).
//
// Never run both paths in one world and never compare their outputs for equality. This is NOT a
// client-desync risk — within one binary the field is proven pure and every peer runs the same path —
// but a half-migrated strate would produce a seam.
//
// ─────────────────────────────────────────────────────────────────────────────────────────
// LA BARRE D'ACCEPTATION, ENCODÉE CI-DESSOUS / THE ACCEPTANCE BAR, ENCODED BELOW
// ─────────────────────────────────────────────────────────────────────────────────────────
//   • ÉCHEC DUR : un seul échantillon qui change de côté de l'isosurface (la géométrie bouge).
//   • INFO      : des écarts à l'échelle de l'ULP (le plancher, attendu).
//   • WARN      : un écart plus grand — ÇA, c'est une vraie dérive de portage, et il faut chercher.
// Un test qui avertit à chaque portage serait ignoré par le portage qui compte.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Async/ParallelFor.h"
#include "HAL/PlatformMisc.h"

#include "VoxelForgeTestFixture.h"
#include "VoxelDensityOpStack.h"

#include <atomic>

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FVoxelForgeOpStackMazeTest,
    "VoxelForge.OpStack.MazeEquivalence",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

namespace
{
    constexpr int32 NumMazeSamples = 20000;

    /** Les params Maze de la strate Maze de la fixture, bornes Z de runtime comprises. */
    bool ResolveMazeParams(const VoxelForgeTest::FTestWorld& World, FMazeGenerationParams& Out,
                           int32& OutTopVoxelZ, int32& OutBottomVoxelZ)
    {
        using namespace VoxelForgeTest;
        if (!World.GetSlotVoxelZRange(FTestWorld::SlotMaze, OutTopVoxelZ, OutBottomVoxelZ)) { return false; }
        const int32 MidChunkZ = ((OutTopVoxelZ + OutBottomVoxelZ) / 2) / CHUNK_SIZE;
        Out = World.StrateManager->GetMazeParamsForChunk(FIntVector(0, 0, MidChunkZ));
        return true;
    }
}

bool FVoxelForgeOpStackMazeTest::RunTest(const FString& Parameters)
{
    using namespace VoxelForgeTest;

    FTestWorld World;
    World.Build();
    if (!World.IsValid())
    {
        AddError(World.WhyInvalid());
        return false;
    }

    FMazeGenerationParams MazeParams;
    int32 TopVoxelZ = 0, BottomVoxelZ = 0;
    if (!ResolveMazeParams(World, MazeParams, TopVoxelZ, BottomVoxelZ))
    {
        AddError(TEXT("The fixture layout has no Maze slot. Check FTestWorld::Build's Archetypes[] ")
                 TEXT("against FTestWorld::SlotMaze."));
        return false;
    }

    // GetMazeDensity court-circuite sur une strate dégénérée (`return 1.0f`). Cette garde appartient
    // à la fonction d'archétype, pas à un opérateur ; la pile suppose une strate valide.
    if (MazeParams.StrateTopWorldZ - MazeParams.StrateBottomWorldZ <= 0.0f)
    {
        AddError(FString::Printf(
            TEXT("The Maze strate has degenerate Z bounds (top %.1f, bottom %.1f), which sends ")
            TEXT("GetMazeDensity down its early-out. The op stack has no such early-out by design."),
            MazeParams.StrateTopWorldZ, MazeParams.StrateBottomWorldZ));
        return false;
    }

    const UVoxelGenerator* Gen = World.Generator.Get();

    FVoxelOpStack Stack;
    VoxelDensityOps::BuildMazeStack(Stack, MazeParams, World.Settings->Seed,
                                    Gen->OriginSpineRadius, World.StrateManager.Get());

    // La décomposition doit être une DÉCOMPOSITION. Un `FMazeOp` monolithique passerait tous les
    // tests numériques ci-dessous et aurait pourtant raté l'objet entier du refactor (§2.5).
    TestEqual(TEXT("the Maze stack is decomposed, not wrapped (rock + corridors + roughness + carve + 3 structural)"),
              Stack.Num(), 7);

    FVoxelOpContext Ctx;
    Ctx.Seed                = (uint32)World.Settings->Seed;
    Ctx.LayoutVersion       = World.StrateManager->GetLayoutVersion();
    Ctx.StrateTopWorldZ     = MazeParams.StrateTopWorldZ;
    Ctx.StrateBottomWorldZ  = MazeParams.StrateBottomWorldZ;
    Stack.PrepareChunk(Ctx);

    TArray<FVector> Points;
    Points.Reserve(NumMazeSamples);
    {
        FRandomStream Rng(31337);
        for (int32 i = 0; i < NumMazeSamples; ++i)
        {
            Points.Add(FVector(
                (float)Rng.RandRange(-3 * CHUNK_SIZE, 3 * CHUNK_SIZE),
                (float)Rng.RandRange(-3 * CHUNK_SIZE, 3 * CHUNK_SIZE),
                (float)Rng.RandRange(BottomVoxelZ, TopVoxelZ)));
        }
    }

    //=========================================================================
    // ÉQUIVALENCE — géométrie d'abord, bits ensuite.
    //=========================================================================
    int32 NumDiff = 0, WorstIdx = -1, NumBeyondUlpNoise = 0, NumSolidDisagreements = 0;
    float WorstDelta = 0.0f;
    for (int32 i = 0; i < NumMazeSamples; ++i)
    {
        const float X = (float)Points[i].X, Y = (float)Points[i].Y, Z = (float)Points[i].Z;

        const float Old = Gen->GetMazeDensity(X, Y, Z, MazeParams);   // MC : négatif = solide
        const float New = Stack.EvalMC(X, Y, Z);

        if (!BitEqual(Old, New))
        {
            ++NumDiff;
            const float Delta = FMath::Abs(Old - New);
            if (Delta > WorstDelta) { WorstDelta = Delta; WorstIdx = i; }

            // `Blend - Sdf` annule catastrophiquement au bord de la coquille de blend, donc un
            // écart d'ULP sur le SDF ressort amplifié sur la densité : marge généreuse, mais bornée.
            const float UlpNoise = 16.0f * FMath::Max(FMath::Abs(Old), 1.0f) * FLT_EPSILON;
            if (Delta > UlpNoise) { ++NumBeyondUlpNoise; }
        }
        // Le mesher ne lit que le SIGNE (D >= IsoLevel ⇒ air). Un désaccord de CÔTÉ bouge la géométrie.
        if ((Old >= 0.0f) != (New >= 0.0f)) { ++NumSolidDisagreements; }
    }

    if (NumDiff == 0)
    {
        AddInfo(FString::Printf(TEXT("Bit-identical across %d samples."), NumMazeSamples));
    }
    else if (NumBeyondUlpNoise == 0)
    {
        AddInfo(FString::Printf(
            TEXT("%d of %d samples differ, ALL at ULP scale (largest |delta| %.9g at (%.0f, %.0f, ")
            TEXT("%.0f)), and 0 cross the isosurface -- not one triangle would move. This is the ")
            TEXT("accepted floor; see the header comment and AUDIT-2026-07.md C10. The SDF itself is ")
            TEXT("reproduced BIT FOR BIT, so the lattice, the hashes and VoxelSDF::Capsule are exact; ")
            TEXT("only the final SDF->density rounding differs. Do not go hunting this again without ")
            TEXT("reading C10 first -- five hypotheses have already been measured and refuted."),
            NumDiff, NumMazeSamples, WorstDelta,
            WorstIdx >= 0 ? Points[WorstIdx].X : 0.0f,
            WorstIdx >= 0 ? Points[WorstIdx].Y : 0.0f,
            WorstIdx >= 0 ? Points[WorstIdx].Z : 0.0f));
    }
    else
    {
        AddWarning(FString::Printf(
            TEXT("%d of %d samples differ and %d are TOO LARGE to be the accepted ULP floor (largest ")
            TEXT("|delta| %.9g at (%.0f, %.0f, %.0f)); %d cross the isosurface. THIS one is real port ")
            TEXT("drift, not the known floor. Check, in order: the roughness apply-window ")
            TEXT("(R + SurfaceRoughness + 2), the carve blend (2.0), the noise frequency (0.12) and ")
            TEXT("octave count (3), and the order of the structural post ops."),
            NumDiff, NumMazeSamples, NumBeyondUlpNoise, WorstDelta,
            WorstIdx >= 0 ? Points[WorstIdx].X : 0.0f,
            WorstIdx >= 0 ? Points[WorstIdx].Y : 0.0f,
            WorstIdx >= 0 ? Points[WorstIdx].Z : 0.0f,
            NumSolidDisagreements));
    }

    // Le SEUL échec dur : un désaccord de côté d'iso EST une différence de géométrie.
    TestEqual(TEXT("no sample lands on the opposite side of the isosurface from the original"),
              NumSolidDisagreements, 0);

    //=========================================================================
    // INVARIANCE DE FENÊTRE — la pile doit tenir les mêmes règles que le générateur.
    //=========================================================================
    // Le cache par cellule de la source de couloirs est `thread_local` : c'est exactement le genre
    // d'endroit où une clé incomplète produit une couture (cf. AUDIT C2).
    {
        std::atomic<int32> Impure{ 0 };
        const int32 NumBlocks = FMath::Max(4, FMath::Min(16, FPlatformMisc::NumberOfCores()));

        TArray<float> Ref;
        Ref.SetNumUninitialized(NumMazeSamples);
        for (int32 i = 0; i < NumMazeSamples; ++i)
        {
            Ref[i] = Stack.EvalMC((float)Points[i].X, (float)Points[i].Y, (float)Points[i].Z);
        }

        ParallelFor(NumBlocks, [&](int32 Block)
        {
            TArray<int32> LocalOrder;
            BuildShuffledOrder(NumMazeSamples, 500 + Block, LocalOrder);
            for (const int32 i : LocalOrder)
            {
                const float V = Stack.EvalMC((float)Points[i].X, (float)Points[i].Y, (float)Points[i].Z);
                if (!BitEqual(V, Ref[i])) { Impure.fetch_add(1, std::memory_order_relaxed); }
            }
        });

        TestEqual(TEXT("the op stack is window-invariant across query order and worker threads"),
                  Impure.load(), 0);
    }

    //=========================================================================
    // LE VERDICT DE BOÎTE — le vrai prix perf : Maze n'a JAMAIS su sauter une tuile.
    //=========================================================================
    // ClassifyTile renvoie Mixed pour tout archétype de grotte, donc TunnelNetwork, Maze,
    // VerticalShafts, FloatingIslands, FlatPlain, CrystalChamber et Underwater ne captent RIEN du
    // gain T1.d. Tout nombre > 0 ici est du saut de tuile que Maze n'a jamais eu.
    {
        int32 NumProved = 0, NumMixed = 0, NumUnsound = 0;
        FRandomStream Rng(24680);

        for (int32 t = 0; t < 60; ++t)
        {
            const int32 Step = 1, Cells = 8;                  // petites tuiles : force brute tenable
            const int32 Extent = Step * Cells;
            const FIntVector Origin(
                Rng.RandRange(-6, 6) * Extent,
                Rng.RandRange(-6, 6) * Extent,
                FMath::Clamp(Rng.RandRange(BottomVoxelZ / Extent, TopVoxelZ / Extent), -4096, 4096) * Extent);

            const int32 GridDim = Cells + 1;   // le MÊME treillis que le mesher, marge ±1 comprise
            const FBox Box(
                FVector(Origin.X - Step, Origin.Y - Step, Origin.Z - Step),
                FVector(Origin.X + GridDim * Step, Origin.Y + GridDim * Step, Origin.Z + GridDim * Step));

            const EVoxelTileClass Verdict = Stack.ClassifyBox(Box, Ctx);
            if (Verdict == EVoxelTileClass::Mixed) { ++NumMixed; continue; }
            ++NumProved;

            const bool bClaimsSolid = (Verdict == EVoxelTileClass::AllSolid);
            for (int32 gz = -1; gz <= GridDim; ++gz)
            for (int32 gy = -1; gy <= GridDim; ++gy)
            for (int32 gx = -1; gx <= GridDim; ++gx)
            {
                const float X = (float)(Origin.X + gx * Step);
                const float Y = (float)(Origin.Y + gy * Step);
                const float Z = (float)(Origin.Z + gz * Step);
                const float D = Stack.EvalMC(X, Y, Z);
                if (bClaimsSolid ? (D >= 0.0f) : (D < 0.0f))
                {
                    if (NumUnsound == 0)
                    {
                        AddError(FString::Printf(
                            TEXT("HOLE: the op stack claimed %s for the box at (%d,%d,%d) but ")
                            TEXT("EvalMC(%.0f, %.0f, %.0f) = %.6g is on the %s side. One of the ops' ")
                            TEXT("EffectOverBox/ClassifyBox is not conservative. Suspects, in order: ")
                            TEXT("the lattice source's ExtraReach (does it cover the roughness ")
                            TEXT("amplitude AND the carve blend?), then the seal's forcing verdict."),
                            bClaimsSolid ? TEXT("AllSolid") : TEXT("AllAir"),
                            Origin.X, Origin.Y, Origin.Z, X, Y, Z, D,
                            (D >= 0.0f) ? TEXT("AIR") : TEXT("SOLID")));
                    }
                    ++NumUnsound;
                    gz = gy = gx = GridDim + 1;   // ce verdict est déjà mort, tuile suivante
                }
            }
        }

        TestEqual(TEXT("every box verdict the stack emits survives brute force (a false verdict is a hole)"),
                  NumUnsound, 0);

        AddInfo(FString::Printf(
            TEXT("Box verdicts over 60 Maze tiles: %d proved uniform, %d Mixed. Today's ClassifyTile ")
            TEXT("proves ZERO of these -- every cave archetype falls through to \"pas prouvable en ")
            TEXT("v1\". Any number above zero here is tile-skipping Maze has never had."),
            NumProved, NumMixed));

        if (NumProved == 0)
        {
            AddWarning(TEXT("The stack proved no tile uniform, so it is not yet better than today's ")
                       TEXT("classifier for Maze. Not a correctness problem, but the perf case for ")
                       TEXT("the port rests on this number."));
        }
    }

    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
