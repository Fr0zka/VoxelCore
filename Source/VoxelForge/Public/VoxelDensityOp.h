// VoxelDensityOp.h
// LE CONTRAT de la pile d'opérateurs de densité / THE density operator stack CONTRACT.
// Phase 1 de OPSTACK-PLAN.md. HEADER SEUL — rien n'est encore branché dans GetDensityAt.
//
// ─────────────────────────────────────────────────────────────────────────────────────────
// POURQUOI / WHY
// ─────────────────────────────────────────────────────────────────────────────────────────
// UVoxelGenerator::GetDensityAt est aujourd'hui un `switch` sur 8 ECaveGeneratorType, chacun
// possédant sa fonction de densité et son struct de params. Conséquences : une nouvelle idée de
// monde coûte ~6 sites d'édition, et surtout LES IDÉES NE PEUVENT PAS SE COMBINER — un archétype
// possède le voxel entier. On ne peut pas écrire « une strate de surface dont les montagnes
// contiennent un réseau de salles, avec des îles flottantes dans le vide au-dessus », à aucun prix.
//
// GetDensityAt is today a `switch` over 8 ECaveGeneratorType, each owning a bespoke density
// function and param struct. A new world idea costs ~6 edit sites, and — the real problem —
// IDEAS CANNOT COMBINE: one archetype owns the whole voxel.
//
// ─────────────────────────────────────────────────────────────────────────────────────────
// ⚠️ CE N'EST PAS L'ANCIEN SYSTÈME DE « ROOM OPERATIONS » / THIS IS NOT THE OLD ROOM-OPS SYSTEM
// ─────────────────────────────────────────────────────────────────────────────────────────
// UVoxelTerrainOpDefinition (Terrace, Ribbing, Cliff, Scallop, Overhang, Arch, Column, Pit…) ne
// sait que PERTURBER une densité près d'une surface qui existe déjà. Il ne décide jamais ce que le
// champ EST — cette décision vit dans le `switch`. Une pile d'opérateurs qui se contenterait de
// cela aurait reconstruit le switch avec des étapes en plus.
//
// D'où QUATRE RÔLES, dont l'ancien système n'occupait que le troisième :
//
//   1. FIELD SOURCE      — fabrique un champ À PARTIR DE RIEN. C'est ce qui fait qu'une grotte est
//                          une grotte et qu'un monde ouvert est un monde ouvert. Chaque archétype
//                          d'aujourd'hui est fondamentalement l'une de ces sources.
//   2. COMBINER          — comment deux champs fusionnent (min/max/smooth/mask). C'EST le rôle qui
//                          achète la composition ; sans lui il n'y a pas de refactor.
//   3. DETAIL MODIFIER   — l'ancien système, rétrogradé à un rôle sur quatre. Inchangé.
//   4. STRUCTURAL POST   — spine (0,0) → seal → passages → diff layer. Des INVARIANTS de monde,
//                          pas des choix créatifs : toujours ajoutés, dans cet ordre, jamais
//                          omissibles par l'auteur.
//
// The old system only ever had role 3. Roles 1 and 2 are the new thing, and role 4 is what keeps
// the descent structure intact no matter what an author assembles.
//
// ─────────────────────────────────────────────────────────────────────────────────────────
// LA CLÉ DE VOÛTE : EffectOverBox — direction, pas intervalle / THE KEYSTONE: direction, not intervals
// ─────────────────────────────────────────────────────────────────────────────────────────
// La version « complète » d'une borne rendrait un intervalle numérique. NE PAS COMMENCER LÀ.
// Presque tout opérateur existant est UNIDIRECTIONNEL : il ne fait que creuser, ou que remplir.
// Cela suffit à reproduire GÉNÉRIQUEMENT chaque garde écrite à la main dans ClassifyTile :
//
//     « passages ⇒ bCanSolid = false »        EST     CarveOnly
//     « ponts/arêtes ⇒ bCanAir = false »      EST     FillOnly
//     « aucun passage près de cette boîte »   EST     Identity
//
// Donc la Phase 1 n'a besoin d'AUCUNE borne numérique et obtient déjà toute la propriété de
// sûreté. Les intervalles sont un resserrement ultérieur pour le coût de génération, pas un
// prérequis de correction. C'est ce qui rend le premier pas petit.
//
// Phase 1 needs NO numeric bounds and already gets the whole safety property. Intervals are a
// later tightening for gen cost, not a correctness prerequisite.
//
// ─────────────────────────────────────────────────────────────────────────────────────────
// LE GROS LOT PERF : les strates de grotte ne sautent AUCUNE tuile aujourd'hui
// THE PERF PRIZE: cave strates skip ZERO tiles today
// ─────────────────────────────────────────────────────────────────────────────────────────
// ClassifyTile ne sait prouver que les gaps de bedrock et SurfaceWorld ; tout le reste tombe sur
// `return EVoxelTileClass::Mixed; // archétype cave […] pas prouvable en v1`. TunnelNetwork, Maze,
// VerticalShafts, FloatingIslands, FlatPlain, CrystalChamber et Underwater ne captent donc RIEN du
// gain T1.d (84 % des générations vides, −44 % de CPU worker). Écrire un prouveur sur mesure par
// archétype a toujours été trop cher — EffectOverBox EST le mécanisme générique qui le rend gratuit :
// une source à graphe de salles qui rend Identity quand aucune borne de salle ni de tunnel n'atteint
// la boîte rend le bedrock profond sautable pour la première fois.
//
// **Traiter cela comme un livrable explicite de chaque portage, pas comme un effet de bord.**
//
// ─────────────────────────────────────────────────────────────────────────────────────────
// ÉTAT / STATUS
// ─────────────────────────────────────────────────────────────────────────────────────────
// Phase 1, header seul. Aucun opérateur n'existe encore, GetDensityAt n'a pas changé, le `switch`
// est intact. Prochaine étape : porter Maze en le DÉCOMPOSANT (source réseau de couloirs + modifier
// de rugosité), pas en l'emballant. Voir OPSTACK-DECOMPOSITION.md pour le plan par archétype.
//
// Phase 1, header only. No operator exists yet, GetDensityAt is unchanged, the switch is intact.
//
// NOTE sur les UENUM : ces types sont volontairement du C++ nu (pas d'UHT, pas de .generated.h).
// Ils deviendront UENUM/USTRUCT en Phase 3, quand les opérateurs deviendront des data assets et
// auront besoin d'être édités dans l'éditeur. Les promouvoir plus tôt n'achèterait rien et
// ajouterait une étape UHT à chaque itération.

