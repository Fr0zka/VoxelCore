// VoxelDensityOpStack.h
// La PILE : un conteneur ordonné d'opérateurs, plus les fabriques d'opérateurs concrets.
// The STACK: an ordered container of operators, plus the concrete-operator factories.
//
// ⚠️ CECI ALIMENTE LE JEU, MAIS SEULEMENT SUR OPT-IN (depuis OPSTACK-PLAN §4, Phase 1, point 3).
// `UVoxelGenerator::GetDensityAt` construit la pile par chunk et l'évalue à la place du `switch`
// UNIQUEMENT quand `UVoxelStrateManager::UsesOperatorStackForChunk` rend true — c.-à-d. quand la
// strate a coché `bUseOperatorStack` ET que son archétype figure dans la liste des portés :
// **Maze, FlatPlain, CrystalChamber, SurfaceWorld, VerticalShafts, FloatingIslands, TunnelNetwork,
// Underwater (8 sur 8)**.
// Toute autre strate passe encore par le `switch`, inchangé.
// `ClassifyTile` est branché pour les archétypes de cave opt-in : il construit la même pile et
// plie `ClassifyBox`; les gaps et SurfaceWorld gardent leurs preuves exactes dédiées.
//
// THIS FEEDS THE GAME, BUT ONLY BEHIND AN OPT-IN. GetDensityAt builds the stack per chunk and
// evaluates it instead of the switch only when UsesOperatorStackForChunk returns true (strate ticked
// bUseOperatorStack AND its archetype is ported — all 8). ClassifyTile uses the same stack for cave
// archetypes; gaps and SurfaceWorld retain their exact hand-written proofs.
//
// ⛔ NE JAMAIS faire tourner les deux chemins dans le même monde.
// ⚠️ EN REVANCHE, LES COMPARER EST DEVENU LÉGITIME — cette ligne disait l'inverse et elle est
// périmée. `AUDIT §C10` (le résidu ~1 ULP) est CLOS depuis `FPSemantics = Precise` : les cinq tests
// d'équivalence comparent bit à bit et sont verts. Ils ne sont plus des contrôles de FIDÉLITÉ (la
// barre `§2.6.1` n'exige aucune ressemblance avec l'ancien monde) mais des oracles de
// CORRECTION DE PORTAGE — une faute de transcription reste un vrai bug, et l'ancienne fonction est
// le moyen le moins cher de l'attraper.
//
// POURQUOI CETTE FORME / WHY THIS SHAPE
// La question à laquelle la Phase 1 doit répondre n'est pas « est-ce que ça marche ? » mais
// **« est-ce que la séparation source / modifier tombe naturellement du code existant ? »**
// (OPSTACK-PLAN §4, le déclencheur d'arrêt). En portant Maze hors du chemin chaud et en le
// comparant à l'original, cette question reçoit une réponse MESURÉE plutôt qu'une opinion.

#pragma once

#include "CoreMinimal.h"
#include "VoxelDensityOp.h"
#include "VoxelStrateTypes.h"   // FMazeGenerationParams
#include "VoxelHeightOp.h"      // IVoxelBiomeField — BuildSurfaceStack takes ownership of one

class UVoxelStrateManager;

/**
 * FVoxelOpStack — une liste ordonnée d'opérateurs + le pliage de verdict de boîte.
 *
 * PROPRIÉTÉ (rôle 4) : les opérateurs STRUCTURELS sont ajoutés par `AppendStructuralPost` et
 * l'ordre spine → seal → passage → seal XY est garanti par cette fonction, pas par l'auteur. Un auteur ne
 * peut pas les omettre ni les réordonner — ce sont des invariants de monde (la descente doit rester
 * possible, les seals doivent tenir, les passages doivent percer).
 *
 * PROPRIÉTÉ (threading) : la pile est LUE par les workers. Les opérateurs concrets qui ont besoin
 * d'un cache par cellule/chunk le tiennent en `thread_local` à l'intérieur de leur `Eval`, comme le
 * fait déjà chaque fonction d'archétype. En Phase 3, quand les opérateurs deviendront des assets
 * partagés, il faudra un objet d'état PAR WORKER — noté ici pour que ça ne surprenne personne.
 *
 * THREADING: the stack is READ by workers. Concrete ops that need a per-cell/per-chunk cache keep it
 * thread_local inside Eval, exactly as every archetype function already does. Phase 3 (ops as shared
 * assets) will need a per-worker state object — flagged here so it is not a surprise.
 */
