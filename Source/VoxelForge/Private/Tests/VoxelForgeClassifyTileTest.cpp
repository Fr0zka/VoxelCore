// VoxelForgeClassifyTileTest.cpp
// Phase 0.5 test #2 — LA SOLIDITÉ DE ClassifyTile / ClassifyTile soundness.
//
// ⚠️ LE TEST LE PLUS IMPORTANT DU PLUGIN / THE HIGHEST-CONSEQUENCE TEST IN THE PLUGIN.
//
// ClassifyTile (T1.d) répond "cette tuile est entièrement solide / entièrement air" AVANT tout
// échantillonnage, et sur un verdict non-Mixed le monde SAUTE GenerateMesh entièrement. Le contrat
// est asymétrique, et le commentaire de la fonction le dit déjà :
//
//     un faux Mixed ne coûte que du CPU ;
//     un faux AllSolid / AllAir est un TROU — pas de géométrie, PAS DE COLLISION, invisible
//     jusqu'à ce qu'un joueur tombe au travers.
//
// ClassifyTile answers "this tile is entirely solid / entirely air" BEFORE any sampling, and on a
// non-Mixed verdict the world SKIPS GenerateMesh completely. The contract is asymmetric:
// a false Mixed only costs CPU; a false AllSolid/AllAir is a HOLE — no geometry, NO COLLISION,
// invisible until a player falls through it.
//
// Cette fonction a DÉJÀ produit cette panne : la v1 de T1.d a été revertée le 2026-06-26 pour une
// borne de plafond pas assez conservative. Jusqu'ici elle n'est validée que par raisonnement.
// Ce test la valide par la force brute : pour chaque tuile jugée non-Mixed, on échantillonne le
// treillis des sommets de cellules que ClassifyTile couvre réellement (g = 0..CPA) et on vérifie
// que chaque point est bien du côté annoncé. La marge extérieure du mesher sert aux normales, pas
// au verdict uniforme.
//
// This function has ALREADY produced that failure: T1.d v1 was reverted on 2026-06-26 over a
// non-conservative ceiling bound. Until now it was validated by reasoning only. This test
// validates it by brute force: for every tile judged non-Mixed, sample the exact cell-vertex
// lattice covered by ClassifyTile (g = 0..CPA) and assert every point is on the claimed side. The
// mesher's outer normal halo is deliberately outside the classifier's uniformity contract.
//
// CONVENTION (VoxelMarchingCubesMesher.cpp:309, IsoLevel == 0):
//     D >= 0  ⇒ côté AIR   / air side
//     D <  0  ⇒ côté SOLIDE / solid side
// Le classifieur utilise exactement ces inégalités (cf. TestColumn), donc le test aussi.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "VoxelForgeTestFixture.h"
// Inclus ici DÉLIBÉRÉMENT : VoxelDensityOp.h n'est encore inclus par aucun .cpp, donc le
// compilateur ne le verrait jamais. Le fold qu'il définit prétend reproduire ClassifyTile — ce
// fichier est l'endroit naturel pour que cette prétention soit à la fois COMPILÉE et TESTÉE.
// Deliberately included here: VoxelDensityOp.h is not yet included by any .cpp, so the compiler
// would never see it. Its fold claims to reproduce ClassifyTile, so this is the natural place for
// that claim to be both compiled and tested.
#include "VoxelDensityOp.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FVoxelForgeClassifyTileTest,
    "VoxelForge.Determinism.ClassifyTileSoundness",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

namespace
{
    /** Tuiles balayées à la recherche d'un verdict non-Mixed (ClassifyTile est bon marché). */
    constexpr int32 NumTilesScanned = 600;

    /** Tuiles réellement brute-forcées (chacune ~(Cells+3)³ appels à GetDensityAt — cher). */
    constexpr int32 MaxTilesVerified = 24;

    struct FTileSpec
    {
        FIntVector Origin = FIntVector::ZeroValue;
        int32 Step = 1;
        int32 Cells = 16;
    };
}

