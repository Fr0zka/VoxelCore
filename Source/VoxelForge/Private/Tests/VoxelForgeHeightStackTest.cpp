// VoxelForgeHeightStackTest.cpp
// LA QUESTION D'ARCHITECTURE DE LA PHASE 2, POSÉE AVANT D'ÉCRIRE CE QUI EN DÉPEND.
// PHASE 2'S ARCHITECTURAL QUESTION, ASKED BEFORE WRITING WHAT DEPENDS ON THE ANSWER.
//
// SurfaceWorld a forcé une décision que ni Maze ni Slab n'avaient forcée : ses opérateurs de
// terrain (cliff / terrace / layer lines / plage) n'opèrent PAS sur la densité. Ils lisent et
// écrivent **une altitude**. Ils ne rentrent donc pas dans `IVoxelDensityOp`, et les y forcer
// voudrait dire soit un canal par-voxel pour une propriété de COLONNE, soit un seul opérateur
// opaque — ce que `OPSTACK-PLAN §2.5` appelle exactement l'échec du refactor.
//
// D'où une seconde famille, `VoxelHeightOp.h`. **Ce test est ce qui dit si elle était une bonne
// idée** — la même méthode que la Phase 1 a appliquée à la densité : décomposer, puis MESURER
// contre l'original, avant de construire par-dessus.
//
// ⚠️ Ce test ne touche PAS au chemin densité. `FHeightfieldSource` / `FSkyCapSource` /
// `FOverhangShelfMod` (OPSTACK-DECOMPOSITION §5) sont l'étape SUIVANTE, délibérément séparée : si
// l'espace-hauteur ne se décomposait pas proprement, on l'apprendrait ici, pour le prix d'un test,
// et pas après avoir écrit l'adaptateur, le cache de colonne et le branchement.
//
// LA BARRE : **bit à bit.** Depuis `FPSemantics = Precise` (AUDIT §C9/§C10), Maze et Slab sont
// bit-identiques à leur original ; il n'y a plus de « plancher ULP » à tolérer. Un écart ici est
// donc une vraie trouvaille — un offset de bruit faux, un ordre d'op inversé, un gate oublié.
// Ces fonctions sont des ALTITUDES en voxels, pas des densités : un écart d'un demi-voxel est un
// terrain visiblement différent, pas du bruit d'arrondi.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Async/ParallelFor.h"
#include "HAL/PlatformMisc.h"

#include "VoxelForgeTestFixture.h"
#include "VoxelHeightOp.h"

#include <atomic>

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FVoxelForgeHeightStackTest,
    "VoxelForge.OpStack.SurfaceHeightEquivalence",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

namespace
{
    constexpr int32 NumHeightSamples = 20000;

    /** Les params du terrain ne sont intéressants que si les ops sont ALLUMÉS. Ceux de la fixture
     *  sont les défauts, et `§5` note que les ops F20 sont « all off by default ». Un test qui ne
     *  ferait tourner que les défauts vérifierait la source structurelle et RIEN des quatre
     *  modificateurs — c'est-à-dire l'essentiel de ce qui est nouveau ici. */
    void EnableAllTerrainOps(FSurfaceGenerationParams& P)
    {
        P.CliffStrength        = 0.6f;
        P.CliffSampleDist      = 2.0f;
        P.CliffSlopeThreshold  = 0.15f;
        P.CliffSharpness       = 1.4f;

        P.TerraceStrength      = 0.7f;
        P.TerraceHeight        = 9.0f;
        P.TerraceHardness      = 0.8f;

        P.LayerLineDepth       = 1.3f;
        P.LayerLineSpacing     = 7.0f;

        // ⚠️ `WaterLevelRelative` DOIT être > 0, sinon `FBeachHeightMod` sort immédiatement et le
        // cinquième op n'est jamais exercé — un test vert qui n'a rien testé. Le défaut de la
        // struct est 0.0f, donc l'oublier est le piège naturel ici.
        // The beach op early-outs unless WaterLevelRelative > 0, so without this the fifth op is
        // never exercised at all — a green test that measured nothing.
        P.WaterLevelRelative   = 0.30f;
        P.BeachWidth           = 6.0f;
    }
}

