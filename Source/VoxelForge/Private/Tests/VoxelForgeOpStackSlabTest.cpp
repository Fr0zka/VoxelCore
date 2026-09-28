// VoxelForgeOpStackSlabTest.cpp
// La pile Slab, sur LES DEUX archétypes / The Slab operator stack, on BOTH archetypes.
//
// CE QUE CE TEST DOIT PROUVER / WHAT THIS TEST HAS TO PROVE
//
//   1. LE CHEMIN DU JEU — la pile rend une densité finie, invariante à la fenêtre (ordre et
//      threads), et ses verdicts de boîte sont conservatifs.
//      The stack's density is finite and window-invariant, and its box verdicts are conservative.
//   2. UN OPÉRATEUR, DEUX ARCHÉTYPES — la MÊME pile est vérifiée pour FlatPlain ET CrystalChamber.
//      ⚠️ La fixture ne règle que `GeneratorType`, donc les deux slots portent des params PAR
//      DÉFAUT : à eux seuls ils exécutent la même configuration à deux profondeurs. C'est la
//      TROISIÈME passe (`CrystalChamber(tuned)`, `CeilingRoughness` 6 → 20) qui fait réellement
//      varier ce qui distingue les deux archétypes — et qui sert en même temps de pire cas aux
//      bornes d'amplitude de `ClassifyBox`. Voir le bloc en bas de fichier.
//   3. LE VERDICT DE BOÎTE — les deux surfaces sont XY-PURES, donc leurs bornes en Z sont connues
//      exactement (contrat [-1,1] de FBM) : toute tuile entièrement sous le sol ou entre les deux
//      bandes se prouve SANS échantillonner.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Async/ParallelFor.h"
#include "HAL/PlatformMisc.h"

#include "VoxelForgeTestFixture.h"
#include "VoxelDensityOpStack.h"

#include <atomic>

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FVoxelForgeOpStackSlabTest,
    "VoxelForge.OpStack.SlabEquivalence",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

namespace
{
    constexpr int32 NumSlabSamples = 20000;
    constexpr int32 NumSlabTiles   = 60;
}

