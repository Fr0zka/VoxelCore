// VoxelDensityOp.h
// LE CONTRAT de la pile d'opérateurs de densité / THE density operator stack CONTRACT.
// Design : Docs/archive/OPSTACK-PLAN.md. The contract is implemented by the opt-in stack path in
// UVoxelGenerator::GetDensityAt; the legacy archetype switch remains for non-opt-in strates.
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
//   4. STRUCTURAL POST   — spine (0,0) → seal vertical → passages → seal XY → diff layer. Des INVARIANTS de monde,
//                          pas des choix créatifs : toujours ajoutés, dans cet ordre, jamais
//                          omissibles par l'auteur.
//
// The old system only ever had role 3. Roles 1 and 2 are the new thing, and role 4 is what keeps
// the descent structure intact no matter what an author assembles.
//
// ─────────────────────────────────────────────────────────────────────────────────────────
// LA CLÉ DE VOÛTE : EffectOverBox + intervalle SDF / THE KEYSTONE: EffectOverBox + SDF interval
// ─────────────────────────────────────────────────────────────────────────────────────────
// Un verdict de boîte doit être une preuve. Rendre `Both` coûte du CPU ; rendre `Identity` ou une
// direction fausse supprime de la géométrie et peut laisser un joueur tomber au travers du monde.
// En cas de doute, `Mixed`.
//
// Un opérateur qui écrit seulement le canal SDF ne peut pas honnêtement répondre pour le convertisseur
// qui viendra peut-être après lui : il doit propager un intervalle SDF, et le convertisseur doit plier
// cet intervalle avec sa propre formule. Chaque opérateur répond ainsi pour lui-même, et la composition
// reste sûre même pour une combinaison que personne n'a écrite à la main auparavant.
//
// A box verdict is a proof. `Both` costs CPU; a false `Identity` or direction removes geometry and can
// let a player fall through the world. When in doubt, return `Mixed`.
// An SDF-only writer reports an SDF interval; a later converter folds that interval through its own
// response. Each operator answers for itself, so an unverified composition remains sound.
//
// La première version de cette interface n'avait qu'une direction numérique. Presque tout opérateur
// existant est UNIDIRECTIONNEL : il ne fait que creuser, ou que remplir. Cela suffit à reproduire
// GÉNÉRIQUEMENT chaque garde écrite à la main dans ClassifyTile :
//
//     « passages ⇒ bCanSolid = false »        EST     CarveOnly
//     « ponts/arêtes ⇒ bCanAir = false »      EST     FillOnly
//     « aucun passage près de cette boîte »   EST     Identity
//
// La direction reste le repli conservateur par défaut pour les opérateurs qui ne connaissent pas
// d'intervalle ; elle ne permet jamais à un écrivain SDF d'usurper la réponse d'un voisin.
//
// Direction remains the conservative fallback for operators without an interval implementation;
// it never lets an SDF writer impersonate a downstream neighbour.
//
// ─────────────────────────────────────────────────────────────────────────────────────────
// LE GROS LOT PERF : les strates de grotte ne sautent AUCUNE tuile aujourd'hui
// THE PERF PRIZE: cave strates skip ZERO tiles today
// ─────────────────────────────────────────────────────────────────────────────────────────
// ClassifyTile conserve ses preuves exactes pour les gaps de bedrock et SurfaceWorld ; les
// archétypes de cave opt-in passent maintenant par le même pliage `ClassifyBox`. Le post XY fournit
// en plus la première preuve globale de coque : une tuile entièrement dans la bande forcée peut être
// sautée sans connaître l'archétype ou le Z.
//
// **Traiter cela comme un livrable explicite de chaque portage, pas comme un effet de bord.**
//
// ─────────────────────────────────────────────────────────────────────────────────────────
// ÉTAT / STATUS
// ─────────────────────────────────────────────────────────────────────────────────────────
// Les huit archétypes ont maintenant une pile derrière l'opt-in `bUseOperatorStack`, et ClassifyTile
// utilise cette pile pour les slots de cave opt-in. Le chemin legacy reste disponible pour les
// strates non opt-in; les tests d'équivalence servent de garde de portage.
//
// All eight archetypes now have an operator-stack path behind `bUseOperatorStack`, and ClassifyTile
// uses that stack for opt-in cave slots. The legacy switch remains available for opt-out strates;
// equivalence tests are port-correctness guards.
//
// NOTE sur les UENUM : ces types sont volontairement du C++ nu (pas d'UHT, pas de .generated.h).
// Ils deviendront UENUM/USTRUCT en Phase 3, quand les opérateurs deviendront des data assets et
// auront besoin d'être édités dans l'éditeur. Les promouvoir plus tôt n'achèterait rien et
// ajouterait une étape UHT à chaque itération.

#pragma once

#include "CoreMinimal.h"
#include "VoxelTypes.h"        // CHUNK_SIZE, EVoxelTileClass

