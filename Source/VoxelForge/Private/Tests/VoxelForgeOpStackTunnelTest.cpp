// VoxelForgeOpStackTunnelTest.cpp
// TunnelNetwork ET Underwater — étapes A + B + C : les deux archétypes, entiers.
// TunnelNetwork AND Underwater — stages A + B + C: both archetypes, whole.
//
// POURQUOI UN TEST D'UNE PILE INCOMPLÈTE
// `GetDensityWithParams` fait ~1080 lignes et douze modificateurs de détail (le chiffre « treize »
// traînait dans les notes ; il y en a douze, et onze seulement lisent la copie de params par salle).
// Tout porter avant de pouvoir rien vérifier, ce serait écrire ~600 lignes non compilées par-dessus ~200 non vérifiées —
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
// L'ÉTAPE B A REMONTÉ CES AMPLITUDES UN GROUPE À LA FOIS, dans l'autre sens : chaque groupe porté
// sortait de la liste des amplitudes éteintes pour entrer dans `EnableTunnelFeatures`, avec (i) une
// sonde de couverture prouvant qu'il a réellement bougé quelque chose, et (ii) le compte d'ops qui monte.
//   • B1 : rugosité de paroi, STEP 4b.
//   • B2 : terrasses, lignes de strates, nervures — STEP 4c.
//   • B3 : surplombs, falaise, festons, arches — STEP 4c.
//   • B4 : colonnes (4d), dômes (4g), pincement (4h), biais de sol. La liste des amplitudes
//     éteintes est vide : les douze modificateurs sont portés.
//   • B5 : le GATE lui-même. Aucun opérateur ajouté — la décision (early-out répété par opérateur
//     plutôt que conteneur de portée) est écrite dans `VF_NearCaveSurface`, et le contrôle 1d la
//     paie : hors gate, la pile complète doit être BIT À BIT celle sans modificateurs.
//   • C1 : l'override d'op PAR SALLE. Aucun opérateur ajouté non plus — onze des douze
//     modificateurs lisent désormais `FRoomGraphSource::LocalParams()`. Le pool du test reçoit un op
//     `Terrace` dont les valeurs écrasent celles de la strate : **une pile qui ignorerait l'override
//     ne peut plus être bit-identique**, ce qu'aucun test de l'étape B ne pouvait exiger.
//
//   • C2 (ce commit) : `Underwater`. Aucun opérateur, aucun paramètre — le même rocher atteint
//     depuis un second `case`. Le contrôle 5 l'échantillonne dans SON slot, ce qui exerce en prime
//     un second index de strate (donc un second `StrateSeed` de cuisson et une seconde entrée de la
//     mémo d'index) — ce que six chunks d'un seul slot ne peuvent pas faire.
//
// CE QUE CE TEST NE PROUVE TOUJOURS PAS : le saut de tuile — `FRoomGraphSource::EffectOverBox` rend
// `Both` et le ver `CarveOnly` partout, donc zéro verdict prouvé, ce que le contrôle 4 ASSERTE.
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

    /**
     * L'empreinte de params que `GetDensityWithParams` exige depuis le correctif d'`AUDIT §C2`.
     *
     * ⚠️ CE N'EST PAS DU REMPLISSAGE D'ARGUMENT. Avant ce correctif, l'original clé son cache SDF
     * sans les params, et le contrôle 3 plus bas explique en détail pourquoi il fallait alors
     * comparer chaque pile à ELLE-MÊME plutôt qu'à l'original : l'oracle partageait le défaut
     * testé. En passant la même empreinte que la production, l'oracle ne le partage plus.
     *
     * `LayoutVersion = 0` partout dans ce test : le monde de test ne rebâtit jamais son layout en
     * cours de route, donc la version est constante — ce qui compte ici, c'est que l'empreinte
     * DIFFÈRE entre deux jeux de params, et c'est exactement ce que la CRC donne.
     */
    FORCEINLINE uint32 VF_FP(const FStrateGenerationParams& InP)
    {
        return FCrc::MemCrc32(&InP, sizeof(InP));
    }

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

        // ⚠️⚠️ AJOUTÉ À L'ÉTAPE C1, ET C'EST *LE* TEST DE L'OVERRIDE PAR SALLE.
        // Jusqu'ici le pool était volontairement limité aux types Pit / Chimney / Column, dont les
        // `ApplyTo` n'écrivent AUCUN champ de détail : c'est ce qui rendait l'étape B vérifiable
        // alors même que les onze modificateurs lisaient les params de la strate au lieu de ceux de
        // la salle. Un op `Terrace` change exactement ça — les salles qui le tirent voient
        // `TerraceStepHeight/Hardness/NoiseDisplacement` ÉCRASÉS par ceux de l'op. Une pile qui
        // ignorerait l'override rendrait donc une densité différente **sur ces salles-là**, et
        // l'équivalence tomberait. C'est la seule preuve possible de C1.
        //
        // Valeurs délibérément TRÈS différentes de celles de la strate (6.0 / 0.6 / 0.5) : un
        // override qui n'écrase qu'avec des valeurs proches serait indétectable au bit près sur peu
        // de points, et « peu de points » est une couverture qu'on ne saurait pas lire.
        UVoxelTerrainOpDefinition* TerraceOp = NewObject<UVoxelTerrainOpDefinition>(
            GetTransientPackage(), NAME_None, RF_Transient);
        TerraceOp->Type                     = EVoxelTerrainOpType::Terrace;
        TerraceOp->TerraceStepHeight        = 3.0f;    // strate : 6.0
        TerraceOp->TerraceHardness          = 0.95f;   // strate : 0.6
        TerraceOp->TerraceNoiseDisplacement = 1.4f;    // strate : 0.5
        OutKeepAlive.Add(TStrongObjectPtr<UVoxelTerrainOpDefinition>(TerraceOp));

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

        // `Probability = 1.0` sur les quatre : la somme dépasse 1, donc la sélection est normalisée
        // et chaque salle tire exactement un op parmi les quatre, à parts égales.
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

        // ⚠️ `Weight = 1.0` et pas autre chose : `ApplyTo` multiplie le champ d'ACTIVATION par le
        // poids (`TerraceStepHeight * Weight`) et copie les autres tels quels. Un poids ≠ 1 rendrait
        // la valeur attendue moins lisible dans un diff sans rien prouver de plus.
        FStrateTerrainOpEntry TerraceEntry;
        TerraceEntry.Operation   = TSoftObjectPtr<UVoxelTerrainOpDefinition>(TerraceOp);
        TerraceEntry.Weight      = 1.0f;
        TerraceEntry.Probability = 1.0f;

        Def->TerrainOperations.Reset();
        Def->TerrainOperations.Add(PitEntry);
        Def->TerrainOperations.Add(ChimEntry);
        Def->TerrainOperations.Add(ColEntry);
        Def->TerrainOperations.Add(TerraceEntry);
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

    /**
     * ⚠️ DIVISION ENTIÈRE **PLANCHER**, ET C'EST UN VRAI BUG QU'ELLE CORRIGE, PAS UNE PRÉCAUTION.
     *
     * En C++ `/` TRONQUE vers zéro. Pour la strate TunnelNetwork, qui est en HAUT du layout et donc
     * en Z positif, tronquer et plancher donnent le même résultat — le bug ne pouvait pas se voir.
     * La strate Underwater est en BAS, en Z NÉGATIF : `-1 / 32` vaut `0` par troncature contre `-1`
     * par plancher, donc la borne haute de chunk part un cran trop haut, et le `Clamp` qui suit
     * entasse les points en trop sur le tout dernier voxel de la strate — en pleine bande de seal,
     * c'est-à-dire dans du roc plein.
     *
     * C'est un candidat pour le « 0 de 2000 échantillons en grotte ouverte » qu'a rapporté le premier
     * run vert du contrôle 5. Candidat, pas conclusion : le bloc de diagnostic du contrôle 5
     * INTERROGE désormais la cuisson et l'intervalle réellement échantillonné plutôt que de l'inférer.
     * (Même famille que `DivideAndRoundDown` dans les notes de projet : la troncature coûte des
     * cycles de build à chaque fois qu'on la suppose être un plancher.)
     */
    FORCEINLINE int32 FloorDivChunk(int32 A)
    {
        const int32 Q = A / CHUNK_SIZE, R = A % CHUNK_SIZE;
        return (R != 0 && R < 0) ? Q - 1 : Q;
    }

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

        // ÉTAPE C2 — le slot Underwater reçoit le MÊME pool, pour la même raison et au même
        // moment. Sans lui, le contrôle 5 comparerait deux chemins sur une strate sans aucune
        // primitive par salle : vert, et vide de sens.
        if (World.Definitions.IsValidIndex(FTestWorld::SlotUnderwater)
            && World.Definitions[FTestWorld::SlotUnderwater].IsValid())
        {
            MakeRoomOpPool(World.Definitions[FTestWorld::SlotUnderwater].Get(), OpAssets);
        }
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
        const int32 ChunkZ0 = FloorDivChunk(BottomVoxelZ);
        const int32 ChunkZ1 = FMath::Max(ChunkZ0, FloorDivChunk(TopVoxelZ) - 1);
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

        const float Old = Gen->GetDensityWithParams(X, Y, Z, P, VF_FP(P), 0);
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
                if (!BitEqual(Gen->GetDensityWithParams(X, Y, Z, PV, VF_FP(PV), 0),
                              VStack.EvalMC(X, Y, Z)))
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
    // 1d. LE GATE `bNearCaveSurface` — UN VOXEL HORS PORTÉE EST-IL VRAIMENT INTACT ?  (ÉTAPE B5)
    //=========================================================================
    // ⚠️ LA DÉCISION DE L'ÉTAPE B5, RAPPELÉE ICI PARCE QUE C'EST CE CONTRÔLE QUI LA PAIE.
    // Dans l'original les douze modificateurs vivent dans UN SEUL `if (bNearCaveSurface)`. La pile
    // est une LISTE PLATE, donc chaque opérateur re-teste le gate lui-même (`VF_NearCaveSurface`,
    // VoxelDensityOpStack.cpp, où le raisonnement complet est écrit). Un opérateur conteneur aurait
    // dû replier ses enfants à la place de `ClassifyBox` et les lui aurait cachés.
    //
    // Le risque que ce choix introduit est précis : **douze occasions d'oublier le gate au lieu
    // d'une**. Un oubli ne se verrait pas forcément dans l'équivalence globale (le modificateur
    // s'appliquerait aussi dans l'original si son propre range était large), d'où un contrôle dédié.
    //
    // MÉTHODE : on classe chaque échantillon par son SDF final (aucun opérateur en aval du graphe de
    // salles n'écrit ce canal), puis on compare la pile COMPLÈTE à une pile dont les onze amplitudes
    // pilotables sont à zéro. **Hors gate, les deux doivent être bit à bit identiques** ; dedans,
    // elles doivent différer souvent — sinon le contrôle ne compare rien.
    //
    // ⚠️ CE QUE CE CONTRÔLE NE PEUT PAS COUVRIR : l'opérateur de COLONNES, qui n'a aucune amplitude
    // à éteindre (cf. la note du contrôle 3b). Son gate à lui n'est couvert que par la bit-identité
    // globale du contrôle 1 face à l'original — ce qui suffit, mais ne se voit pas ici.
    {
        FStrateGenerationParams PNoMods = P;
        PNoMods.SurfaceRoughness   = 0.0f;
        PNoMods.TerraceStepHeight  = 0.0f;
        PNoMods.LayerLineSpacing   = 0.0f;
        PNoMods.RibbingSpacing     = 0.0f;
        PNoMods.OverhangStrength   = 0.0f;
        PNoMods.CliffStrength      = 0.0f;
        PNoMods.ScallopStrength    = 0.0f;
        PNoMods.ArchDensity        = 0.0f;
        PNoMods.DomeDensity        = 0.0f;
        PNoMods.PinchDensity       = 0.0f;
        PNoMods.FloorBias          = 0.0f;

        FVoxelOpStack NoModStack;
        VoxelDensityOps::BuildTunnelNetworkStack(NoModStack, PNoMods, World.Settings->Seed,
                                                 Gen->OriginSpineRadius, World.StrateManager.Get());
        NoModStack.PrepareChunk(Ctx);

        TArray<float> NoModVals;
        NoModVals.SetNumUninitialized(NumTunnelSamples);
        TArray<uint8> bOutsideGate;
        bOutsideGate.SetNumUninitialized(NumTunnelSamples);

        const float DetailThreshold = P.SDFBlendRadius * 3.0f;

        for (int32 i = 0; i < NumTunnelSamples; ++i)
        {
            const float X = (float)Points[i].X, Y = (float)Points[i].Y, Z = (float)Points[i].Z;
            const FVoxelOpSample S = NoModStack.EvalSample(X, Y, Z);
            NoModVals[i] = -S.Density;   // même négation MC que EvalMC
            // Le gate, écrit exactement comme l'original et comme VF_NearCaveSurface.
            bOutsideGate[i] = ((S.Sdf < DetailThreshold) && (S.Sdf < FLT_MAX)) ? 0 : 1;
        }

        int32 NumOutside = 0, NumLeaked = 0, NumInsideMoved = 0;
        for (int32 i = 0; i < NumTunnelSamples; ++i)
        {
            if (bOutsideGate[i])
            {
                ++NumOutside;
                if (!BitEqual(NoModVals[i], FullVals[i])) { ++NumLeaked; }
            }
            else if (!BitEqual(NoModVals[i], FullVals[i]))
            {
                ++NumInsideMoved;
            }
        }

        TestEqual(TEXT("no detail modifier touches a voxel outside the bNearCaveSurface gate"),
                  NumLeaked, 0);

        AddInfo(FString::Printf(
            TEXT("Gate check (stage B5): %d of %d samples (%.1f%%) sit OUTSIDE the gate ")
            TEXT("(SDF >= SDFBlendRadius*3), and %d of them leaked. Inside the gate, %d samples ")
            TEXT("move when the eleven amplitudes are zeroed -- that second number is what says ")
            TEXT("the comparison is not vacuous: if it were 0, 'nothing leaked' would only mean ")
            TEXT("'nothing happened anywhere'. The column operator (STEP 4d) is NOT covered here, ")
            TEXT("having no amplitude to zero; its gate rides on check 1's bit-identity instead."),
            NumOutside, NumTunnelSamples, 100.0f * (float)NumOutside / (float)NumTunnelSamples,
            NumLeaked, NumInsideMoved));

        if (NumOutside < NumTunnelSamples / 10)
        {
            AddWarning(FString::Printf(
                TEXT("Only %d of %d samples are outside the gate, so this check barely asked its ")
                TEXT("question. Deep rock is the common case in production -- a sampling set that ")
                TEXT("almost never leaves the cave is not representative of what the gate skips."),
                NumOutside, NumTunnelSamples));
        }
        if (NumInsideMoved == 0)
        {
            AddError(TEXT("Inside the gate, zeroing all eleven modifier amplitudes changed NOTHING. ")
                     TEXT("The gate check above is therefore vacuous, and so, probably, is a large ")
                     TEXT("part of check 1b. Suspect the params never reaching the operators."));
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
    // ⚠️⚠️ HISTORIQUE, ET LE DÉNOUEMENT EST DANS LE PARAGRAPHE SUIVANT — À LIRE EN ENTIER.
    // Ce bloc a été écrit quand `GetDensityWithParams` clé son cache sur (boîte XY, strate, seed),
    // **sans les params** : en alternance il rendait, pour B, les salles de A, donc l'original
    // ÉCHOUAIT ce contrôle. Le comparer à lui ici n'aurait pas mesuré l'opérateur, ça aurait mesuré
    // son bug — d'où le choix de comparer chaque pile à ELLE-MÊME évaluée seule.
    //
    // ✅ **CE N'ÉTAIT PAS QU'UN ARTEFACT DE TEST, ET C'EST MAINTENANT CORRIGÉ** (2026-07-28). Le
    // soupçon écrit ici s'est confirmé : `GetGenerationParams` blende les params À L'INTÉRIEUR
    // d'une strate (`Alpha` = f(chunk Z) en `Gradient`, le défaut), donc deux chunks de Z différents
    // partageaient boîte XY, index de strate et seed ⇒ aucune reconstruction ⇒ le deuxième chunk
    // évalué contre les salles du premier. Et comme l'ordre des workers décide lequel est « le
    // premier », **deux pairs divergeaient depuis la même seed**, ce que §2.6.1 interdit.
    // `GetDensityWithParams` prend désormais une empreinte de params et une `LayoutVersion`
    // OBLIGATOIRES (calculées une fois par chunk côté production, `VF_FP` ici).
    //
    // ⚠️ ON GARDE POURTANT L'ORACLE « CHAQUE PILE CONTRE ELLE-MÊME », et ce n'est pas de la
    // paresse : il teste la clé de la PILE, qui est une clé distincte de celle de l'original. Les
    // faire dépendre l'une de l'autre remettrait exactement le couplage qu'on vient de défaire.
    //
    // The suspicion recorded here was CONFIRMED and is now fixed: the params fingerprint and layout
    // version are required arguments. The self-comparison oracle stays, because it tests the STACK's
    // key, which is a different key from the original's.
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
    // 3c. L'OVERRIDE D'OP PAR SALLE A-T-IL EU L'OCCASION DE COMPTER ?  (ÉTAPE C1)
    //=========================================================================
    // Le pool contient depuis C1 un op `Terrace` dont les valeurs (3.0 / 0.95 / 1.4) écrasent celles
    // de la strate (6.0 / 0.6 / 0.5) sur les salles qui le tirent. Une pile qui ignorerait l'override
    // rendrait donc une densité DIFFÉRENTE près de ces salles, et le contrôle 1 tomberait.
    //
    // ⚠️ MAIS SEULEMENT SI DE TELLES SALLES EXISTENT ET SI ON A ÉCHANTILLONNÉ PRÈS D'ELLES. C'est
    // exactement le piège des pits, une quatrième fois : une équivalence verte prouverait alors que
    // deux chemins s'accordent là où l'override ne s'applique pas. Il n'y a pas de sonde de params
    // possible (le pool n'est pas dans la clé du cache), donc on interroge encore la STRUCTURE :
    //   (1) combien de salles portent un op `Terrace` ?
    //   (2) combien d'échantillons tombent dans le rayon d'influence d'une de ces salles ?
    // Zéro à l'une ou l'autre ⇒ le contrôle 1 ne dit RIEN de C1, quelle que soit sa couleur.
    //
    // (2) est approximatif — il mesure en coordonnées NON warpées, alors que le voxel choisit sa
    // salle en coordonnées warpées. C'est un compteur de COUVERTURE, pas un oracle : sur-estimer
    // légèrement ne rend rien faux, ça rend seulement le seuil un peu généreux.
    {
        int32 StrateIdx = 0;
        {
            const int32 QZ = FMath::FloorToInt((float)((TopVoxelZ + BottomVoxelZ) / 2) / (float)CHUNK_SIZE);
            StrateIdx = World.StrateManager->GetStrateIndex(
                ((float)QZ + 0.5f) * CHUNK_SIZE * VOXEL_SIZE);
        }

        const UVoxelStrateDefinition* Def = World.Definitions[FTestWorld::SlotTunnelNetwork].Get();

        // UNE grande boîte couvrant tout le domaine échantillonné (chunks XY de -3 à +3), plutôt que
        // 49 boîtes par chunk : les salles sont hachées par cellule, donc leurs positions ne
        // dépendent pas du découpage — seule la décision de STOCKAGE change, et on ne lit ici que
        // des centres et des ops.
        const float Expansion = P.CaveWarpStrength + 2.0f;
        const float BigMinX = -3.0f * CHUNK_SIZE - Expansion;
        const float BigMinY = -3.0f * CHUNK_SIZE - Expansion;
        const float BigMaxX =  4.0f * CHUNK_SIZE + Expansion;
        const float BigMaxY =  4.0f * CHUNK_SIZE + Expansion;

        FChunkSDFCache BigCache;
        VoxelCaveMorphology::BuildChunkCache(
            BigCache, BigMinX, BigMinY, BigMaxX, BigMaxY,
            P, (uint32)World.Settings->Seed, StrateIdx, &Def->TerrainOperations);

        TArray<const FCachedRoom*> TerraceRooms;
        int32 NumRoomsWithOp = 0;
        for (const FCachedRoom& R : BigCache.Rooms)
        {
            if (R.RoomOp == nullptr) { continue; }
            ++NumRoomsWithOp;
            if (R.RoomOp->Type == EVoxelTerrainOpType::Terrace) { TerraceRooms.Add(&R); }
        }

        int32 NumSamplesNearTerrace = 0;
        for (int32 i = 0; i < NumTunnelSamples; ++i)
        {
            const FVector Pt = Points[i];
            for (const FCachedRoom* R : TerraceRooms)
            {
                if ((float)FVector::DistSquared(Pt, R->Center) < R->CullRadiusSq)
                {
                    ++NumSamplesNearTerrace;
                    break;
                }
            }
        }

        AddInfo(FString::Printf(
            TEXT("Per-room override coverage (stage C1): %d of %d baked rooms carry a terrain op, ")
            TEXT("%d of them a Terrace op whose params (3.0/0.95/1.4) overwrite the strate's ")
            TEXT("(6.0/0.6/0.5); %d of %d samples fall inside one of those rooms' influence radius. ")
            TEXT("The bit-identity in check 1 means C1 works ONLY insofar as those last two numbers ")
            TEXT("are non-zero -- otherwise it proves that two paths agree where the override never ")
            TEXT("applies, which is what stage B already proved."),
            NumRoomsWithOp, BigCache.Rooms.Num(), TerraceRooms.Num(),
            NumSamplesNearTerrace, NumTunnelSamples));

        TestTrue(TEXT("the bake rolled at least one Terrace op onto a room (stage C1's premise)"),
                 TerraceRooms.Num() > 0);
        TestTrue(TEXT("at least one sample lands inside a Terrace-op room, so the per-room override ")
                 TEXT("is actually exercised by the equivalence above"),
                 NumSamplesNearTerrace > 0);
    }

    //=========================================================================
    // 4. LE VERDICT DE BOÎTE — plus attendu nul, et CHAQUE VERDICT EST BRUTE-FORCÉ
    //=========================================================================
    // ⚠️ CE BLOC A CHANGÉ DE NATURE LE 2026-07-28, ET IL FAUT SAVOIR POURQUOI.
    // Il ASSERTAIT `NumProved == 0`. C'était juste tant que `FRoomGraphSource::EffectOverBox`
    // rendait `Both` inconditionnellement : « zéro » était alors une description honnête de l'état
    // du portage. Depuis que la source répond SPATIALEMENT, asserter zéro reviendrait à interdire
    // le gain qu'on vient de construire — et pire, ça transformerait le test en gardien du bug.
    //
    // Ce qui le remplace n'est PAS « on enlève l'assertion » : c'est l'assertion qui compte
    // vraiment, la SOUNDNESS. Un verdict faux ne se voit pas — pas de géométrie, **pas de
    // collision** — jusqu'à ce qu'un joueur traverse le sol. Donc chaque tuile déclarée prouvée est
    // ré-évaluée voxel par voxel, et le test échoue si UN seul échantillon contredit le verdict.
    // Le nombre de tuiles prouvées, lui, est REPORTÉ, pas asserté : c'est une mesure, pas un
    // contrat (la leçon « coverage is a number, not a boolean »).
    //
    // Was: assert zero proved. That was honest while the source answered Both unconditionally; it
    // would now forbid the very gain this change makes. What replaces it is the assertion that
    // actually matters — every proved tile is brute-forced voxel by voxel, because a false verdict
    // means no geometry and NO COLLISION until a player falls through it.
    {
        int32 NumProved = 0, NumMixed = 0, NumSolid = 0, NumAir = 0;
        int32 NumBruteSamples = 0, NumViolations = 0;
        float WorstViolation = 0.0f;
        TMap<FString, int32> SolidKillerCounts;
        int32 NumRoomKilled = 0;
        int32 TilesHitByRooms = 0, TilesHitByTunnels = 0, TilesHitByPits = 0, TilesHitByChimneys = 0;
        int32 SumHitRooms = 0, SumNumRooms = 0, SumHitTunnels = 0, SumNumTunnels = 0;

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

            // ATTRIBUTION — le même pliage, mais il dit QUI tue chaque hypothèse. Le premier build
            // de l'`EffectOverBox` spatial est revenu vert avec 0 tuile prouvée, et le rapport ne
            // savait nommer aucun coupable : les deux causes que la mise en garde proposait étaient
            // toutes les deux fausses, la vraie étant un troisième opérateur. On ne redevine pas.
            int32 SolidKiller = INDEX_NONE, AirKiller = INDEX_NONE;
            const EVoxelTileClass Verdict = Stack.ClassifyBoxAttributed(Box, Ctx, SolidKiller, AirKiller);

            if (SolidKiller != INDEX_NONE)
            {
                const FString KillerName = Stack.GetOpDebugName(SolidKiller);
                SolidKillerCounts.FindOrAdd(KillerName)++;

                // VENTILATION PAR CLASSE DE PRIMITIVE. Quand c'est la source de salles qui tue,
                // « les tuiles traversent une grotte » n'est pas une réponse : les salles, les
                // tunnels, les pits et les cheminées ont chacun leur borne, de finesse très
                // différente (une sphère englobante de capsule est un très mauvais tunnel). On lit
                // ce que l'opérateur a RÉELLEMENT calculé plutôt que de rejouer le critère ici.
                if (KillerName == TEXT("RoomGraphSource"))
                {
                    const VoxelDensityOps::FRoomBoxDiagnostic D =
                        VoxelDensityOps::GetLastRoomBoxDiagnostic();
                    ++NumRoomKilled;
                    if (D.HitRooms    > 0) { ++TilesHitByRooms; }
                    if (D.HitTunnels  > 0) { ++TilesHitByTunnels; }
                    if (D.HitPits     > 0) { ++TilesHitByPits; }
                    if (D.HitChimneys > 0) { ++TilesHitByChimneys; }
                    SumHitRooms   += D.HitRooms;   SumNumRooms   += D.NumRooms;
                    SumHitTunnels += D.HitTunnels; SumNumTunnels += D.NumTunnels;
                }
            }

            if (Verdict == EVoxelTileClass::Mixed) { ++NumMixed; continue; }

            ++NumProved;
            const bool bClaimSolid = (Verdict == EVoxelTileClass::AllSolid);
            if (bClaimSolid) { ++NumSolid; } else { ++NumAir; }

            // BRUTE FORCE — la boîte entière, pas un échantillonnage. `EvalMC` rend la convention
            // du mesher (négatif = solide), donc « tout solide » veut dire qu'aucun échantillon
            // n'est du côté air. On teste le SIGNE, c'est-à-dire l'existence d'une traversée
            // d'isosurface : c'est exactement la propriété sur laquelle le mesher est sauté.
            for (float Z = (float)Box.Min.Z; Z <= (float)Box.Max.Z; Z += 1.0f)
            for (float Y = (float)Box.Min.Y; Y <= (float)Box.Max.Y; Y += 1.0f)
            for (float X = (float)Box.Min.X; X <= (float)Box.Max.X; X += 1.0f)
            {
                const float D = Stack.EvalMC(X, Y, Z);
                ++NumBruteSamples;
                const bool bViolates = bClaimSolid ? (D > 0.0f) : (D < 0.0f);
                if (bViolates)
                {
                    ++NumViolations;
                    WorstViolation = FMath::Max(WorstViolation, FMath::Abs(D));
                }
            }
        }

        AddInfo(FString::Printf(
            TEXT("Box verdicts over 40 TunnelNetwork tiles: %d proved (%d AllSolid, %d AllAir), ")
            TEXT("%d Mixed -- brute-forced over %d voxels, %d violations. This number was 0 proved / ")
            TEXT("40 Mixed until FRoomGraphSource::EffectOverBox learned to answer spatially, and it ")
            TEXT("is the single largest perf item of the whole plan (OPSTACK-DECOMPOSITION 0.2): a ")
            TEXT("proved tile skips GenerateMesh entirely, so it trades one BuildChunkCache against ")
            TEXT("30000+ density evaluations. Read the PROVED count as a measurement, never as a ")
            TEXT("contract -- what is asserted below is that none of them is WRONG, because a false ")
            TEXT("verdict leaves no geometry and no collision behind it."),
            NumProved, NumSolid, NumAir, NumMixed, NumBruteSamples, NumViolations));

        // QUI TUE `AllSolid`, ET COMBIEN DE FOIS. Toujours imprimé, pas seulement en cas d'échec :
        // c'est aussi la ligne qui dit, quand des tuiles SONT prouvées, ce qui bloque les autres.
        {
            SolidKillerCounts.ValueSort([](int32 A, int32 B) { return A > B; });
            FString Breakdown;
            for (const TPair<FString, int32>& Kv : SolidKillerCounts)
            {
                if (!Breakdown.IsEmpty()) { Breakdown += TEXT(", "); }
                Breakdown += FString::Printf(TEXT("%s x%d"), *Kv.Key, Kv.Value);
            }
            if (Breakdown.IsEmpty()) { Breakdown = TEXT("nothing -- AllSolid survived every tile"); }

            AddInfo(FString::Printf(
                TEXT("AllSolid killed by: %s. This is the line that replaced a guess. The first ")
                TEXT("build of the spatial EffectOverBox reported 0 proved of 40, and the warning ")
                TEXT("offered two candidate causes -- BOTH WRONG. The real one was a third operator ")
                TEXT("nobody was looking at: FWormFieldSource answered CarveOnly everywhere, and ")
                TEXT("since BaseDensity=8 < WormStrength=10 BY DEFAULT, its provable amplitude bound ")
                TEXT("alone drove SolidMargin negative on every tile in the world. Attribution is ")
                TEXT("cheap; a second wrong guess is not."),
                *Breakdown));

            if (NumRoomKilled > 0)
            {
                AddInfo(FString::Printf(
                    TEXT("...and when RoomGraphSource is the killer (%d tiles), WHICH primitive class ")
                    TEXT("reaches the box: rooms %d, tunnels %d, pits %d, chimneys %d (tiles, not ")
                    TEXT("primitives -- a tile can be hit by several). Averages per killed tile: ")
                    TEXT("%.1f of %.1f rooms reach, %.1f of %.1f tunnels reach. THIS is the line that ")
                    TEXT("says what to tighten. A tunnel is culled per voxel by its BOUNDING SPHERE, ")
                    TEXT("which for a long thin capsule is an enormous over-estimate; a room's cull ")
                    TEXT("sphere is a fair fit. So tunnels >> rooms here would mean the box test is ")
                    TEXT("losing to capsule bounding spheres, not to real cave -- and the fix would ")
                    TEXT("be a segment-vs-box distance, not anything about the sampler."),
                    NumRoomKilled, TilesHitByRooms, TilesHitByTunnels, TilesHitByPits, TilesHitByChimneys,
                    (float)SumHitRooms   / (float)NumRoomKilled, (float)SumNumRooms   / (float)NumRoomKilled,
                    (float)SumHitTunnels / (float)NumRoomKilled, (float)SumNumTunnels / (float)NumRoomKilled));
            }
        }

        if (NumProved == 0)
        {
            AddWarning(TEXT("No TunnelNetwork tile was proved, so the brute force below verified ")
                       TEXT("nothing -- it has no verdict to contradict. Do NOT re-derive the cause: ")
                       TEXT("read the 'AllSolid killed by' line above, which names the operator and ")
                       TEXT("counts how often. If it names RoomGraphSource, the tiles genuinely ")
                       TEXT("straddle cave (check the bake-coverage line of 5b); anything else is an ")
                       TEXT("operator whose box answer is more pessimistic than its Eval."));
        }

        TestEqual(FString::Printf(
                      TEXT("every proved TunnelNetwork tile survives brute force (worst |density| ")
                      TEXT("on the wrong side: %.9g)"), WorstViolation),
                  NumViolations, 0);
    }

    //=========================================================================
    // 5. UNDERWATER — LE MÊME ROCHER, DEPUIS UN AUTRE SLOT  (ÉTAPE C2)
    //=========================================================================
    // `OPSTACK-DECOMPOSITION §8` affirme qu'il n'y a **aucune différence de densité** entre
    // `Underwater` et `TunnelNetwork` : `GetDensityAt` les met dans le MÊME `case`, et
    // `WaterLevelRelative` n'est lu que par `UVoxelStrateManager` (nappe d'eau, côté rendu), jamais
    // par la densité. Les deux affirmations ont été RELUES dans le code avant d'écrire ce contrôle,
    // pas supposées — c'est la leçon « vérifier la prémisse » et elle a déjà été payée quatre fois.
    //
    // Ce que ce contrôle ajoute par-dessus, et qui n'est PAS tautologique : le slot Underwater est
    // une AUTRE strate, donc un autre `StrateIndex`, donc un autre `StrateSeed` dans la cuisson et
    // une autre entrée de la mémo d'index de strate. Il exerce ce que six chunks du slot 0 ne
    // peuvent pas : que la pile suive le bon jeu de salles quand deux strates du même archétype
    // coexistent dans le même monde.
    {
        int32 UWTop = 0, UWBottom = 0;
        if (!World.GetSlotVoxelZRange(FTestWorld::SlotUnderwater, UWTop, UWBottom))
        {
            AddError(TEXT("The fixture layout has no Underwater slot, so stage C2 is untested. ")
                     TEXT("Check FTestWorld::Build's Archetypes[] against SlotUnderwater."));
        }
        else
        {
            const int32 UWMidChunkZ = ((UWTop + UWBottom) / 2) / CHUNK_SIZE;
            FStrateGenerationParams UP =
                World.StrateManager->GetGenerationParams(FIntVector(0, 0, UWMidChunkZ));
            EnableTunnelFeatures(UP);

            FVoxelOpStack UWStack;
            VoxelDensityOps::BuildTunnelNetworkStack(UWStack, UP, World.Settings->Seed,
                                                     Gen->OriginSpineRadius, World.StrateManager.Get());

            FVoxelOpContext UWCtx;
            UWCtx.Seed               = (uint32)World.Settings->Seed;
            UWCtx.LayoutVersion      = World.StrateManager->GetLayoutVersion();
            UWCtx.StrateTopWorldZ    = UP.StrateTopWorldZ;
            UWCtx.StrateBottomWorldZ = UP.StrateBottomWorldZ;
            UWStack.PrepareChunk(UWCtx);

            TestEqual(TEXT("the Underwater stack is the tunnel stack: same 19 ops"), UWStack.Num(), 19);

            // 8 → 24 chunks, comme le balayage principal. Le premier run vert a rapporté **0 de 2000
            // échantillons en grotte ouverte** là où le slot TunnelNetwork en avait 16,7 % : trop peu de
            // grappes est l'une des explications possibles, la troncature de la division en est une
            // autre, et le bloc de diagnostic plus bas dit laquelle.
            const int32 UWChunks = 24, UWPerChunk = 250;
            const int32 UWSamples = UWChunks * UWPerChunk;

            TArray<FVector> UWPoints;
            UWPoints.Reserve(UWSamples);
            {
                FRandomStream Rng(20260728);
                const int32 CZ0 = FloorDivChunk(UWBottom);
                const int32 CZ1 = FMath::Max(CZ0, FloorDivChunk(UWTop) - 1);
                for (int32 c = 0; c < UWChunks; ++c)
                {
                    const int32 CX = Rng.RandRange(-3, 3);
                    const int32 CY = Rng.RandRange(-3, 3);
                    const int32 CZ = Rng.RandRange(CZ0, CZ1);
                    for (int32 i = 0; i < UWPerChunk; ++i)
                    {
                        UWPoints.Add(FVector(
                            (float)(CX * CHUNK_SIZE + Rng.RandRange(0, CHUNK_SIZE - 1)),
                            (float)(CY * CHUNK_SIZE + Rng.RandRange(0, CHUNK_SIZE - 1)),
                            (float)FMath::Clamp(CZ * CHUNK_SIZE + Rng.RandRange(0, CHUNK_SIZE - 1),
                                                UWBottom, UWTop)));
                    }
                }
            }

            const float UWInnerBot = UP.StrateBottomWorldZ + UP.BoundarySealThickness;
            const float UWInnerTop = UP.StrateTopWorldZ    - UP.BoundarySealThickness;

            int32 UWDiff = 0, UWInCave = 0;
            float UWWorst = 0.0f;
            for (int32 i = 0; i < UWSamples; ++i)
            {
                const float X = (float)UWPoints[i].X, Y = (float)UWPoints[i].Y, Z = (float)UWPoints[i].Z;
                const float Old = Gen->GetDensityWithParams(X, Y, Z, UP, VF_FP(UP), 0);
                const float New = UWStack.EvalMC(X, Y, Z);
                if (Z > UWInnerBot && Z < UWInnerTop && Old >= 0.0f) { ++UWInCave; }
                if (!BitEqual(Old, New))
                {
                    ++UWDiff;
                    UWWorst = FMath::Max(UWWorst, FMath::Abs(Old - New));
                }
            }

            if (UWDiff == 0)
            {
                AddInfo(FString::Printf(
                    TEXT("Underwater (stage C2): bit-identical across %d samples in %d chunks of the ")
                    TEXT("Underwater slot, %d of them in open cave (%.1f%%). This is a DIFFERENT ")
                    TEXT("strate index from the TunnelNetwork slot, so it also exercises the ")
                    TEXT("strate-index memo and the per-strate room seed -- which six chunks of one ")
                    TEXT("slot cannot. No density difference between the two archetypes was found, ")
                    TEXT("which is what OPSTACK-DECOMPOSITION 8 predicts."),
                    UWSamples, UWChunks, UWInCave, 100.0f * (float)UWInCave / (float)UWSamples));
            }
            else
            {
                AddError(FString::Printf(
                    TEXT("Underwater (stage C2): %d of %d samples differ (largest |delta| %.9g). ")
                    TEXT("⚠️ READ THIS BEFORE FIXING CODE: if the TunnelNetwork equivalence above is ")
                    TEXT("GREEN and only this one fails, the operators are fine and the finding is a ")
                    TEXT("real DENSITY DIFFERENCE between Underwater and TunnelNetwork -- which ")
                    TEXT("would contradict OPSTACK-DECOMPOSITION 8 and is worth more written down ")
                    TEXT("than patched. The likeliest culprit is the strate index (the memo is keyed ")
                    TEXT("on chunk-Z and layout version) rather than anything archetype-specific."),
                    UWDiff, UWSamples, UWWorst));
            }

            //-----------------------------------------------------------------
            // 5b. POURQUOI ? — ON INTERROGE, ON N'INFÈRE PAS  (le premier run vert a dit 0,0 %)
            //-----------------------------------------------------------------
            // ⚠️ Trois causes possibles à « aucun échantillon en grotte ouverte », et elles se
            // corrigent différemment. Plutôt que d'en choisir une au jugé — la faute que ce fichier
            // documente trois fois déjà — chacune a désormais son propre nombre :
            //   (a) la cuisson n'a produit AUCUNE salle pour cet index de strate ⇒ `Rooms == 0` ;
            //   (b) les points échantillonnés ne sont pas dans l'INTÉRIEUR de la strate (bandes de
            //       seal, ou intervalle de chunks mal calculé) ⇒ `interior` faible ;
            //   (c) il y a des salles et l'intervalle est bon, mais elles ne croisent pas ces XY
            //       ⇒ `Rooms > 0` et `interior` élevé, et il faut alors élargir les XY.
            {
                const int32 UWStrateIdx = World.StrateManager->GetStrateIndex(
                    ((float)FloorDivChunk((UWTop + UWBottom) / 2) + 0.5f) * CHUNK_SIZE * VOXEL_SIZE);

                const UVoxelStrateDefinition* UWDef =
                    World.Definitions.IsValidIndex(FTestWorld::SlotUnderwater)
                        ? World.Definitions[FTestWorld::SlotUnderwater].Get() : nullptr;

                int32 UWRooms = 0, UWPits = 0, UWChims = 0, UWCols = 0;
                if (UWDef)
                {
                    const float Expansion = UP.CaveWarpStrength + 2.0f;
                    for (int32 c = 0; c < 6; ++c)
                    {
                        const float MinX = (c - 3) * (float)CHUNK_SIZE - Expansion;
                        const float MinY = (c - 3) * (float)CHUNK_SIZE - Expansion;

                        FChunkSDFCache UWProbe;
                        VoxelCaveMorphology::BuildChunkCache(
                            UWProbe, MinX, MinY, MinX + CHUNK_SIZE + 2.0f * Expansion,
                            MinY + CHUNK_SIZE + 2.0f * Expansion,
                            UP, (uint32)World.Settings->Seed, UWStrateIdx, &UWDef->TerrainOperations);
                        UWRooms += UWProbe.Rooms.Num();
                        UWPits  += UWProbe.Pits.Num();
                        UWChims += UWProbe.Chimneys.Num();
                        UWCols  += UWProbe.Columns.Num();
                    }
                }

                int32 UWInterior = 0;
                for (int32 i = 0; i < UWSamples; ++i)
                {
                    const float Z = (float)UWPoints[i].Z;
                    if (Z > UWInnerBot && Z < UWInnerTop) { ++UWInterior; }
                }

                AddInfo(FString::Printf(
                    TEXT("Underwater diagnosis: strate index %d, voxel Z range [%d, %d], seal-free ")
                    TEXT("interior (%.0f, %.0f); %d of %d samples are inside that interior; the bake ")
                    TEXT("produced %d rooms, %d pits, %d chimneys, %d columns over 6 search boxes. ")
                    TEXT("Read it as: 0 rooms means the bake, a low interior count means the sampled ")
                    TEXT("Z range, and rooms>0 with a high interior count means the XY spread."),
                    UWStrateIdx, UWBottom, UWTop, UWInnerBot, UWInnerTop,
                    UWInterior, UWSamples, UWRooms, UWPits, UWChims, UWCols));

                if (UWInCave == 0)
                {
                    AddWarning(FString::Printf(
                        TEXT("No Underwater sample landed in open cave (%d in the seal-free interior, ")
                        TEXT("%d rooms baked), so check 5 compared solid rock to solid rock and proves ")
                        TEXT("almost nothing about this slot. Same failure mode as the 1.1%% run of ")
                        TEXT("stage A -- and the equivalence being bit-identical is NOT evidence ")
                        TEXT("against it, it is exactly what two agreeing voids look like. The ")
                        TEXT("diagnosis line above says which of the three causes it is."),
                        UWInterior, UWRooms));
                }
            }
        }
    }

    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
