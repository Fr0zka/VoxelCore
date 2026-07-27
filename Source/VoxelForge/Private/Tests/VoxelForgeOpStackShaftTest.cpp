// VoxelForgeOpStackShaftTest.cpp
// VerticalShafts — le portage qui teste la RÉUTILISATION, pas seulement la fidélité.
// VerticalShafts — the port that tests REUSE, not just fidelity.
//
// CE QUE CELUI-CI PROUVE EN PLUS DES AUTRES
// Les portages précédents demandaient « la décomposition reproduit-elle l'original ? ». Celui-ci
// demande **« les opérateurs se RÉUTILISENT-ils vraiment entre archétypes ? »**, qui est la thèse
// de `OPSTACK-PLAN §2.5` et la seule raison de faire ce refactor plutôt que de nettoyer le `switch`.
//
// Trois des cinq opérateurs de VerticalShafts sont ceux de Maze, **repris sans une ligne de
// changement** : `ConstantRock`, `SdfRoughness`, `SdfCarve`. Dans le `switch`, `GetMazeDensity` et
// `GetVerticalShaftDensity` sont deux fonctions de ~100 lignes qui n'ont rien en commun à l'œil.
// En opérateurs, ce sont les mêmes trois ops avec une source différente et d'autres réglages
// (fréquence 0.1 au lieu de 0.12, fenêtre `rough + 4` au lieu de `R + rough + 2`).
//
// **Si ce test passe en bit-à-bit, la réutilisation n'est plus une intention : c'est une mesure.**
//
// LA BARRE : bit à bit, comme les autres depuis `FPSemantics = Precise` (AUDIT §C9/§C10). Un écart
// est une vraie trouvaille, pas du bruit d'arrondi.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Async/ParallelFor.h"
#include "HAL/PlatformMisc.h"

#include "VoxelForgeTestFixture.h"
#include "VoxelDensityOpStack.h"

#include <atomic>

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FVoxelForgeOpStackShaftTest,
    "VoxelForge.OpStack.VerticalShaftEquivalence",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

namespace
{
    constexpr int32 NumShaftSamples = 20000;

    /** Les ledges et les connecteurs sont éteints ou discrets par défaut. Un test sur les seuls
     *  défauts vérifierait les cylindres et laisserait les DEUX opérateurs intéressants au repos —
     *  le même piège que `WaterLevelRelative` et la fenêtre d'overhang. */
    void EnableShaftFeatures(FVerticalShaftParams& P)
    {
        P.CrossConnectChance = 0.65f;    // des connecteurs, donc des capsules dans le SDF
        P.ConnectorRadius    = 3.5f;
        P.LedgeSpacing       = 11.0f;    // des étagères, donc l'op forçant s'exécute
        P.LedgeDepth         = 2.5f;
        P.SurfaceRoughness   = 3.0f;     // la rugosité SDF partagée avec Maze
    }
}