bool FVoxelForgeClassifyTileTest::RunTest(const FString& Parameters)
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

    // Quelques carves : la garde diff-layer de ClassifyTile doit elle aussi être couverte, et
    // c'est la garde la plus facile à casser en ajoutant une feature (elle est globale, pas
    // par-archétype). / A few carves: ClassifyTile's diff-layer guard needs covering too, and it
    // is the guard most easily broken by a new feature since it is global rather than per-archetype.
    {
        FVoxelModification Mod;
        Mod.Shape    = EVoxelBrushShape::Sphere;
        Mod.Radius   = 10.0f;
        Mod.Strength = -12.0f;
        for (int32 k = 0; k < 4; ++k)
        {
            Mod.Center = FVector((float)(k * CHUNK_SIZE * 2), 0.0f,
                                 World.MidVoxelZ() + (float)(k * CHUNK_SIZE));
            World.DiffLayer->ApplyModification(Mod);
        }
    }

    // ── Balayage : trouver des tuiles où le classifieur ose un verdict. ──
    // Les origines suivent la géométrie réelle du clipmap : une tuile couvre Step*Cells voxels et
    // est alignée sur son propre pas. / Tile origins follow the real clipmap geometry: a tile
    // covers Step*Cells voxels and is aligned to its own extent.
    FRandomStream Rng(20260727);
    TArray<FTileSpec> ToVerify;
    int32 NumMixed = 0, NumAllSolid = 0, NumAllAir = 0;

    const int32 TopVoxelZ    = World.TopChunkZ    * CHUNK_SIZE;
    const int32 BottomVoxelZ = World.BottomChunkZ * CHUNK_SIZE;

    // La moitié des tuiles vise la strate SurfaceWorld : c'est le SEUL archétype dont ClassifyTile
    // sait prouver quoi que ce soit aujourd'hui (avec les gaps de bedrock), donc un tirage uniforme
    // sur tout le layout gaspillerait le budget en tuiles Mixed garanties.
    // Half the tiles target the SurfaceWorld strate: it is the ONLY archetype ClassifyTile can prove
    // anything about today (alongside bedrock gaps), so a uniform draw over the whole layout would
    // spend the budget on guaranteed-Mixed tiles.
    int32 SurfTopZ = 0, SurfBotZ = 0;
    const bool bHaveSurface = World.GetSlotVoxelZRange(FTestWorld::SlotSurfaceWorld, SurfTopZ, SurfBotZ);

    for (int32 t = 0; t < NumTilesScanned; ++t)
    {
        FTileSpec Spec;
        // Step 1/2/4 comme le clipmap ; Cells petit pour que la vérification brute reste tenable.
        Spec.Step  = 1 << Rng.RandRange(0, 2);
        Spec.Cells = (t % 8 == 0) ? CHUNK_SIZE : 16;
        const int32 Extent = Spec.Step * Spec.Cells;

        const bool bAimSurface = bHaveSurface && (t % 2 == 0);
        const int32 LoZ = bAimSurface ? SurfBotZ : BottomVoxelZ;
        const int32 HiZ = bAimSurface ? SurfTopZ : TopVoxelZ;
        // Division entière PLANCHER : en C++ la troncature va vers zéro, ce qui décalerait la
        // borne basse (négative) d'un extent vers le haut. / Integer FLOOR division: C++ truncates
        // toward zero, which would shift the negative low bound up by one extent.
        auto FloorDiv = [](int32 A, int32 B) { const int32 Q = A / B, R = A % B; return (R != 0 && (R < 0) != (B < 0)) ? Q - 1 : Q; };
        const int32 LoTile = FloorDiv(LoZ, Extent);
        const int32 HiTile = FMath::Max(LoTile, FloorDiv(HiZ, Extent));

        Spec.Origin = FIntVector(
            Rng.RandRange(-4, 4) * Extent,
            Rng.RandRange(-4, 4) * Extent,
            Rng.RandRange(LoTile, HiTile) * Extent);

        const EVoxelTileClass Verdict = Gen->ClassifyTile(Spec.Origin, Spec.Step, Spec.Cells);
        switch (Verdict)
        {
        case EVoxelTileClass::Mixed:    ++NumMixed;                                       break;
        case EVoxelTileClass::AllSolid: ++NumAllSolid; if (ToVerify.Num() < MaxTilesVerified) ToVerify.Add(Spec); break;
        case EVoxelTileClass::AllAir:   ++NumAllAir;   if (ToVerify.Num() < MaxTilesVerified) ToVerify.Add(Spec); break;
        }
    }

    AddInfo(FString::Printf(
        TEXT("ClassifyTile verdicts over %d scanned tiles: Mixed %d, AllSolid %d, AllAir %d ")
        TEXT("(brute-forcing %d of them). Cave archetypes now use the opt-in operator-stack ")
        TEXT("ClassifyBox fold; the global XY edge proof also skips proven outer-shell tiles. A low ")
        TEXT("non-Mixed count is expected: every uncertain case remains Mixed for safety."),
        NumTilesScanned, NumMixed, NumAllSolid, NumAllAir, ToVerify.Num()));

    if (ToVerify.Num() == 0)
    {
        AddError(TEXT("VACUOUS: not one scanned tile produced an AllSolid/AllAir verdict, so this ")
                 TEXT("test verified nothing. Either the fixture's layout has no SurfaceWorld/gap ")
                 TEXT("chunks in the sampled Z range, or T1.d has stopped emitting verdicts entirely ")
                 TEXT("(which would be a large silent perf regression). Widen the Z range before ")
                 TEXT("trusting a green run."));
        return false;
    }

    // ── Vérification par force brute du contrat de ClassifyTile. ──
    // ClassifyTile covers actual cell vertices only: g ∈ [0, CPA]. GenerateMesh may sample a
    // one-vertex outer halo for central-difference normals, but that halo cannot create a cell and
    // is intentionally not part of the uniform verdict.
    int32 NumHoles = 0;
    for (const FTileSpec& Spec : ToVerify)
    {
        const EVoxelTileClass Verdict = Gen->ClassifyTile(Spec.Origin, Spec.Step, Spec.Cells);
        if (Verdict == EVoxelTileClass::Mixed) { continue; }   // verdict instable ⇒ rien à prouver

        const int32 CPA     = FMath::Clamp(Spec.Cells, 2, CHUNK_SIZE);
        const int32 VertexCount = CPA + 1;
        const bool  bClaimsSolid = (Verdict == EVoxelTileClass::AllSolid);

        bool bTileBad = false;
        for (int32 gz = 0; gz < VertexCount && !bTileBad; ++gz)
        for (int32 gy = 0; gy < VertexCount && !bTileBad; ++gy)
        for (int32 gx = 0; gx < VertexCount && !bTileBad; ++gx)
        {
            const float X = (float)(Spec.Origin.X + gx * Spec.Step);
            const float Y = (float)(Spec.Origin.Y + gy * Spec.Step);
            const float Z = (float)(Spec.Origin.Z + gz * Spec.Step);
            const float D = Gen->GetDensityAt(X, Y, Z);

            // AllSolid  ⇒ tout le treillis doit être D <  0
            // AllAir    ⇒ tout le treillis doit être D >= 0
            const bool bAgrees = bClaimsSolid ? (D < 0.0f) : (D >= 0.0f);
            if (!bAgrees)
            {
                bTileBad = true;
                ++NumHoles;
                AddError(FString::Printf(
                    TEXT("HOLE: ClassifyTile said %s for tile origin (%d,%d,%d) Step=%d Cells=%d, ")
                    TEXT("but GetDensityAt(%.0f, %.0f, %.0f) = %.6g is on the %s side. This tile ")
                    TEXT("would be skipped by the mesher: no triangles and NO COLLISION where there ")
                    TEXT("should be a surface. This point is inside ClassifyTile's actual cell ")
                    TEXT("lattice, so find which classifier guard failed to fire for the feature."),
                    bClaimsSolid ? TEXT("AllSolid") : TEXT("AllAir"),
                    Spec.Origin.X, Spec.Origin.Y, Spec.Origin.Z, Spec.Step, Spec.Cells,
                    X, Y, Z, D, (D >= 0.0f) ? TEXT("AIR") : TEXT("SOLID")));
            }
        }
    }

    TestEqual(TEXT("no tile was classified uniform while containing a surface (a false verdict is a hole)"),
              NumHoles, 0);

    // ── Stabilité du verdict : ClassifyTile partage GSurfColCache avec GetDensityAt, donc le
    //    brute-force ci-dessus a réchauffé les caches. Re-classifier doit rendre le MÊME verdict.
    //    Verdict stability: ClassifyTile shares GSurfColCache with GetDensityAt, so the brute force
    //    above warmed the caches. Re-classifying must yield the SAME verdict.
    for (const FTileSpec& Spec : ToVerify)
    {
        const EVoxelTileClass A = Gen->ClassifyTile(Spec.Origin, Spec.Step, Spec.Cells);
        const EVoxelTileClass B = Gen->ClassifyTile(Spec.Origin, Spec.Step, Spec.Cells);
        if (A != B)
        {
            AddError(FString::Printf(
                TEXT("UNSTABLE VERDICT at tile (%d,%d,%d) Step=%d: two consecutive ClassifyTile ")
                TEXT("calls disagreed (%d vs %d). The classifier is reading state that GetDensityAt ")
                TEXT("mutates — the shared column cache is the prime suspect."),
                Spec.Origin.X, Spec.Origin.Y, Spec.Origin.Z, Spec.Step, (int32)A, (int32)B));
        }
    }

    return true;
}