struct FBiomeContext;
struct FTunnelCoreWorldEvaluation;
struct FChunkSDFCache;

//=============================================================================
// LES QUATRE RÔLES / THE FOUR ROLES
//=============================================================================
// Le rôle n'est pas décoratif : le compilateur de pile s'en sert pour ORDONNER. Les
// StructuralPost sont toujours ajoutés en dernier, dans l'ordre fixe spine → seal → passage →
// seal XY → diff, quoi que l'auteur ait assemblé. Un auteur ne peut pas les omettre : la descente doit
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
// CANAUX DE L'ÉCHANTILLON / SAMPLE CHANNELS
//=============================================================================
// `FVoxelOpSample` is deliberately a small, explicit state record. Keep its channel set here,
// next to the interface that declares which fields an operator consumes and publishes. The enum
// values are bits so a declaration can name more than one channel without introducing another
// per-voxel object or a dynamic container.
//
// `Density` is the internal positive=solid field; `Sdf` is the standard negative=inside field.
// If a future field is added to `FVoxelOpSample`, it must also be added here and to every operator
// declaration before the composer can treat that field as part of the dependency graph.
enum class EVoxelOpChannel : uint8
{
    None    = 0,
    Density = 1 << 0,
    Sdf     = 1 << 1,
};

using EVoxelOpChannelMask = uint8;

namespace VoxelOpChannels
{
    static constexpr EVoxelOpChannelMask None    = 0;
    static constexpr EVoxelOpChannelMask Density = static_cast<EVoxelOpChannelMask>(EVoxelOpChannel::Density);
    static constexpr EVoxelOpChannelMask Sdf     = static_cast<EVoxelOpChannelMask>(EVoxelOpChannel::Sdf);
    static constexpr EVoxelOpChannelMask All     = Density | Sdf;
}

//=============================================================================
// OPERATOR RESOURCES / RESSOURCES D'OPERATEUR
//=============================================================================
// Sample-channel declarations describe the FVoxelOpSample only.  Some operators also consume
// stateful geometry published by an earlier source (the room graph, shaft field, or surface
// column).  Keep that dependency explicit as a second, tiny graph so the composer cannot place a
// room modifier on a stack that has no room source merely because its Density/Sdf masks happen to
// fit.
enum class EVoxelOpResource : uint8
{
    None          = 0,
    RoomGeometry  = 1 << 0,
    ShaftGeometry = 1 << 1,
    SurfaceColumn = 1 << 2,
};

using EVoxelOpResourceMask = uint8;

namespace VoxelOpResources
{
    static constexpr EVoxelOpResourceMask None          = 0;
    static constexpr EVoxelOpResourceMask RoomGeometry  = static_cast<EVoxelOpResourceMask>(EVoxelOpResource::RoomGeometry);
    static constexpr EVoxelOpResourceMask ShaftGeometry = static_cast<EVoxelOpResourceMask>(EVoxelOpResource::ShaftGeometry);
    static constexpr EVoxelOpResourceMask SurfaceColumn = static_cast<EVoxelOpResourceMask>(EVoxelOpResource::SurfaceColumn);
    static constexpr EVoxelOpResourceMask All = RoomGeometry | ShaftGeometry | SurfaceColumn;
}

//=============================================================================
// COMPOSITION / COMBINERS
//=============================================================================
// Vocabulaire délibérément petit, et il réutilise ce qui existe déjà
// (VoxelSDF::SmoothMin / SmoothMax).
//
// ⚠️⚠️ RAPPEL DE SIGNE — LA source n°1 de confusion du plugin, et il y a DEUX conventions en jeu.
// Lire ceci en entier avant d'écrire un opérateur.
//
//   • CANAL DENSITÉ, à l'intérieur de la pile : convention INTERNE, **POSITIF = SOLIDE**.
//     C'est celle dans laquelle CHAQUE fonction d'archétype est écrite aujourd'hui. La négation
//     vers la convention marching-cubes (négatif = solide) se fait UNE FOIS, tout à la fin, par
//     l'appelant. Donc ici : « ajouter du solide » = MAX, « creuser de l'air » = MIN.
//
//   • CANAL SDF : convention SDF standard, **NÉGATIF = À L'INTÉRIEUR de la primitive**.
//     Réunir deux formes = MIN (c'est `SmoothMin`, ce que fait déjà le code pour salle+puits).
//     Le sens de « min » est donc l'INVERSE d'un canal à l'autre. Ce n'est pas une incohérence :
//     un SDF décrit une FORME, une densité décrit de la MATIÈRE.
//
//   DENSITY channel inside the stack: INTERNAL convention, **POSITIVE = SOLID** (what every
//   archetype body already uses; the MC negate happens once, at the end, in the caller). So
//   "add solid" is max(), "carve air" is min().
//   SDF channel: standard SDF, **NEGATIVE = INSIDE the primitive**; unioning shapes is min().
//   The meaning of min() is therefore opposite between the two channels — an SDF describes a
//   SHAPE, a density describes MATTER.
enum class EVoxelOpCombine : uint8
{
    Replace,         // ignore l'entrée — racine de pile (heightfield, densité de base)
    Union,           // max() sur la DENSITÉ — ajoute du solide : ponts, îles, colonnes
    Subtract,        // min() sur la DENSITÉ — creuse de l'air : salles, tunnels, passages, spine
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

