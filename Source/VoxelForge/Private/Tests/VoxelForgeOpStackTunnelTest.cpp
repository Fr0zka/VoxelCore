// VoxelForgeOpStackTunnelTest.cpp
// TunnelNetwork — ÉTAPE A (squelette SDF) + ÉTAPE B COMPLÈTE (les douze modificateurs, 4b–4h).
// TunnelNetwork — STAGE A (the SDF spine) + ALL OF STAGE B (the twelve detail modifiers, 4b-4h).
//
// POURQUOI UN TEST D'UNE PILE INCOMPLÈTE
// `GetDensityWithParams` fait ~1080 lignes et douze modificateurs de détail (le chiffre « treize »
// traînait dans les notes ; il y en a douze, et onze seulement lisent la copie de params par salle).
// Tout porter avant de
// pouvoir rien vérifier, ce serait écrire ~600 lignes non compilées par-dessus ~200 non vérifiées —
// exactement le motif que `AUDIT §P3` documente et que ce refactor a évité six fois de suite.
//
// La sortie : **tous les modificateurs de détail sont pilotés par une amplitude**, et
// `FStrateGenerationParams` les laisse déjà TOUS à zéro par défaut (`BuildParamsFromDefinition` ne
// les fusionne plus globalement — ils viennent d'ops par salle). Une seule exception,
// `SurfaceRoughness = 5`. Les mettre à zéro fait passer l'ORIGINAL par exactement le chemin que
// l'étape A a porté, donc l'étape A est vérifiable AUJOURD'HUI, bit à bit, contre la vraie fonction.
// Même discipline que la passe « défauts puis tous les ops ON » du test de pile de hauteur, prise
// dans l'autre sens.
//
// L'ÉTAPE B A REMONTÉ CES AMPLITUDES UN GROUPE À LA FOIS, dans l'autre sens : chaque groupe porté sortait
// de la liste des amplitudes éteintes pour entrer dans `EnableTunnelFeatures`, avec (i) une sonde de couverture
// qui prouve qu'il a réellement bougé quelque chose et (ii) le compte d'ops de la pile qui augmente.
//   • B1 : rugosité de paroi, STEP 4b.
//   • B2 : terrasses, lignes de strates, nervures — STEP 4c.
//   • B3 : surplombs, falaise, festons, arches — STEP 4c.
//   • B4 (ce commit) : colonnes (4d), dômes (4g), pincement (4h), biais de sol. La liste des
//     amplitudes éteintes est vide : **l'étape B est complète**.
//
// CE QUE CE TEST NE PROUVE PAS (et le dit) : rien sur l'override d'op PAR SALLE — onze des douze
// modificateurs lisent, dans l'original, une copie des params où l'op de la salle la plus proche a
// été appliqué, et la pile lit les params de la strate. Les deux coïncident **uniquement** parce que
// le pool du test ne contient que Pit / Chimney / Column, dont les `ApplyTo` n'écrivent aucun champ
// de détail. Un op `Terrace` dans ce pool casserait l'équivalence : c'est exactement le test que C1
// ajoutera, et c'est la seule preuve possible de l'override. Rien non plus sur le saut de tuile —
// `FRoomGraphSource::EffectOverBox` rend `Both`.
//
// ⚠️ ÉCHANTILLONNAGE PAR GRAPPES, PAS UNIFORME. Le cache SDF se reconstruit quand la requête sort de
// sa boîte de recherche ; 20 000 points uniformément aléatoires feraient ~20 000 `BuildChunkCache`
// par chemin, et un test qui dure trois minutes est un test qu'on finit par ne plus lancer. On tire
// donc N chunks et M points DANS chacun — ce qui est aussi plus représentatif du vrai motif d'accès
// (un mesher parcourt une tuile, il ne saute pas au hasard).

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Async/ParallelFor.h"
#include "HAL/PlatformMisc.h"

#include "VoxelForgeTestFixture.h"
#include "VoxelDensityOpStack.h"
#include "VoxelTerrainOpDefinition.h"   // pits/chimneys n'existent QUE via un op par salle
#include "VoxelCaveMorphology.h"        // FChunkSDFCache — le contrôle 3b regarde la cuisson

#include <atomic>

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FVoxelForgeOpStackTunnelTest,
    "VoxelForge.OpStack.TunnelNetworkSpineEquivalence",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

namespace
{
    constexpr int32 NumTunnelChunks  = 24;
    constexpr int32 PointsPerChunk   = 250;
    constexpr int32 NumTunnelSamples = NumTunnelChunks * PointsPerChunk;

    //=========================================================================
    // ⚠️ `DisableStageBModifiers` A DISPARU, ET SA DISPARITION EST LE RÉSULTAT DE L'ÉTAPE B
    //=========================================================================
    // Cette fonction mettait à zéro tout ce que le portage n'avait pas encore atteint, pour que
    // l'original emprunte le même chemin que la pile. À chaque groupe (B1…B4) une ligne en sortait
    // pour entrer dans `EnableTunnelFeatures` ET dans `FeatureProbes` ; à B4 il n'en restait plus.
    // Sa liste vide EST la mesure de l'avancement — d'où le fait de le dire ici plutôt que de
    // laisser une fonction vide qu'on continuerait d'appeler sans y penser.
    //
    // Une ligne y était de toute façon inopérante et mérite d'être notée : `ColumnDensity = 0`
    // n'éteignait rien. La cuisson des colonnes lit `OpParams` (un struct NEUF où seul l'op de la
    // salle a été appliqué), jamais le champ de la strate — même mécanisme que pour les pits. Ce qui
    // tenait les colonnes éteintes à l'étape A, c'était l'ABSENCE d'op `Column` dans le pool, et le
    // garde-fou qui ERREUR-ait si la cuisson en produisait quand même. B4 ajoute l'op au pool et
    // retourne ce garde-fou : il EXIGE maintenant des colonnes.