//=============================================================================
// LE CHEMIN PILE D'OPÉRATEURS DE ClassifyTile — MÊME FORCE BRUTE, MONDE OPT-IN
//=============================================================================
// `ClassifyTile` rendait `Mixed` sans appel pour tout archétype de CAVE. Il consulte désormais
// `FVoxelOpStack::ClassifyBox` quand la strate a coché `bUseOperatorStack` — donc **un tout nouveau
// chemin peut faire sauter le maillage d'une tuile**, et son erreur est un TROU : pas de triangles,
// pas de collision, invisible jusqu'à ce qu'un joueur tombe au travers.
//
// Ce test est le même oracle par force brute que `ClassifyTileSoundness`, sur un monde dont TOUTES
// les strates ont coché la case. Il ne vérifie pas le pliage (c'est `BoxVerdictFold`) ni les
// opérateurs (ce sont les huit tests d'équivalence) : il vérifie le **câblage** — que la pile
// interrogée par le classifieur est bien celle qui produit la densité, params, drapeau et
// disturbances compris.
//
// ⚠️ LE COMPTEUR À LIRE EN PREMIER est le nombre de tuiles réellement brute-forcées. Un run vert
// avec zéro verdict non-Mixed ne prouverait RIEN — exactement le piège que ce fichier documente
// depuis sa première version, et la raison pour laquelle l'absence de verdict est une ERREUR ici.
//
// Same brute-force oracle as ClassifyTileSoundness, on a world where every strate has opted in.
// It checks the WIRING, not the fold and not the operators.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FVoxelForgeOpStackClassifyTileTest,
    "VoxelForge.OpStack.ClassifyTileSoundness",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVoxelForgeOpStackClassifyTileTest::RunTest(const FString& Parameters)
{
    using namespace VoxelForgeTest;

    // ⚠️ `bUseOperatorStack = true` sur toutes les strates : c'est LE point du test. La fixture
    // donne à ce monde une `LayoutVersion` unique dans le processus, sans quoi les caches par chunk
    // de `GetDensityAt` — dont `CP_UseOpStack` — pourraient encore porter ceux d'un autre test.
    FTestWorld World;
    World.Build(/*Seed*/1337, /*GapChunks*/2, /*bUseOperatorStack*/true);
    if (!World.IsValid())
    {
        AddError(World.WhyInvalid());
        return false;
    }

    const UVoxelGenerator* Gen = World.Generator.Get();

    FRandomStream Rng(20260728);
    TArray<FTileSpec> ToVerify;
    int32 NumMixed = 0, NumAllSolid = 0, NumAllAir = 0;

    const int32 TopVoxelZ    = World.TopChunkZ    * CHUNK_SIZE;
    const int32 BottomVoxelZ = World.BottomChunkZ * CHUNK_SIZE;

    // Tirage uniforme sur tout le layout, PAS biaisé vers SurfaceWorld comme l'autre test : ici ce
    // sont précisément les strates de cave qui intéressent, puisque ce sont elles qui passent par le
    // nouveau chemin. SurfaceWorld continue d'être prouvé par le code écrit à la main.
    for (int32 t = 0; t < NumTilesScanned; ++t)
    {
        FTileSpec Spec;
        Spec.Step  = 1 << Rng.RandRange(0, 2);
        Spec.Cells = (t % 8 == 0) ? CHUNK_SIZE : 16;
        const int32 Extent = Spec.Step * Spec.Cells;

        auto FloorDiv = [](int32 A, int32 B) { const int32 Q = A / B, R = A % B; return (R != 0 && (R < 0) != (B < 0)) ? Q - 1 : Q; };
        const int32 LoTile = FloorDiv(BottomVoxelZ, Extent);
        const int32 HiTile = FMath::Max(LoTile, FloorDiv(TopVoxelZ, Extent));

        Spec.Origin = FIntVector(
            Rng.RandRange(-4, 4) * Extent,
            Rng.RandRange(-4, 4) * Extent,
            Rng.RandRange(LoTile, HiTile) * Extent);

        const EVoxelTileClass Verdict = Gen->ClassifyTile(Spec.Origin, Spec.Step, Spec.Cells);
        switch (Verdict)
        {
        case EVoxelTileClass::Mixed:    ++NumMixed;                                                   break;
        case EVoxelTileClass::AllSolid: ++NumAllSolid; if (ToVerify.Num() < MaxTilesVerified) ToVerify.Add(Spec); break;
        case EVoxelTileClass::AllAir:   ++NumAllAir;   if (ToVerify.Num() < MaxTilesVerified) ToVerify.Add(Spec); break;
        }
    }

    AddInfo(FString::Printf(
        TEXT("ClassifyTile ON THE OPERATOR-STACK PATH, %d scanned tiles: Mixed %d, AllSolid %d, ")
        TEXT("AllAir %d (brute-forcing %d). Compare with VoxelForge.Determinism.ClassifyTileSoundness, ")
        TEXT("which runs the SAME scan on a world that has NOT opted in: every verdict beyond what ")
        TEXT("that test reports is a tile the mesher now skips and did not before. That difference ")
        TEXT("IS the T1.d prize OPSTACK-PLAN has been aiming at -- and every one of those tiles is a ")
        TEXT("hole if the wiring is wrong, which is what the brute force below is for."),
        NumTilesScanned, NumMixed, NumAllSolid, NumAllAir, ToVerify.Num()));

    if (ToVerify.Num() == 0)
    {
        AddError(TEXT("VACUOUS: not one tile got a non-Mixed verdict on the operator-stack path, so ")
                 TEXT("this test verified NOTHING about the new wiring. Either no strate actually ")
                 TEXT("opted in (check FTestWorld::Build's bUseOperatorStack), or every guard in the ")
                 TEXT("cave branch of ClassifyTile bailed to Mixed -- the params-identical check and ")
                 TEXT("the 27-chunk-coord cap are the likeliest. Do NOT read a green run as proof."));
        return false;
    }

    int32 NumHoles = 0;
    for (const FTileSpec& Spec : ToVerify)
    {
        const EVoxelTileClass Verdict = Gen->ClassifyTile(Spec.Origin, Spec.Step, Spec.Cells);
        if (Verdict == EVoxelTileClass::Mixed) { continue; }

        const int32 CPA     = FMath::Clamp(Spec.Cells, 2, CHUNK_SIZE);
        const int32 VertexCount = CPA + 1;
        const bool  bClaimsSolid = (Verdict == EVoxelTileClass::AllSolid);

        bool bTileBad = false;
        for (int32 gz = 0; gz < VertexCount && !bTileBad; ++gz)
        for (int32 gy = 0; gy < VertexCount && !bTileBad; ++gy)
        for (int32 gx = 0; gx < VertexCount && !bTileBad; ++gx)
        {
            const float X = (float)(Spec.Origin.X + gx * Spec.Step);
            const float Y = (float)(Spec.Origin.Y + gy * Spec.Step);
            const float Z = (float)(Spec.Origin.Z + gz * Spec.Step);
            const float D = Gen->GetDensityAt(X, Y, Z);

            const bool bAgrees = bClaimsSolid ? (D < 0.0f) : (D >= 0.0f);
            if (!bAgrees)
            {
                bTileBad = true;
                ++NumHoles;
                AddError(FString::Printf(
                    TEXT("HOLE ON THE OPERATOR-STACK PATH: ClassifyTile said %s for tile (%d,%d,%d) ")
                    TEXT("Step=%d Cells=%d, but GetDensityAt(%.0f, %.0f, %.0f) = %.6g is on the %s ")
                    TEXT("side. Check, in order: (1) does GetDensityAt for this chunk actually take ")
                    TEXT("the stack (CP_UseOpStack), or did the classifier judge a field the mesher ")
                    TEXT("will not produce; (2) the params-identical check -- a blended transition ")
                    TEXT("band means one stack cannot represent the whole tile (AUDIT C2); (3) the ")
                    TEXT("disturbance fold, since disturbances are applied AFTER the stack and are ")
                    TEXT("not part of it; (4) an operator's EffectOverBox claiming Identity where it ")
                    TEXT("can act -- the per-room op override can ENABLE a modifier the strate had ")
                    TEXT("switched off, which makes a box bound too optimistic."),
                    bClaimsSolid ? TEXT("AllSolid") : TEXT("AllAir"),
                    Spec.Origin.X, Spec.Origin.Y, Spec.Origin.Z, Spec.Step, Spec.Cells,
                    X, Y, Z, D, (D >= 0.0f) ? TEXT("AIR") : TEXT("SOLID")));
            }
        }
    }

    TestEqual(TEXT("no tile was classified uniform on the operator-stack path while containing a ")
              TEXT("surface (a false verdict is a hole)"), NumHoles, 0);

    // Même contrôle de stabilité que sur l'autre chemin : la pile est reconstruite à chaque appel,
    // et `FRoomGraphSource` partage un cache `thread_local` avec le chemin densité — deux appels
    // successifs doivent malgré tout rendre le même verdict.
    for (const FTileSpec& Spec : ToVerify)
    {
        const EVoxelTileClass A = Gen->ClassifyTile(Spec.Origin, Spec.Step, Spec.Cells);
        const EVoxelTileClass B = Gen->ClassifyTile(Spec.Origin, Spec.Step, Spec.Cells);
        if (A != B)
        {
            AddError(FString::Printf(
                TEXT("UNSTABLE VERDICT on the operator-stack path at tile (%d,%d,%d) Step=%d: %d vs ")
                TEXT("%d. The classifier builds a fresh stack per call, so a difference means an ")
                TEXT("operator is reading thread_local state the density path mutates."),
                Spec.Origin.X, Spec.Origin.Y, Spec.Origin.Z, Spec.Step, (int32)A, (int32)B));
        }
    }

    return true;
}

