// VoxelHeightOp.h
// L'ESPACE DES HAUTEURS — une seconde famille d'opérateurs, et pourquoi elle DOIT exister.
// HEIGHT SPACE — a second operator family, and why it has to exist.
//
// ─────────────────────────────────────────────────────────────────────────────────────────
// LE CONSTAT QUI FORCE CE FICHIER
// ─────────────────────────────────────────────────────────────────────────────────────────
// `OPSTACK-DECOMPOSITION §5` décompose SurfaceWorld ainsi :
//
//     FHeightfieldSource                      ← toute la chaîne de colonne, XY-pure
//       ├─ FStructuralHeightField
//       ├─ FCliffHeightMod
//       ├─ FTerraceHeightMod
//       ├─ FLayerLineHeightMod
//       └─ FBeachHeightMod
//
// et note, sans en tirer la conséquence : *« les ops de hauteur opèrent sur des valeurs Z dans la
// colonne, pas sur la densité »*. En lisant `ComputeSurfaceTerrainZ`, c'est littéralement vrai :
// c'est une suite de blocs qui lisent et écrivent **un seul float `Terrain`**, une altitude.
//
// **Ils ne rentrent donc PAS dans `IVoxelDensityOp`.** Sa signature est
// `Eval(x, y, z, FVoxelOpSample&)` — par voxel, deux canaux densité/SDF. Un op de hauteur n'a pas
// de Z d'entrée (il en PRODUIT un), ne veut pas être appelé par voxel (il est XY-pur, une fois par
// colonne), et n'écrit ni densité ni SDF. Les forcer dans le contrat densité demanderait soit un
// troisième canal par voxel — alors que la hauteur est une propriété de COLONNE, pas de voxel —,
// soit de replier les cinq en un seul op opaque, ce que `§2.5` appelle précisément l'échec du
// refactor.
//
// **Donc : une seconde famille, dans son propre espace.** C'est la même leçon que `§0.1` (il fallait
// un canal SDF en plus de la densité), un cran plus loin : certaines choses ne sont pas un canal de
// plus, elles sont un ESPACE de plus.
//
// The height ops read and write a single float ALTITUDE. They have no input Z (they produce one),
// are XY-pure (once per column, not per voxel), and write neither density nor SDF. Forcing them into
// IVoxelDensityOp would need either a per-voxel third channel for what is a COLUMN property, or
// collapsing all five into one opaque op — which §2.5 calls the failure mode. Hence a second family.
//
// ─────────────────────────────────────────────────────────────────────────────────────────
// CE QUE ÇA ACHÈTE / WHAT IT BUYS
// ─────────────────────────────────────────────────────────────────────────────────────────
// • **Le cache de colonne T1.a tombe naturellement.** Une pile de hauteur est XY-pure PAR
//   CONSTRUCTION — il n'y a pas de Z à mettre dedans par erreur. `AUDIT §6.3` avertit qu'une donnée
//   dépendante de Z glissée dans `FSurfaceColumn` corrompt silencieusement toute la pile verticale
//   de chunks, et que `ValidateDeterminism` ne le verrait pas. Ici c'est le TYPE qui l'interdit.
// • **La composition d'idées de terrain devient de l'authoring**, comme pour la densité.
// • Les mêmes ops resserviront à VerticalShafts (ledges) et FloatingIslands.
//
// ⚠️ CE FICHIER NE TOUCHE PAS AU JEU. Il est bâti et exercé par
// `VoxelForge.OpStack.SurfaceHeightEquivalence`, qui le compare à `ComputeSurfaceTerrainZ` point par
// point. Le branchement dans le chemin densité est l'étape SUIVANTE (§5 : `FHeightfieldSource`,
// `FSkyCapSource`, `FOverhangShelfMod`), délibérément séparée pour que la question d'architecture
// — *« l'espace des hauteurs se décompose-t-il vraiment ? »* — reçoive une réponse MESURÉE avant
// qu'on écrive l'adaptateur qui en dépend.

#pragma once

#include "CoreMinimal.h"
#include "Templates/UniquePtr.h"
#include "VoxelStrateTypes.h"   // FSurfaceGenerationParams

/**
 * L'état qui traverse une pile de hauteur. DEUX canaux, exactement comme `FVoxelOpSample` — et
 * pour la même raison : le code le fait déjà.
 *
 * `Relief` (le `M` de `SampleSurfaceStructuralZ`) est PRODUIT par la source structurelle et CONSOMMÉ
 * par le gate du terrace (`TerraceStrength * M`). Sans ce second canal, le terrace devrait
 * ré-échantillonner le champ de relief — plus lent, et surtout une occasion de diverger de la valeur
 * que la source a réellement utilisée.
 *
 * Two channels, for the same reason as FVoxelOpSample: Relief (the `M` of the structural field) is
 * produced by the source and consumed by the terrace gate. Threading it beats resampling it.
 */
struct FVoxelHeightSample
{
    /** Altitude monde en VOXELS (pas cm). */
    float Height = 0.0f;

    /** « Montagnosité » [0,1]. 1 = uniforme (ReliefStrength = 0). */
    float Relief = 1.0f;
};

/**
 * Un opérateur d'espace-hauteur. Trois différences avec `IVoxelDensityOp`, toutes voulues :
 *   • pas de Z d'entrée — la pile en PRODUIT un ;
 *   • XY-pur par construction, donc pas de `IsXYPure()` à déclarer ni à oublier ;
 *   • pas de `PrepareChunk` — ces ops sont déjà appelés une fois par colonne, ce qui EST la
 *     granularité que `PrepareChunk` sert à obtenir côté densité.
 */
class IVoxelHeightOp
{
public:
    virtual ~IVoxelHeightOp() = default;

