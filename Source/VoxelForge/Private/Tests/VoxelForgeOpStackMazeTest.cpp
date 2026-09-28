// VoxelForgeOpStackMazeTest.cpp
// La pile d'opérateurs Maze / The Maze operator stack.
//
// Ce que ce test vérifie / What this test checks:
//   • TOPOLOGIE — le graphe parent orienté vers l'origine est un arbre couvrant (N-1 arêtes, zéro
//     cellule déconnectée), les boucles optionnelles restent bornées, et le graphe est déterministe
//     sur 64 seeds. / The origin-directed parent graph is a spanning tree, deterministic per seed.
//   • DÉCOMPOSITION — huit opérateurs (roche + couloirs + rugosité + carve + 4 structurels), pas un
//     `FMazeOp` monolithique. / Eight ops, not one wrapped monolith.
//   • INVARIANCE DE FENÊTRE — le cache par cellule est `thread_local` : bit-identique quel que soit
//     l'ordre des requêtes et le thread. / Bit-identical across query order and worker threads.
//   • VERDICTS DE BOÎTE — chaque verdict uniforme survit à la force brute (un faux verdict est un
//     trou), y compris pour une pile source/convertisseur jamais écrite à la main.
//     Every uniform box verdict survives brute force, including for a novel source/converter stack.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Async/ParallelFor.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"

#include "VoxelForgeTestFixture.h"
#include "VoxelDensityOpStack.h"
#include "VoxelCaveMorphology.h"

#include <atomic>

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FVoxelForgeOpStackMazeTest,
    "VoxelForge.OpStack.MazeEquivalence",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

namespace
{
    constexpr int32 NumMazeSamples = 20000;
    constexpr int32 NumMazeBoxTests = 256;

    struct FMazeGraphAudit
    {
        int32 NumNodes = 0;
        int32 NumEdges = 0;
        int32 NumDisconnected = 0;
        TArray<uint64> EdgeKeys;
        TArray<int32> RunLengths;
        int64 DegreeCounts[8] = {};
    };

    int32 MazeGraphIndex(int32 X, int32 Y, int32 Z, int32 Radius)
    {
        const int32 Dim = Radius * 2 + 1;
        return (X + Radius) + Dim * ((Y + Radius) + Dim * (Z + Radius));
    }

    uint64 MazeGraphEdgeKey(int32 A, int32 B)
    {
        const uint32 Lo = static_cast<uint32>(FMath::Min(A, B));
        const uint32 Hi = static_cast<uint32>(FMath::Max(A, B));
        return (static_cast<uint64>(Lo) << 32) | static_cast<uint64>(Hi);
    }

    void MazeGraphAddEdge(TArray<TArray<int32>>& Adjacency, TSet<uint64>& Seen,
                          int32 A, int32 B)
    {
        if (A == B) { return; }
        const uint64 Key = MazeGraphEdgeKey(A, B);
        if (!Seen.Contains(Key))
        {
            Seen.Add(Key);
            Adjacency[A].Add(B);
            Adjacency[B].Add(A);
        }
    }

