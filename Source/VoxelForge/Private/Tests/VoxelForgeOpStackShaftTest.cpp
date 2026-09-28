// VoxelForgeOpStackShaftTest.cpp
// VerticalShafts — la RÉUTILISATION d'opérateurs et le contrat de la pile.
// VerticalShafts — operator REUSE and the stack's contract.
//
// Trois des cinq opérateurs de VerticalShafts sont ceux de Maze, repris tels quels :
// `ConstantRock`, `SdfRoughness`, `SdfCarve` — mêmes ops, autre source et autres réglages
// (fréquence 0.1 au lieu de 0.12, fenêtre `rough + 4` au lieu de `R + rough + 2`). C'est la thèse
// de `OPSTACK-PLAN §2.5` : les opérateurs se réutilisent entre archétypes.
//
// Three of the five VerticalShafts ops are Maze's, reused unchanged. The assertions below target
// the stack used in-game: channel count/order, feature liveness, window invariance, cache cost and
// box-proof soundness.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Async/ParallelFor.h"
#include "HAL/PlatformTime.h"
#include "HAL/PlatformMisc.h"

#include "VoxelForgeTestFixture.h"
#include "VoxelDensityOpStack.h"
#include "VoxelPassageGeometry.h"

#include <atomic>

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FVoxelForgeOpStackShaftTest,
    "VoxelForge.OpStack.VerticalShaftEquivalence",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

