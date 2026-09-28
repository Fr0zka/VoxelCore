// VoxelForgeOpStackIslandTest.cpp
// FloatingIslands — la pile qui tourne À L'ENVERS / the stack that runs BACKWARDS.
//
// Les autres archétypes partent de ROC et CREUSENT ; FloatingIslands part du VIDE et REMPLIT, avec
// les MÊMES opérateurs au signe près (convention interne positif = solide) :
//
//     FConstantFieldSource(+Base)  ←→  FConstantFieldSource(-Base)
//     FSdfConvertOp(Sign = -1)     ←→  FSdfConvertOp(Sign = +1)
//
// Le seul opérateur propre à l'archétype est le blob d'île. / The only archetype-specific op is the
// island blob; the rest is Maze's rock + carve with the opposite sign.
//
// ET LE VERDICT DE BOÎTE : une strate d'îles flottantes est surtout vide, donc `ClassifyBox` doit
// savoir rendre **AllAir** (`OPSTACK-DECOMPOSITION §7`). Le test compte AllSolid et AllAir
// SÉPARÉMENT, parce qu'un total agrégé masquerait exactement ce verdict-là.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Async/ParallelFor.h"
#include "HAL/PlatformMisc.h"

#include "VoxelForgeTestFixture.h"
#include "VoxelDensityOpStack.h"

#include <atomic>

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FVoxelForgeOpStackIslandTest,
    "VoxelForge.OpStack.FloatingIslandEquivalence",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

namespace
{
    constexpr int32 NumIslandSamples = 20000;

    /**
     * Les défauts génèrent bien des îles, mais un test qui les prend tels quels laisse la question
     * « les échantillons sont-ils VRAIMENT tombés dedans ? » à la chance du seed. On force donc une
     * densité d'îles haute, et surtout un `TopFlatten < 1` — la branche du dôme de bord est le seul
     * endroit où `TopHalf` et `Edge²` interviennent, et elle est silencieusement morte à 1.0.
     * (Même piège que `WaterLevelRelative` et la fenêtre d'overhang : un paramètre au repos est un
     * opérateur non testé.)
     */
    void EnableIslandFeatures(FFloatingIslandParams& P)
    {
        P.IslandDensity     = 0.75f;   // des îles dans presque chaque cellule du 3×3
        P.TopFlatten        = 0.55f;   // < 1 ⇒ la branche du dôme de bord s'exécute
        P.SurfaceRoughness  = 4.0f;    // la rugosité SDF partagée avec Maze et VerticalShafts
        P.VerticalJitter    = 0.6f;    // des îles à des hauteurs différentes
        P.ThicknessRatio    = 0.7f;
    }
}