#pragma once

#include "CoreMinimal.h"
#include "VoxelTypes.h"        // CHUNK_SIZE, EVoxelTileClass

struct FBiomeContext;

//=============================================================================
// LES QUATRE RÔLES / THE FOUR ROLES
//=============================================================================
// Le rôle n'est pas décoratif : le compilateur de pile s'en sert pour ORDONNER. Les
// StructuralPost sont toujours ajoutés en dernier, dans l'ordre fixe spine → seal → passage →
// diff, quoi que l'auteur ait assemblé. Un auteur ne peut pas les omettre : la descente doit
// rester possible, les seals doivent tenir, les passages doivent percer, les éditions du joueur
// gagnent toujours.
enum class EVoxelOpRole : uint8
{
    // Rôle 1 — fabrique un champ à partir de rien. Une pile en a au moins un (sa racine).
    FieldSource,

    // Rôle 2 — fusionne le champ précédent avec le suivant. Voir EVoxelOpCombine.
    Combiner,

    // Rôle 3 — perturbe un champ existant près de sa surface (l'ancien UVoxelTerrainOpDefinition).
    DetailModifier,

    // Rôle 4 — invariants de monde. Ajoutés automatiquement, ordre fixe, non omissibles.
    StructuralPost,
};

//=============================================================================
// COMPOSITION / COMBINERS
//=============================================================================
// Vocabulaire délibérément petit, et il réutilise ce qui existe déjà
// (VoxelSDF::SmoothMin / SmoothMax).
//
// RAPPEL DE SIGNE — la source n°1 de confusion du plugin :
//   au mesher, NÉGATIF = SOLIDE, POSITIF = AIR (IsoLevel 0).
//   Donc « ajouter du solide » = prendre le MIN, « creuser de l'air » = prendre le MAX.
//   SIGN REMINDER: at the mesher NEGATIVE = SOLID, POSITIVE = AIR. So "add solid" is min(),
//   "carve air" is max(). Getting this backwards inverts the world.
enum class EVoxelOpCombine : uint8
{
    Replace,         // ignore l'entrée — racine de pile (heightfield, densité de base)
    Union,           // min() — ajoute du solide : ponts, îles, colonnes
    Subtract,        // max() — creuse de l'air : salles, tunnels, passages, spine
    SmoothUnion,     // VoxelSDF::SmoothMin(k) — jonctions organiques
    SmoothSubtract,  // VoxelSDF::SmoothMax(k)
    Add,             // accumulation scalaire — termes de bruit / rugosité
    Mask,            // met à l'échelle l'opérateur SUIVANT par un champ [0,1]
                     // (poids de biome, porte de pente, relief, profondeur).
                     // C'est Mask qui achète le plus d'expressivité : « cet opérateur, mais
                     // seulement dans les régions à fort relief » devient de la composition
                     // au lieu d'une garde codée en dur dans chaque opérateur.
};

