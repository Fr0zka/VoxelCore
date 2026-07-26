// VoxelForgeDensityPurityTest.cpp
// Phase 0.5 test #1 — LA PURETÉ DE LA DENSITÉ / density purity.
//
// L'INVARIANT / THE INVARIANT (ARCHITECTURE §8.4, "window invariance"):
//   GetDensityAt(x,y,z) est une fonction PURE de (coords monde, seed, layout). Le même point
//   interrogé depuis une autre tuile, un autre ordre de requêtes ou un autre thread doit rendre
//   le float BIT-IDENTIQUE. Pas "proche" — identique : un écart d'1 ULP entre deux fenêtres de
//   chunk est une COUTURE visible, et en multijoueur une divergence de monde.
//
//   GetDensityAt is a PURE function of (world coords, seed, layout). The same point queried from
//   a different tile, in a different order, or on a different thread must return the BIT-IDENTICAL
//   float. Not "close" — identical: a 1-ULP disagreement between two chunk windows is a visible
//   seam, and in multiplayer a world divergence.
//
// POURQUOI CE TEST EXISTE / WHY THIS TEST EXISTS:
//   ~30 caches thread_local à clé manuelle vivent sous GetDensityAt (CP_*, GSurfColCache, les
//   slots de diff, le cache SDF). Chacun est correct exactement tant que sa CLÉ contient toutes
//   les entrées dont dépend la valeur cachée. Une entrée oubliée ne casse rien tout de suite :
//   elle produit une mauvaise valeur seulement quand le cache est chaud pour une AUTRE entrée —
//   c'est-à-dire de façon intermittente, dépendante de l'ordre, et invisible en jeu jusqu'à ce
//   qu'un joueur trouve la couture. C'est exactement ainsi que AUDIT C2 s'est caché.
//
//   ~30 hand-keyed thread_local caches live under GetDensityAt. Each is correct exactly as long as
//   its KEY contains every input the cached value depends on. A forgotten input breaks nothing
//   immediately: it yields a wrong value only when the cache is warm for a DIFFERENT input — i.e.
//   intermittently, order-dependently, invisible in play until a player finds the seam. That is
//   precisely how AUDIT C2 stayed hidden.
//
//   AVoxelWorld::ValidateDeterminism existe déjà mais tourne sur le GAME THREAD uniquement : il ne
//   peut structurellement pas voir une divergence de cache worker. Ce test tourne multi-thread.
//   AVoxelWorld::ValidateDeterminism already exists but runs on the GAME THREAD only: it
//   structurally cannot see a worker-cache divergence. This test runs multi-threaded.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Async/ParallelFor.h"
#include "HAL/PlatformMisc.h"

#include "VoxelForgeTestFixture.h"

#include <atomic>

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FVoxelForgeDensityPurityTest,
    "VoxelForge.Determinism.DensityPurity",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

namespace
{
    // Assez de points pour traverser plusieurs chunks/strates et faire tourner tous les caches,
    // assez peu pour rester sous la seconde. / Enough points to cross many chunks and strates and
    // churn every cache, few enough to stay under a second.
    constexpr int32 NumSamples = 10000;

    struct FMismatch
    {
        std::atomic<int32> Count{ 0 };
        std::atomic<int32> FirstIndex{ -1 };

        void Record(int32 Index)
        {
            Count.fetch_add(1, std::memory_order_relaxed);
            int32 Expected = -1;
            FirstIndex.compare_exchange_strong(Expected, Index, std::memory_order_relaxed);
        }
    };

    /** Report the first divergent point with both floats and their raw bits — a mismatch that is
     *  invisible in decimal (a 1-ULP cache seam) is the exact case this test is for. */
    FString DescribeMismatch(const FVector& P, float Ref, float Got)
    {
        return FString::Printf(
            TEXT("at (%.0f, %.0f, %.0f): reference %.9g [0x%08X] vs re-sample %.9g [0x%08X]"),
            P.X, P.Y, P.Z,
            Ref, *reinterpret_cast<const uint32*>(&Ref),
            Got, *reinterpret_cast<const uint32*>(&Got));
    }
}

