// VoxelForgeOpStackMazeTest.cpp
// PHASE 1, LE TEST QUI COMPTE — la pile d'opérateurs Maze contre GetMazeDensity.
// PHASE 1'S LOAD-BEARING TEST — the Maze operator stack against GetMazeDensity.
//
// CE QUE LA PHASE 1 DOIT PROUVER / WHAT PHASE 1 HAS TO PROVE
// La question n'est pas « est-ce que le code tourne ». C'est celle du déclencheur d'arrêt de
// `OPSTACK-PLAN §4` : **« est-ce que la séparation source / modifier tombe naturellement du code
// existant ? »** Si oui, la décomposition reproduit l'original à l'identique sans contorsion. Si
// non, on s'en aperçoit ici — pas trois archétypes plus tard.
//
// Not "does the code run". It is the stop-trigger question from OPSTACK-PLAN §4: **does the
// source/modifier split fall out naturally from the existing code?** If it does, the decomposition
// reproduces the original without contortion. If it doesn't, we find out HERE — not three
// archetypes later.
//
// SUR LA BARRE D'ACCEPTATION / ON THE ACCEPTANCE BAR
// `OPSTACK-PLAN §2.6` n'EXIGE PAS l'identité binaire avec l'ancien système — c'est justement la
// relaxation qui autorise une vraie décomposition plutôt qu'un emballage. Mais Maze se décompose
// si proprement qu'on peut viser l'identité binaire, et quand on peut l'avoir il faut la prendre :
// elle transforme « je crois que la décomposition est juste » en preuve. Un ÉCHEC ici n'est donc
// pas forcément une erreur — c'est un signal à lire (le test rapporte l'écart max et où).
//
// §2.6 does NOT require bit-identity — that relaxation is what permits real decomposition. But Maze
// decomposes cleanly enough to achieve it, and where it is achievable it should be taken: it turns
// belief into proof. A FAILURE here is not automatically a bug — it is a signal to read (the test
// reports the largest divergence and where it is).

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

    // GetMazeDensity court-circuite sur une strate dégénérée (`return 1.0f`, air, en convention MC).
    // Cette garde appartient à la fonction d'archétype, pas à un opérateur ; la pile suppose une
    // strate valide. Vérifier plutôt que supposer.
    if (MazeParams.StrateTopWorldZ - MazeParams.StrateBottomWorldZ <= 0.0f)
    {
        AddError(FString::Printf(
            TEXT("The Maze strate has degenerate Z bounds (top %.1f, bottom %.1f), which sends ")
            TEXT("GetMazeDensity down its early-out. The op stack has no such early-out by design, ")
            TEXT("so the comparison below would be meaningless."),
            MazeParams.StrateTopWorldZ, MazeParams.StrateBottomWorldZ));
        return false;
    }

    const UVoxelGenerator* Gen = World.Generator.Get();

    FVoxelOpStack Stack;
    VoxelDensityOps::BuildMazeStack(Stack, MazeParams, World.Settings->Seed,
                                    Gen->OriginSpineRadius, World.StrateManager.Get());

    // La décomposition doit être une DÉCOMPOSITION. Un `FMazeOp` monolithique passerait tous les
    // tests numériques ci-dessous et aurait pourtant raté l'objet entier du refactor
    // (OPSTACK-PLAN §2.5). C'est le seul test que le nombre d'opérateurs mérite.
    // A monolithic FMazeOp would pass every numeric check below and still have missed the entire
    // point (OPSTACK-PLAN §2.5). This is the one thing an op COUNT is worth asserting.
    TestEqual(TEXT("the Maze stack is decomposed, not wrapped (rock + corridors + roughness + carve + 3 structural)"),
              Stack.Num(), 7);

    FVoxelOpContext Ctx;
    Ctx.Seed                = (uint32)World.Settings->Seed;
    Ctx.LayoutVersion       = World.StrateManager->GetLayoutVersion();
    Ctx.StrateTopWorldZ     = MazeParams.StrateTopWorldZ;
    Ctx.StrateBottomWorldZ  = MazeParams.StrateBottomWorldZ;
    Stack.PrepareChunk(Ctx);

    // ── Points d'échantillonnage : dans la bande Z de la strate Maze, largement autour de (0,0)
    //    pour que la spine, les passages et le roc ordinaire soient tous représentés. ──
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

    // ── L'ÉQUIVALENCE. ──
    int32 NumDiff = 0, WorstIdx = -1;
    float WorstDelta = 0.0f;
    int32 NumSolidDisagreements = 0;   // le seul écart qui compte VRAIMENT : un côté d'iso différent
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
        }
        // Le mesher ne lit que le SIGNE (D >= IsoLevel ⇒ air). Deux valeurs peuvent différer d'un
        // ULP sans changer un seul triangle ; un désaccord de CÔTÉ change la géométrie.
        if ((Old >= 0.0f) != (New >= 0.0f)) { ++NumSolidDisagreements; }
    }

    if (NumDiff == 0)
    {
        AddInfo(FString::Printf(
            TEXT("Bit-identical across %d samples. The Maze decomposition (constant rock -> lattice ")
            TEXT("corridors -> SDF roughness -> carve -> spine/seal/passage) reproduces ")
            TEXT("GetMazeDensity exactly, which is as strong a signal as Phase 1 can get that the ")
            TEXT("source/modifier split is real and not imposed."), NumMazeSamples));
    }
    else
    {
        AddWarning(FString::Printf(
            TEXT("%d of %d samples differ (largest |delta| %.9g at (%.0f, %.0f, %.0f)); %d of them ")
            TEXT("land on the OPPOSITE side of the isosurface. OPSTACK-PLAN section 2.6 does not ")
            TEXT("require bit-identity, so this is a warning, not a failure -- but Maze SHOULD be ")
            TEXT("reproducible exactly, so a nonzero count means the port drifted somewhere. Check, ")
            TEXT("in order: the roughness apply-window (R + SurfaceRoughness + 2), the carve blend ")
            TEXT("(2.0), the noise frequency (0.12) and octave count (3), and the order of the ")
            TEXT("structural post ops."),
            NumDiff, NumMazeSamples, WorstDelta,
            WorstIdx >= 0 ? Points[WorstIdx].X : 0.0f,
            WorstIdx >= 0 ? Points[WorstIdx].Y : 0.0f,
            WorstIdx >= 0 ? Points[WorstIdx].Z : 0.0f,
            NumSolidDisagreements));
    }

    // Un désaccord de côté d'iso EST une différence de géométrie. C'est la seule chose ici qui
    // mérite un échec dur. / A side-of-iso disagreement IS a geometry difference. The one hard fail.
    TestEqual(TEXT("no sample lands on the opposite side of the isosurface from the original"),
              NumSolidDisagreements, 0);

    // ── La pile doit satisfaire les MÊMES invariants que le reste du générateur. ──
    // Invariance de fenêtre : pure, ordre-indépendante, identique sur tous les threads. Le cache
    // par cellule de la source de couloirs est `thread_local` — c'est exactement le genre d'endroit
    // où une clé incomplète produit une couture (cf. AUDIT C2).
    {
        TArray<int32> Order;
        BuildShuffledOrder(NumMazeSamples, 8675309, Order);
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

    // ── LE VERDICT DE BOÎTE : Maze n'a JAMAIS su sauter une tuile. ──
    // ClassifyTile renvoie Mixed pour tout archétype de grotte ("pas prouvable en v1"), donc
    // TunnelNetwork, Maze, VerticalShafts, FloatingIslands, FlatPlain, CrystalChamber et Underwater
    // ne captent RIEN du gain T1.d. C'est le vrai prix perf du refactor, et c'est vérifiable ici.
    //
    // Maze has NEVER skipped a tile: ClassifyTile returns Mixed for every cave archetype. This is
    // the refactor's real perf prize, and it is checkable right here.
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

            // La MÊME boîte que le treillis du mesher, marge +/-1 comprise (cf. ClassifyTile).
            const int32 GridDim = Cells + 1;
            const FBox Box(
                FVector(Origin.X - Step, Origin.Y - Step, Origin.Z - Step),
                FVector(Origin.X + GridDim * Step, Origin.Y + GridDim * Step, Origin.Z + GridDim * Step));

            const EVoxelTileClass Verdict = Stack.ClassifyBox(Box, Ctx);
            if (Verdict == EVoxelTileClass::Mixed) { ++NumMixed; continue; }
            ++NumProved;

            // Force brute : le verdict doit tenir sur CHAQUE point du treillis. Un faux verdict
            // n'est pas une imprécision, c'est un trou — pas de géométrie, PAS DE COLLISION.
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
                       TEXT("the port rests on this number -- check whether BranchProbability is high ")
                       TEXT("enough that corridors genuinely reach every sampled tile, or whether the ")
                       TEXT("lattice source's reach is over-conservative."));
        }
    }

    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