bool FVoxelForgeOpStackSlabTest::RunTest(const FString& Parameters)
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

    //=========================================================================
    // LA BATTERIE, PARAMÉTRÉE PAR ARCHÉTYPE
    //=========================================================================
    // Exécutée à l'identique sur FlatPlain et CrystalChamber. Si les deux passent avec la MÊME
    // pile et la MÊME fabrique, la fusion des deux archétypes est démontrée plutôt qu'affirmée.
    auto RunBattery = [&](const FSlabGenerationParams& SlabParams,
                          int32 TopVoxelZ, int32 BottomVoxelZ,
                          int32 SlotIndex, const TCHAR* SlotName)
    {
        // La pile suppose une strate valide ; `GetDensityAt` rend de l'air sans pile sur une strate
        // dégénérée. / The stack assumes a valid strate; GetDensityAt returns air for a degenerate one.
        if (SlabParams.StrateTopWorldZ - SlabParams.StrateBottomWorldZ <= 0.0f)
        {
            AddError(FString::Printf(
                TEXT("%s has degenerate Z bounds (top %.1f, bottom %.1f); GetDensityAt builds no ")
                TEXT("stack for it. The op stack has no degenerate-strate early-out by design."),
                SlotName, SlabParams.StrateTopWorldZ, SlabParams.StrateBottomWorldZ));
            return;
        }

        FVoxelOpStack Stack;
        VoxelDensityOps::BuildSlabStack(Stack, SlabParams, World.Settings->Seed,
                                        Gen->OriginSpineRadius, World.StrateManager.Get());

        // La décomposition doit rester une DÉCOMPOSITION : vide + colonnes + 4 structurels.
        TestEqual(*FString::Printf(TEXT("%s decomposes into void + columns + 4 structural"), SlotName),
                  Stack.Num(), 6);

        FVoxelOpContext Ctx;
        Ctx.Seed               = (uint32)World.Settings->Seed;
        Ctx.LayoutVersion      = World.StrateManager->GetLayoutVersion();
        Ctx.StrateTopWorldZ    = SlabParams.StrateTopWorldZ;
        Ctx.StrateBottomWorldZ = SlabParams.StrateBottomWorldZ;
        Stack.PrepareChunk(Ctx);

        TArray<FVector> Points;
        Points.Reserve(NumSlabSamples);
        {
            FRandomStream Rng(31337 + SlotIndex);
            for (int32 i = 0; i < NumSlabSamples; ++i)
            {
                Points.Add(FVector(
                    (float)Rng.RandRange(-3 * CHUNK_SIZE, 3 * CHUNK_SIZE),
                    (float)Rng.RandRange(-3 * CHUNK_SIZE, 3 * CHUNK_SIZE),
                    (float)Rng.RandRange(BottomVoxelZ, TopVoxelZ)));
            }
        }

        //=====================================================================
        // 1. DENSITÉ FINIE / FINITE DENSITY
        //=====================================================================
        int32 NumNonFinite = 0;
        for (int32 i = 0; i < NumSlabSamples; ++i)
        {
            if (!FMath::IsFinite(Stack.EvalMC((float)Points[i].X, (float)Points[i].Y, (float)Points[i].Z)))
            {
                ++NumNonFinite;
            }
        }
        TestEqual(*FString::Printf(TEXT("%s: current stack has no non-finite density"), SlotName),
                  NumNonFinite, 0);

        //=====================================================================
        // 2. INVARIANCE DE FENÊTRE
        //=====================================================================
        // Le cache 3×3 des colonnes est `thread_local` et sa clé n'est PAS le chunk mais le jeu de
        // params + le seed. Si cette clé est incomplète, la couture apparaît ici.
        {
            std::atomic<int32> Impure{ 0 };
            const int32 NumBlocks = FMath::Max(4, FMath::Min(16, FPlatformMisc::NumberOfCores()));

            TArray<float> Ref;
            Ref.SetNumUninitialized(NumSlabSamples);
            for (int32 i = 0; i < NumSlabSamples; ++i)
            {
                Ref[i] = Stack.EvalMC((float)Points[i].X, (float)Points[i].Y, (float)Points[i].Z);
            }

            ParallelFor(NumBlocks, [&](int32 Block)
            {
                TArray<int32> LocalOrder;
                BuildShuffledOrder(NumSlabSamples, 700 + Block + SlotIndex * 32, LocalOrder);
                for (const int32 i : LocalOrder)
                {
                    const float V = Stack.EvalMC((float)Points[i].X, (float)Points[i].Y, (float)Points[i].Z);
                    if (!BitEqual(V, Ref[i])) { Impure.fetch_add(1, std::memory_order_relaxed); }
                }
            });

            TestEqual(*FString::Printf(
                          TEXT("%s: the op stack is window-invariant across order and threads"), SlotName),
                      Impure.load(), 0);
        }

        //=====================================================================
        // 3. LE VERDICT DE BOÎTE — ce que §3.1 a acheté
        //=====================================================================
        {
            int32 NumProved = 0, NumMixed = 0, NumUnsound = 0, NumBruteSamples = 0;
            FRandomStream Rng(24680 + SlotIndex);
            // Hors de la boucle : la ligne de rapport en a besoin. Une étendue d'échantillonnage qu'on
            // ne peut pas citer dans le rapport est une étendue que personne ne surveille.
            const int32 SpanCells  = 60;
            const int32 SpanVoxels = SpanCells * 8;   // Extent = Step * Cells = 1 * 8

            for (int32 t = 0; t < NumSlabTiles; ++t)
            {
                const int32 Step = 1, Cells = 8;
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
                                TEXT("HOLE: %s claimed %s for the box at (%d,%d,%d) but ")
                                TEXT("EvalMC(%.0f, %.0f, %.0f) = %.6g is on the %s side. One of the ")
                                TEXT("ops is not conservative. Suspects, in order: the slab source's ")
                                TEXT("noise amplitude bounds (the proved FBM supremum is 1.5, not 1.0), the ")
                                TEXT("ceiling clamp raising CeilSurface above CeilZ, then the column ")
                                TEXT("mod's reach (MaxRadius + blend)."),
                                SlotName, bClaimsSolid ? TEXT("AllSolid") : TEXT("AllAir"),
                                Origin.X, Origin.Y, Origin.Z, X, Y, Z, D,
                                (D >= 0.0f) ? TEXT("AIR") : TEXT("SOLID")));
                        }
                        ++NumUnsound;
                        gz = gy = gx = GridDim + 1;
                    }
                }
            }

            TestEqual(*FString::Printf(
                          TEXT("%s: every box verdict survives brute force (a false verdict is a hole)"),
                          SlotName),
                      NumUnsound, 0);

            AddInfo(FString::Printf(
                TEXT("%s box verdicts over %d tiles (XY sampled from +/- %d voxels = %.1f x ")
                TEXT("ColumnSpacing %.0f): %d proved uniform, %d Mixed, %d voxels checked, %d violations. ")
                TEXT("This number is the whole point of making the slab surfaces XY-pure ")
                TEXT("(OPSTACK-DECOMPOSITION 3.1)."),
                SlotName, NumSlabTiles, SpanVoxels,
                (float)SpanVoxels / FMath::Max(SlabParams.ColumnSpacing, 1.0f), SlabParams.ColumnSpacing,
                NumProved, NumMixed, NumBruteSamples, NumUnsound));

            if (NumProved == 0)
            {
                AddWarning(FString::Printf(
                    TEXT("%s proved no tile uniform. Not a correctness problem, but the entire perf ")
                    TEXT("case for dropping the Z term rests on this number being well above zero -- ")
                    TEXT("a slab is mostly solid rock below the floor. Check that the sampled tile Z ")
                    TEXT("range actually reaches below FloorZ - FloorAmp."), SlotName));
            }
        }
    };

    //=========================================================================
    // LES TROIS PASSES
    //=========================================================================
    auto ResolveSlot = [&](int32 SlotIndex, const TCHAR* SlotName,
                           FSlabGenerationParams& OutParams, int32& OutTop, int32& OutBottom) -> bool
    {
        if (!World.GetSlotVoxelZRange(SlotIndex, OutTop, OutBottom))
        {
            AddError(FString::Printf(
                TEXT("The fixture layout has no %s slot. Check FTestWorld::Build's Archetypes[] ")
                TEXT("against FTestWorld::Slot%s."), SlotName, SlotName));
            return false;
        }
        const int32 MidChunkZ = ((OutTop + OutBottom) / 2) / CHUNK_SIZE;
        OutParams = World.StrateManager->GetSlabParamsForChunk(FIntVector(0, 0, MidChunkZ));
        return true;
    };

    FSlabGenerationParams FlatParams, CrystalParams;
    int32 FlatTop = 0, FlatBottom = 0, CrystalTop = 0, CrystalBottom = 0;

    if (ResolveSlot(FTestWorld::SlotFlatPlain, TEXT("FlatPlain"), FlatParams, FlatTop, FlatBottom))
    {
        RunBattery(FlatParams, FlatTop, FlatBottom, FTestWorld::SlotFlatPlain, TEXT("FlatPlain"));
    }

    if (ResolveSlot(FTestWorld::SlotCrystalChamber, TEXT("CrystalChamber"),
                    CrystalParams, CrystalTop, CrystalBottom))
    {
        RunBattery(CrystalParams, CrystalTop, CrystalBottom,
                   FTestWorld::SlotCrystalChamber, TEXT("CrystalChamber"));

        //=====================================================================
        // LA PASSE QUI FAIT VRAIMENT LA DÉMONSTRATION
        //=====================================================================
        // ⚠️ La fixture ne règle QUE `GeneratorType` : FlatPlain et CrystalChamber y reçoivent des
        // `FSlabGenerationParams` PAR DÉFAUT, donc identiques. Les deux passes ci-dessus exécutent
        // en réalité la même configuration à deux profondeurs — ce qui est un test utile, mais qui
        // ne démontre PAS « un opérateur, deux jeux de défauts » : `CeilingRoughness`, la seule
        // chose qui distingue réellement CrystalChamber, n'y varie jamais.
        //
        // Cette passe-ci fait varier ce qui compte, et elle est aussi le PIRE CAS pour les bornes
        // d'amplitude de `ClassifyBox` : un `CeilingRoughness` élevé élargit la bande du plafond et
        // rend le clamp `Max(CeilZ - bruit, FloorSurface + 2)` beaucoup plus susceptible de mordre.
        // Si un verdict de boîte est faux quelque part, c'est ici qu'il apparaît.
        //
        // The fixture only sets GeneratorType, so both slots get DEFAULT slab params — the two
        // passes above are the same configuration at two depths. This pass varies what actually
        // distinguishes CrystalChamber, and is simultaneously the worst case for the ClassifyBox
        // amplitude bounds: a large CeilingRoughness widens the ceiling band and makes the
        // FloorSurface + 2 clamp far more likely to bind.
        FSlabGenerationParams Tuned = CrystalParams;
        Tuned.CeilingRoughness         = 20.0f;   // vs 6.0 par défaut — de vraies stalactites
        Tuned.CeilingRoughnessFrequency = 0.09f;
        Tuned.FloorRoughness           = 9.0f;
        Tuned.ColumnDensity            = 0.25f;   // beaucoup plus de colonnes ⇒ FillOnly plus souvent
        Tuned.ColumnMaxRadius          = 11.0f;

        RunBattery(Tuned, CrystalTop, CrystalBottom, 64, TEXT("CrystalChamber(tuned)"));
    }

    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
