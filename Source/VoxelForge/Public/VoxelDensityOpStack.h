// VoxelDensityOpStack.h
// La PILE : un conteneur ordonné d'opérateurs, plus les fabriques d'opérateurs concrets.
// The STACK: an ordered container of operators, plus the concrete-operator factories.
//
// ⚠️ CECI ALIMENTE LE JEU, MAIS SEULEMENT SUR OPT-IN (depuis OPSTACK-PLAN §4, Phase 1, point 3).
// `UVoxelGenerator::GetDensityAt` construit la pile par chunk et l'évalue à la place du `switch`
// UNIQUEMENT quand `UVoxelStrateManager::UsesOperatorStackForChunk` rend true — c.-à-d. quand la
// strate a coché `bUseOperatorStack` ET que son archétype figure dans la liste des portés (Maze
// seul aujourd'hui). Toute autre strate passe encore par le `switch`, inchangé.
// `ClassifyTile` n'est PAS branché : il utilise toujours ses gardes écrites à la main, pas
// `ClassifyBox`. C'est la Phase 2.
//
// THIS FEEDS THE GAME, BUT ONLY BEHIND AN OPT-IN. GetDensityAt builds the stack per chunk and
// evaluates it instead of the switch only when UsesOperatorStackForChunk returns true (strate
// ticked bUseOperatorStack AND its archetype is ported — Maze only, today). ClassifyTile is NOT
// wired: it still uses its hand-written guards rather than ClassifyBox. That is Phase 2.
//
// ⛔ NE JAMAIS faire tourner les deux chemins dans le même monde, ni les comparer pour l'égalité :
// le résidu de ~1 ULP est INHÉRENT et documenté (AUDIT-2026-07 §C10). La barre est visuelle (§2.6).
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
 * l'ordre spine → seal → passage est garanti par cette fonction, pas par l'auteur. Un auteur ne
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
public:
    // DÉPLAÇABLE, PAS COPIABLE — et c'est la bonne sémantique, pas un contournement de compilateur :
    // une pile POSSÈDE ses opérateurs de façon unique. La copier voudrait dire cloner des opérateurs
    // polymorphes, ce qui n'a pas de sens ici (une pile n'existe qu'une fois par strate).
    //
    // ⚠️ NOTE COMPILATEUR : ne PAS remettre `VOXELFORGE_API` sur la classe. Sous MSVC, dllexport sur
    // une classe force l'instanciation de TOUS ses membres implicites, y compris l'opérateur
    // d'affectation par copie — impossible à générer pour un `TArray<TUniquePtr<...>>`, d'où
    // l'erreur C2280 « fonction supprimée ». L'export va sur la seule méthode hors-ligne.
    //
    // MOVE-ONLY, and that is the correct semantics rather than a compiler workaround: a stack
    // uniquely OWNS its operators. Do NOT put VOXELFORGE_API back on the class — under MSVC,
    // dllexport forces instantiation of every implicit member including copy-assignment, which
    // cannot be generated for a TArray<TUniquePtr<...>> (error C2280). Export the out-of-line
    // method instead.
    FVoxelOpStack() = default;
    FVoxelOpStack(FVoxelOpStack&&) = default;
    FVoxelOpStack& operator=(FVoxelOpStack&&) = default;
    FVoxelOpStack(const FVoxelOpStack&) = delete;
    FVoxelOpStack& operator=(const FVoxelOpStack&) = delete;

    void Add(TUniquePtr<IVoxelDensityOp> Op) { Ops.Add(MoveTemp(Op)); }

    int32 Num() const { return Ops.Num(); }

    /** Hoist chunk-constant work for every op. Une fois par chunk et par worker.
     *  Non-const : ça MUTE l'état par-chunk des opérateurs, et le prétendre const serait un
     *  mensonge utile qui finirait par masquer une course. */
    void PrepareChunk(const FVoxelOpContext& Ctx)
    {
        for (const TUniquePtr<IVoxelDensityOp>& Op : Ops) { Op->PrepareChunk(Ctx); }
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
        for (const TUniquePtr<IVoxelDensityOp>& Op : Ops) { Op->Eval(WorldX, WorldY, WorldZ, S); }
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
     * opérateur FORÇANT (le seal dans sa bande) écrase ce que la pile avait conclu avant lui.
     */
    EVoxelTileClass ClassifyBox(const FBox& VoxelBox, const FVoxelOpContext& Ctx) const
    {
        FVoxelBoxHypotheses H;
        for (const TUniquePtr<IVoxelDensityOp>& Op : Ops)
        {
            VF_FoldOp(H, *Op, VoxelBox, Ctx);
            if (H.IsDead()) { return EVoxelTileClass::Mixed; }   // early-out : plus rien à prouver
        }
        return H.Resolve();
    }

    /**
     * RÔLE 4 — ajoute les invariants de monde, dans l'ordre fixe, à la fin de la pile.
     * spine (0,0) → seal de frontière → carve de passage.
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
     */
    VOXELFORGE_API void AppendStructuralPost(float StrateTopWorldZ, float StrateBottomWorldZ,
                                             float SealThickness, float BaseDensity, float SpineRadius,
                                             const UVoxelStrateManager* StrateManager);

private:
    TArray<TUniquePtr<IVoxelDensityOp>> Ops;
};