class FVoxelOpStack
{
private:
    /** Metadata is snapshotted when an op enters the stack so no declaration virtual is ever
     *  dispatched from `EvalSample`'s per-voxel loop. */
    struct FOpEntry
    {
        TUniquePtr<IVoxelDensityOp> Op;
        EVoxelOpChannelMask Reads = VoxelOpChannels::None;
        EVoxelOpChannelMask Writes = VoxelOpChannels::None;
        bool bAdditive = false;
    };

public:
    // DÉPLAÇABLE, PAS COPIABLE — et c'est la bonne sémantique, pas un contournement de compilateur :
    // une pile POSSÈDE ses opérateurs de façon unique. La copier voudrait dire cloner des opérateurs
    // polymorphes, ce qui n'a pas de sens ici (une pile n'existe qu'une fois par strate).
    //
    // ⚠️ NOTE COMPILATEUR : ne PAS remettre `VOXELFORGE_API` sur la classe. Sous MSVC, dllexport sur
    // une classe force l'instanciation de TOUS ses membres implicites, y compris l'opérateur
    // d'affectation par copie — impossible à générer pour un `TArray<FOpEntry>`, d'où
    // l'erreur C2280 « fonction supprimée ». L'export va sur la seule méthode hors-ligne.
    //
    // MOVE-ONLY, and that is the correct semantics rather than a compiler workaround: a stack
    // uniquely OWNS its operators. Do NOT put VOXELFORGE_API back on the class — under MSVC,
    // dllexport forces instantiation of every implicit member including copy-assignment, which
    // cannot be generated for a TArray<FOpEntry> (error C2280). Export the out-of-line
    // method instead.
    FVoxelOpStack() = default;
    FVoxelOpStack(FVoxelOpStack&&) = default;
    FVoxelOpStack& operator=(FVoxelOpStack&&) = default;
    FVoxelOpStack(const FVoxelOpStack&) = delete;
    FVoxelOpStack& operator=(const FVoxelOpStack&) = delete;

    void Add(TUniquePtr<IVoxelDensityOp> Op)
    {
        FOpEntry Entry;
        Entry.Op = MoveTemp(Op);
        if (Entry.Op.Get() != nullptr)
        {
            // Assembly-time metadata only. ValidateChannelOrder reuses this snapshot, and Eval
            // never asks an operator for its declaration.
            Entry.Reads     = Entry.Op->ChannelReads();
            Entry.Writes    = Entry.Op->ChannelWrites();
            Entry.bAdditive = Entry.Op->IsAdditive();
        }
        Ops.Add(MoveTemp(Entry));
    }

    int32 Num() const { return Ops.Num(); }

    /** Hoist chunk-constant work for every op. Une fois par chunk et par worker.
     *  Non-const : ça MUTE l'état par-chunk des opérateurs, et le prétendre const serait un
     *  mensonge utile qui finirait par masquer une course. */
    void PrepareChunk(const FVoxelOpContext& Ctx)
    {
        for (const FOpEntry& Entry : Ops) { Entry.Op->PrepareChunk(Ctx); }
    }

    /**
     * Évalue la pile complète en un point. Rend la densité en convention INTERNE
     * (positif = solide) — l'appelant négate UNE FOIS pour le marching cubes.
     *
     * Returns INTERNAL-convention density (positive = solid). The caller negates once for MC.
     */
    float EvalInternal(float WorldX, float WorldY, float WorldZ) const
    {
        return EvalSample(WorldX, WorldY, WorldZ).Density;
    }