//=============================================================================
// EFFET SUR UNE BOÎTE / EFFECT OVER A BOX
//=============================================================================
// CONSERVATIF PAR CONSTRUCTION. Rendre `Both` est TOUJOURS SÛR (ça ne coûte que du CPU) ;
// rendre le mauvais est un TROU — pas de géométrie, pas de collision, invisible jusqu'à ce
// qu'un joueur tombe au travers. En cas de doute : `Both`.
//
// CONSERVATIVE BY CONSTRUCTION. Returning `Both` is ALWAYS SAFE (it only costs CPU); returning
// the wrong one is a HOLE. When unsure: `Both`.
enum class EVoxelOpEffect : uint8
{
    // Prouvablement aucun effet sur cette boîte. C'est le early-out qui rend la pile rapide,
    // et c'est ce qui rendra le bedrock profond sautable pour les strates de grotte.
    Identity,

    // Ne peut que pousser la densité vers l'AIR ⇒ tue l'hypothèse « tout solide ».
    CarveOnly,

    // Ne peut que pousser la densité vers le SOLIDE ⇒ tue l'hypothèse « tout air ».
    FillOnly,

    // Non contraint ⇒ tue les deux hypothèses.
    Both,
};

//=============================================================================
// CONTEXTE DE CHUNK / CHUNK CONTEXT
//=============================================================================
// Miroir de ce que le bloc thread_local CP_* résout aujourd'hui dans GetDensityAt.
//
// ⚠️ LayoutVersion est ici PAR CONSTRUCTION, pas par politesse. AUDIT C2 : trois caches
// existants (CP_Chunk, OC_Chunk, BM_Chunk) sont clés sur ChunkCoord SEUL, donc après un
// RebuildStrates ou une édition à chaud un worker dont le cache est encore chaud pour ce chunk
// saute le refetch et génère avec les ANCIENS params. En faisant porter LayoutVersion par le
// contexte, un nouvel opérateur ne PEUT PAS oublier de l'inclure dans sa clé.
//
// LayoutVersion is here BY CONSTRUCTION, not by politeness — see AUDIT C2. Carrying it in the
// context means a new op CANNOT forget to put it in its cache key.
struct FVoxelOpContext
{
    FIntVector ChunkCoord = FIntVector::ZeroValue;

    // Pas d'échantillonnage LOD (1/2/4…). Un opérateur a le droit de se simplifier quand Step
    // est grand — c'est le contrat T2.b : le bruit volumétrique par voxel perd des octaves au
    // loin, le bruit de champ XY délibérément non (il alimente des caches box-validés partagés).
    int32 Step = 1;

    uint32 Seed = 0;

