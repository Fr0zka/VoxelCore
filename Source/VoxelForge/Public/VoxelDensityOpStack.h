// VoxelDensityOpStack.h
// La PILE : un conteneur ordonné d'opérateurs, plus les fabriques d'opérateurs concrets.
// The STACK: an ordered container of operators, plus the concrete-operator factories.
//
// ⚠️ RIEN ICI N'ALIMENTE LE JEU. `UVoxelGenerator::GetDensityAt` et `ClassifyTile` ne sont pas
// touchés ; le `switch` par archétype reste le seul chemin de production. Cette pile est construite
// et exercée UNIQUEMENT par le test `VoxelForge.OpStack.MazeEquivalence`, qui la compare point par
// point à `GetMazeDensity`. Le branchement attend un build vert (OPSTACK-PLAN §4, Phase 1, point 3).
//
// NOTHING HERE FEEDS THE GAME. GetDensityAt and ClassifyTile are untouched; the archetype switch is
// still the only production path. This stack is built and exercised only by the equivalence test.
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