    // Global XY world-edge invariant. These are copied from UVoxelSettings by the generator and
    // travel with the chunk context so the automatically appended edge op cannot be omitted by a
    // builder. Radius 0 is the legacy unbounded/no-op mode.
    float WorldRadiusVoxels = 8192.0f;
    float EdgeSealThickness = 64.0f;

    // Bornes Z de la strate en coords VOXEL (pas cm).
    float StrateTopWorldZ = 0.0f;
    float StrateBottomWorldZ = 0.0f;

    // ClassifyTile may ask box bounds about the exact marching-cubes lattice rather than the
    // continuous volume between samples.  Production voxel evaluation leaves this disabled;
    // when enabled, operators may tighten a conservative bound only for points
    // LatticeOriginVoxels + n * Step that lie inside the queried box.
    bool bUseLatticeProof = false;
    FIntVector LatticeOriginVoxels = FIntVector::ZeroValue;

    // A mixed tile may opt into a tighter, still conservative finite-box bound for domain-warped
    // sources.  The normal classifier leaves this off; ClassifyTile retries only the few boxes
    // that the cheap global proof could not resolve.
    bool bTightenWarpProof = false;

    // null = cette strate n'a pas de champ de biome.
    const FBiomeContext* Biome = nullptr;
};

//=============================================================================
// L'ÉTAT QUI TRAVERSE LA PILE / THE STATE THE STACK THREADS THROUGH
//=============================================================================
// DEUX canaux, pas un. Ce n'est pas de la généralité gratuite — c'est ce que le code fait déjà :
//
//   CaveSDF = EvaluateSDFCached(salles + tunnels)          ← espace SDF
//   CaveSDF = SmoothMin(CaveSDF, PitSDF,     BlendK)       ← espace SDF
//   CaveSDF = SmoothMin(CaveSDF, ChimneySDF, BlendK)       ← espace SDF
//   → UN SEUL carve à la fin : Density -= CarveFactor · BaseDensity · 2
//
// Maze, VerticalShafts et FloatingIslands ont la même forme, et TROIS d'entre eux appliquent la
// rugosité au **SDF** (`MazeSDF += bruit·Rough`), pas à la densité. Sur la densité, le même bruit
// est mis à l'échelle par le gradient local : effet visiblement différent.
//
// Avec un seul canal, un opérateur ne peut qu'ÉCRASER le précédent — les jonctions SmoothMin
// (salle↔puits, et demain « un graphe de salles creusé DANS une montagne ») sont impossibles.
// Un `SmoothMin` entre deux SOURCES différentes est précisément ce qui fait qu'une idée composée
// a l'air d'appartenir au lieu au lieu d'y avoir été percée. Coût : un float.
//
// Two channels, not one — because that is what the code already does, and because SmoothMin between
// two different SOURCES is precisely what makes a composed idea look like it belongs there rather
// than like a hole punched in something else. Cost: one float.
struct FVoxelOpSample
{
    // Convention INTERNE : POSITIF = SOLIDE. Négation vers MC une seule fois, par l'appelant.
    // INTERNAL convention: POSITIVE = SOLID. Negated to MC once, by the caller.
    float Density = 0.0f;

    // Convention SDF standard : NÉGATIF = à l'intérieur de la primitive.
    // FLT_MAX = « aucune surface à proximité » (l'état initial, et le early-out des sources
    // placées quand aucune primitive n'atteint ce voxel).
    // FLT_MAX = "no surface nearby" — the initial state, and the early-out placed sources use.
    float Sdf = FLT_MAX;

    // Block evaluation keeps the room source's per-sample selection explicit.  The scalar path
    // still uses the source's worker-local state, but a later block-capable modifier must never
    // observe the state belonging to the last sample of the preceding operator.
    const FChunkSDFCache* RoomCache = nullptr;
    int32 NearestRoomIndex = INDEX_NONE;

    // The common generator tail consumes the continuous swept-shape metadata after the block has
    // left the stack. Keeping it in the sample avoids re-running the world-space tunnel scan or
    // consulting a last-sample TLS value after an op-major evaluation.
    bool bHasTunnelCoreWorldEvaluation = false;
    float TunnelCoreWorldSDF = FLT_MAX;
    bool bTunnelCoreRoomFloor = false;
    bool bHasTunnelCoreSweptFloor = false;
    float TunnelCoreSweptFloorZ = -FLT_MAX;
    float TunnelCoreSweptFloorRadius = 0.0f;
};

