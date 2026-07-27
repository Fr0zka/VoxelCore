// VoxelForgeCrossPlatformTest.cpp
// LA QUESTION MULTIJOUEUR, RENDUE MESURABLE / THE MULTIPLAYER QUESTION, MADE MEASURABLE
//
// Jahni, 2026-07-27 : *« je voudrais que le jeu soit jouable sur les deux plateformes, Linux et
// Windows, donc un hôte Windows avec un client Linux pourrait arriver, et l'inverse. »*
// Et la barre d'acceptation : *« 99.99% au pire reproductible si deux personnes partagent la même
// seed, puisque tout le monde le reconstruit en multijoueur. »*
//
// ⚠️ LE PROBLÈME (AUDIT §C9) : le MÊME `FPSemanticsMode.Default` d'UBT ne veut pas dire la même
// chose selon la toolchain — `VCToolChain` (Windows/MSVC) le résout en **`/fp:fast`**,
// `ClangToolChain` (Linux/Mac/Windows-Clang) le résout en **précis + `-ffp-contract=off`**. Deux
// builds de la MÊME source sont donc compilés sous des règles flottantes OPPOSÉES. Un hôte Windows
// et un client Linux ne sont pas seulement *autorisés* à diverger : ils sont compilés pour.
//
// ─────────────────────────────────────────────────────────────────────────────────────────
// CE TEST NE CORRIGE RIEN — IL MESURE, et c'est ce qui manque
// ─────────────────────────────────────────────────────────────────────────────────────────
// Aucune quantité de raisonnement ne dit à quel point les deux plateformes divergent : il faut le
// LIRE. Ce test produit deux empreintes du même monde, à la même seed, et les affiche. On le lance
// sur Windows, on le lance sur Linux, on compare les deux lignes.
//
//   • **EMPREINTE DE FORME** — le SIGNE de la densité seulement (solide / air). C'est la SEULE
//     chose que le mesher lit (`D >= IsoLevel ⇒ air`). Si cette empreinte est identique, les deux
//     plateformes ont **le même monde** : mêmes cavités, mêmes murs, même navigabilité, même
//     collision au voxel près. C'est littéralement le critère « 99.99% reproductible » de Jahni.
//
//   • **EMPREINTE DE CHAMP** — tous les bits de tous les floats. Identique ⇒ reproductibilité
//     BIT à BIT. Différente alors que la forme est identique ⇒ la divergence est un frémissement
//     sous-voxel de la position des sommets, sans conséquence de jeu.
//
// C'est le bon découpage parce qu'il sépare les deux échecs possibles, qui n'ont pas du tout la
// même gravité :
//
//   forme ==  &&  champ ==   ⇒ parfait, rien à faire.
//   forme ==  &&  champ !=   ⇒ ACCEPTABLE. Les sommets bougent de ~1e-5 voxel. Personne ne le voit,
//                              rien ne s'y accroche — SAUF si un jour on compare des hashs de
//                              géométrie entre pairs. À ne pas faire, donc.
//   forme !=                 ⇒ **INACCEPTABLE**. Un voxel solide chez l'un est de l'air chez
//                              l'autre : un joueur traverse un mur que l'autre voit plein.
//
// ⚠️ POURQUOI LA FORME A DE BONNES CHANCES DE TENIR MÊME AUJOURD'HUI — et pourquoi il faut quand
// même la mesurer : toutes les décisions STRUCTURELLES du plugin (quelle arête de treillis est
// ouverte, quelle cellule porte une colonne, où sont les salles et les passages) passent par
// `VoxelHash::*`, c.-à-d. de l'ARITHMÉTIQUE ENTIÈRE, identique sur toute plateforme. Le flottant
// ne décide que la POSITION de la surface. Un signe ne bascule donc que si un échantillon tombe à
// ~1e-5 de l'isosurface — d'où le troisième chiffre affiché, `NearIso`, qui BORNE le risque au
// lieu de le supposer.
//
// The structural decisions all go through integer hashing, so only the surface POSITION is
// float-decided. A sign flips only where a sample sits within ~1e-5 of the isosurface, which is why
// NearIso is reported: it bounds the risk instead of assuming it.
//
// ─────────────────────────────────────────────────────────────────────────────────────────
// COMMENT S'EN SERVIR / HOW TO USE THIS
// ─────────────────────────────────────────────────────────────────────────────────────────
//   1. Lancer sur Windows, noter les deux empreintes.
//   2. Lancer sur Linux, comparer.
//   3. Une fois `FPSemantics = Precise` posé sur le module (AUDIT §C9, bloqué par la dette IWYU)
//      et les deux plateformes d'accord : **épingler** les valeurs dans `PinnedShapeDigest` /
//      `PinnedFieldDigest` ci-dessous. Le test devient alors un garde-fou permanent — toute
//      régression de déterminisme échoue bruyamment, sur la plateforme qui a dérivé.
//
// Tant que les constantes valent 0, le test ne peut pas échouer sur les empreintes : il RAPPORTE.
// C'est délibéré — épingler une valeur avant que les plateformes soient d'accord ne ferait que
// graver la divergence dans le test.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "VoxelForgeTestFixture.h"
#include "VoxelGenerator.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FVoxelForgeCrossPlatformTest,
    "VoxelForge.Determinism.CrossPlatformDigest",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