    // Compteur de génération du layout (UVoxelStrateManager::GetLayoutVersion()).
    // DOIT faire partie de toute clé de cache. Voir AUDIT C2.
    uint32 LayoutVersion = 0;

    // Bornes Z de la strate en coords VOXEL (pas cm).
    float StrateTopWorldZ = 0.0f;
    float StrateBottomWorldZ = 0.0f;

    // null = cette strate n'a pas de champ de biome.
    const FBiomeContext* Biome = nullptr;
};

//=============================================================================
// L'INTERFACE / THE INTERFACE
//=============================================================================
// Trois méthodes, et elles FORMALISENT CE QUE LE CODE FAIT DÉJÀ À LA MAIN : chaque archétype
// hisse déjà son travail constant-par-chunk dans un cache thread_local (= PrepareChunk), évalue
// à bas coût par voxel (= Eval), et possède déjà dans ClassifyTile une déclaration écrite à la
// main de ce qu'il peut faire à une tuile (= EffectOverBox). Ce n'est pas une nouvelle
// discipline, c'est la discipline existante, nommée.
class IVoxelDensityOp
{
public:
    virtual ~IVoxelDensityOp() = default;

    virtual EVoxelOpRole GetRole() const = 0;

    /**
     * Hisser ici TOUT le travail constant sur le chunk : listes de salles, grilles de biome,
     * caches de colonnes, cuissons de treillis. Appelé une fois par chunk et par worker.
     * C'est ici que déménagent les caches thread_local d'aujourd'hui.
     *
     * THREADING : appelé sur des workers. L'opérateur ne doit écrire QUE son propre état
     * par-chunk ; le Generator / le Mesher / le StrateManager restent en LECTURE SEULE
     * (invariant ARCHITECTURE §8.10). Toute clé de cache DOIT inclure Ctx.LayoutVersion.
     */
    virtual void PrepareChunk(const FVoxelOpContext& Ctx) = 0;

    /**
     * Par voxel. InDensity = ce que la pile a produit jusqu'ici, convention MC
     * (négatif = solide, positif = air). Coordonnées en VOXELS, pas en cm.
     *
     * INVARIANCE DE FENÊTRE (ARCHITECTURE §8.4) : fonction PURE de (coords monde, seed, layout).
     * Le même point évalué depuis une autre tuile, un autre ordre, un autre thread doit rendre le
     * float BIT-IDENTIQUE. Pas « proche » : 1 ULP d'écart entre deux fenêtres est une couture
     * visible, et en multijoueur une divergence de monde. Le test
     * VoxelForge.Determinism.DensityPurity vérifie cela.
     */
    virtual float Eval(float WorldX, float WorldY, float WorldZ, float InDensity) const = 0;

    /**
     * CONSERVATIF. Phase 1 : direction seule. Phase 3 : surcharge avec intervalle numérique.
     * Rendre Both est toujours sûr ; rendre le mauvais est un trou.
     *
     * Le contrat est « conservatif », pas « forme close » : un opérateur A LE DROIT
     * D'ÉCHANTILLONNER pour répondre. C'est exactement ce que fait ClassifyTile aujourd'hui pour
     * SurfaceWorld — il évalue ComputeSurfaceColumn sur le treillis EXACT du mesher, mêmes
     * fonctions, mêmes floats, donc verdict exact plutôt qu'estimé. Cela DOIT survivre au portage.
     *
     * The contract is "conservative", not "closed-form": an op MAY sample to answer.
     */
    virtual EVoxelOpEffect EffectOverBox(const FBox& VoxelBox, const FVoxelOpContext& Ctx) const = 0;