/**
 * A regular world lattice handed to one operator at a time.
 *
 * The default implementation is deliberately scalar and therefore source-compatible with custom
 * operators.  An operator may override EvalBlock only when it preserves the exact per-sample
 * semantics of Eval.  Samples are laid out X-fastest, then Y, then Z; all coordinates are voxel
 * coordinates and are formed as Origin + index * Step using the same integer arithmetic as the
 * mesher's scalar path.
 */
struct FVoxelOpBlock
{
    FIntVector OriginVoxels = FIntVector::ZeroValue;
    int32 Step = 1;
    int32 SizeX = 0;
    int32 SizeY = 0;
    int32 SizeZ = 0;
    FVoxelOpSample* Samples = nullptr;

    int32 NumSamples() const { return SizeX * SizeY * SizeZ; }

    FORCEINLINE FVoxelOpSample& At(int32 X, int32 Y, int32 Z) const
    {
        return Samples[(Z * SizeY + Y) * SizeX + X];
    }
};

/**
 * Intervalle conservateur du canal SDF sur une boîte.
 *
 * `Min` et `Max` sont les bornes SUPRÊMES de toutes les valeurs que l'opérateur peut produire dans
 * la boîte. `[FLT_MAX, FLT_MAX]` est l'identité exacte : aucune surface connue. Un intervalle inconnu
 * est volontairement large ; il force le fold à rester `Mixed` plutôt que de risquer un faux tile
 * uniforme. Les setters refusent toute borne non finie ou inversée : un opérateur qui ne peut pas
 * prouver son intervalle perd le skip, jamais la géométrie.
 *
 * Conservative SDF interval over a box. `Min`/`Max` bound every value the operator can produce.
 * `[FLT_MAX, FLT_MAX]` is the exact "no surface" identity. Unknown is deliberately wide: it costs
 * a skip, never a hole. Invalid bounds become unknown instead of being guessed.
 */
struct FVoxelBoxSdfInterval
{
    bool  bKnown = true;
    float Min = FLT_MAX;
    float Max = FLT_MAX;

    bool IsKnown() const { return bKnown; }

    void SetUnknown()
    {
        bKnown = false;
        Min = -FLT_MAX;
        Max = FLT_MAX;
    }

    void Set(float InMin, float InMax)
    {
        if (!VoxelMath::IsFinite(InMin) || !VoxelMath::IsFinite(InMax) || InMin > InMax)
        {
            SetUnknown();
            return;
        }
        bKnown = true;
        Min = InMin;
        Max = InMax;
    }

    /** Output of `min(Input, Other)` when both intervals are known. */
    void MinWith(const FVoxelBoxSdfInterval& Other)
    {
        if (!bKnown || !Other.bKnown)
        {
            SetUnknown();
            return;
        }
        Min = FMath::Min(Min, Other.Min);
        Max = FMath::Min(Max, Other.Max);
    }
};