namespace
{
    //=========================================================================
    // ÉPINGLES / PINS — 0 = « pas encore d'accord de référence », le test rapporte sans juger.
    //=========================================================================
    // À remplir UNIQUEMENT quand Windows et Linux rendent la même valeur. Voir l'en-tête.
    constexpr uint64 PinnedShapeDigest = 0;
    constexpr uint64 PinnedFieldDigest = 0;

    // FNV-1a 64 bits, octet par octet, **entier pur**. Volontairement écrit à la main plutôt que
    // pris dans le moteur : une empreinte de déterminisme ne doit dépendre d'aucune implémentation
    // qui pourrait, elle, varier. Ici il n'y a que des `^` et des `*` sur uint64.
    // Hand-rolled on purpose: a determinism digest must not depend on an implementation that could
    // itself vary. Nothing here but XOR and multiply on uint64.
    constexpr uint64 FnvOffsetBasis = 0xcbf29ce484222325ull;
    constexpr uint64 FnvPrime       = 0x00000100000001b3ull;

    FORCEINLINE void FnvAccumByte(uint64& H, uint8 B)
    {
        H ^= (uint64)B;
        H *= FnvPrime;
    }

    FORCEINLINE void FnvAccumU32(uint64& H, uint32 V)
    {
        FnvAccumByte(H, (uint8)( V        & 0xFFu));
        FnvAccumByte(H, (uint8)((V >>  8) & 0xFFu));
        FnvAccumByte(H, (uint8)((V >> 16) & 0xFFu));
        FnvAccumByte(H, (uint8)((V >> 24) & 0xFFu));
    }

    /** Bits d'un float, avec les NaN NORMALISÉS : un NaN a plusieurs représentations et rien ne
     *  garantit que deux plateformes produisent la même. On les compte à part. */
    FORCEINLINE uint32 FloatBitsNormalised(float V, bool& bOutWasNaN)
    {
        bOutWasNaN = FMath::IsNaN(V);
        if (bOutWasNaN) { return 0x7FC00000u; }
        return *reinterpret_cast<const uint32*>(&V);
    }
}

