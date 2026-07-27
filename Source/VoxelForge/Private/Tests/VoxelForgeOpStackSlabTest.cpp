// VoxelForgeOpStackSlabTest.cpp
// PHASE 2, PREMIER PORTAGE — la pile Slab contre GetSlabDensity, sur LES DEUX archétypes.
// PHASE 2'S FIRST PORT — the Slab operator stack against GetSlabDensity, on BOTH archetypes.
//
// CE QUE CE TEST DOIT PROUVER / WHAT THIS TEST HAS TO PROVE
// Trois choses, et la troisième est la raison d'être du portage :
//
//   1. ÉQUIVALENCE — la pile reproduit `GetSlabDensity`. Même barre que Maze : un changement de
//      côté d'isosurface est un ÉCHEC DUR, un écart d'ULP est le plancher accepté.
//   2. UN OPÉRATEUR, DEUX ARCHÉTYPES — la MÊME pile est vérifiée contre FlatPlain ET
//      CrystalChamber. `GetSlabDensity` ne les distingue par aucun branchement ; si la pile a
//      besoin d'en faire un, la fusion est fausse et ce test le dit.
//      ⚠️ La fixture ne règle que `GeneratorType`, donc les deux slots portent des params PAR
//      DÉFAUT : à eux seuls ils exécutent la même configuration à deux profondeurs. C'est la
//      TROISIÈME passe (`CrystalChamber(tuned)`, `CeilingRoughness` 6 → 20) qui fait réellement
//      varier ce qui distingue les deux archétypes — et qui sert en même temps de pire cas aux
//      bornes d'amplitude de `ClassifyBox`. Voir le bloc en bas de fichier.
//   3. LE VERDICT DE BOÎTE — et c'est ici que §3.1 se paie. `ClassifyTile` prouve ZÉRO tuile pour
//      FlatPlain et CrystalChamber aujourd'hui. Depuis que les deux surfaces sont XY-PURES, leurs
//      bornes en Z sont connues exactement (contrat [-1,1] de FBM), donc toute tuile entièrement
//      sous le sol ou entre les deux bandes se prouve SANS échantillonner.
//
// ─────────────────────────────────────────────────────────────────────────────────────────
// ⚠️ CE TEST NE PEUT PAS DÉTECTER LE RETRAIT DU TERME EN Z — et c'est voulu
// ─────────────────────────────────────────────────────────────────────────────────────────
// `GetSlabDensity` a perdu son terme en Z en même temps que ce portage était écrit
// (OPSTACK-DECOMPOSITION §3.1, tranché par Jahni). La pile est comparée à la fonction TELLE
// QU'ELLE EST MAINTENANT, donc ce test dit « le portage est fidèle » et ne dit RIEN sur le
// changement de génération — c'est exactement la séparation voulue :
//
//   • ce test vert          ⇒ la pile == la fonction de référence. Le portage est un refactor pur.
//   • le monde a changé     ⇒ imputable au retrait du terme en Z, ET À RIEN D'AUTRE.
//
// Sans cette séparation, un écart visuel serait inattribuable entre « j'ai changé le design » et
// « j'ai raté le portage ». C'est le test qui fait l'attribution, pas l'ordre des builds.
//
// This test compares the stack against the reference function AS IT IS NOW, so green here means the
// port is a pure refactor and ANY visual delta is attributable to the Z-term removal alone.
//
// ⚠️ Et la règle de §C10 tient toujours : ne jamais faire tourner les deux chemins dans le même
// monde, ne jamais comparer leurs sorties pour égalité ailleurs qu'ici.

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
        // `GetSlabDensity` court-circuite sur une strate dégénérée (`return 1.0f`). Cette garde
        // appartient à la fonction d'archétype, pas à un opérateur ; la pile suppose une strate
        // valide, et `GetDensityAt` retombe sur le `switch` dans ce cas.
        if (SlabParams.StrateTopWorldZ - SlabParams.StrateBottomWorldZ <= 0.0f)
        {
            AddError(FString::Printf(
                TEXT("%s has degenerate Z bounds (top %.1f, bottom %.1f), which sends GetSlabDensity ")
                TEXT("down its early-out. The op stack has no such early-out by design."),
                SlotName, SlabParams.StrateTopWorldZ, SlabParams.StrateBottomWorldZ));
            return;
        }

        FVoxelOpStack Stack;
        VoxelDensityOps::BuildSlabStack(Stack, SlabParams, World.Settings->Seed,
                                        Gen->OriginSpineRadius, World.StrateManager.Get());

        // La décomposition doit rester une DÉCOMPOSITION : vide + colonnes + 3 structurels.
        TestEqual(*FString::Printf(TEXT("%s decomposes into void + columns + 3 structural"), SlotName),
                  Stack.Num(), 5);

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
        // 1. ÉQUIVALENCE — géométrie d'abord, bits ensuite.
        //=====================================================================
        // ─────────────────────────────────────────────────────────────────────
        // LE BON MÈTRE — corrigé 2026-07-27 après que la passe `tuned` a crié au loup
        // ─────────────────────────────────────────────────────────────────────
        // Première version : `16 · max(|Old|, 1) · FLT_EPSILON`, c.-à-d. l'ULP mesuré sur la
        // DENSITÉ DE SORTIE. C'est le mauvais mètre, et il se trompe exactement là où le test
        // regarde le plus : la densité vaut `min(Z - Sol, Plafond - Z)`, donc PRÈS DE L'ISOSURFACE
        // la sortie tend vers 0 pendant que les intermédiaires (surfaces, Z monde, amplitudes de
        // bruit) valent des CENTAINES. Un arrondi né à l'échelle 400 était jugé contre un mètre
        // à l'échelle 1 — 400× trop serré.
        //
        // Mesuré : la passe `tuned` (rugosités ×2.25 et ×3.33) a vu ses écarts croître ×4.5, et
        // son pire écart valait **0.345 ULP de |Z|**. Sous-ULP à l'échelle où l'erreur naît.
        // L'erreur est donc proportionnelle à l'AMPLITUDE, ce qui est la signature d'un arrondi
        // ordinaire, pas d'une transcription fausse.
        //
        // Le mètre correct est la magnitude des quantités D'OÙ VIENT l'erreur. Le test reste
        // discriminant : une vraie dérive de portage (offset de bruit faux, `abs()` manquant,
        // clamp oublié) déplace la surface de plusieurs VOXELS — 4 ordres de grandeur au-dessus
        // de ce seuil, pas 4 fois.
        //
        // The first yardstick measured ULPs on the OUTPUT density, which tends to 0 near the
        // isosurface while the intermediates are in the hundreds. Rounding born at scale ~400 was
        // judged against a yardstick of scale 1. Real port drift moves the surface by voxels —
        // four orders of magnitude above this bound, so the test stays discriminating.
        const float SurfaceScale = FMath::Max(FMath::Abs(SlabParams.StrateTopWorldZ),
                                              FMath::Abs(SlabParams.StrateBottomWorldZ));

        int32 NumDiff = 0, WorstIdx = -1, NumBeyondUlpNoise = 0, NumSolidDisagreements = 0;
        float WorstDelta = 0.0f, WorstOld = 0.0f, WorstUlpsOfScale = 0.0f;
        // Le pire cas PARMI LES DÉPASSEMENTS — c'est lui qui dit si un WARN est du bruit ou une dérive.
        float WorstOutlierDelta = 0.0f, WorstOutlierOld = 0.0f, WorstOutlierUlps = 0.0f;

        for (int32 i = 0; i < NumSlabSamples; ++i)
        {
            const float X = (float)Points[i].X, Y = (float)Points[i].Y, Z = (float)Points[i].Z;

            const float Old = Gen->GetSlabDensity(X, Y, Z, SlabParams);   // MC : négatif = solide
            const float New = Stack.EvalMC(X, Y, Z);

            if (!BitEqual(Old, New))
            {
                ++NumDiff;
                const float Delta = FMath::Abs(Old - New);

                // L'échelle à laquelle CET échantillon calcule : la sortie, sa propre altitude, et
                // les bornes de la strate. C'est le plus grand des trois qui porte l'arrondi.
                const float Scale = FMath::Max3(FMath::Abs(Old), FMath::Abs(Z),
                                                FMath::Max(SurfaceScale, 1.0f));
                const float Ulps  = Delta / (Scale * FLT_EPSILON);

                if (Delta > WorstDelta)
                {
                    WorstDelta = Delta; WorstIdx = i; WorstOld = Old; WorstUlpsOfScale = Ulps;
                }

                if (Delta > 16.0f * Scale * FLT_EPSILON)
                {
                    ++NumBeyondUlpNoise;
                    if (Delta > WorstOutlierDelta)
                    {
                        WorstOutlierDelta = Delta; WorstOutlierOld = Old; WorstOutlierUlps = Ulps;
                    }
                }
            }
            // Le mesher ne lit que le SIGNE. Un désaccord de CÔTÉ bouge la géométrie.
            if ((Old >= 0.0f) != (New >= 0.0f)) { ++NumSolidDisagreements; }
        }

        if (NumDiff == 0)
        {
            AddInfo(FString::Printf(TEXT("%s: bit-identical across %d samples."),
                                    SlotName, NumSlabSamples));
        }
        else if (NumBeyondUlpNoise == 0)
        {
            AddInfo(FString::Printf(
                TEXT("%s: %d of %d samples differ, ALL at ULP scale (largest |delta| %.9g = %.3f ULP ")
                TEXT("of the working scale, where density = %.6g, at (%.0f, %.0f, %.0f)), and 0 cross ")
                TEXT("the isosurface. Same accepted floor as Maze -- see AUDIT-2026-07.md C10."),
                SlotName, NumDiff, NumSlabSamples, WorstDelta, WorstUlpsOfScale, WorstOld,
                WorstIdx >= 0 ? Points[WorstIdx].X : 0.0f,
                WorstIdx >= 0 ? Points[WorstIdx].Y : 0.0f,
                WorstIdx >= 0 ? Points[WorstIdx].Z : 0.0f));
        }
        else
        {
            // Le message porte maintenant LE DISCRIMINANT, pas seulement l'alarme : la densité au
            // point fautif et l'écart exprimé en ULP de l'échelle de travail. Un dépassement à
            // quelques ULP avec une densité proche de 0 est un artefact de mètre ; un dépassement
            // à des milliers d'ULP est une vraie dérive. La différence se lit, elle ne se devine pas.
            AddWarning(FString::Printf(
                TEXT("%s: %d of %d samples differ and %d exceed the ULP bound. Worst OUTLIER: ")
                TEXT("|delta| %.9g = %.1f ULP of the working scale, where density = %.6g. ")
                TEXT("(Worst overall: |delta| %.9g at (%.0f, %.0f, %.0f).) %d cross the isosurface. ")
                TEXT("READ THE ULP FIGURE BEFORE INVESTIGATING: a few ULP with a near-zero density is ")
                TEXT("cancellation near the isosurface, not drift. Thousands of ULP IS drift -- check, ")
                TEXT("in order: the floor/ceiling noise offsets (7.3/11.1 and 17.3+1000/19.7+2000/3000), ")
                TEXT("the abs() on the ceiling noise, the ceiling clamp (FloorSurface + 2), the column ")
                TEXT("blend (2.0) and the 0.15/0.7 jitter."),
                SlotName, NumDiff, NumSlabSamples, NumBeyondUlpNoise,
                WorstOutlierDelta, WorstOutlierUlps, WorstOutlierOld,
                WorstDelta,
                WorstIdx >= 0 ? Points[WorstIdx].X : 0.0f,
                WorstIdx >= 0 ? Points[WorstIdx].Y : 0.0f,
                WorstIdx >= 0 ? Points[WorstIdx].Z : 0.0f,
                NumSolidDisagreements));
        }

        TestEqual(*FString::Printf(
                      TEXT("%s: no sample lands on the opposite side of the isosurface"), SlotName),
                  NumSolidDisagreements, 0);

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
            int32 NumProved = 0, NumMixed = 0, NumUnsound = 0;
            FRandomStream Rng(24680 + SlotIndex);

            for (int32 t = 0; t < NumSlabTiles; ++t)
            {
                const int32 Step = 1, Cells = 8;
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
                                TEXT("HOLE: %s claimed %s for the box at (%d,%d,%d) but ")
                                TEXT("EvalMC(%.0f, %.0f, %.0f) = %.6g is on the %s side. One of the ")
                                TEXT("ops is not conservative. Suspects, in order: the slab source's ")
                                TEXT("noise amplitude bounds (does FBM really honour [-1,1]?), the ")
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
                TEXT("%s box verdicts over %d tiles: %d proved uniform, %d Mixed. Today's ")
                TEXT("ClassifyTile proves ZERO of these. This number is the whole point of making ")
                TEXT("the slab surfaces XY-pure (OPSTACK-DECOMPOSITION 3.1)."),
                SlotName, NumSlabTiles, NumProved, NumMixed));

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