    /** L'état COMPLET (densité + SDF) après toute la pile. Diagnostic : quand une comparaison
     *  avec l'ancien chemin diverge, c'est le canal SDF qui dit si l'écart naît avant ou après
     *  la conversion. / The full state after the stack — the SDF channel is what says whether a
     *  divergence is born before or after the carve. */
    FVoxelOpSample EvalSample(float WorldX, float WorldY, float WorldZ) const
    {
        FVoxelOpSample S;
        for (const FOpEntry& Entry : Ops) { Entry.Op->Eval(WorldX, WorldY, WorldZ, S); }
        return S;
    }

    /** Le même, négaté pour le mesher (négatif = solide). */
    float EvalMC(float WorldX, float WorldY, float WorldZ) const
    {
        return -EvalInternal(WorldX, WorldY, WorldZ);
    }

    /**
     * Le pliage générique qui remplacera les gardes écrites à la main dans ClassifyTile.
     * Voir `VF_FoldOp` (VoxelDensityOp.h) pour la sémantique — en particulier pourquoi un
     * opérateur FORÇANT (les seals dans leurs bandes) écrase ce que la pile avait conclu avant lui.
     */
    EVoxelTileClass ClassifyBox(const FBox& VoxelBox, const FVoxelOpContext& Ctx) const
    {
        FVoxelBoxHypotheses H;
        for (const FOpEntry& Entry : Ops)
        {
            VF_FoldOp(H, *Entry.Op, VoxelBox, Ctx,
                      (Entry.Writes & VoxelOpChannels::Sdf) != 0);
            // Do not early-out on a dead hypothesis: a later forcing structural post may
            // deliberately overwrite it (the XY edge seal is appended after passage carving).
        }
        return H.Resolve();
    }

    /**
     * LE MÊME PLIAGE, MAIS QUI DIT **QUI** A TUÉ CHAQUE HYPOTHÈSE. Diagnostic, réservé aux tests.
     *
     * ⚠️ IL DOIT RENDRE EXACTEMENT LE MÊME VERDICT QUE `ClassifyBox` — même boucle, même ordre,
     * y compris le fait de laisser un post forçant ultérieur ressusciter une hypothèse morte. Un diagnostic qui emprunte un chemin légèrement différent de celui qu'il explique
     * est pire que pas de diagnostic : il envoie chercher le bug ailleurs. Si l'un des deux change,
     * l'autre change avec lui.
     *
     * `OutSolidKiller` / `OutAirKiller` reçoivent l'INDEX du premier opérateur qui fait passer
     * l'hypothèse correspondante de vraie à fausse, ou `INDEX_NONE` si elle a survécu. Le nom
     * lisible s'obtient par `GetOpDebugName(index)`.
     *
     * Same fold, but it reports WHICH op killed each hypothesis. Must stay verdict-identical to
     * ClassifyBox, including later forcing posts that may overwrite a dead hypothesis — a
     * diagnostic that takes a slightly different path sends you hunting in the wrong place.
     */
    EVoxelTileClass ClassifyBoxAttributed(const FBox& VoxelBox, const FVoxelOpContext& Ctx,
                                          int32& OutSolidKiller, int32& OutAirKiller) const
    {
        OutSolidKiller = INDEX_NONE;
        OutAirKiller   = INDEX_NONE;

        FVoxelBoxHypotheses H;
        for (int32 i = 0; i < Ops.Num(); ++i)
        {
            const bool bSolidBefore = H.bCanBeAllSolid;
            const bool bAirBefore   = H.bCanBeAllAir;

            VF_FoldOp(H, *Ops[i].Op, VoxelBox, Ctx,
                      (Ops[i].Writes & VoxelOpChannels::Sdf) != 0);

            if (bSolidBefore && !H.bCanBeAllSolid && OutSolidKiller == INDEX_NONE) { OutSolidKiller = i; }
            if (bAirBefore   && !H.bCanBeAllAir   && OutAirKiller   == INDEX_NONE) { OutAirKiller   = i; }

        }
        return H.Resolve();
    }

    /** Nom lisible d'un opérateur, pour les rapports de test. Voir `IVoxelDensityOp::DebugName`. */
    const TCHAR* GetOpDebugName(int32 Index) const
    {
        return Ops.IsValidIndex(Index) ? Ops[Index].Op->DebugName() : TEXT("(none)");
    }