// Forward declaration: the interval-aware EffectOverBox overloads consume the hypotheses that
// have already been folded, while the hypotheses themselves are declared immediately below the
// interface.
struct FVoxelBoxHypotheses;

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
     * Channel contract for the composer. A read is a field inspected from `InOut` before this op
     * publishes its result; a write is a field whose value may differ when the op returns. A
     * read/write declaration therefore means "transform the current version". `+=` and `-=` are
     * both reads and writes; an assignment that ignores the old field is a write-only producer /
     * replacement. Context, coordinates, caches and referenced objects are not sample channels.
     *
     * These are graph metadata, not voxel work: the stack snapshots them while it is assembled and
     * the validator reads the snapshot. They must never be called from `Eval`.
     */
    virtual EVoxelOpChannelMask ChannelReads() const = 0;
    virtual EVoxelOpChannelMask ChannelWrites() const = 0;

    /**
     * Non-sample state required by this op.  A declaration is satisfied only by a provider that
     * appeared earlier in the stack.  Defaults are deliberately empty so existing independent
     * operators and external test doubles remain source-compatible.
     */
    virtual EVoxelOpResourceMask RequiredResources() const
    {
        return VoxelOpResources::None;
    }

    /** State published for later consumers, e.g. FRoomGraphSource's room geometry cache. */
    virtual EVoxelOpResourceMask ProvidedResources() const
    {
        return VoxelOpResources::None;
    }

    /**
     * True when this op contributes a delta to its written channels without replacing, clamping,
     * selecting, or otherwise depending on the previous value of those written channels. Such an
     * op may be shuffled with another compatible additive delta; a false result means its position
     * is explicit. A gate on a channel the op itself writes is transformative even if its final
     * arithmetic contains `+=`.
     */
    virtual bool IsAdditive() const = 0;

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
     * Par voxel. `InOut` est l'état que la pile a produit jusqu'ici (voir FVoxelOpSample).
     * Coordonnées en VOXELS, pas en cm.
     *
     * INVARIANCE DE FENÊTRE (ARCHITECTURE §8.4) : fonction PURE de (coords monde, seed, layout).
     * Le même point évalué depuis une autre tuile, un autre ordre, un autre thread doit rendre le
     * float BIT-IDENTIQUE. Pas « proche » : 1 ULP d'écart entre deux fenêtres est une couture
     * visible, et en multijoueur une divergence de monde. Le test
     * VoxelForge.Determinism.DensityPurity vérifie cela.
     */
    virtual void Eval(float WorldX, float WorldY, float WorldZ, FVoxelOpSample& InOut) const = 0;

    /**
     * Op-major evaluation seam.  The conservative default is intentionally not clever: it calls
     * the canonical scalar Eval for every sample.  This lets the stack and mesher be converted in
     * reviewable stages while byte-identical output remains the acceptance test.  Stateful ops
     * may restore their per-sample hand-off from FVoxelOpSample in PrepareBlockSample; the default
     * invokes that hook before each scalar fallback call.
     */
    virtual void EvalBlock(const FVoxelOpBlock& Block) const
    {
        if (Block.Samples == nullptr || Block.Step <= 0
            || Block.SizeX <= 0 || Block.SizeY <= 0 || Block.SizeZ <= 0)
        {
            return;
        }

        for (int32 Z = 0; Z < Block.SizeZ; ++Z)
        {
            for (int32 Y = 0; Y < Block.SizeY; ++Y)
            {
                for (int32 X = 0; X < Block.SizeX; ++X)
                {
                    FVoxelOpSample& Sample = Block.At(X, Y, Z);
                    PrepareBlockSample(Sample);
                    Eval(
                        static_cast<float>(Block.OriginVoxels.X + X * Block.Step),
                        static_cast<float>(Block.OriginVoxels.Y + Y * Block.Step),
                        static_cast<float>(Block.OriginVoxels.Z + Z * Block.Step),
                        Sample);
                }
            }
        }
    }

    /** Restore a source-owned per-sample context before a scalar fallback in EvalBlock. */
    virtual void PrepareBlockSample(const FVoxelOpSample&) const {}

    /** Optional hand-off for the common structural tail.  The default keeps custom operators
     * conservative: the caller uses its canonical cache path when no source publishes a result. */
    virtual bool TryGetLastTunnelCoreWorldEvaluation(
        FTunnelCoreWorldEvaluation& OutEvaluation) const
    {
        return false;
    }

    /**
     * CONSERVATIF. The two-argument method is the intrinsic fallback. The state-aware overload
     * below is what the stack calls: it sees the SDF interval already produced by earlier ops.
     * Returning Both is always safe; returning the wrong direction is a hole.
     *
     * Le contrat est « conservatif », pas « forme close » : un opérateur A LE DROIT
     * D'ÉCHANTILLONNER pour répondre. C'est exactement ce que fait ClassifyTile aujourd'hui pour
     * SurfaceWorld — il évalue ComputeSurfaceColumn sur le treillis EXACT du mesher, mêmes
     * fonctions, mêmes floats, donc verdict exact plutôt qu'estimé. Cela DOIT survivre au portage.
     *
     * The contract is "conservative", not "closed-form": an op MAY sample to answer.
     *
     * An SDF-only source MUST NOT answer for a converter that happens to follow it. It reports its
     * own interval through `PropagateSdfOverBox`; the converter's state-aware overload then folds
     * the interval through its own formula. This is the isolation rule that makes free composition
     * safe.
     */
    virtual EVoxelOpEffect EffectOverBox(const FBox& VoxelBox, const FVoxelOpContext& Ctx) const = 0;

    /** State-aware effect. The default preserves old custom operators' conservative direction. */
    virtual EVoxelOpEffect EffectOverBox(const FBox& VoxelBox, const FVoxelOpContext& Ctx,
                                         const FVoxelBoxHypotheses&) const
    {
        return EffectOverBox(VoxelBox, Ctx);
    }

    /**
     * ⚠️ LE PLIAGE QUI PORTE DES NOMBRES — `OPSTACK-DECOMPOSITION §0.2`, et le plus gros poste de
     * perf du plan.
     *
     * La direction seule ne suffit pas pour les opérateurs FIELDÉS. Un carve à seuil de bruit (les
     * vers de TunnelNetwork, la rugosité de paroi) n'a AUCUNE borne spatiale : il rend `CarveOnly`
     * sur CHAQUE boîte de CHAQUE strate qui l'active, donc il tue l'hypothèse `AllSolid` partout et
     * l'archétype ne saute pas une tuile. Aucun raffinement de `EffectOverBox` ne peut le récupérer,
     * parce que la réponse « oui, je peux creuser ici » est VRAIE.
     *
     * **Mais son AMPLITUDE est bornée, et souvent triviale** : pour un ver, `t ∈ [0,1]` et
     * `Mask ∈ [0,1]`, donc il ne peut déplacer la densité vers l'air que de `WormStrength` au plus.
     * Si le roc est solide d'une marge SUPÉRIEURE à la somme de tous les carves restants, la boîte
     * est prouvablement pleine — quel que soit le bruit.
     *
     * D'où deux nombres, en unités de DENSITÉ (convention interne, positif = solide) :
     *   • `MaxCarveOverBox` — de combien AU PLUS cet opérateur peut baisser la densité sur la boîte,
     *   • `MaxFillOverBox`  — de combien AU PLUS il peut la monter.
     *
     * **`FLT_MAX` = « je ne sais pas », et c'est le DÉFAUT.** Un opérateur qui ne redéfinit rien se
     * comporte donc EXACTEMENT comme avant ce changement : le pliage retire `FLT_MAX` à la marge,
     * elle passe sous zéro, l'hypothèse meurt. Les treize tests d'équivalence et
     * `VoxelForge.OpStack.BoxVerdictFold` ne bougent pas d'un verdict.
     *
     * ⚠️ SENS DE L'ERREUR : SUR-estimer une amplitude coûte du CPU (une tuile maillée pour rien) ;
     * SOUS-estimer produit un TROU. Comme partout ailleurs dans ce fichier, en cas de doute rendre
     * `FLT_MAX`. Ce n'est pas une borne « raisonnable », c'est une borne PROUVÉE ou rien.
     *
     * The fold carries NUMBERS, not just directions. A fielded noise carve has no spatial bound but
     * its AMPLITUDE is bounded, so "the rock is solid by more than the sum of every remaining carve"
     * becomes provable. FLT_MAX means "unknown" and is the default, so every existing op is
     * unchanged. Over-estimating costs CPU; under-estimating is a hole.
     */
    virtual float MaxCarveOverBox(const FBox& VoxelBox, const FVoxelOpContext& Ctx) const
    {
        return FLT_MAX;
    }

    /** State-aware amplitude. The default is the old, conservative amplitude. */
    virtual float MaxCarveOverBox(const FBox& VoxelBox, const FVoxelOpContext& Ctx,
                                  const FVoxelBoxHypotheses&) const
    {
        return MaxCarveOverBox(VoxelBox, Ctx);
    }

    virtual float MaxFillOverBox(const FBox& VoxelBox, const FVoxelOpContext& Ctx) const
    {
        return FLT_MAX;
    }

    /** State-aware amplitude. The default is the old, conservative amplitude. */
    virtual float MaxFillOverBox(const FBox& VoxelBox, const FVoxelOpContext& Ctx,
                                 const FVoxelBoxHypotheses&) const
    {
        return MaxFillOverBox(VoxelBox, Ctx);
    }

    /**
     * Propagate this operator's own SDF result through a box query. Only call this for an operator
     * whose cached channel declaration writes `Sdf`. The default is unknown by construction: a new
     * SDF writer cannot accidentally make a false uniform verdict merely because it forgot this
     * method. Implementations that assign SDF replace the interval; min/accumulating writers use
     * `FVoxelBoxSdfInterval::MinWith`.
     */
    virtual void PropagateSdfOverBox(FVoxelBoxSdfInterval& InOut, const FBox& VoxelBox,
                                     const FVoxelOpContext&) const
    {
        InOut.SetUnknown();
    }

    /**
     * Pour un opérateur FORÇANT (celui dont `ClassifyBox` rend autre chose que `Mixed`) : de combien
     * la densité est-elle garantie du bon côté de zéro, PARTOUT dans la boîte ?
     *
     * C'est l'autre moitié du pliage numérique. `MaxCarveOverBox` dit ce qu'on peut RETIRER ; ceci
     * dit ce qu'il y avait à retirer. Sans les deux, la soustraction n'a pas de premier terme.
     *
     * Exemple, et c'est LE cas qui compte : `FConstantFieldSource` pose `Density = BaseDensity`
     * partout. Sa marge est donc exactement `BaseDensity`. Un ver à `WormStrength = 0.6` sur un roc
     * à `BaseDensity = 1.0` laisse 0.4 de marge ⇒ la boîte reste prouvablement pleine.
     *
     * **0 = « je ne sais pas », et c'est le DÉFAUT** : la marge tombe à zéro, le premier carve la
     * fait passer sous zéro, l'hypothèse meurt — le comportement d'avant, à l'identique.
     */
    virtual float ForcedMarginOverBox(const FBox& VoxelBox, const FVoxelOpContext& Ctx) const
    {
        return 0.0f;
    }

    /**
     * OPÉRATEURS FORÇANTS. Certains opérateurs ne « déplacent » pas la densité d'entrée : ils
     * l'ÉCRASENT. La question « dans quelle direction peux-tu bouger ce champ ? » n'a alors pas de
     * sens ; la bonne question est « sais-tu prouver que toute cette boîte est d'un seul côté,
     * QUELLE QUE SOIT l'entrée ? ».
     *
     * Deux familles répondent autre chose que Mixed :
     *   • les SOURCES (rôle 1) — elles posent le champ, donc elles le savent par construction ;
     *   • les op STRUCTURELS forçants — ApplyBoundarySeal et ApplyXYEdgeSeal — qui, dans leur bande
     *     prouvée, font `Max(Density, SealFactor·BaseDensity)` avec SealFactor > 0 : le résultat est
     *     solide garanti quoi qu'il y ait eu avant. C'est exactement ce que ClassifyTile encode
     *     aujourd'hui avec « bande de seal ⇒ bCanAir = false » — et un simple FillOnly ne suffirait
     *     PAS à le reproduire (voir VF_FoldOp plus bas).
     *
     * Par défaut Mixed = « je ne sais pas », toujours sûr. Une source à primitives placées (graphe
     * de salles, îles, puits) répond en testant ses bornes ; une source heightfield répond en
     * échantillonnant ses colonnes sur le treillis exact, exactement comme aujourd'hui.
     *
     * FORCING OPS. Some ops do not *move* the input density, they *overwrite* it. Default Mixed =
     * "I don't know", always safe. The vertical and XY edge seals are the non-source examples, and
     * they are the reason this method exists at all rather than being folded into EffectOverBox.
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

    /**
     * DIAGNOSTIC UNIQUEMENT — le nom que les rapports de test impriment pour cet opérateur.
     *
     * ⚠️ POURQUOI CETTE MÉTHODE EXISTE. Sans nom par opérateur, un rapport « 0 tuile prouvée sur
     * 40 » ne peut dire que « ou bien les tuiles traversent toutes une grotte, ou bien la source
     * n'atteint pas sa branche `Identity` » — deux causes, zéro nombre pour les départager, alors
     * que la vraie cause peut être un TROISIÈME opérateur (p. ex. un ver qui rend `CarveOnly`
     * partout). Avec un nom par opérateur, `ClassifyBoxAttributed` répond « c'est celui-là » au
     * lieu de laisser deviner.
     *
     * N'entre dans AUCUNE clé de cache, dans aucun hash, dans aucune décision de génération : le
     * changer ne peut pas changer le monde. Le défaut est volontairement laconique — un opérateur
     * sans nom se repère à son index, ce qui suffit à savoir où regarder.
     *
     * Diagnostic only: the first build of the spatial EffectOverBox came back green with 0 tiles
     * proved, and the report could not name which operator was killing the hypothesis. It was a
     * third one nobody was looking at. Never part of a cache key or any generation decision.
     */
    virtual const TCHAR* DebugName() const { return TEXT("(unnamed op)"); }
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
//   ApplyXYEdgeSeal             → ClassifyBox = AllSolid DANS sa bande radiale (forçant) ; il gagne
//                                après les passages, donc la coque extérieure reste fermée.
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

    /**
     * ⚠️ LES DEUX NOMBRES DU PLIAGE (`OPSTACK-DECOMPOSITION §0.2`).
     * `SolidMargin` = de combien la densité est encore garantie AU-DESSUS de zéro partout dans la
     * boîte, SOUS l'hypothèse « tout solide ». Un opérateur forçant la pose ; chaque carve en retire
     * son amplitude maximale ; quand elle n'est plus strictement positive, l'hypothèse meurt.
     * `AirMargin` est son miroir.
     *
     * **Elles valent 0 sur un état neuf, et c'est ce qui rend le changement rétro-compatible :**
     * sans opérateur forçant qui déclare une marge, le premier carve fait `0 − FLT_MAX < 0` et tue
     * l'hypothèse — le comportement exact d'avant le pliage numérique.
     */
    float SolidMargin = 0.0f;
    float AirMargin   = 0.0f;

    /** SDF interval after all SDF writers folded so far. It starts at the exact no-surface identity. */
    FVoxelBoxSdfInterval Sdf;

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
/** @param Margin  de combien la densité est garantie du bon côté de zéro dans toute la boîte.
 *                 0 (le défaut) = « je ne sais pas » ⇒ comportement d'avant le pliage numérique. */