    FMazeGraphAudit AuditMazeGraph(const FMazeGenerationParams& Params, int32 Seed,
                                   int32 Radius, int32 MetricRadius)
    {
        FMazeGraphAudit Out;
        const int32 Dim = Radius * 2 + 1;
        Out.NumNodes = Dim * Dim * Dim;
        TArray<TArray<int32>> Adjacency;
        Adjacency.SetNum(Out.NumNodes);
        TSet<uint64> Seen;
        const uint32 Salt = static_cast<uint32>(Seed) ^ 0x4D617A65u;

        // One parent per node. In this centered finite audit the parent of every sampled node is
        // also sampled because it lowers Manhattan distance to the origin.
        for (int32 Z = -Radius; Z <= Radius; ++Z)
        for (int32 Y = -Radius; Y <= Radius; ++Y)
        for (int32 X = -Radius; X <= Radius; ++X)
        {
            FIntVector Parent;
            if (VoxelMazeTopology::TryGetParent(X, Y, Z, Salt, Parent))
            {
                MazeGraphAddEdge(Adjacency, Seen,
                    MazeGraphIndex(X, Y, Z, Radius),
                    MazeGraphIndex(Parent.X, Parent.Y, Parent.Z, Radius));
            }
        }

        // Add only the optional non-tree edges. The tree count is therefore observable directly,
        // and the same routine can audit the authored loop knobs without treating them as a
        // connectivity requirement.
        for (int32 Z = -Radius; Z <= Radius; ++Z)
        for (int32 Y = -Radius; Y <= Radius; ++Y)
        for (int32 X = -Radius; X <= Radius; ++X)
        {
            const int32 A = MazeGraphIndex(X, Y, Z, Radius);
            if (X < Radius && VoxelMazeTopology::IsLoopEdgeOpen(
                    X, Y, Z, VoxelMazeTopology::EAxis::X, Salt,
                    Params.BranchProbability, Params.Verticality))
            {
                MazeGraphAddEdge(Adjacency, Seen, A,
                    MazeGraphIndex(X + 1, Y, Z, Radius));
            }
            if (Y < Radius && VoxelMazeTopology::IsLoopEdgeOpen(
                    X, Y, Z, VoxelMazeTopology::EAxis::Y, Salt,
                    Params.BranchProbability, Params.Verticality))
            {
                MazeGraphAddEdge(Adjacency, Seen, A,
                    MazeGraphIndex(X, Y + 1, Z, Radius));
            }
            if (Z < Radius && VoxelMazeTopology::IsLoopEdgeOpen(
                    X, Y, Z, VoxelMazeTopology::EAxis::Z, Salt,
                    Params.BranchProbability, Params.Verticality))
            {
                MazeGraphAddEdge(Adjacency, Seen, A,
                    MazeGraphIndex(X, Y, Z + 1, Radius));
            }
        }

        Out.NumEdges = Seen.Num();
        Out.EdgeKeys.Reserve(Seen.Num());
        for (const uint64 Key : Seen) { Out.EdgeKeys.Add(Key); }
        Out.EdgeKeys.Sort();

        TArray<uint8> Visited;
        Visited.Init(0, Out.NumNodes);
        TArray<int32> Pending;
        Pending.Add(MazeGraphIndex(0, 0, 0, Radius));
        Visited[Pending[0]] = 1;
        while (Pending.Num() > 0)
        {
            const int32 Current = Pending.Pop(EAllowShrinking::No);
            for (const int32 Next : Adjacency[Current])
            {
                if (!Visited[Next])
                {
                    Visited[Next] = 1;
                    Pending.Add(Next);
                }
            }
        }
        for (const uint8 WasVisited : Visited)
        {
            if (!WasVisited) { ++Out.NumDisconnected; }
        }

        // Report degree only for an interior window so the finite audit boundary cannot invent
        // dead ends. Degree 1 is the dead-end count; degree 6 would be the regular cubic grid.
        for (int32 Z = -MetricRadius; Z <= MetricRadius; ++Z)
        for (int32 Y = -MetricRadius; Y <= MetricRadius; ++Y)
        for (int32 X = -MetricRadius; X <= MetricRadius; ++X)
        {
            const int32 Degree = Adjacency[ MazeGraphIndex(X, Y, Z, Radius) ].Num();
            ++Out.DegreeCounts[FMath::Min(Degree, 7)];
        }

        // A run is a maximal sequence of degree-2 edges between junctions/dead ends. The graph
        // is sampled with a larger halo than the reported degree window; truncation only affects
        // the outermost runs, never the connectivity assertion.
        TSet<uint64> UsedRunEdges;
        for (int32 Start = 0; Start < Adjacency.Num(); ++Start)
        {
            if (Adjacency[Start].Num() == 2) { continue; }
            for (const int32 First : Adjacency[Start])
            {
                const uint64 FirstKey = MazeGraphEdgeKey(Start, First);
                if (UsedRunEdges.Contains(FirstKey)) { continue; }

                int32 Length = 1;
                UsedRunEdges.Add(FirstKey);
                int32 Previous = Start;
                int32 Current = First;
                while (Adjacency[Current].Num() == 2)
                {
                    const int32 Next = Adjacency[Current][0] == Previous
                        ? Adjacency[Current][1] : Adjacency[Current][0];
                    const uint64 EdgeKey = MazeGraphEdgeKey(Current, Next);
                    if (UsedRunEdges.Contains(EdgeKey)) { break; }
                    UsedRunEdges.Add(EdgeKey);
                    ++Length;
                    Previous = Current;
                    Current = Next;
                }
                Out.RunLengths.Add(Length);
            }
        }
        // A completely degree-2 cycle is not expected for this tree-plus-loops graph, but keep
        // the distribution total honest if a future loop policy creates one.
        for (int32 Start = 0; Start < Adjacency.Num(); ++Start)
        for (const int32 First : Adjacency[Start])
        {
            const uint64 FirstKey = MazeGraphEdgeKey(Start, First);
            if (UsedRunEdges.Contains(FirstKey)) { continue; }
            int32 Length = 1;
            UsedRunEdges.Add(FirstKey);
            int32 Previous = Start;
            int32 Current = First;
            while (true)
            {
                int32 Next = INDEX_NONE;
                for (const int32 Candidate : Adjacency[Current])
                {
                    if (Candidate != Previous) { Next = Candidate; break; }
                }
                if (Next == INDEX_NONE) { break; }
                const uint64 EdgeKey = MazeGraphEdgeKey(Current, Next);
                if (UsedRunEdges.Contains(EdgeKey)) { break; }
                UsedRunEdges.Add(EdgeKey);
                ++Length;
                Previous = Current;
                Current = Next;
            }
            Out.RunLengths.Add(Length);
        }
        return Out;
    }