    /**
     * ⚠️ DENSIFIÉ après le premier run vert. Aux défauts (`RoomSpacing = 80`, `RoomDensity = 0.35`)
     * le premier passage a rendu **65 échantillons en grotte sur 6000, soit 1,1 %** : bit-identique,
     * oui, mais en comparant surtout du roc plein à du roc plein, là où le carve et les vers ne
     * s'exécutent même pas. Le compteur avait été écrit exactement pour dire ça, et il l'a dit ;
     * l'avertissement, lui, ne se déclenchait qu'à ZÉRO. Les deux sont corrigés. → 21 %.
     *
     * ⚠️⚠️ CE QUI N'EST **PAS** ICI : `PitDensity` et `ChimneyDensity`. Les y mettre était une
     * ERREUR, et le contrôle 3b l'a attrapée (0 point sur 1500 bougeait en les remettant à zéro).
     * `BuildChunkCache` ouvre sa cuisson par `if (!CR.RoomOp) continue;` puis lit `OpParams`, un
     * `FStrateGenerationParams` NEUF sur lequel seul l'op de la salle a été appliqué. **Les champs
     * pit/cheminée/colonne de la strate ne sont donc jamais lus** — ces primitives n'existent QUE
     * via un `UVoxelTerrainOpDefinition` tiré par salle. (Ils n'ont pas d'`UPROPERTY` sur
     * `FStrateGenerationParams`, donc ce n'est pas un piège d'éditeur : c'est un struct de transport,
     * pas un réglage. Rien à corriger côté produit — c'était ma lecture qui était fausse.)
     *
     * NOT here: PitDensity / ChimneyDensity. Setting them was a mistake that check 3b caught — the
     * bake reads a FRESH param struct with only the room's op applied, so those fields are never
     * read. Pits exist only through a per-room terrain-op asset. See MakeRoomOpPool below.
     */
    void EnableTunnelFeatures(FStrateGenerationParams& P)
    {
        P.RoomSpacing     = 42.0f;   // 80 → 42 : des salles à portée de chaque chunk échantillonné
        P.RoomDensity     = 0.85f;   // 0.35 → 0.85
        P.VerticalScale   = 1.35f;   // ≠ 1 ⇒ le Z « effectif » diverge du Z monde partout

        // ── ÉTAPE B1 : rugosité de paroi (4b) ────────────────────────────────────────────────
        // Écrits EXPLICITEMENT, pas laissés au défaut : un test qui dépend d'un défaut se casse en
        // silence le jour où le défaut change. `SurfaceRoughness` était le seul de ces champs non nul
        // par défaut (5.0), et l'étape A le remettait à zéro — c'est ce zéro qui disparaît ici.
        P.SurfaceRoughness    = 5.0f;
        P.RoughnessFrequency  = 0.1f;
        P.RoughnessNoiseType  = EVoxelNoiseType::FBM;   // les 4 types sont balayés au contrôle 1c
        P.DomainWarpStrength  = 3.0f;                   // ≠ 0 ⇒ le chemin de warp de domaine est pris
        P.DomainWarpFrequency = 0.03f;

        // ── ÉTAPE B2 : les trois remaniements « sédimentaires » (4c) ──────────────────────────
        // Espacements CHOISIS PETITS devant la fenêtre d'échantillonnage : `LineRange` vaut
        // `Spacing · 1.5`, donc un espacement de 30 ne laisserait presque aucun point dans la
        // fenêtre et la sonde de couverture rapporterait un quasi-zéro pour la mauvaise raison.
        P.TerraceStepHeight        = 6.0f;
        P.TerraceHardness          = 0.6f;
        P.TerraceNoiseDisplacement = 0.5f;   // ≠ 0 ⇒ le bruit de déplacement du palier est pris
        P.LayerLineSpacing         = 5.0f;
        P.LayerLineDepth           = 0.35f;
        P.RibbingSpacing           = 4.0f;
        P.RibbingDepth             = 0.4f;

        // ── ÉTAPE B3 : surplombs · falaise · festons · arches (4c) ───────────────────────────
        P.OverhangStrength = 0.6f;
        P.OverhangDepth    = 5.0f;
        P.OverhangFrequency= 0.06f;
        P.CliffStrength    = 0.5f;
        P.ScallopStrength  = 0.8f;
        P.ScallopFrequency = 0.1f;
        // ⚠️ ArchDensity BEAUCOUP plus haut que le défaut (0.1) : chaque salle tire 3 arches et
        // n'en garde que celles dont `hash01 <= ArchDensity`. À 0.1, ~0.3 arche par salle et une
        // fenêtre d'influence de 2 voxels autour d'une capsule — la sonde de couverture rapporterait
        // zéro sans que rien ne soit faux. La couverture d'un test n'est pas la valeur de prod.
        P.ArchDensity      = 0.9f;
        P.ArchMinRadius    = 3.0f;
        P.ArchMaxRadius    = 6.0f;

        // ── ÉTAPE B4 : dômes · pincement · biais de sol (4g, 4h) ─────────────────────────────
        // ⚠️ LES COLONNES (4d) NE SONT PAS ICI, et ce n'est pas un oubli : elles n'ont AUCUN
        // paramètre de strate. Ni la cuisson ni la boucle par voxel ne lisent `ColumnDensity` ;
        // elles n'existent que par un op `Column` dans le pool (voir `MakeRoomOpPool`) et leur
        // couverture se prouve au contrôle 3b, pas par une sonde de params.
        P.DomeDensity      = 0.9f;    // 2 tirages par salle ⇒ il en faut beaucoup pour couvrir
        P.DomeMinRadius    = 8.0f;
        P.DomeMaxRadius    = 15.0f;
        P.DomeHeightRatio  = 0.8f;
        P.PinchDensity     = 0.9f;    // idem, 3 tirages par salle
        P.PinchStrength    = 5.0f;
        P.PinchLength      = 12.0f;
        P.FloorBias        = 0.8f;    // ne s'applique que dans l'air certain (CaveSDF < 0)
    }