//=============================================================================
// FABRIQUES / FACTORIES
//=============================================================================

namespace VoxelDensityOps
{
    /** Rôle 1 — `Density = BaseDensity` partout. `ClassifyBox` → AllSolid, exact et gratuit.
     *  Racine de TunnelNetwork, Maze, VerticalShafts et des gaps de bedrock. */
    VOXELFORGE_API TUniquePtr<IVoxelDensityOp> MakeConstantRockSource(float BaseDensity);

    /** Rôle 1 — les couloirs de Maze : capsules sur les arêtes ouvertes d'un treillis 3D.
     *  Écrit le canal SDF uniquement. Identité d'arête = hash(nœud inférieur, axe), donc deux
     *  chunks adjacents NE PEUVENT PAS être en désaccord : pas de cache de chunk, pas de région
     *  COLLECT, zéro risque de couture (AUDIT §6.4 — le motif à préférer). */
    /*  `ExtraReach` = tout ce qui peut eLARGIR la portée du couloir en aval (amplitude de rugosité
     *  rayon de blend du carve). La source répond pour la paire source+conversion dans
     *  `EffectOverBox` (voir la note « SIMPLIFICATION DE PHASE 1 » dans VoxelDensityOp.h), donc elle
     *  doit connaître cette marge, sinon sa réponse `Identity` serait un MENSONGE — c'est-à-dire un
     *  trou. / The source answers for the source+conversion pair, so it must know the downstream
     *  margin: an Identity that is wrong is a hole. */
    VOXELFORGE_API TUniquePtr<IVoxelDensityOp> MakeLatticeCorridorSource(const FMazeGenerationParams& P,
                                                                         int32 Seed, float ExtraReach);

    /** Rôle 3 — rugosité de paroi appliquée au canal SDF (variante Maze/Shafts/Islands).
     *  `Frequency` est codée en dur au site d'appel aujourd'hui (0.12 pour Maze) ; l'exposer est
     *  un gain d'authoring gratuit, et §2.6 autorise explicitement le re-tune. */
    VOXELFORGE_API TUniquePtr<IVoxelDensityOp> MakeSdfRoughnessMod(float Strength, float Frequency,
                                                                   int32 BaseOctaves, float ApplyWithin);

    /** Rôle 2 — conversion SDF → densité : creuse de l'air là où le SDF est à l'intérieur.
     *  Les six mêmes lignes apparaissent aujourd'hui dans TunnelNetwork, Maze et VerticalShafts. */
    VOXELFORGE_API TUniquePtr<IVoxelDensityOp> MakeSdfCarve(float Blend, float BaseDensity);

    /** Rôle 1 — la dalle : surface de sol + surface de plafond → champ de vide. **XY-PUR** depuis
     *  OPSTACK-DECOMPOSITION §3.1 (le terme en Z des deux bruits est parti), ce qui lui donne un
     *  `ClassifyBox` EXACT sans échantillonnage : les deux surfaces vivent dans des bandes en Z
     *  bornées par le contrat [-1,1] de FBM. Sert FlatPlain **et** CrystalChamber. */
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
     *   SlabVoidSource → GridColumnMod → [structural post ×3]
     *
     * C'est le premier vrai gain du refactor (OPSTACK-PLAN §4) : deux des huit archétypes
     * disparaissent dans un opérateur, et leur différence redevient ce qu'elle était déjà dans
     * `GetSlabDensity` — un jeu de valeurs par défaut, pas du code.
     */
    VOXELFORGE_API void BuildSlabStack(FVoxelOpStack& OutStack, const FSlabGenerationParams& P,
                                       int32 Seed, float SpineRadius,
                                       const UVoxelStrateManager* StrateManager);

    /**
     * VerticalShafts — 8 ops, et **TROIS viennent de Maze sans une ligne de changement** :
     *   ConstantRock → ShaftField → SdfRoughness → SdfCarve → ShaftLedge → [structural post ×3]
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