    FString DescribeMazeDistribution(const TArray<int64>& DegreeCounts,
                                     const TArray<int32>& RunLengths)
    {
        int64 TotalDegree = 0;
        int64 DegreeSum = 0;
        for (int32 Degree = 0; Degree < DegreeCounts.Num(); ++Degree)
        {
            TotalDegree += DegreeCounts[Degree];
            DegreeSum += Degree * DegreeCounts[Degree];
        }

        TArray<int32> SortedRuns = RunLengths;
        SortedRuns.Sort();
        const int32 Median = SortedRuns.Num() > 0 ? SortedRuns[SortedRuns.Num() / 2] : 0;
        const int32 P90 = SortedRuns.Num() > 0
            ? SortedRuns[FMath::Min(SortedRuns.Num() - 1,
                                    FMath::FloorToInt(SortedRuns.Num() * 0.90f))]
            : 0;
        int32 MaxRun = 0;
        for (const int32 Run : SortedRuns) { MaxRun = FMath::Max(MaxRun, Run); }

        FString DegreeText;
        for (int32 Degree = 0; Degree < DegreeCounts.Num(); ++Degree)
        {
            if (Degree > 0) { DegreeText += TEXT(", "); }
            DegreeText += FString::Printf(TEXT("d%d=%lld"), Degree,
                                          static_cast<long long>(DegreeCounts[Degree]));
        }
        return FString::Printf(
            TEXT("degrees{%s}, mean=%.3f, dead_end_fraction=%.3f; runs n=%d min=%d "
                 "median=%d p90=%d max=%d"),
            *DegreeText,
            TotalDegree > 0 ? static_cast<double>(DegreeSum) / static_cast<double>(TotalDegree) : 0.0,
            TotalDegree > 0 ? static_cast<double>(DegreeCounts.IsValidIndex(1) ? DegreeCounts[1] : 0)
                                / static_cast<double>(TotalDegree) : 0.0,
            SortedRuns.Num(), SortedRuns.Num() > 0 ? SortedRuns[0] : 0,
            Median, P90, MaxRun);
    }

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

    // La pile suppose une strate valide ; `GetDensityAt` rend de l'air sans pile sur une strate
    // dégénérée. / The stack assumes a valid strate; GetDensityAt returns air for a degenerate one.
    if (MazeParams.StrateTopWorldZ - MazeParams.StrateBottomWorldZ <= 0.0f)
    {
        AddError(FString::Printf(
            TEXT("The Maze strate has degenerate Z bounds (top %.1f, bottom %.1f); GetDensityAt ")
            TEXT("builds no stack for it. The op stack has no degenerate-strate early-out by design."),
            MazeParams.StrateTopWorldZ, MazeParams.StrateBottomWorldZ));
        return false;
    }