    /**
     * Validate the stack's channel dependency DAG without evaluating a voxel.
     *
     * Rule: each channel has a current version. A channel is unavailable until an earlier op
     * writes it. The one deliberate exception is a root `FieldSource` that reads and writes the
     * same channel as an identity fold (for example `min(FLT_MAX, Sdf)`); it may consume that
     * channel's `FVoxelOpSample` initial identity while publishing the first real version. An op
     * may read only the version visible on entry; a read/write op consumes that version and
     * publishes the next one. A write-only op is a replacement/producer, so it is legal only as
     * the first producer of that channel; allowing a second write-only producer would silently
     * clobber the earlier field. An additive op must read every channel it writes, because an
     * additive delta cannot introduce or replace a channel. All producer-to-consumer edges
     * therefore point forward in the list, and the list is a legal topological ordering of the
     * channel DAG. A later read deliberately sees the writer's post-op version; a consumer that
     * needs a pre-write value must be placed before that writer. The declarations do not invent
     * dependencies from names, roles, or Eval side effects.
     *
     * This is assembly/diagnostic work only. It must not be called from Eval or any voxel loop.
     * Returns false and optionally describes the first violation.
     */
    VOXELFORGE_API bool ValidateChannelOrder(FString* OutError = nullptr) const;

    /**
     * RÔLE 4 — ajoute les invariants de monde, dans l'ordre fixe, à la fin de la pile.
     * spine (0,0) → seal de frontière → carve de passage → seal de limite XY.
     *
     * ⚠️ La couche de diff (édits joueur) n'est PAS ici : elle vit dans `GetDensityAt`, APRÈS la
     * négation MC, avec les disturbances. Elle rejoindra la pile quand les disturbances seront
     * portées et que la question de convention MC-vs-interne sera tranchée pour de bon
     * (OPSTACK-DECOMPOSITION §10.2). Tant que la pile n'alimente pas le jeu, c'est sans effet.
     *
     * The diff layer is NOT here: it lives in GetDensityAt, AFTER the MC negate, with disturbances.
     * It joins the stack when disturbances are ported. Harmless while the stack feeds nothing.
     *
     * @param StrateManager  peut être nullptr → pas de carve de passage (comme le fallback actuel).
     *                        Le seal XY reste toujours présent ; son rayon/thickness viennent de Ctx
     *                        au moment de PrepareChunk (0 = monde non borné).
     */
    VOXELFORGE_API void AppendStructuralPost(float StrateTopWorldZ, float StrateBottomWorldZ,
                                             float SealThickness, float BaseDensity, float SpineRadius,
                                             const UVoxelStrateManager* StrateManager);

private:
    TArray<FOpEntry> Ops;
};

//=============================================================================
// FABRIQUES / FACTORIES
//=============================================================================

namespace VoxelDensityOps
{
    /** Rôle 1 — `Density = BaseDensity` partout. `ClassifyBox` → AllSolid, exact et gratuit.
     *  Racine de TunnelNetwork, Maze, VerticalShafts et des gaps de bedrock. */
    VOXELFORGE_API TUniquePtr<IVoxelDensityOp> MakeConstantRockSource(float BaseDensity);

    /** Rôle 1 — le MÊME opérateur au signe près : `Density = -BaseDensity`, un grand vide ouvert.
     *  `ClassifyBox` → **AllAir**, ce qu'aucune source n'avait encore su rendre — c'est ce qui rend
     *  une strate d'îles flottantes (surtout vide) sautable là où aucune île n'arrive. Racine de
     *  FloatingIslands. */
    VOXELFORGE_API TUniquePtr<IVoxelDensityOp> MakeConstantVoidSource(float BaseDensity);