    /**
     * OPÉRATEURS FORÇANTS. Certains opérateurs ne « déplacent » pas la densité d'entrée : ils
     * l'ÉCRASENT. La question « dans quelle direction peux-tu bouger ce champ ? » n'a alors pas de
     * sens ; la bonne question est « sais-tu prouver que toute cette boîte est d'un seul côté,
     * QUELLE QUE SOIT l'entrée ? ».
     *
     * Deux familles répondent autre chose que Mixed :
     *   • les SOURCES (rôle 1) — elles posent le champ, donc elles le savent par construction ;
     *   • les op STRUCTURELS forçants — typiquement ApplyBoundarySeal, qui à l'intérieur de sa
     *     bande fait `Max(Density, SealFactor·BaseDensity)` avec SealFactor > 0 : le résultat est
     *     solide garanti quoi qu'il y ait eu avant. C'est exactement ce que ClassifyTile encode
     *     aujourd'hui avec « bande de seal ⇒ bCanAir = false » — et un simple FillOnly ne suffirait
     *     PAS à le reproduire (voir VF_FoldOp plus bas).
     *
     * Par défaut Mixed = « je ne sais pas », toujours sûr. Une source à primitives placées (graphe
     * de salles, îles, puits) répond en testant ses bornes ; une source heightfield répond en
     * échantillonnant ses colonnes sur le treillis exact, exactement comme aujourd'hui.
     *
     * FORCING OPS. Some ops do not *move* the input density, they *overwrite* it. Default Mixed =
     * "I don't know", always safe. The boundary seal is the non-source example, and it is the
     * reason this method exists at all rather than being folded into EffectOverBox.
     */
    virtual EVoxelTileClass ClassifyBox(const FBox& VoxelBox, const FVoxelOpContext& Ctx) const
    {
        return EVoxelTileClass::Mixed;
    }

    /**
     * Déclare si Eval dépend de Z. Les opérateurs purs en XY reçoivent le traitement du cache de
     * colonnes T1.a de façon GÉNÉRIQUE, au lieu que SurfaceWorld en ait un sur mesure.
     *
     * ⚠️ Le cache de colonnes est clé sur (boîte XY, StrateKey, Seed) SANS ChunkZ et il est
     * partagé sur TOUTE la pile verticale de chunks. Mettre une donnée dépendante de Z dans un
     * opérateur qui se déclare XY-pur corrompt silencieusement chaque chunk de la colonne, et
     * ValidateDeterminism — qui échantillonne le long d'une frontière en X — ne le verrait pas.
     */
    virtual bool IsXYPure() const { return false; }
};

//=============================================================================
// LE PLIAGE : comment la pile devient un verdict de tuile
// THE FOLD: how a stack becomes a tile verdict
//=============================================================================
// C'est l'algorithme générique qui remplace le ClassifyTile écrit à la main, et il REPRODUIT
// EXACTEMENT le comportement d'aujourd'hui — vérifié ligne à ligne contre VoxelGenerator.cpp :
//
//   source gap bedrock         → ClassifyBox = AllSolid
//   source SurfaceWorld        → ClassifyBox échantillonne les colonnes sur le treillis exact
//   ApplyPassageCarving        → CarveOnly   (tue AllSolid) ≡ « AnyPassageNearBox ⇒ bCanSolid=false »
//   ApplyOriginSpine           → CarveOnly   (tue AllSolid) ≡ le test cercle/boîte XY
//   ApplyBoundarySeal          → ClassifyBox = AllSolid DANS sa bande (opérateur forçant),
//                                FillOnly ailleurs         ≡ « bande de seal ⇒ bCanAir=false »
//   disturbances chasms        → CarveOnly   ≡ « ChasmDensity > 0 ⇒ bCanSolid=false »
//   disturbances ponts/arêtes  → FillOnly    ≡ « Bridge/RidgeDensity > 0 ⇒ bCanAir=false »
//   diff layer                 → Both si des mods touchent la boîte, sinon Identity
//                                             ≡ « HasAnyModInChunkRange ⇒ Mixed »
//
// et le verdict final « exactement une hypothèse survit, sinon Mixed » est littéralement le
// `if (bCanSolid == bCanAir) return Mixed;` de la fin de ClassifyTile.
//
// This fold reproduces today's hand-written ClassifyTile exactly — verified line by line against
// VoxelGenerator.cpp. That correspondence is the evidence that the abstraction fits this codebase
// rather than being imposed on it.

/** Les deux hypothèses que ClassifyTile poursuit, sous forme d'état pliable. */
struct FVoxelBoxHypotheses
{
    bool bCanBeAllSolid = true;
    bool bCanBeAllAir   = true;