FORCEINLINE void VF_ForceHypotheses(FVoxelBoxHypotheses& H, EVoxelTileClass ForcedVerdict,
                                    float Margin = 0.0f)
{
    switch (ForcedVerdict)
    {
    case EVoxelTileClass::AllSolid: H.bCanBeAllSolid = true;  H.bCanBeAllAir = false;
                                    H.SolidMargin = Margin;   H.AirMargin = 0.0f;     break;
    case EVoxelTileClass::AllAir:   H.bCanBeAllSolid = false; H.bCanBeAllAir = true;
                                    H.SolidMargin = 0.0f;     H.AirMargin = Margin;   break;
    case EVoxelTileClass::Mixed:
    default:                        H.bCanBeAllSolid = false; H.bCanBeAllAir = false;
                                    H.SolidMargin = 0.0f;     H.AirMargin = 0.0f;     break;
    }
}

/**
 * Plier l'effet d'un opérateur dans l'état. **Monotone : ne fait que tuer**, jamais ressusciter —
 * c'est la propriété de sûreté, et le pliage numérique ne l'affaiblit pas : une marge ne peut que
 * DESCENDRE, jamais remonter, en dehors d'un opérateur forçant.
 *
 * ⚠️ `MaxCarve` / `MaxFill` valent `FLT_MAX` par défaut = « amplitude inconnue ». Une borne finie
 * nulle signifie au contraire « identité prouvée » pour cette direction : l'opérateur peut être
 * géométriquement présent mais ne modifie aucun échantillon de la boîte. La soustraction
 * fait alors passer la marge très en dessous de zéro et l'hypothèse meurt, exactement comme la
 * version purement directionnelle de ce pliage. Aucun opérateur existant ne change de verdict.
 * (Arithmétique volontairement laissée en float sans garde : `0 − FLT_MAX` vaut `−FLT_MAX`,
 * `−FLT_MAX − FLT_MAX` sature à `−inf`, et `−inf > 0` est faux. Pas de NaN possible, les deux
 * termes étant de même signe.)
 */