    /**
     * Le SEUL moyen d'obtenir des pits, des cheminées et des colonnes : donner à la strate un pool
     * d'ops de terrain, que `BuildChunkCache` tire par salle. Trois entrées à `Probability = 1.0` ⇒
     * `TotalOpProb = 3`, `NormFactor = 1/3`, curseurs à 1/3, 2/3, 1 : **chaque salle reçoit un op**,
     * un tiers de chaque sorte.
     *
     * ⚠️⚠️ CE POOL DOIT RESTER Pit / Chimney / Column JUSQU'À L'ÉTAPE C1, ET LA RAISON EST SUBTILE.
     * L'override par salle du chemin d'origine applique l'op de la salle sur une COPIE des params,
     * et ONZE des douze modificateurs de détail lisent cette copie (la rugosité 4b, non : le shadow
     * est déclaré après elle). L'étape B les a portés contre les params de la STRATE. Les deux ne
     * coïncident que tant qu'aucune salle ne porte un op de type DÉTAIL — or `ApplyTo(Pit)` n'écrit
     * que les quatre champs de pit, `ApplyTo(Chimney)` que les quatre de cheminée, `ApplyTo(Column)`
     * que les trois de colonne. Aucun champ de détail touché ⇒ aucun modificateur ne diverge.
     *
     * **Mettre un op `Terrace` ici casserait l'équivalence — et c'est exactement le test que C1
     * ajoutera**, parce que c'est la seule preuve possible de l'override par salle.
     *
     * The pool must stay Pit/Chimney/Column until C1: eleven of the twelve modifiers read the
     * per-room param copy in the original and the strate params here, and those agree only while no
     * room carries a detail-type op. A Terrace entry is what C1 adds, deliberately.
     *
     * @param OutKeepAlive  les assets transitoires, à garder vivants pour la durée du test.
     */
    void MakeRoomOpPool(UVoxelStrateDefinition* Def,
                        TArray<TStrongObjectPtr<UVoxelTerrainOpDefinition>>& OutKeepAlive)
    {
        UVoxelTerrainOpDefinition* PitOp = NewObject<UVoxelTerrainOpDefinition>(
            GetTransientPackage(), NAME_None, RF_Transient);
        PitOp->Type          = EVoxelTerrainOpType::Pit;
        PitOp->PitDensity    = 0.9f;    // par salle, pas par cellule de hash : on en veut vraiment
        PitOp->PitMinRadius  = 5.0f;
        PitOp->PitMaxRadius  = 11.0f;
        PitOp->PitDepth      = 22.0f;
        OutKeepAlive.Add(TStrongObjectPtr<UVoxelTerrainOpDefinition>(PitOp));

        UVoxelTerrainOpDefinition* ChimOp = NewObject<UVoxelTerrainOpDefinition>(
            GetTransientPackage(), NAME_None, RF_Transient);
        ChimOp->Type             = EVoxelTerrainOpType::Chimney;
        ChimOp->ChimneyDensity   = 0.9f;
        ChimOp->ChimneyMinRadius = 3.0f;
        ChimOp->ChimneyMaxRadius = 6.0f;
        ChimOp->ChimneyHeight    = 18.0f;
        OutKeepAlive.Add(TStrongObjectPtr<UVoxelTerrainOpDefinition>(ChimOp));

        // ⚠️ AJOUTÉ À L'ÉTAPE B4 : sans cette entrée, `SDFCache.Columns` reste VIDE et l'opérateur
        // de colonnes ne s'exécute sur rien — ce que le contrôle 3b exigeait à l'étape A et qu'il
        // exige désormais dans l'autre sens.
        UVoxelTerrainOpDefinition* ColOp = NewObject<UVoxelTerrainOpDefinition>(
            GetTransientPackage(), NAME_None, RF_Transient);
        ColOp->Type            = EVoxelTerrainOpType::Column;
        ColOp->ColumnDensity   = 0.9f;
        ColOp->ColumnMinRadius = 2.0f;
        ColOp->ColumnMaxRadius = 5.0f;
        OutKeepAlive.Add(TStrongObjectPtr<UVoxelTerrainOpDefinition>(ColOp));

        // `Probability = 1.0` sur les trois : la somme dépasse 1, donc la sélection est normalisée
        // et chaque salle tire exactement un op parmi les trois, à parts égales.
        FStrateTerrainOpEntry PitEntry;
        PitEntry.Operation   = TSoftObjectPtr<UVoxelTerrainOpDefinition>(PitOp);
        PitEntry.Weight      = 1.0f;
        PitEntry.Probability = 1.0f;

        FStrateTerrainOpEntry ChimEntry;
        ChimEntry.Operation   = TSoftObjectPtr<UVoxelTerrainOpDefinition>(ChimOp);
        ChimEntry.Weight      = 1.0f;
        ChimEntry.Probability = 1.0f;

        FStrateTerrainOpEntry ColEntry;
        ColEntry.Operation   = TSoftObjectPtr<UVoxelTerrainOpDefinition>(ColOp);
        ColEntry.Weight      = 1.0f;
        ColEntry.Probability = 1.0f;

        Def->TerrainOperations.Reset();
        Def->TerrainOperations.Add(PitEntry);
        Def->TerrainOperations.Add(ChimEntry);
        Def->TerrainOperations.Add(ColEntry);
    }

    /** Fraction minimale d'échantillons devant tomber en grotte ouverte pour que l'équivalence
     *  signifie quelque chose. 10 % est modeste et très au-dessus du 1,1 % observé. */
    constexpr float MinCaveFraction = 0.10f;

    //=========================================================================
    // COUVERTURE PAR GROUPE — une entrée par groupe de l'étape B
    //=========================================================================
    // ⚠️ LA LEÇON DES PITS, GÉNÉRALISÉE : **activer une fonctionnalité n'est pas une preuve qu'elle
    // a tiré.** `PitDensity = 0.55` n'a rien fait pendant tout un run (mauvais struct) et le test
    // restait vert. Ici la question « le groupe B_n a-t-il changé quelque chose ? » se pose de la
    // seule façon qui ne puisse répondre juste par hasard : reconstruire la pile avec CE groupe
    // éteint et COMPTER LES POINTS QUI BOUGENT.
    //
    // ⚠️⚠️ POURQUOI CE DIFF-DE-PILES EST LÉGITIME ICI ALORS QU'IL AURAIT MENTI POUR LES PITS :
    // le pool d'ops de terrain n'est PAS dans la clé du cache SDF, donc deux piles n'en différant
    // que par lui se servaient le même cache `thread_local` et rendaient exactement la même chose.
    // Les champs ci-dessous, eux, sont des params — et l'empreinte CRC des params EST dans la clé.
    // Deux piles qui n'en diffèrent que par un de ces champs se reconstruisent donc bien chacune.
    //
    // Enabling a feature is not evidence it fired. Each entry rebuilds the stack with that group
    // OFF and counts moved points. Legitimate here (unlike for the op pool) because these are
    // params, and the params CRC is part of the SDF cache key.
    struct FFeatureProbe
    {
        const TCHAR* Name;
        void (*Disable)(FStrateGenerationParams&);   // lambda sans capture ⇒ pointeur de fonction
    };