    //==========================================================================
    // TOPOLOGY — construction proof, determinism, and maze morphology
    //==========================================================================
    {
        constexpr int32 NumTopologySeeds = 64;
        constexpr int32 GraphRadius = 10;
        constexpr int32 MetricRadius = 7;

        FMazeGenerationParams TreeParams = MazeParams;
        TreeParams.BranchProbability = 0.0f;
        TreeParams.Verticality = 0.0f;

        int32 TreeEdgeCountViolations = 0;
        int32 TreeDisconnected = 0;
        int32 MazeDisconnected = 0;
        int32 DeterminismFailures = 0;
        int64 OptionalEdges = 0;
        TArray<int64> DegreeCounts;
        DegreeCounts.Init(0, 8);
        TArray<int32> RunLengths;

        for (int32 Seed = 0; Seed < NumTopologySeeds; ++Seed)
        {
            const FMazeGraphAudit Tree = AuditMazeGraph(
                TreeParams, Seed, GraphRadius, MetricRadius);
            const FMazeGraphAudit Maze = AuditMazeGraph(
                MazeParams, Seed, GraphRadius, MetricRadius);
            const FMazeGraphAudit Repeat = AuditMazeGraph(
                MazeParams, Seed, GraphRadius, MetricRadius);

            if (Tree.NumEdges != Tree.NumNodes - 1) { ++TreeEdgeCountViolations; }
            TreeDisconnected += Tree.NumDisconnected;
            MazeDisconnected += Maze.NumDisconnected;
            OptionalEdges += static_cast<int64>(Maze.NumEdges - (Maze.NumNodes - 1));
            if (Maze.EdgeKeys != Repeat.EdgeKeys) { ++DeterminismFailures; }
            for (int32 Degree = 0; Degree < DegreeCounts.Num(); ++Degree)
            {
                DegreeCounts[Degree] += Maze.DegreeCounts[Degree];
            }
            RunLengths.Append(Maze.RunLengths);
        }

        TestEqual(TEXT("origin-directed parent graph has exactly N-1 edges with loops disabled"),
                  TreeEdgeCountViolations, 0);
        TestEqual(TEXT("origin-directed parent graph has zero disconnected cells"),
                  TreeDisconnected, 0);
        TestEqual(TEXT("Maze corridor graph has zero disconnected cells across 64 seeds"),
                  MazeDisconnected, 0);
        TestEqual(TEXT("Maze topology is deterministic for repeated same-seed builds"),
                  DeterminismFailures, 0);

        int64 TotalDegree = 0;
        int64 DegreeSum = 0;
        for (int32 Degree = 0; Degree < DegreeCounts.Num(); ++Degree)
        {
            TotalDegree += DegreeCounts[Degree];
            DegreeSum += static_cast<int64>(Degree) * DegreeCounts[Degree];
        }
        TestTrue(TEXT("Maze topology has actual degree-1 dead ends"),
                 DegreeCounts.IsValidIndex(1) && DegreeCounts[1] > 0);
        TestTrue(TEXT("Maze topology has varied corridor run lengths"),
                 RunLengths.Num() > 0 && RunLengths[0] >= 1
                     && [&RunLengths]()
                     {
                         int32 MinRun = MAX_int32, MaxRun = 0;
                         for (const int32 Run : RunLengths)
                         {
                             MinRun = FMath::Min(MinRun, Run);
                             MaxRun = FMath::Max(MaxRun, Run);
                         }
                         return MinRun < MaxRun;
                     }());
        TestTrue(TEXT("Maze mean junction degree stays below a cubic grid's degree 6"),
                 TotalDegree > 0
                     && static_cast<double>(DegreeSum) / static_cast<double>(TotalDegree) < 6.0);

        AddInfo(FString::Printf(
            TEXT("Maze topology: %d seeds, centered graph radius %d (%d nodes); tree edges "
                 "N-1 violations=%d, tree disconnected=%d, Maze disconnected=%d, optional "
                 "loop edges=%lld; %s; cubic-grid control: degree6=100.000%%, degree1=0.000%%, "
                 "mean=6.000 (no dead ends)."),
            NumTopologySeeds, GraphRadius,
            (GraphRadius * 2 + 1) * (GraphRadius * 2 + 1) * (GraphRadius * 2 + 1),
            TreeEdgeCountViolations, TreeDisconnected, MazeDisconnected,
            static_cast<long long>(OptionalEdges),
            *DescribeMazeDistribution(DegreeCounts, RunLengths)));
    }

    const UVoxelGenerator* Gen = World.Generator.Get();

    FVoxelOpStack Stack;
    VoxelDensityOps::BuildMazeStack(Stack, MazeParams, World.Settings->Seed,
                                    Gen->OriginSpineRadius, World.StrateManager.Get());

    // La décomposition doit être une DÉCOMPOSITION. Un `FMazeOp` monolithique passerait tous les
    // tests numériques ci-dessous et aurait pourtant raté l'objet entier du refactor (§2.5).
    TestEqual(TEXT("the Maze stack is decomposed, not wrapped (rock + corridors + roughness + carve + 4 structural)"),
              Stack.Num(), 8);

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