//=============================================================================
// LE FOLD DE LA PILE D'OPÉRATEURS / the op-stack fold
//=============================================================================
// `VoxelDensityOp.h` affirme que son fold reproduit le ClassifyTile écrit à la main. C'est de la
// logique pure — pas de monde, pas de bruit, pas de thread — donc elle peut être vérifiée
// exhaustivement, et elle doit l'être : c'est elle qui décidera un jour si une tuile est maillée.
//
// `VoxelDensityOp.h` claims its fold reproduces the hand-written ClassifyTile. That is pure logic —
// no world, no noise, no threads — so it can be checked exhaustively, and it should be: this is what
// will one day decide whether a tile gets meshed at all.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FVoxelForgeOpFoldTest,
    "VoxelForge.OpStack.BoxVerdictFold",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVoxelForgeOpFoldTest::RunTest(const FString& Parameters)
{
    // Un état neuf ne prouve rien ⇒ Mixed (les deux hypothèses vivantes = égalité = prudence).
    {
        FVoxelBoxHypotheses H;
        TestEqual(TEXT("a fresh state proves nothing"), (int32)H.Resolve(), (int32)EVoxelTileClass::Mixed);
    }

    // Une source qui affirme un côté tue l'autre hypothèse.
    {
        FVoxelBoxHypotheses H;
        VF_ForceHypotheses(H, EVoxelTileClass::AllSolid);
        TestEqual(TEXT("a solid source yields AllSolid"), (int32)H.Resolve(), (int32)EVoxelTileClass::AllSolid);
        VF_FoldEffect(H, EVoxelOpEffect::Identity);
        TestEqual(TEXT("Identity changes nothing"), (int32)H.Resolve(), (int32)EVoxelTileClass::AllSolid);
    }

    // ≡ « AnyPassageNearBox ⇒ bCanSolid = false » : un carve tue AllSolid.
    {
        FVoxelBoxHypotheses H;
        VF_ForceHypotheses(H, EVoxelTileClass::AllSolid);
        VF_FoldEffect(H, EVoxelOpEffect::CarveOnly);
        TestEqual(TEXT("a passage over solid rock forces Mixed"), (int32)H.Resolve(), (int32)EVoxelTileClass::Mixed);
    }

    // ≡ « bande de seal ⇒ bCanAir = false » : un fill tue AllAir.
    {
        FVoxelBoxHypotheses H;
        VF_ForceHypotheses(H, EVoxelTileClass::AllAir);
        VF_FoldEffect(H, EVoxelOpEffect::FillOnly);
        TestEqual(TEXT("a fill over open air forces Mixed"), (int32)H.Resolve(), (int32)EVoxelTileClass::Mixed);
    }

    // LE CAS QUI JUSTIFIE ClassifyBox : au-dessus du terrain mais DANS la bande de seal supérieure,
    // la source dit « tout air » et le seal FORCE « tout solide ». Aujourd'hui ClassifyTile rend
    // AllSolid ici. Un simple FillOnly rendrait Mixed et perdrait la tuile.
    // THE CASE THAT JUSTIFIES ClassifyBox — a pure FillOnly would lose this tile.
    {
        FVoxelBoxHypotheses H;
        VF_ForceHypotheses(H, EVoxelTileClass::AllAir);      // source: above the terrain
        VF_ForceHypotheses(H, EVoxelTileClass::AllSolid);    // seal: forcing, inside its band
        TestEqual(TEXT("a forcing seal recovers AllSolid over an air source"),
                  (int32)H.Resolve(), (int32)EVoxelTileClass::AllSolid);

        // …et un passage qui traverse cette même boîte la reprend, exactement comme aujourd'hui.
        VF_FoldEffect(H, EVoxelOpEffect::CarveOnly);
        TestEqual(TEXT("a passage still takes the sealed verdict back"),
                  (int32)H.Resolve(), (int32)EVoxelTileClass::Mixed);
    }

    // Le diff layer : Both tue tout, ce qui est le comportement voulu (une édition joueur peut
    // creuser OU remplir n'importe où).
    {
        FVoxelBoxHypotheses H;
        VF_ForceHypotheses(H, EVoxelTileClass::AllSolid);
        VF_FoldEffect(H, EVoxelOpEffect::Both);
        TestTrue(TEXT("Both kills every hypothesis"), H.IsDead());
        TestEqual(TEXT("a player edit in range forces Mixed"), (int32)H.Resolve(), (int32)EVoxelTileClass::Mixed);
    }

    // Une source qui ne sait rien (Mixed) ne peut jamais être ressuscitée par un opérateur
    // directionnel — seul un opérateur FORÇANT le peut. C'est la propriété de sûreté.
    {
        for (const EVoxelOpEffect E : { EVoxelOpEffect::Identity, EVoxelOpEffect::CarveOnly,
                                        EVoxelOpEffect::FillOnly, EVoxelOpEffect::Both })
        {
            FVoxelBoxHypotheses H;
            VF_ForceHypotheses(H, EVoxelTileClass::Mixed);
            VF_FoldEffect(H, E);
            TestEqual(TEXT("a directional op can never resurrect an unprovable box"),
                      (int32)H.Resolve(), (int32)EVoxelTileClass::Mixed);
        }
    }

    //=========================================================================
    // LE PLIAGE NUMÉRIQUE — `OPSTACK-DECOMPOSITION §0.2`
    //=========================================================================
    // La direction seule ne récupère jamais un carve FIELDÉ : il peut creuser partout, donc il rend
    // `CarveOnly` partout, et c'est VRAI. Ce que la direction ignore, c'est qu'il ne peut creuser que
    // de `WormStrength` au plus. Le pliage porte donc deux nombres : une MARGE posée par l'opérateur
    // forçant, et une AMPLITUDE retirée par chaque carve.
    //
    // ⚠️ LE PREMIER CONTRÔLE CI-DESSOUS EST LE PLUS IMPORTANT : il vérifie que la rétro-compatibilité
    // est réelle. Les cinq blocs au-dessus n'ont pas changé d'une ligne et doivent rester verts —
    // ils appellent les mêmes fonctions sans marge ni amplitude, donc avec les défauts
    // (`Margin = 0`, `MaxCarve = FLT_MAX`), qui reproduisent le comportement purement directionnel.

    // 1. Les défauts REPRODUISENT l'ancien pliage — c'est ce qui rend le changement sûr.
    {
        FVoxelBoxHypotheses H;
        VF_ForceHypotheses(H, EVoxelTileClass::AllSolid);   // marge par défaut = 0
        VF_FoldEffect(H, EVoxelOpEffect::CarveOnly);        // amplitude par défaut = FLT_MAX
        TestEqual(TEXT("with default margin and amplitude, a carve still kills AllSolid"),
                  (int32)H.Resolve(), (int32)EVoxelTileClass::Mixed);
    }

    // 2. Une amplitude INCONNUE tue même une grosse marge. « Je ne sais pas » n'est pas « zéro ».
    {
        FVoxelBoxHypotheses H;
        VF_ForceHypotheses(H, EVoxelTileClass::AllSolid, 1000.0f);
        VF_FoldEffect(H, EVoxelOpEffect::CarveOnly);        // FLT_MAX
        TestEqual(TEXT("an unbounded carve kills AllSolid however solid the rock is"),
                  (int32)H.Resolve(), (int32)EVoxelTileClass::Mixed);
    }

    // 3. LE CAS QUI JUSTIFIE TOUT : roc à 1.0, ver à 0.6 ⇒ il reste 0.4 de marge, la boîte est
    //    prouvablement pleine. C'est exactement `FConstantFieldSource` + `FWormFieldSource`.
    {
        FVoxelBoxHypotheses H;
        VF_ForceHypotheses(H, EVoxelTileClass::AllSolid, 1.0f);    // BaseDensity
        VF_FoldEffect(H, EVoxelOpEffect::CarveOnly, 0.6f, 0.0f);   // WormStrength
        TestEqual(TEXT("rock solid by more than the worm can carve stays provably AllSolid"),
                  (int32)H.Resolve(), (int32)EVoxelTileClass::AllSolid);

        // …et les carves S'ACCUMULENT : un second à 0.5 fait passer la marge sous zéro.
        VF_FoldEffect(H, EVoxelOpEffect::CarveOnly, 0.5f, 0.0f);
        TestEqual(TEXT("carve amplitudes accumulate until the margin runs out"),
                  (int32)H.Resolve(), (int32)EVoxelTileClass::Mixed);
    }

    // 4. ÉGALITÉ ⇒ ON PERD LA TUILE, délibérément. Une marge de 1.0 contre un carve de 1.0 peut
    //    atteindre exactement zéro, et zéro est du côté AIR pour le mesher. Le test `> 0` est
    //    STRICT, et il doit le rester : se tromper ici ferait un trou, pas une tuile en trop.
    {
        FVoxelBoxHypotheses H;
        VF_ForceHypotheses(H, EVoxelTileClass::AllSolid, 1.0f);
        VF_FoldEffect(H, EVoxelOpEffect::CarveOnly, 1.0f, 0.0f);
        TestEqual(TEXT("a carve exactly equal to the margin loses the tile (strict >, on purpose)"),
                  (int32)H.Resolve(), (int32)EVoxelTileClass::Mixed);
    }

    // 5. Le miroir côté AIR : une strate d'îles flottantes, surtout vide, avec un remplissage borné.
    {
        FVoxelBoxHypotheses H;
        VF_ForceHypotheses(H, EVoxelTileClass::AllAir, 1.0f);
        VF_FoldEffect(H, EVoxelOpEffect::FillOnly, 0.0f, 0.35f);
        TestEqual(TEXT("air deeper than the fill can reach stays provably AllAir"),
                  (int32)H.Resolve(), (int32)EVoxelTileClass::AllAir);
    }

    // 6. `Both` BORNÉ : la rugosité de paroi peut aller dans les deux sens, mais pas loin. Sur du
    //    roc forcé, l'hypothèse AIR est déjà morte (le forçage l'a tuée) ; ce qui compte est que
    //    l'hypothèse SOLIDE survive à un `Both` d'amplitude connue — impossible avant ce changement,
    //    où `Both` tuait tout inconditionnellement.
    {
        FVoxelBoxHypotheses H;
        VF_ForceHypotheses(H, EVoxelTileClass::AllSolid, 1.0f);
        VF_FoldEffect(H, EVoxelOpEffect::Both, 0.3f, 0.3f);
        TestEqual(TEXT("a bounded Both no longer kills a margin it cannot cross"),
                  (int32)H.Resolve(), (int32)EVoxelTileClass::AllSolid);
    }

    // 7. LA PROPRIÉTÉ DE SÛRETÉ TIENT TOUJOURS : rien de borné ne ressuscite quoi que ce soit.
    //    Le pliage numérique ne fait que retarder la mort d'une hypothèse, jamais l'annuler.
    {
        for (const EVoxelOpEffect E : { EVoxelOpEffect::Identity, EVoxelOpEffect::CarveOnly,
                                        EVoxelOpEffect::FillOnly, EVoxelOpEffect::Both })
        {
            FVoxelBoxHypotheses H;
            VF_ForceHypotheses(H, EVoxelTileClass::Mixed, 1000.0f);   // marge ignorée sur Mixed
            VF_FoldEffect(H, E, 0.0f, 0.0f);                          // amplitudes NULLES
            TestEqual(TEXT("a zero-amplitude op cannot resurrect an unprovable box either"),
                      (int32)H.Resolve(), (int32)EVoxelTileClass::Mixed);
        }
    }

    // 8. Une marge NULLE avec une amplitude NULLE : le carve ne retire rien, mais `0 > 0` est faux,
    //    donc l'hypothèse meurt quand même. C'est voulu — une marge inconnue reste inconnue, et un
    //    opérateur qui ne fait rien devrait rendre `Identity`, pas `CarveOnly` d'amplitude 0.
    {
        FVoxelBoxHypotheses H;
        VF_ForceHypotheses(H, EVoxelTileClass::AllSolid, 0.0f);
        VF_FoldEffect(H, EVoxelOpEffect::CarveOnly, 0.0f, 0.0f);
        TestEqual(TEXT("zero margin dies even to a zero carve -- unknown is not zero"),
                  (int32)H.Resolve(), (int32)EVoxelTileClass::Mixed);
    }

    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