    /** Rôle 1 — les couloirs de Maze : capsules sur les arêtes ouvertes d'un treillis 3D.
     *  Écrit le canal SDF uniquement. Identité d'arête = hash(nœud inférieur, axe), donc deux
     *  chunks adjacents NE PEUVENT PAS être en désaccord : pas de cache de chunk, pas de région
     *  COLLECT, zéro risque de couture (AUDIT §6.4 — le motif à préférer). */
    /** The source answers only for its own SDF field. Its box query publishes an SDF interval;
     *  each later converter or modifier consumes that interval through the generic fold. It does
     *  not know, and must not answer for, any downstream operator. */
    VOXELFORGE_API TUniquePtr<IVoxelDensityOp> MakeLatticeCorridorSource(const FMazeGenerationParams& P,
                                                                         int32 Seed);

    /** Rôle 3 — rugosité de paroi appliquée au canal SDF (variante Maze/Shafts/Islands).
     *  `Frequency` est codée en dur au site d'appel aujourd'hui (0.12 pour Maze) ; l'exposer est
     *  un gain d'authoring gratuit, et §2.6 autorise explicitement le re-tune. */
    VOXELFORGE_API TUniquePtr<IVoxelDensityOp> MakeSdfRoughnessMod(float Strength, float Frequency,
                                                                   int32 BaseOctaves, float ApplyWithin);

    /** Rôle 2 — conversion SDF → densité : creuse de l'air là où le SDF est à l'intérieur.
     *  Les six mêmes lignes apparaissent aujourd'hui dans TunnelNetwork, Maze et VerticalShafts.
     *  @param MinDivisor  plancher du diviseur `Blend·2`. **TunnelNetwork passe 1.0** (son original
     *                     écrit `FMath::Max(SDFBlendRadius·2, 1)`) ; Maze/Shafts laissent 0, où
     *                     `Max(x,0) == x` exactement. Les deux formules divergent si `Blend·2 < 1`,
     *                     donc ce paramètre est une vraie différence, pas une précaution. */
    VOXELFORGE_API TUniquePtr<IVoxelDensityOp> MakeSdfCarve(float Blend, float BaseDensity,
                                                            float MinDivisor = 0.0f);

    /** Rôle 2 — la même conversion, signe opposé : REMPLIT du solide là où le SDF est à l'intérieur.
     *  C'est ce que fait FloatingIslands (`Density += Fill·Base·2`), et la multiplication par ±1
     *  étant exacte en IEEE-754, le chemin carve reste bit pour bit ce qu'il était. */
    VOXELFORGE_API TUniquePtr<IVoxelDensityOp> MakeSdfFill(float Blend, float BaseDensity);

    /** Rôle 1 — la dalle : surface de sol + surface de plafond → champ de vide. **XY-PUR** depuis
     *  OPSTACK-DECOMPOSITION §3.1 (le terme en Z des deux bruits est parti), ce qui lui donne un
     *  `ClassifyBox` EXACT sans échantillonnage : les deux surfaces vivent dans des bandes en Z
     *  bornées par le supremum prouvé de FBM (1.5, not the nominal parameter label). Sert
     *  FlatPlain **et** CrystalChamber. */
    VOXELFORGE_API TUniquePtr<IVoxelDensityOp> MakeSlabVoidSource(const FSlabGenerationParams& P, int32 Seed);

    /** Rôle 3 — cylindres de hauteur infinie sur une grille monde. N'ajoute que du solide ⇒
     *  `FillOnly` quand une colonne atteint la boîte, `Identity` (le cas courant) sinon. */
    VOXELFORGE_API TUniquePtr<IVoxelDensityOp> MakeGridColumnMod(const FSlabGenerationParams& P, int32 Seed);

    /** Rôle 1 — le pont entre les deux espaces : consomme les piles de HAUTEUR (sol + voûte,
     *  `VoxelHeightOp.h`) et en fait une densité. `IsXYPure()` est **false** — les hauteurs sont
     *  pures en XY, la densité est une distance à celles-ci et ne peut pas l'être. */
    VOXELFORGE_API TUniquePtr<IVoxelDensityOp> MakeSurfaceColumnSource(const FSurfaceGenerationParams& P,
                                                                       int32 Seed);