// This late sweep remains full-strength and is run with -TestFilter when the aggregate launch
// has already reached the harness's 30-minute guard.
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

    // The owner generator resets this call-local hand-off at the start and end of every density
    // query. Direct stack tests must model that boundary too: otherwise a connector-air write from
    // the previous sample can suppress the next sample's passage support floor.
    float EvalStackMCForTest(const FVoxelOpStack& Stack, float X, float Y, float Z)
    {
        VoxelPassageGeometry::ResetVerticalShaftConnectorAirMarker();
        const float Result = Stack.EvalMC(X, Y, Z);
        VoxelPassageGeometry::ResetVerticalShaftConnectorAirMarker();
        return Result;
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
        AddError(TEXT("The VerticalShafts strate has degenerate Z bounds; GetDensityAt builds no ")
                 TEXT("stack for it. The op stack has no degenerate-strate early-out by design."));
        return false;
    }

    EnableShaftFeatures(P);

    FVoxelOpStack Stack;
    VoxelDensityOps::BuildVerticalShaftStack(Stack, P, World.Settings->Seed,
                                             Gen->OriginSpineRadius, World.StrateManager.Get());

    // rock + shafts + roughness + carve + ledges + 4 structural posts.
    TestEqual(TEXT("the shaft stack is decomposed into 11 ops"), Stack.Num(), 11);

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
    // 1. LIVENESS — des échantillons tombent dans l'air des puits / samples land in shaft air
    //=========================================================================
    int32 NumInsideShaft = 0;
    for (int32 i = 0; i < NumShaftSamples; ++i)
    {
        const float X = (float)Points[i].X, Y = (float)Points[i].Y, Z = (float)Points[i].Z;
        // air ⇒ dans un puits/connecteur/étagère / air ⇒ inside a shaft, connector or ledge
        if (EvalStackMCForTest(Stack, X, Y, Z) >= 0.0f) { ++NumInsideShaft; }
    }
    AddInfo(FString::Printf(TEXT("VerticalShafts: %d of %d samples are air in the stack."),
                            NumInsideShaft, NumShaftSamples));
    TestTrue(TEXT("shaft feature sampling exercised the operator stack"), NumInsideShaft > 0);

    if (NumInsideShaft == 0)
    {
        AddWarning(TEXT("No sample landed inside a shaft, so the source, carve and ledge ops were ")
                   TEXT("never meaningfully exercised. Raise ShaftDensity or ShaftMaxRadius."));
    }

    //=========================================================================
    // 2. INVARIANCE DE FENÊTRE
    //=========================================================================
    // La source garde un cache inner 3×3 `thread_local`; chaque rebuild collecte un halo 9×9/15×15
    // pour les fenêtres d'arbre 5×5 et le fallback ±3. La clé est le jeu de params : c'est exactement le genre d'endroit où
    // une clé incomplète produit une couture (AUDIT §C2).
    {
        std::atomic<int32> Impure{ 0 };
        std::atomic<int32> FirstImpure{ -1 };
        std::atomic<uint32> FirstImpureRefBits{ 0 };
        std::atomic<uint32> FirstImpureValueBits{ 0 };
        const int32 NumBlocks = FMath::Max(4, FMath::Min(16, FPlatformMisc::NumberOfCores()));

        TArray<float> Ref;
        Ref.SetNumUninitialized(NumShaftSamples);
        for (int32 i = 0; i < NumShaftSamples; ++i)
        {
            Ref[i] = EvalStackMCForTest(
                Stack, (float)Points[i].X, (float)Points[i].Y, (float)Points[i].Z);
        }

        ParallelFor(NumBlocks, [&](int32 Block)
        {
            TArray<int32> LocalOrder;
            BuildShuffledOrder(NumShaftSamples, 2200 + Block, LocalOrder);
            for (const int32 i : LocalOrder)
            {
                const float V = EvalStackMCForTest(
                    Stack, (float)Points[i].X, (float)Points[i].Y, (float)Points[i].Z);
                if (!BitEqual(V, Ref[i]))
                {
                    Impure.fetch_add(1, std::memory_order_relaxed);
                    int32 Expected = -1;
                    if (FirstImpure.compare_exchange_strong(
                            Expected, i, std::memory_order_relaxed))
                    {
                        uint32 RefBits = 0;
                        uint32 ValueBits = 0;
                        FMemory::Memcpy(&RefBits, &Ref[i], sizeof(RefBits));
                        FMemory::Memcpy(&ValueBits, &V, sizeof(ValueBits));
                        FirstImpureRefBits.store(RefBits, std::memory_order_relaxed);
                        FirstImpureValueBits.store(ValueBits, std::memory_order_relaxed);
                    }
                }
            }
        });

        TestEqual(TEXT("the shaft stack is window-invariant across order and threads"),
                  Impure.load(), 0);
        if (FirstImpure.load(std::memory_order_relaxed) >= 0)
        {
            float RefValue = 0.0f;
            float ParallelValue = 0.0f;
            const uint32 RefBits = FirstImpureRefBits.load(std::memory_order_relaxed);
            const uint32 ValueBits = FirstImpureValueBits.load(std::memory_order_relaxed);
            FMemory::Memcpy(&RefValue, &RefBits, sizeof(RefValue));
            FMemory::Memcpy(&ParallelValue, &ValueBits, sizeof(ParallelValue));
            const int32 Index = FirstImpure.load(std::memory_order_relaxed);
            AddInfo(FString::Printf(
                TEXT("first shaft window mismatch: sample %d point (%.0f,%.0f,%.0f) "
                     "serial=%.9g parallel=%.9g"),
                Index, Points[Index].X, Points[Index].Y, Points[Index].Z,
                RefValue, ParallelValue));
        }
    }

    //==========================================================================
    // 2b. CACHE COST
    //==========================================================================
    {
        // Force one distinct centre cell per call, then hold one cell constant. This measures the
    // widened collection where it belongs (cache rebuild), separately from the hot per-voxel
    // path. The implementation deliberately performs 225 cell rolls for the former and zero
    // cell rolls for the latter; only the cached inner 3×3/capsule SDFs remain hot.
        constexpr int32 NumRebuildSamples = 256;
        constexpr int32 NumHotSamples = 20000;
        const float PerfSpacing = FMath::Max(P.ShaftSpacing, 1.0f);
        const float PerfZ = 0.5f * (P.StrateBottomWorldZ + P.StrateTopWorldZ);
        volatile float Sink = 0.0f;

        const double RebuildStart = FPlatformTime::Seconds();
        float LastX = 0.0f;
        float LastY = 0.0f;
        for (int32 i = 0; i < NumRebuildSamples; ++i)
        {
            const int32 CellX = (i % 16) - 8;
            const int32 CellY = (i / 16) - 8;
            LastX = (static_cast<float>(CellX) + 0.37f) * PerfSpacing;
            LastY = (static_cast<float>(CellY) + 0.61f) * PerfSpacing;
            Sink += EvalStackMCForTest(Stack, LastX, LastY, PerfZ);
        }
        const double RebuildSeconds = FPlatformTime::Seconds() - RebuildStart;

        const double HotStart = FPlatformTime::Seconds();
        for (int32 i = 0; i < NumHotSamples; ++i)
        {
            Sink += EvalStackMCForTest(Stack, LastX, LastY, PerfZ);
        }
        const double HotSeconds = FPlatformTime::Seconds() - HotStart;

        AddInfo(FString::Printf(
            TEXT("VerticalShafts cache perf: %d forced rebuilds at %.3f us/call and %d hot calls "
                 "at %.3f us/call (15x15=225 cell rolls only on rebuild; inner 3x3/capsule SDF "
                 "only on hot calls; sink=%.9g)."),
            NumRebuildSamples,
            RebuildSeconds * 1.0e6 / static_cast<double>(NumRebuildSamples),
            NumHotSamples,
            HotSeconds * 1.0e6 / static_cast<double>(NumHotSamples),
            static_cast<float>(Sink)));
    }

    //=========================================================================
    // 3. LE VERDICT DE BOÎTE
    //=========================================================================
    {
        int32 NumProved = 0, NumMixed = 0, NumUnsound = 0, NumBruteSamples = 0;
        FRandomStream Rng(13579);
        // Hors de la boucle : la ligne de rapport en a besoin. Une étendue d'échantillonnage qu'on
        // ne peut pas citer dans le rapport est une étendue que personne ne surveille.
        const int32 SpanCells  = 55;
        const int32 SpanVoxels = SpanCells * 8;   // Extent = Step * Cells = 1 * 8

        for (int32 t = 0; t < 60; ++t)
        {
            const int32 Step = 1, Cells = 8;
            const int32 Extent = Step * Cells;
            // ⚠️ L'ÉTENDUE XY ÉTAIT ±48 VOXELS, POUR UN `ShaftSpacing` DE 55 : moins d'UNE cellule
            // de puits. C'est le même piège que celui qui a coûté trois runs au test TunnelNetwork —
            // un échantillonneur qui ne couvre pas une période du motif ne mesure pas le monde, il
            // mesure un point du motif. ±440 = 8 périodes.
            // The XY extent was ±48 voxels for a ShaftSpacing of 55 — less than one shaft cell, the
            // same trap that cost the TunnelNetwork test three runs. ±440 covers 8 periods.
            const FIntVector Origin(
                Rng.RandRange(-SpanCells, SpanCells) * Extent,
                Rng.RandRange(-SpanCells, SpanCells) * Extent,
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
                const float D = EvalStackMCForTest(Stack, X, Y, Z);
                ++NumBruteSamples;
                if (bClaimsSolid ? (D >= 0.0f) : (D < 0.0f))
                {
                    if (NumUnsound == 0)
                    {
                        AddError(FString::Printf(
                            TEXT("HOLE: the shaft stack claimed %s for the box at (%d,%d,%d) but ")
                            TEXT("EvalMC(%.0f, %.0f, %.0f) = %.6g is on the %s side. Suspects, in ")
                            TEXT("order: the shaft source's SDF interval (does it cover all shaft ")
                            TEXT("and connector geometry?), then the connector sweep (a ")
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
            TEXT("Box verdicts over 60 VerticalShafts tiles (XY sampled from +/-%d voxels = %.1f x ")
            TEXT("ShaftSpacing %.0f): %d proved uniform, %d Mixed, %d voxels checked, %d violations. ")
            TEXT("This was 0 proved for as long as the connector branch bailed on mere shaft ")
            TEXT("EXISTENCE within Spacing*1.6 -- true almost everywhere at ShaftDensity 0.6, so it ")
            TEXT("was conservative AND sterile. It now tests the real connector capsules. Read the ")
            TEXT("proved count as a measurement; what is ASSERTED is that none of them is wrong, ")
            TEXT("because a false verdict here leaves no geometry and no collision."),
            SpanVoxels, (float)SpanVoxels / FMath::Max(P.ShaftSpacing, 1.0f), P.ShaftSpacing,
            NumProved, NumMixed, NumBruteSamples, NumUnsound));

        if (NumProved == 0)
        {
            AddWarning(TEXT("No VerticalShafts tile was proved, so the brute force above verified ")
                       TEXT("nothing. Before hypothesising: the shaft CIRCLE test and the connector ")
                       TEXT("CAPSULE test are the only two things that can return CarveOnly here, ")
                       TEXT("and the interval must cover both the shaft and connector capsules before ")
                       TEXT("either test is relaxed."));
        }
    }

    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