bool FVoxelForgeCrossPlatformTest::RunTest(const FString& Parameters)
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
    // LA GRILLE — entièrement déterministe, SANS RNG
    //=========================================================================
    // Pas de `FRandomStream` ici, contrairement aux autres tests : l'ensemble des points doit être
    // identique sur les deux plateformes SANS dépendre d'une seule ligne de code partagé. Une
    // boucle entière sur des bornes entières ne peut pas diverger.
    // No RNG: the point set must be identical across platforms without depending on any shared
    // code at all. An integer loop over integer bounds cannot diverge.
    const int32 TopVoxelZ    = World.TopChunkZ    * CHUNK_SIZE + CHUNK_SIZE - 1;
    const int32 BottomVoxelZ = World.BottomChunkZ * CHUNK_SIZE;

    constexpr int32 XYStep = 8;
    constexpr int32 ZStep  = 8;
    const int32 XYExtent   = 3 * CHUNK_SIZE;   // couvre la spine (0,0), les passages et le rocher

    uint64 ShapeDigest = FnvOffsetBasis;
    uint64 FieldDigest = FnvOffsetBasis;

    int32 NumSamples = 0, NumSolid = 0, NumNaN = 0, NumNearIso = 0;

    // `NearIso` : à quelle distance de zéro un échantillon doit-il être pour qu'une différence
    // d'ULP puisse faire basculer son SIGNE ? Les écarts mesurés entre le `switch` et la pile
    // valent ~1e-5 au pire (AUDIT §C10) ; un écart entre modèles flottants est du même ordre.
    // Tout échantillon plus loin que ça de l'isosurface ne peut PAS changer de côté.
    constexpr float NearIsoBand = 1.0e-4f;   // 10× la pire divergence observée : marge délibérée

    for (int32 Z = BottomVoxelZ; Z <= TopVoxelZ; Z += ZStep)
    {
        for (int32 Y = -XYExtent; Y <= XYExtent; Y += XYStep)
        {
            for (int32 X = -XYExtent; X <= XYExtent; X += XYStep)
            {
                const float D = Gen->GetDensityAt((float)X, (float)Y, (float)Z);

                bool bWasNaN = false;
                const uint32 Bits = FloatBitsNormalised(D, bWasNaN);
                if (bWasNaN) { ++NumNaN; }

                // FORME : un seul bit par échantillon — le côté de l'isosurface, ce que lit le
                // mesher. C'est l'empreinte qui doit tenir entre plateformes.
                const bool bAir = (D >= 0.0f);
                if (!bAir) { ++NumSolid; }
                FnvAccumByte(ShapeDigest, bAir ? 1u : 0u);

                // CHAMP : tous les bits.
                FnvAccumU32(FieldDigest, Bits);

                if (!bWasNaN && FMath::Abs(D) < NearIsoBand) { ++NumNearIso; }
                ++NumSamples;
            }
        }
    }

    //=========================================================================
    // LE RAPPORT — c'est le produit de ce test
    //=========================================================================
    AddInfo(FString::Printf(
        TEXT("CROSS-PLATFORM DIGEST (seed %d, %d samples, step %d/%d)\n")
        TEXT("    SHAPE digest : 0x%016llX   <- must match across Windows/Linux. This is the world.\n")
        TEXT("    FIELD digest : 0x%016llX   <- bit-for-bit. May differ; see NearIso below.\n")
        TEXT("    solid %d / air %d / NaN %d"),
        World.Settings->Seed, NumSamples, XYStep, ZStep,
        ShapeDigest, FieldDigest, NumSolid, NumSamples - NumSolid, NumNaN));

    // LE chiffre qui borne le risque, plutôt que de le supposer.
    if (NumNearIso == 0)
    {
        AddInfo(FString::Printf(
            TEXT("NearIso: 0 of %d samples sit within %.1e of the isosurface. No sample is close ")
            TEXT("enough for a float-model difference (~1e-5 worst observed) to flip its SIGN, so ")
            TEXT("the SHAPE digest is robust to the /fp:fast-vs-precise split by a 10x margin at ")
            TEXT("these sample points. That is evidence, not proof -- it covers this grid, not ")
            TEXT("every voxel in a world."),
            NumSamples, NearIsoBand));
    }
    else
    {
        AddWarning(FString::Printf(
            TEXT("NearIso: %d of %d samples sit within %.1e of the isosurface -- close enough that ")
            TEXT("a float-model difference could flip their SIGN, which is a solid-vs-air ")
            TEXT("disagreement between a Windows host and a Linux client. This is the concrete ")
            TEXT("mechanism behind AUDIT C9, and the count is roughly how many voxels per %d are at ")
            TEXT("risk. It does not mean they DO differ -- run this on both platforms and compare ")
            TEXT("the SHAPE digest to find out."),
            NumNearIso, NumSamples, NearIsoBand, NumSamples));
    }

    TestEqual(TEXT("no sample produced NaN"), NumNaN, 0);

    // Un monde entièrement solide ou entièrement vide rendrait les empreintes vraies mais vides de
    // sens. Garde-fou minimal contre un test qui se félicite de ne rien mesurer.
    TestTrue(TEXT("the sampled world contains both solid and air (the digest is meaningful)"),
             NumSolid > 0 && NumSolid < NumSamples);

    //=========================================================================
    // LES ÉPINGLES — inertes tant que personne ne les a posées
    //=========================================================================
    if (PinnedShapeDigest != 0)
    {
        TestEqual(TEXT("SHAPE digest matches the pinned cross-platform reference"),
                  ShapeDigest, PinnedShapeDigest);
    }
    else
    {
        AddInfo(TEXT("SHAPE digest is not pinned yet. Pin it only once Windows and Linux agree -- ")
                TEXT("pinning first would just carve the divergence into the test."));
    }

    if (PinnedFieldDigest != 0)
    {
        TestEqual(TEXT("FIELD digest matches the pinned cross-platform reference"),
                  FieldDigest, PinnedFieldDigest);
    }

    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