    //==========================================================================
    // CACHE COST — topology decisions belong to the rebuild, not the voxel loop
    //==========================================================================
    {
        constexpr int32 NumRebuildSamples = 256;
        constexpr int32 NumHotSamples = 20000;
        const float PerfCellSize = FMath::Max(MazeParams.CellSize, 1.0f);
        const float PerfZ = 0.5f * (MazeParams.StrateBottomWorldZ + MazeParams.StrateTopWorldZ);
        volatile float Sink = 0.0f;

        float LastX = 0.0f;
        float LastY = 0.0f;
        const double RebuildStart = FPlatformTime::Seconds();
        for (int32 i = 0; i < NumRebuildSamples; ++i)
        {
            const int32 CellX = (i % 16) - 8;
            const int32 CellY = (i / 16) - 8;
            LastX = (static_cast<float>(CellX) + 0.37f) * PerfCellSize;
            LastY = (static_cast<float>(CellY) + 0.61f) * PerfCellSize;
            Sink += Stack.EvalMC(LastX, LastY, PerfZ);
        }
        const double RebuildSeconds = FPlatformTime::Seconds() - RebuildStart;

        const double HotStart = FPlatformTime::Seconds();
        for (int32 i = 0; i < NumHotSamples; ++i)
        {
            Sink += Stack.EvalMC(LastX, LastY, PerfZ);
        }
        const double HotSeconds = FPlatformTime::Seconds() - HotStart;

        AddInfo(FString::Printf(
            TEXT("Maze cache perf: %d forced cell rebuilds at %.3f us/call and %d hot calls "
                 "at %.3f us/call (8 child nodes + capped loop rolls only on rebuild; capsule "
                 "SDFs only on hot calls; sink=%.9g)."),
            NumRebuildSamples,
            RebuildSeconds * 1.0e6 / static_cast<double>(NumRebuildSamples),
            NumHotSamples,
            HotSeconds * 1.0e6 / static_cast<double>(NumHotSamples),
            static_cast<float>(Sink)));
    }

