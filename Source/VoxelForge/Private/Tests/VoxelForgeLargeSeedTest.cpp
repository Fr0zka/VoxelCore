// VoxelForgeLargeSeedTest.cpp
// AUDIT §C1 — le monde doit rester un monde quand la seed est grande.
// AUDIT C1 — the world must still be a world at a large seed.
//
// LE BUG / THE BUG
// Les sites de bruit s'écrivaient `WorldX * Freq + (float)Seed * 97.7f`. Un float a 24 bits de
// mantisse, donc à magnitude `V` l'ULP vaut `V · 2⁻²³` :
//
//     Seed = 1 000        → terme 9.8e4  → ULP 0.012   → correct
//     Seed = 100 000      → terme 9.8e6  → ULP 1.2     → le bruit se cale sur un treillis
//     Seed = 10 000 000   → terme 9.8e8  → ULP 117     → la coordonnée du voxel (~0.02/voxel) est
//                                                        ENTIÈREMENT absorbée ⇒ champ CONSTANT
//
// `ChangeSeed(int32)` est `BlueprintCallable` : un `FMath::Rand()` (jusqu'à 2³¹) suffit à produire
// un monde plat. Ça n'a jamais été vu parce que les seeds de test restaient petites — et la fixture
// des autres tests garde délibérément une petite seed, ce qui veut dire qu'**aucun autre test de ce
// dossier ne peut voir ce bug**.
//
// ⚠️ POURQUOI LES TESTS D'ÉQUIVALENCE NE L'AURAIENT JAMAIS ATTRAPÉ
// Ils comparent la pile d'opérateurs au `switch` d'archétype. Les deux lisent la MÊME expression
// fautive, donc les deux s'effondrent EXACTEMENT DE LA MÊME FAÇON à grande seed : bit-identiques,
// verts, et tous les deux plats. Un oracle qui partage le bug de l'implémentation ne le voit pas.
// **Ce test-ci ne compare rien à rien : il vérifie une PROPRIÉTÉ** — le terrain doit varier.
//
// The equivalence tests compare the op stack to the archetype switch. Both read the same faulty
// expression, so at a large seed both collapse identically: bit-identical, green, and both flat. An
// oracle that shares the implementation's bug cannot see it. This test asserts a PROPERTY instead.
//
// LE CORRECTIF, ET POURQUOI L'ÉVIDENT ÉTAIT FAUX
// Borner `SeedF` en gardant le `· 97.7` laisse le terme atteindre 1.6e6 (ULP 0.19 = 9.5× le pas par
// voxel) : moins spectaculaire, toujours cassé, ticket refermé. C'est le MULTIPLICATEUR qu'il faut
// supprimer. `VoxelHash::SeedOffset(Seed, SiteKey)` rend un décalage déjà dans les unités finales,
// borné à [0, 16383], salé par site — donc deux seeds doivent collisionner sur les ~50 sites à la
// fois pour donner le même monde, au lieu d'un seul bucket partagé.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "VoxelForgeTestFixture.h"
#include "VoxelGenerator.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FVoxelForgeLargeSeedTest,
    "VoxelForge.Determinism.LargeSeedSurvives",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

namespace
{
    /** Les seeds à éprouver. La première est le régime « ça marchait par chance », les suivantes
     *  sont là où le champ s'effondrait. La dernière est ce qu'un `FMath::Rand()` produit. */
    const int32 SeedsUnderTest[] = { 1337, 100000, 10000000, 2000000000 };

    /** Combien de hauteurs distinctes faut-il pour dire « ce n'est pas plat » ? Un champ effondré
     *  rend UNE valeur (ou deux ou trois par effet de bord d'arrondi). Un terrain sain en rend des
     *  centaines sur 400 échantillons. Le seuil est bas exprès : on teste « le bruit existe-t-il
     *  encore », pas « est-il joli ». */
    constexpr int32 MinDistinctHeights = 50;
}

bool FVoxelForgeLargeSeedTest::RunTest(const FString& Parameters)
{
    using namespace VoxelForgeTest;

    bool bAnyCollapse = false;

    for (const int32 Seed : SeedsUnderTest)
    {
        FTestWorld World;
        World.Build(Seed);
        if (!World.IsValid())
        {
            AddError(FString::Printf(TEXT("Seed %d: %s"), Seed, *World.WhyInvalid()));
            continue;
        }

        int32 TopVoxelZ = 0, BottomVoxelZ = 0;
        if (!World.GetSlotVoxelZRange(FTestWorld::SlotSurfaceWorld, TopVoxelZ, BottomVoxelZ))
        {
            AddError(TEXT("The fixture layout has no SurfaceWorld slot."));
            return false;
        }

        const UVoxelStrateDefinition* Def =
            World.StrateManager->GetStrateForChunk(
                FIntVector(0, 0, ((TopVoxelZ + BottomVoxelZ) / 2) / CHUNK_SIZE));
        if (!Def) { AddError(TEXT("No SurfaceWorld definition.")); return false; }

        FSurfaceGenerationParams P = Def->SurfaceParams;
        P.StrateTopWorldZ    = (float)TopVoxelZ;
        P.StrateBottomWorldZ = (float)BottomVoxelZ;

        // Échantillonner le HEIGHTFIELD plutôt que la densité : c'est là que le bruit vit, et une
        // hauteur est directement lisible ("le terrain est-il plat ?") là où une densité demande
        // d'être interprétée.
        TSet<uint32> DistinctBits;
        float MinH =  FLT_MAX, MaxH = -FLT_MAX;

        const UVoxelGenerator* Gen = World.Generator.Get();
        for (int32 iy = 0; iy < 20; ++iy)
        for (int32 ix = 0; ix < 20; ++ix)
        {
            // Pas de 7 voxels : assez large pour traverser plusieurs cellules de bruit, assez
            // petit pour rester dans une région cohérente.
            const float X = (float)(ix * 7);
            const float Y = (float)(iy * 7);
            const float H = Gen->ComputeSurfaceTerrainZ(X, Y, P);

            DistinctBits.Add(*reinterpret_cast<const uint32*>(&H));
            MinH = FMath::Min(MinH, H);
            MaxH = FMath::Max(MaxH, H);
        }

        const int32 NumDistinct = DistinctBits.Num();
        const float Range = MaxH - MinH;

        if (NumDistinct < MinDistinctHeights)
        {
            bAnyCollapse = true;
            AddError(FString::Printf(
                TEXT("SEED %d COLLAPSED THE NOISE FIELD: only %d distinct heights across 400 ")
                TEXT("samples (range %.4f voxels). This is AUDIT C1 — a seed offset large enough ")
                TEXT("that the float ULP swallows the voxel coordinate, so the noise input is ")
                TEXT("constant across many voxels and the terrain goes flat. Check that every noise ")
                TEXT("site uses VoxelHash::SeedOffset(SeedU, K) and that no `SeedF * K` pattern has ")
                TEXT("come back."),
                Seed, NumDistinct, Range));
        }
        else
        {
            AddInfo(FString::Printf(
                TEXT("Seed %d: %d distinct heights across 400 samples, range %.2f voxels. Field alive."),
                Seed, NumDistinct, Range));
        }
    }

    TestFalse(TEXT("no seed collapses the noise field (AUDIT C1)"), bAnyCollapse);

    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