    /**
     * SurfaceWorld, COMPLET : colonne (sol + voûte) → densité, overhang 3D, post structurel, et le
     * mélange de biomes quand `PerBiomeParams` est non vide.
     *
     * @param PerBiomeParams  vide ⇒ pas de biomes, chemin d'origine strictement inchangé. Non vide
     *                        ⇒ une pile de hauteur COMPLÈTE par biome, sol mélangé / voûte
     *                        sélectionnée, amplitude d'overhang interpolée (§5, combiner `Mask`).
     * @param BiomeField      **transféré** à la pile, qui le possède. Doit répondre pour les mêmes
     *                        indices que `PerBiomeParams`. `nullptr` avec des params non vides ⇒
     *                        biome 0 partout (dégradation sûre, pas un crash).
     */
    VOXELFORGE_API void BuildSurfaceStack(FVoxelOpStack& OutStack, const FSurfaceGenerationParams& P,
                                          int32 Seed, float SpineRadius,
                                          const UVoxelStrateManager* StrateManager,
                                          const TArray<FSurfaceGenerationParams>& PerBiomeParams =
                                              TArray<FSurfaceGenerationParams>(),
                                          TUniquePtr<IVoxelBiomeField> BiomeField = nullptr);

    /**
     * FlatPlain ET CrystalChamber — la même pile, **sans branchement sur le type** :
     *   SlabVoidSource → GridColumnMod → [structural post ×4]
     *
     * C'est le premier vrai gain du refactor (OPSTACK-PLAN §4) : deux des huit archétypes
     * disparaissent dans un opérateur, et leur différence redevient ce qu'elle était déjà dans
     * `GetSlabDensity` — un jeu de valeurs par défaut, pas du code.
     */
    VOXELFORGE_API void BuildSlabStack(FVoxelOpStack& OutStack, const FSlabGenerationParams& P,
                                       int32 Seed, float SpineRadius,
                                       const UVoxelStrateManager* StrateManager);

    /**
     * VerticalShafts — 9 ops, et **TROIS viennent de Maze sans une ligne de changement** :
     *   ConstantRock → ShaftField → SdfRoughness → SdfCarve → ShaftLedge → [structural post ×4]
     *
     * C'est la démonstration que `§2.5` promettait : dans le `switch`, Maze et VerticalShafts sont
     * deux fonctions de ~100 lignes sans rien de commun à l'œil ; en opérateurs, ce sont les mêmes
     * trois ops avec une source différente et d'autres réglages (fréquence 0.1 au lieu de 0.12,
     * fenêtre `rough + 4` au lieu de `R + rough + 2`).
     */
    VOXELFORGE_API void BuildVerticalShaftStack(FVoxelOpStack& OutStack, const FVerticalShaftParams& P,
                                                int32 Seed, float SpineRadius,
                                                const UVoxelStrateManager* StrateManager);

    /**
     * TunnelNetwork — **COMPLET, 20 ops** :
     *   ConstantRock → RoomGraph(warp + pits + cheminées) → SdfCarve → CaveRoughness(4b)
     *   → Terrace → LayerLines → Ribbing → Overhang → Cliff → Scallop → Arch → RoomColumn(4d)
     *   → Dome(4g) → Pinch(4h) → FloorBias → Worms → [structural ×4]
     *
     * L'override d'op PAR SALLE (étape C1) n'ajoute aucun opérateur : `FRoomGraphSource` publie
     * `LocalParams()` — les params de la strate avec l'op de la salle la plus proche appliqué — et
     * ONZE des douze modificateurs y lisent leurs champs. La rugosité (4b) lit les params de la
     * STRATE, parce que dans l'original elle précède la déclaration du shadow.
     *
     * ⚠️ `FRoomGraphSource` **APPELLE** `BuildChunkCache`/`EvaluateSDFCached`, il ne les transcrit
     * pas : c'est là que vit la discipline d'invariance de fenêtre à deux régions (`ARCHITECTURE
     * §8.4`), et en faire une copie serait le pire résultat possible pour un refactor dont le but est
     * d'avoir UNE définition de chaque idée.
     */
    VOXELFORGE_API void BuildTunnelNetworkStack(FVoxelOpStack& OutStack,
                                                const FStrateGenerationParams& P,
                                                int32 Seed, float SpineRadius,
                                                const UVoxelStrateManager* StrateManager);