bool FVoxelForgeOpStackShaftTest::RunTest(const FString& Parameters)
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
    if (!World.GetSlotVoxelZRange(FTestWorld::SlotVerticalShafts, TopVoxelZ, BottomVoxelZ))
    {
        AddError(TEXT("The fixture layout has no VerticalShafts slot. Check FTestWorld::Build's ")
                 TEXT("Archetypes[] against FTestWorld::SlotVerticalShafts."));
        return false;
    }

    const int32 MidChunkZ = ((TopVoxelZ + BottomVoxelZ) / 2) / CHUNK_SIZE;
    FVerticalShaftParams P = World.StrateManager->GetVerticalShaftParamsForChunk(
        FIntVector(0, 0, MidChunkZ));

    if (P.StrateTopWorldZ - P.StrateBottomWorldZ <= 0.0f)
    {
        AddError(TEXT("The VerticalShafts strate has degenerate Z bounds, which sends ")
                 TEXT("GetVerticalShaftDensity down its early-out. The op stack has none by design."));
        return false;
    }

    EnableShaftFeatures(P);

    FVoxelOpStack Stack;
    VoxelDensityOps::BuildVerticalShaftStack(Stack, P, World.Settings->Seed,
                                             Gen->OriginSpineRadius, World.StrateManager.Get());

    // rock + shafts + roughness + carve + ledges + 3 structurels.
    TestEqual(TEXT("the shaft stack is decomposed into 8 ops"), Stack.Num(), 8);

    FVoxelOpContext Ctx;
    Ctx.Seed               = (uint32)World.Settings->Seed;
    Ctx.LayoutVersion      = World.StrateManager->GetLayoutVersion();
    Ctx.StrateTopWorldZ    = P.StrateTopWorldZ;
    Ctx.StrateBottomWorldZ = P.StrateBottomWorldZ;
    Stack.PrepareChunk(Ctx);

    TArray<FVector> Points;
    Points.Reserve(NumShaftSamples);
    {
        FRandomStream Rng(80486);
        for (int32 i = 0; i < NumShaftSamples; ++i)
        {
            Points.Add(FVector(
                (float)Rng.RandRange(-3 * CHUNK_SIZE, 3 * CHUNK_SIZE),
                (float)Rng.RandRange(-3 * CHUNK_SIZE, 3 * CHUNK_SIZE),
                (float)Rng.RandRange(BottomVoxelZ, TopVoxelZ)));
        }
    }

    //=========================================================================
    // 1. ÉQUIVALENCE
    //=========================================================================
    int32 NumDiff = 0, NumSideDisagree = 0, WorstIdx = -1, NumInsideShaft = 0;
    float WorstDelta = 0.0f;

    for (int32 i = 0; i < NumShaftSamples; ++i)
    {
        const float X = (float)Points[i].X, Y = (float)Points[i].Y, Z = (float)Points[i].Z;

        const float Old = Gen->GetVerticalShaftDensity(X, Y, Z, P);
        const float New = Stack.EvalMC(X, Y, Z);

        if (Old >= 0.0f) { ++NumInsideShaft; }   // air ⇒ dans un puits/connecteur/étagère

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
            TEXT("VerticalShafts: bit-identical across %d samples (%d of them inside a shaft, so ")
            TEXT("the cylinders, connectors, roughness, carve and ledges were all exercised). ")
            TEXT("THREE of the five ops here are Maze's, reused unchanged -- operator reuse across ")
            TEXT("archetypes is now measured rather than intended (OPSTACK-PLAN 2.5)."),
            NumShaftSamples, NumInsideShaft));
    }
    else
    {
        AddError(FString::Printf(
            TEXT("VerticalShafts: %d of %d samples differ (largest |delta| %.9g at (%.0f, %.0f, ")
            TEXT("%.0f)); %d cross the isosurface. Since /fp:precise the bar is bit-identity, so ")
            TEXT("this is a real port error. Check, in order: the roughness FREQUENCY (0.1 here, ")
            TEXT("NOT Maze's 0.12) and window (rough + 4, not R + rough + 2), the 'Shft' salt ")
            TEXT("(0x53686674), the connector pair hash and its Z lerp between sealed bounds, and ")
            TEXT("the ledge gate reading the POST-roughness Sdf rather than re-deriving it."),
            NumDiff, NumShaftSamples, WorstDelta,
            WorstIdx >= 0 ? Points[WorstIdx].X : 0.0f,
            WorstIdx >= 0 ? Points[WorstIdx].Y : 0.0f,
            WorstIdx >= 0 ? Points[WorstIdx].Z : 0.0f,
            NumSideDisagree));
    }

    TestEqual(TEXT("no sample lands on the opposite side of the isosurface"), NumSideDisagree, 0);

    if (NumInsideShaft == 0)
    {
        AddWarning(TEXT("No sample landed inside a shaft, so the source, carve and ledge ops were ")
                   TEXT("never meaningfully exercised. Raise ShaftDensity or ShaftMaxRadius."));
    }

    //=========================================================================
    // 2. INVARIANCE DE FENÊTRE
    //=========================================================================
    // La source garde un cache 3×3 `thread_local` dont la clé est le jeu de params : c'est
    // exactement le genre d'endroit où une clé incomplète produit une couture (AUDIT §C2).
    {
        std::atomic<int32> Impure{ 0 };
        const int32 NumBlocks = FMath::Max(4, FMath::Min(16, FPlatformMisc::NumberOfCores()));

        TArray<float> Ref;
        Ref.SetNumUninitialized(NumShaftSamples);
        for (int32 i = 0; i < NumShaftSamples; ++i)
        {
            Ref[i] = Stack.EvalMC((float)Points[i].X, (float)Points[i].Y, (float)Points[i].Z);
        }

        ParallelFor(NumBlocks, [&](int32 Block)
        {
            TArray<int32> LocalOrder;
            BuildShuffledOrder(NumShaftSamples, 2200 + Block, LocalOrder);
            for (const int32 i : LocalOrder)
            {
                const float V = Stack.EvalMC((float)Points[i].X, (float)Points[i].Y, (float)Points[i].Z);
                if (!BitEqual(V, Ref[i])) { Impure.fetch_add(1, std::memory_order_relaxed); }
            }
        });

        TestEqual(TEXT("the shaft stack is window-invariant across order and threads"),
                  Impure.load(), 0);
    }

    //=========================================================================
    // 3. LE VERDICT DE BOÎTE
    //=========================================================================
    {
        int32 NumProved = 0, NumMixed = 0, NumUnsound = 0;
        FRandomStream Rng(13579);

        for (int32 t = 0; t < 60; ++t)
        {
            const int32 Step = 1, Cells = 8;
            const int32 Extent = Step * Cells;
            const FIntVector Origin(
                Rng.RandRange(-6, 6) * Extent,
                Rng.RandRange(-6, 6) * Extent,
                FMath::Clamp(Rng.RandRange(BottomVoxelZ / Extent, TopVoxelZ / Extent), -4096, 4096) * Extent);

            const int32 GridDim = Cells + 1;
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
                            TEXT("HOLE: the shaft stack claimed %s for the box at (%d,%d,%d) but ")
                            TEXT("EvalMC(%.0f, %.0f, %.0f) = %.6g is on the %s side. Suspects, in ")
                            TEXT("order: the shaft source's ExtraReach (does it cover the roughness ")
                            TEXT("amplitude AND the carve blend?), then the connector sweep (a ")
                            TEXT("connector can reach Spacing*1.6 beyond its cell), then the ledge ")
                            TEXT("op's FillOnly."),
                            bClaimsSolid ? TEXT("AllSolid") : TEXT("AllAir"),
                            Origin.X, Origin.Y, Origin.Z, X, Y, Z, D,
                            (D >= 0.0f) ? TEXT("AIR") : TEXT("SOLID")));
                    }
                    ++NumUnsound;
                    gz = gy = gx = GridDim + 1;
                }
            }
        }

        TestEqual(TEXT("every box verdict the shaft stack emits survives brute force"), NumUnsound, 0);

        AddInfo(FString::Printf(
            TEXT("Box verdicts over 60 VerticalShafts tiles: %d proved uniform, %d Mixed. Today's ")
            TEXT("ClassifyTile proves ZERO of these -- every cave archetype falls through to \"pas ")
            TEXT("prouvable en v1\"."),
            NumProved, NumMixed));
    }

    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