    /**
     * INVARIANCE DE FENÊTRE (ARCHITECTURE §8.4) : fonction PURE de (X, Y, seed, params). Le même XY
     * évalué depuis une autre tuile, un autre ordre, un autre thread doit rendre le float
     * BIT-IDENTIQUE — le cache de colonne T1.a est partagé sur toute la pile verticale de chunks,
     * donc une impureté ici se propage à tous les Z d'un coup.
     */
    virtual void Eval(float WorldX, float WorldY, FVoxelHeightSample& InOut) const = 0;

    /**
     * Majorant CONSERVATIF du déplacement vertical que cet op peut ajouter, en voxels.
     * Sert à borner la colonne pour un futur `ClassifyBox` exact du heightfield — la même logique
     * que les bandes de `FSlabVoidSource`, qui prouvent 36-40 tuiles sur 60.
     * Rendre trop grand coûte du CPU ; rendre trop petit serait un TROU. `FLT_MAX` = « je ne sais
     * pas », toujours sûr, et c'est le défaut.
     */
    virtual float MaxDisplacement() const { return FLT_MAX; }
};

/**
 * Pile de hauteur : source → modificateurs, dans l'ordre. Déplaçable, pas copiable, exactement
 * comme `FVoxelOpStack` et pour la même raison (elle POSSÈDE ses opérateurs).
 */
class FVoxelHeightStack
{
public:
    FVoxelHeightStack() = default;
    FVoxelHeightStack(FVoxelHeightStack&&) = default;
    FVoxelHeightStack& operator=(FVoxelHeightStack&&) = default;
    FVoxelHeightStack(const FVoxelHeightStack&) = delete;
    FVoxelHeightStack& operator=(const FVoxelHeightStack&) = delete;

    void Add(TUniquePtr<IVoxelHeightOp> Op) { Ops.Add(MoveTemp(Op)); }
    int32 Num() const { return Ops.Num(); }

    /** L'altitude après toute la pile. */
    float EvalHeight(float WorldX, float WorldY) const
    {
        return EvalSample(WorldX, WorldY).Height;
    }

    /** L'état complet (altitude + relief). */
    FVoxelHeightSample EvalSample(float WorldX, float WorldY) const
    {
        FVoxelHeightSample S;
        for (const TUniquePtr<IVoxelHeightOp>& Op : Ops) { Op->Eval(WorldX, WorldY, S); }
        return S;
    }

    /** Somme des majorants. `FLT_MAX` dès qu'un seul op ne sait pas répondre. */
    float MaxTotalDisplacement() const
    {
        float Total = 0.0f;
        for (const TUniquePtr<IVoxelHeightOp>& Op : Ops)
        {
            const float D = Op->MaxDisplacement();
            if (D >= FLT_MAX) { return FLT_MAX; }
            Total += D;
        }
        return Total;
    }

private:
    TArray<TUniquePtr<IVoxelHeightOp>> Ops;
};

//=============================================================================
// FABRIQUES / FACTORIES
//=============================================================================

namespace VoxelHeightOps
{
    /**
     * La source structurelle : continents + montagnes + détail, sous une frame de warp.
     * Produit `Height` ET `Relief`. Transcription littérale de `SampleSurfaceStructuralZ`.
     *
     * ⚠️ Rend un pointeur NON-POSSÉDANT via `OutSource` : `FCliffHeightMod` doit pouvoir
     * RÉ-ÉCHANTILLONNER ce champ (4 fois, en différences centrées) et doit le faire sur la MÊME
     * fonction, pas sur une copie qui pourrait dériver. La pile garde la propriété ; la source vit
     * donc aussi longtemps que le modificateur qui la référence, parce que le constructeur de pile
     * les ajoute ensemble et que la pile ne réordonne jamais.
     */
    VOXELFORGE_API TUniquePtr<IVoxelHeightOp> MakeStructuralHeightSource(
        const FSurfaceGenerationParams& P, int32 Seed, const IVoxelHeightOp** OutSource);

    /** Raidissement conditionné par la pente. Le seul op qui coûte des échantillons en plus
     *  (4 resamples structurels), et seulement quand il est activé. */
    VOXELFORGE_API TUniquePtr<IVoxelHeightOp> MakeCliffHeightMod(
        const FSurfaceGenerationParams& P, const IVoxelHeightOp* StructuralSource);

    /** Plateaux quantifiés, gatés par le relief (`TerraceStrength * M`) — d'où le canal Relief. */
    VOXELFORGE_API TUniquePtr<IVoxelHeightOp> MakeTerraceHeightMod(const FSurfaceGenerationParams& P);

    /** Bandes sédimentaires : `Height -= sin(Height · 2π / Spacing) · Depth`. */
    VOXELFORGE_API TUniquePtr<IVoxelHeightOp> MakeLayerLineHeightMod(const FSurfaceGenerationParams& P);

    /** Aplatissement vers la ligne d'eau dans `BeachWidth`. */
    VOXELFORGE_API TUniquePtr<IVoxelHeightOp> MakeBeachHeightMod(const FSurfaceGenerationParams& P);

    /**
     * La pile de hauteur complète de SurfaceWorld, dans l'ordre de `ComputeSurfaceTerrainZ` :
     *   structural → cliff → terrace → layer lines → beach
     *
     * L'ordre n'est PAS négociable : le terrace quantifie une hauteur que le cliff a déjà raidie,
     * les layer lines se posent sur le résultat, et la plage écrase tout près de l'eau. C'est
     * l'ordre du code d'origine, et le test échouerait bruyamment sur toute permutation.
     */
    VOXELFORGE_API void BuildSurfaceHeightStack(FVoxelHeightStack& OutStack,
                                                const FSurfaceGenerationParams& P, int32 Seed);
}