bool FVoxelForgeDensityPurityTest::RunTest(const FString& Parameters)
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

    TArray<FVector> Points;
    BuildSamplePoints(World, NumSamples, /*Seed*/ 20260727, Points);

    // ── Référence : ordre linéaire, thread de jeu, caches chauds naturellement. ──
    TArray<float> Ref;
    Ref.SetNumUninitialized(NumSamples);
    for (int32 i = 0; i < NumSamples; ++i)
    {
        Ref[i] = Gen->GetDensityAt((float)Points[i].X, (float)Points[i].Y, (float)Points[i].Z);
    }

    // Un monde entièrement NaN/constant passerait tout ce qui suit trivialement. Vérifier qu'on
    // mesure bien un vrai champ. / An all-NaN or constant world would pass everything below
    // trivially. Check we are measuring a real field. (This is also the canary for AUDIT C1: a
    // large seed collapses the noise terms and the field goes constant.)
    {
        int32 NumFinite = 0, NumDistinct = 0;
        TSet<uint32> Seen;
        for (const float V : Ref)
        {
            if (FMath::IsFinite(V)) { ++NumFinite; }
            Seen.Add(*reinterpret_cast<const uint32*>(&V));
        }
        NumDistinct = Seen.Num();
        TestEqual(TEXT("every density sample is finite (no NaN/Inf leaking out of the generator)"),
                  NumFinite, NumSamples);
        if (NumDistinct < NumSamples / 100)
        {
            AddError(FString::Printf(
                TEXT("The density field is suspiciously flat: only %d distinct values across %d ")
                TEXT("samples. Either the fixture built an empty world, or the noise field has ")
                TEXT("collapsed (see AUDIT-2026-07.md C1 — unbounded SeedF). The purity checks ")
                TEXT("below would pass trivially on a constant field, so they prove nothing here."),
                NumDistinct, NumSamples));
        }
    }

    // ── 1. INDÉPENDANCE À L'ORDRE, même thread. ──
    // Un cache dont la clé est incomplète rend une valeur différente selon ce qui l'a précédé.
    // An incompletely-keyed cache returns a different value depending on what preceded it.
    {
        TArray<int32> Order;
        BuildShuffledOrder(NumSamples, /*Seed*/ 991, Order);

        int32 Mismatches = 0;
        FString First;
        for (const int32 i : Order)
        {
            const float Got = Gen->GetDensityAt((float)Points[i].X, (float)Points[i].Y, (float)Points[i].Z);
            if (!BitEqual(Got, Ref[i]))
            {
                if (Mismatches == 0) { First = DescribeMismatch(Points[i], Ref[i], Got); }
                ++Mismatches;
            }
        }
        if (Mismatches > 0)
        {
            AddError(FString::Printf(
                TEXT("ORDER DEPENDENCE: %d of %d points changed value when queried in a different ")
                TEXT("order on the SAME thread. A per-chunk cache is missing an input from its key. ")
                TEXT("First: %s"), Mismatches, NumSamples, *First));
        }
    }

    // ── 2. INDÉPENDANCE AU THREAD. ──
    // C'est la moitié que ValidateDeterminism (game-thread) ne peut pas voir. Chaque worker
    // parcourt SON propre ordre mélangé, donc ses thread_local se réchauffent différemment.
    // This is the half game-thread ValidateDeterminism cannot see. Each worker walks its OWN
    // shuffled order, so its thread_locals warm up differently.
    {
        const int32 NumBlocks = FMath::Max(4, FMath::Min(16, FPlatformMisc::NumberOfCores()));
        FMismatch Bad;

        ParallelFor(NumBlocks, [&](int32 Block)
        {
            TArray<int32> Order;
            BuildShuffledOrder(NumSamples, /*Seed*/ 4000 + Block, Order);
            const UVoxelGenerator* LocalGen = World.Generator.Get();
            for (const int32 i : Order)
            {
                // Chaque bloc parcourt TOUS les points (pas seulement une tranche) : c'est le
                // parcours complet dans un ordre différent qui réchauffe les caches thread_local
                // différemment, et c'est exactement ce qu'on cherche à faire diverger.
                // Every block walks ALL the points, not a slice: it is the full walk in a
                // different order that warms the thread_local caches differently, which is
                // precisely what we are trying to make diverge.
                const float V = LocalGen->GetDensityAt(
                    (float)Points[i].X, (float)Points[i].Y, (float)Points[i].Z);
                if (!BitEqual(V, Ref[i])) { Bad.Record(i); }
            }
        });

        const int32 Count = Bad.Count.load();
        if (Count > 0)
        {
            const int32 Idx = Bad.FirstIndex.load();
            AddError(FString::Printf(
                TEXT("WORKER DIVERGENCE: %d sample evaluations on worker threads disagreed with the ")
                TEXT("game-thread reference. This is the failure mode AVoxelWorld::ValidateDeterminism ")
                TEXT("cannot detect, and it means a thread_local cache under GetDensityAt is serving a ")
                TEXT("value it should not. First: %s"),
                Count, *DescribeMismatch(Points[Idx], Ref[Idx],
                                         Gen->GetDensityAt((float)Points[Idx].X, (float)Points[Idx].Y,
                                                           (float)Points[Idx].Z))));
        }
    }

    // ── 3. PURETÉ AVEC LA COUCHE DE DIFF ACTIVE. ──
    // Les DiffSlots sont un cache direct-mapped à 64 entrées, indexé par les bits bas du chunk.
    // Une collision servirait la liste de mods d'un AUTRE chunk : un carve fantôme à distance.
    // DiffSlots is a 64-entry direct-mapped cache indexed by the chunk coord's low bits. A
    // collision would serve another chunk's mod list: a ghost carve at a distance.
    {
        FVoxelModification Mod;
        Mod.Shape    = EVoxelBrushShape::Sphere;
        Mod.Radius   = 12.0f;
        Mod.Strength = -10.0f;
        for (int32 k = 0; k < 8; ++k)
        {
            Mod.Center = FVector((float)(k * CHUNK_SIZE), (float)(-k * CHUNK_SIZE), World.MidVoxelZ());
            World.DiffLayer->ApplyModification(Mod);
        }
        TestTrue(TEXT("the diff layer registered the test carves"), World.DiffLayer->HasAnyMods());

        TArray<float> DiffRef;
        DiffRef.SetNumUninitialized(NumSamples);
        for (int32 i = 0; i < NumSamples; ++i)
        {
            DiffRef[i] = Gen->GetDensityAt((float)Points[i].X, (float)Points[i].Y, (float)Points[i].Z);
        }

        FMismatch Bad;
        const int32 NumBlocks = FMath::Max(4, FMath::Min(16, FPlatformMisc::NumberOfCores()));
        ParallelFor(NumBlocks, [&](int32 Block)
        {
            TArray<int32> Order;
            BuildShuffledOrder(NumSamples, /*Seed*/ 7000 + Block, Order);
            const UVoxelGenerator* LocalGen = World.Generator.Get();
            for (const int32 i : Order)
            {
                const float V = LocalGen->GetDensityAt(
                    (float)Points[i].X, (float)Points[i].Y, (float)Points[i].Z);
                if (!BitEqual(V, DiffRef[i])) { Bad.Record(i); }
            }
        });

        const int32 Count = Bad.Count.load();
        if (Count > 0)
        {
            const int32 Idx = Bad.FirstIndex.load();
            AddError(FString::Printf(
                TEXT("DIFF-LAYER IMPURITY: %d evaluations diverged with player edits present. ")
                TEXT("Suspect the direct-mapped DiffSlots cache in GetDensityAt (chunk low-bit ")
                TEXT("index + ModsVersion). First mismatch index %d at (%.0f, %.0f, %.0f)."),
                Count, Idx, Points[Idx].X, Points[Idx].Y, Points[Idx].Z));
        }

        // Et le carve doit vraiment avoir changé quelque chose, sinon le sous-test ci-dessus
        // n'a rien testé. / And the carve must actually have changed something, else the sub-test
        // above tested nothing.
        int32 NumChanged = 0;
        for (int32 i = 0; i < NumSamples; ++i)
        {
            if (!BitEqual(DiffRef[i], Ref[i])) { ++NumChanged; }
        }
        if (NumChanged == 0)
        {
            AddError(TEXT("No sample changed after applying 8 carves — the diff layer branch of ")
                     TEXT("GetDensityAt was never exercised, so the purity check above is vacuous. ")
                     TEXT("Move the carve centres so they overlap the sample cloud."));
        }
    }

    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