    /**
     * DIAGNOSTIC — la ventilation par CLASSE DE PRIMITIVE de la dernière propagation d'intervalle
     * de `FRoomGraphSource` évaluée sur ce thread. **Tests uniquement. N'entre dans aucune décision
     * de génération.**
     *
     * ⚠️ POURQUOI ÇA EXISTE PLUTÔT QUE D'ÊTRE REFAIT DANS LE TEST. Le test a déjà tout ce qu'il faut
     * pour rejouer le critère — il appelle `BuildChunkCache` ailleurs. Le rejouer serait une
     * DEUXIÈME définition du critère, qui dériverait de la vraie et mentirait exactement le jour où
     * on la croirait. C'est la même raison qui a fait exister `VF_BuildOpStackForChunk`. On expose
     * donc ce que l'opérateur a réellement calculé.
     *
     * `Hit*` = combien de primitives de cette classe atteignent la boîte (0 partout ⇒ `Identity`).
     * `Num*` = combien le cache en contenait, ce qui distingue « aucune n'atteint » de « il n'y en
     * avait aucune » — deux zéros de sens opposé.
     *
     * Reads back what the operator actually computed, rather than letting the test re-derive the
     * criterion: a second copy would drift and would lie on the day it was believed.
     */
    struct FRoomBoxDiagnostic
    {
        int32 HitRooms = 0, HitTunnels = 0, HitPits = 0, HitChimneys = 0;
        int32 NumRooms = 0, NumTunnels = 0, NumPits = 0, NumChimneys = 0;

        /** Les mêmes comptes si la dilatation de warp valait ZÉRO, et de combien de voxels la boîte
         *  est effectivement dilatée. `Hit* - Hit*NoWarp` = la part du blocage due à MA boîte plutôt
         *  qu'à la géométrie. Cette mesure manquait, et son absence a coûté trois builds de
         *  resserrement autour du mauvais terme. */
        int32 HitRoomsNoWarp = 0, HitTunnelsNoWarp = 0;
        float WarpDilation = 0.0f;
    };

    VOXELFORGE_API FRoomBoxDiagnostic GetLastRoomBoxDiagnostic();

    /**
     * FloatingIslands — 8 ops, et **la pile tourne à l'ENVERS** :
     *   ConstantVoid → IslandBlob → SdfRoughness → SdfFill → [structural post ×4]
     *
     * Les quatre archétypes portés jusqu'ici partent de ROC et CREUSENT ; celui-ci part du VIDE et
     * REMPLIT. Aucune des deux extrémités n'a demandé d'opérateur neuf — `FConstantFieldSource` et
     * `FSdfConvertOp` sont les mêmes classes au signe près, et `FSdfRoughnessMod` est repris sans
     * une ligne de changement (4ᵉ archétype). Seul le blob d'île est nouveau.
     *
     * The stack that runs backwards: void source + fill instead of rock source + carve, using the
     * SAME operators with the opposite sign.
     */
    VOXELFORGE_API void BuildFloatingIslandStack(FVoxelOpStack& OutStack, const FFloatingIslandParams& P,
                                                 int32 Seed, float SpineRadius,
                                                 const UVoxelStrateManager* StrateManager);

    /**
     * La pile Maze complète, décomposée — PAS un `FMazeOp` monolithique :
     *   ConstantRockSource → LatticeCorridorSource → SdfRoughnessMod → SdfCarve → [structural post]
     *
     * C'est le test de la Phase 1 : si Maze ne se décompose pas ainsi, l'abstraction est mauvaise
     * pour ce domaine (OPSTACK-PLAN §4, déclencheur d'arrêt).
     */
    VOXELFORGE_API void BuildMazeStack(FVoxelOpStack& OutStack, const FMazeGenerationParams& P,
                                       int32 Seed, float SpineRadius,
                                       const UVoxelStrateManager* StrateManager);
}