FORCEINLINE void VF_FoldEffect(FVoxelBoxHypotheses& H, EVoxelOpEffect Effect,
                               float MaxCarve = FLT_MAX, float MaxFill = FLT_MAX)
{
    switch (Effect)
    {
    case EVoxelOpEffect::Identity:
        break;

    case EVoxelOpEffect::CarveOnly:
        // A finite zero bound is a proved identity for this direction. This matters for
        // state-aware structural guards: a passage may be geometrically present in a box while
        // its exact lattice carve factor is zero, and that must not kill an otherwise valid
        // AllSolid hypothesis merely because its margin is zero/unknown.
        if (MaxCarve == 0.0f)
        {
            break;
        }
        if (!(MaxCarve >= 0.0f))
        {
            H.bCanBeAllSolid = false;
            H.SolidMargin = -FLT_MAX;
            break;
        }
        H.SolidMargin -= MaxCarve;
        if (!(H.SolidMargin > 0.0f)) { H.bCanBeAllSolid = false; }
        break;

    case EVoxelOpEffect::FillOnly:
        if (MaxFill == 0.0f)
        {
            break;
        }
        if (!(MaxFill >= 0.0f))
        {
            H.bCanBeAllAir = false;
            H.AirMargin = -FLT_MAX;
            break;
        }
        H.AirMargin -= MaxFill;
        if (!(H.AirMargin > 0.0f)) { H.bCanBeAllAir = false; }
        break;

    case EVoxelOpEffect::Both:
    default:
        if (MaxCarve != 0.0f)
        {
            if (!(MaxCarve >= 0.0f))
            {
                H.bCanBeAllSolid = false;
                H.SolidMargin = -FLT_MAX;
            }
            else
            {
                H.SolidMargin -= MaxCarve;
                if (!(H.SolidMargin > 0.0f)) { H.bCanBeAllSolid = false; }
            }
        }
        if (MaxFill != 0.0f)
        {
            if (!(MaxFill >= 0.0f))
            {
                H.bCanBeAllAir = false;
                H.AirMargin = -FLT_MAX;
            }
            else
            {
                H.AirMargin -= MaxFill;
                if (!(H.AirMargin > 0.0f)) { H.bCanBeAllAir = false; }
            }
        }
        break;
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
                           const FBox& VoxelBox, const FVoxelOpContext& Ctx,
                           bool bWritesSdf)
{
    // Soundness bias: an unknown SDF interval is allowed to kill a skip, never to manufacture a
    // uniform tile. The interval is propagated only after this op's own effect has been folded, so
    // the next op sees exactly the SDF version that Eval would expose at this point in the stack.
    const EVoxelTileClass Forced = Op.ClassifyBox(VoxelBox, Ctx);
    if (Forced != EVoxelTileClass::Mixed)
    {
        VF_ForceHypotheses(H, Forced, Op.ForcedMarginOverBox(VoxelBox, Ctx));
        if (bWritesSdf) { Op.PropagateSdfOverBox(H.Sdf, VoxelBox, Ctx); }
        return;
    }
    VF_FoldEffect(H, Op.EffectOverBox(VoxelBox, Ctx, H),
                  Op.MaxCarveOverBox(VoxelBox, Ctx, H),
                  Op.MaxFillOverBox(VoxelBox, Ctx, H));
    if (bWritesSdf) { Op.PropagateSdfOverBox(H.Sdf, VoxelBox, Ctx); }
}

/** Backward-compatible helper for unit tests and callers with no cached declaration metadata. */
FORCEINLINE void VF_FoldOp(FVoxelBoxHypotheses& H, const IVoxelDensityOp& Op,
                           const FBox& VoxelBox, const FVoxelOpContext& Ctx)
{
    VF_FoldOp(H, Op, VoxelBox, Ctx,
              (Op.ChannelWrites() & VoxelOpChannels::Sdf) != 0);
}
