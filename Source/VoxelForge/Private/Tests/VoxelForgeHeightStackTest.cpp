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
// D'où une seconde famille, `VoxelHeightOp.h`, mesurée ici contre `ComputeSurfaceTerrainZ`.
//
// ⚠️ CE QUE CE TEST COUVRE
//   ✅ la pile de HAUTEUR du sol, contre `ComputeSurfaceTerrainZ` (défauts puis tous les ops F20) ;
//   ✅ la pile de densité SurfaceWorld : décomposition, ordre des canaux, densité finie, overhang ;
//   ✅ le mélange synthétique à poids zéro.
//
// LA BARRE : **bit à bit** pour les opérations en espace hauteur.
// The bar is bit-identity for height-space operations.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Async/ParallelFor.h"
#include "HAL/PlatformMisc.h"

#include "VoxelForgeTestFixture.h"
#include "VoxelHeightOp.h"
#include "VoxelDensityOpStack.h"

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

    //=========================================================================
    // ÉTAPE 2a — LE PONT VERS L'ESPACE DENSITÉ
    //=========================================================================
    // `FSurfaceColumnSource` consomme les DEUX piles de hauteur (sol + voûte) et rend une densité.
    // Cette passe est la variante **sans overhang** et **sans biomes** ; l'overhang a sa propre
    // passe juste en dessous. / No-overhang, no-biome variant; the overhang has its own pass below.
    {
        FSurfaceGenerationParams P = AllOps;
        P.OverhangStrength = 0.0f;

        FVoxelOpStack Stack;
        VoxelDensityOps::BuildSurfaceStack(Stack, P, World.Settings->Seed,
                                           Gen->OriginSpineRadius, World.StrateManager.Get());

        // 1 source + 1 overhang + 4 structurels. L'op overhang est présent mais inerte ici
        // (amp 0 ⇒ sortie immédiate) — la décomposition ne change pas selon les params.
        TestEqual(TEXT("the surface density stack is source + overhang + 4 structural"),
                  Stack.Num(), 6);
        TestTrue(TEXT("the surface density stack has valid channel order"),
                 Stack.ValidateChannelOrder());

        FVoxelOpContext Ctx;
        Ctx.Seed               = (uint32)World.Settings->Seed;
        Ctx.LayoutVersion      = World.StrateManager->GetLayoutVersion();
        Ctx.StrateTopWorldZ    = P.StrateTopWorldZ;
        Ctx.StrateBottomWorldZ = P.StrateBottomWorldZ;
        Stack.PrepareChunk(Ctx);

        int32 NumNonFinite = 0;

        FRandomStream Rng(5150);
        for (int32 i = 0; i < NumHeightSamples; ++i)
        {
            const float X = (float)Rng.RandRange(-4 * CHUNK_SIZE, 4 * CHUNK_SIZE);
            const float Y = (float)Rng.RandRange(-4 * CHUNK_SIZE, 4 * CHUNK_SIZE);
            const float Z = (float)Rng.RandRange(BottomVoxelZ, TopVoxelZ);
            if (!FMath::IsFinite(Stack.EvalMC(X, Y, Z)))
            {
                ++NumNonFinite;
            }
        }

        TestEqual(TEXT("surface: current density stack has no non-finite samples"),
                  NumNonFinite, 0);
    }

    //=========================================================================
    // ÉTAPE 2b — L'OVERHANG
    //=========================================================================
    // `ComputeSurfaceColumn` résout le gate et la direction amont par colonne ; la moitié des
    // échantillons est placée DANS la fenêtre d'overhang qu'il rapporte, pour que l'op overhang de
    // la pile soit réellement exercé. / Half the samples are placed inside the overhang window that
    // ComputeSurfaceColumn reports, so the stack's overhang op is actually exercised.
    {
        FSurfaceGenerationParams P = AllOps;
        P.OverhangStrength        = 0.8f;
        P.OverhangSlopeThreshold  = 0.12f;
        P.OverhangHeight          = 14.0f;
        P.OverhangReach           = 10.0f;
        P.OverhangFrequency       = 0.05f;
        P.OverhangZScale          = 0.6f;

        FVoxelOpStack Stack;
        VoxelDensityOps::BuildSurfaceStack(Stack, P, World.Settings->Seed,
                                           Gen->OriginSpineRadius, World.StrateManager.Get());

        FVoxelOpContext Ctx;
        Ctx.Seed               = (uint32)World.Settings->Seed;
        Ctx.LayoutVersion      = World.StrateManager->GetLayoutVersion();
        Ctx.StrateTopWorldZ    = P.StrateTopWorldZ;
        Ctx.StrateBottomWorldZ = P.StrateBottomWorldZ;
        Stack.PrepareChunk(Ctx);

        // Pas de biomes : contexte vide ⇒ ComputeSurfaceColumn retombe sur BaseSurface pour les
        // deux côtés, poids 0. C'est exactement ce que la pile fait aujourd'hui.
        FBiomeContext EmptyCtx;
        TArray<FSurfaceGenerationParams> NoBiomeParams;
        FChunkBiomeCache BiomeCache;

        int32 NumInWindow = 0, NumNonFinite = 0;

        FRandomStream Rng(1337);
        for (int32 i = 0; i < NumHeightSamples; ++i)
        {
            const float X = (float)Rng.RandRange(-4 * CHUNK_SIZE, 4 * CHUNK_SIZE);
            const float Y = (float)Rng.RandRange(-4 * CHUNK_SIZE, 4 * CHUNK_SIZE);

            float TerrainZ = 0.0f, CeilSurf = 0.0f, Amp = 0.0f, DirX = 0.0f, DirY = 0.0f;
            Gen->ComputeSurfaceColumn(X, Y, MidChunkZ, P, EmptyCtx, NoBiomeParams, BiomeCache,
                                      TerrainZ, CeilSurf, Amp, DirX, DirY);

            // Échantillonner DANS la fenêtre d'overhang la moitié du temps : un tirage uniforme sur
            // toute la strate la raterait presque toujours, et le test serait vert sans avoir
            // exercé l'op une seule fois — le même piège que `WaterLevelRelative` plus haut.
            float Z;
            if ((i & 1) && Amp > 0.0f)
            {
                Z = TerrainZ + P.OverhangHeight * ((float)(i % 97) / 97.0f);
                ++NumInWindow;
            }
            else
            {
                Z = (float)Rng.RandRange(BottomVoxelZ, TopVoxelZ);
            }

            if (!FMath::IsFinite(Stack.EvalMC(X, Y, Z)))
            {
                ++NumNonFinite;
            }
        }

        AddInfo(FString::Printf(
            TEXT("Overhang: %d of %d samples deliberately inside the overhang window; current "
                 "stack non-finite samples=%d."),
            NumInWindow, NumHeightSamples, NumNonFinite));
        TestEqual(TEXT("overhang: current density stack has no non-finite samples"),
                  NumNonFinite, 0);

        if (NumInWindow == 0)
        {
            AddWarning(TEXT("No sample landed inside the overhang window, so the op was never ")
                       TEXT("actually exercised. Raise OverhangStrength or lower ")
                       TEXT("OverhangSlopeThreshold until this is well above zero."));
        }
    }

    //=========================================================================
    // LE COMBINER `Mask` — mélange de biomes (§5 : le prototype de la Phase 3)
    //=========================================================================
    // Testé contre un champ de biomes SYNTHÉTIQUE plutôt que contre le résolveur Voronoï réel, et
    // c'est le bon choix ici : le vrai résolveur est déjà couvert par ses propres tests, alors
    // qu'un champ synthétique permet de balayer le poids de 0 à 1 de façon CONTINUE et de vérifier
    // l'identité `blend(w) == lerp(A, B, w)` sur toute la plage — y compris les deux bouts, où une
    // erreur d'inversion (`1-w` au lieu de `w`) se cache le mieux.
    //
    // Tested against a SYNTHETIC field rather than the real Voronoi resolver: the resolver has its
    // own tests, while a synthetic field lets the weight be swept continuously from 0 to 1, which is
    // where an inverted lerp hides.
    {
        // Deux jeux de params franchement différents : si le mélange était un no-op, ou prenait le
        // mauvais côté, l'écart serait énorme plutôt que subtil.
        FSurfaceGenerationParams A = Defaults;
        FSurfaceGenerationParams B = Defaults;
        A.ElevationRange   = 40.0f;   A.MountainStrength = 0.2f;
        B.ElevationRange   = 12.0f;   B.MountainStrength = 0.9f;
        B.BaseGroundRelative = FMath::Clamp(A.BaseGroundRelative + 0.15f, 0.0f, 1.0f);

        TArray<FSurfaceGenerationParams> PerBiome;
        PerBiome.Add(A);
        PerBiome.Add(B);

        /** Champ synthétique : biome 0 dominant, biome 1 voisin, poids imposé par le test. */
        class FFixedWeightField final : public IVoxelBiomeField
        {
        public:
            float W = 0.0f;
            FVoxelBiomeWeights SampleAt(float, float) const override
            {
                FVoxelBiomeWeights Out;
                Out.Dominant = 0;  Out.Neighbor = 1;  Out.NeighborWeight = W;
                return Out;
            }
        };
        FFixedWeightField FieldA;

        // Les deux piles de référence, non mélangées.
        FVoxelHeightStack StackA, StackB;
        VoxelHeightOps::BuildSurfaceHeightStack(StackA, A, World.Settings->Seed);
        VoxelHeightOps::BuildSurfaceHeightStack(StackB, B, World.Settings->Seed);

        FVoxelHeightStack Blended;
        Blended.Add(VoxelHeightOps::MakeBiomeBlendHeightSource(PerBiome, World.Settings->Seed, &FieldA));

        const float Weights[] = { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f };
        int32 NumWrong = 0;
        float WorstDelta = 0.0f;

        for (const float W : Weights)
        {
            FieldA.W = W;
            for (int32 i = 0; i < 400; ++i)
            {
                const float X = (float)((i % 20) * 11);
                const float Y = (float)((i / 20) * 13);

                const float HA = StackA.EvalHeight(X, Y);
                const float HB = StackB.EvalHeight(X, Y);
                // ⚠️ L'attendu doit reproduire la MÊME expression que l'op, `FMath::Lerp` compris :
                // écrire `HA + (HB - HA) * W` à la place testerait l'algèbre, pas le code.
                const float Expect = (W > 0.0f) ? FMath::Lerp(HA, HB, W) : HA;
                const float Got    = Blended.EvalHeight(X, Y);

                if (!BitEqual(Expect, Got))
                {
                    ++NumWrong;
                    WorstDelta = FMath::Max(WorstDelta, FMath::Abs(Expect - Got));
                }
            }
        }

        TestEqual(TEXT("biome blend: heights lerp between the two biomes' full stacks, bit-exactly"),
                  NumWrong, 0);

        // ⚠️ Rapporter le SUCCÈS, pas seulement l'échec. Un `TestEqual` qui passe n'écrit rien, et
        // une vérification silencieuse est indiscernable d'une vérification qui n'a jamais tourné —
        // exactement le piège signalé pour `WaterLevelRelative` et la fenêtre d'overhang, dans
        // lequel ce bloc-ci était tombé au premier jet. Le compte rend l'exécution visible.
        // Report success, not just failure: a silent pass is indistinguishable from a check that
        // never ran.
        AddInfo(FString::Printf(
            TEXT("Biome blend: %d (weight, point) pairs across weights 0/0.25/0.5/0.75/1.0 all match ")
            TEXT("Lerp of the two biomes' full height stacks bit-exactly. Weight 0 returns the ")
            TEXT("dominant untouched and weight 1 the neighbour, so the lerp is not inverted."),
            (int32)UE_ARRAY_COUNT(Weights) * 400));

        if (NumWrong > 0)
        {
            AddError(FString::Printf(
                TEXT("Biome blend wrong on %d of 2000 (weight, point) pairs, worst |delta| %.6g. ")
                TEXT("Check: is the lerp toward the NEIGHBOUR (weight 0 must give the dominant ")
                TEXT("untouched, weight 1 the neighbour), and does each biome's stack compute its ")
                TEXT("OWN relief for its OWN terrace gate rather than sharing the dominant's?"),
                NumWrong, WorstDelta));
        }

        // Le plafond SÉLECTIONNE au lieu de mélanger — comportement d'origine, reproduit tel quel.
        {
            FieldA.W = 1.0f;   // le voisin l'emporterait si le plafond mélangeait
            FVoxelHeightStack CeilSel;
            CeilSel.Add(VoxelHeightOps::MakeBiomeSelectCeilingSource(PerBiome, World.Settings->Seed, &FieldA));

            FVoxelHeightStack CeilDominant;
            VoxelHeightOps::BuildSurfaceCeilingStack(CeilDominant, A, World.Settings->Seed);

            int32 NumCeilWrong = 0;
            for (int32 i = 0; i < 200; ++i)
            {
                const float X = (float)((i % 20) * 11), Y = (float)((i / 20) * 13);
                if (!BitEqual(CeilSel.EvalHeight(X, Y), CeilDominant.EvalHeight(X, Y))) { ++NumCeilWrong; }
            }
            TestEqual(TEXT("biome ceiling SELECTS the dominant (never blends), even at weight 1"),
                      NumCeilWrong, 0);

            AddInfo(TEXT("Biome ceiling: 200 points at neighbour-weight 1.0 still return the ")
                    TEXT("DOMINANT biome's sky cap, i.e. it selects rather than blends -- the "
                         "original's behaviour, and the case a \"blend everything\" refactor would "
                         "silently break."));
        }
    }

    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