bool FVoxelForgeHeightStackTest::RunTest(const FString& Parameters)
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
    if (!World.GetSlotVoxelZRange(FTestWorld::SlotSurfaceWorld, TopVoxelZ, BottomVoxelZ))
    {
        AddError(TEXT("The fixture layout has no SurfaceWorld slot. Check FTestWorld::Build's ")
                 TEXT("Archetypes[] against FTestWorld::SlotSurfaceWorld."));
        return false;
    }

    const int32 MidChunkZ = ((TopVoxelZ + BottomVoxelZ) / 2) / CHUNK_SIZE;

    // Les points d'échantillonnage : XY seulement, la hauteur ne dépend pas de Z (c'est le point).
    TArray<FVector2D> Points;
    Points.Reserve(NumHeightSamples);
    {
        FRandomStream Rng(90210);
        for (int32 i = 0; i < NumHeightSamples; ++i)
        {
            Points.Add(FVector2D(
                (float)Rng.RandRange(-6 * CHUNK_SIZE, 6 * CHUNK_SIZE),
                (float)Rng.RandRange(-6 * CHUNK_SIZE, 6 * CHUNK_SIZE)));
        }
    }

    //=========================================================================
    // LA BATTERIE, PARAMÉTRÉE PAR JEU DE PARAMS
    //=========================================================================
    auto RunForParams = [&](const FSurfaceGenerationParams& P, const TCHAR* Label, int32 SeedSalt)
    {
        FVoxelHeightStack Stack;
        VoxelHeightOps::BuildSurfaceHeightStack(Stack, P, World.Settings->Seed);

        // Une DÉCOMPOSITION, pas une enveloppe : source + 4 modificateurs.
        TestEqual(*FString::Printf(TEXT("%s: the height stack is decomposed into 5 ops"), Label),
                  Stack.Num(), 5);

        //---------------------------------------------------------------------
        // 1. ÉQUIVALENCE — contre ComputeSurfaceTerrainZ, en ALTITUDE
        //---------------------------------------------------------------------
        int32 NumDiff = 0, WorstIdx = -1;
        float WorstDelta = 0.0f, WorstOld = 0.0f;

        for (int32 i = 0; i < NumHeightSamples; ++i)
        {
            const float X = (float)Points[i].X, Y = (float)Points[i].Y;

            const float Old = Gen->ComputeSurfaceTerrainZ(X, Y, P);
            const float New = Stack.EvalHeight(X, Y);

            if (!BitEqual(Old, New))
            {
                ++NumDiff;
                const float Delta = FMath::Abs(Old - New);
                if (Delta > WorstDelta) { WorstDelta = Delta; WorstIdx = i; WorstOld = Old; }
            }
        }

        if (NumDiff == 0)
        {
            AddInfo(FString::Printf(
                TEXT("%s: bit-identical across %d samples. The height-space decomposition ")
                TEXT("reproduces ComputeSurfaceTerrainZ exactly."), Label, NumHeightSamples));
        }
        else
        {
            // Pas de gradation ULP ici, à dessein : ce sont des ALTITUDES. Depuis /fp:precise la
            // barre est l'égalité binaire, et un écart de hauteur se voit dans le monde.
            AddError(FString::Printf(
                TEXT("%s: %d of %d samples differ from ComputeSurfaceTerrainZ (largest |delta| ")
                TEXT("%.9g voxels at (%.0f, %.0f), where the reference height is %.4f). These are ")
                TEXT("ALTITUDES, not densities -- this is a real port error, not rounding. Check, ")
                TEXT("in order: the op ORDER (structural -> cliff -> terrace -> layer lines -> ")
                TEXT("beach), the terrace's `* Relief` gate (that is the original's `* M`), the ")
                TEXT("cliff resampling the STRUCTURAL field rather than the modified height, and ")
                TEXT("the noise offsets (3.1/5.7/0.7, 11/22/1.3, 99/77/0.9, 7.3/2.1/0.5)."),
                Label, NumDiff, NumHeightSamples, WorstDelta,
                WorstIdx >= 0 ? Points[WorstIdx].X : 0.0f,
                WorstIdx >= 0 ? Points[WorstIdx].Y : 0.0f,
                WorstOld));
        }

        //---------------------------------------------------------------------
        // 2. INVARIANCE DE FENÊTRE
        //---------------------------------------------------------------------
        // Une pile de hauteur alimente le cache de colonne T1.a, qui est PARTAGÉ sur toute la pile
        // verticale de chunks. Une impureté ici ne fait pas une couture locale : elle se propage à
        // tous les Z d'un coup (AUDIT §6.3).
        {
            std::atomic<int32> Impure{ 0 };
            const int32 NumBlocks = FMath::Max(4, FMath::Min(16, FPlatformMisc::NumberOfCores()));

            TArray<float> Ref;
            Ref.SetNumUninitialized(NumHeightSamples);
            for (int32 i = 0; i < NumHeightSamples; ++i)
            {
                Ref[i] = Stack.EvalHeight((float)Points[i].X, (float)Points[i].Y);
            }

            ParallelFor(NumBlocks, [&](int32 Block)
            {
                TArray<int32> LocalOrder;
                BuildShuffledOrder(NumHeightSamples, 1200 + Block + SeedSalt, LocalOrder);
                for (const int32 i : LocalOrder)
                {
                    const float V = Stack.EvalHeight((float)Points[i].X, (float)Points[i].Y);
                    if (!BitEqual(V, Ref[i])) { Impure.fetch_add(1, std::memory_order_relaxed); }
                }
            });

            TestEqual(*FString::Printf(
                          TEXT("%s: the height stack is window-invariant across order and threads"), Label),
                      Impure.load(), 0);
        }

        //---------------------------------------------------------------------
        // 3. LES MAJORANTS DE DÉPLACEMENT SONT-ILS HONNÊTES ?
        //---------------------------------------------------------------------
        // `MaxDisplacement` servira à borner une colonne pour un `ClassifyBox` de heightfield, la
        // même mécanique qui fait prouver 36-40 tuiles sur 60 à la dalle. Un majorant FAUX serait
        // un TROU, donc on le teste par force brute AVANT de construire quoi que ce soit dessus.
        //
        // On mesure le déplacement des trois mods bornables en comparant la pile complète à une
        // pile tronquée (source + cliff seuls) : la différence est exactement ce que terrace +
        // layer lines + plage ont déplacé.
        {
            FVoxelHeightStack Base;
            const IVoxelHeightOp* Structural = nullptr;
            Base.Add(VoxelHeightOps::MakeStructuralHeightSource(P, World.Settings->Seed, &Structural));
            Base.Add(VoxelHeightOps::MakeCliffHeightMod(P, Structural));

            FVoxelHeightStack Bounded;
            const IVoxelHeightOp* Structural2 = nullptr;
            Bounded.Add(VoxelHeightOps::MakeStructuralHeightSource(P, World.Settings->Seed, &Structural2));
            Bounded.Add(VoxelHeightOps::MakeCliffHeightMod(P, Structural2));
            Bounded.Add(VoxelHeightOps::MakeTerraceHeightMod(P));
            Bounded.Add(VoxelHeightOps::MakeLayerLineHeightMod(P));
            Bounded.Add(VoxelHeightOps::MakeBeachHeightMod(P));

            const float Claimed = FMath::Max(P.TerraceStrength > 0.0f ? P.TerraceHeight : 0.0f, 0.0f)
                                + FMath::Max(P.LayerLineSpacing > 0.0f ? P.LayerLineDepth : 0.0f, 0.0f)
                                + FMath::Max(P.WaterLevelRelative > 0.0f ? P.BeachWidth : 0.0f, 0.0f);

            float WorstObserved = 0.0f;
            int32 NumOverBound = 0;
            for (int32 i = 0; i < NumHeightSamples; ++i)
            {
                const float X = (float)Points[i].X, Y = (float)Points[i].Y;
                const float Moved = FMath::Abs(Bounded.EvalHeight(X, Y) - Base.EvalHeight(X, Y));
                WorstObserved = FMath::Max(WorstObserved, Moved);
                if (Moved > Claimed) { ++NumOverBound; }
            }

            TestEqual(*FString::Printf(
                          TEXT("%s: no sample exceeds the claimed MaxDisplacement (a false bound is a hole)"),
                          Label),
                      NumOverBound, 0);

            AddInfo(FString::Printf(
                TEXT("%s: MaxDisplacement claims %.3f voxels, worst observed %.3f (%.0f%% of the ")
                TEXT("claim). A loose bound only costs CPU later; a tight-but-wrong one would be a hole."),
                Label, Claimed, WorstObserved,
                Claimed > 0.0f ? 100.0f * WorstObserved / Claimed : 0.0f));
        }
    };

    //=========================================================================
    // DEUX PASSES — et la seconde est celle qui compte
    //=========================================================================
    const UVoxelStrateDefinition* SurfaceDef =
        World.StrateManager->GetStrateForChunk(FIntVector(0, 0, MidChunkZ));
    if (!SurfaceDef)
    {
        AddError(TEXT("No strate definition resolved for the SurfaceWorld slot's mid chunk."));
        return false;
    }

    // Les bornes Z de runtime sont posées à la main : `GetSlabParamsForChunk` a un équivalent pour
    // la dalle, mais le chemin surface passe par `ResolveSurfaceChunkParams`, qui est privé et
    // mêle la résolution de biome. La pile de hauteur ne dépend que des params + du seed, donc
    // fournir les params directement est à la fois suffisant et plus lisible en cas d'échec.
    FSurfaceGenerationParams Defaults = SurfaceDef->SurfaceParams;
    Defaults.StrateTopWorldZ    = (float)TopVoxelZ;
    Defaults.StrateBottomWorldZ = (float)BottomVoxelZ;

    RunForParams(Defaults, TEXT("SurfaceWorld(defaults)"), 0);

    // ⚠️ LA PASSE LOAD-BEARING. Les ops de terrain F20 sont éteints par défaut, donc la passe
    // ci-dessus n'exerce que la source structurelle et laisse les QUATRE modificateurs — c'est-à-
    // dire tout ce qui est nouveau dans cette décomposition — non testés. Celle-ci les allume.
    FSurfaceGenerationParams AllOps = Defaults;
    EnableAllTerrainOps(AllOps);
    RunForParams(AllOps, TEXT("SurfaceWorld(all terrain ops on)"), 64);

    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