bool FVoxelForgeOpStackIslandTest::RunTest(const FString& Parameters)
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
    if (!World.GetSlotVoxelZRange(FTestWorld::SlotFloatingIsland, TopVoxelZ, BottomVoxelZ))
    {
        AddError(TEXT("The fixture layout has no FloatingIslands slot. Check FTestWorld::Build's ")
                 TEXT("Archetypes[] against FTestWorld::SlotFloatingIsland."));
        return false;
    }

    const int32 MidChunkZ = ((TopVoxelZ + BottomVoxelZ) / 2) / CHUNK_SIZE;
    FFloatingIslandParams P = World.StrateManager->GetFloatingIslandParamsForChunk(
        FIntVector(0, 0, MidChunkZ));

    if (P.StrateTopWorldZ - P.StrateBottomWorldZ <= 0.0f)
    {
        AddError(TEXT("The FloatingIslands strate has degenerate Z bounds; GetDensityAt builds no ")
                 TEXT("stack for it. The op stack has no degenerate-strate early-out by design."));
        return false;
    }

    EnableIslandFeatures(P);

    FVoxelOpStack Stack;
    VoxelDensityOps::BuildFloatingIslandStack(Stack, P, World.Settings->Seed,
                                              Gen->OriginSpineRadius, World.StrateManager.Get());

    // void + blobs + roughness + fill + 4 structurels.
    TestEqual(TEXT("the island stack is decomposed into 8 ops"), Stack.Num(), 8);

    FVoxelOpContext Ctx;
    Ctx.Seed               = (uint32)World.Settings->Seed;
    Ctx.LayoutVersion      = World.StrateManager->GetLayoutVersion();
    Ctx.StrateTopWorldZ    = P.StrateTopWorldZ;
    Ctx.StrateBottomWorldZ = P.StrateBottomWorldZ;
    Stack.PrepareChunk(Ctx);

    TArray<FVector> Points;
    Points.Reserve(NumIslandSamples);
    {
        FRandomStream Rng(60186);
        for (int32 i = 0; i < NumIslandSamples; ++i)
        {
            Points.Add(FVector(
                (float)Rng.RandRange(-3 * CHUNK_SIZE, 3 * CHUNK_SIZE),
                (float)Rng.RandRange(-3 * CHUNK_SIZE, 3 * CHUNK_SIZE),
                (float)Rng.RandRange(BottomVoxelZ, TopVoxelZ)));
        }
    }

    //=========================================================================
    // 1. LIVENESS — roche d'île et vide ouvert / island rock and open void
    //=========================================================================
    // On compte SÉPARÉMENT le solide d'intérieur et le solide de seal : sur cet archétype la
    // quasi-totalité du volume est de l'air, donc un « N solides » agrégé serait dominé par les
    // deux bandes de seal et ne dirait RIEN sur les îles elles-mêmes.
    const float InnerBot = P.StrateBottomWorldZ + P.BoundarySealThickness;
    const float InnerTop = P.StrateTopWorldZ    - P.BoundarySealThickness;

    int32 NumInsideIsland = 0, NumOpenVoid = 0;
    for (int32 i = 0; i < NumIslandSamples; ++i)
    {
        const float X = (float)Points[i].X, Y = (float)Points[i].Y, Z = (float)Points[i].Z;
        const float New = Stack.EvalMC(X, Y, Z);

        const bool bInterior = (Z > InnerBot && Z < InnerTop);
        if (bInterior && New < 0.0f)  { ++NumInsideIsland; }   // solide loin des seals ⇒ une île
        if (bInterior && New >= 0.0f) { ++NumOpenVoid; }
    }
    AddInfo(FString::Printf(
        TEXT("FloatingIslands: %d of %d samples inside island rock away from the seal bands, %d in ")
        TEXT("open void."),
        NumInsideIsland, NumIslandSamples, NumOpenVoid));

    if (NumInsideIsland == 0)
    {
        AddWarning(TEXT("No sample landed inside island rock away from the seal bands, so the blob ")
                   TEXT("source and the fill were never meaningfully exercised. Raise IslandDensity ")
                   TEXT("or IslandMaxRadius."));
    }

    //=========================================================================
    // 2. INVARIANCE DE FENÊTRE
    //=========================================================================
    // La source garde un cache 3×3 `thread_local` dont la clé est le jeu de params — et cette clé
    // inclut délibérément `BoundarySealThickness`, que `SpreadZ` lit (voir la note dans
    // FIslandBlobSource::GetCells).
    {
        std::atomic<int32> Impure{ 0 };
        const int32 NumBlocks = FMath::Max(4, FMath::Min(16, FPlatformMisc::NumberOfCores()));

        TArray<float> Ref;
        Ref.SetNumUninitialized(NumIslandSamples);
        for (int32 i = 0; i < NumIslandSamples; ++i)
        {
            Ref[i] = Stack.EvalMC((float)Points[i].X, (float)Points[i].Y, (float)Points[i].Z);
        }

        ParallelFor(NumBlocks, [&](int32 Block)
        {
            TArray<int32> LocalOrder;
            BuildShuffledOrder(NumIslandSamples, 3300 + Block, LocalOrder);
            for (const int32 i : LocalOrder)
            {
                const float V = Stack.EvalMC((float)Points[i].X, (float)Points[i].Y, (float)Points[i].Z);
                if (!BitEqual(V, Ref[i])) { Impure.fetch_add(1, std::memory_order_relaxed); }
            }
        });

        TestEqual(TEXT("the island stack is window-invariant across order and threads"),
                  Impure.load(), 0);
    }

    //=========================================================================
    // 3. LE VERDICT DE BOÎTE — et la première preuve « AllAir » du plugin
    //=========================================================================
    {
        int32 NumProvedSolid = 0, NumProvedAir = 0, NumMixed = 0, NumUnsound = 0, NumBruteSamples = 0;
        FRandomStream Rng(24680);
        // Hors de la boucle : la ligne de rapport en a besoin. Une étendue d'échantillonnage qu'on
        // ne peut pas citer dans le rapport est une étendue que personne ne surveille.
        const int32 SpanCells  = 95;
        const int32 SpanVoxels = SpanCells * 8;   // Extent = Step * Cells = 1 * 8

        for (int32 t = 0; t < 60; ++t)
        {
            const int32 Step = 1, Cells = 8;
            const int32 Extent = Step * Cells;
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

            const bool bClaimsSolid = (Verdict == EVoxelTileClass::AllSolid);
            if (bClaimsSolid) { ++NumProvedSolid; } else { ++NumProvedAir; }

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
                            TEXT("HOLE: the island stack claimed %s for the box at (%d,%d,%d) but ")
                            TEXT("EvalMC(%.0f, %.0f, %.0f) = %.6g is on the %s side. Suspects, in ")
                            TEXT("order: the blob source's propagated SDF interval (does it cover the WARP amplitude ")
                            TEXT("AND the roughness AND the fill blend AND the SmoothMin dip?), ")
                            TEXT("then the Z bound -- note there is NO lower bound, a thin thread ")
                            TEXT("of matter hangs below each island down the axis, so only the ")
                            TEXT("TOP may be used to reject."),
                            bClaimsSolid ? TEXT("AllSolid") : TEXT("AllAir"),
                            Origin.X, Origin.Y, Origin.Z, X, Y, Z, D,
                            (D >= 0.0f) ? TEXT("AIR") : TEXT("SOLID")));
                    }
                    ++NumUnsound;
                    gz = gy = gx = GridDim + 1;
                }
            }
        }

        TestEqual(TEXT("every box verdict the island stack emits survives brute force"), NumUnsound, 0);

        AddInfo(FString::Printf(
            TEXT("Box verdicts over 60 FloatingIslands tiles (XY sampled from +/- %d voxels = %.1f x ")
            TEXT("IslandSpacing %.0f): %d proved AllSolid, %d proved AllAir, ")
            TEXT("%d Mixed, %d voxels checked, %d violations. The AllAir count matters most: a ")
            TEXT("floating-island strate is mostly open air (OPSTACK-DECOMPOSITION 7)."),
            SpanVoxels, (float)SpanVoxels / FMath::Max(P.IslandSpacing, 1.0f), P.IslandSpacing,
            NumProvedSolid, NumProvedAir, NumMixed, NumBruteSamples, NumUnsound));

        if (NumProvedAir == 0)
        {
            AddWarning(TEXT("Zero tiles proved AllAir. The stack is still SOUND, but the whole perf ")
                       TEXT("argument for this archetype rests on that verdict, so it is worth ")
                       TEXT("knowing it did not fire. Most likely the blob source's Pad is so wide ")
                       TEXT("that every box finds an island within reach -- the same pessimism ")
                       TEXT("VerticalShafts has (0 of 60), for the same reason."));
        }
    }

    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