    const FFeatureProbe FeatureProbes[] =
    {
        { TEXT("B1 surface roughness (STEP 4b)"),
          [](FStrateGenerationParams& Q) { Q.SurfaceRoughness = 0.0f; } },
        { TEXT("B2 terracing (STEP 4c)"),
          [](FStrateGenerationParams& Q) { Q.TerraceStepHeight = 0.0f; } },
        { TEXT("B2 layer lines (STEP 4c)"),
          [](FStrateGenerationParams& Q) { Q.LayerLineSpacing = 0.0f; } },
        { TEXT("B2 ribbing (STEP 4c)"),
          [](FStrateGenerationParams& Q) { Q.RibbingSpacing = 0.0f; } },
        { TEXT("B3 cave overhangs (STEP 4c)"),
          [](FStrateGenerationParams& Q) { Q.OverhangStrength = 0.0f; } },
        { TEXT("B3 cliff sharpening (STEP 4c)"),
          [](FStrateGenerationParams& Q) { Q.CliffStrength = 0.0f; } },
        { TEXT("B3 scallop (STEP 4c)"),
          [](FStrateGenerationParams& Q) { Q.ScallopStrength = 0.0f; } },
        { TEXT("B3 arches (STEP 4c)"),
          [](FStrateGenerationParams& Q) { Q.ArchDensity = 0.0f; } },
        { TEXT("B4 domes (STEP 4g)"),
          [](FStrateGenerationParams& Q) { Q.DomeDensity = 0.0f; } },
        { TEXT("B4 pinch (STEP 4h)"),
          [](FStrateGenerationParams& Q) { Q.PinchDensity = 0.0f; } },
        { TEXT("B4 floor bias"),
          [](FStrateGenerationParams& Q) { Q.FloorBias = 0.0f; } },
        // ⚠️ PAS DE SONDE POUR LES COLONNES (STEP 4d), et ce n'est pas un oubli : elles n'ont aucun
        // paramètre de strate qu'on puisse éteindre. Leur couverture se prouve au contrôle 3b, en
        // demandant à la cuisson combien elle en a produit. Voir la note là-bas.
        // ⚠️ UNE SONDE PAR OPÉRATEUR, PAS UNE PAR GROUPE. Le groupe B2 en contient trois ; une seule
        // sonde « B2 » serait verte tant qu'UN des trois tire, et les deux autres pourraient être
        // faux sans que rien ne le dise. La granularité de la sonde est la granularité de la preuve.
    };

    //=========================================================================
    // LE `switch` SUR LE TYPE DE BRUIT — quatre branches, et une seule serait testée
    //=========================================================================
    // L'équivalence principale tourne en FBM. Les trois autres branches (Ridged, Mixed, Cellular)
    // et les deux chemins de warp de domaine ne seraient JAMAIS exécutés — une transcription fausse
    // dans `case Cellular:` passerait tout l'étage B sans un mot. Ce balayage les prend une par une.
    struct FRoughVariant
    {
        const TCHAR*     Name;
        EVoxelNoiseType  Type;
        float            WarpStrength;
    };

    const FRoughVariant RoughVariants[] =
    {
        { TEXT("FBM, no domain warp"),      EVoxelNoiseType::FBM,      0.0f },
        { TEXT("FBM, domain warp"),         EVoxelNoiseType::FBM,      3.0f },
        { TEXT("Ridged, no domain warp"),   EVoxelNoiseType::Ridged,   0.0f },
        { TEXT("Ridged, domain warp"),      EVoxelNoiseType::Ridged,   3.0f },
        { TEXT("Mixed, no domain warp"),    EVoxelNoiseType::Mixed,    0.0f },
        { TEXT("Mixed, domain warp"),       EVoxelNoiseType::Mixed,    3.0f },
        { TEXT("Cellular, no domain warp"), EVoxelNoiseType::Cellular, 0.0f },
        { TEXT("Cellular, domain warp"),    EVoxelNoiseType::Cellular, 3.0f },
    };

    /** Le balayage ne relit pas les 6000 points : les branches de bruit sont par-voxel et sans
     *  état, donc un sous-ensemble les couvre autant. Ce qui coûte, c'est la reconstruction du
     *  cache SDF à chaque changement de chunk, et elle est proportionnelle aux chunks visités. */
    constexpr int32 RoughSweepPoints = 1500;
}