    bool IsDead() const { return !bCanBeAllSolid && !bCanBeAllAir; }

    /** Verdict final : exactement une hypothèse doit survivre. Égalité = prudence ⇒ Mixed. */
    EVoxelTileClass Resolve() const
    {
        if (bCanBeAllSolid == bCanBeAllAir) { return EVoxelTileClass::Mixed; }
        return bCanBeAllSolid ? EVoxelTileClass::AllSolid : EVoxelTileClass::AllAir;
    }
};

/** Poser l'état depuis un verdict FORÇANT (source, ou seal dans sa bande) : l'opérateur écrase
 *  l'entrée, donc il écrase aussi tout ce que la pile avait conclu avant lui. Un verdict « tout
 *  air » affirme du même coup « pas tout solide », et réciproquement. */
FORCEINLINE void VF_ForceHypotheses(FVoxelBoxHypotheses& H, EVoxelTileClass ForcedVerdict)
{
    switch (ForcedVerdict)
    {
    case EVoxelTileClass::AllSolid: H.bCanBeAllSolid = true;  H.bCanBeAllAir = false; break;
    case EVoxelTileClass::AllAir:   H.bCanBeAllSolid = false; H.bCanBeAllAir = true;  break;
    case EVoxelTileClass::Mixed:
    default:                        H.bCanBeAllSolid = false; H.bCanBeAllAir = false; break;
    }
}

/** Plier l'effet DIRECTIONNEL d'un opérateur dans l'état. Monotone : ne fait que tuer. */
FORCEINLINE void VF_FoldEffect(FVoxelBoxHypotheses& H, EVoxelOpEffect Effect)
{
    switch (Effect)
    {
    case EVoxelOpEffect::Identity:                                          break;
    case EVoxelOpEffect::CarveOnly: H.bCanBeAllSolid = false;               break;
    case EVoxelOpEffect::FillOnly:  H.bCanBeAllAir   = false;               break;
    case EVoxelOpEffect::Both:
    default:                        H.bCanBeAllSolid = false;
                                    H.bCanBeAllAir   = false;               break;
    }
}

/**
 * Plier UN opérateur. L'ORDRE COMPTE ICI, et c'est délibéré : un opérateur forçant écrase ce que
 * la pile avait conclu AVANT lui, tandis que les opérateurs qui suivent continuent de s'appliquer.
 *
 * Exemple à garder en tête, parce qu'il est le piège : sur une boîte entièrement dans la bande de
 * seal, la source dit peut-être « tout air » (on est au-dessus du terrain), puis le seal FORCE
 * « tout solide » — verdict AllSolid, comme aujourd'hui. Si le seal ne savait dire que FillOnly,
 * on obtiendrait « les deux hypothèses mortes ⇒ Mixed » : pas un trou, mais la perte pure et
 * simple d'une des tuiles triviales que T1.d sait sauter. C'est pour cela que ClassifyBox existe.
 *
 * Réciproquement, un passage qui traverse cette même boîte rend CarveOnly APRÈS le seal et retue
 * l'hypothèse solide ⇒ Mixed. Identique au code actuel, où la garde passage et la garde seal se
 * neutralisent en `bCanSolid == bCanAir`.
 *
 * ORDER MATTERS HERE, deliberately: a forcing op overwrites what the stack concluded before it,
 * while ops after it still apply. This is what lets the seal recover an AllSolid verdict that a
 * pure FillOnly would have thrown away, while still letting a passage take it back.
 */
FORCEINLINE void VF_FoldOp(FVoxelBoxHypotheses& H, const IVoxelDensityOp& Op,
                           const FBox& VoxelBox, const FVoxelOpContext& Ctx)
{
    const EVoxelTileClass Forced = Op.ClassifyBox(VoxelBox, Ctx);
    if (Forced != EVoxelTileClass::Mixed)
    {
        VF_ForceHypotheses(H, Forced);
        return;
    }
    VF_FoldEffect(H, Op.EffectOverBox(VoxelBox, Ctx));
}