    //=========================================================================
    // LE VERDICT DE BOÎTE — le prix perf de l'intervalle SDF conservatif.
    //=========================================================================
    // Pour un slot de cave, ClassifyTile plie `ClassifyBox` de cette pile : tout nombre > 0 ici est
    // une tuile que le mesher saute. / For a cave slot ClassifyTile folds this stack's ClassifyBox:
    // every proved box here is a tile the mesher skips.
    {
        int32 NumProved = 0, NumMixed = 0, NumUnsound = 0, NumBruteSamples = 0;
        FRandomStream Rng(24680);
        // Hors de la boucle : la ligne de rapport en a besoin. Une étendue d'échantillonnage qu'on
        // ne peut pas citer dans le rapport est une étendue que personne ne surveille.
        const int32 SpanCells  = 40;
        const int32 SpanVoxels = SpanCells * 8;   // Extent = Step * Cells = 1 * 8

        for (int32 t = 0; t < NumMazeBoxTests; ++t)
        {
            const int32 Step = 1, Cells = 8;                  // petites tuiles : force brute tenable
            const int32 Extent = Step * Cells;
            const FIntVector Origin(
                Rng.RandRange(-SpanCells, SpanCells) * Extent,
                Rng.RandRange(-SpanCells, SpanCells) * Extent,
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
                ++NumBruteSamples;
                if (bClaimsSolid ? (D >= 0.0f) : (D < 0.0f))
                {
                    if (NumUnsound == 0)
                    {
                        AddError(FString::Printf(
                            TEXT("HOLE: the op stack claimed %s for the box at (%d,%d,%d) but ")
                            TEXT("EvalMC(%.0f, %.0f, %.0f) = %.6g is on the %s side. One of the ops' ")
                            TEXT("EffectOverBox/ClassifyBox is not conservative. Suspects, in order: ")
                            TEXT("the source interval, the SDF roughness interval, the converter's ")
                            TEXT("threshold fold, then the seal's forcing verdict."),
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
            TEXT("Box verdicts over %d Maze tiles (XY sampled from +/- %d voxels = %.1f x ")
            TEXT("CellSize %.0f): %d proved uniform, %d Mixed, %d voxels checked, %d violations."),
            NumMazeBoxTests, SpanVoxels, (float)SpanVoxels / FMath::Max(MazeParams.CellSize, 1.0f), MazeParams.CellSize,
            NumProved, NumMixed, NumBruteSamples, NumUnsound));

        if (NumProved == 0)
        {
            AddWarning(TEXT("The stack proved no tile uniform, so ClassifyTile skips no Maze tile. ")
                       TEXT("Not a correctness problem, but a perf regression."));
        }
    }

    //==========================================================================
    // ISOLATION — a source with a converter it was never authored with
    //==========================================================================
    // Maze's hand-written stack uses ConstantRock -> Lattice -> SdfRoughness -> SdfCarve.
    // This stack deliberately uses a VOID root and SdfFill instead.  The only way it may
    // claim AllAir is for the propagated lattice/roughness interval to prove that the fill
    // converter is inactive over the whole box.  A source that still answered for its old
    // carve neighbour would not be a valid implementation of this test.
    {
        FMazeGenerationParams NovelParams = MazeParams;
        NovelParams.BranchProbability = 0.12f;
        NovelParams.Verticality = 0.08f;
        NovelParams.CorridorRadius = 1.0f;

        FVoxelOpStack NovelStack;
        NovelStack.Add(VoxelDensityOps::MakeConstantVoidSource(NovelParams.BaseDensity));
        NovelStack.Add(VoxelDensityOps::MakeLatticeCorridorSource(NovelParams, World.Settings->Seed));
        NovelStack.Add(VoxelDensityOps::MakeSdfRoughnessMod(
            0.35f, 0.12f, 3, NovelParams.CorridorRadius + 2.0f));
        NovelStack.Add(VoxelDensityOps::MakeSdfFill(1.25f, NovelParams.BaseDensity));
        NovelStack.PrepareChunk(Ctx);

        FString ChannelError;
        TestTrue(TEXT("the unverified source/converter stack has a valid channel order"),
                 NovelStack.ValidateChannelOrder(&ChannelError));
        if (!ChannelError.IsEmpty()) { AddError(ChannelError); }

        constexpr int32 NumNovelBoxTests = 256;
        const int32 NovelSpanCells = 160;
        const int32 NovelSpanVoxels = NovelSpanCells * 8;
        int32 NumNovelSolid = 0, NumNovelAir = 0, NumNovelMixed = 0;
        int32 NumNovelVoxels = 0, NumNovelViolations = 0;
        FRandomStream NovelRng(86420);

        for (int32 t = 0; t < NumNovelBoxTests; ++t)
        {
            const int32 Step = 1, Cells = 8;
            const int32 Extent = Step * Cells;
            const FIntVector Origin(
                NovelRng.RandRange(-NovelSpanCells, NovelSpanCells) * Extent,
                NovelRng.RandRange(-NovelSpanCells, NovelSpanCells) * Extent,
                FMath::Clamp(NovelRng.RandRange(BottomVoxelZ / Extent, TopVoxelZ / Extent), -4096, 4096) * Extent);
            const int32 GridDim = Cells + 1;
            const FBox Box(
                FVector(Origin.X - Step, Origin.Y - Step, Origin.Z - Step),
                FVector(Origin.X + GridDim * Step, Origin.Y + GridDim * Step, Origin.Z + GridDim * Step));

            const EVoxelTileClass Verdict = NovelStack.ClassifyBox(Box, Ctx);
            if (Verdict == EVoxelTileClass::Mixed)
            {
                ++NumNovelMixed;
                continue;
            }

            const bool bClaimsSolid = (Verdict == EVoxelTileClass::AllSolid);
            if (bClaimsSolid) { ++NumNovelSolid; } else { ++NumNovelAir; }

            // No sampled subset: every lattice point in every proved box is checked.
            for (int32 gz = -1; gz <= GridDim; ++gz)
            for (int32 gy = -1; gy <= GridDim; ++gy)
            for (int32 gx = -1; gx <= GridDim; ++gx)
            {
                const float X = (float)(Origin.X + gx * Step);
                const float Y = (float)(Origin.Y + gy * Step);
                const float Z = (float)(Origin.Z + gz * Step);
                const float D = NovelStack.EvalMC(X, Y, Z);
                ++NumNovelVoxels;
                if (bClaimsSolid ? (D >= 0.0f) : (D < 0.0f)) { ++NumNovelViolations; }
            }
        }

        TestEqual(TEXT("the unverified source/converter stack has zero box-verdict violations"),
                  NumNovelViolations, 0);
        AddInfo(FString::Printf(
            TEXT("Novel lattice+roughness+FILL stack: %d boxes proved (%d AllSolid, %d AllAir), " )
            TEXT("%d Mixed; %d voxels checked, %d violations; XY sampled from +/- %d voxels."),
            NumNovelSolid + NumNovelAir, NumNovelSolid, NumNovelAir, NumNovelMixed,
            NumNovelVoxels, NumNovelViolations, NovelSpanVoxels));
    }

    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