bool FVoxelForgeOpStackTunnelTest::RunTest(const FString& Parameters)
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

    // ⚠️ AVANT LA MOINDRE ÉVALUATION. Le pool d'ops est lu au moment où `BuildChunkCache` construit
    // le cache ; le muter après coup laisserait un cache `thread_local` déjà chaud, bâti sans pits,
    // et les deux chemins ne seraient plus comparables. On le pose donc pendant que rien n'est chaud.
    TArray<TStrongObjectPtr<UVoxelTerrainOpDefinition>> OpAssets;
    if (World.Definitions.IsValidIndex(FTestWorld::SlotTunnelNetwork)
        && World.Definitions[FTestWorld::SlotTunnelNetwork].IsValid())
    {
        MakeRoomOpPool(World.Definitions[FTestWorld::SlotTunnelNetwork].Get(), OpAssets);
    }
    else
    {
        AddError(TEXT("The fixture has no definition object for the TunnelNetwork slot, so no ")
                 TEXT("terrain-op pool could be attached -- pits and chimneys would silently not ")
                 TEXT("exist and check 3b would report zero for the wrong reason."));
        return false;
    }

    int32 TopVoxelZ = 0, BottomVoxelZ = 0;
    if (!World.GetSlotVoxelZRange(FTestWorld::SlotTunnelNetwork, TopVoxelZ, BottomVoxelZ))
    {
        AddError(TEXT("The fixture layout has no TunnelNetwork slot. Check FTestWorld::Build's ")
                 TEXT("Archetypes[] against FTestWorld::SlotTunnelNetwork."));
        return false;
    }

    const int32 MidChunkZ = ((TopVoxelZ + BottomVoxelZ) / 2) / CHUNK_SIZE;
    FStrateGenerationParams P = World.StrateManager->GetGenerationParams(FIntVector(0, 0, MidChunkZ));

    if (P.StrateTopWorldZ - P.StrateBottomWorldZ <= 0.0f)
    {
        AddError(TEXT("The TunnelNetwork strate has degenerate Z bounds."));
        return false;
    }

    EnableTunnelFeatures(P);

    FVoxelOpStack Stack;
    VoxelDensityOps::BuildTunnelNetworkStack(Stack, P, World.Settings->Seed,
                                             Gen->OriginSpineRadius, World.StrateManager.Get());

    // rock + roomgraph + carve + **les 12 modificateurs de détail** + worms + 3 structurels = 19.
    // L'étape C n'ajoute AUCUN opérateur — elle change ce que onze d'entre eux LISENT — donc ce
    // nombre DOIT bouger à chaque groupe de l'étape B — c'est un compteur de progression, pas une
    // formalité : une pile qui ne grandit pas est une pile dont l'opérateur n'a pas été ajouté.
    TestEqual(TEXT("the stage-A+B tunnel stack is decomposed into 19 ops"), Stack.Num(), 19);

    FVoxelOpContext Ctx;
    Ctx.Seed               = (uint32)World.Settings->Seed;
    Ctx.LayoutVersion      = World.StrateManager->GetLayoutVersion();
    Ctx.StrateTopWorldZ    = P.StrateTopWorldZ;
    Ctx.StrateBottomWorldZ = P.StrateBottomWorldZ;
    Stack.PrepareChunk(Ctx);

    // Grappes : N chunks, M points dans chacun. Voir l'en-tête — un tirage uniforme ferait
    // reconstruire le cache SDF à presque chaque point, sur les DEUX chemins.
    TArray<FVector> Points;
    Points.Reserve(NumTunnelSamples);
    {
        FRandomStream Rng(1080601);
        const int32 ChunkZ0 = BottomVoxelZ / CHUNK_SIZE;
        const int32 ChunkZ1 = FMath::Max(ChunkZ0, (TopVoxelZ / CHUNK_SIZE) - 1);
        for (int32 c = 0; c < NumTunnelChunks; ++c)
        {
            const int32 CX = Rng.RandRange(-3, 3);
            const int32 CY = Rng.RandRange(-3, 3);
            const int32 CZ = Rng.RandRange(ChunkZ0, ChunkZ1);
            for (int32 i = 0; i < PointsPerChunk; ++i)
            {
                Points.Add(FVector(
                    (float)(CX * CHUNK_SIZE + Rng.RandRange(0, CHUNK_SIZE - 1)),
                    (float)(CY * CHUNK_SIZE + Rng.RandRange(0, CHUNK_SIZE - 1)),
                    (float)FMath::Clamp(CZ * CHUNK_SIZE + Rng.RandRange(0, CHUNK_SIZE - 1),
                                        BottomVoxelZ, TopVoxelZ)));
            }
        }
    }

    //=========================================================================
    // 1. ÉQUIVALENCE
    //=========================================================================
    const float InnerBot = P.StrateBottomWorldZ + P.BoundarySealThickness;
    const float InnerTop = P.StrateTopWorldZ    - P.BoundarySealThickness;

    int32 NumDiff = 0, NumSideDisagree = 0, WorstIdx = -1;
    int32 NumInCave = 0, NumInRock = 0;
    float WorstDelta = 0.0f;

    // Gardé pour le contrôle 1b : la référence « pile complète » que chaque sonde de couverture
    // compare à une pile dont UN groupe est éteint. Rempli ici pour ne pas repayer une passe.
    TArray<float> FullVals;
    FullVals.SetNumUninitialized(NumTunnelSamples);

    for (int32 i = 0; i < NumTunnelSamples; ++i)
    {
        const float X = (float)Points[i].X, Y = (float)Points[i].Y, Z = (float)Points[i].Z;

        const float Old = Gen->GetDensityWithParams(X, Y, Z, P);
        const float New = Stack.EvalMC(X, Y, Z);
        FullVals[i] = New;

        const bool bInterior = (Z > InnerBot && Z < InnerTop);
        if (bInterior && Old >= 0.0f) { ++NumInCave; }   // air loin des seals ⇒ salle/tunnel/ver
        if (bInterior && Old <  0.0f) { ++NumInRock; }

        if (!BitEqual(Old, New))
        {
            ++NumDiff;
            const float D = FMath::Abs(Old - New);
            if (D > WorstDelta) { WorstDelta = D; WorstIdx = i; }
        }
        if ((Old >= 0.0f) != (New >= 0.0f)) { ++NumSideDisagree; }
    }

    if (NumDiff == 0)
    {
        AddInfo(FString::Printf(
            TEXT("TunnelNetwork STAGE A+B: bit-identical across %d samples in %d chunks (%d in ")
            TEXT("open cave, %d in rock, away from the seal bands). Exercised: vertical scale (1.35, ")
            TEXT("so effective Z differs from world Z everywhere), cave warp, the room/tunnel SDF via ")
            TEXT("the SHARED BuildChunkCache, the carve with its floored divisor, the ported detail ")
            TEXT("modifiers of 4b-4c, and the worm carve with its network mask. Every one of those ")
            TEXT("is covered ONLY insofar as the bake-coverage and group-coverage lines below report ")
            TEXT("non-zero -- this message used to CLAIM coverage outright, and was wrong for a ")
            TEXT("whole run. NOT covered at all: whatever is still listed in ")
            TEXT("the per-room op override (stage C1), and any tile verdict."),
            NumTunnelSamples, NumTunnelChunks, NumInCave, NumInRock));

        AddInfo(FString::Printf(
            TEXT("Cave coverage: %.1f%% of samples are in open cave (floor %.0f%%). This is the ")
            TEXT("number that says whether the equivalence MEANS anything -- the carve, the pits, ")
            TEXT("the chimneys and the worm carve only execute near the network, so a run dominated ")
            TEXT("by deep rock would be green while proving almost nothing."),
            100.0f * (float)NumInCave / (float)NumTunnelSamples, 100.0f * MinCaveFraction));
    }
    else
    {
        AddError(FString::Printf(
            TEXT("TunnelNetwork STAGE A+B: %d of %d samples differ (largest |delta| %.9g at ")
            TEXT("(%.0f, %.0f, %.0f)); %d cross the isosurface. Check, in order: the carve's ")
            TEXT("MinDivisor (TunnelNetwork floors Blend*2 at 1.0 and the other archetypes do NOT ")
            TEXT("-- getting this wrong only shows up when SDFBlendRadius*2 < 1), then EffectiveZ ")
            TEXT("(VerticalScale must divide BEFORE the warp and the worms, and must NOT touch pit ")
            TEXT("or chimney Z), then the pit/chimney loops reading UNWARPED coords while the room ")
            TEXT("SDF reads warped ones, then the SDF cache key (it now includes a params ")
            TEXT("fingerprint the original lacks -- that can cost a rebuild, never a wrong room), ")
            TEXT("then the worm early-out on N1 >= threshold. NEW AT B1, so suspect these first: ")
            TEXT("the roughness gate is bNearCaveSurface (SDF < BlendRadius*3) AND ")
            TEXT("|SDF| < SurfaceRoughness*2 -- two different windows; the domain warp adds ONE ")
            TEXT("shared offset to BOTH noise positions (two independent warps would be tidier and ")
            TEXT("wrong); the fine octave set is frequency*3 with +2000/+2500/+3000 offsets; the ")
            TEXT("anti-fill clamp is min(TotalRough, 0) only where SDF < 0; and the fade is ")
            TEXT("quadratic in DistFromSurface/RoughnessDepth. Roughness also reads EffectiveZ, ")
            TEXT("not WorldZ."),
            NumDiff, NumTunnelSamples, WorstDelta,
            WorstIdx >= 0 ? Points[WorstIdx].X : 0.0f,
            WorstIdx >= 0 ? Points[WorstIdx].Y : 0.0f,
            WorstIdx >= 0 ? Points[WorstIdx].Z : 0.0f,
            NumSideDisagree));
    }

    TestEqual(TEXT("no sample lands on the opposite side of the isosurface"), NumSideDisagree, 0);

    // ⚠️ SEUIL EN FRACTION, PAS « > 0 ». La version « == 0 » de ce garde-fou a laissé passer un run
    // à 1,1 % en silence. Un test qui ne se plaint qu'au zéro absolu ne mesure pas la couverture,
    // il constate seulement qu'elle n'est pas vide.
    if ((float)NumInCave < MinCaveFraction * (float)NumTunnelSamples)
    {
        AddWarning(FString::Printf(
            TEXT("Only %.1f%% of samples landed in open cave (want >= %.0f%%): the carve, the pits, ")
            TEXT("the chimneys and the worm carve were barely exercised, so the equivalence above ")
            TEXT("mostly compares solid rock to solid rock. Lower RoomSpacing or raise RoomDensity ")
            TEXT("in EnableTunnelFeatures."),
            100.0f * (float)NumInCave / (float)NumTunnelSamples, 100.0f * MinCaveFraction));
    }

    //=========================================================================
    // 1b. CHAQUE GROUPE DE L'ÉTAPE B A-T-IL RÉELLEMENT TIRÉ ?
    //=========================================================================
    // Voir `FeatureProbes` : on rebâtit la pile avec un groupe éteint et on compte les points qui
    // BOUGENT. Zéro ⇒ l'équivalence ci-dessus ne dit rien de ce groupe, quelle que soit sa couleur.
    // C'est une ERREUR, pas un avertissement : un groupe porté et jamais exécuté est exactement
    // l'état dans lequel une faute de transcription traverse tout un run sans se faire voir.
    for (const FFeatureProbe& Probe : FeatureProbes)
    {
        FStrateGenerationParams PWithout = P;
        Probe.Disable(PWithout);

        FVoxelOpStack StackWithout;
        VoxelDensityOps::BuildTunnelNetworkStack(StackWithout, PWithout, World.Settings->Seed,
                                                 Gen->OriginSpineRadius, World.StrateManager.Get());
        StackWithout.PrepareChunk(Ctx);

        int32 NumMoved = 0;
        for (int32 i = 0; i < NumTunnelSamples; ++i)
        {
            const float V = StackWithout.EvalMC((float)Points[i].X, (float)Points[i].Y,
                                                (float)Points[i].Z);
            if (!BitEqual(V, FullVals[i])) { ++NumMoved; }
        }

        if (NumMoved > 0)
        {
            AddInfo(FString::Printf(
                TEXT("Group coverage -- %s: %d of %d samples (%.1f%%) move when this group is ")
                TEXT("switched off, so the equivalence above genuinely covers it."),
                Probe.Name, NumMoved, NumTunnelSamples,
                100.0f * (float)NumMoved / (float)NumTunnelSamples));
        }
        else
        {
            AddError(FString::Printf(
                TEXT("Group coverage -- %s: ZERO of %d samples move when this group is switched ")
                TEXT("off. The group contributed NOTHING to the 6000-sample equivalence, so that ")
                TEXT("equivalence says nothing about it. Either its params never reach the op (the ")
                TEXT("PitDensity mistake, second time), or its gate never opens at these sample ")
                TEXT("points. Do not read the green equivalence as covering this group."),
                Probe.Name, NumTunnelSamples));
        }
    }

    //=========================================================================
    // 1c. LES QUATRE BRANCHES DU `switch` DE BRUIT, ET LES DEUX CHEMINS DE WARP
    //=========================================================================
    // L'équivalence principale ne prend QU'UNE branche (FBM). Une transcription fausse dans
    // `case Cellular:` ou `case Mixed:` la traverserait sans un mot. Chaque variante est donc
    // comparée à l'original sur un sous-ensemble des mêmes points.
    {
        int32 TotalVariantDiffs = 0;
        for (const FRoughVariant& V : RoughVariants)
        {
            FStrateGenerationParams PV = P;
            PV.RoughnessNoiseType = V.Type;
            PV.DomainWarpStrength = V.WarpStrength;

            FVoxelOpStack VStack;
            VoxelDensityOps::BuildTunnelNetworkStack(VStack, PV, World.Settings->Seed,
                                                     Gen->OriginSpineRadius, World.StrateManager.Get());
            VStack.PrepareChunk(Ctx);

            int32 VDiff = 0;
            for (int32 i = 0; i < RoughSweepPoints; ++i)
            {
                const float X = (float)Points[i].X, Y = (float)Points[i].Y, Z = (float)Points[i].Z;
                if (!BitEqual(Gen->GetDensityWithParams(X, Y, Z, PV), VStack.EvalMC(X, Y, Z)))
                {
                    ++VDiff;
                }
            }

            TotalVariantDiffs += VDiff;
            if (VDiff > 0)
            {
                AddError(FString::Printf(
                    TEXT("Roughness variant '%s': %d of %d samples differ from the original. Only ")
                    TEXT("this branch of the noise switch is implicated -- the other variants and ")
                    TEXT("the main equivalence use the same code either side of it."),
                    V.Name, VDiff, RoughSweepPoints));
            }
        }

        if (TotalVariantDiffs == 0)
        {
            AddInfo(FString::Printf(
                TEXT("Roughness noise sweep: all %d variants (FBM / Ridged / Mixed / Cellular, each ")
                TEXT("with and without the domain warp) are bit-identical over %d samples. This is ")
                TEXT("what makes the three unused branches of the 4b switch mean anything -- the ")
                TEXT("main equivalence only ever takes the FBM one."),
                (int32)UE_ARRAY_COUNT(RoughVariants), RoughSweepPoints));
        }
    }

    //=========================================================================
    // 2. INVARIANCE DE FENÊTRE — le test qui compte le plus sur cet archétype
    //=========================================================================
    // `BuildChunkCache` porte la discipline à deux régions de `ARCHITECTURE §8.4` : c'est LE endroit
    // du plugin où un cache mal clé produit une couture visible entre deux tuiles. La pile ajoute sa
    // propre clé par-dessus (boîte + strate + seed + empreinte de params + version de layout), donc
    // c'est cette clé-là que ce bloc met à l'épreuve : mêmes points, ordre mélangé, N threads.
    {
        std::atomic<int32> Impure{ 0 };
        const int32 NumBlocks = FMath::Max(4, FMath::Min(16, FPlatformMisc::NumberOfCores()));

        TArray<float> Ref;
        Ref.SetNumUninitialized(NumTunnelSamples);
        for (int32 i = 0; i < NumTunnelSamples; ++i)
        {
            Ref[i] = Stack.EvalMC((float)Points[i].X, (float)Points[i].Y, (float)Points[i].Z);
        }

        ParallelFor(NumBlocks, [&](int32 Block)
        {
            TArray<int32> LocalOrder;
            BuildShuffledOrder(NumTunnelSamples, 4400 + Block, LocalOrder);
            for (const int32 i : LocalOrder)
            {
                const float V = Stack.EvalMC((float)Points[i].X, (float)Points[i].Y, (float)Points[i].Z);
                if (!BitEqual(V, Ref[i])) { Impure.fetch_add(1, std::memory_order_relaxed); }
            }
        });

        TestEqual(TEXT("the tunnel stack is window-invariant across order and threads"),
                  Impure.load(), 0);
    }

    //=========================================================================
    // 3. LE CACHE NE PEUT PAS SERVIR LES PARAMS DU VOISIN
    //=========================================================================
    // La régression d'overhang du 2026-07-27 : deux piles dans la MÊME strate, au MÊME seed, ne
    // différant QUE par des params, partageaient un cache `thread_local` dont la clé ignorait les
    // params — et la seconde lisait les salles de la première. La pile clé donc aussi sur une
    // empreinte CRC des params. Ce bloc le vérifie en ALTERNANT A, B, A, B au même point, le motif
    // qui fait mentir une clé incomplète.
    //
    // ⚠️⚠️ ON NE COMPARE **PAS** À L'ORIGINAL ICI, ET C'EST LE POINT LE PLUS IMPORTANT DE CE TEST.
    // `GetDensityWithParams` clé son cache sur (boîte XY, strate, seed) — **sans les params**. En
    // alternance il rendrait donc, pour B, les salles de A : l'original ÉCHOUERAIT ce contrôle. Le
    // comparer à lui ici ne mesurerait pas mon opérateur, ça mesurerait son bug. On compare donc
    // chaque pile à ELLE-MÊME évaluée seule — un oracle qui ne partage pas le défaut testé.
    //
    // ⚠️ ET CE N'EST PEUT-ÊTRE PAS QU'UN ARTEFACT DE TEST — à vérifier, pas à croire. En production
    // `GetGenerationParams` MÉLANGE les params entre strates voisines (transitions Gradient), donc
    // deux chunks de Z différents dans la même strate peuvent avoir des params différents, avec la
    // même boîte XY, le même index de strate et le même seed ⇒ aucune reconstruction. Si c'est
    // exact, un worker qui descend une bande de transition sert les salles du chunk précédent.
    // Noté dans `AUDIT §C2` comme SUSPECTÉ, avec le test qui le confirmerait — pas comme prouvé.
    //
    // We compare each stack to ITSELF evaluated alone, not to the original: the original keys its
    // SDF cache without the params and would fail this check, so comparing against it would measure
    // its bug rather than this operator.
    {
        FStrateGenerationParams P2 = P;
        P2.RoomSpacing = P.RoomSpacing * 0.6f;    // une autre disposition de salles
        P2.RoomDensity = FMath::Min(P.RoomDensity * 1.7f, 1.0f);

        FVoxelOpStack Stack2;
        VoxelDensityOps::BuildTunnelNetworkStack(Stack2, P2, World.Settings->Seed,
                                                 Gen->OriginSpineRadius, World.StrateManager.Get());
        Stack2.PrepareChunk(Ctx);

        // Chaque pile compte 2 reconstructions de cache par point en alternance (elles partagent le
        // `thread_local`), donc on reste modeste sur le nombre de sondes : `BuildChunkCache` est la
        // fonction la plus chère du plugin.
        const int32 Probe = FMath::Min(400, NumTunnelSamples);

        TArray<float> SoloA, SoloB;
        SoloA.SetNumUninitialized(Probe);
        SoloB.SetNumUninitialized(Probe);
        for (int32 i = 0; i < Probe; ++i)
        {
            SoloA[i] = Stack.EvalMC((float)Points[i].X, (float)Points[i].Y, (float)Points[i].Z);
        }
        for (int32 i = 0; i < Probe; ++i)
        {
            SoloB[i] = Stack2.EvalMC((float)Points[i].X, (float)Points[i].Y, (float)Points[i].Z);
        }

        int32 NumWrong = 0, NumActuallyDifferent = 0;
        for (int32 i = 0; i < Probe; ++i)
        {
            const float X = (float)Points[i].X, Y = (float)Points[i].Y, Z = (float)Points[i].Z;
            const float GotA = Stack.EvalMC(X, Y, Z);
            const float GotB = Stack2.EvalMC(X, Y, Z);

            if (!BitEqual(GotA, SoloA[i]) || !BitEqual(GotB, SoloB[i])) { ++NumWrong; }
            if (!BitEqual(SoloA[i], SoloB[i])) { ++NumActuallyDifferent; }
        }

        TestEqual(TEXT("two tunnel stacks with different params never serve each other's rooms"),
                  NumWrong, 0);

        AddInfo(FString::Printf(
            TEXT("Params-fingerprint check: %d of %d probe points (%.1f%%) genuinely differ between ")
            TEXT("the two param sets, and %d were served wrong under A/B interleaving. The FIRST ")
            TEXT("number is the check's real strength: only those points could ever reveal a stale ")
            TEXT("cache, so it is a count of how many times the question was actually asked."),
            NumActuallyDifferent, Probe, 100.0f * (float)NumActuallyDifferent / (float)Probe,
            NumWrong));

        // Le premier run a donné 3 sur 400 (0,75 %) : 397 sondes ne pouvaient RIEN distinguer. Même
        // correction que la couverture de grotte — un seuil en fraction, pas « non nul ».
        if ((float)NumActuallyDifferent < 0.05f * (float)Probe)
        {
            AddWarning(FString::Printf(
                TEXT("Only %d of %d probe points differ between the two param sets (want >= 5%%), so ")
                TEXT("this check asked its question %d times, not %d. A stale cache would go ")
                TEXT("unnoticed at every other point. Make P2 differ more, or probe nearer the ")
                TEXT("network."),
                NumActuallyDifferent, Probe, NumActuallyDifferent, Probe));
        }
    }

    //=========================================================================
    // 3b. LES PITS ET LES CHEMINÉES ONT-ILS RÉELLEMENT CONTRIBUÉ ?
    //=========================================================================
    // `§2` désigne ces deux boucles comme « le plus retors de toute la décomposition » : elles
    // écrivent le MÊME canal SDF que le graphe de salles mais à des coordonnées NON warpées. Les
    // activer dans les params ne prouve pas qu'elles ont changé quoi que ce soit — un test peut très
    // bien être vert avec `SDFCache.Pits` vide.
    //
    // ⚠️ ON INTERROGE LA CUISSON, PAS LA DENSITÉ — et le premier essai a échoué exactement là.
    // La v1 de ce contrôle reconstruisait la pile avec `PitDensity = ChimneyDensity = 0` et comptait
    // les points qui bougent : **0 sur 1500**. Cause : ces champs-là ne sont jamais lus. La v2 par
    // « pile sans pits » ne marche pas non plus, pour une raison plus subtile — le pool d'ops n'est
    // PAS dans la clé du cache SDF (en production c'est `LayoutVersion` qui couvre son édition), donc
    // deux piles ne différant que par le pool se serviraient le même cache `thread_local`.
    //
    // On demande donc directement à `BuildChunkCache` ce qu'elle a cuit. C'est la structure dont la
    // vacuité était la cause suspectée : autant la regarder plutôt que d'en inférer l'existence
    // depuis une densité. Aucun risque de clé, aucun ordre à respecter.
    //
    // Ask the bake what it baked, instead of inferring it from a density: the suspected cause was an
    // empty SDFCache.Pits, and that is a structure this test can simply look at. (v1 zeroed params
    // nothing reads; a "stack without pits" would share the thread_local cache, since the op pool is
    // not in its key — LayoutVersion covers pool edits in production.)
    {
        int32 StrateIdx = 0;
        {
            const int32 QZ = FMath::FloorToInt((float)((TopVoxelZ + BottomVoxelZ) / 2) / (float)CHUNK_SIZE);
            StrateIdx = World.StrateManager->GetStrateIndex(
                ((float)QZ + 0.5f) * CHUNK_SIZE * VOXEL_SIZE);
        }

        const UVoxelStrateDefinition* Def = World.Definitions[FTestWorld::SlotTunnelNetwork].Get();

        int32 TotalPits = 0, TotalChimneys = 0, TotalRooms = 0, TotalColumns = 0;
        for (int32 c = 0; c < 6; ++c)
        {
            const float Expansion = P.CaveWarpStrength + 2.0f;
            const float MinX = (c - 3) * (float)CHUNK_SIZE - Expansion;
            const float MinY = (c - 3) * (float)CHUNK_SIZE - Expansion;

            FChunkSDFCache ProbeCache;
            VoxelCaveMorphology::BuildChunkCache(
                ProbeCache, MinX, MinY, MinX + CHUNK_SIZE + 2.0f * Expansion,
                MinY + CHUNK_SIZE + 2.0f * Expansion,
                P, (uint32)World.Settings->Seed, StrateIdx, &Def->TerrainOperations);

            TotalRooms     += ProbeCache.Rooms.Num();
            TotalPits      += ProbeCache.Pits.Num();
            TotalChimneys  += ProbeCache.Chimneys.Num();
            TotalColumns   += ProbeCache.Columns.Num();
        }

        AddInfo(FString::Printf(
            TEXT("Bake coverage over 6 search boxes: %d rooms, %d pits, %d chimneys, %d columns. ")
            TEXT("Pits and chimneys are the two loops OPSTACK-DECOMPOSITION 2 calls the fiddliest ")
            TEXT("thing in the decomposition (unwarped coords SmoothMin'd into the warped room SDF); ")
            TEXT("a zero here means the 6000-sample equivalence above says NOTHING about them, ")
            TEXT("whatever colour it reports."),
            TotalRooms, TotalPits, TotalChimneys, TotalColumns));

        TestTrue(TEXT("the bake produced rooms at all"), TotalRooms > 0);
        TestTrue(TEXT("the bake produced pits, so the pit loop has data to run on"), TotalPits > 0);
        TestTrue(TEXT("the bake produced chimneys, so the chimney loop has data to run on"),
                 TotalChimneys > 0);

        // ⚠️ CE GARDE-FOU S'EST RETOURNÉ À L'ÉTAPE B4, ET C'EST LE SEUL MOYEN DE COUVRIR LES
        // COLONNES. Il ERREUR-ait si la cuisson produisait des colonnes (l'étape A n'avait pas porté
        // `STEP 4d`) ; il EXIGE maintenant qu'elle en produise. Il n'y a pas d'alternative par
        // paramètre : ni la cuisson ni la boucle par voxel ne lisent `FStrateGenerationParams::
        // ColumnDensity`, donc aucune sonde de `FeatureProbes` ne peut éteindre les colonnes — et
        // une pile bâtie sur un pool SANS op `Column` partagerait le cache `thread_local` (le pool
        // n'est pas dans sa clé) et rendrait exactement la même densité, c'est-à-dire mentirait.
        // Reste à demander à la cuisson ce qu'elle a cuit. Troisième application de la même leçon.
        TestTrue(TEXT("the bake produced columns, so the STEP 4d loop has data to run on ")
                 TEXT("(no params probe can cover this one -- see the comment above)"),
                 TotalColumns > 0);
    }

    //=========================================================================
    // 4. LE VERDICT DE BOÎTE — attendu NUL, et c'est le point
    //=========================================================================
    {
        int32 NumProved = 0, NumMixed = 0;
        FRandomStream Rng(97531);
        for (int32 t = 0; t < 40; ++t)
        {
            const int32 Step = 1, Cells = 8;
            const int32 Extent = Step * Cells;
            const FIntVector Origin(
                Rng.RandRange(-4, 4) * Extent,
                Rng.RandRange(-4, 4) * Extent,
                FMath::Clamp(Rng.RandRange(BottomVoxelZ / Extent, TopVoxelZ / Extent), -4096, 4096) * Extent);
            const int32 GridDim = Cells + 1;
            const FBox Box(
                FVector(Origin.X - Step, Origin.Y - Step, Origin.Z - Step),
                FVector(Origin.X + GridDim * Step, Origin.Y + GridDim * Step, Origin.Z + GridDim * Step));

            if (Stack.ClassifyBox(Box, Ctx) == EVoxelTileClass::Mixed) { ++NumMixed; }
            else { ++NumProved; }
        }

        AddInfo(FString::Printf(
            TEXT("Box verdicts over 40 TunnelNetwork tiles: %d proved, %d Mixed. %d proved is the ")
            TEXT("EXPECTED result at stage A and not a defect: the room source answers Both (its ")
            TEXT("bounds live in the SDF cache, which it would have to build for the queried box), ")
            TEXT("and the worm source answers CarveOnly EVERYWHERE because a fielded noise carve ")
            TEXT("has no spatial bound at all. Recovering these needs the numeric amplitude cap in ")
            TEXT("OPSTACK-DECOMPOSITION 0.2 -- the largest single perf item in the whole plan, and ")
            TEXT("the reason this archetype currently skips zero tiles."),
            NumProved, NumMixed, NumProved));

        TestEqual(TEXT("stage A emits no unsound verdict (it emits none at all)"), NumProved, 0);
    }

    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
