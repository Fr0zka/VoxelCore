// VoxelDensityOpStack.cpp
// Les opérateurs concrets : les décompositions d'archétypes + le post-traitement structurel.
// The concrete operators: the archetype decompositions + the structural post-process.
//
// Ces opérateurs alimentent le jeu derrière l'opt-in décrit dans l'en-tête de VoxelDensityOpStack.h.
//
// FIDÉLITÉ / FIDELITY
// Chaque corps ci-dessous est une transcription LITTÉRALE du bloc correspondant de
// `UVoxelGenerator::GetMazeDensity` — mêmes hashes, mêmes constantes, même ordre d'opérations
// flottantes, même convention de signe (INTERNE : positif = solide). L'objectif est
// l'égalité BIT à BIT, vérifiée par `VoxelForge.OpStack.MazeEquivalence`.
//
// `OPSTACK-PLAN §2.6` n'EXIGE pas l'identité binaire avec l'ancien système — mais Maze se
// décompose si proprement qu'on peut l'obtenir, et quand on peut l'obtenir il faut la prendre :
// une égalité binaire transforme « je crois que la décomposition est correcte » en preuve.
//
// §2.6 does not REQUIRE bit-identity with the old system — but Maze decomposes cleanly enough that
// it is achievable, and when it is achievable it should be taken: bit-equality turns "I believe the
// decomposition is right" into a proof.

#include "VoxelDensityOpStack.h"
#include "VoxelStrateComposer.h"

#include "VoxelDensityPrimitives.h"   // VF_ApplyOriginSpine / seals / PassageCarving
#include "VoxelDensityAblation.h"      // fingerprinted development-only stage measurements
#include "VoxelCaveMorphology.h"      // VoxelSDF::Capsule, VoxelHash
#include "VoxelGenerator.h"           // VoxelGenLOD::Eff
#include "VoxelHeightOp.h"            // FVoxelHeightStack — SurfaceWorld's two height stacks
#include "VoxelNoise.h"               // VoxelNoise::FBM
#include "VoxelWormField.h"
#include "VoxelStrateDefinition.h"    // TerrainOperations — le pool que BuildChunkCache tire par salle
#include "VoxelTerrainOpDefinition.h" // ApplyTo — l'override d'op PAR SALLE (étape C1)
#include "VoxelStrateManager.h"       // EvaluateModifierSDF / AnyPassageNearBox
#include "VoxelTypes.h"               // SmoothStep01, VOXEL_NOISE_SCALE
#include "VoxelStats.h"

#include <atomic>                     // l'id d'instance non recyclé du mémo de colonne
#include "HAL/CriticalSection.h"
#include "HAL/Event.h"
#include "HAL/PlatformTime.h"
#include "HAL/PlatformProcess.h"
#include "Misc/ScopeLock.h"

namespace
{
    /**
     * Concrete block thunks use the scalar body with the concrete operator type visible to the
     * compiler.  This is deliberately not a second numerical implementation: the scalar Eval is
     * still the single source of truth, but the op-major caller pays one virtual dispatch for the
     * block and the inner calls can inline/devirtualize.  Operators with a block-native kernel can
     * replace this helper later without changing the stack contract.
     */
    template<typename TOp>
    FORCEINLINE void VF_EvalBlockByScalar(const TOp& Op, const FVoxelOpBlock& Block)
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
                    Op.PrepareBlockSample(Sample);
                    Op.Eval(
                        static_cast<float>(Block.OriginVoxels.X + X * Block.Step),
                        static_cast<float>(Block.OriginVoxels.Y + Y * Block.Step),
                        static_cast<float>(Block.OriginVoxels.Z + Z * Block.Step),
                        Sample);
                }
            }
        }
    }

    /**
     * BORNE **PROUVABLE** DE `|Perlin3D|`, ET ELLE N'EST PAS 1.0.
     *
     * L'en-tête de `VoxelNoise::Perlin3D` annonce « ~[-1,1] (typiquement [-0.7,0.7]) ». Le `~`
     * est un aveu : c'est une observation, pas un théorème, et un verdict de boîte fondé sur une
     * observation est exactement le genre de trou que ce fichier passe son temps à éviter.
     *
     * Ce qui EST démontrable, en lisant `GradDot` : il rend `ru + rv` où `ru` et `rv` sont des
     * composantes de l'offset fractionnaire, donc chacune dans `[-1, 1]` ⇒ `|GradDot| ≤ 2`. La
     * valeur finale est une interpolation trilinéaire de huit `GradDot`, et une interpolation
     * convexe ne sort jamais de l'enveloppe de ses entrées ⇒ `|Perlin3D| ≤ 2`. (La vraie borne
     * de Perlin 3D est `√3/2 ≈ 0.87` ; on ne s'appuie pas dessus, elle dépend du jeu de
     * gradients.) Se tromper ici coûte une boîte de recherche un peu plus large, jamais un
     * verdict faux : plus large ⇒ SUR-ensemble de primitives ⇒ `Identity` plus rare.
     *
     * ⚠️⚠️ **CETTE CONSTANTE EST LE TERME DOMINANT DE TOUTE LA FONCTION.** À lire avant d'y
     * retoucher.
     *
     * La dilatation vaut `CaveWarpStrength · VOXEL_NOISE_SCALE · CETTE BORNE`. Avec les défauts
     * (`CaveWarpStrength = 8`, `SCALE = 1.25`), une borne de 2.0 vaut **20 voxels** — appliquée
     * des deux côtés de chaque axe d'une tuile de **10 voxels**, soit une boîte de requête de
     * 50 voxels, **125× le volume de la tuile** : `BoxHalfDiag` est alors dominé par cette
     * dilatation et non par la géométrie, et tout resserrement ailleurs ne gagne presque rien.
     *
     * ⚠️ LE RESTE DU PLUGIN EST MOINS PRUDENT : `BuildChunkCache` est appelée avec
     * `Expansion = CaveWarpStrength + 2` (ici comme dans `GetDensityWithParams`), ce qui suppose
     * `|Perlin3D| · SCALE ≤ CaveWarpStrength`, donc `|Perlin3D| ≤ 0.8`. Le code de production
     * parie déjà là-dessus. 2.0 serait 2,5× plus conservateur que l'hypothèse dont dépend déjà la
     * correction du cache.
     *
     * LA BORNE 1.5, DÉMONTRÉE (et non observée) :
     *   1. `GradDot` rend `±u ± v` où `u` et `v` sont deux composantes **distinctes** de l'offset
     *      du coin — vérifié sur les quatre branches du `switch` de hash, pas supposé.
     *   2. Pour l'axe x : les coins à `i=0` portent le poids `(1−su)` et l'offset `fx`, ceux à
     *      `i=1` le poids `su` et l'offset `1−fx`. Donc `Σ_c w_c·|dx_c| = (1−su)·fx + su·(1−fx)`,
     *      dont le maximum sur `[0,1]` vaut **0.5** (atteint en `fx = 0.5`, où `su = 0.5` ;
     *      0.302 en 0.25 comme en 0.75).
     *   3. `|Perlin| ≤ Σ_c w_c(|a_c| + |b_c|) ≤ S_x + S_y + S_z ≤ 3 × 0.5 = 1.5.`
     * (Le vrai maximum est plus bas encore — seuls DEUX axes apparaissent par coin — mais 1.5
     * est la borne qui se démontre sans analyse de cas sur les hash. `√3/2 ≈ 0.87`, la borne
     * classique de Perlin 3D, dépend du jeu de gradients : on ne s'appuie pas dessus.)
     *
     * This bound is the dominant term of the whole function: 2.0 would inflate a 10-voxel tile
     * into a 50-voxel query box (125x the volume). The rest of the plugin already assumes
     * |Perlin3D| <= 0.8 (BuildChunkCache's Expansion = CaveWarpStrength + 2). 1.5 is PROVED
     * above from GradDot's two-distinct-axes form and the per-axis weighted bound of 0.5.
     */
    static constexpr float VF_PerlinAbsBound = 1.5f;

    struct FVFInterval
    {
        float Min = 0.0f;
        float Max = 0.0f;
    };

    FORCEINLINE FVFInterval VF_AddInterval(const FVFInterval& A, const FVFInterval& B)
    {
        return {A.Min + B.Min, A.Max + B.Max};
    }

    FORCEINLINE FVFInterval VF_NegateInterval(const FVFInterval& A)
    {
        return {-A.Max, -A.Min};
    }

    FORCEINLINE FVFInterval VF_MultiplyNonNegativeInterval(
        const FVFInterval& A, const FVFInterval& B)
    {
        const float P0 = A.Min * B.Min;
        const float P1 = A.Min * B.Max;
        const float P2 = A.Max * B.Min;
        const float P3 = A.Max * B.Max;
        return {FMath::Min(FMath::Min(P0, P1), FMath::Min(P2, P3)),
                FMath::Max(FMath::Max(P0, P1), FMath::Max(P2, P3))};
    }

    /** Exact interval for GradDot on one subdivision of one Perlin cell. */
    FORCEINLINE FVFInterval VF_GradDotInterval(
        uint32 Hash, const FVFInterval& X, const FVFInterval& Y, const FVFInterval& Z)
    {
        const uint32 H = Hash & 15u;
        const FVFInterval* Axes[3] = {&X, &Y, &Z};
        const FVFInterval& U = *Axes[(H & 8u) == 0u ? 0 : 1];

        int32 VAxis = 2;
        if ((H & 12u) == 0u)
        {
            VAxis = 1;
        }
        else if ((H & 13u) == 12u)
        {
            VAxis = 0;
        }

        const FVFInterval SignedU = (H & 1u) == 0u ? U : VF_NegateInterval(U);
        const FVFInterval SignedV = (H & 2u) == 0u
            ? *Axes[VAxis] : VF_NegateInterval(*Axes[VAxis]);
        return VF_AddInterval(SignedU, SignedV);
    }

    /**
     * Conservative local envelope for the exact scalar Perlin3D implementation.
     *
     * Perlin3D is a trilinear blend of eight linear GradDot values in each integer cell.  We split
     * every intersected cell into two pieces per axis and interval-evaluate the same fade/lerp
     * formula.  The result is therefore a proof over the whole finite box, not a sample-based
     * guess.  Large/invalid boxes fall back to the already-proven global envelope.
     */
    FORCEINLINE float VF_PerlinAbsBoundOverBox(
        const FVector3f& InMin, const FVector3f& InMax)
    {
        if (!VoxelMath::IsFinite(InMin.X) || !VoxelMath::IsFinite(InMin.Y)
            || !VoxelMath::IsFinite(InMin.Z) || !VoxelMath::IsFinite(InMax.X)
            || !VoxelMath::IsFinite(InMax.Y) || !VoxelMath::IsFinite(InMax.Z)
            || InMin.X > InMax.X || InMin.Y > InMax.Y || InMin.Z > InMax.Z
            || FMath::Abs(InMin.X) > 1000000.0f || FMath::Abs(InMax.X) > 1000000.0f
            || FMath::Abs(InMin.Y) > 1000000.0f || FMath::Abs(InMax.Y) > 1000000.0f
            || FMath::Abs(InMin.Z) > 1000000.0f || FMath::Abs(InMax.Z) > 1000000.0f)
        {
            return VF_PerlinAbsBound;
        }

        const int32 X0 = FMath::FloorToInt(InMin.X);
        const int32 Y0 = FMath::FloorToInt(InMin.Y);
        const int32 Z0 = FMath::FloorToInt(InMin.Z);
        const int32 X1 = FMath::FloorToInt(InMax.X);
        const int32 Y1 = FMath::FloorToInt(InMax.Y);
        const int32 Z1 = FMath::FloorToInt(InMax.Z);
        const int64 CellCount = ((int64)X1 - X0 + 1)
                              * ((int64)Y1 - Y0 + 1)
                              * ((int64)Z1 - Z0 + 1);
        if (X1 < X0 || Y1 < Y0 || Z1 < Z0 || CellCount <= 0 || CellCount > 64)
        {
            return VF_PerlinAbsBound;
        }

        // The interval remains a proof for every subdivision size: each cell piece is evaluated
        // with interval arithmetic, so a coarser partition can only widen the envelope.  Eight
        // pieces made a 32-voxel LOD0 root pay hundreds of thousands of tiny interval operations
        // before the graph proof even started. Four pieces are still bounded, but retain enough
        // finite-domain tightness at the 4-voxel refinement tail to discharge a proof without an
        // exact lattice walk. If the bound cannot discharge the proof, the caller returns Mixed
        // and the normal mesher remains the conservative fallback.
        constexpr int32 Subdivisions = 4;
        float Bound = 0.0f;
        for (int32 CellZ = Z0; CellZ <= Z1; ++CellZ)
        for (int32 CellY = Y0; CellY <= Y1; ++CellY)
        for (int32 CellX = X0; CellX <= X1; ++CellX)
        {
            const float TX0 = FMath::Clamp(InMin.X - (float)CellX, 0.0f, 1.0f);
            const float TY0 = FMath::Clamp(InMin.Y - (float)CellY, 0.0f, 1.0f);
            const float TZ0 = FMath::Clamp(InMin.Z - (float)CellZ, 0.0f, 1.0f);
            const float TX1 = FMath::Clamp(InMax.X - (float)CellX, 0.0f, 1.0f);
            const float TY1 = FMath::Clamp(InMax.Y - (float)CellY, 0.0f, 1.0f);
            const float TZ1 = FMath::Clamp(InMax.Z - (float)CellZ, 0.0f, 1.0f);

            for (int32 SZ = 0; SZ < Subdivisions; ++SZ)
            for (int32 SY = 0; SY < Subdivisions; ++SY)
            for (int32 SX = 0; SX < Subdivisions; ++SX)
            {
                const float AX = TX0 + (TX1 - TX0) * (float)SX / Subdivisions;
                const float BX = TX0 + (TX1 - TX0) * (float)(SX + 1) / Subdivisions;
                const float AY = TY0 + (TY1 - TY0) * (float)SY / Subdivisions;
                const float BY = TY0 + (TY1 - TY0) * (float)(SY + 1) / Subdivisions;
                const float AZ = TZ0 + (TZ1 - TZ0) * (float)SZ / Subdivisions;
                const float BZ = TZ0 + (TZ1 - TZ0) * (float)(SZ + 1) / Subdivisions;

                const float FX0 = VoxelNoise::Detail::Fade(AX);
                const float FX1 = VoxelNoise::Detail::Fade(BX);
                const float FY0 = VoxelNoise::Detail::Fade(AY);
                const float FY1 = VoxelNoise::Detail::Fade(BY);
                const float FZ0 = VoxelNoise::Detail::Fade(AZ);
                const float FZ1 = VoxelNoise::Detail::Fade(BZ);
                const float WXMin[2] = {1.0f - FX1, FX0};
                const float WXMax[2] = {1.0f - FX0, FX1};
                const float WYMin[2] = {1.0f - FY1, FY0};
                const float WYMax[2] = {1.0f - FY0, FY1};
                const float WZMin[2] = {1.0f - FZ1, FZ0};
                const float WZMax[2] = {1.0f - FZ0, FZ1};

                float ValueMin = 0.0f;
                float ValueMax = 0.0f;
                for (int32 CZ = 0; CZ < 2; ++CZ)
                for (int32 CY = 0; CY < 2; ++CY)
                for (int32 CX = 0; CX < 2; ++CX)
                {
                    const FVFInterval Grad = VF_GradDotInterval(
                        VoxelNoise::Detail::HashCorner(CellX + CX, CellY + CY, CellZ + CZ),
                        {AX - (float)CX, BX - (float)CX},
                        {AY - (float)CY, BY - (float)CY},
                        {AZ - (float)CZ, BZ - (float)CZ});
                    const FVFInterval Weight = VF_MultiplyNonNegativeInterval(
                        {WXMin[CX], WXMax[CX]},
                        VF_MultiplyNonNegativeInterval(
                            {WYMin[CY], WYMax[CY]}, {WZMin[CZ], WZMax[CZ]}));
                    const FVFInterval Contribution = VF_MultiplyNonNegativeInterval(Weight, Grad);
                    ValueMin += Contribution.Min;
                    ValueMax += Contribution.Max;
                }

                if (!VoxelMath::IsFinite(ValueMin) || !VoxelMath::IsFinite(ValueMax))
                {
                    return VF_PerlinAbsBound;
                }
                Bound = FMath::Max(Bound, FMath::Max(FMath::Abs(ValueMin), FMath::Abs(ValueMax)));
            }
        }

        // Cover the finite-precision rounding in the nested float lerps.  If the interval proof is
        // looser than the global theorem, retaining the theorem is both cheaper and safer.
        Bound += 0.002f;
        return FMath::Min(FMath::Max(Bound, 0.0f), VF_PerlinAbsBound);
    }

    FORCEINLINE float VF_PerlinAbsBoundOverWorldBox(
        float WorldMinX, float WorldMaxX, float WorldMinY, float WorldMaxY,
        float EffectiveMinZ, float EffectiveMaxZ, float Frequency,
        float OffsetX, float OffsetY, float OffsetZ)
    {
        const float X0 = WorldMinX * Frequency + OffsetX;
        const float X1 = WorldMaxX * Frequency + OffsetX;
        const float Y0 = WorldMinY * Frequency + OffsetY;
        const float Y1 = WorldMaxY * Frequency + OffsetY;
        const float Z0 = EffectiveMinZ * Frequency + OffsetZ;
        const float Z1 = EffectiveMaxZ * Frequency + OffsetZ;
        return VF_PerlinAbsBoundOverBox(
            FVector3f(FMath::Min(X0, X1), FMath::Min(Y0, Y1), FMath::Min(Z0, Z1)),
            FVector3f(FMath::Max(X0, X1), FMath::Max(Y0, Y1), FMath::Max(Z0, Z1)));
    }

    // Cellular3D returns 2*(F2-F1)-1 without a clamp. Every searched feature point is at most
    // sqrt(12) from the query's unit cell, so F2-F1 <= sqrt(12) and the positive supremum is
    // 2*sqrt(12)-1 = 5.9282... . Round that up for float evaluation instead of trusting the
    // "~[-1,1]" comment in the noise implementation (which is an observation, not a proof).
    static constexpr float VF_CellularAbsBound = 6.0f;

    /** La même enveloppe que `FractalNoise3D` de VoxelGenerator.cpp (qui y est `static`, donc
     *  invisible ici). Les coordonnées de bruit restent en float : FVector est double dans UE5
     *  et son aller-retour ne servait qu'à conserver l'ancien arrondi, exigence abandonnée. */
    FORCEINLINE float HFractal3D(const FVector3f& Position, int32 Octaves = 4,
                                 float Lacunarity = 2.0f, float Persistence = 0.5f)
    {
        return VoxelNoise::FBM(Position.X, Position.Y, Position.Z,
                               Octaves, Lacunarity, Persistence);
    }

    /** Idem pour `RidgedNoise3D` (également `static` dans VoxelGenerator.cpp). */
    FORCEINLINE float HRidged3D(const FVector3f& Position, int32 Octaves = 4,
                                float Lacunarity = 2.0f, float Persistence = 0.5f)
    {
        return VoxelNoise::Ridged(Position.X, Position.Y, Position.Z,
                                  Octaves, Lacunarity, Persistence);
    }

    //=========================================================================
    // LE GATE `bNearCaveSurface` — ÉTAPE B
    //=========================================================================
    // ⚠️ DÉCISION DE L'ÉTAPE B5, ÉCRITE ICI PARCE QUE C'EST LE POINT OÙ ELLE SE LIT.
    // Dans l'original, les douze modificateurs de détail vivent dans UN SEUL `if (bNearCaveSurface)`.
    // Deux façons de porter ça : (a) un opérateur « conteneur » qui enveloppe ses enfants, (b) le
    // même early-out répété dans chaque opérateur. **C'est (b), délibérément :**
    //
    //   • La pile est une LISTE PLATE, et `FVoxelOpStack::ClassifyBox` plie les opérateurs un par un.
    //     Un conteneur devrait replier ses enfants lui-même — donc reproduire `VF_FoldOp` — et ses
    //     enfants deviendraient invisibles au pliage. On paierait une abstraction pour en casser une.
    //   • Un opérateur qui n'existe QUE dans un conteneur n'est pas composable, donc pas transposable
    //     en asset (Phase 3). Le motif « chaque op teste son propre gate » est déjà celui de
    //     `FSdfRoughnessMod` (`InOut.Sdf >= ApplyWithin`) et de `FShaftLedgeMod`.
    //   • Le gate n'est de toute façon PAS uniforme : chaque modificateur a EN PLUS sa propre fenêtre
    //     (`RoughnessDepth`, `TerraceRange`, `LineRange`…). Le gate partagé n'est qu'un early-out
    //     commun, pas la condition réelle de chacun.
    //
    // ⚠️ CE QUE ÇA COÛTE, dit franchement : l'original teste UNE fois et saute les douze ; la pile
    // teste douze fois. Douze comparaisons flottantes parfaitement prédites par voxel de roc profond
    // — mesurable, mais c'est exactement le genre de chose que `AUDIT §C10` dit de MESURER avant
    // d'optimiser. C'est un poste de perf connu, pas à « corriger » à l'aveugle.
    //
    // STAGE B5 DECISION: repeated early-out in each op, NOT a scoping container — the stack is a flat
    // list that ClassifyBox folds op by op, and an op that only exists inside a container is not
    // composable. Cost stated honestly: twelve predictable compares instead of one branch.
    FORCEINLINE bool VF_NearCaveSurface(float Sdf, float SDFBlendRadius)
    {
        // Transcrit tel quel, ordre des comparaisons compris :
        //   const float DetailThreshold = Params.SDFBlendRadius * 3.0f;
        //   const bool bNearCaveSurface = (CaveSDF < DetailThreshold) && (CaveSDF < FLT_MAX);
        const float DetailThreshold = SDFBlendRadius * 3.0f;
        return (Sdf < DetailThreshold) && (Sdf < FLT_MAX);
    }

    /** Lower bound for the distance between two axis-aligned boxes. The segment's AABB contains
     * the segment, so this is a conservative lower bound for point-to-segment distance. */
    FORCEINLINE float VF_DistanceBetweenBoxes(const FVector& AMin, const FVector& AMax,
                                               const FVector& BMin, const FVector& BMax)
    {
        const float DX = FMath::Max3((float)(AMin.X - BMax.X), (float)(BMin.X - AMax.X), 0.0f);
        const float DY = FMath::Max3((float)(AMin.Y - BMax.Y), (float)(BMin.Y - AMax.Y), 0.0f);
        const float DZ = FMath::Max3((float)(AMin.Z - BMax.Z), (float)(BMin.Z - AMax.Z), 0.0f);
        return FMath::Sqrt(DX * DX + DY * DY + DZ * DZ);
    }

    FORCEINLINE float VF_DistanceBoxToPointXY(const FBox& Box, float X, float Y)
    {
        const float DX = FMath::Max3((float)(Box.Min.X - X), 0.0f, (float)(X - Box.Max.X));
        const float DY = FMath::Max3((float)(Box.Min.Y - Y), 0.0f, (float)(Y - Box.Max.Y));
        return FMath::Sqrt(DX * DX + DY * DY);
    }

    /** Distance from the nearest exact lattice coordinate in one axis to a point. The continuous
     * box proof uses the interval [BoxMin, BoxMax]; the lattice proof uses only
     * Origin + integer * Step values inside that interval. Extra is a per-axis displacement
     * envelope (the cave warp); subtracting it keeps the result conservative. */
    FORCEINLINE float VF_LatticeAxisDistanceToPoint(
        float BoxMin, float BoxMax, float Origin, float Step, float Target, float Extra)
    {
        if (!(Step > 0.0f) || !VoxelMath::IsFinite(BoxMin) || !VoxelMath::IsFinite(BoxMax)
            || !VoxelMath::IsFinite(Origin) || !VoxelMath::IsFinite(Target)
            || BoxMin > BoxMax)
        {
            return FLT_MAX;
        }

        const int32 GMin = FMath::CeilToInt((BoxMin - Origin) / Step - 1.0e-4f);
        const int32 GMax = FMath::FloorToInt((BoxMax - Origin) / Step + 1.0e-4f);
        if (GMin > GMax) { return FLT_MAX; }

        const int32 Near = FMath::FloorToInt((Target - Origin) / Step);
        float Distance = FLT_MAX;
        auto Consider = [&](int32 G)
        {
            G = FMath::Clamp(G, GMin, GMax);
            Distance = FMath::Min(Distance,
                FMath::Abs((Origin + static_cast<float>(G) * Step) - Target));
        };
        Consider(GMin);
        Consider(GMax);
        Consider(Near);
        Consider(Near + 1);
        return FMath::Max(0.0f, Distance - FMath::Max(Extra, 0.0f));
    }

    /** Same discrete distance, but to an axis-aligned interval. */
    FORCEINLINE float VF_LatticeAxisDistanceToInterval(
        float BoxMin, float BoxMax, float Origin, float Step,
        float TargetMin, float TargetMax, float Extra)
    {
        if (!(Step > 0.0f) || !VoxelMath::IsFinite(TargetMin) || !VoxelMath::IsFinite(TargetMax)
            || TargetMin > TargetMax)
        {
            return FLT_MAX;
        }

        const int32 GMin = FMath::CeilToInt((BoxMin - Origin) / Step - 1.0e-4f);
        const int32 GMax = FMath::FloorToInt((BoxMax - Origin) / Step + 1.0e-4f);
        if (GMin > GMax) { return FLT_MAX; }

        const int32 NearMin = FMath::FloorToInt((TargetMin - Origin) / Step);
        const int32 NearMax = FMath::FloorToInt((TargetMax - Origin) / Step);
        float Distance = FLT_MAX;
        auto Consider = [&](int32 G)
        {
            G = FMath::Clamp(G, GMin, GMax);
            const float Value = Origin + static_cast<float>(G) * Step;
            const float Delta = Value < TargetMin ? TargetMin - Value
                              : Value > TargetMax ? Value - TargetMax : 0.0f;
            Distance = FMath::Min(Distance, Delta);
        };
        Consider(GMin);
        Consider(GMax);
        Consider(NearMin);
        Consider(NearMin + 1);
        Consider(NearMax);
        Consider(NearMax + 1);
        return FMath::Max(0.0f, Distance - FMath::Max(Extra, 0.0f));
    }

    FORCEINLINE float VF_SaturatingAdd(float A, float B)
    {
        if (!VoxelMath::IsFinite(A) || !VoxelMath::IsFinite(B)) { return FLT_MAX; }
        if (B > 0.0f && A > FLT_MAX - B) { return FLT_MAX; }
        if (B < 0.0f && A < -FLT_MAX - B) { return -FLT_MAX; }
        return A + B;
    }

    //=========================================================================
    // RÔLE 1 — SOURCE : CHAMP CONSTANT / CONSTANT FIELD  (roc ET vide)
    //=========================================================================
    // `float Density = Params.BaseDensity;  // start solid` — la première ligne de TunnelNetwork,
    // de Maze ET de VerticalShafts. Et `float Density = -Params.BaseDensity;  // open air (void)` —
    // la première ligne de FloatingIslands. **C'est le MÊME opérateur au signe près**, et le signe
    // n'est pas un détail : il décide du verdict de boîte de départ (AllSolid contre AllAir), donc
    // de ce que la strate saura sauter.
    //
    // Quatre archétypes, une ligne, un opérateur. Deux fabriques (`MakeConstantRockSource` /
    // `MakeConstantVoidSource`) parce que « roc » et « vide » sont ce que l'auteur veut DIRE ; la
    // classe, elle, n'a aucune raison d'exister en deux exemplaires.
    //
    // One operator, two factories: rock and void are the same constant field with opposite signs,
    // and the sign is what decides the starting box verdict (AllSolid vs AllAir).
    class FConstantFieldSource final : public IVoxelDensityOp
    {
    public:
        explicit FConstantFieldSource(float InValue) : Value(InValue) {}

        EVoxelOpRole GetRole() const override { return EVoxelOpRole::FieldSource; }
        EVoxelOpChannelMask ChannelReads() const override { return VoxelOpChannels::None; }
        EVoxelOpChannelMask ChannelWrites() const override { return VoxelOpChannels::Density; }
        bool IsAdditive() const override { return false; }
        void PrepareChunk(const FVoxelOpContext&) override {}
        bool IsXYPure() const override { return true; }   // constant ⇒ trivialement sans Z

        void Eval(float, float, float, FVoxelOpSample& InOut) const override
        {
            InOut.Density = Value;   // Replace : racine de pile, ignore l'entrée
        }

        // Exact et gratuit, dans les DEUX sens (convention interne : positif = solide).
        // Positif ⇒ AllSolid : c'est ce qui donne aux strates de grotte une hypothèse de départ
        // qu'elles n'ont jamais eue. Négatif ⇒ AllAir : c'est ce qui rend une strate d'îles
        // flottantes — un grand vide surtout vide — sautable là où aucune île n'arrive.
        EVoxelTileClass ClassifyBox(const FBox&, const FVoxelOpContext&) const override
        {
            if (Value > 0.0f) { return EVoxelTileClass::AllSolid; }
            if (Value < 0.0f) { return EVoxelTileClass::AllAir; }
            return EVoxelTileClass::Mixed;   // exactement 0 : le mesher le compte du côté AIR,
                                             // mais un champ nul n'est pas une hypothèse utile.
        }

        EVoxelOpEffect EffectOverBox(const FBox&, const FVoxelOpContext&) const override
        {
            return EVoxelOpEffect::Both;   // jamais atteint sauf Value == 0 : ClassifyBox répond avant
        }

        /** ⚠️ LA MOITIÉ MANQUANTE DU PLIAGE NUMÉRIQUE (`OPSTACK-DECOMPOSITION §0.2`).
         *  `MaxCarveOverBox` dit ce qu'un opérateur peut RETIRER ; ceci dit ce qu'il y avait à
         *  retirer. Un champ constant est le seul opérateur du plugin qui connaisse cette marge
         *  EXACTEMENT : la densité vaut `Value` partout, donc la marge est `|Value|`. Sans elle la
         *  soustraction n'a pas de premier terme et tout carve borné tue quand même l'hypothèse. */
        float ForcedMarginOverBox(const FBox&, const FVoxelOpContext&) const override
        {
            return FMath::Abs(Value);
        }

        const TCHAR* DebugName() const override { return TEXT("ConstantFieldSource"); }

        void EvalBlock(const FVoxelOpBlock& Block) const override
        {
            VF_EvalBlockByScalar(*this, Block);
        }

    private:
        float Value;
    };

    //=======================================================================
    // TIER 4b — GENERIC ROLLED SOURCE/MODIFIER PRIMITIVES
    //=======================================================================
    // These two small primitives keep the non-room channel space large enough for the promised
    // k=4..8 draw.  They are deliberately bounded: their box contracts are amplitude proofs, not
    // guesses.  They do not carry any room/shaft/surface pointer, so the resource graph correctly
    // treats them as legal after every SDF shape source.
    class FNoiseRibbonSource final : public IVoxelDensityOp
    {
    public:
        FNoiseRibbonSource(const FMazeGenerationParams& InP, int32 InSeed)
            : CellSize(FMath::Max(InP.CellSize, 8.0f))
            , CorridorRadius(FMath::Max(InP.CorridorRadius, 0.5f))
            , SeedU(static_cast<uint32>(InSeed) ^ 0x5249626Eu) {}

        EVoxelOpRole GetRole() const override { return EVoxelOpRole::FieldSource; }
        EVoxelOpChannelMask ChannelReads() const override { return VoxelOpChannels::None; }
        EVoxelOpChannelMask ChannelWrites() const override { return VoxelOpChannels::Sdf; }
        bool IsAdditive() const override { return false; }
        void PrepareChunk(const FVoxelOpContext&) override {}

        void Eval(float WorldX, float WorldY, float WorldZ, FVoxelOpSample& InOut) const override
        {
            const float Frequency = 1.0f / CellSize;
            const FVector3f NoisePos(
                WorldX * Frequency + VoxelHash::SeedOffset(SeedU, 3.17f),
                WorldY * Frequency + VoxelHash::SeedOffset(SeedU, 7.31f),
                WorldZ * Frequency + VoxelHash::SeedOffset(SeedU, 11.47f));
            const float Ribbon = FMath::Clamp(
                FMath::Abs(VoxelNoise::FBM(NoisePos.X, NoisePos.Y, NoisePos.Z,
                                           VoxelGenLOD::Eff(3), 2.0f, 0.5f))
                    * VOXEL_NOISE_SCALE,
                0.0f, 2.0f);
            InOut.Sdf = Ribbon * CellSize - CorridorRadius;
        }

        EVoxelOpEffect EffectOverBox(const FBox&, const FVoxelOpContext&) const override
        {
            return EVoxelOpEffect::Identity;
        }

        void PropagateSdfOverBox(FVoxelBoxSdfInterval& InOut, const FBox&,
                                 const FVoxelOpContext&) const override
        {
            if (!VoxelMath::IsFinite(CellSize) || !VoxelMath::IsFinite(CorridorRadius)
                || CellSize <= 0.0f || CorridorRadius < 0.0f)
            {
                InOut.SetUnknown();
                return;
            }
            // Eval clamps the FBM envelope to [0,2].  This interval is exact enough for safety
            // and independent of the queried box, so it never claims a false empty region.
            InOut.Set(-CorridorRadius, 2.0f * CellSize - CorridorRadius);
        }

        const TCHAR* DebugName() const override { return TEXT("NoiseRibbonSource"); }

    private:
        float CellSize;
        float CorridorRadius;
        uint32 SeedU;
    };

    class FDensityNoiseMod final : public IVoxelDensityOp
    {
    public:
        FDensityNoiseMod(float InStrength, float InFrequency, int32 InOctaves,
                         int32 InSeed, bool bInFill)
            : Strength(FMath::Max(InStrength, 0.0f))
            , Frequency(FMath::Max(InFrequency, 0.0001f))
            , Octaves(FMath::Clamp(InOctaves, 1, 8))
            , SeedU(static_cast<uint32>(InSeed) ^ (bInFill ? 0x46696C6Cu : 0x43617276u))
            , bFill(bInFill) {}

        EVoxelOpRole GetRole() const override { return EVoxelOpRole::DetailModifier; }
        EVoxelOpChannelMask ChannelReads() const override { return VoxelOpChannels::Density; }
        EVoxelOpChannelMask ChannelWrites() const override { return VoxelOpChannels::Density; }
        bool IsAdditive() const override { return true; }
        void PrepareChunk(const FVoxelOpContext&) override {}
        void Eval(float WorldX, float WorldY, float WorldZ, FVoxelOpSample& InOut) const override
        {
            if (!(Strength > 0.0f)) { return; }
            const FVector3f NoisePos(
                WorldX * Frequency + VoxelHash::SeedOffset(SeedU, 13.2f),
                WorldY * Frequency + VoxelHash::SeedOffset(SeedU, 17.8f),
                WorldZ * Frequency + VoxelHash::SeedOffset(SeedU, 23.4f));
            const float Noise01 = FMath::Clamp(
                VoxelNoise::FBM(NoisePos.X, NoisePos.Y, NoisePos.Z,
                                VoxelGenLOD::Eff(Octaves), 2.0f, 0.5f) * 0.5f + 0.5f,
                0.0f, 1.0f);
            const float Delta = Strength * Noise01;
            InOut.Density += bFill ? Delta : -Delta;
        }

        EVoxelOpEffect EffectOverBox(const FBox&, const FVoxelOpContext&) const override
        {
            if (!(Strength > 0.0f)) { return EVoxelOpEffect::Identity; }
            return bFill ? EVoxelOpEffect::FillOnly : EVoxelOpEffect::CarveOnly;
        }

        float MaxCarveOverBox(const FBox&, const FVoxelOpContext&) const override
        {
            return bFill ? 0.0f : Strength;
        }

        float MaxFillOverBox(const FBox&, const FVoxelOpContext&) const override
        {
            return bFill ? Strength : 0.0f;
        }

        const TCHAR* DebugName() const override
        {
            return bFill ? TEXT("DensityNoiseFillMod") : TEXT("DensityNoiseCarveMod");
        }

    private:
        float Strength;
        float Frequency;
        int32 Octaves;
        uint32 SeedU;
        bool bFill;
    };

    //=========================================================================
    // RÔLE 1 — SOURCE : COULOIRS SUR TREILLIS 3D / 3D LATTICE CORRIDORS
    //=========================================================================
    // Chaque nœud du treillis est au centre d'une cellule. Il choisit exactement un parent parmi
    // les axes qui le rapprochent de l'origine ; l'arête parent abaisse donc |X|+|Y|+|Z| de un.
    // Le résultat est un arbre couvrant de tout le treillis infini. Quelques arêtes supplémentaires
    // sont hashées comme des boucles, avec des probabilités plafonnées qui ne participent jamais à
    // la connectivité. Le couloir est une capsule.
    //
    // La règle est locale et seam-safe : les huit nœuds-enfants du halo {-1,0}³ suffisent à émettre
    // toutes les arêtes qui peuvent toucher la cellule évaluée. Chaque décision est une fonction
    // pure du nœud, de l'axe et de la seed ; il n'y a ni collect global ni dépendance à la fenêtre
    // de chunk.
    class FLatticeCorridorSource final : public IVoxelDensityOp
    {
    public:
        FLatticeCorridorSource(const FMazeGenerationParams& P, int32 Seed)
            : CellSize(FMath::Max(P.CellSize, 1.0f))
            , CorridorRadius(FMath::Max(P.CorridorRadius, 0.5f))
            , BranchProbability(P.BranchProbability)
            , Verticality(P.Verticality)
            , Salt((uint32)Seed ^ 0x4D617A65u)   // 'Maze' — identique à GetMazeDensity
        {}

        EVoxelOpRole GetRole() const override { return EVoxelOpRole::FieldSource; }
        EVoxelOpChannelMask ChannelReads() const override { return VoxelOpChannels::Sdf; }
        EVoxelOpChannelMask ChannelWrites() const override { return VoxelOpChannels::Sdf; }
        bool IsAdditive() const override { return false; }
        void PrepareChunk(const FVoxelOpContext&) override {}

        void Eval(float WorldX, float WorldY, float WorldZ, FVoxelOpSample& InOut) const override
        {
            const int32 CX = FMath::FloorToInt(WorldX / CellSize);
            const int32 CY = FMath::FloorToInt(WorldY / CellSize);
            const int32 CZ = FMath::FloorToInt(WorldZ / CellSize);

            const TArray<FEdge, TInlineAllocator<24>>& Edges = GetCellEdges(FIntVector(CX, CY, CZ));

            const FVector Pos(WorldX, WorldY, WorldZ);
            float Sdf = FLT_MAX;
            for (const FEdge& E : Edges)
            {
                Sdf = FMath::Min(Sdf, VoxelSDF::Capsule(Pos, E.A, E.B, CorridorRadius));
            }
            // Union de formes ⇒ MIN sur le canal SDF (voir la note de signe dans VoxelDensityOp.h :
            // « min » ici veut dire l'inverse de ce qu'il veut dire sur le canal densité).
            InOut.Sdf = FMath::Min(InOut.Sdf, Sdf);
        }

        // This source writes SDF only. The following converter decides whether that SDF carves;
        // the source itself has no density effect in isolation.
        EVoxelOpEffect EffectOverBox(const FBox&, const FVoxelOpContext&) const override
        {
            return EVoxelOpEffect::Identity;
        }

        /** Exact cell coverage plus a geometric lower bound for every open edge. The upper bound
         * is FLT_MAX because a far point can retain an arbitrarily positive capsule distance. */
        void PropagateSdfOverBox(FVoxelBoxSdfInterval& InOut, const FBox& VoxelBox,
                                 const FVoxelOpContext&) const override
        {
            const int32 CX0 = FMath::FloorToInt((float)VoxelBox.Min.X / CellSize);
            const int32 CY0 = FMath::FloorToInt((float)VoxelBox.Min.Y / CellSize);
            const int32 CZ0 = FMath::FloorToInt((float)VoxelBox.Min.Z / CellSize);
            const int32 CX1 = FMath::FloorToInt((float)VoxelBox.Max.X / CellSize);
            const int32 CY1 = FMath::FloorToInt((float)VoxelBox.Max.Y / CellSize);
            const int32 CZ1 = FMath::FloorToInt((float)VoxelBox.Max.Z / CellSize);

            constexpr int64 MaxCellsScanned = 64 * 64 * 64;
            const int64 CellCount = (int64)(CX1 - CX0 + 1) * (CY1 - CY0 + 1) * (CZ1 - CZ0 + 1);
            if (CellCount <= 0 || CellCount > MaxCellsScanned)
            {
                InOut.SetUnknown();
                return;
            }

            float Lower = FLT_MAX;
            bool bAnyEdge = false;
            for (int32 cz = CZ0; cz <= CZ1; ++cz)
            for (int32 cy = CY0; cy <= CY1; ++cy)
            for (int32 cx = CX0; cx <= CX1; ++cx)
            {
                const TArray<FEdge, TInlineAllocator<24>>& Edges =
                    GetCellEdges(FIntVector(cx, cy, cz));
                for (const FEdge& E : Edges)
                {
                    bAnyEdge = true;
                    const FVector SegmentMin(
                        FMath::Min(E.A.X, E.B.X), FMath::Min(E.A.Y, E.B.Y), FMath::Min(E.A.Z, E.B.Z));
                    const FVector SegmentMax(
                        FMath::Max(E.A.X, E.B.X), FMath::Max(E.A.Y, E.B.Y), FMath::Max(E.A.Z, E.B.Z));
                    Lower = FMath::Min(Lower,
                        VF_DistanceBetweenBoxes(VoxelBox.Min, VoxelBox.Max,
                                                 SegmentMin, SegmentMax) - CorridorRadius);
                }
            }

            FVoxelBoxSdfInterval Own;
            if (!bAnyEdge) { Own.Set(FLT_MAX, FLT_MAX); }
            else            { Own.Set(Lower, FLT_MAX); }
            InOut.MinWith(Own);
        }

    private:
        struct FEdge { FVector A, B; };

        FVector NodeCenter(int32 X, int32 Y, int32 Z) const
        {
            return FVector((X + 0.5f) * CellSize, (Y + 0.5f) * CellSize, (Z + 0.5f) * CellSize);
        }

        /**
         * Le cache par CELLULE, partagé avec le même contrat que GetMazeDensity. Il est `thread_local` et non
         * membre parce que la pile est PARTAGÉE entre workers en lecture — un membre mutable serait
         * une course. La reconstruction seule inspecte le petit voisinage ; Eval ne refait aucune
         * décision de parent ou de boucle.
         *
         * ⚠️ PHASE 3 : quand les opérateurs deviendront des assets partagés, il faudra un objet
         * d'état PAR WORKER plutôt que ce `thread_local` (qui est global à la fonction, donc partagé
         * entre DEUX piles Maze différentes sur le même thread — la clé le rattrape, mais au prix
         * d'un rebuild à chaque alternance).
         */
        const TArray<FEdge, TInlineAllocator<24>>& GetCellEdges(const FIntVector& Cell) const
        {
            thread_local TArray<FEdge, TInlineAllocator<24>> MZ_Edges;
            thread_local FIntVector MZ_Cell(INT32_MAX, INT32_MAX, INT32_MAX);
            thread_local uint32 MZ_Seed = 0xFFFFFFFFu;
            thread_local float  MZ_CS = -1.0f, MZ_Branch = -1.0f, MZ_Vert = -1.0f;

            if (Cell != MZ_Cell || Salt != MZ_Seed || CellSize != MZ_CS ||
                BranchProbability != MZ_Branch || Verticality != MZ_Vert)
            {
                MZ_Cell = Cell;  MZ_Seed = Salt;  MZ_CS = CellSize;
                MZ_Branch = BranchProbability;  MZ_Vert = Verticality;
                MZ_Edges.Reset();

                // Canonical lower-node edges in {-1,0} per axis cover every corridor that can
                // reach this voxel's cell. IsOpenEdge checks both endpoints, including a +1
                // node's parent choice, without needing a wider cache window.
                for (int32 dz = -1; dz <= 0; dz++)
                for (int32 dy = -1; dy <= 0; dy++)
                for (int32 dx = -1; dx <= 0; dx++)
                {
                    const int32 nx = Cell.X + dx, ny = Cell.Y + dy, nz = Cell.Z + dz;
                    const FVector A = NodeCenter(nx, ny, nz);

                    if (VoxelMazeTopology::IsOpenEdge(
                            nx, ny, nz, VoxelMazeTopology::EAxis::X,
                            Salt, BranchProbability, Verticality))
                    {
                        MZ_Edges.Add({ A, NodeCenter(nx + 1, ny, nz) });
                    }
                    if (VoxelMazeTopology::IsOpenEdge(
                            nx, ny, nz, VoxelMazeTopology::EAxis::Y,
                            Salt, BranchProbability, Verticality))
                    {
                        MZ_Edges.Add({ A, NodeCenter(nx, ny + 1, nz) });
                    }
                    if (VoxelMazeTopology::IsOpenEdge(
                            nx, ny, nz, VoxelMazeTopology::EAxis::Z,
                            Salt, BranchProbability, Verticality))
                    {
                        MZ_Edges.Add({ A, NodeCenter(nx, ny, nz + 1) });
                    }
                }
            }
            return MZ_Edges;
        }

        float  CellSize, CorridorRadius, BranchProbability, Verticality;
        uint32 Salt;
    };

    //=========================================================================
    // RÔLE 1 — SOURCE : DALLE / SLAB VOID  (FlatPlain ET CrystalChamber)
    //=========================================================================
    // Transcription littérale des ÉTAPES 1-3 de `GetSlabDensity` : surface de sol, surface de
    // plafond, puis `Density = -min(distAuSol, distAuPlafond)`.
    //
    // DEUX archétypes, UN opérateur. `GetSlabDensity` est appelé pour FlatPlain et
    // CrystalChamber sans le moindre branchement sur le type — CrystalChamber n'est rien d'autre
    // que FlatPlain avec un `CeilingRoughness` plus grand. C'est le premier vrai gain du refactor
    // (OPSTACK-PLAN §4) : deux des huit archétypes disparaissent dans un seul opérateur, et la
    // différence entre eux redevient ce qu'elle a toujours été — un jeu de valeurs par défaut.
    //
    // Two archetypes, ONE op: GetSlabDensity is called for both with no branch on the type.
    // CrystalChamber IS FlatPlain with a bigger CeilingRoughness.
    //
    // XY-PUR depuis §3.1 (le terme en Z des deux bruits est parti). C'est ce qui rend
    // `ClassifyBox` exact plutôt qu'estimé — voir plus bas.
    class FSlabVoidSource final : public IVoxelDensityOp
    {
    public:
        FSlabVoidSource(const FSlabGenerationParams& P, int32 Seed)
            : SeedU((uint32)Seed)
            , FloorRoughness(P.FloorRoughness)
            , FloorFrequency(P.FloorRoughnessFrequency)
            , CeilRoughness(P.CeilingRoughness)
            , CeilFrequency(P.CeilingRoughnessFrequency)
        {
            const float StrateHeight = P.StrateTopWorldZ - P.StrateBottomWorldZ;
            FloorZ = P.StrateBottomWorldZ + StrateHeight * P.FloorRelativeHeight;
            CeilZ  = P.StrateBottomWorldZ + StrateHeight * P.CeilingRelativeHeight;

            // FBM's proved supremum is 1.5, not the parameter's nominal [-1,1] label. Bounds must
            // cover every authored value, so never normalise the roughness parameter to repair a
            // bound: multiply the actual maximum noise envelope instead.
            FloorAmp = VOXEL_NOISE_SCALE * VF_PerlinAbsBound * FMath::Abs(FloorRoughness);
            CeilAmp  = VOXEL_NOISE_SCALE * VF_PerlinAbsBound * FMath::Abs(CeilRoughness);
        }

        EVoxelOpRole GetRole() const override { return EVoxelOpRole::FieldSource; }
        EVoxelOpChannelMask ChannelReads() const override { return VoxelOpChannels::None; }
        EVoxelOpChannelMask ChannelWrites() const override { return VoxelOpChannels::Density; }
        bool IsAdditive() const override { return false; }
        void PrepareChunk(const FVoxelOpContext&) override {}

        // ⚠️⚠️ `false`, ET NE PAS LE PASSER À `true`.
        //
        // Le contrat de `IsXYPure` est « **`Eval`** ne dépend pas de Z » — pas « les surfaces ne
        // dépendent pas de Z ». Or `Eval` calcule `min(Z - sol, plafond - Z)` : il dépend de Z de
        // la façon la plus directe qui soit. §3.1 a rendu les SURFACES pures en XY ; la DENSITÉ,
        // elle, ne l'a jamais été et ne peut pas l'être — c'est une distance à une surface.
        //
        // Le jour où un cache de colonnes générique lit ce drapeau, un `true` ici ferait partager
        // UNE valeur de densité sur TOUTE la pile verticale de chunks — un monde silencieusement
        // faux, que `ValidateDeterminism` ne verrait pas parce qu'il échantillonne le long d'un
        // bord X. C'est exactement le piège que l'avertissement de `VoxelDensityOp.h` décrit.
        //
        // ⚠️ Must stay `false`. The contract is "**Eval** does not depend on Z", and Eval computes
        // min(Z - floor, ceil - Z). §3.1 made the SURFACES XY-pure; the DENSITY never was and
        // cannot be — it is a distance to a surface. A generic column cache reading `true` here
        // would share one density down the whole vertical chunk stack.
        //
        // C'est précisément cette distinction qui justifie l'espace-hauteur (`VoxelHeightOp.h`) :
        // ce qui est pur en XY, ce sont les HAUTEURS, et elles y sont dans un type qui n'a pas de Z.
        bool IsXYPure() const override { return false; }

        void Eval(float WorldX, float WorldY, float WorldZ, FVoxelOpSample& InOut) const override
        {
            const float FloorSurface = SurfaceFloor(WorldX, WorldY);
            const float CeilSurface  = SurfaceCeil(WorldX, WorldY, FloorSurface);

            const float DistAboveFloor = WorldZ - FloorSurface;
            const float DistBelowCeil  = CeilSurface - WorldZ;
            const float VoidField      = FMath::Min(DistAboveFloor, DistBelowCeil);

            InOut.Density = -VoidField;   // Replace : interne, positif = solide
        }

        //---------------------------------------------------------------------
        // LE VERDICT QUE FLATPLAIN N'A JAMAIS EU
        //---------------------------------------------------------------------
        // `ClassifyTile` ne prouve AUCUNE tuile pour les archétypes de grotte aujourd'hui. Ici la
        // preuve est immédiate et n'exige aucun échantillonnage : les deux surfaces vivent dans des
        // BANDES en Z dont on connaît les bornes exactes, donc une boîte entièrement sous la bande
        // du sol est solide, et une boîte entièrement entre les deux bandes est de l'air.
        //
        // ⚠️ Conservatif dans le bon sens : rendre `Mixed` ne coûte que du CPU, rendre le mauvais
        // verdict est un TROU. Toutes les comparaisons ci-dessous sont donc strictes et prennent le
        // pire cas des deux bruits.
        EVoxelTileClass ClassifyBox(const FBox& VoxelBox, const FVoxelOpContext&) const override
        {
            const float ZMin = (float)VoxelBox.Min.Z;
            const float ZMax = (float)VoxelBox.Max.Z;

            // Bornes de la surface de sol : FloorZ ± FloorAmp.
            const float FloorLo = FloorZ - FloorAmp;
            const float FloorHi = FloorZ + FloorAmp;

            // Bornes du plafond. `CeilNoise = |bruit| · rugosité` ∈ [0, CeilAmp] ⇒ la surface ne
            // peut que DESCENDRE depuis CeilZ… sauf que le clamp `Max(…, FloorSurface + 2)` peut la
            // remonter. Le majorant honnête est donc le max des deux possibilités.
            const float CeilLo = CeilZ - CeilAmp;
            const float CeilHi = FMath::Max(CeilZ, FloorHi + 2.0f);

            // Sous le sol le plus bas possible ⇒ distAuSol < 0 partout ⇒ densité > 0 ⇒ SOLIDE.
            if (ZMax < FloorLo) { return EVoxelTileClass::AllSolid; }

            // Au-dessus du plafond le plus haut possible ⇒ distAuPlafond < 0 ⇒ SOLIDE.
            if (ZMin > CeilHi)  { return EVoxelTileClass::AllSolid; }

            // Strictement entre les deux bandes ⇒ les deux distances sont > 0 ⇒ densité < 0 ⇒ AIR.
            // (Les colonnes peuvent re-remplir cet air : c'est FGridColumnMod qui le déclare, en
            //  rendant FillOnly quand une colonne atteint la boîte. Le pliage s'en charge.)
            if (ZMin > FloorHi && ZMax < CeilLo) { return EVoxelTileClass::AllAir; }

            return EVoxelTileClass::Mixed;
        }

        EVoxelOpEffect EffectOverBox(const FBox&, const FVoxelOpContext&) const override
        {
            return EVoxelOpEffect::Both;   // jamais atteint : ClassifyBox répond avant
        }

    private:
        // The surface coordinates stay in float; an FVector (double) round-trip is not part of
        // the output contract.
        float SurfaceFloor(float WorldX, float WorldY) const
        {
            if (FloorRoughness <= 0.0f) { return FloorZ; }
            const float FF = FloorFrequency;
            const FVector3f NoisePos(WorldX * FF + VoxelHash::SeedOffset(SeedU, 7.3f),
                                   WorldY * FF + VoxelHash::SeedOffset(SeedU, 11.1f),
                                   0.0f);
            const float N = VoxelNoise::FBM(NoisePos.X, NoisePos.Y, NoisePos.Z,
                                            VoxelGenLOD::Eff(3), 2.0f, 0.5f)
                          * VOXEL_NOISE_SCALE * FloorRoughness;
            return FloorZ + N;
        }

        float SurfaceCeil(float WorldX, float WorldY, float FloorSurface) const
        {
            float CeilNoise = 0.0f;
            if (CeilRoughness > 0.0f)
            {
                const float CF = CeilFrequency;
                const FVector3f NoisePos(WorldX * CF + VoxelHash::SeedOffset(SeedU, 17.3f) + 1000.0f,
                                       WorldY * CF + VoxelHash::SeedOffset(SeedU, 19.7f) + 2000.0f,
                                       3000.0f);
                const float Raw = VoxelNoise::FBM(NoisePos.X, NoisePos.Y, NoisePos.Z,
                                                  VoxelGenLOD::Eff(3), 2.0f, 0.5f)
                                * VOXEL_NOISE_SCALE;
                // abs() ⇒ les formations ne pendent QUE vers le bas.
                CeilNoise = FMath::Abs(Raw) * CeilRoughness;
            }
            return FMath::Max(CeilZ - CeilNoise, FloorSurface + 2.0f);
        }

        uint32 SeedU;
        float FloorZ = 0.0f, CeilZ = 0.0f;
        float FloorRoughness, FloorFrequency;
        float CeilRoughness,  CeilFrequency;
        float FloorAmp = 0.0f, CeilAmp = 0.0f;
    };

    //=========================================================================
    // RÔLE 1 — SOURCE : COLONNE DE SURFACE / SURFACE COLUMN  (SurfaceWorld)
    //=========================================================================
    // Le pont entre les deux espaces : consomme DEUX piles de hauteur (sol et voûte) et en fait une
    // densité. C'est tout le combine de `SurfaceDensityFromColumn` :
    //
    //     Density = TerrainZ - Z                 ← solide sous le sol
    //     Density = max(Density, Z - CeilSurf)   ← solide au-dessus de la voûte
    //
    // ⚠️ `IsXYPure()` est **false**, et la distinction est LE point de tout ce découpage : les
    // HAUTEURS sont pures en XY (elles vivent dans `VoxelHeightOp.h`, un type sans Z), la DENSITÉ
    // ne l'est pas et ne peut pas l'être — c'est une distance à une surface. Confondre les deux est
    // exactement le bug que `FSlabVoidSource` portait jusqu'à aujourd'hui.
    //
    // The bridge between the two spaces: consumes two HEIGHT stacks and turns them into density.
    // IsXYPure is false — the heights are XY-pure, the density is a distance to them and never can be.
    class FSurfaceColumnSource final : public IVoxelDensityOp
    {
    public:
        /**
         * @param PerBiome  vide ⇒ pas de biomes, chemin d'origine inchangé. Non vide ⇒ le sol est
         *                  mélangé et le plafond sélectionné par `InField`.
         * @param InField   **POSSÉDÉ** — délibérément, plutôt qu'un pointeur nu. L'adaptateur réel
         *                  pointe vers des `thread_local` du générateur ; lier sa durée de vie à
         *                  celle de la pile (elle-même `thread_local`, reconstruite au même moment)
         *                  rend la question de survie structurelle au lieu de la laisser à une
         *                  convention que le prochain lecteur devrait deviner.
         *                  OWNED on purpose rather than borrowed: tying its lifetime to the stack's
         *                  makes the survival question structural instead of conventional.
         */
        FSurfaceColumnSource(const FSurfaceGenerationParams& InP, int32 Seed,
                             const TArray<FSurfaceGenerationParams>& PerBiome,
                             TUniquePtr<IVoxelBiomeField> InField)
            : P(InP), BiomeParams(PerBiome), Field(MoveTemp(InField))
        {
            if (BiomeParams.Num() > 0)
            {
                // Chemin BIOMES : une pile complète par biome, sol mélangé / plafond sélectionné.
                TerrainStack.Add(VoxelHeightOps::MakeBiomeBlendHeightSource(BiomeParams, Seed, Field.Get()));
                CeilingStack.Add(VoxelHeightOps::MakeBiomeSelectCeilingSource(BiomeParams, Seed, Field.Get()));

                // Un champ structurel PAR BIOME : la pente de l'overhang doit venir du champ du
                // biome DOMINANT (l'original échantillonne `*PD`), pas d'un champ moyen.
                PerBiomeStructural.Reserve(BiomeParams.Num());
                for (const FSurfaceGenerationParams& BP : BiomeParams)
                {
                    const IVoxelHeightOp* Raw = nullptr;
                    PerBiomeStructural.Add(VoxelHeightOps::MakeStructuralHeightSource(BP, Seed, &Raw));
                }
                Structural = PerBiomeStructural.Num() > 0 ? PerBiomeStructural[0].Get() : nullptr;
            }
            else
            {
                BuildSingleBiome(InP, Seed);
            }

            // Identité unique et NON RECYCLÉE — la clé du mémo par colonne. `this` ne suffirait
            // pas : une pile détruite puis une autre allouée à la même adresse avec d'autres params
            // donnerait un faux positif silencieux. Un compteur qui ne redescend jamais l'interdit.
            // Assignée ICI et nulle part ailleurs : l'autre constructeur délègue à celui-ci.
            // A unique, never-recycled id — assigned here only; the other ctor delegates.
            static std::atomic<uint64> NextId{ 1 };
            InstanceId = NextId.fetch_add(1, std::memory_order_relaxed);

            // Départ sur l'identité D'INSTANCE, pas sur 0 : les entrées du mémo s'initialisent à
            // `Key = 0`, donc une clé nulle ferait FAUSSEMENT toucher le slot vierge en (0,0).
            // `PrepareChunk` remplacera ceci par la clé partagée de strate ; sans lui, on garde un
            // cache par instance — moins de partage, mais correct. Dégrader, jamais mentir.
            // Starting at the INSTANCE id rather than 0: slots initialise to Key = 0, so a zero key
            // would falsely hit the pristine slot at (0,0). PrepareChunk upgrades this to the shared
            // strate key; without it we simply cache per instance. Degrade, never lie.
            ColumnKey = InstanceId;

            // Empreinte des params qui déterminent une colonne. `FSurfaceGenerationParams` est du
            // POD pur (que des float/int/bool, vérifié : aucun TArray, FString ni pointeur), donc
            // un CRC mémoire ne peut pas produire de FAUX POSITIF — au pire du padding non
            // initialisé donne un faux NÉGATIF, c'est-à-dire un recalcul. Se tromper du côté qui
            // coûte du CPU plutôt que du côté qui rend une mauvaise colonne.
            // Pure POD (verified: no TArray/FString/pointer), so a memory CRC cannot produce a false
            // HIT; at worst padding causes a false miss, i.e. a recompute. Err toward CPU, not lies.
            ParamsFingerprint = FCrc::MemCrc32(&P, sizeof(P));
            for (const FSurfaceGenerationParams& BP : BiomeParams)
            {
                ParamsFingerprint = VoxelHash::Mix(ParamsFingerprint ^ FCrc::MemCrc32(&BP, sizeof(BP)));
            }
        }

        void BuildSingleBiome(const FSurfaceGenerationParams& InP, int32 Seed)
        {
            // Construite à la main (pas via BuildSurfaceHeightStack) pour GARDER le pointeur vers la
            // source structurelle : l'overhang en a besoin, pour son gradient de pente comme pour
            // son ré-échantillonnage amont. Même dépendance que le cliff, même raison.
            TerrainStack.Add(VoxelHeightOps::MakeStructuralHeightSource(InP, Seed, &Structural));
            TerrainStack.Add(VoxelHeightOps::MakeCliffHeightMod(InP, Structural));
            TerrainStack.Add(VoxelHeightOps::MakeTerraceHeightMod(InP));
            TerrainStack.Add(VoxelHeightOps::MakeLayerLineHeightMod(InP));
            TerrainStack.Add(VoxelHeightOps::MakeBeachHeightMod(InP));

            VoxelHeightOps::BuildSurfaceCeilingStack(CeilingStack, InP, Seed);
        }

        /** La colonne complète, exactement les cinq sorties de `ComputeSurfaceColumn`.
         *  Le mémo est un LRU spatial de six boîtes à index direct, comme `GSurfColCache` : la
         *  pile évalue tous les Z d'une colonne au même XY, donc l'overhang lit la MÊME colonne
         *  que la source, par construction plutôt que par convention.
         *
         *  The memo is a six-box spatial LRU with direct XY indexing, matching `GSurfColCache`.
         *  Six boxes retain interleaved strate regions at the cost of roughly 0.79 MiB of TLS for
         *  the five-float column payload plus one computed flag per cell, before compiler padding. */
        struct FColumn { float TerrainZ, CeilSurf, OverhangAmp, DirX, DirY; };

        const FColumn& GetColumn(float WorldX, float WorldY) const
        {
            // Même schéma éprouvé que `GSurfColCache` : six boîtes à index direct dans XY, chacune
            // avec un drapeau `Computed` par cellule et une clé uint64 exacte. Une tuile MC pleine
            // résolution demande 35×35 = 1225 colonnes (anneau de marge inclus) ; une boîte de
            // Dim×Dim, recentrée sur le premier échantillon, les garde toutes sans éviction.
            // Same proven scheme as `GSurfColCache`: six direct-indexed XY boxes, each with one
            // `Computed` flag per cell and an exact uint64 key. A full-resolution MC tile needs
            // 35×35 = 1225 columns including its margin ring; one Dim×Dim box holds that tile.
            struct FColumnBox
            {
                enum : int32 { Halo = CHUNK_SIZE + 8, Dim = 2 * Halo + 1 };
                int32 BaseX = 0, BaseY = 0;
                uint64 Key = 0;             // strate + layout + seed + ParamsFingerprint
                uint32 LastUse = 0;         // LRU stamp
                bool bValid = false;
                FColumn Cols[Dim * Dim];
                bool Computed[Dim * Dim];
            };

            struct FColumnCache
            {
                enum : int32 { NumBoxes = 6 };
                FColumnBox Boxes[NumBoxes];
                uint32 Clock = 0;

                // Hit exact : clé complète + couverture XY complète. En cas de miss, seul le
                // victim LRU est recentré et invalidé ; les cinq autres boîtes restent chaudes.
                // Exact hit: full key + full XY coverage. On a miss, only the LRU victim is
                // recentered and invalidated; the other five boxes remain warm.
                FColumnBox& Acquire(int32 IX, int32 IY, uint64 InColumnKey)
                {
                    ++Clock;
                    for (FColumnBox& B : Boxes)
                    {
                        if (B.bValid && B.Key == InColumnKey
                            && IX >= B.BaseX && IX < B.BaseX + FColumnBox::Dim
                            && IY >= B.BaseY && IY < B.BaseY + FColumnBox::Dim)
                        {
                            B.LastUse = Clock;
                            return B;
                        }
                    }

                    // Miss d'acquisition : évincer/recentrer une seule boîte, jamais tout le cache.
                    // Acquisition miss: evict/recenter one box only, never the whole cache.
                    FColumnBox* Victim = &Boxes[0];
                    for (FColumnBox& B : Boxes)
                    {
                        if (B.LastUse < Victim->LastUse) Victim = &B;
                    }
                    Victim->BaseX = IX - FColumnBox::Halo;
                    Victim->BaseY = IY - FColumnBox::Halo;
                    Victim->Key = InColumnKey;
                    Victim->LastUse = Clock;
                    Victim->bValid = true;
                    FMemory::Memzero(Victim->Computed, sizeof(Victim->Computed));
                    return *Victim;
                }
            };

            thread_local FColumnCache Cache = {};
            thread_local FColumn DirectColumn = {};

            // The production mesher and the exact-lattice classifier use integer XY. Fractional
            // XY is still valid for the public density/equivalence probes: compute it directly so
            // no integer cell can ever be returned for a different full (WorldX, WorldY) pair.
            const bool bIntegerXY = WorldX == FMath::FloorToFloat(WorldX)
                                 && WorldY == FMath::FloorToFloat(WorldY);
            FColumn* MemoColumn = &DirectColumn;
            // ⚠️ La boîte acquise doit survivre au `if` : le drapeau `Computed` n'est posé qu'APRÈS
            // le calcul, plus bas, hors de cette portée. Non nul ⇔ chemin XY entier.
            // The acquired box must outlive the `if`: the `Computed` flag is only set AFTER the
            // column is computed, further down and outside this scope. Non-null <=> integer path.
            FColumnBox* AcquiredBox = nullptr;
            int32 CI = 0;
            bool bNeedsCompute = true;

            if (bIntegerXY)
            {
                const int32 IX = (int32)WorldX;
                const int32 IY = (int32)WorldY;

                // Acquire vérifie la clé uint64 complète et les bornes exactes avant de dériver CI.
                // Acquire checks the exact uint64 key and exact bounds before deriving CI.
                FColumnBox& Box = Cache.Acquire(IX, IY, ColumnKey);
                AcquiredBox = &Box;

                CI = (IY - Box.BaseY) * FColumnBox::Dim + (IX - Box.BaseX);
                MemoColumn = &Box.Cols[CI];
                if (Box.Computed[CI])
                {
                    INC_DWORD_STAT(STAT_VoxelForgeColumnMemoHit);
                    bNeedsCompute = false;
                }
                else
                {
                    INC_DWORD_STAT(STAT_VoxelForgeColumnMemoMiss);
                }
            }
            else
            {
                INC_DWORD_STAT(STAT_VoxelForgeColumnMemoMiss);
            }

            if (bNeedsCompute)
            {
                FColumn& C = *MemoColumn;

                C.TerrainZ = TerrainStack.EvalHeight(WorldX, WorldY);
                C.CeilSurf = CeilingStack.EvalHeight(WorldX, WorldY);
                C.OverhangAmp = 0.0f;  C.DirX = 0.0f;  C.DirY = 0.0f;

                // Gate d'overhang par colonne : pente issue d'une différence AVANT du champ
                // STRUCTUREL, à l'échelle de la portée mais CLAMPÉE à [4,16]. Sans ce clamp, une
                // grande `Reach` moyenne la pente sur une énorme portée et lit même une vraie
                // falaise comme plate — le bug « grande Reach = rien ». Transcrit tel quel.
                // Quel jeu de params gouverne cette colonne ? Sans biomes, `P`. Avec, le DOMINANT
                // pour la pente et le seuil, et une interpolation de l'AMPLITUDE vers le voisin —
                // exactement ce que fait `ComputeSurfaceColumn` (`Lerp(Amp(PD), Amp(PN), W)`, pente
                // depuis `*PD` seul). Interpoler la pente n'aurait pas de sens : c'est une mesure du
                // terrain, pas un réglage.
                const FSurfaceGenerationParams* PD = &P;
                const FSurfaceGenerationParams* PN = nullptr;
                float W = 0.0f;
                const IVoxelHeightOp* SlopeField = Structural;

                if (BiomeParams.Num() > 0 && Field)
                {
                    const FVoxelBiomeWeights BW = Field->SampleAt(WorldX, WorldY);
                    const int32 Di = BiomeParams.IsValidIndex(BW.Dominant) ? BW.Dominant : 0;
                    PD = &BiomeParams[Di];
                    if (PerBiomeStructural.IsValidIndex(Di)) { SlopeField = PerBiomeStructural[Di].Get(); }
                    if (BW.NeighborWeight > 0.0f && BiomeParams.IsValidIndex(BW.Neighbor))
                    {
                        PN = &BiomeParams[BW.Neighbor];
                        W  = BW.NeighborWeight;
                    }
                }

                const bool bAnyOverhang = (PD->OverhangStrength > 0.0f)
                                       || (PN && PN->OverhangStrength > 0.0f);

                if (bAnyOverhang && SlopeField != nullptr)
                {
                    const float SD = FMath::Clamp(PD->OverhangReach, 4.0f, 16.0f);
                    const float Z0 = SampleStructuralOf(SlopeField, WorldX, WorldY);
                    const float GX = (SampleStructuralOf(SlopeField, WorldX + SD, WorldY) - Z0) / SD;
                    const float GY = (SampleStructuralOf(SlopeField, WorldX, WorldY + SD) - Z0) / SD;
                    const float Slope = FMath::Sqrt(GX * GX + GY * GY);

                    auto Amp = [Slope](const FSurfaceGenerationParams& Q) -> float
                    {
                        if (Q.OverhangStrength <= 0.0f) { return 0.0f; }
                        const float Thr  = FMath::Max(Q.OverhangSlopeThreshold, 0.05f);
                        const float Gate = FMath::Clamp((Slope - Thr) / Thr, 0.0f, 1.0f);
                        return Q.OverhangStrength * Gate;   // [0,1]
                    };
                    C.OverhangAmp = (W > 0.0f && PN) ? FMath::Lerp(Amp(*PD), Amp(*PN), W) : Amp(*PD);

                    // Direction amont unitaire (le gradient pointe vers le haut). Dégénérée sur le
                    // plat — mais l'amplitude y vaut 0 de toute façon.
                    if (Slope > KINDA_SMALL_NUMBER) { C.DirX = GX / Slope; C.DirY = GY / Slope; }
                }

                if (AcquiredBox) { AcquiredBox->Computed[CI] = true; }
            }
            return *MemoColumn;
        }

        /** Le champ structurel nu — l'overhang s'en sert pour emprunter la roche amont.
         *  Avec des biomes, c'est celui du biome DOMINANT en ce point (l'original emprunte à `*PD`). */
        float SampleStructural(float WorldX, float WorldY) const
        {
            const IVoxelHeightOp* Src = Structural;
            if (BiomeParams.Num() > 0 && Field)
            {
                const FVoxelBiomeWeights BW = Field->SampleAt(WorldX, WorldY);
                const int32 Di = PerBiomeStructural.IsValidIndex(BW.Dominant) ? BW.Dominant : 0;
                if (PerBiomeStructural.IsValidIndex(Di)) { Src = PerBiomeStructural[Di].Get(); }
            }
            return SampleStructuralOf(Src, WorldX, WorldY);
        }

        static float SampleStructuralOf(const IVoxelHeightOp* Src, float WorldX, float WorldY)
        {
            if (!Src) { return 0.0f; }
            FVoxelHeightSample S;
            Src->Eval(WorldX, WorldY, S);
            return S.Height;
        }

        EVoxelOpRole GetRole() const override { return EVoxelOpRole::FieldSource; }
        EVoxelOpChannelMask ChannelReads() const override { return VoxelOpChannels::None; }
        EVoxelOpChannelMask ChannelWrites() const override { return VoxelOpChannels::Density; }
        EVoxelOpResourceMask ProvidedResources() const override { return VoxelOpResources::SurfaceColumn; }
        bool IsAdditive() const override { return false; }
        bool IsXYPure() const override { return false; }   // voir le bloc ci-dessus

        /**
         * ⚠️ C'EST ICI QUE SE JOUE LA PERF DE CET ARCHÉTYPE.
         *
         * Ne PAS clé ce mémo sur `InstanceId` : il change à CHAQUE reconstruction de pile,
         * c'est-à-dire à chaque chunk, et une strate haute de 4 chunks recalculerait ses colonnes
         * **4 fois**, resamples du cliff compris. `GSurfColCache` est clé sur
         * `(boîte XY, StrateKey, Seed, LayoutVersion)` **SANS ChunkZ**, délibérément, « shared down
         * the whole vertical strate stack ». Cette pile reprend la même identité de
         * strate/layout/seed, en ajoutant l'empreinte obligatoire des params pour protéger ses
         * sorties propres ; son mémo est un LRU spatial de six boîtes.
         *
         * Donc la clé garde l'identité partagée : ce qui rend deux colonnes interchangeables, c'est
         * la STRATE, le seed, la version de layout et les params, pas le chunk. Le mémo étant
         * `thread_local`, il SURVIT à la reconstruction de la pile — seule la clé l'invalide.
         *
         * POURQUOI C'EST SÛR : les hauteurs sont XY-pures par construction (c'est tout l'objet de
         * `VoxelHeightOp.h`, où le type n'a pas de Z), et le champ de biomes est documenté
         * XY-pur — « ZERO Z dependence: the climate/Voronoi fields are pure-XY ». C'est exactement
         * la justification sur laquelle `GSurfColCache` repose déjà.
         *
         * Do not key the memo on InstanceId: it changes every chunk, so a 4-chunk strate would
         * recompute every column 4x. GSurfColCache deliberately omits ChunkZ and shares down the whole
         * vertical stack; this shares the same strate/layout/seed identity and adds the required
         * params fingerprint for its own outputs. The six-box LRU keeps independent XY regions alive. Safe
         * because heights are XY-pure by type and the biome field is documented Z-independent.
         */
        void PrepareChunk(const FVoxelOpContext& Ctx) override
        {
            // `StrateBottomWorldZ` est unique par strate empilée — la même valeur que
            // `CP_StrateKey` utilise dans `GetDensityAt`. La version de layout entre dans la clé
            // (AUDIT §C2) : une édition à chaud qui change les params sans déplacer la strate doit
            // invalider, sinon on sert des colonnes périmées.
            const uint32 A = (uint32)FMath::RoundToInt(Ctx.StrateBottomWorldZ);
            const uint32 B = Ctx.LayoutVersion;
            const uint32 C = Ctx.Seed;

            // ⚠️ `ParamsFingerprint` EST OBLIGATOIRE, et son absence a été un vrai bug — attrapé par
            // `SurfaceHeightEquivalence` au build suivant (69/20000 écarts, 1 traversée d'iso).
            //
            // Sans lui, la clé ne contenait que (strate, layout, seed). Deux piles de la MÊME strate
            // avec des params DIFFÉRENTS obtenaient donc la même clé et se partageaient les colonnes :
            // la seconde lisait les colonnes de la première, calculées avec `OverhangAmp = 0`, et
            // l'overhang disparaissait purement et simplement.
            //
            // ET CE N'EST PAS QU'UN ARTEFACT DE TEST : c'est exactement la faiblesse que le code
            // documente déjà pour `GSurfColCache` (VoxelGenerator.cpp, note AUDIT §C2 étendue) —
            // « une édition à chaud qui change les params SANS déplacer la strate laisse la clé
            // identique et sert des colonnes périmées ». En production `LayoutVersion` bouge à chaque
            // `RebuildStrates`, ce qui masque le trou ; ma clé en avait hérité, et le test l'a trouvé
            // tout de suite. Empreinte incluse ⇒ le trou est fermé ici, pas seulement masqué.
            //
            // The fingerprint is REQUIRED: without it two stacks of the same strate with different
            // params shared columns, and the overhang silently vanished. Same weakness the codebase
            // already documents for GSurfColCache, which LayoutVersion merely masks.
            ColumnKey = ((uint64)VoxelHash::Mix(A ^ VoxelHash::Mix(B)) << 32)
                      |  (uint64)VoxelHash::Mix(C ^ VoxelHash::Mix(A) ^ ParamsFingerprint);
            if (ColumnKey == 0) { ColumnKey = 1; }   // 0 = « jamais préparé »
        }

        void Eval(float WorldX, float WorldY, float WorldZ, FVoxelOpSample& InOut) const override
        {
            // Le mémo par colonne vit ici, dans six boîtes thread_local partagées par les instances
            // mais séparées par la clé, et lues par les Eval de cette source et FOverhangShelfMod.
            // Il est séparé de `GSurfColCache` : la pile possède ses propres sorties et sa clé
            // complète (strate + layout + seed + empreinte des params), donc réutiliser le cache
            // du générateur serait incorrect.
            // The per-column memo lives here in six thread-local boxes shared across instances but
            // separated by the key, and read by this source's Eval calls and FOverhangShelfMod.
            // It is separate from `GSurfColCache`: the stack owns its own outputs and full key.
            const FColumn& C = GetColumn(WorldX, WorldY);

            float Density = C.TerrainZ - WorldZ;
            Density = FMath::Max(Density, WorldZ - C.CeilSurf);
            InOut.Density = Density;   // Replace : interne, positif = solide
        }

        // Mixed, honnêtement. Un verdict exact demanderait de borner le heightfield structurel sur
        // la boîte XY (continents + montagnes + détail sous un domain-warp) — faisable, mais c'est
        // une vraie borne à dériver, pas une constante à lire comme pour la dalle. Rendre Mixed ne
        // coûte que du CPU ; rendre faux serait un trou. À faire quand `MaxDisplacement` saura
        // répondre pour la source structurelle.
        EVoxelOpEffect EffectOverBox(const FBox&, const FVoxelOpContext&) const override
        {
            return EVoxelOpEffect::Both;
        }

    private:
        FSurfaceGenerationParams P;
        FVoxelHeightStack TerrainStack;
        FVoxelHeightStack CeilingStack;
        const IVoxelHeightOp* Structural = nullptr;   // NON possédant : la pile terrain le possède

        // Chemin BIOMES. Vide ⇒ chemin d'origine, inchangé bit pour bit.
        TArray<FSurfaceGenerationParams>     BiomeParams;
        TUniquePtr<IVoxelBiomeField>         Field;              // POSSÉDÉ (voir le constructeur)
        TArray<TUniquePtr<IVoxelHeightOp>>   PerBiomeStructural; // pente d'overhang par biome

        uint64 InstanceId = 0;   // unique, jamais recyclée — le repli quand PrepareChunk n'a pas eu lieu
        uint64 ColumnKey  = 0;   // l'identité PARTAGÉE (strate + layout + seed + params) : voir PrepareChunk
        uint32 ParamsFingerprint = 0;   // sans lui, deux piles de la même strate se volaient leurs colonnes
    };

    //=========================================================================
    // RÔLE 3 — MODIFIER : ÉTAGÈRE D'OVERHANG / OVERHANG SHELF  (le seul op vraiment 3D)
    //=========================================================================
    // Pour les voxels d'AIR dans une fenêtre juste au-dessus d'une pente raide, ré-échantillonne le
    // heightfield EN AMONT (vers la falaise) d'une distance qui CROÎT avec la hauteur, et fait
    // l'union de cette roche → la roche du haut de falaise déborde AU-DESSUS du vide, avec de l'air
    // EN DESSOUS : un vrai surplomb.
    //
    // ⚠️ Il dépend de Z de façon essentielle — `Frac` fait varier la portée avec l'altitude. C'est
    // le seul op de SurfaceWorld qui ne pouvait PAS vivre en espace-hauteur, et c'est exactement
    // pour ça que la frontière entre les deux espaces est utile : elle est passée là où le code
    // change de nature, pas là où c'était commode.
    //
    // The one op here that genuinely depends on Z (the uphill reach grows with height), which is
    // precisely why it could not live in height space. The boundary between the two spaces falls
    // where the code changes nature.
    class FOverhangShelfMod final : public IVoxelDensityOp
    {
    public:
        FOverhangShelfMod(const FSurfaceGenerationParams& InP, int32 Seed,
                          const FSurfaceColumnSource* InColumn)
            : P(InP), SeedU((uint32)Seed), Column(InColumn) {}

        EVoxelOpRole GetRole() const override { return EVoxelOpRole::DetailModifier; }
        EVoxelOpChannelMask ChannelReads() const override { return VoxelOpChannels::Density; }
        EVoxelOpChannelMask ChannelWrites() const override { return VoxelOpChannels::Density; }
        EVoxelOpResourceMask RequiredResources() const override { return VoxelOpResources::SurfaceColumn; }
        bool IsAdditive() const override { return false; }
        void PrepareChunk(const FVoxelOpContext&) override {}
        bool IsXYPure() const override { return false; }   // franchement non : voir `Frac`

        void Eval(float WorldX, float WorldY, float WorldZ, FVoxelOpSample& InOut) const override
        {
            if (Column == nullptr || P.OverhangHeight <= 0.0f) { return; }

            // Même colonne que la source, garantie par le mémo : pas une seconde évaluation.
            const FSurfaceColumnSource::FColumn& C = Column->GetColumn(WorldX, WorldY);
            if (C.OverhangAmp <= 0.0f) { return; }

            // Gate dur, transcrit : seulement les voxels d'air dans `OverhangHeight` du sol local.
            if (!(WorldZ > C.TerrainZ && WorldZ <= C.TerrainZ + P.OverhangHeight)) { return; }

            const float f = P.OverhangFrequency;
            // Bruit de forme d'étagère [0,1] ; le terme en Z fait onduler la portée avec la hauteur
            // (déchiqueté, pas une lèvre lisse).
            const float Ns = HFractal3D(FVector3f(
                WorldX * f + VoxelHash::SeedOffset(SeedU, 17.3f),
                WorldY * f + VoxelHash::SeedOffset(SeedU, 23.9f),
                WorldZ * f * P.OverhangZScale + VoxelHash::SeedOffset(SeedU, 5.1f)), 3) * 0.5f + 0.5f;   // [0,1]

            // LA CLÉ : la portée amont CROÎT avec la hauteur dans la fenêtre (Frac : 0 au sol → 1
            // au plafond de la fenêtre). En bas le décalage est minuscule ⇒ on emprunte de la roche
            // basse voisine ⇒ ça reste de l'AIR au-dessus du vide ; en haut le décalage atteint la
            // falaise ⇒ solide ⇒ la lèvre se pose dessus avec de l'air DESSOUS = un vrai surplomb.
            const float Frac   = (WorldZ - C.TerrainZ) / P.OverhangHeight;
            const float ShiftV = P.OverhangReach * C.OverhangAmp * Frac * Ns;
            if (ShiftV > 0.5f)
            {
                // On emprunte la hauteur STRUCTURELLE amont (pas la surface complète avec ops) :
                // le dessous de l'étagère n'a pas besoin du raffinement cliff/terrace, et ça évite
                // de relancer les 4 resamples du cliff par voxel de lèvre.
                const float ShiftedTZ = Column->SampleStructural(WorldX + C.DirX * ShiftV,
                                                                 WorldY + C.DirY * ShiftV);
                InOut.Density = FMath::Max(InOut.Density, ShiftedTZ - WorldZ);   // union
            }
        }

        // N'ajoute que du solide (`Max`) ⇒ tue AllAir, jamais AllSolid. Conservateur : on ne sait
        // pas sans échantillonner si une colonne d'overhang touche la boîte, donc FillOnly partout
        // où l'archétype peut en produire, Identity quand il est éteint.
        EVoxelOpEffect EffectOverBox(const FBox&, const FVoxelOpContext&) const override
        {
            return (P.OverhangStrength > 0.0f && P.OverhangHeight > 0.0f)
                 ? EVoxelOpEffect::FillOnly : EVoxelOpEffect::Identity;
        }

    private:
        FSurfaceGenerationParams P;
        uint32 SeedU;
        const FSurfaceColumnSource* Column;   // NON possédant : la pile possède la source
    };

    //=========================================================================
    // RÔLE 3 — MODIFIER : RUGOSITÉ DE PAROI, ESPACE SDF
    //=========================================================================
    // La variante SDF (Maze / VerticalShafts / FloatingIslands) : `Sdf += bruit · échelle · force`.
    // Déplace la SURFACE. La variante densité de TunnelNetwork est un opérateur DIFFÉRENT (fade
    // quadratique, clamp anti-remplissage, 4 types de bruit) — voir OPSTACK-DECOMPOSITION §1.
    class FSdfRoughnessMod final : public IVoxelDensityOp
    {
    public:
        FSdfRoughnessMod(float InStrength, float InFrequency, int32 InBaseOctaves, float InApplyWithin)
            : Strength(InStrength), Frequency(InFrequency)
            , BaseOctaves(InBaseOctaves), ApplyWithin(InApplyWithin) {}

        EVoxelOpRole GetRole() const override { return EVoxelOpRole::DetailModifier; }
        EVoxelOpChannelMask ChannelReads() const override { return VoxelOpChannels::Sdf; }
        EVoxelOpChannelMask ChannelWrites() const override { return VoxelOpChannels::Sdf; }
        bool IsAdditive() const override { return false; } // gate depends on the SDF it writes
        void PrepareChunk(const FVoxelOpContext&) override {}

        void Eval(float WorldX, float WorldY, float WorldZ, FVoxelOpSample& InOut) const override
        {
            if (Strength <= 0.0f || InOut.Sdf >= ApplyWithin) { return; }

            // Keep hot noise coordinates in float. The old FVector round-trip only preserved
            // legacy rounding; output compatibility with that field is intentionally abandoned.
            const FVector3f NoisePos(WorldX * Frequency, WorldY * Frequency, WorldZ * Frequency);
            InOut.Sdf += VoxelNoise::FBM(NoisePos.X, NoisePos.Y, NoisePos.Z,
                                         VoxelGenLOD::Eff(BaseOctaves), 2.0f, 0.5f)
                       * VOXEL_NOISE_SCALE * Strength;
        }

        // This modifier does not touch density itself. Its interval update is the only box-query
        // responsibility; the downstream converter consumes the resulting interval.
        EVoxelOpEffect EffectOverBox(const FBox&, const FVoxelOpContext&) const override
        {
            return EVoxelOpEffect::Identity;
        }

        void PropagateSdfOverBox(FVoxelBoxSdfInterval& InOut, const FBox& VoxelBox,
                                 const FVoxelOpContext&) const override
        {
            if (!InOut.IsKnown()) { return; }
            if (!VoxelMath::IsFinite(Strength) || !VoxelMath::IsFinite(Frequency)
                || !VoxelMath::IsFinite(ApplyWithin) || BaseOctaves <= 0)
            {
                InOut.SetUnknown();
                return;
            }
            if (Strength <= 0.0f) { return; }

            // If an authored frequency/box product overflows, Eval can publish a non-finite SDF
            // and no finite interval is a proof. Unknown costs the skip and protects the geometry.
            const float MaxAbsCoord = FMath::Max3(
                FMath::Max(FMath::Abs((float)VoxelBox.Min.X), FMath::Abs((float)VoxelBox.Max.X)),
                FMath::Max(FMath::Abs((float)VoxelBox.Min.Y), FMath::Abs((float)VoxelBox.Max.Y)),
                FMath::Max(FMath::Abs((float)VoxelBox.Min.Z), FMath::Abs((float)VoxelBox.Max.Z)));
            if (!VoxelMath::IsFinite(MaxAbsCoord) || !VoxelMath::IsFinite(MaxAbsCoord * FMath::Abs(Frequency)))
            {
                InOut.SetUnknown();
                return;
            }

            // The gate is `Sdf < ApplyWithin`. If the whole input interval is outside it, the
            // modifier is exactly the identity. Otherwise add the proven FBM envelope to both
            // sides; conditional untouched points are still covered by [Min, Max] + envelope.
            if (InOut.Min >= ApplyWithin) { return; }

            const float Amplitude = FMath::Abs(Strength) * VOXEL_NOISE_SCALE * VF_PerlinAbsBound;
            InOut.Min = VF_SaturatingAdd(InOut.Min, -Amplitude);
            InOut.Max = VF_SaturatingAdd(InOut.Max,  Amplitude);
        }

    private:
        float Strength, Frequency;
        int32 BaseOctaves;
        float ApplyWithin;
    };

    //=========================================================================
    // RÔLE 3 — MODIFIER : COLONNES SUR GRILLE MONDE / WORLD-GRID COLUMNS
    //=========================================================================
    // ÉTAPE 4 de `GetSlabDensity`. Des cylindres de hauteur infinie posés sur une grille de
    // `ColumnSpacing`, un tirage d'existence et un jitter par cellule. Le champ de vide décide déjà
    // où est le solide, donc la colonne n'a qu'à AJOUTER de la densité le long de son XY — elle
    // n'est visible que là où le vide avait creusé autour d'elle.
    //
    // Le cache 3×3 par cellule est repris tel quel (il était déjà `thread_local` dans l'original,
    // et c'est exactement ce que la note de threading de VoxelDensityOp.h autorise). Sa clé
    // contient tous les paramètres qui influent sur le résultat + le seed, donc un changement de
    // layout qui change un param invalide bien ; un changement qui n'en touche aucun produirait
    // des colonnes identiques (cf. AUDIT C2 — la clé est complète, pas seulement le coord).
    class FGridColumnMod final : public IVoxelDensityOp
    {
    public:
        explicit FGridColumnMod(const FSlabGenerationParams& P, int32 InSeed)
            : Seed((uint32)InSeed)
            , Spacing(P.ColumnSpacing)
            , ColDensity(P.ColumnDensity)
            , MinRadius(P.ColumnMinRadius)
            , MaxRadius(P.ColumnMaxRadius)
            , BaseDensity(P.BaseDensity)
        {}

        EVoxelOpRole GetRole() const override { return EVoxelOpRole::DetailModifier; }
        EVoxelOpChannelMask ChannelReads() const override { return VoxelOpChannels::Density; }
        EVoxelOpChannelMask ChannelWrites() const override { return VoxelOpChannels::Density; }
        bool IsAdditive() const override { return true; }
        void PrepareChunk(const FVoxelOpContext&) override {}
        bool IsXYPure() const override { return true; }   // cylindres de hauteur infinie

        void Eval(float WorldX, float WorldY, float, FVoxelOpSample& InOut) const override
        {
            if (ColDensity <= 0.0f || Spacing <= 0.0f) { return; }

            const int32 ColCX = FMath::FloorToInt(WorldX / Spacing);
            const int32 ColCY = FMath::FloorToInt(WorldY / Spacing);

            const TArray<FSlabColumn, TInlineAllocator<9>>& Cols = GetCells(ColCX, ColCY);

            float ColumnSDF = FLT_MAX;
            for (const FSlabColumn& Col : Cols)
            {
                const float DX2D = WorldX - Col.X;
                const float DY2D = WorldY - Col.Y;
                ColumnSDF = FMath::Min(ColumnSDF, FMath::Sqrt(DX2D * DX2D + DY2D * DY2D) - Col.R);
            }

            if (ColumnSDF < ColBlend && ColumnSDF < FLT_MAX)
            {
                float Fill = FMath::Clamp((ColBlend - ColumnSDF) / (ColBlend * 2.0f), 0.0f, 1.0f);
                Fill = SmoothStep01(Fill);
                InOut.Density += Fill * BaseDensity * 1.5f;
            }
        }

        // N'AJOUTE que du solide ⇒ tue AllAir, jamais AllSolid. `Identity` dès qu'aucune colonne
        // n'atteint la boîte — ce qui, pour un `ColumnDensity` de 0.08, est l'écrasante majorité du
        // volume. C'est cet `Identity` qui laisse survivre le verdict AllAir de la source.
        EVoxelOpEffect EffectOverBox(const FBox& VoxelBox, const FVoxelOpContext&) const override
        {
            if (ColDensity <= 0.0f || Spacing <= 0.0f) { return EVoxelOpEffect::Identity; }

            // Marge : l'enveloppe de `Lerp(MinRadius, MaxRadius, t)` est max(MinRadius, MaxRadius),
            // pas `MaxRadius` seul si l'asset inverse les paramètres. The bound must cover both
            // endpoints; using `MaxRadius` alone would leave a hole when the asset reverses them.
            const float Reach = FMath::Max3(MinRadius, MaxRadius, 0.0f) + ColBlend;

            const int32 CX0 = FMath::FloorToInt(((float)VoxelBox.Min.X - Reach) / Spacing);
            const int32 CX1 = FMath::FloorToInt(((float)VoxelBox.Max.X + Reach) / Spacing);
            const int32 CY0 = FMath::FloorToInt(((float)VoxelBox.Min.Y - Reach) / Spacing);
            const int32 CY1 = FMath::FloorToInt(((float)VoxelBox.Max.Y + Reach) / Spacing);

            for (int32 CY = CY0; CY <= CY1; ++CY)
            {
                for (int32 CX = CX0; CX <= CX1; ++CX)
                {
                    FSlabColumn Col;
                    if (!RollColumn(CX, CY, Col)) { continue; }

                    // Cercle (rayon + blend) contre le rectangle XY de la boîte.
                    const float R  = Col.R + ColBlend;
                    const float QX = FMath::Max(0.0f, FMath::Max((float)VoxelBox.Min.X - Col.X,
                                                                 Col.X - (float)VoxelBox.Max.X));
                    const float QY = FMath::Max(0.0f, FMath::Max((float)VoxelBox.Min.Y - Col.Y,
                                                                 Col.Y - (float)VoxelBox.Max.Y));
                    if (QX * QX + QY * QY < R * R) { return EVoxelOpEffect::FillOnly; }
                }
            }
            return EVoxelOpEffect::Identity;
        }

    private:
        struct FSlabColumn { float X, Y, R; };

        static constexpr float ColBlend = 2.0f;   // identique à GetSlabDensity

        /** Le tirage d'une cellule : existence, jitter, rayon. Fonction PURE de (cellule, seed,
         *  params) — donc `Eval` et `EffectOverBox` voient forcément la même colonne. */
        bool RollColumn(int32 CX, int32 CY, FSlabColumn& Out) const
        {
            const uint32 H = VoxelHash::Cell(CX, CY, Seed ^ 0xC01C01u);
            if (VoxelHash::ToFloat01(H) > ColDensity) { return false; }

            const float JX = VoxelHash::ToFloat01(VoxelHash::Mix(H ^ 0x12345678u));
            const float JY = VoxelHash::ToFloat01(VoxelHash::Mix(H ^ 0x9ABCDEF0u));

            Out.X = (CX + 0.15f + JX * 0.7f) * Spacing;
            Out.Y = (CY + 0.15f + JY * 0.7f) * Spacing;
            Out.R = FMath::Lerp(MinRadius, MaxRadius,
                                VoxelHash::ToFloat01(VoxelHash::Mix(H ^ 0xBEEFu)));
            return true;
        }

        /** Le voisinage 3×3 de la cellule centrale, mémoïsé par worker. */
        const TArray<FSlabColumn, TInlineAllocator<9>>& GetCells(int32 ColCX, int32 ColCY) const
        {
            thread_local TArray<FSlabColumn, TInlineAllocator<9>> SC_Cols;
            thread_local int32  SC_CX = INT32_MAX, SC_CY = INT32_MAX;
            thread_local uint32 SC_Seed = 0xFFFFFFFFu;
            thread_local float  SC_Spacing = -1.0f, SC_Dens = -1.0f, SC_MinR = -1.0f, SC_MaxR = -1.0f;

            if (ColCX != SC_CX || ColCY != SC_CY || Seed != SC_Seed || Spacing != SC_Spacing ||
                ColDensity != SC_Dens || MinRadius != SC_MinR || MaxRadius != SC_MaxR)
            {
                SC_CX = ColCX;  SC_CY = ColCY;  SC_Seed = Seed;  SC_Spacing = Spacing;
                SC_Dens = ColDensity;  SC_MinR = MinRadius;  SC_MaxR = MaxRadius;
                SC_Cols.Reset();

                for (int32 DY = -1; DY <= 1; DY++)
                {
                    for (int32 DX = -1; DX <= 1; DX++)
                    {
                        FSlabColumn Col;
                        if (RollColumn(ColCX + DX, ColCY + DY, Col)) { SC_Cols.Add(Col); }
                    }
                }
            }
            return SC_Cols;
        }

        uint32 Seed;
        float  Spacing, ColDensity, MinRadius, MaxRadius, BaseDensity;
    };

    //=========================================================================
    // RÔLE 2 — COMBINER : SDF → DENSITÉ (CARVE et FILL)
    //=========================================================================
    // Les six mêmes lignes dans TunnelNetwork, Maze, VerticalShafts — et FloatingIslands, où le
    // SEUL changement est `Density += Fill·Base·2` au lieu de `Density -= Carve·Base·2`.
    //
    // Un archétype qui CREUSE dans du roc et un archétype qui REMPLIT du vide sont donc le même
    // opérateur au signe près, exactement comme la source constante au-dessus. C'est la symétrie
    // que le `switch` ne pouvait pas montrer : les deux blocs y sont à 900 lignes l'un de l'autre.
    //
    // ⚠️ `Sign` vaut ±1.0f et rien d'autre. La multiplication par ±1 est EXACTE en IEEE-754, donc
    // `D += (-1·F)·B·2` rend bit pour bit ce que `D -= F·B·2` rendait — l'égalité binaire des trois
    // portages déjà verts en dépend.
    //
    // Same operator, opposite sign. Multiplying by ±1 is exact in IEEE-754, so the carve path is
    // bit-for-bit what it was before this generalisation — the three green ports depend on that.
    class FSdfConvertOp final : public IVoxelDensityOp
    {
    public:
        FSdfConvertOp(float InBlend, float InBaseDensity, float InSign, float InMinDivisor)
            : Blend(InBlend), BaseDensity(InBaseDensity), Sign(InSign), MinDivisor(InMinDivisor) {}

        EVoxelOpRole GetRole() const override { return EVoxelOpRole::Combiner; }
        EVoxelOpChannelMask ChannelReads() const override
        {
            return VoxelOpChannels::Density | VoxelOpChannels::Sdf;
        }
        EVoxelOpChannelMask ChannelWrites() const override { return VoxelOpChannels::Density; }
        bool IsAdditive() const override { return true; }
        void PrepareChunk(const FVoxelOpContext&) override {}

        void Eval(float, float, float, FVoxelOpSample& InOut) const override
        {
            if (InOut.Sdf >= Blend) { return; }
            // ⚠️ `MinDivisor` n'est PAS une précaution ajoutée : TunnelNetwork écrit
            // `/ FMath::Max(SDFBlendRadius * 2, 1.0f)` là où Maze/Shafts/Islands écrivent `/ (Blend*2)`.
            // Les deux formules DIVERGENT dès que `Blend·2 < 1`, donc les confondre serait une faute
            // de portage silencieuse. Avec `MinDivisor = 0` et un Blend positif, `Max(x, 0) == x`
            // exactement — les trois portages déjà verts ne bougent pas d'un bit.
            // Not a safety tweak: TunnelNetwork genuinely floors this divisor at 1 and the others
            // do not. Max(x, 0) is exactly x for positive Blend, so existing ports are untouched.
            float T = FMath::Clamp((Blend - InOut.Sdf) / FMath::Max(Blend * 2.0f, MinDivisor), 0.0f, 1.0f);
            T = SmoothStep01(T);
            InOut.Density += Sign * T * BaseDensity * 2.0f;   // interne : monter = vers le solide
        }

        EVoxelOpEffect EffectOverBox(const FBox&, const FVoxelOpContext&) const override
        {
            const float Coefficient = Sign * BaseDensity * 2.0f;
            if (!VoxelMath::IsFinite(Coefficient) || !VoxelMath::IsFinite(Blend)
                || !VoxelMath::IsFinite(MinDivisor) || Blend <= 0.0f)
            {
                return EVoxelOpEffect::Both;
            }
            if (Coefficient < 0.0f) { return EVoxelOpEffect::CarveOnly; }
            if (Coefficient > 0.0f) { return EVoxelOpEffect::FillOnly; }
            return EVoxelOpEffect::Identity;
        }

        EVoxelOpEffect EffectOverBox(const FBox& VoxelBox, const FVoxelOpContext& Ctx,
                                     const FVoxelBoxHypotheses& H) const override
        {
            const EVoxelOpEffect Intrinsic = EffectOverBox(VoxelBox, Ctx);
            if (Intrinsic == EVoxelOpEffect::Both || Intrinsic == EVoxelOpEffect::Identity)
            {
                return Intrinsic;
            }
            return H.Sdf.IsKnown() && H.Sdf.Min >= Blend
                 ? EVoxelOpEffect::Identity : Intrinsic;
        }

        float MaxCarveOverBox(const FBox&, const FVoxelOpContext&) const override
        {
            const float Coefficient = Sign * BaseDensity * 2.0f;
            if (!VoxelMath::IsFinite(Coefficient) || !VoxelMath::IsFinite(Blend)
                || !VoxelMath::IsFinite(MinDivisor) || Blend <= 0.0f)
            {
                return FLT_MAX;
            }
            return Coefficient < 0.0f ? FMath::Abs(Coefficient) : 0.0f;
        }

        float MaxFillOverBox(const FBox&, const FVoxelOpContext&) const override
        {
            const float Coefficient = Sign * BaseDensity * 2.0f;
            if (!VoxelMath::IsFinite(Coefficient) || !VoxelMath::IsFinite(Blend)
                || !VoxelMath::IsFinite(MinDivisor) || Blend <= 0.0f)
            {
                return FLT_MAX;
            }
            return Coefficient > 0.0f ? FMath::Abs(Coefficient) : 0.0f;
        }

        float MaxCarveOverBox(const FBox& VoxelBox, const FVoxelOpContext& Ctx,
                              const FVoxelBoxHypotheses& H) const override
        {
            const float Coefficient = Sign * BaseDensity * 2.0f;
            if (!VoxelMath::IsFinite(Coefficient)) { return FLT_MAX; }
            if (!VoxelMath::IsFinite(Blend) || !VoxelMath::IsFinite(MinDivisor) || Blend <= 0.0f)
            {
                return FLT_MAX;
            }
            if (Coefficient >= 0.0f) { return 0.0f; }
            return IsInactive(H) ? 0.0f : FMath::Abs(Coefficient) * MaxFactor(H);
        }

        float MaxFillOverBox(const FBox& VoxelBox, const FVoxelOpContext& Ctx,
                             const FVoxelBoxHypotheses& H) const override
        {
            const float Coefficient = Sign * BaseDensity * 2.0f;
            if (!VoxelMath::IsFinite(Coefficient)) { return FLT_MAX; }
            if (!VoxelMath::IsFinite(Blend) || !VoxelMath::IsFinite(MinDivisor) || Blend <= 0.0f)
            {
                return FLT_MAX;
            }
            if (Coefficient <= 0.0f) { return 0.0f; }
            return IsInactive(H) ? 0.0f : FMath::Abs(Coefficient) * MaxFactor(H);
        }

        const TCHAR* DebugName() const override { return TEXT("SdfConvertOp"); }

        void EvalBlock(const FVoxelOpBlock& Block) const override
        {
            VF_EvalBlockByScalar(*this, Block);
        }

    private:
        bool IsInactive(const FVoxelBoxHypotheses& H) const
        {
            return !H.Sdf.IsKnown() ? false : H.Sdf.Min >= Blend;
        }

        float MaxFactor(const FVoxelBoxHypotheses& H) const
        {
            if (!H.Sdf.IsKnown() || !VoxelMath::IsFinite(Blend)
                || !VoxelMath::IsFinite(MinDivisor) || Blend <= 0.0f)
            {
                return 1.0f;
            }

            const float Denom = FMath::Max(Blend * 2.0f, MinDivisor);
            if (!(Denom > 0.0f) || !VoxelMath::IsFinite(Denom)) { return 1.0f; }

            const float T = FMath::Clamp((Blend - H.Sdf.Min) / Denom, 0.0f, 1.0f);
            return SmoothStep01(T);
        }

        float Blend, BaseDensity, Sign, MinDivisor;
    };

    //=========================================================================
    // RÔLE 4 — STRUCTUREL : SPINE (0,0)
    //=========================================================================
    class FOriginSpineOp final : public IVoxelDensityOp
    {
    public:
        FOriginSpineOp(float InTopZ, float InBotZ, float InSeal, float InBase, float InRadius)
            : TopZ(InTopZ), BotZ(InBotZ), Seal(InSeal), Base(InBase), Radius(InRadius) {}

        EVoxelOpRole GetRole() const override { return EVoxelOpRole::StructuralPost; }
        EVoxelOpChannelMask ChannelReads() const override { return VoxelOpChannels::Density; }
        EVoxelOpChannelMask ChannelWrites() const override { return VoxelOpChannels::Density; }
        bool IsAdditive() const override { return true; }
        void PrepareChunk(const FVoxelOpContext&) override {}

        void Eval(float X, float Y, float Z, FVoxelOpSample& InOut) const override
        {
            VF_ApplyOriginSpine(InOut.Density, X, Y, Z, TopZ, BotZ, Seal, Base, Radius);
        }

        // The room kills AllSolid; its support slab also kills AllAir. Identity when the room
        // misses the box or the box is wholly outside this strate's safe interior.
        EVoxelOpEffect EffectOverBox(const FBox& VoxelBox,
                                     const FVoxelOpContext& Ctx) const override
        {
            if (VoxelDensityAblation::IsOriginSpineOff())
            {
                return EVoxelOpEffect::Identity;
            }
            const bool bRoomTouches = Ctx.bUseLatticeProof
                ? VoxelPassageGeometry::OriginLandingRoomTouchesLattice(
                    VoxelBox, Ctx.LatticeOriginVoxels, Ctx.Step,
                    TopZ, BotZ, Seal, Radius)
                : VoxelPassageGeometry::OriginLandingRoomTouchesBox(
                    VoxelBox, TopZ, BotZ, Seal, Radius);
            if (!bRoomTouches)
            {
                return EVoxelOpEffect::Identity;
            }
            const bool bFloorTouches = Ctx.bUseLatticeProof
                ? VoxelPassageGeometry::OriginLandingFloorTouchesLattice(
                    VoxelBox, Ctx.LatticeOriginVoxels, Ctx.Step,
                    TopZ, BotZ, Seal, Radius)
                : VoxelPassageGeometry::OriginLandingFloorTouchesBox(
                    VoxelBox, TopZ, BotZ, Seal, Radius);
            return bFloorTouches
                ? EVoxelOpEffect::Both : EVoxelOpEffect::CarveOnly;
        }

        // The landing carve interpolates from the incoming density toward an air target, so its
        // delta is unbounded without an input-density interval.  It is nevertheless exactly
        // inactive outside the geometric influence box; return zero there so an unrelated block
        // does not inherit the interface's conservative FLT_MAX sentinel.
        float MaxCarveOverBox(const FBox& VoxelBox, const FVoxelOpContext& Ctx) const override
        {
            const EVoxelOpEffect Effect = EffectOverBox(VoxelBox, Ctx);
            return (Effect == EVoxelOpEffect::CarveOnly || Effect == EVoxelOpEffect::Both)
                 ? FLT_MAX : 0.0f;
        }

        float MaxFillOverBox(const FBox& VoxelBox, const FVoxelOpContext& Ctx) const override
        {
            return EffectOverBox(VoxelBox, Ctx) == EVoxelOpEffect::Both ? FLT_MAX : 0.0f;
        }

        const TCHAR* DebugName() const override { return TEXT("OriginSpineOp"); }

        void EvalBlock(const FVoxelOpBlock& Block) const override
        {
            VF_EvalBlockByScalar(*this, Block);
        }

    private:
        float TopZ, BotZ, Seal, Base, Radius;
    };

    //=========================================================================
    // RÔLE 4 — STRUCTUREL : SEAL DE FRONTIÈRE (l'opérateur FORÇANT)
    //=========================================================================
    class FBoundarySealOp final : public IVoxelDensityOp
    {
    public:
        FBoundarySealOp(float InTopZ, float InBotZ, float InThickness, float InBase)
            : TopZ(InTopZ), BotZ(InBotZ), Thickness(InThickness), Base(InBase) {}

        EVoxelOpRole GetRole() const override { return EVoxelOpRole::StructuralPost; }
        EVoxelOpChannelMask ChannelReads() const override { return VoxelOpChannels::Density; }
        EVoxelOpChannelMask ChannelWrites() const override { return VoxelOpChannels::Density; }
        bool IsAdditive() const override { return false; }
        void PrepareChunk(const FVoxelOpContext&) override {}

        void Eval(float, float, float Z, FVoxelOpSample& InOut) const override
        {
            VF_ApplyBoundarySeal(InOut.Density, Z, TopZ, BotZ, Thickness, Base);
        }

        /**
         * Dans sa bande, le seal fait `Max(D, SealFactor·Base)` avec SealFactor > 0 : le résultat
         * est solide GARANTI quelle qu'ait été l'entrée. C'est un opérateur FORÇANT, et la raison
         * d'être de `ClassifyBox` (voir VoxelDensityOp.h).
         *
         * ⚠️ MARGE DE SÛRETÉ DÉLIBÉRÉE. Au bord INTÉRIEUR de la bande, `1 - Dist/Thickness` peut
         * arrondir à exactement 0.0f en float ; SealFactor·Base vaut alors 0, la densité interne
         * finit à 0, et le mesher (`D >= IsoLevel`) compte ce point du côté AIR. Prétendre AllSolid
         * là serait un TROU. On exige donc que la boîte soit dans la bande avec 1 voxel de marge
         * avant de forcer ; sinon on retombe sur le FillOnly, qui est toujours sûr.
         *
         * (Le `ClassifyTile` actuel n'a pas cette marge — il exclut simplement ces z du test de
         * colonne. La fenêtre est infime et demande que l'archétype produise de l'air pile à ce z,
         * mais elle est réelle.)
         */
        EVoxelTileClass ClassifyBox(const FBox& VoxelBox, const FVoxelOpContext&) const override
        {
            if (VoxelDensityAblation::IsBoundarySealOff())
            {
                return EVoxelTileClass::Mixed;
            }
            if (Thickness <= 0.0f || Base <= 0.0f) { return EVoxelTileClass::Mixed; }

            constexpr float SafetyMargin = 1.0f;
            const float Usable = Thickness - SafetyMargin;
            if (Usable <= 0.0f) { return EVoxelTileClass::Mixed; }

            const float MinDistTop = TopZ - (float)VoxelBox.Min.Z;   // plus petite distance au plafond
            const float MaxDistTop = TopZ - (float)VoxelBox.Max.Z;
            const bool bWhollyInTopBand = (MaxDistTop >= 0.0f) && (MinDistTop < Usable);

            const float MinDistBot = (float)VoxelBox.Min.Z - BotZ;
            const float MaxDistBot = (float)VoxelBox.Max.Z - BotZ;
            const bool bWhollyInBotBand = (MinDistBot >= 0.0f) && (MaxDistBot < Usable);

            return (bWhollyInTopBand || bWhollyInBotBand) ? EVoxelTileClass::AllSolid
                                                          : EVoxelTileClass::Mixed;
        }

        // Hors de sa bande, le seal ne fait rien du tout ; à cheval, il ne peut qu'ajouter du solide.
        EVoxelOpEffect EffectOverBox(const FBox& VoxelBox, const FVoxelOpContext&) const override
        {
            if (VoxelDensityAblation::IsBoundarySealOff())
            {
                return EVoxelOpEffect::Identity;
            }
            if (Thickness <= 0.0f) { return EVoxelOpEffect::Identity; }
            const bool bTouchesTopBand = ((float)VoxelBox.Max.Z >= TopZ - Thickness) && ((float)VoxelBox.Min.Z <= TopZ);
            const bool bTouchesBotBand = ((float)VoxelBox.Min.Z <= BotZ + Thickness) && ((float)VoxelBox.Max.Z >= BotZ);
            if (!bTouchesTopBand && !bTouchesBotBand) { return EVoxelOpEffect::Identity; }
            return EVoxelOpEffect::FillOnly;
        }

        // A partial seal can raise an arbitrarily negative incoming density, so its fill delta is
        // genuinely unbounded through this API.  Outside both bands it is an exact identity.
        float MaxCarveOverBox(const FBox&, const FVoxelOpContext&) const override { return 0.0f; }

        float MaxFillOverBox(const FBox& VoxelBox, const FVoxelOpContext& Ctx) const override
        {
            return EffectOverBox(VoxelBox, Ctx) == EVoxelOpEffect::Identity ? 0.0f : FLT_MAX;
        }

        const TCHAR* DebugName() const override { return TEXT("BoundarySealOp"); }

        void EvalBlock(const FVoxelOpBlock& Block) const override
        {
            VF_EvalBlockByScalar(*this, Block);
        }

    private:
        float TopZ, BotZ, Thickness, Base;
    };

    //=========================================================================
    // RÔLE 4 — STRUCTUREL : SCELLEMENT DE LA LIMITE XY (opérateur FORÇANT)
    //=========================================================================
    class FXYEdgeSealOp final : public IVoxelDensityOp
    {
    public:
        explicit FXYEdgeSealOp(float InBase)
            : Base(InBase) {}

        EVoxelOpRole GetRole() const override { return EVoxelOpRole::StructuralPost; }
        EVoxelOpChannelMask ChannelReads() const override { return VoxelOpChannels::Density; }
        EVoxelOpChannelMask ChannelWrites() const override { return VoxelOpChannels::Density; }
        bool IsAdditive() const override { return false; }

        void PrepareChunk(const FVoxelOpContext& Ctx) override
        {
            // The settings are global, but PrepareChunk is the stack's one per-chunk hand-off.
            // Keeping the prepared copy makes Eval hot and keeps the op independent of the asset.
            WorldRadius = Ctx.WorldRadiusVoxels;
            Thickness    = Ctx.EdgeSealThickness;
        }

        void Eval(float X, float Y, float, FVoxelOpSample& InOut) const override
        {
            VF_ApplyXYEdgeSeal(InOut.Density, X, Y, WorldRadius, Thickness, Base);
        }

        /**
         * The seal is forcing, not merely FillOnly: once every point in the box is at least one
         * voxel into the ramp (or beyond the radius), the result is positive-solid regardless of
         * all preceding source/carve operators.
         */
        EVoxelTileClass ClassifyBox(const FBox& VoxelBox, const FVoxelOpContext& Ctx) const override
        {
            if (VoxelDensityAblation::IsXYEdgeSealOff())
            {
                return EVoxelTileClass::Mixed;
            }
            return VF_XYEdgeSealBoxIsForcedSolid(
                VoxelBox, Ctx.WorldRadiusVoxels, Ctx.EdgeSealThickness, Base)
                ? EVoxelTileClass::AllSolid : EVoxelTileClass::Mixed;
        }

        float ForcedMarginOverBox(const FBox& VoxelBox, const FVoxelOpContext& Ctx) const override
        {
            return VF_XYEdgeSealForcedMarginOverBox(
                VoxelBox, Ctx.WorldRadiusVoxels, Ctx.EdgeSealThickness, Base);
        }

        // In the inner world it is Identity; a box that reaches the radial band can only gain rock.
        EVoxelOpEffect EffectOverBox(const FBox& VoxelBox, const FVoxelOpContext& Ctx) const override
        {
            if (VoxelDensityAblation::IsXYEdgeSealOff())
            {
                return EVoxelOpEffect::Identity;
            }
            return VF_XYEdgeSealBoxTouchesBand(
                VoxelBox, Ctx.WorldRadiusVoxels, Ctx.EdgeSealThickness)
                 ? EVoxelOpEffect::FillOnly : EVoxelOpEffect::Identity;
        }

        float MaxCarveOverBox(const FBox&, const FVoxelOpContext&) const override { return 0.0f; }

        float MaxFillOverBox(const FBox& VoxelBox, const FVoxelOpContext& Ctx) const override
        {
            return EffectOverBox(VoxelBox, Ctx) == EVoxelOpEffect::Identity ? 0.0f : FLT_MAX;
        }

        bool IsXYPure() const override { return true; }
        const TCHAR* DebugName() const override { return TEXT("XYEdgeSealOp"); }

        void EvalBlock(const FVoxelOpBlock& Block) const override
        {
            VF_EvalBlockByScalar(*this, Block);
        }

    private:
        float Base = 8.0f;
        mutable float WorldRadius = 8192.0f;
        mutable float Thickness = 64.0f;
    };

    //=========================================================================
    // RÔLE 4 — STRUCTUREL : CARVE DE PASSAGE
    //=========================================================================
    class FPassageCarveOp final : public IVoxelDensityOp
    {
    public:
        FPassageCarveOp(const UVoxelStrateManager* InManager, float InBase, float InSeal,
                        float InTopZ, float InBotZ, float InSpineRadius)
            : Manager(InManager), Base(InBase), Seal(InSeal),
              TopZ(InTopZ), BotZ(InBotZ), SpineRadius(InSpineRadius) {}

        EVoxelOpRole GetRole() const override { return EVoxelOpRole::StructuralPost; }
        EVoxelOpChannelMask ChannelReads() const override { return VoxelOpChannels::Density; }
        EVoxelOpChannelMask ChannelWrites() const override { return VoxelOpChannels::Density; }
        bool IsAdditive() const override { return false; }
        void PrepareChunk(const FVoxelOpContext&) override {}

        void Eval(float X, float Y, float Z, FVoxelOpSample& InOut) const override
        {
            if (const UVoxelStrateManager* LiveManager = Manager.Get())
            {
                // Landing/tunnel support is a shared MC-space post after disturbances. Calling the
                // legacy full modifier here repeats those same writers before the shared post.
                LiveManager->ApplyPassageCarvingOnly(InOut.Density, X, Y, Z, Base, Seal);
            }
            // Keep the source and op-stack operation order identical: the origin room is carved
            // before the boundary seal, then its support floor is reasserted after the passage
            // post so no passage-air post can remove the standing surface.
            VF_ApplyOriginLandingFloor(
                InOut.Density, X, Y, Z, TopZ, BotZ, Seal, Base, SpineRadius);
        }

        // ≡ la garde `AnyPassageNearBox` écrite à la main dans ClassifyTile — déjà écrite, ici
        // simplement branchée au bon endroit au lieu d'être un cas particulier du classifieur.
        EVoxelOpEffect EffectOverBox(const FBox& VoxelBox,
                                     const FVoxelOpContext& Ctx) const override
        {
            if (VoxelDensityAblation::IsPassageCarvingOff()
                && VoxelDensityAblation::IsLandingPostsOff()
                && VoxelDensityAblation::IsPassageStructuralPostsOff()
                && VoxelDensityAblation::IsOriginSpineOff())
            {
                return EVoxelOpEffect::Identity;
            }
            const UVoxelStrateManager* LiveManager = Manager.Get();
            if (!LiveManager) { return EVoxelOpEffect::Identity; }
            const bool bOriginFloor = !VoxelDensityAblation::IsLandingPostsOff()
                && (Ctx.bUseLatticeProof
                    ? VoxelPassageGeometry::OriginLandingFloorTouchesLattice(
                        VoxelBox, Ctx.LatticeOriginVoxels, Ctx.Step,
                        TopZ, BotZ, Seal, SpineRadius)
                    : VoxelPassageGeometry::OriginLandingFloorTouchesBox(
                        VoxelBox, TopZ, BotZ, Seal, SpineRadius));
            const bool bPassageFloor = !VoxelDensityAblation::IsLandingPostsOff()
                && (Ctx.bUseLatticeProof
                    ? LiveManager->AnyPassageLandingFloorNearLattice(
                        VoxelBox, Ctx.LatticeOriginVoxels, Ctx.Step)
                    : LiveManager->AnyPassageLandingFloorNearBox(
                        VoxelBox.Min, VoxelBox.Max));
            if (bOriginFloor || bPassageFloor)
            {
                // The room carves air, while its support slab force-writes solid. Both
                // hypotheses must therefore be killed for a box touching that slab.
                return EVoxelOpEffect::Both;
            }
            const bool bPassageCarve = !VoxelDensityAblation::IsPassageCarvingOff()
                && (Ctx.bUseLatticeProof
                    ? LiveManager->AnyPassageNearLattice(
                        VoxelBox, Ctx.LatticeOriginVoxels, Ctx.Step)
                    : LiveManager->AnyPassageNearBox(VoxelBox.Min, VoxelBox.Max));
            return bPassageCarve
                 ? EVoxelOpEffect::CarveOnly : EVoxelOpEffect::Identity;
        }

        // The current-density Lerp/Min passage carve has no finite delta bound for an arbitrary
        // incoming density.  Once the preceding stack has proved a positive solid margin,
        // however, the monotone carve can be bounded exactly on the MC lattice: evaluate the
        // largest carve factor, apply it to the lower input bound, and keep the resulting positive
        // margin.  This preserves the old conservative fallback for non-lattice callers.
        float MaxCarveOverBox(const FBox& VoxelBox, const FVoxelOpContext& Ctx,
                              const FVoxelBoxHypotheses& H) const override
        {
            if (VoxelDensityAblation::IsPassageCarvingOff())
            {
                return 0.0f;
            }
            const UVoxelStrateManager* LiveManager = Manager.Get();
            if (!LiveManager) { return 0.0f; }
            if (!Ctx.bUseLatticeProof)
            {
                const EVoxelOpEffect Effect = EffectOverBox(VoxelBox, Ctx);
                return (Effect == EVoxelOpEffect::CarveOnly || Effect == EVoxelOpEffect::Both)
                     ? FLT_MAX : 0.0f;
            }

            const float MaxFactor = LiveManager->MaxPassageCarveFactorNearLattice(
                VoxelBox, Ctx.LatticeOriginVoxels, Ctx.Step);
            if (!(MaxFactor > 0.0f)) { return 0.0f; }
            if (!(H.SolidMargin > 0.0f) || !VoxelMath::IsFinite(H.SolidMargin)
                || !VoxelMath::IsFinite(Base) || !VoxelMath::IsFinite(Seal))
            {
                return FLT_MAX;
            }

            const float AirTarget = -(Base * 2.0f + Seal + 4.0f);
            if (!VoxelMath::IsFinite(AirTarget)) { return FLT_MAX; }
            const float LowerAfterCarve = FMath::Min(
                H.SolidMargin,
                FMath::Lerp(H.SolidMargin, AirTarget, FMath::Clamp(MaxFactor, 0.0f, 1.0f)));
            if (!VoxelMath::IsFinite(LowerAfterCarve)) { return FLT_MAX; }
            return FMath::Max(0.0f, H.SolidMargin - LowerAfterCarve);
        }

        float MaxFillOverBox(const FBox& VoxelBox, const FVoxelOpContext& Ctx) const override
        {
            return EffectOverBox(VoxelBox, Ctx) == EVoxelOpEffect::Both ? FLT_MAX : 0.0f;
        }

        const TCHAR* DebugName() const override { return TEXT("PassageCarveOp"); }

        void EvalBlock(const FVoxelOpBlock& Block) const override
        {
            VF_EvalBlockByScalar(*this, Block);
        }

    private:
        TWeakObjectPtr<const UVoxelStrateManager> Manager;
        float Base, Seal, TopZ, BotZ, SpineRadius;
    };

    //=========================================================================
    // RÔLE 1 — SOURCE : PUITS VERTICAUX / VERTICAL SHAFTS
    //=========================================================================
    // Cylindres infinis sur une grille XY jitterée + un arbre de drainage déterministe, complété
    // par des connecteurs horizontaux entre paires proches ouverts par un hash de paire. Écrit le
    // canal SDF uniquement.
    //
    // ⚠️ ÉCART ASSUMÉ AVEC `OPSTACK-DECOMPOSITION §6`, qui suggérait DEUX sources (colonnes XY-pures
    // + connecteurs) pour que la moitié cylindrique reçoive le traitement du cache de colonne et un
    // `ClassifyBox` exact en XY. Gardé en UN opérateur, et voici pourquoi :
    //   • les connecteurs se dérivent de la MÊME liste inner 3×3 que les puits (il faut les paires), donc
    //     séparer imposerait soit de rouler les cellules deux fois, soit un cache partagé entre
    //     deux ops — c'est-à-dire la complexité qu'on voulait éviter ;
    //   • le `FShaftLedgeMod` en aval a de toute façon besoin de la liste des puits, donc il faut
    //     l'exposer depuis une source ; l'exposer depuis deux serait pire.
    // Ce qui est perdu : le verdict de boîte exact sur la seule moitié cylindrique. Le compromis
    // actuel est une propagation d'intervalle qui couvre cercles ET capsules, tandis que le
    // `EffectOverBox` intrinsèque reste Identity : le convertisseur ou consommateur aval décide
    // séparément de son propre effet.
    //
    // Kept as ONE op against §6's suggestion: the per-voxel connectors derive from the same inner
    // 3×3 roll as the shafts, while a rebuild-only 9×9 collection resolves each 5×5 tree window
    // and fixed ±3 fallback;
    // the downstream ledge mod needs the inner shaft list anyway. What is forfeited is an exact XY
    // box verdict on the cylinder half alone.
    class FShaftFieldSource final : public IVoxelDensityOp
    {
    public:
        FShaftFieldSource(const FVerticalShaftParams& InP, int32 Seed, float InSpineRadius)
            : P(InP), Salt((uint32)Seed ^ 0x53686674u)   // 'Shft' — identique à GetVerticalShaftDensity
            , SpineRadius(InSpineRadius) {}

        EVoxelOpRole GetRole() const override { return EVoxelOpRole::FieldSource; }
        EVoxelOpChannelMask ChannelReads() const override { return VoxelOpChannels::None; }
        EVoxelOpChannelMask ChannelWrites() const override { return VoxelOpChannels::Sdf; }
        EVoxelOpResourceMask ProvidedResources() const override { return VoxelOpResources::ShaftGeometry; }
        bool IsAdditive() const override { return false; }
        void PrepareChunk(const FVoxelOpContext&) override {}

        struct FShaft { float X, Y, R; int32 CellX, CellY; bool bOriginSpine; };
        struct FConn
        {
            FVector A, B;
            float Radius = 0.0f;
            float FloorZ = 0.0f;
            bool bWalkableFloor = false;
        };

        // ⚠️ DÉCLARÉE ICI, avant toute fonction qui la renvoie. Un type imbriqué doit exister au
        // moment où le COMPILATEUR lit la SIGNATURE — les corps de méthodes sont différés, pas les
        // types de retour. La mettre en bas de la classe donne un C4430 « int par défaut » suivi
        // d'une cascade illisible, ce qui masque une cause pourtant triviale.
        // Declared here, before any function returning it: a nested type must exist when the
        // compiler reads the SIGNATURE — bodies are deferred, return types are not.
        struct FCells
        {
            TArray<FShaft, TInlineAllocator<10>> Shafts;
            TArray<FConn,  TInlineAllocator<32>> Conns;
        };

        void Eval(float WorldX, float WorldY, float WorldZ, FVoxelOpSample& InOut) const override
        {
            const FCells& C = GetCells(WorldX, WorldY);

            float CaveSDF = FLT_MAX;
            for (const FShaft& Sh : C.Shafts)
            {
                if (Sh.bOriginSpine) continue;  // VF_ApplyOriginSpine owns the structural column.
                const float DX = WorldX - Sh.X;
                const float DY = WorldY - Sh.Y;
                CaveSDF = FMath::Min(CaveSDF, FMath::Sqrt(DX * DX + DY * DY) - Sh.R);
            }
            const FVector Pos(WorldX, WorldY, WorldZ);
            for (const FConn& Cn : C.Conns)
            {
                CaveSDF = FMath::Min(CaveSDF, VoxelSDF::Capsule(Pos, Cn.A, Cn.B, Cn.Radius));
            }
            InOut.Sdf = CaveSDF;
        }

        /** La liste des puits proches — `FShaftLedgeMod` doit trouver le PLUS PROCHE pour ne
         *  poser d'étagère que sur sa moitié +X/+Y. Même motif que colonne → overhang. */
        const FCells& GetCellsAt(float WorldX, float WorldY) const { return GetCells(WorldX, WorldY); }

        EVoxelOpEffect EffectOverBox(const FBox&, const FVoxelOpContext&) const override
        {
            // This source writes SDF only. A converter or other SDF consumer must inspect the
            // propagated interval; the source has no density effect in isolation.
            return EVoxelOpEffect::Identity;
        }

        /**
         * Propagate the source's own SDF interval. Eval selects the inner 3x3 cell neighbourhood,
         * so enumerating every cell touched by the box and asking GetCells at that cell centre is an
         * exact superset. Shaft axes are infinite in Z; connector capsules use their segment AABB
         * as a conservative distance lower bound.
         */
        void PropagateSdfOverBox(FVoxelBoxSdfInterval& InOut, const FBox& VoxelBox,
                                 const FVoxelOpContext&) const override
        {
            if (!VoxelMath::IsFinite((float)VoxelBox.Min.X) || !VoxelMath::IsFinite((float)VoxelBox.Min.Y)
                || !VoxelMath::IsFinite((float)VoxelBox.Min.Z)
                || !VoxelMath::IsFinite((float)VoxelBox.Max.X) || !VoxelMath::IsFinite((float)VoxelBox.Max.Y)
                || !VoxelMath::IsFinite((float)VoxelBox.Max.Z)
                || VoxelBox.Min.X > VoxelBox.Max.X || VoxelBox.Min.Y > VoxelBox.Max.Y
                || VoxelBox.Min.Z > VoxelBox.Max.Z
                || !VoxelMath::IsFinite(P.ShaftSpacing) || P.ShaftSpacing <= 0.0f)
            {
                InOut.SetUnknown();
                return;
            }

            const float Spacing = FMath::Max(P.ShaftSpacing, 1.0f);
            const int32 CX0 = FMath::FloorToInt((float)VoxelBox.Min.X / Spacing);
            const int32 CY0 = FMath::FloorToInt((float)VoxelBox.Min.Y / Spacing);
            const int32 CX1 = FMath::FloorToInt((float)VoxelBox.Max.X / Spacing);
            const int32 CY1 = FMath::FloorToInt((float)VoxelBox.Max.Y / Spacing);
            constexpr int64 MaxCellsScanned = 64 * 64;
            const int64 CellCount = ((int64)CX1 - CX0 + 1) * ((int64)CY1 - CY0 + 1);
            if (CellCount <= 0 || CellCount > MaxCellsScanned)
            {
                InOut.SetUnknown();
                return;
            }

            float Lower = FLT_MAX;
            bool bAnyPrimitive = false;
            for (int32 cy = CY0; cy <= CY1; ++cy)
            for (int32 cx = CX0; cx <= CX1; ++cx)
            {
                const FCells& C = GetCells((cx + 0.5f) * Spacing, (cy + 0.5f) * Spacing);
                for (const FShaft& Sh : C.Shafts)
                {
                    if (Sh.bOriginSpine) { continue; }
                    bAnyPrimitive = true;
                    Lower = FMath::Min(Lower,
                        VF_DistanceBoxToPointXY(VoxelBox, Sh.X, Sh.Y) - FMath::Abs(Sh.R));
                }
                for (const FConn& Conn : C.Conns)
                {
                    bAnyPrimitive = true;
                    const FVector ConnMin(
                        FMath::Min(Conn.A.X, Conn.B.X), FMath::Min(Conn.A.Y, Conn.B.Y),
                        FMath::Min(Conn.A.Z, Conn.B.Z));
                    const FVector ConnMax(
                        FMath::Max(Conn.A.X, Conn.B.X), FMath::Max(Conn.A.Y, Conn.B.Y),
                        FMath::Max(Conn.A.Z, Conn.B.Z));
                    Lower = FMath::Min(Lower,
                        VF_DistanceBetweenBoxes(VoxelBox.Min, VoxelBox.Max, ConnMin, ConnMax)
                        - FMath::Abs(Conn.Radius));
                }
            }

            if (bAnyPrimitive) { InOut.Set(Lower, FLT_MAX); }
            else               { InOut.Set(FLT_MAX, FLT_MAX); }
        }

    private:
        /** Tirage d'une cellule. PURE en (cellule, seed, params) ⇒ `Eval` et `EffectOverBox` ne
         *  peuvent pas voir des puits différents. */
        bool RollShaft(int32 nx, int32 ny, FShaft& Out) const
        {
            const float Spacing = FMath::Max(P.ShaftSpacing, 1.0f);
            const uint32 Hh = VoxelHash::Cell(nx, ny, Salt);
            if (VoxelHash::ToFloat01(Hh) > P.ShaftDensity) { return false; }

            const float JX = VoxelHash::ToFloat01(VoxelHash::Mix(Hh ^ 0x12345678u));
            const float JY = VoxelHash::ToFloat01(VoxelHash::Mix(Hh ^ 0x9ABCDEF0u));
            Out.X = (nx + 0.15f + JX * 0.7f) * Spacing;
            Out.Y = (ny + 0.15f + JY * 0.7f) * Spacing;
            Out.R = FMath::Lerp(P.ShaftMinRadius, P.ShaftMaxRadius,
                                VoxelHash::ToFloat01(VoxelHash::Mix(Hh ^ 0xBEEFu)));
            Out.CellX = nx;
            Out.CellY = ny;
            Out.bOriginSpine = false;
            return true;
        }

        /** Spine links use a radius above the repository's proven sup|FBM|=1.5 roughness
         * envelope, so the roughness pass cannot pinch their centreline shut. */
        float SpineConnectorRadius() const
        {
            constexpr float RoughnessAbsBound = 1.5f;
            const float RoughnessReach = FMath::Max(P.SurfaceRoughness, 0.0f)
                                       * VOXEL_NOISE_SCALE * RoughnessAbsBound;
            return FMath::Max(P.ConnectorRadius, RoughnessReach + 1.0f);
        }

        /** Le voisinage inner 3×3 + ses connecteurs, mémoïsés par worker. Le rebuild collecte un
         *  halo 9×9 émetteur / 15×15 total pour résoudre les fenêtres parent 5×5 et le fallback fixe ±3. Clé = cellule + tous les params
         *  qui influent (comme l'original) : stable à travers les reconstructions de pile, ce qui
         *  est la leçon retenue du mémo de colonne de SurfaceWorld. */
        const FCells& GetCells(float WorldX, float WorldY) const
        {
            const float Spacing = FMath::Max(P.ShaftSpacing, 1.0f);
            const int32 CX = FMath::FloorToInt(WorldX / Spacing);
            const int32 CY = FMath::FloorToInt(WorldY / Spacing);

            thread_local FCells  Cache;
            thread_local int32   VS_CX = INT32_MAX, VS_CY = INT32_MAX;
            thread_local uint32  VS_Salt = 0xFFFFFFFFu;
            thread_local float   VS_Spacing = -1.0f, VS_Dens = -1.0f, VS_MinR = -1.0f,
                                 VS_MaxR = -1.0f, VS_Cross = -1.0f, VS_ConnR = -1.0f,
                                 VS_Rough = -1.0f, VS_SpineR = -1.0f,
                                 VS_BotZ = FLT_MAX, VS_TopZ = FLT_MAX, VS_Seal = -1.0f,
                                 VS_LedgeSpacing = -1.0f, VS_LedgeDepth = -1.0f;

            if (CX != VS_CX || CY != VS_CY || Salt != VS_Salt || Spacing != VS_Spacing ||
                P.ShaftDensity != VS_Dens || P.ShaftMinRadius != VS_MinR || P.ShaftMaxRadius != VS_MaxR ||
                P.CrossConnectChance != VS_Cross || P.ConnectorRadius != VS_ConnR ||
                P.SurfaceRoughness != VS_Rough || SpineRadius != VS_SpineR ||
                P.StrateBottomWorldZ != VS_BotZ || P.StrateTopWorldZ != VS_TopZ ||
                P.BoundarySealThickness != VS_Seal || P.LedgeSpacing != VS_LedgeSpacing ||
                P.LedgeDepth != VS_LedgeDepth)
            {
                VS_CX = CX;  VS_CY = CY;  VS_Salt = Salt;  VS_Spacing = Spacing;
                VS_Dens = P.ShaftDensity;  VS_MinR = P.ShaftMinRadius;  VS_MaxR = P.ShaftMaxRadius;
                VS_Cross = P.CrossConnectChance;  VS_ConnR = P.ConnectorRadius;
                VS_Rough = P.SurfaceRoughness;    VS_SpineR = SpineRadius;
                VS_BotZ = P.StrateBottomWorldZ;  VS_TopZ = P.StrateTopWorldZ;
                VS_Seal = P.BoundarySealThickness;
                VS_LedgeSpacing = P.LedgeSpacing; VS_LedgeDepth = P.LedgeDepth;
                Cache.Shafts.Reset();
                Cache.Conns.Reset();

                constexpr int32 EmitRadius = 1;      // 3×3: shaft cylinders and random links
                constexpr int32 CandidateRadius = 2; // 5×5 parent window around an emitting shaft
                constexpr int32 FallbackRadius = 3;  // fixed fallback window around an emitting shaft
                constexpr uint32 TreeSalt = 0x7A11u;

                const float BottomZ = P.StrateBottomWorldZ + P.BoundarySealThickness;
                const float TopZ    = P.StrateTopWorldZ    - P.BoundarySealThickness;
                const float TreeRadius = SpineConnectorRadius();
                // A tree capsule can affect a cell even when neither endpoint is in that cell.
                // Keep a rebuild-only child halo for that geometric reach, then add the
                // parent-search halo. With the defaults this is 4 + 3 cells = 15×15 rolls.
                const int32 ConnectorCellPad = FMath::Max(
                    1, FMath::CeilToInt(TreeRadius / Spacing));
                const int32 TreeEmitRadius = FallbackRadius + ConnectorCellPad;
                const int32 CollectRadius = TreeEmitRadius + FallbackRadius;

                const int32 CollectSide = CollectRadius * 2 + 1;
                TArray<FShaft, TInlineAllocator<225>> ShaftGrid;
                TArray<uint8, TInlineAllocator<225>> ShaftPresent;
                TArray<FShaft, TInlineAllocator<81>> TreeEmitShafts;
                ShaftGrid.SetNum(CollectSide * CollectSide);
                ShaftPresent.Init(0, CollectSide * CollectSide);
                // Rebuild-only wide collection. `Cache.Shafts` remains the inner 3×3 working set
                // used by Eval and the ledge modifier; the wider tree work is not paid per voxel.
                for (int32 dy = -CollectRadius; dy <= CollectRadius; dy++)
                for (int32 dx = -CollectRadius; dx <= CollectRadius; dx++)
                {
                    FShaft Sh;
                    if (!RollShaft(CX + dx, CY + dy, Sh)) { continue; }
                    const int32 GridIndex = (dy + CollectRadius) * CollectSide + (dx + CollectRadius);
                    ShaftGrid[GridIndex] = Sh;
                    ShaftPresent[GridIndex] = 1;
                    if (FMath::Abs(dx) <= EmitRadius && FMath::Abs(dy) <= EmitRadius)
                    {
                        Cache.Shafts.Add(Sh);
                    }
                    if (FMath::Abs(dx) <= TreeEmitRadius && FMath::Abs(dy) <= TreeEmitRadius)
                    {
                        TreeEmitShafts.Add(Sh);
                    }
                }

                auto FindCollectedShaft = [&](int32 CellX, int32 CellY) -> const FShaft*
                {
                    const int32 dx = CellX - CX;
                    const int32 dy = CellY - CY;
                    if (FMath::Abs(dx) > CollectRadius || FMath::Abs(dy) > CollectRadius)
                    {
                        return nullptr;
                    }
                    const int32 GridIndex = (dy + CollectRadius) * CollectSide + (dx + CollectRadius);
                    return ShaftPresent[GridIndex] ? &ShaftGrid[GridIndex] : nullptr;
                };

                const bool bOriginNearby = FMath::Abs(CX) <= 1 && FMath::Abs(CY) <= 1;
                if (bOriginNearby && SpineRadius > 0.0f)
                {
                    Cache.Shafts.Add({0.0f, 0.0f, SpineRadius, 0, 0, true});
                }

                const float CellMinX = static_cast<float>(CX) * Spacing;
                const float CellMaxX = static_cast<float>(CX + 1) * Spacing;
                const float CellMinY = static_cast<float>(CY) * Spacing;
                const float CellMaxY = static_cast<float>(CY + 1) * Spacing;
                auto ConnectorMayReachCell = [&](const FConn& Conn)
                {
                    const float MinX = FMath::Min(Conn.A.X, Conn.B.X) - Conn.Radius;
                    const float MaxX = FMath::Max(Conn.A.X, Conn.B.X) + Conn.Radius;
                    const float MinY = FMath::Min(Conn.A.Y, Conn.B.Y) - Conn.Radius;
                    const float MaxY = FMath::Max(Conn.A.Y, Conn.B.Y) + Conn.Radius;
                    const float GapX = FMath::Max3(CellMinX - MaxX, MinX - CellMaxX, 0.0f);
                    const float GapY = FMath::Max3(CellMinY - MaxY, MinY - CellMaxY, 0.0f);
                    return GapX == 0.0f && GapY == 0.0f;
                };

                auto EmitTreeConnector = [&](const FShaft& Child, const FShaft* Parent)
                {
                    const float Zc = VoxelPassageGeometry::VerticalShaftConnectorCenterZ(
                        P.StrateBottomWorldZ,
                        BottomZ,
                        TopZ,
                        P.LedgeSpacing,
                        P.LedgeDepth,
                        TreeRadius);
                    const float FloorZ = VoxelPassageGeometry::VerticalShaftConnectorFloorZ(
                        P.StrateBottomWorldZ,
                        BottomZ,
                        TopZ,
                        P.LedgeSpacing,
                        P.LedgeDepth,
                        TreeRadius);
                    const FVector ParentPoint = Parent != nullptr
                        ? FVector(Parent->X, Parent->Y, Zc)
                        : FVector(0.0f, 0.0f, Zc);
                    const FConn Conn{
                        FVector(Child.X, Child.Y, Zc), ParentPoint, TreeRadius,
                        FloorZ, true };
                    if (ConnectorMayReachCell(Conn))
                    {
                        Cache.Conns.Add(Conn);
                    }
                };

                // Structural drainage tree. Every real shaft in the rebuild-only child window
                // first chooses the nearest strictly more-central shaft in its complete 5×5
                // window. A local minimum falls back to the nearest lower-origin shaft in the
                // deterministic halo; a window touching the origin uses the spine directly, with
                // a final direct-spine fallback for an unusually empty finite halo. Every parent
                // edge therefore either strictly decreases origin distance or terminates at the
                // spine: no cycles and no orphans. Spatial culling makes the same edge visible
                // throughout its capsule, independent of which cache cell evaluates it.
                for (const FShaft& Child : TreeEmitShafts)
                {
                    if (Child.bOriginSpine) continue;

                    const float ChildOriginSq = FMath::Square(Child.X) + FMath::Square(Child.Y);
                    const FShaft* Parent = nullptr;
                    float BestDistanceSq = FLT_MAX;
                    for (int32 dy = -CandidateRadius; dy <= CandidateRadius; ++dy)
                    for (int32 dx = -CandidateRadius; dx <= CandidateRadius; ++dx)
                    {
                        const FShaft* Candidate = FindCollectedShaft(
                            Child.CellX + dx, Child.CellY + dy);
                        if (Candidate == nullptr
                            || (Candidate->CellX == Child.CellX && Candidate->CellY == Child.CellY))
                        {
                            continue;
                        }

                        const float CandidateOriginSq = FMath::Square(Candidate->X)
                                                       + FMath::Square(Candidate->Y);
                        if (!(CandidateOriginSq < ChildOriginSq))
                        {
                            continue;
                        }

                        const float DistanceSq = FMath::Square(Child.X - Candidate->X)
                                               + FMath::Square(Child.Y - Candidate->Y);
                        const bool bLowerCell = Parent == nullptr
                            || Candidate->CellY < Parent->CellY
                            || (Candidate->CellY == Parent->CellY
                                && Candidate->CellX < Parent->CellX);
                        if (DistanceSq < BestDistanceSq
                            || (DistanceSq == BestDistanceSq && bLowerCell))
                        {
                            BestDistanceSq = DistanceSq;
                            Parent = Candidate;
                        }
                    }

                    if (Parent == nullptr
                        && !(FMath::Abs(Child.CellX) <= CandidateRadius
                            && FMath::Abs(Child.CellY) <= CandidateRadius))
                    {
                        // Specified neighbour-shaft fallback. The lower-origin restriction is the
                        // proof that this fallback cannot create a cycle.
                        for (int32 dy = -FallbackRadius; dy <= FallbackRadius; ++dy)
                        for (int32 dx = -FallbackRadius; dx <= FallbackRadius; ++dx)
                        {
                            const FShaft* Candidate = FindCollectedShaft(
                                Child.CellX + dx, Child.CellY + dy);
                            if (Candidate == nullptr
                                || (Candidate->CellX == Child.CellX
                                    && Candidate->CellY == Child.CellY))
                            {
                                continue;
                            }

                            const float CandidateOriginSq = FMath::Square(Candidate->X)
                                                           + FMath::Square(Candidate->Y);
                            if (!(CandidateOriginSq < ChildOriginSq))
                            {
                                continue;
                            }

                            const float DistanceSq = FMath::Square(Child.X - Candidate->X)
                                                   + FMath::Square(Child.Y - Candidate->Y);
                            const bool bLowerCell = Parent == nullptr
                                || Candidate->CellY < Parent->CellY
                                || (Candidate->CellY == Parent->CellY
                                    && Candidate->CellX < Parent->CellX);
                            if (DistanceSq < BestDistanceSq
                                || (DistanceSq == BestDistanceSq && bLowerCell))
                            {
                                BestDistanceSq = DistanceSq;
                                Parent = Candidate;
                            }
                        }
                    }

                    EmitTreeConnector(Child, Parent);
                }

                // Existing random links remain the texture/loop layer and stay on the inner 3×3
                // set. Only the deterministic tree build uses the wider 9×9/15×15 collection.
                if (P.CrossConnectChance > 0.0f && Cache.Shafts.Num() >= 2)
                {
                    for (int32 i = 0; i < Cache.Shafts.Num(); i++)
                    for (int32 j = i + 1; j < Cache.Shafts.Num(); j++)
                    {
                        const FShaft& A = Cache.Shafts[i];
                        const FShaft& B = Cache.Shafts[j];
                        const float DSq = FMath::Square(A.X - B.X) + FMath::Square(A.Y - B.Y);
                        if (DSq > FMath::Square(Spacing * 1.6f)) { continue; }

                        const uint32 PH = VoxelHash::Pair(
                            FMath::RoundToInt(A.X), FMath::RoundToInt(A.Y),
                            FMath::RoundToInt(B.X), FMath::RoundToInt(B.Y), Salt ^ 0xC04Eu);
                        const bool bSpineConnector = A.bOriginSpine || B.bOriginSpine;
                        if (VoxelHash::ToFloat01(PH) >= P.CrossConnectChance)
                        {
                            continue;
                        }

                        const float Zc = FMath::Lerp(BottomZ, TopZ, VoxelHash::ToFloat01(VoxelHash::Mix(PH)));
                        const float ConnectorR = bSpineConnector
                            ? SpineConnectorRadius() : P.ConnectorRadius;
                        const FConn Conn{
                            FVector(A.X, A.Y, Zc), FVector(B.X, B.Y, Zc), ConnectorR,
                            0.0f, false };
                        if (ConnectorMayReachCell(Conn))
                        {
                            Cache.Conns.Add(Conn);
                        }
                    }
                }
            }
            return Cache;
        }

        FVerticalShaftParams P;
        uint32 Salt;
        float  SpineRadius;
    };

    //=========================================================================
    // RÔLE 3 — MODIFIER : ÉTAGÈRES DE PUITS / SHAFT LEDGES
    //=========================================================================
    // Des tablettes fines à intervalles réguliers en Z, posées UNIQUEMENT sur la moitié +X/+Y du
    // puits le plus proche — pour que la moitié opposée reste libre et le puits franchissable.
    // C'est un op FORÇANT (`Max`) : il ne peut qu'ajouter du solide.
    class FShaftLedgeMod final : public IVoxelDensityOp
    {
    public:
        FShaftLedgeMod(const FVerticalShaftParams& InP, const FShaftFieldSource* InField)
            : P(InP), Field(InField) {}

        EVoxelOpRole GetRole() const override { return EVoxelOpRole::DetailModifier; }
        EVoxelOpChannelMask ChannelReads() const override
        {
            return VoxelOpChannels::Density | VoxelOpChannels::Sdf;
        }
        EVoxelOpChannelMask ChannelWrites() const override { return VoxelOpChannels::Density; }
        EVoxelOpResourceMask RequiredResources() const override { return VoxelOpResources::ShaftGeometry; }
        bool IsAdditive() const override { return false; }
        void PrepareChunk(const FVoxelOpContext&) override {}

        void Eval(float WorldX, float WorldY, float WorldZ, FVoxelOpSample& InOut) const override
        {
            if (P.LedgeSpacing <= 0.0f || P.LedgeDepth <= 0.0f || Field == nullptr) { return; }
            // ⚠️ La garde de l'original est `CaveSDF < 0` — le SDF APRÈS rugosité, tel que la pile
            // l'a laissé. C'est bien `InOut.Sdf` ici, pas une re-évaluation : re-calculer donnerait
            // le SDF SANS rugosité et déplacerait les étagères.
            if (InOut.Sdf >= 0.0f) { return; }

            const float Phase = FMath::Frac((WorldZ - P.StrateBottomWorldZ) / P.LedgeSpacing);
            const float BandT = FMath::Min(Phase, 1.0f - Phase) * P.LedgeSpacing;
            if (BandT >= P.LedgeDepth) { return; }

            const FShaftFieldSource::FCells& C = Field->GetCellsAt(WorldX, WorldY);
            if (C.Shafts.Num() == 0) { return; }

            const FShaftFieldSource::FShaft* Near = nullptr;
            float BestSq = FLT_MAX;
            for (const FShaftFieldSource::FShaft& Sh : C.Shafts)
            {
                if (Sh.bOriginSpine) continue;  // ledges belong to real shafts, not the post.
                const float D2 = FMath::Square(WorldX - Sh.X) + FMath::Square(WorldY - Sh.Y);
                if (D2 < BestSq) { BestSq = D2; Near = &Sh; }
            }
            // The shaft axis is the structural tree's terminal point. Keep the mathematical
            // half-plane boundary open: tiny cancellation error must not turn an exact axis
            // landing into a solid ledge cap and sever the parent capsule.
            if (Near && (WorldX - Near->X) + (WorldY - Near->Y) > 1.0e-3f)
            {
                const float Shelf = 1.0f - SmoothStep01(BandT / P.LedgeDepth);
                InOut.Density = FMath::Max(InOut.Density, Shelf * P.BaseDensity);
            }
        }

        // N'ajoute que du solide ⇒ tue AllAir, jamais AllSolid.
        EVoxelOpEffect EffectOverBox(const FBox&, const FVoxelOpContext&) const override
        {
            return (Field != nullptr && P.LedgeSpacing > 0.0f && P.LedgeDepth > 0.0f)
                 ? EVoxelOpEffect::FillOnly : EVoxelOpEffect::Identity;
        }

        EVoxelOpEffect EffectOverBox(const FBox& VoxelBox, const FVoxelOpContext& Ctx,
                                     const FVoxelBoxHypotheses& H) const override
        {
            if (Field == nullptr || P.LedgeSpacing <= 0.0f || P.LedgeDepth <= 0.0f)
            {
                return EVoxelOpEffect::Identity;
            }
            return H.Sdf.IsKnown() && H.Sdf.Min >= 0.0f
                 ? EVoxelOpEffect::Identity : EVoxelOpEffect::FillOnly;
        }

    private:
        FVerticalShaftParams P;
        const FShaftFieldSource* Field;   // NON possédant : la pile possède la source
    };

    // The deterministic shaft tree is a walkable network as well as an air topology. Reassert
    // its air core after partial shaft ledges, then add the same inset floor band as the native
    // path at the common first ledge level. This lets a player-fit route cross between shafts
    // without restoring the removed landing-to-origin roads.
    class FShaftConnectorAirMod final : public IVoxelDensityOp
    {
    public:
        explicit FShaftConnectorAirMod(const FShaftFieldSource* InField, float InBaseDensity)
            : Field(InField), BaseDensity(InBaseDensity) {}

        EVoxelOpRole GetRole() const override { return EVoxelOpRole::DetailModifier; }
        EVoxelOpChannelMask ChannelReads() const override { return VoxelOpChannels::Density; }
        EVoxelOpChannelMask ChannelWrites() const override { return VoxelOpChannels::Density; }
        EVoxelOpResourceMask RequiredResources() const override { return VoxelOpResources::ShaftGeometry; }
        bool IsAdditive() const override { return false; }
        void PrepareChunk(const FVoxelOpContext&) override {}

        void Eval(float WorldX, float WorldY, float WorldZ,
                  FVoxelOpSample& InOut) const override
        {
            if (Field == nullptr) return;
            const FShaftFieldSource::FCells& Cells = Field->GetCellsAt(WorldX, WorldY);
            for (const FShaftFieldSource::FConn& Conn : Cells.Conns)
            {
                if (!Conn.bWalkableFloor) continue;
                VoxelPassageGeometry::VF_ApplyVerticalShaftConnectorAir(
                    InOut.Density, WorldX, WorldY, WorldZ,
                    Conn.A, Conn.B, Conn.Radius, Conn.FloorZ, BaseDensity);
            }
        }

        EVoxelOpEffect EffectOverBox(const FBox&, const FVoxelOpContext&) const override
        {
            return Field != nullptr ? EVoxelOpEffect::Both : EVoxelOpEffect::Identity;
        }

        const TCHAR* DebugName() const override { return TEXT("ShaftConnectorAirOp"); }

    private:
        const FShaftFieldSource* Field = nullptr; // NON possédant : la pile possède la source
        float BaseDensity = 8.0f;
    };

    class FShaftConnectorFloorMod final : public IVoxelDensityOp
    {
    public:
        FShaftConnectorFloorMod(const FShaftFieldSource* InField, float InBaseDensity)
            : Field(InField), BaseDensity(InBaseDensity) {}

        EVoxelOpRole GetRole() const override { return EVoxelOpRole::DetailModifier; }
        EVoxelOpChannelMask ChannelReads() const override { return VoxelOpChannels::Density; }
        EVoxelOpChannelMask ChannelWrites() const override { return VoxelOpChannels::Density; }
        EVoxelOpResourceMask RequiredResources() const override { return VoxelOpResources::ShaftGeometry; }
        bool IsAdditive() const override { return false; }
        void PrepareChunk(const FVoxelOpContext&) override {}

        void Eval(float WorldX, float WorldY, float WorldZ,
                  FVoxelOpSample& InOut) const override
        {
            if (Field == nullptr) return;
            const FShaftFieldSource::FCells& Cells = Field->GetCellsAt(WorldX, WorldY);
            for (const FShaftFieldSource::FConn& Conn : Cells.Conns)
            {
                if (!Conn.bWalkableFloor) continue;
                VoxelPassageGeometry::VF_ApplyVerticalShaftConnectorFloor(
                    InOut.Density, WorldX, WorldY, WorldZ,
                    Conn.A, Conn.B, Conn.Radius, Conn.FloorZ, BaseDensity);
            }
        }

        EVoxelOpEffect EffectOverBox(const FBox&, const FVoxelOpContext&) const override
        {
            return Field != nullptr ? EVoxelOpEffect::FillOnly : EVoxelOpEffect::Identity;
        }

        const TCHAR* DebugName() const override { return TEXT("ShaftConnectorFloorOp"); }

    private:
        const FShaftFieldSource* Field = nullptr; // NON possédant : la pile possède la source
        float BaseDensity = 8.0f;
    };

    //=========================================================================
    // RÔLE 1 — SOURCE : ÎLES FLOTTANTES / FLOATING ISLAND BLOBS
    //=========================================================================
    // Le SEUL archétype dont la source est de l'AIR : `FConstantFieldSource(-BaseDensity)` pose un
    // grand vide, et cet opérateur y suspend des blobs. C'est ce qui en fait le bon test de
    // composition — tous les autres portages partent de roc et creusent.
    //
    // FORME D'UNE ÎLE : une dalle assez plate au-dessus du centre (`TopHalf = 0.20·Rxy`) et un
    // dessous qui s'effile vers une pointe (`ThicknessRatio·Rxy`). C'est l'asymétrie qui se lit
    // comme une île flottante plutôt que comme une sphère.
    //
    // ⚠️ ÉCART ASSUMÉ AVEC `OPSTACK-DECOMPOSITION §7`, qui décrivait un `FRAME IslandWarp` enveloppant
    // la source. Le warp reste À L'INTÉRIEUR de l'opérateur, et c'est délibéré : `§7` compte trois
    // usages de frames (îles, caves de TunnelNetwork, tunnels), et aucun n'en a besoin. Voir
    // `BuildTunnelNetworkStack` : `CaveWarp` n'enveloppe qu'UN opérateur (donc c'est une variable
    // locale, pas un frame) et `VerticalScale` est une fonction pure d'un scalaire. **Zéro frame
    // sur trois candidats.** Ne pas rouvrir : le warp reste local ICI pour la même raison qu'il
    // est resté local là-bas. On n'abstrait qu'à la deuxième occurrence réelle (cf.
    // `IVoxelBiomeField`, né d'un besoin réel).
    //
    // The warp stays INSIDE the op against §7's FRAME suggestion. TunnelNetwork is ported, and the
    // second real use dissolved the question rather than settling it. See BuildTunnelNetworkStack:
    // CaveWarp wraps exactly ONE operator (a local variable, not a frame) and VerticalScale is a
    // pure function of a scalar. ZERO frames out of three candidates. Do not reopen.
    class FIslandBlobSource final : public IVoxelDensityOp
    {
    public:
        FIslandBlobSource(const FFloatingIslandParams& InP, int32 Seed)
            : P(InP), Salt((uint32)Seed ^ 0x49736C64u)   // 'Isld' — identique à GetFloatingIslandDensity
        {}

        EVoxelOpRole GetRole() const override { return EVoxelOpRole::FieldSource; }
        EVoxelOpChannelMask ChannelReads() const override { return VoxelOpChannels::None; }
        EVoxelOpChannelMask ChannelWrites() const override { return VoxelOpChannels::Sdf; }
        bool IsAdditive() const override { return false; }
        void PrepareChunk(const FVoxelOpContext&) override {}

        struct FIsland { float X, Y, Rxy, TopHalf, TopZ, BotZ, TaperEnd; };

        // ⚠️ DÉCLARÉE ICI, avant toute fonction qui la renvoie — même piège que `FShaftFieldSource`
        // (C4430 : les corps de méthodes sont différés, les types de retour non).
        struct FCells { TArray<FIsland, TInlineAllocator<9>> Islands; };

        void Eval(float WorldX, float WorldY, float WorldZ, FVoxelOpSample& InOut) const override
        {
            const float BlendK = FMath::Max(P.SDFBlendRadius, 0.01f);
            const FCells& C = GetCells(WorldX, WorldY);

            // CONTOUR IRRÉGULIER : on déforme la requête HORIZONTALE pour que les bords des îles
            // soient lobés au lieu d'être des cercles parfaits. Calculé une fois par voxel et
            // partagé par toutes les îles proches — chacune échantillonne une autre partie du champ,
            // d'où des silhouettes distinctes.
            const float WarpAmp = (P.IslandMinRadius + P.IslandMaxRadius) * 0.5f * 0.35f;
            const float WX = WorldX + HFractal3D(FVector3f(WorldX * 0.04f + VoxelHash::SeedOffset(Salt, 0.0007f),
                                                         WorldY * 0.04f, WorldZ * 0.012f), VoxelGenLOD::Eff(3))
                                      * VOXEL_NOISE_SCALE * WarpAmp;
            const float WY = WorldY + HFractal3D(FVector3f(WorldX * 0.04f + 31.0f, WorldY * 0.04f + 7.0f,
                                                         WorldZ * 0.012f), VoxelGenLOD::Eff(3))
                                      * VOXEL_NOISE_SCALE * WarpAmp;

            float IslandSDF = FLT_MAX;
            for (const FIsland& Isl : C.Islands)
            {
                // Distance horizontale dans le repère DÉFORMÉ, donc le contour n'est pas un cercle.
                const float Dxw = WX - Isl.X, Dyw = WY - Isl.Y;
                const float DistXY = FMath::Sqrt(Dxw * Dxw + Dyw * Dyw);

                // Enveloppe de rayon par la hauteur : pleine largeur en haut, resserrée jusqu'à une
                // pointe en bas (taper SmoothStep).
                const float Hgt = FMath::Clamp((WorldZ - Isl.BotZ) / FMath::Max(Isl.TopZ - Isl.BotZ, 1.0f),
                                               0.0f, 1.0f);
                const float Taper = SmoothStep01(FMath::Clamp(Hgt / Isl.TaperEnd, 0.0f, 1.0f));
                const float Env = Isl.Rxy * Taper;

                // Surface du dessus : plate par défaut ; les bords retombent en dôme si TopFlatten < 1.
                float TopSurf = Isl.TopZ;
                if (P.TopFlatten < 1.0f)
                {
                    const float Edge = FMath::Clamp(DistXY / FMath::Max(Isl.Rxy, 1.0f), 0.0f, 1.0f);
                    TopSurf = Isl.TopZ - (1.0f - P.TopFlatten) * Isl.TopHalf * 2.0f * Edge * Edge;
                }

                // Pseudo-SDF : dehors si au-delà de l'enveloppe radiale OU au-dessus du dessus.
                const float Sdf = FMath::Max(DistXY - Env, WorldZ - TopSurf);

                IslandSDF = VoxelSDF::SmoothMin(IslandSDF, Sdf, BlendK);
            }

            InOut.Sdf = IslandSDF;
        }

        /**
     * Cette source écrit uniquement le canal SDF : son `EffectOverBox` intrinsèque est donc
     * `Identity`. Le convertisseur de remplissage consomme ensuite l'intervalle publié ; sur une
     * strate d'îles, l'absence d'île prouvée est le cas COURANT et, combinée à l'`AllAir` de la
     * source constante, permet enfin de prouver « tout air » (`OPSTACK-DECOMPOSITION §7`).
         *
         * BORNE, et pourquoi elle est sûre dans les deux directions :
         *   • en XY, `Sdf ≥ DistXY − Rxy` (l'enveloppe ne dépasse jamais `Rxy`), et le warp déplace
         *     le POINT de `WarpAmp · VOXEL_NOISE_SCALE · √2` au plus (la borne prouvée de chaque
         *     FBM est 1.5 sur DEUX axes indépendants — voir la note √2 dans le corps) ;
         *   • en Z, `Sdf ≥ WorldZ − TopSurf ≥ WorldZ − TopZ`, donc au-dessus du sommet + marge il
         *     n'y a plus rien à faire. **En dessous, il n'y a PAS de borne** : sous une île, le SDF
         *     vaut ≈ `DistXY` à toute profondeur, donc un mince fil de matière descend le long de
         *     l'axe. C'est le comportement de l'original ; le confondre avec « rien en dessous »
         *     serait un TROU, et c'est pourquoi seule la borne HAUTE est testée.
         *   • cette source ne couvre pas l'aval : elle publie seulement son propre intervalle SDF.
         *     La rugosité, le blend du fill et le creux du SmoothMin sont bornés par leurs propres
         *     opérateurs dans le pliage, chacun avec son contrat isolé.
         */
        EVoxelOpEffect EffectOverBox(const FBox&, const FVoxelOpContext&) const override
        {
            // This source writes SDF only. Island filling is the converter's responsibility.
            return EVoxelOpEffect::Identity;
        }

        /**
         * Propagate a source-only lower bound. The exact 3x3 neighbourhood used by Eval is
         * enumerated for every XY cell touched by the box. The warp, authored radius endpoints,
         * top envelope, and the bounded SmoothMin dip are all included in the bound.
         */
        void PropagateSdfOverBox(FVoxelBoxSdfInterval& InOut, const FBox& VoxelBox,
                                 const FVoxelOpContext&) const override
        {
            if (!VoxelMath::IsFinite((float)VoxelBox.Min.X) || !VoxelMath::IsFinite((float)VoxelBox.Min.Y)
                || !VoxelMath::IsFinite((float)VoxelBox.Min.Z)
                || !VoxelMath::IsFinite((float)VoxelBox.Max.X) || !VoxelMath::IsFinite((float)VoxelBox.Max.Y)
                || !VoxelMath::IsFinite((float)VoxelBox.Max.Z)
                || VoxelBox.Min.X > VoxelBox.Max.X || VoxelBox.Min.Y > VoxelBox.Max.Y
                || VoxelBox.Min.Z > VoxelBox.Max.Z
                || !VoxelMath::IsFinite(P.IslandSpacing) || !VoxelMath::IsFinite(P.IslandDensity)
                || !VoxelMath::IsFinite(P.IslandMinRadius) || !VoxelMath::IsFinite(P.IslandMaxRadius)
                || !VoxelMath::IsFinite(P.ThicknessRatio) || !VoxelMath::IsFinite(P.VerticalJitter)
                || !VoxelMath::IsFinite(P.TopFlatten) || !VoxelMath::IsFinite(P.SDFBlendRadius)
                || P.IslandDensity < 0.0f)
            {
                InOut.SetUnknown();
                return;
            }

            if (P.IslandDensity <= 0.0f)
            {
                InOut.Set(FLT_MAX, FLT_MAX);
                return;
            }

            const float Spacing = FMath::Max(P.IslandSpacing, 1.0f);
            const int32 CX0 = FMath::FloorToInt((float)VoxelBox.Min.X / Spacing);
            const int32 CY0 = FMath::FloorToInt((float)VoxelBox.Min.Y / Spacing);
            const int32 CX1 = FMath::FloorToInt((float)VoxelBox.Max.X / Spacing);
            const int32 CY1 = FMath::FloorToInt((float)VoxelBox.Max.Y / Spacing);
            constexpr int64 MaxCellsScanned = 64 * 64;
            const int64 CellCount = ((int64)CX1 - CX0 + 1) * ((int64)CY1 - CY0 + 1);
            if (CellCount <= 0 || CellCount > MaxCellsScanned)
            {
                InOut.SetUnknown();
                return;
            }

            constexpr float Sqrt2 = 1.4142136f;
            const float WarpAmp = (P.IslandMinRadius + P.IslandMaxRadius) * 0.5f * 0.35f;
            const float WarpDiag = FMath::Abs(WarpAmp) * VOXEL_NOISE_SCALE * VF_PerlinAbsBound * Sqrt2;
            const float RadiusUpper = FMath::Max3(P.IslandMinRadius, P.IslandMaxRadius, 0.0f);
            // For the authored TopFlatten range [0,1], TopSurf never exceeds TopZ. Keeping the
            // positive general-range term makes the proof remain safe if a data asset is widened.
            const float TopSurfExtra = FMath::Max(P.TopFlatten - 1.0f, 0.0f)
                                      * RadiusUpper * 0.20f * 2.0f;

            float Lower = FLT_MAX;
            bool bAnyIsland = false;
            const float BlendK = FMath::Max(P.SDFBlendRadius, 0.01f);
            for (int32 cy = CY0; cy <= CY1; ++cy)
            for (int32 cx = CX0; cx <= CX1; ++cx)
            {
                const FCells& C = GetCells((cx + 0.5f) * Spacing, (cy + 0.5f) * Spacing);
                for (const FIsland& Isl : C.Islands)
                {
                    bAnyIsland = true;
                    const float RadialLower = VF_DistanceBoxToPointXY(VoxelBox, Isl.X, Isl.Y)
                                            - WarpDiag
                                            - FMath::Max(Isl.Rxy, 0.0f);
                    const float TopLower = (float)VoxelBox.Min.Z - Isl.TopZ - TopSurfExtra;
                    const float IslandLower = FMath::Max(RadialLower, TopLower);
                    Lower = FMath::Min(Lower, IslandLower);
                }
            }

            if (!bAnyIsland)
            {
                InOut.Set(FLT_MAX, FLT_MAX);
                return;
            }

            // SmoothMin can dip below the smallest raw term, but its total deficit is bounded by
            // one blend radius: once the running minimum is a blend radius below another term,
            // the next penalty is zero. This is a lower bound for any number of islands.
            InOut.Set(VF_SaturatingAdd(Lower, -BlendK), FLT_MAX);
        }

    private:
        /** Tirage d'une cellule. PURE en (cellule, seed, params) ⇒ `Eval` et `EffectOverBox` ne
         *  peuvent pas voir des îles différentes. Transcription littérale du bloc de cuisson de
         *  `GetFloatingIslandDensity`. */
        bool RollIsland(int32 nx, int32 ny, FIsland& Out) const
        {
            const float H = P.StrateTopWorldZ - P.StrateBottomWorldZ;
            const float Spacing = FMath::Max(P.IslandSpacing, 1.0f);
            const float MidZ = (P.StrateTopWorldZ + P.StrateBottomWorldZ) * 0.5f;

            const uint32 Hh = VoxelHash::Cell(nx, ny, Salt);
            if (VoxelHash::ToFloat01(Hh) > P.IslandDensity) { return false; }

            const float JX = VoxelHash::ToFloat01(VoxelHash::Mix(Hh ^ 0x12345678u));
            const float JY = VoxelHash::ToFloat01(VoxelHash::Mix(Hh ^ 0x9ABCDEF0u));

            Out.X = (nx + 0.15f + JX * 0.7f) * Spacing;
            Out.Y = (ny + 0.15f + JY * 0.7f) * Spacing;
            Out.Rxy = FMath::Lerp(P.IslandMinRadius, P.IslandMaxRadius,
                                  VoxelHash::ToFloat01(VoxelHash::Mix(Hh ^ 0x5A5Au)));

            // PROFIL ASYMÉTRIQUE : dalle de terre au-dessus, dessous qui s'effile en pointe.
            Out.TopHalf = Out.Rxy * 0.20f;
            const float UnderDepth = Out.Rxy * FMath::Max(P.ThicknessRatio, 0.25f);

            const float SpreadZ = FMath::Max(H * 0.5f - FMath::Max(Out.TopHalf, UnderDepth)
                                             - P.BoundarySealThickness, 0.0f) * P.VerticalJitter;
            const float Cz = MidZ + VoxelHash::ToFloatSigned(VoxelHash::Mix(Hh ^ 0xB17Du)) * SpreadZ;
            Out.TopZ = Cz + Out.TopHalf;
            Out.BotZ = Cz - UnderDepth;

            // Netteté du taper par île (point d'arrivée du SmoothStep) → silhouettes variées.
            Out.TaperEnd = FMath::Lerp(0.45f, 0.7f, VoxelHash::ToFloat01(VoxelHash::Mix(Hh ^ 0x7A1Eu)));
            return true;
        }

        /**
         * Le voisinage 3×3, mémoïsé par worker — la même cuisson `thread_local` que l'original.
         *
         * ⚠️ LA CLÉ INCLUT `BoundarySealThickness`, QUE L'ORIGINAL OMET. `SpreadZ` s'en sert
         * (`H·0.5 − max(TopHalf, UnderDepth) − Seal`), donc dans `GetFloatingIslandDensity` une
         * édition à chaud qui ne change QUE l'épaisseur de seal sert des îles périmées. Même famille
         * que `AUDIT §C2` : une clé de cache incomplète ne se voit pas, elle produit du terrain
         * plausible. Ajouter le champ ne coûte
         * qu'un recalcul, jamais une valeur différente — donc l'égalité binaire tient.
         *
         * The key includes BoundarySealThickness, which the original omits although SpreadZ reads it.
         * Adding it can only cost a recompute, never change a value — bit-equality is unaffected.
         */
        const FCells& GetCells(float WorldX, float WorldY) const
        {
            const float Spacing = FMath::Max(P.IslandSpacing, 1.0f);
            const int32 CX = FMath::FloorToInt(WorldX / Spacing);
            const int32 CY = FMath::FloorToInt(WorldY / Spacing);

            thread_local FCells  Cache;
            thread_local int32   FI_CX = INT32_MAX, FI_CY = INT32_MAX;
            thread_local uint32  FI_Salt = 0xFFFFFFFFu;
            thread_local float   FI_Spacing = -1.0f, FI_Dens = -1.0f, FI_MinR = -1.0f, FI_MaxR = -1.0f,
                                 FI_Thick = -1.0f, FI_VJit = -1.0f, FI_Seal = -1.0f,
                                 FI_BotZ = FLT_MAX, FI_TopZ = FLT_MAX;

            if (CX != FI_CX || CY != FI_CY || Salt != FI_Salt || Spacing != FI_Spacing ||
                P.IslandDensity != FI_Dens || P.IslandMinRadius != FI_MinR ||
                P.IslandMaxRadius != FI_MaxR || P.ThicknessRatio != FI_Thick ||
                P.VerticalJitter != FI_VJit || P.BoundarySealThickness != FI_Seal ||
                P.StrateBottomWorldZ != FI_BotZ || P.StrateTopWorldZ != FI_TopZ)
            {
                FI_CX = CX;  FI_CY = CY;  FI_Salt = Salt;  FI_Spacing = Spacing;
                FI_Dens = P.IslandDensity;  FI_MinR = P.IslandMinRadius;  FI_MaxR = P.IslandMaxRadius;
                FI_Thick = P.ThicknessRatio;  FI_VJit = P.VerticalJitter;
                FI_Seal = P.BoundarySealThickness;
                FI_BotZ = P.StrateBottomWorldZ;  FI_TopZ = P.StrateTopWorldZ;
                Cache.Islands.Reset();

                for (int32 dy = -1; dy <= 1; dy++)
                for (int32 dx = -1; dx <= 1; dx++)
                {
                    FIsland Isl;
                    if (RollIsland(CX + dx, CY + dy, Isl)) { Cache.Islands.Add(Isl); }
                }
            }
            return Cache;
        }

        FFloatingIslandParams P;
        uint32 Salt;
    };

    // The mesher's coarse samples are sparse enough that one sample can land in a different
    // exact 32-voxel chunk every time.  Keeping one SDF cache inside every exact-chunk op-stack
    // therefore turns L4 into thousands of BuildChunkCache calls.  These worker-local entries
    // share the immutable room graph cache by its complete deterministic input key.  A wider
    // region is safe because BuildChunkCache's collect/store construction is window-invariant;
    // the key still includes every value that can change the graph or its terrain-op rolls.
    // A coarse tile traverses several deterministic XY search windows while LODs are generated
    // concurrently.  One mixed table made fine and coarse windows evict one another before the
    // next tile could reuse them.  Keep a bounded table per region-size class: this is a cache
    // partition only, not a change to the graph key or to graph construction.
    constexpr int32 RoomGraphCacheSlotCount = 128;
    constexpr int32 RoomGraphCacheBankCount = 3;

    struct FRoomGraphCacheEntry
    {
        bool bValid = false;
        uint32 Seed = 0;
        int32 StrateIndex = INT32_MIN;
        uint32 ParamsFingerprint = 0xFFFFFFFFu;
        uint32 LayoutVersion = 0xFFFFFFFFu;
        uint64 ManagerLifetimeId = 0;
        const TArray<FStrateTerrainOpEntry>* TerrainOps = nullptr;
        int32 RegionMinX = 0;
        int32 RegionMinY = 0;
        int32 RegionSize = 0;
        uint32 LastUse = 0;
        FChunkSDFCache Cache;
    };

    static thread_local FRoomGraphCacheEntry GRoomGraphCache[
        RoomGraphCacheBankCount][RoomGraphCacheSlotCount];
    static thread_local uint32 GRoomGraphCacheClock = 0;

    FORCEINLINE int32 RoomGraphCacheBank(int32 RegionSize)
    {
        return RegionSize <= CHUNK_SIZE ? 0
             : (RegionSize <= 2 * CHUNK_SIZE ? 1 : 2);
    }

    FORCEINLINE int32 RoomGraphCacheSlot(
        uint32 Seed, int32 StrateIndex, uint32 ParamsFingerprint, uint32 LayoutVersion,
        uint64 ManagerLifetimeId, int32 RegionMinX, int32 RegionMinY, int32 RegionSize)
    {
        uint32 Hash = Seed * 0x9E3779B9u;
        Hash ^= static_cast<uint32>(StrateIndex) * 0x85EBCA6Bu;
        Hash ^= ParamsFingerprint * 0xC2B2AE35u;
        Hash ^= LayoutVersion * 0x27D4EB2Fu;
        Hash ^= static_cast<uint32>(ManagerLifetimeId)
            ^ static_cast<uint32>(ManagerLifetimeId >> 32);
        Hash ^= static_cast<uint32>(RegionMinX) * 0x165667B1u;
        Hash ^= static_cast<uint32>(RegionMinY) * 0xD3A2646Cu;
        Hash ^= static_cast<uint32>(RegionSize) * 0xFD7046C5u;
        Hash ^= Hash >> 16;
        return static_cast<int32>(Hash & (RoomGraphCacheSlotCount - 1));
    }

    FRoomGraphCacheEntry* FindRoomGraphCacheEntry(
        uint32 Seed, int32 StrateIndex, uint32 ParamsFingerprint, uint32 LayoutVersion,
        uint64 ManagerLifetimeId, const TArray<FStrateTerrainOpEntry>* TerrainOps,
        int32 RegionMinX, int32 RegionMinY, int32 RegionSize)
    {
        auto SelectForReuse = [](FRoomGraphCacheEntry* Selected, uint32 Use)
            -> FRoomGraphCacheEntry*
        {
            if (Selected != nullptr && Selected->bValid)
            {
                // Reset() deliberately preserves hot-path capacity. That is useful while the
                // same window is reused, but not when this slot is about to become a different
                // key: every worker could otherwise retain one full graph window per historical
                // streaming key. Release before invalidating the key so a long-lived game has a
                // bounded working set instead of a capacity leak.
                Selected->Cache.Release();
                Selected->bValid = false;
            }
            if (Selected != nullptr)
            {
                Selected->LastUse = Use;
            }
            return Selected;
        };

        const int32 Bank = RoomGraphCacheBank(RegionSize);
        const uint32 Use = ++GRoomGraphCacheClock;
        const int32 StartSlot = RoomGraphCacheSlot(
            Seed, StrateIndex, ParamsFingerprint, LayoutVersion, ManagerLifetimeId,
            RegionMinX, RegionMinY, RegionSize);
        FRoomGraphCacheEntry* ReuseEntry = nullptr;
        FRoomGraphCacheEntry* LeastRecentlyUsed = nullptr;
        for (int32 Probe = 0; Probe < RoomGraphCacheSlotCount; ++Probe)
        {
            FRoomGraphCacheEntry& Candidate = GRoomGraphCache[Bank][
                (StartSlot + Probe) & (RoomGraphCacheSlotCount - 1)];
            if (!Candidate.bValid)
            {
                FRoomGraphCacheEntry* Selected = ReuseEntry != nullptr ? ReuseEntry : &Candidate;
                return SelectForReuse(Selected, Use);
            }
            if (Candidate.Seed == Seed
                && Candidate.StrateIndex == StrateIndex
                && Candidate.ParamsFingerprint == ParamsFingerprint
                && Candidate.LayoutVersion == LayoutVersion
                && Candidate.ManagerLifetimeId == ManagerLifetimeId
                && Candidate.TerrainOps == TerrainOps
                && Candidate.RegionMinX == RegionMinX
                && Candidate.RegionMinY == RegionMinY
                && Candidate.RegionSize == RegionSize)
            {
                Candidate.LastUse = Use;
                return &Candidate;
            }
            if (ReuseEntry == nullptr && Candidate.ManagerLifetimeId != ManagerLifetimeId)
            {
                ReuseEntry = &Candidate;
            }
            if (LeastRecentlyUsed == nullptr || Candidate.LastUse < LeastRecentlyUsed->LastUse)
            {
                LeastRecentlyUsed = &Candidate;
            }
        }
        FRoomGraphCacheEntry* Selected = ReuseEntry != nullptr
            ? ReuseEntry : LeastRecentlyUsed;
        return SelectForReuse(
            Selected != nullptr ? Selected : &GRoomGraphCache[Bank][StartSlot], Use);
    }

    // Classification is dispatched across many workers before most of those workers have
    // evaluated a density sample.  A thread-local cache prevents races, but it also makes every
    // worker rebuild the same deterministic collect/NN graph.  Keep a small process-local set of
    // immutable classifier caches: construction is serialized, publication happens only after
    // BuildChunkCache has completed, and each consumer holds a shared reference while refining.
    // This cache never stores mutable manager state; all manager/terrain data are read while the
    // entry is built and the resulting FChunkSDFCache is read-only thereafter.
    struct FSharedRoomGraphCacheEntry
    {
        uint32 Seed = 0;
        int32 StrateIndex = INT32_MIN;
        uint32 ParamsFingerprint = 0xFFFFFFFFu;
        uint32 LayoutVersion = 0xFFFFFFFFu;
        uint64 ManagerLifetimeId = 0;
        const TArray<FStrateTerrainOpEntry>* TerrainOps = nullptr;
        int32 RegionMinX = 0;
        int32 RegionMinY = 0;
        int32 RegionSize = 0;
        uint32 LastUse = 0;
        bool bBuilding = false;
        FEvent* ReadyEvent = nullptr;
        TSharedPtr<const FChunkSDFCache, ESPMode::ThreadSafe> Cache;
        uint64 AllocatedBytes = 0;

        ~FSharedRoomGraphCacheEntry()
        {
            if (ReadyEvent != nullptr)
            {
                FPlatformProcess::ReturnSynchEventToPool(ReadyEvent);
            }
        }
    };

    static FCriticalSection GSharedRoomGraphCacheMutex;
    static TArray<TSharedPtr<FSharedRoomGraphCacheEntry, ESPMode::ThreadSafe>>
        GSharedRoomGraphCache;
    static uint32 GSharedRoomGraphCacheClock = 0;
    static uint64 GSharedRoomGraphCacheResidentBytes = 0;
    static FCriticalSection GSharedRoomGraphCacheBuildGateMutex;
    static int32 GSharedRoomGraphCacheActiveBuilds[2] = {0, 0};
    // The byte budget is the actual memory guard.  A 128-entry count is too small for the
    // player's streaming radius: it turns otherwise reusable 128-voxel windows into transient
    // rebuilds while only a few MiB are resident.  Keep a generous bounded index so the budget,
    // rather than an arbitrary key count, controls retention.
    constexpr int32 SharedRoomGraphCacheMaxEntries = 4096;
    // Limit the number of graph allocations that can exist before their size is
    // known and published into the byte budget. This is the guard against a
    // burst of distinct coarse tiles retaining one large build per worker.
    // Fine LOD0 proof windows are small and latency-sensitive. Keep their build slots separate
    // from coarse streaming windows: a pair of 250 ms LOD4 graph builds must not make a 32/64
    // voxel empty tile wait behind them. The fine slots are still bounded, and their graph
    // allocations are tiny compared with the coarse builds.
    constexpr int32 SharedRoomGraphCacheBuildBankCount = 2;
    constexpr int32 SharedRoomGraphCacheBuildConcurrency[SharedRoomGraphCacheBuildBankCount] = {4, 2};
    // This is a proof-cache budget, not a geometry budget. A miss rebuilds an immutable cache;
    // unknown/evicted proof data therefore falls back to sampling and cannot change the mesh.
    constexpr uint64 SharedRoomGraphCacheBudgetBytes = 256ull * 1024ull * 1024ull;

    static TSharedPtr<const FChunkSDFCache, ESPMode::ThreadSafe>
    SnapshotSharedRoomGraphCache(
        const TSharedPtr<FSharedRoomGraphCacheEntry, ESPMode::ThreadSafe>& Entry)
    {
        FScopeLock Lock(&GSharedRoomGraphCacheMutex);
        return Entry.IsValid()
            ? Entry->Cache
            : TSharedPtr<const FChunkSDFCache, ESPMode::ThreadSafe>();
    }

    int32 SharedRoomGraphCacheBuildBank(int32 RegionSize)
    {
        return RegionSize <= 2 * CHUNK_SIZE ? 0 : 1;
    }

    void AcquireSharedRoomGraphCacheBuildSlot(int32 RegionSize)
    {
        const int32 Bank = SharedRoomGraphCacheBuildBank(RegionSize);
        for (;;)
        {
            {
                FScopeLock Lock(&GSharedRoomGraphCacheBuildGateMutex);
                if (GSharedRoomGraphCacheActiveBuilds[Bank]
                    < SharedRoomGraphCacheBuildConcurrency[Bank])
                {
                    ++GSharedRoomGraphCacheActiveBuilds[Bank];
                    return;
                }
            }
            FPlatformProcess::Yield();
        }
    }

    void ReleaseSharedRoomGraphCacheBuildSlot(int32 RegionSize)
    {
        const int32 Bank = SharedRoomGraphCacheBuildBank(RegionSize);
        FScopeLock Lock(&GSharedRoomGraphCacheBuildGateMutex);
        check(GSharedRoomGraphCacheActiveBuilds[Bank] > 0);
        --GSharedRoomGraphCacheActiveBuilds[Bank];
    }

    void EvictSharedRoomGraphCacheLocked(const FSharedRoomGraphCacheEntry* ProtectedEntry = nullptr)
    {
        while (GSharedRoomGraphCache.Num() > SharedRoomGraphCacheMaxEntries
            || GSharedRoomGraphCacheResidentBytes > SharedRoomGraphCacheBudgetBytes)
        {
            int32 OldestIndex = INDEX_NONE;
            for (int32 Index = 0; Index < GSharedRoomGraphCache.Num(); ++Index)
            {
                const TSharedPtr<FSharedRoomGraphCacheEntry, ESPMode::ThreadSafe>& Candidate =
                    GSharedRoomGraphCache[Index];
                if (Candidate->bBuilding || Candidate.Get() == ProtectedEntry)
                {
                    continue;
                }
                if (OldestIndex == INDEX_NONE
                    || Candidate->LastUse < GSharedRoomGraphCache[OldestIndex]->LastUse)
                {
                    OldestIndex = Index;
                }
            }
            if (OldestIndex == INDEX_NONE)
            {
                // All remaining entries are in-flight or the protected oversized entry. Keep
                // them alive until their consumers finish; the next completed publish can evict.
                break;
            }

            const uint64 Bytes = GSharedRoomGraphCache[OldestIndex]->AllocatedBytes;
            GSharedRoomGraphCacheResidentBytes = Bytes >= GSharedRoomGraphCacheResidentBytes
                ? 0
                : GSharedRoomGraphCacheResidentBytes - Bytes;
            GSharedRoomGraphCache.RemoveAtSwap(OldestIndex, 1, EAllowShrinking::No);
        }
    }

    void RemoveStaleSharedRoomGraphCachesLocked(uint64 KeepManagerLifetimeId)
    {
        for (int32 Index = GSharedRoomGraphCache.Num() - 1; Index >= 0; --Index)
        {
            const TSharedPtr<FSharedRoomGraphCacheEntry, ESPMode::ThreadSafe>& Candidate =
                GSharedRoomGraphCache[Index];
            if (Candidate->bBuilding
                || (KeepManagerLifetimeId != 0
                    && Candidate->ManagerLifetimeId == KeepManagerLifetimeId))
            {
                continue;
            }

            const uint64 Bytes = Candidate->AllocatedBytes;
            GSharedRoomGraphCacheResidentBytes = Bytes >= GSharedRoomGraphCacheResidentBytes
                ? 0
                : GSharedRoomGraphCacheResidentBytes - Bytes;
            GSharedRoomGraphCache.RemoveAtSwap(Index, 1, EAllowShrinking::No);
        }
        EvictSharedRoomGraphCacheLocked();
    }

    TSharedPtr<FSharedRoomGraphCacheEntry, ESPMode::ThreadSafe> FindOrBuildSharedRoomGraphCache(
        uint32 Seed, int32 StrateIndex, uint32 ParamsFingerprint, uint32 LayoutVersion,
        uint64 ManagerLifetimeId, const TArray<FStrateTerrainOpEntry>* TerrainOps,
        int32 RegionMinX, int32 RegionMinY, int32 RegionSize,
        float SearchMinX, float SearchMinY, float SearchMaxX, float SearchMaxY,
        const FStrateGenerationParams& Params,
        ERoomGraphBuildSite BuildSite)
    {
        for (;;)
        {
            TSharedPtr<FSharedRoomGraphCacheEntry, ESPMode::ThreadSafe> Entry;
            bool bBuild = false;
            bool bBuildTransient = false;
            {
                FScopeLock Lock(&GSharedRoomGraphCacheMutex);
                const uint32 Use = ++GSharedRoomGraphCacheClock;
                for (const TSharedPtr<FSharedRoomGraphCacheEntry, ESPMode::ThreadSafe>& Candidate
                     : GSharedRoomGraphCache)
                {
                    if (Candidate->Seed == Seed
                        && Candidate->StrateIndex == StrateIndex
                        && Candidate->ParamsFingerprint == ParamsFingerprint
                        && Candidate->LayoutVersion == LayoutVersion
                        && Candidate->ManagerLifetimeId == ManagerLifetimeId
                        && Candidate->TerrainOps == TerrainOps
                        && Candidate->RegionMinX == RegionMinX
                        && Candidate->RegionMinY == RegionMinY
                        && Candidate->RegionSize == RegionSize)
                    {
                        Candidate->LastUse = Use;
                        Entry = Candidate;
                        break;
                    }
                }

                if (!Entry.IsValid())
                {
                    // Evict completed entries before admitting a new key. If
                    // the table is full of in-flight keys, do not append an
                    // unbounded waiting entry: build this request transiently
                    // under the build gate instead. The caller still gets the
                    // same immutable cache, but it is released with that
                    // classifier state rather than retained globally.
                    EvictSharedRoomGraphCacheLocked();
                    if (GSharedRoomGraphCache.Num() >= SharedRoomGraphCacheMaxEntries)
                    {
                        bBuildTransient = true;
                    }
                    else
                    {
                        Entry = MakeShared<FSharedRoomGraphCacheEntry, ESPMode::ThreadSafe>();
                        Entry->Seed = Seed;
                        Entry->StrateIndex = StrateIndex;
                        Entry->ParamsFingerprint = ParamsFingerprint;
                        Entry->LayoutVersion = LayoutVersion;
                        Entry->ManagerLifetimeId = ManagerLifetimeId;
                        Entry->TerrainOps = TerrainOps;
                        Entry->RegionMinX = RegionMinX;
                        Entry->RegionMinY = RegionMinY;
                        Entry->RegionSize = RegionSize;
                        Entry->LastUse = Use;
                        Entry->bBuilding = true;
                        Entry->ReadyEvent = FPlatformProcess::GetSynchEventFromPool(true);
                        GSharedRoomGraphCache.Add(Entry);
                        bBuild = true;
                    }
                }
            }

            if (bBuild || bBuildTransient)
            {
                const double CacheBuildCallStart = FPlatformTime::Seconds();
                AcquireSharedRoomGraphCacheBuildSlot(RegionSize);
                const double CacheBuildSlotWait = FPlatformTime::Seconds() - CacheBuildCallStart;
                TSharedPtr<FChunkSDFCache, ESPMode::ThreadSafe> BuiltCache =
                    MakeShared<FChunkSDFCache, ESPMode::ThreadSafe>();
                VoxelCaveMorphology::BuildChunkCache(
                    *BuiltCache, SearchMinX, SearchMinY, SearchMaxX, SearchMaxY,
                    Params, Seed, StrateIndex, TerrainOps, BuildSite);
                const double CacheBuildSeconds = FPlatformTime::Seconds() - CacheBuildCallStart;

                const uint64 AllocatedBytes = static_cast<uint64>(sizeof(FSharedRoomGraphCacheEntry))
                    + static_cast<uint64>(sizeof(FChunkSDFCache))
                    + static_cast<uint64>(BuiltCache->GetAllocatedSize());
                if (bBuildTransient)
                {
                    Entry = MakeShared<FSharedRoomGraphCacheEntry, ESPMode::ThreadSafe>();
                    Entry->Seed = Seed;
                    Entry->StrateIndex = StrateIndex;
                    Entry->ParamsFingerprint = ParamsFingerprint;
                    Entry->LayoutVersion = LayoutVersion;
                    Entry->ManagerLifetimeId = ManagerLifetimeId;
                    Entry->TerrainOps = TerrainOps;
                    Entry->RegionMinX = RegionMinX;
                    Entry->RegionMinY = RegionMinY;
                    Entry->RegionSize = RegionSize;
                    Entry->LastUse = 0;
                    Entry->AllocatedBytes = AllocatedBytes;
                    {
                        FScopeLock Lock(&GSharedRoomGraphCacheMutex);
                        Entry->Cache = BuiltCache;
                    }
                }
                else
                {
                    FScopeLock Lock(&GSharedRoomGraphCacheMutex);
                    Entry->Cache = BuiltCache;
                    Entry->AllocatedBytes = AllocatedBytes;
                    Entry->bBuilding = false;
                    Entry->ReadyEvent->Trigger();
                    if (AllocatedBytes <= SharedRoomGraphCacheBudgetBytes)
                    {
                        GSharedRoomGraphCacheResidentBytes += Entry->AllocatedBytes;
                        EvictSharedRoomGraphCacheLocked(Entry.Get());
                    }
                    else
                    {
                        // Publish to any waiters through the shared entry, but do not retain an
                        // oversized graph in the process-wide table. The caller's shared pointer
                        // keeps this one immutable build alive only for the current work item.
                        for (int32 Index = 0; Index < GSharedRoomGraphCache.Num(); ++Index)
                        {
                            if (GSharedRoomGraphCache[Index].Get() == Entry.Get())
                            {
                                GSharedRoomGraphCache.RemoveAtSwap(
                                    Index, 1, EAllowShrinking::No);
                                break;
                            }
                        }
                        EvictSharedRoomGraphCacheLocked();
                        UE_LOG(LogTemp, Warning,
                            TEXT("[VoxelForgeSharedCacheMemory] oversized_entry_bytes=%llu "
                                 "budget_bytes=%llu retained=0"),
                            static_cast<unsigned long long>(AllocatedBytes),
                            static_cast<unsigned long long>(SharedRoomGraphCacheBudgetBytes));
                    }
                }
                ReleaseSharedRoomGraphCacheBuildSlot(RegionSize);
                if (!bBuildTransient)
                {
                    VoxelDensityOps::ReportWorkerRoomGraphCacheFootprint();
                }
                if (RegionSize <= 2 * CHUNK_SIZE
                    || CacheBuildSeconds >= 0.25 || CacheBuildSlotWait >= 0.25)
                {
                    UE_LOG(LogTemp, Warning,
                        TEXT("[VoxelForgeSharedCacheTiming] action=build transient=%d "
                             "region=(%d,%d,%d) slot_wait=%.6f build=%.6f bytes=%llu"),
                        bBuildTransient ? 1 : 0, RegionMinX, RegionMinY, RegionSize,
                        CacheBuildSlotWait, CacheBuildSeconds,
                        static_cast<unsigned long long>(AllocatedBytes));
                }
                return Entry;
            }

            if (SnapshotSharedRoomGraphCache(Entry).IsValid())
            {
                return Entry;
            }

            // A different worker is building this exact immutable key.  Waiting on its event
            // avoids duplicate graph work without serializing unrelated region keys.
            const double CacheWaitStart = FPlatformTime::Seconds();
            Entry->ReadyEvent->Wait();
            const double CacheWaitSeconds = FPlatformTime::Seconds() - CacheWaitStart;
            if (Entry->RegionSize <= 2 * CHUNK_SIZE || CacheWaitSeconds >= 0.25)
            {
                UE_LOG(LogTemp, Warning,
                    TEXT("[VoxelForgeSharedCacheTiming] action=wait region=(%d,%d,%d) wait=%.6f"),
                    Entry->RegionMinX, Entry->RegionMinY, Entry->RegionSize, CacheWaitSeconds);
            }
        }
    }

    //=========================================================================
    // RÔLE 1 — SOURCE : GRAPHE DE SALLES / ROOM GRAPH  (TunnelNetwork)
    //=========================================================================
    // ⚠️⚠️ CET OPÉRATEUR N'A PAS RÉÉCRIT `BuildChunkCache` / `EvaluateSDFCached` : IL LES APPELLE.
    //
    // C'est LA décision de ce portage, et elle mérite d'être dite explicitement parce que la
    // tentation inverse est forte : les six autres portages sont des transcriptions littérales.
    // Celui-ci ne peut pas l'être. `BuildChunkCache` porte la discipline d'invariance de fenêtre à
    // deux régions (ARCHITECTURE §8.4) — la région COLLECT (plus large, décide QUELLES primitives
    // existent) et la région STORE (ce qu'on garde) — et c'est le code le plus délicat du plugin.
    // Le transcrire, ce serait le FORKER : deux copies d'un invariant qui dérivent, dont l'une n'est
    // testée que par un test d'équivalence qui compare... la copie à l'original.
    //
    // Ce qui EST transcrit ici, c'est la glu autour : la mémo d'index de strate, la clé de cache par
    // BOÎTE DE RECHERCHE (pas par chunk — voir plus bas), le warp, et les boucles pits/cheminées.
    // ~60 lignes déjà relues, contre ~400 lignes d'algorithme qu'on ne touche pas.
    //
    // This op CALLS the morphology cache rather than transcribing it: BuildChunkCache carries the
    // two-region window-invariance discipline (§8.4) and forking it would be the worst possible
    // outcome of a refactor whose whole point is to have ONE definition of each idea.
    class FRoomGraphSource final : public IVoxelDensityOp
    {
    public:
        FRoomGraphSource(const FStrateGenerationParams& InP, int32 InSeed,
                         const UVoxelStrateManager* InManager)
            : P(InP), Seed(InSeed), SeedU((uint32)InSeed), Manager(InManager)
            , ManagerLifetimeId(InManager ? InManager->GetCacheLifetimeId() : 0)
            , ParamsFingerprint(FCrc::MemCrc32(&InP, sizeof(InP)))
        {}

        EVoxelOpRole GetRole() const override { return EVoxelOpRole::FieldSource; }
        EVoxelOpChannelMask ChannelReads() const override { return VoxelOpChannels::None; }
        EVoxelOpChannelMask ChannelWrites() const override { return VoxelOpChannels::Sdf; }
        EVoxelOpResourceMask ProvidedResources() const override { return VoxelOpResources::RoomGeometry; }
        bool IsAdditive() const override { return false; }

        void PrepareChunk(const FVoxelOpContext& Ctx) override
        {
            // La seule chose vraiment constante par chunk ET dépendante du contexte. Le reste
            // (index de strate, pool d'ops) est résolu paresseusement dans `Eval` comme l'original,
            // parce que le cache SDF se ré-clé sur une BOÎTE, pas sur un chunk.
            LayoutVersion = Ctx.LayoutVersion;
        }

        //---------------------------------------------------------------------
        // L'ÉTAT PAR WORKER, ET POURQUOI IL EST SORTI DE `Eval`
        //---------------------------------------------------------------------
        // Les modificateurs de détail de l'étape B ont besoin de ce que CETTE source a produit pour
        // CE voxel : le cache SDF (les terrasses le ré-interrogent en Z±1, les colonnes le
        // parcourent) et l'index de la salle la plus proche (arches, dômes, pincement, biais de sol).
        // Dans l'original tout cela vit dans des `thread_local` d'une seule fonction de 1080 lignes ;
        // ici la source les possède et les expose.
        //
        // The state belongs to this source instance. The outer tunnel cache retains the immutable
        // op stack, so retaining the source's prepared SDF window with that stack avoids rebuilding
        // the room graph every time a coarse tile switches between cached chunk keys. It remains
        // worker-local because each op stack lives in the thread-local density cache.
        struct FState
        {
            // Fallback only for the invalid-owner path. Normal evaluations point at the worker-local
            // shared cache bank below, so switching exact chunk op-stacks does not rebuild the room
            // graph merely because the stack object changed.
            FChunkSDFCache Cache;
            const FChunkSDFCache* ActiveCache = nullptr;
            // Coarse density samples use the process-local immutable graph cache. Keep the entry
            // alive for the whole sequence of detail operators that consume ActiveCache.
            TSharedPtr<FSharedRoomGraphCacheEntry, ESPMode::ThreadSafe>
                SharedCacheEntry;

            /** La salle de SDF minimal pour le dernier voxel évalué. -1 = aucune. */
            int32 NearestRoom = -1;

        /** The warped SDF-space position belonging to the current voxel. */
        FVector LastWarpedPosition = FVector::ZeroVector;

            /** The unwarped position belonging to the current voxel. */
            FVector LastWorldPosition = FVector::ZeroVector;

            // The common generator tail runs after disturbances.  The tunnel structural posts
            // already evaluate this immutable result inside the stack; retain the exact same
            // sample's answer so the tail does not scan the tunnel list a second time.
            FTunnelCoreWorldEvaluation LastTunnelCoreWorldEvaluation;
            FVector LastTunnelCoreEvaluationPosition = FVector::ZeroVector;
            bool bLastTunnelCoreWorldEvaluationValid = false;
            bool bLastRoomOwnsBottom = false;
            bool bLastRoomOwnsBottomValid = false;

            /** ÉTAPE C1 — les params de la strate avec l'op de CETTE salle appliqué par-dessus.
             *  Mémo par voxel : invalidé au début de chaque `Eval`, calculé au PREMIER modificateur
             *  qui le demande. C'est ce qui reproduit le coût de l'original (une copie de struct par
             *  voxel PRÈS D'UNE SURFACE, pas partout) sans que onze opérateurs la refassent chacun. */
            FStrateGenerationParams LocalParams;
            bool bLocalParamsValid = false;

            // Window invariance is a cache-policy proof, not a sample evaluator.  Keep its result
            // beside the worker-local source state so LOD0 does not pay the full reach walk once
            // per voxel.
            bool bTileCachePolicyValid = false;
            bool bTileCachePolicyResult = false;
            bool bTileCachePolicyHasContext = false;
            int32 TileCachePolicySampleStep = 0;
            FIntVector TileCachePolicyOrigin = FIntVector::ZeroValue;
            int32 TileCachePolicyStep = 0;
            int32 TileCachePolicyCells = 0;
            uint32 TileCachePolicyFingerprint = 0xFFFFFFFFu;
            uint32 TileCachePolicyLayout = 0xFFFFFFFFu;
            uint64 TileCachePolicyManagerLifetimeId = 0;
            const TArray<FStrateTerrainOpEntry>* TileCachePolicyTerrainOps = nullptr;
        };

        FState& State() const
        {
            // The prepared SDF data lives in the worker-local keyed cache bank above.  This state
            // is only scratch for the current EvalSample: Eval resets NearestRoom and the local
            // parameter memo before its detail ops consume them.  A single TLS slot is therefore
            // safe even when this worker switches between immutable source instances, and avoids
            // a map lookup on every sample.  A mutable member is not safe because stacks can be
            // shared by concurrent readers during generation.
            static thread_local FState Scratch;
            return Scratch;
        }

        /** Le cache que la source vient de bâtir/servir pour ce voxel. Lu par les colonnes (4d). */
        const FChunkSDFCache& GetCache() const
        {
            return State().ActiveCache != nullptr ? *State().ActiveCache : State().Cache;
        }

        /** L'index de la salle la plus proche pour le dernier voxel évalué. -1 = aucune.
         *  Lu par les arches, les dômes, le pincement et le biais de sol. */
        int32 GetNearestRoomIdx() const { return State().NearestRoom; }

        /** Restore the source-owned state belonging to one sample before a later block op reads it. */
        void RestoreBlockSample(const FVoxelOpSample& Sample) const
        {
            FState& S = State();
            S.ActiveCache = Sample.RoomCache;
            S.NearestRoom = Sample.NearestRoomIndex;
            S.bLocalParamsValid = false;
            S.bLastRoomOwnsBottomValid = false;
        }

        bool TryGetLastTunnelCoreWorldEvaluation(
            FTunnelCoreWorldEvaluation& OutEvaluation) const override
        {
            if (VoxelDensityAblation::IsTunnelCoreOff())
            {
                return false;
            }
            FState& S = State();
            if (!S.bLastTunnelCoreWorldEvaluationValid
                || S.LastTunnelCoreEvaluationPosition != S.LastWorldPosition)
            {
                if (!(P.RoomDensity > 0.0f && P.RoomSpacing > 0.0f))
                {
                    return false;
                }

                // The common generator tail runs after EvalMC.  The stack has already evaluated
                // the graph source for this exact sample, but the native tunnel-core post is
                // intentionally outside the stack because it must remain after disturbances.  On
                // demand, publish the one combined result here so the tail can reuse it without
                // building/scanning a second native cache.
                const FVector& Position = S.LastWorldPosition;
                S.LastTunnelCoreWorldEvaluation = VoxelCaveMorphology::EvaluateTunnelCoreWorld(
                    Position.X, Position.Y, Position.Z, GetCache(), nullptr,
                    VoxelGenLOD::ShouldUseSpatialIndex(false),
                    &S.LastWarpedPosition,
                    S.bLastRoomOwnsBottomValid ? &S.bLastRoomOwnsBottom : nullptr);
                S.LastTunnelCoreEvaluationPosition = Position;
                S.bLastTunnelCoreWorldEvaluationValid = true;
            }
            OutEvaluation = S.LastTunnelCoreWorldEvaluation;
            return true;
        }

        /**
         * ÉTAPE C1 — L'OVERRIDE D'OP PAR SALLE. Les params de la strate avec l'op de terrain tiré
         * pour la salle la plus proche appliqué par-dessus. **ONZE des douze modificateurs de détail
         * lisent ceci au lieu de leurs propres params.**
         *
         * ⚠️ POURQUOI ÇA VIT ICI ET PAS DANS CHAQUE MODIFICATEUR.
         * `OPSTACK-DECOMPOSITION §2` disait que cette pièce n'a « pas de domicile propre » et
         * proposait de donner à chaque modificateur un prédicat « seulement dans la salle N ». La
         * difficulté venait d'une hypothèse : que chaque modificateur doive POSSÉDER ses params. Dès
         * qu'UN opérateur possède l'état partagé et que les autres le LISENT, elle disparaît — et ce
         * motif est déjà celui de `FOverhangShelfMod` ← `FSurfaceColumnSource` et de
         * `FShaftLedgeMod` ← `FShaftFieldSource`. C'est la résolution des pits/cheminées une
         * deuxième fois : ne pas inventer de mécanisme de portée, laisser une source publier.
         *
         * ⚠️ LA COPIE DE ~74 CHAMPS PAR VOXEL EST TRANSCRITE TELLE QUELLE. L'original écrit
         * `FStrateGenerationParams LocalTerrainParams = Params;` dans le bloc par voxel. C'est un
         * poste de perf réel, noté dans OPSTACK-PROGRESS ; le mémo ci-dessous garantit seulement
         * qu'on ne la fait pas ONZE fois là où l'original la fait une.
         *
         * ⚠️ **LA RUGOSITÉ (4b) N'APPELLE PAS CECI**, et c'est la lecture du code, pas une
         * simplification : dans l'original le shadow `const FStrateGenerationParams& Params =
         * LocalTerrainParams;` est déclaré DANS le bloc `if (bNearCaveSurface)` qui commence APRÈS
         * l'étape 4b. La rugosité lit les params de la strate. Douze modificateurs, onze lecteurs.
         *
         * One op owns the shared state and the rest read it — the same pattern the surface and shaft
         * ports already use, and the reason §2's "no clean home" problem evaporates. The ~74-field
         * per-voxel copy is the original's, kept. Roughness (4b) deliberately does NOT read this.
         */
        const FStrateGenerationParams& LocalParams() const
        {
            FState& S = State();
            if (!S.bLocalParamsValid)
            {
                S.LocalParams = P;
                const FChunkSDFCache& Cache = GetCache();
                if (S.NearestRoom >= 0 && Cache.Rooms.IsValidIndex(S.NearestRoom))
                {
                    const FCachedRoom& NR = Cache.Rooms[S.NearestRoom];
                    if (NR.RoomOp)
                    {
                        // N'écrit que les champs propres au type de l'op ; tout le reste garde la
                        // valeur de la strate. Exactement l'appel de l'original.
                        NR.RoomOp->ApplyTo(S.LocalParams, NR.RoomOpWeight);
                    }
                }
                S.bLocalParamsValid = true;
            }
            return S.LocalParams;
        }

        /**
         * Une requête SDF supplémentaire dans le cache courant, pour les sondes de gradient des
         * terrasses.
         *
         * ⚠️ TRANSCRIT TEL QUEL, Y COMPRIS CE QUI SEMBLE INCOHÉRENT : l'original sonde en
         * `(WorldX, WorldY, WorldZ ± 1)` — coordonnées NON warpées et Z NON divisé par
         * `VerticalScale` — alors que le champ qu'il sonde a été évalué en coordonnées WARPÉES et en
         * Z effectif. La sonde ne voit donc pas exactement le champ dont elle mesure la pente, et
         * elle ignore aussi les pits et les cheminées. C'est un écart réel de l'original ; le
         * corriger changerait le monde, donc il est NOTÉ (OPSTACK-PROGRESS) et porté à l'identique.
         *
         * Transcribed as-is including what looks wrong: the probe uses unwarped X/Y and raw Z while
         * the field it probes was evaluated warped, and it excludes pits/chimneys.
         */
        float ProbeSdfUnwarped(float X, float Y, float Z) const
        {
            return VoxelCaveMorphology::EvaluateSDFCached(
                X, Y, Z, GetCache(), P.SDFBlendRadius, nullptr,
                VoxelGenLOD::ShouldUseSpatialIndex(false));
        }

        /** Le Z « effectif » : `VerticalScale` étire le monde AVANT le bruit. Pure fonction de Z et
         *  d'un param — c'est pourquoi ce portage n'a PAS eu besoin d'un opérateur « frame »
         *  (voir la note de conception dans BuildTunnelNetworkStack). */
        FORCEINLINE float EffZ(float WorldZ) const
        {
            return (P.VerticalScale != 1.0f && P.VerticalScale > 0.0f) ? (WorldZ / P.VerticalScale)
                                                                       : WorldZ;
        }

        void Eval(float WorldX, float WorldY, float WorldZ, FVoxelOpSample& InOut) const override
        {
            VoxelDensityProfile::FScopedTimer ProfileTimer(
                VoxelDensityProfile::EBucket::RoomGraphSource);
            FState& S = State();
            S.LastWorldPosition = FVector(WorldX, WorldY, WorldZ);
            S.bLastTunnelCoreWorldEvaluationValid = false;
            S.bLastRoomOwnsBottomValid = false;

            // ⚠️ REMIS À -1 INCONDITIONNELLEMENT, ce que l'original ne fait pas : chez lui
            // `NearestRoomIdx` est un `thread_local` qui, quand `RoomDensity <= 0`, garde la valeur
            // du voxel PRÉCÉDENT. Inobservable là-bas (sans salles, `CaveSDF` reste FLT_MAX, donc
            // `bNearCaveSurface` est faux et aucun consommateur ne tourne) — mais ici les
            // consommateurs sont des objets séparés, et une valeur périmée qui traverse une frontière
            // d'opérateur est le genre de chose qu'on ne retrouve pas. On paie une écriture.
            S.NearestRoom = -1;
            // ÉTAPE C1 — le mémo d'override est PAR VOXEL. L'invalider ici, avant tout early-out,
            // est ce qui garantit qu'aucun modificateur ne lira les params de la salle du voxel
            // précédent. (Le mémo n'est PAS recalculé ici : le faire coûterait une copie de struct
            // sur chaque voxel de roc profond, ce que l'original ne paie pas.)
            S.bLocalParamsValid = false;

            const UVoxelStrateManager* LiveManager = Manager.Get();
            // A cached stack may outlive its UObject owner when an editor world is stopped. The
            // generator's manager-lifetime key prevents selecting it normally; this guard is the
            // final boundary for a task that is already inside an old stack. Keep commandlet stacks
            // (which deliberately have no manager) functional, but never evaluate stale room data.
            if (ManagerLifetimeId != 0 && LiveManager == nullptr)
            {
                S.Cache = FChunkSDFCache();
                S.SharedCacheEntry.Reset();
                S.ActiveCache = &S.Cache;
                InOut.Sdf = FLT_MAX;
                return;
            }

            if (!(P.RoomDensity > 0.0f && P.RoomSpacing > 0.0f))
            {
                S.SharedCacheEntry.Reset();
                S.ActiveCache = &S.Cache;
                return;   // Sdf reste FLT_MAX
            }

            const float EffectiveZ = EffZ(WorldZ);

            //---------------------------------------------------------------
            // WARP DE CAVE — coordonnées de REQUÊTE uniquement
            //---------------------------------------------------------------
            // ⚠️ Le warp ne s'applique QU'À la requête du graphe de salles. Les pits et les cheminées
            // plus bas lisent les coordonnées RÉELLES, et c'est délibéré dans l'original : leurs
            // ancres viennent de centres de salles NON warpés. C'est aussi pourquoi le warp n'est pas
            // un « frame » : sa portée est exactement UN opérateur, donc elle appartient à cet
            // opérateur.
            float WarpedX = WorldX, WarpedY = WorldY, WarpedZ = EffectiveZ;
            if (!VoxelDensityAblation::IsCaveWarpOff() && P.CaveWarpStrength > 0.0f)
            {
                const float WF = P.CaveWarpFrequency;
                const float WS = P.CaveWarpStrength;
                float WarpX, WarpY, WarpZ;
                VoxelNoise::Perlin3D_x3(
                    WorldX * WF + VoxelHash::SeedOffset(SeedU, 0.37f),
                    WorldY * WF + 1.3f,
                    EffectiveZ * WF + 5.7f,
                    WorldX * WF + 7.1f,
                    WorldY * WF + VoxelHash::SeedOffset(SeedU, 0.59f),
                    EffectiveZ * WF + 2.3f,
                    WorldX * WF + 11.3f,
                    WorldY * WF + 9.7f,
                    EffectiveZ * WF + VoxelHash::SeedOffset(SeedU, 0.41f),
                    WarpX, WarpY, WarpZ);
                WarpedX += WarpX * VOXEL_NOISE_SCALE * WS;
                WarpedY += WarpY * VOXEL_NOISE_SCALE * WS;
                WarpedZ += WarpZ * VOXEL_NOISE_SCALE * WS;
            }
            S.LastWarpedPosition = FVector(WarpedX, WarpedY, WarpedZ);

            //---------------------------------------------------------------
            // LE CACHE PAR BOÎTE DE RECHERCHE
            //---------------------------------------------------------------
            // ⚠️ CLÉ PAR BOÎTE, PAS PAR CHUNK, et c'est un INVARIANT DE PERF (§8.10) : les
            // échantillons de gradient interrogent `WorldX ± 1` et le warp déplace encore, donc une
            // clé « égalité de chunk » se retournait à chaque cellule de bord et reconstruisait le
            // cache (coûteux) en boucle. Comme le cache couvre la boîte + MaxInfluence, toute requête
            // DANS la boîte est correcte. Ne pas « simplifier » en clé de chunk.
            // (Les champs de cache vivent dans `FState`, au-dessus, pour que les modificateurs de
            // détail puissent les lire ; ce sont des `thread_local` partagés entre instances.)
            // ⚠️ LA CLÉ INCLUT LES PARAMS, CONTRAIREMENT À L'ORIGINAL.
            // L'original ne clé QUE sur (boîte, strate, seed) : deux jeux de params différents dans
            // la MÊME strate au MÊME seed se servent mutuellement leur cache. En production
            // `RebuildStrates` masque le trou en bougeant la strate ; en test, deux piles construites
            // côte à côte le déclenchent immédiatement. Empreinte CRC des params + LayoutVersion.
            // `FStrateGenerationParams` est du POD pur (aucun TArray/FString/pointeur), donc une CRC
            // mémoire ne peut pas donner un FAUX POSITIF ; au pire un padding donne un faux MANQUE,
            // c'est-à-dire un recalcul. On se trompe du côté du CPU, jamais du côté d'une salle fausse.

            // Index de strate — mémo (chunk-Z, version de layout), transcrit tel quel. La requête
            // vise le CENTRE de la bande, donc le résultat est une fonction pure de la clé.
            int32 StrateIdx = 0;
            if (LiveManager)
            {
                thread_local int32  SI_ChunkZ  = INT32_MAX;
                thread_local uint32 SI_Version = 0xFFFFFFFFu;
                thread_local int32  SI_Index   = 0;
                const int32 QZ = FMath::FloorToInt(WorldZ / (float)CHUNK_SIZE);
                const uint32 LV = LiveManager->GetLayoutVersion();
                if (QZ != SI_ChunkZ || LV != SI_Version)
                {
                    SI_ChunkZ  = QZ;
                    SI_Version = LV;
                    SI_Index = LiveManager->GetStrateIndex(
                        ((float)QZ + 0.5f) * CHUNK_SIZE * VOXEL_SIZE);
                }
                StrateIdx = SI_Index;
            }

            // The graph is window-invariant, so a tile worker can use the exact requesting tile as
            // its deterministic XY cache region.  The step/origin/cell extent is supplied by the
            // mesher through VoxelGenLOD TLS; ordinary point queries keep the historical region
            // policy.  The cache key retains the exact params/strate/op-pool identity, so no
            // blended chunk can borrow another chunk's room graph.
            const int32 SampleStep = FMath::Max(VoxelGenLOD::SampleStep, 1);
            FIntVector RequestedTileOrigin = FIntVector::ZeroValue;
            int32 RequestedTileStep = SampleStep;
            int32 RequestedTileCells = 0;
            const TArray<FStrateTerrainOpEntry>* TerrainOps = nullptr;
            if (LiveManager)
            {
                const int32 ChunkX = FMath::FloorToInt(WorldX / (float)CHUNK_SIZE);
                const int32 ChunkY = FMath::FloorToInt(WorldY / (float)CHUNK_SIZE);
                const int32 ChunkZ = FMath::FloorToInt(WorldZ / (float)CHUNK_SIZE);
                UVoxelStrateDefinition* Def = LiveManager->GetStrateForChunk(
                    FIntVector(ChunkX, ChunkY, ChunkZ));
                if (Def) { TerrainOps = &Def->TerrainOperations; }
            }
            const bool bTileWindowEligible = VoxelGenLOD::IsTileCacheWindowEnabled(false);
            const bool bHasTileWindowContext = VoxelGenLOD::GetThreadTileCacheWindow(
                RequestedTileOrigin, RequestedTileStep, RequestedTileCells);
            const bool bTilePolicyKeyChanged =
                !S.bTileCachePolicyValid
                || S.bTileCachePolicyHasContext
                    != (bTileWindowEligible && bHasTileWindowContext)
                || S.TileCachePolicySampleStep != RequestedTileStep
                || S.TileCachePolicyOrigin != RequestedTileOrigin
                || S.TileCachePolicyStep != RequestedTileStep
                || S.TileCachePolicyCells != RequestedTileCells
                || S.TileCachePolicyFingerprint != ParamsFingerprint
                || S.TileCachePolicyLayout != LayoutVersion
                || S.TileCachePolicyManagerLifetimeId != ManagerLifetimeId
                || S.TileCachePolicyTerrainOps != TerrainOps;
            if (bTilePolicyKeyChanged)
            {
                S.bTileCachePolicyValid = true;
                S.bTileCachePolicyHasContext = bTileWindowEligible && bHasTileWindowContext;
                S.TileCachePolicySampleStep = RequestedTileStep;
                S.TileCachePolicyOrigin = RequestedTileOrigin;
                S.TileCachePolicyStep = RequestedTileStep;
                S.TileCachePolicyCells = RequestedTileCells;
                S.TileCachePolicyFingerprint = ParamsFingerprint;
                S.TileCachePolicyLayout = LayoutVersion;
                S.TileCachePolicyManagerLifetimeId = ManagerLifetimeId;
                S.TileCachePolicyTerrainOps = TerrainOps;
                S.bTileCachePolicyResult = S.bTileCachePolicyHasContext
                    && VoxelCaveMorphology::IsRoomGraphWindowInvariant(P, TerrainOps);
            }
            const bool bUseTileCacheWindow = S.bTileCachePolicyResult;
            int32 RegionSize = 0;
            int32 RegionMinX = 0;
            int32 RegionMinY = 0;
            if (bUseTileCacheWindow)
            {
                RegionSize = static_cast<int32>(
                    static_cast<int64>(RequestedTileStep)
                    * static_cast<int64>(RequestedTileCells));
                RegionMinX = RequestedTileOrigin.X;
                RegionMinY = RequestedTileOrigin.Y;
            }
            else
            {
                // Fine requests retain the historical four-chunk worker window. The op-stack
                // tile-sized window is gated to LOD4+ (the fused path starts at LOD3), so
                // LOD0-LOD2 and the op-stack's LOD3 floor do not pay a full tile rebuild on their
                // critical path while still reusing neighbouring chunks on the same worker.
                const int32 RegionChunks = 4;
                RegionSize = CHUNK_SIZE * RegionChunks;
                const int32 RegionCellX = FMath::FloorToInt(WorldX / (float)RegionSize);
                const int32 RegionCellY = FMath::FloorToInt(WorldY / (float)RegionSize);
                RegionMinX = RegionCellX * RegionSize;
                RegionMinY = RegionCellY * RegionSize;
            }
            const float CacheExpansion = FMath::Abs(P.CaveWarpStrength)
                * VOXEL_NOISE_SCALE * VF_PerlinAbsBound
                + (bUseTileCacheWindow ? static_cast<float>(SampleStep) : 0.0f)
                + 2.0f;

            if (SampleStep > 1)
            {
                TSharedPtr<const FChunkSDFCache, ESPMode::ThreadSafe> SharedRoomGraphCache =
                    S.SharedCacheEntry.IsValid()
                        ? SnapshotSharedRoomGraphCache(S.SharedCacheEntry)
                        : TSharedPtr<const FChunkSDFCache, ESPMode::ThreadSafe>();
                const bool bSharedCacheMatches =
                    S.SharedCacheEntry.IsValid()
                    && SharedRoomGraphCache.IsValid()
                    && S.SharedCacheEntry->Seed == SeedU
                    && S.SharedCacheEntry->StrateIndex == StrateIdx
                    && S.SharedCacheEntry->ParamsFingerprint == ParamsFingerprint
                    && S.SharedCacheEntry->LayoutVersion == LayoutVersion
                    && S.SharedCacheEntry->ManagerLifetimeId == ManagerLifetimeId
                    && S.SharedCacheEntry->TerrainOps == TerrainOps
                    && S.SharedCacheEntry->RegionMinX == RegionMinX
                    && S.SharedCacheEntry->RegionMinY == RegionMinY
                    && S.SharedCacheEntry->RegionSize == RegionSize;
                if (!bSharedCacheMatches)
                {
                    S.SharedCacheEntry = FindOrBuildSharedRoomGraphCache(
                        SeedU, StrateIdx, ParamsFingerprint, LayoutVersion,
                        ManagerLifetimeId, TerrainOps,
                        RegionMinX, RegionMinY, RegionSize,
                        (float)RegionMinX - CacheExpansion,
                        (float)RegionMinY - CacheExpansion,
                        (float)(RegionMinX + RegionSize) + CacheExpansion,
                        (float)(RegionMinY + RegionSize) + CacheExpansion,
                        P,
                        ERoomGraphBuildSite::OpShared);
                    SharedRoomGraphCache = SnapshotSharedRoomGraphCache(S.SharedCacheEntry);
                }
                S.ActiveCache = SharedRoomGraphCache.Get();
            }
            else
            {
                S.SharedCacheEntry.Reset();
                FRoomGraphCacheEntry* LocalCache = FindRoomGraphCacheEntry(
                    SeedU, StrateIdx, ParamsFingerprint, LayoutVersion,
                    ManagerLifetimeId, TerrainOps,
                    RegionMinX, RegionMinY, RegionSize);
                const bool bCacheMatches = LocalCache->bValid
                    && LocalCache->Seed == SeedU
                    && LocalCache->StrateIndex == StrateIdx
                    && LocalCache->ParamsFingerprint == ParamsFingerprint
                    && LocalCache->LayoutVersion == LayoutVersion
                    && LocalCache->ManagerLifetimeId == ManagerLifetimeId
                    && LocalCache->TerrainOps == TerrainOps
                    && LocalCache->RegionMinX == RegionMinX
                    && LocalCache->RegionMinY == RegionMinY
                    && LocalCache->RegionSize == RegionSize;
                if (!bCacheMatches)
                {
                    LocalCache->bValid = false;
                    LocalCache->Seed = SeedU;
                    LocalCache->StrateIndex = StrateIdx;
                    LocalCache->ParamsFingerprint = ParamsFingerprint;
                    LocalCache->LayoutVersion = LayoutVersion;
                    LocalCache->ManagerLifetimeId = ManagerLifetimeId;
                    LocalCache->TerrainOps = TerrainOps;
                    LocalCache->RegionMinX = RegionMinX;
                    LocalCache->RegionMinY = RegionMinY;
                    LocalCache->RegionSize = RegionSize;
                    VoxelCaveMorphology::BuildChunkCache(
                        LocalCache->Cache,
                        (float)RegionMinX - CacheExpansion,
                        (float)RegionMinY - CacheExpansion,
                        (float)(RegionMinX + RegionSize) + CacheExpansion,
                        (float)(RegionMinY + RegionSize) + CacheExpansion,
                         P, SeedU, StrateIdx, TerrainOps,
                         ERoomGraphBuildSite::OpLocal);
                    LocalCache->bValid = true;
                    VoxelDensityOps::ReportWorkerRoomGraphCacheFootprint();
                }
                S.ActiveCache = &LocalCache->Cache;
            }
            const bool bUseSpatialIndex = VoxelGenLOD::ShouldUseSpatialIndex(false);
            const FVector WorldTunnelPosition(WorldX, WorldY, WorldZ);
            float CaveSDF = VoxelCaveMorphology::EvaluateSDFCached(
                WarpedX, WarpedY, WarpedZ, GetCache(), P.SDFBlendRadius,
                &S.NearestRoom, bUseSpatialIndex, &WorldTunnelPosition,
                &S.bLastRoomOwnsBottom);
            S.bLastRoomOwnsBottomValid = true;

            //---------------------------------------------------------------
            // PITS & CHEMINÉES — coordonnées RÉELLES, SmoothMin dans le même canal SDF
            //---------------------------------------------------------------
            // C'est le point que `OPSTACK-DECOMPOSITION §2` annonçait comme « le plus retors de toute
            // la décomposition » : deux primitives qui écrivent le MÊME canal que le graphe de salles
            // mais à des coordonnées NON warpées. Sous un modèle de frames il aurait fallu les sortir
            // du frame tout en gardant le canal — exprimable, mais tordu. Dans un opérateur unique la
            // difficulté disparaît : le warp est une variable locale, pas un contexte hérité.
            if (!VoxelDensityAblation::IsPitChimneySDFOff())
            {
                auto EvaluatePit = [&](int32 PitIndex)
                {
                    const FCachedPit& Pit = GetCache().Pits[PitIndex];
                    const float DZ = WorldZ - Pit.TopZ;
                    if (DZ >= Pit.BlendK) { return; }
                    if (-DZ > Pit.Depth + Pit.BlendK) { return; }

                    const float DX = WorldX - Pit.CenterX;
                    const float DY = WorldY - Pit.CenterY;
                    const float XYDistSq = DX * DX + DY * DY;
                    if (XYDistSq > Pit.BoundXYRadiusSq) { return; }

                    float PitSDF;
                    if (DZ <= 0.0f)
                    {
                        const float DepthBelow  = -DZ;
                        float FlareFactor = FMath::Clamp(1.0f - DepthBelow / Pit.FlareDist, 0.0f, 1.0f);
                        FlareFactor       = FlareFactor * FlareFactor;
                        const float EffRadius = Pit.Radius + Pit.FlareExtra * FlareFactor;
                        PitSDF = FMath::Sqrt(XYDistSq) - EffRadius;
                    }
                    else
                    {
                        PitSDF = FMath::Sqrt(XYDistSq) - (Pit.Radius + Pit.FlareExtra);
                    }

                    CaveSDF = VoxelSDF::SmoothMin(CaveSDF, PitSDF, Pit.BlendK);
                };
            if (!VoxelDensityAblation::IsPitChimneySDFOff())
            {
                VF_ForEachChunkSDFSpatialCandidate(
                    GetCache().PitSpatialIndex, GetCache().Pits.Num(),
                    WorldX, WorldY, EvaluatePit, bUseSpatialIndex);
            }

                auto EvaluateChimney = [&](int32 ChimneyIndex)
                {
                    const FCachedChimney& Chim = GetCache().Chimneys[ChimneyIndex];
                    const float DZ = WorldZ - Chim.BottomZ;
                    if (-DZ >= Chim.BlendK) { return; }
                    if (DZ > Chim.Height + Chim.BlendK) { return; }

                    const float DX = WorldX - Chim.CenterX;
                    const float DY = WorldY - Chim.CenterY;
                    const float XYDistSq = DX * DX + DY * DY;
                    if (XYDistSq > Chim.BoundXYRadiusSq) { return; }

                    float ChmSDF;
                    if (DZ >= 0.0f)
                    {
                        float FlareFactor = FMath::Clamp(1.0f - DZ / Chim.FlareDist, 0.0f, 1.0f);
                        FlareFactor       = FlareFactor * FlareFactor;
                        const float EffRadius = Chim.Radius + Chim.FlareExtra * FlareFactor;
                        ChmSDF = FMath::Sqrt(XYDistSq) - EffRadius;
                    }
                    else
                    {
                        ChmSDF = FMath::Sqrt(XYDistSq) - (Chim.Radius + Chim.FlareExtra);
                    }

                    CaveSDF = VoxelSDF::SmoothMin(CaveSDF, ChmSDF, Chim.BlendK);
                };
            if (!VoxelDensityAblation::IsPitChimneySDFOff())
            {
                VF_ForEachChunkSDFSpatialCandidate(
                    GetCache().ChimneySpatialIndex, GetCache().Chimneys.Num(),
                    WorldX, WorldY, EvaluateChimney, bUseSpatialIndex);
            }
            }

            InOut.Sdf = CaveSDF;

            // Publish the same continuous tunnel-core field that the common generator tail will
            // reassert after generic writers.  The stack does not publish or protect a separate
            // support slab.
            if (!VoxelDensityAblation::IsTunnelCoreOff())
            {
                S.LastTunnelCoreWorldEvaluation = VoxelCaveMorphology::EvaluateTunnelCoreWorld(
                    WorldX, WorldY, WorldZ, GetCache(), nullptr,
                    bUseSpatialIndex, &S.LastWarpedPosition,
                    &S.bLastRoomOwnsBottom);
                S.LastTunnelCoreEvaluationPosition = S.LastWorldPosition;
                S.bLastTunnelCoreWorldEvaluationValid = true;

                // Keep the room-owned bottom hand-off for block evaluation compatibility. The
                // authored floor remains part of the tunnel shape, but it must not win inside a
                // room's own volume.
                InOut.bHasTunnelCoreWorldEvaluation = true;
                InOut.TunnelCoreWorldSDF = S.LastTunnelCoreWorldEvaluation.SDF;
                InOut.bTunnelCoreRoomFloor =
                    S.LastTunnelCoreWorldEvaluation.bRoomFloor;
                InOut.bHasTunnelCoreSweptFloor =
                    S.LastTunnelCoreWorldEvaluation.bHasSweptFloor;
                InOut.TunnelCoreSweptFloorZ =
                    S.LastTunnelCoreWorldEvaluation.SweptFloorZ;
                InOut.TunnelCoreSweptFloorRadius =
                    S.LastTunnelCoreWorldEvaluation.SweptFloorRadius;
            }

            // These cache hand-offs remain valid when tunnel core is off; detail operators still
            // need the room graph and nearest-room ownership.
            InOut.RoomCache = &GetCache();
            InOut.NearestRoomIndex = S.NearestRoom;
        }

        //---------------------------------------------------------------------
        // L'ÉTAT PAR BOÎTE — SÉPARÉ DE `FState`, ET DÉLIBÉRÉMENT
        //---------------------------------------------------------------------
        // `EffectOverBox` construit un cache pour la boîte INTERROGÉE, qui n'est pas la boîte de
        // recherche que `Eval` construit pour le voxel courant. Les faire partager `FState::Cache`
        // serait *correct* — la discipline d'invariance de fenêtre de §8.4 garantit qu'un cache bâti
        // sur une boîte PLUS LARGE donne le même SDF par voxel — mais ça rendrait `ClassifyTile`
        // capable de perturber le cache chaud d'une génération en cours, et un jour quelqu'un
        // paierait cette élégance très cher. Un deuxième cache par worker coûte une allocation
        // amortie ; on la paie.
        //
        // Le cache et l'intervalle sont mémoïsés, et ce n'est pas du confort : les DOUZE
        // modificateurs de détail consultent le même intervalle pour la même boîte. Sans mémo,
        // une tuile coûterait treize `BuildChunkCache` au lieu d'un.
        //
        // Second per-worker cache, on purpose: sharing FState::Cache would be sound but would let
        // tile classification disturb a live generation's hot cache. The verdict is memoised because
        // all twelve detail modifiers ask the same question about the same box.
        struct FBoxState
        {
            FChunkSDFCache Cache;
            const FChunkSDFCache* ActiveCache = nullptr;
            FBox   KeyBox = FBox(ForceInit);
            int32  KeyStrate = INT32_MIN;
            uint32 KeySeed = 0;
            uint32 KeyFingerprint = 0xFFFFFFFFu;
            uint32 KeyLayout = 0xFFFFFFFFu;
            uint64 KeyManagerLifetimeId = 0;
            bool   KeyUsesLatticeProof = false;
            bool   KeyTightenWarpProof = false;
            FIntVector KeyLatticeOrigin = FIntVector::ZeroValue;
            int32  KeyLatticeStep = 1;
            bool   bValid = false;

            // A refined tile proof asks the same source about a root box and then many child
            // boxes.  The morphology cache is window-invariant, so keep the root's superset
            // cache alive for all children instead of rebuilding the graph once per child.
            FBox   CacheWindowBox = FBox(ForceInit);
            int32  CacheWindowStrate = INT32_MIN;
            uint32 CacheWindowSeed = 0;
            uint32 CacheWindowFingerprint = 0xFFFFFFFFu;
            uint32 CacheWindowLayout = 0xFFFFFFFFu;
            uint64 CacheWindowManagerLifetimeId = 0;
            const TArray<FStrateTerrainOpEntry>* CacheWindowTerrainOps = nullptr;
            bool   CacheWindowUsesLatticeProof = false;
            bool   CacheWindowTightenWarpProof = false;
            int32  CacheWindowLatticeStep = 1;
            TSharedPtr<FSharedRoomGraphCacheEntry, ESPMode::ThreadSafe> CacheWindowSharedEntry;
            int32  CacheWindowSharedRegionMinX = 0;
            int32  CacheWindowSharedRegionMinY = 0;
            int32  CacheWindowSharedRegionSize = 0;
            bool   bCacheWindowUsesShared = false;
            bool   CacheWindowUsesTileCacheWindow = false;
            FIntVector CacheWindowTileOrigin = FIntVector::ZeroValue;
            int32  CacheWindowTileStep = 0;
            int32  CacheWindowTileCells = 0;
            bool   bCacheWindowValid = false;

            // Tight warp bounds are proofs over a box. Refinement asks about nested child boxes,
            // so one bound for the current root is a sound superset for all descendants.
            FBox   TightWarpEnvelopeBox = FBox(ForceInit);
            FVector TightWarpEnvelope = FVector::ZeroVector;
            bool   bTightWarpEnvelopeValid = false;

            // Exact lattice warp queries are shared by all refined children of one classifier
            // root.  The old path rebuilt the same Perlin warp for every small child (64 times on
            // a uniform LOD0 tile).  This is a bounded worker-local scratch array, not a process
            // cache: it is keyed by the current box-proof state and never retains data for a
            // different tile or parameter set.
            TArray<FVector> ExactWarpedLatticeQueries;
            // Four-state results for the exact world-space tunnel-post check. This is a bounded
            // per-window point cache: refinement boxes share boundary vertices, but no interval or
            // tile verdict is retained here.
            TArray<uint8> ExactTailStates;
            // Per-box exact proofs reuse these bounded working arrays. They are scratch only: the
            // arrays are reset before every propagation and never retain an interval or verdict.
            // Keeping their capacity on the worker removes a heap allocation for every refined
            // LOD0 child while preserving the existing 100000-sample safety cap below.
            TArray<FVector> ExactWarpedLatticeScratch;
            TArray<FVector> ExactWorldLatticeScratch;
            FBox   ExactWarpedLatticeCacheBox = FBox(ForceInit);
            int32  ExactWarpedLatticeIX0 = 0;
            int32  ExactWarpedLatticeIY0 = 0;
            int32  ExactWarpedLatticeIZ0 = 0;
            int32  ExactWarpedLatticeNX = 0;
            int32  ExactWarpedLatticeNY = 0;
            int32  ExactWarpedLatticeNZ = 0;
            FIntVector ExactWarpedLatticeOrigin = FIntVector::ZeroValue;
            int32  ExactWarpedLatticeStep = 0;
            uint32 ExactWarpedLatticeSeed = 0xFFFFFFFFu;
            uint32 ExactWarpedLatticeFingerprint = 0xFFFFFFFFu;
            uint32 ExactWarpedLatticeLayout = 0xFFFFFFFFu;
            uint64 ExactWarpedLatticeManagerLifetimeId = 0;
            bool   bExactWarpedLatticeCacheValid = false;

            EVoxelOpEffect Verdict = EVoxelOpEffect::Both;
            FVoxelBoxSdfInterval SdfInterval;

            /** DIAGNOSTIC — combien de primitives de chaque classe atteignent la dernière boîte
             *  interrogée, et combien le cache en contenait. Lu par les tests via
             *  `VoxelDensityOps::GetLastRoomBoxDiagnostic`. N'entre dans aucune décision. */
            int32 HitRooms = 0, HitTunnels = 0, HitPits = 0, HitChimneys = 0;
            int32 NumRooms = 0, NumTunnels = 0, NumPits = 0, NumChimneys = 0;
            /** Les mêmes comptes avec une dilatation de warp NULLE, et de combien de voxels la
             *  boîte est dilatée. C'est la mesure qui manquait pendant trois builds : sans elle,
             *  « les tunnels bloquent » et « ma boîte est 125x trop grosse » sont indiscernables. */
            int32 HitRoomsNoWarp = 0, HitTunnelsNoWarp = 0;
            float WarpDilation = 0.0f;
            bool bMayHaveTunnelCoreAir = false;
            bool bMayHaveTunnelSupportFloor = false;
            bool bMayHaveTunnelCoreTail = false;
            uint32 ExactTailQueries = 0;
            uint32 ExactTailEvaluated = 0;
            uint64 ExactTailCycles = 0;
            uint64 PropagateCycles = 0;
            uint64 ExactPrimitiveCycles = 0;
            uint64 CacheWindowCycles = 0;
            int32 NumRoomFloorJoins = 0;
        };

        static FBoxState& BoxState()
        {
            thread_local FBoxState S;
            return S;
        }

        const FChunkSDFCache& GetBoxCache() const
        {
            const FBoxState& B = BoxState();
            return B.ActiveCache != nullptr ? *B.ActiveCache : B.Cache;
        }

        /**
         * ✅ LA RÉPONSE SPATIALE. La dette annoncée ici pendant tout le portage est payée.
         *
         * Ce que ça débloque, en un mot : la source publie un intervalle SDF et `FSdfConvertOp`
         * ainsi que les douze modificateurs de détail décident séparément, à partir de cet
         * intervalle, s'ils sont identités. Une boîte éloignée peut donc éteindre toute la chaîne,
         * mais aucune source ne répond au nom d'un convertisseur ou d'un modificateur.
         *
         * LE CRITÈRE — **UNE PRIMITIVE NE COMPTE PAS SI ELLE RATE SON CULL *OU* SI SON SDF RESTE
         * AU-DESSUS DU SEUIL `T`.** Une disjonction, pas une seule règle, et chaque branche gagne sur
         * une classe différente. Le détail de `T` et sa condition de validité sont dans la note
         * « LE SEUIL T » à l'intérieur de la fonction — la lire avant toute modification.
         *
         *  • branche CULL — `Eval` part de `MinSDF = FLT_MAX` et ne l'abaisse que via une primitive
         *    qui SURVIT à son cull par voxel. Aucune survivante ⇒ `Sdf` reste `FLT_MAX`. C'est la
         *    même inégalité que le cull, élevée du point à la boîte. **Meilleure pour les salles** :
         *    leur cull (`Rmax + 3K`) est plus serré que le seuil (`Rmax + T + K`).
         *  • branche SEUIL — une primitive peut survivre à son cull et rester malgré tout trop loin
         *    pour qu'un consommateur s'allume. **Meilleure pour les tunnels**, dont le cull est la
         *    sphère englobante d'une capsule : rayon ~107 pour un tube de rayon 7 long de 200.
         *
         * Mélanger les deux est sûr : les primitives de la branche cull ne contribuent RIEN, les
         * autres sont toutes ≥ `T + K`, donc le pli vaut ≥ `T` (voir la saturation de `SmoothMin`),
         * et les trois consommateurs sont éteints. Une seule raison d'échouer, dans les deux cas.
         *
         * LES TROIS CHOSES QUI RENDENT LE TEST CONSERVATIF DU BON CÔTÉ :
         *  1. le warp déplace la coordonnée de REQUÊTE, donc la boîte est dilatée de sa borne
         *     prouvable avant d'être confrontée aux salles et aux tunnels ;
         *  2. les pits et les cheminées sont interrogés en coordonnées RÉELLES (voir `Eval`), donc
         *     ils sont confrontés à la boîte NON dilatée — la dilater serait juste plus prudent, ne
         *     pas la dilater pour eux serait faux ;
         *  3. la boîte de recherche du cache est PLUS LARGE que celle de `Eval`, ce qui donne un
         *     SUR-ensemble de primitives : si rien n'atteint la boîte ici, rien ne l'atteint là-bas.
         *
         * The criterion is the per-voxel cull lifted from point to box. The source publishes only
         * its own SDF interval; the converter and the twelve modifiers consume that interval in
         * their own state-aware folds.
         */
        EVoxelOpEffect EffectOverBox(const FBox&, const FVoxelOpContext&) const override
        {
            // This source writes SDF only. It must never answer for a converter or modifier that
            // happens to follow it; the interval is the source's complete box-query contract.
            return EVoxelOpEffect::Identity;
        }

        // This operator publishes SDF and does not change Density.  Reporting zero here is
        // important: FLT_MAX means "an unknown density delta", whereas this source has no density
        // delta at all.  The SDF interval remains the source's separate box contract below.
        float MaxCarveOverBox(const FBox&, const FVoxelOpContext&) const override { return 0.0f; }
        float MaxFillOverBox(const FBox&, const FVoxelOpContext&) const override { return 0.0f; }

        /**
         * State-aware detail gate support.  A room terrain op can replace the strate-level
         * activation/strength fields for the room selected by Eval.  Until the cache tells us
         * that the queried box contains no such override, a detail bound based on P alone would be
         * unsound.  Returning true on an invalid box state deliberately loses a skip rather than
         * manufacturing one.
         */
        bool HasRoomTerrainOverrideForLastBox() const
        {
            const FBoxState& B = BoxState();
            if (!B.bValid) { return true; }
            for (const FCachedRoom& Room : GetBoxCache().Rooms)
            {
                if (Room.RoomOp != nullptr) { return true; }
            }
            return false;
        }

        float RoomColumnFillSupremumForLastBox() const
        {
            const FBoxState& B = BoxState();
            if (!B.bValid) { return FLT_MAX; }

            float Supremum = 0.0f;
            for (const FCachedColumn& Column : GetBoxCache().Columns)
            {
                if (!VoxelMath::IsFinite(Column.BaseDensity) || Column.BaseDensity < 0.0f)
                {
                    // Eval multiplies by BaseDensity.  A negative authored value reverses the
                    // declared FillOnly direction, so the state-aware caller must fall back to
                    // Both/unknown rather than pretend this is a fill envelope.
                    return FLT_MAX;
                }
                const float Contribution = Column.BaseDensity * 1.5f;
                if (!VoxelMath::IsFinite(Contribution)
                    || Supremum > FLT_MAX - Contribution)
                {
                    return FLT_MAX;
                }
                Supremum += Contribution;
            }
            return Supremum;
        }

        float FloorBiasFillSupremumForLastBox() const
        {
            const FBoxState& B = BoxState();
            if (!B.bValid) { return FLT_MAX; }

            // Eval uses NormZ² only below the room centre.  The largest value over the queried
            // box is therefore the squared distance from the room centre to Box.Min.Z, divided by
            // max(RadiusZ, 1)².  Taking the maximum over cached rooms is conservative for whichever
            // room wins the nearest-SDF query, and is finite without assuming a global room height.
            float Supremum = 0.0f;
            const float BoxMinZ = static_cast<float>(B.KeyBox.Min.Z);
            if (!VoxelMath::IsFinite(BoxMinZ)) { return FLT_MAX; }
            for (const FCachedRoom& Room : GetBoxCache().Rooms)
            {
                const float CenterZ = static_cast<float>(Room.Center.Z);
                const float RadiusZ = Room.RadiusZ;
                if (!VoxelMath::IsFinite(CenterZ) || !VoxelMath::IsFinite(RadiusZ)
                    || !VoxelMath::IsFinite(P.FloorBias) || P.FloorBias < 0.0f)
                {
                    return FLT_MAX;
                }
                const float Denominator = FMath::Max(RadiusZ, 1.0f);
                const float Below = FMath::Max(CenterZ - BoxMinZ, 0.0f);
                const float Normalized = Below / Denominator;
                const float Contribution = Normalized * Normalized * P.FloorBias;
                if (!VoxelMath::IsFinite(Contribution)) { return FLT_MAX; }
                Supremum = FMath::Max(Supremum, Contribution);
            }
            return Supremum;
        }

        /**
         * Publish a conservative interval for this source alone.
         *
         * The cache is the same morphology implementation used by Eval, but the box proof never
         * samples a downstream density op. Rooms and tunnel segments are bounded by the exact
         * geometric support that Eval's culls admit; pits and chimneys are bounded in their real
         * (unwarped) coordinates. The final SmoothMin can lower the running minimum by at most K.
         *
         * Soundness is the priority: a failed validation, an overlarge scan, or a non-finite
         * intermediate returns Unknown. That can lose a tile skip; it cannot turn a tile into a
         * false AllSolid/AllAir result.
         */
        void PropagateSdfOverBox(FVoxelBoxSdfInterval& InOut, const FBox& VoxelBox,
                                 const FVoxelOpContext& Ctx) const override
        {
            FBoxState& B = BoxState();
            const uint64 PropagateStartCycles = FPlatformTime::Cycles64();
            B.ExactTailQueries = 0;
            B.ExactTailEvaluated = 0;
            B.ExactTailCycles = 0;
            B.PropagateCycles = 0;
            B.ExactPrimitiveCycles = 0;
            B.CacheWindowCycles = 0;
            B.NumRoomFloorJoins = 0;
            const UVoxelStrateManager* LiveManager = Manager.Get();
            auto InvalidateExactLatticeScratch = [&]()
            {
                B.bExactWarpedLatticeCacheValid = false;
                B.ExactWarpedLatticeQueries.Reset();
                B.ExactTailStates.Reset();
                B.ExactWarpedLatticeScratch.Reset();
                B.ExactWorldLatticeScratch.Reset();
                B.ExactWarpedLatticeCacheBox = FBox(ForceInit);
                B.ExactWarpedLatticeIX0 = B.ExactWarpedLatticeIY0 = 0;
                B.ExactWarpedLatticeIZ0 = 0;
                B.ExactWarpedLatticeNX = B.ExactWarpedLatticeNY = 0;
                B.ExactWarpedLatticeNZ = 0;
                B.ExactWarpedLatticeOrigin = FIntVector::ZeroValue;
                B.ExactWarpedLatticeStep = 0;
                B.ExactWarpedLatticeSeed = 0xFFFFFFFFu;
                B.ExactWarpedLatticeFingerprint = 0xFFFFFFFFu;
                B.ExactWarpedLatticeLayout = 0xFFFFFFFFu;
                B.ExactWarpedLatticeManagerLifetimeId = 0;
            };

            auto Unknown = [&]()
            {
                InvalidateExactLatticeScratch();
                B.bValid = false;
                B.ActiveCache = nullptr;
                B.CacheWindowSharedEntry.Reset();
                B.CacheWindowSharedRegionMinX = 0;
                B.CacheWindowSharedRegionMinY = 0;
                B.CacheWindowSharedRegionSize = 0;
                B.bCacheWindowUsesShared = false;
                B.CacheWindowUsesTileCacheWindow = false;
                B.CacheWindowTileOrigin = FIntVector::ZeroValue;
                B.CacheWindowTileStep = 0;
                B.CacheWindowTileCells = 0;
                B.bCacheWindowValid = false;
                B.bTightWarpEnvelopeValid = false;
                B.bMayHaveTunnelCoreAir = false;
                B.bMayHaveTunnelSupportFloor = false;
                B.bMayHaveTunnelCoreTail = false;
                B.SdfInterval.SetUnknown();
                InOut.SetUnknown();
            };
            auto Finite = [](float V) { return VoxelMath::IsFinite(V); };

            if (ManagerLifetimeId != 0 && LiveManager == nullptr)
            {
                Unknown();
                return;
            }

            const float BoxMinX = (float)VoxelBox.Min.X;
            const float BoxMinY = (float)VoxelBox.Min.Y;
            const float BoxMinZ = (float)VoxelBox.Min.Z;
            const float BoxMaxX = (float)VoxelBox.Max.X;
            const float BoxMaxY = (float)VoxelBox.Max.Y;
            const float BoxMaxZ = (float)VoxelBox.Max.Z;
            if (!Finite(BoxMinX) || !Finite(BoxMinY) || !Finite(BoxMinZ)
                || !Finite(BoxMaxX) || !Finite(BoxMaxY) || !Finite(BoxMaxZ)
                || BoxMinX > BoxMaxX || BoxMinY > BoxMaxY || BoxMinZ > BoxMaxZ)
            {
                Unknown();
                return;
            }

            // These are the fields that influence the source geometry or its cache window. If an
            // authored asset leaves the proven range, the safe answer is Unknown, never a guessed
            // "reasonable" radius. Non-positive room density/spacing is an exact Eval no-op.
            const float RelevantParams[] = {
                P.RoomDensity, P.RoomSpacing, P.MinRoomRadius, P.MaxRoomRadius, P.RoomHeightRatio,
                P.RoomFloorCutMin, P.RoomFloorCutMax, P.FloorReliefStrength, P.FloorReliefFrequency,
                P.RoomShapeVariety, P.OriginRoomRadius, P.TunnelMinRadius, P.TunnelMaxRadius,
                P.TunnelDensity, P.MaxTunnelLength, P.TunnelWarpStrength, P.TunnelHorizontalBias,
                P.TunnelEndpointZOffset, P.SDFBlendRadius, P.CaveWarpStrength, P.CaveWarpFrequency,
                P.VerticalScale
            };
            for (const float V : RelevantParams)
            {
                if (!Finite(V))
                {
                    Unknown();
                    return;
                }
            }

            if (!(P.RoomDensity > 0.0f && P.RoomSpacing > 0.0f))
            {
                InvalidateExactLatticeScratch();
                B.bValid = false;
                B.ActiveCache = nullptr;
                B.CacheWindowSharedEntry.Reset();
                B.bCacheWindowUsesShared = false;
                B.bCacheWindowValid = false;
                B.NumRooms = B.NumTunnels = B.NumPits = B.NumChimneys = 0;
                B.HitRooms = B.HitTunnels = B.HitPits = B.HitChimneys = 0;
                B.HitRoomsNoWarp = B.HitTunnelsNoWarp = 0;
                B.WarpDilation = 0.0f;
                B.bMayHaveTunnelCoreAir = false;
                B.bMayHaveTunnelSupportFloor = false;
                B.bMayHaveTunnelCoreTail = false;
                B.SdfInterval.Set(FLT_MAX, FLT_MAX);
                InOut = B.SdfInterval;
                return;
            }

            if (P.MinRoomRadius < 0.0f || P.MaxRoomRadius < 0.0f || P.RoomHeightRatio < 0.0f
                || P.OriginRoomRadius < 0.0f || P.TunnelMinRadius < 0.0f || P.TunnelMaxRadius < 0.0f
                || P.SDFBlendRadius < 0.0f)
            {
                Unknown();
                return;
            }

            const int32 CX0 = FMath::FloorToInt(BoxMinX / (float)CHUNK_SIZE);
            const int32 CY0 = FMath::FloorToInt(BoxMinY / (float)CHUNK_SIZE);
            const int32 CZ0 = FMath::FloorToInt(BoxMinZ / (float)CHUNK_SIZE);
            const int32 CX1 = FMath::FloorToInt(BoxMaxX / (float)CHUNK_SIZE);
            const int32 CY1 = FMath::FloorToInt(BoxMaxY / (float)CHUNK_SIZE);
            const int32 CZ1 = FMath::FloorToInt(BoxMaxZ / (float)CHUNK_SIZE);

            const int64 SpanX = (int64)CX1 - (int64)CX0 + 1;
            const int64 SpanY = (int64)CY1 - (int64)CY0 + 1;
            const int64 SpanZ = (int64)CZ1 - (int64)CZ0 + 1;
            if (SpanX <= 0 || SpanY <= 0 || SpanZ <= 0)
            {
                Unknown();
                return;
            }

            int32 StrateIdx = 0;
            const TArray<FStrateTerrainOpEntry>* TerrainOps = nullptr;
            if (LiveManager)
            {
                StrateIdx = LiveManager->GetStrateIndex(
                    ((float)CZ0 + 0.5f) * CHUNK_SIZE * VOXEL_SIZE);
                for (int32 CZ = CZ0 + 1; CZ <= CZ1; ++CZ)
                {
                    if (LiveManager->GetStrateIndex(
                            ((float)CZ + 0.5f) * CHUNK_SIZE * VOXEL_SIZE) != StrateIdx)
                    {
                        Unknown();
                        return;
                    }
                }

                // A room operation is part of the graph cache. Every Z slice covered by this
                // proof must resolve to the same definition before one cache can represent it.
                UVoxelStrateDefinition* Def0 = nullptr;
                for (int32 CZ = CZ0; CZ <= CZ1; ++CZ)
                {
                    UVoxelStrateDefinition* Def =
                        LiveManager->GetStrateForChunk(FIntVector(0, 0, CZ));
                    if (CZ == CZ0)
                    {
                        Def0 = Def;
                    }
                    else if (Def != Def0)
                    {
                        Unknown();
                        return;
                    }
                }
                if (Def0) { TerrainOps = &Def0->TerrainOperations; }
            }

            FIntVector RequestedTileOrigin = FIntVector::ZeroValue;
            int32 RequestedTileStep = FMath::Max(Ctx.Step, 1);
            int32 RequestedTileCells = 0;
            const bool bUseTileCacheWindow = VoxelGenLOD::IsTileCacheWindowEnabled(false)
                && VoxelGenLOD::GetThreadTileCacheWindow(
                    RequestedTileOrigin, RequestedTileStep, RequestedTileCells)
                && VoxelCaveMorphology::IsRoomGraphWindowInvariant(P, TerrainOps);
            const float RequestedTileExtent = static_cast<float>(
                static_cast<int64>(RequestedTileStep)
                * static_cast<int64>(RequestedTileCells));
            // FillChunk includes one sample-step of mesher halo around the requested tile.
            // Keep the tile footprint as the shared-cache identity, but let every block in that
            // halo reuse the same cache window.  The morphology search below includes the same
            // coverage, so this widens reuse without changing the set of values evaluated.
            const FBox CacheWindowCoverageBox = bUseTileCacheWindow
                ? FBox(
                    FVector(
                        static_cast<float>(RequestedTileOrigin.X)
                            - static_cast<float>(RequestedTileStep),
                        static_cast<float>(RequestedTileOrigin.Y)
                            - static_cast<float>(RequestedTileStep),
                        static_cast<float>(RequestedTileOrigin.Z)
                            - static_cast<float>(RequestedTileStep)),
                    FVector(
                        static_cast<float>(RequestedTileOrigin.X) + RequestedTileExtent,
                        static_cast<float>(RequestedTileOrigin.Y) + RequestedTileExtent,
                        static_cast<float>(RequestedTileOrigin.Z) + RequestedTileExtent)
                        + FVector(
                            static_cast<float>(RequestedTileStep),
                            static_cast<float>(RequestedTileStep),
                            static_cast<float>(RequestedTileStep)))
                : VoxelBox;
            const int64 RequestedTileChunkSpan = bUseTileCacheWindow
                ? FMath::Max<int64>(
                    1,
                    (static_cast<int64>(RequestedTileExtent) + CHUNK_SIZE - 1)
                        / CHUNK_SIZE)
                : 0;

            // A coarse tile is allowed to warm the proof, but never by allocating a morphology
            // cache for the whole tile.  At LOD3/4 the root can cover hundreds or thousands of
            // chunk keys; that is a proof-domain size, not a useful working set.  Descendants
            // are still queried normally and can use the bounded shared windows below.  Unknown
            // is deliberately the only answer here: it cannot authorize an empty-tile skip.
            constexpr int64 MaxLatticeProofChunkVolume = 512;
            if (Ctx.bUseLatticeProof
                && SpanX * SpanY * SpanZ > MaxLatticeProofChunkVolume
                && !(bUseTileCacheWindow
                    && RequestedTileChunkSpan <= 64
                    && SpanZ <= 16))
            {
                Unknown();
                return;
            }

            // The ordinary (non-lattice) proof keeps its original bounded query contract.  A
            // tile proof is different: its first query is the root box and its descendants are
            // all inside that box.  Build the root cache once even when the root spans more than
            // 64 chunk keys, then let the bounded child boxes use that same superset.  This is
            // what prevents a step-32 tile from paying one graph bake per coarse sample.
            constexpr int64 MaxNonLatticeChunkSpan = 64;
            if (!Ctx.bUseLatticeProof && SpanX * SpanY * SpanZ > MaxNonLatticeChunkSpan)
            {
                Unknown();
                return;
            }

            const uint32 LV = Ctx.LayoutVersion;
            auto IsSharedCacheCurrent = [&]() -> bool
            {
                const FSharedRoomGraphCacheEntry* Entry = B.CacheWindowSharedEntry.Get();
                return Entry != nullptr
                    && Entry->Seed == SeedU
                    && Entry->StrateIndex == StrateIdx
                    && Entry->ParamsFingerprint == ParamsFingerprint
                    && Entry->LayoutVersion == LayoutVersion
                    && Entry->ManagerLifetimeId == ManagerLifetimeId
                    && Entry->TerrainOps == TerrainOps
                    && Entry->RegionMinX == B.CacheWindowSharedRegionMinX
                    && Entry->RegionMinY == B.CacheWindowSharedRegionMinY
                    && Entry->RegionSize == B.CacheWindowSharedRegionSize;
            };
            if (B.bValid && B.KeyBox == VoxelBox && B.KeyStrate == StrateIdx
                && B.KeySeed == SeedU && B.KeyFingerprint == ParamsFingerprint && B.KeyLayout == LV
                && B.KeyManagerLifetimeId == ManagerLifetimeId
                && B.KeyUsesLatticeProof == Ctx.bUseLatticeProof
                && (!B.bCacheWindowUsesShared || IsSharedCacheCurrent())
                && B.KeyTightenWarpProof == Ctx.bTightenWarpProof
                && (!Ctx.bUseLatticeProof
                    || (B.KeyLatticeOrigin == Ctx.LatticeOriginVoxels
                        && B.KeyLatticeStep == Ctx.Step)))
            {
                InOut = B.SdfInterval;
                return;
            }

            const float EffectiveMinZ = (P.VerticalScale > 0.0f && P.VerticalScale != 1.0f)
                                      ? BoxMinZ / P.VerticalScale : BoxMinZ;
            const float EffectiveMaxZ = (P.VerticalScale > 0.0f && P.VerticalScale != 1.0f)
                                      ? BoxMaxZ / P.VerticalScale : BoxMaxZ;
            const float GlobalWarp = (P.CaveWarpStrength > 0.0f)
                                   ? P.CaveWarpStrength * VOXEL_NOISE_SCALE * VF_PerlinAbsBound
                                   : 0.0f;
            if (!Finite(GlobalWarp) || !Finite(EffectiveMinZ) || !Finite(EffectiveMaxZ))
            {
                Unknown();
                return;
            }

            FVector WarpEnvelope(GlobalWarp, GlobalWarp, GlobalWarp);
            // The finite Perlin interval is useful at the root too when the root is one ordinary
            // LOD0 tile wide. Compute it once and retain it as a superset for every descendant;
            // otherwise each refined child repeats the same finite-domain envelope work. The
            // helper falls back to the global theorem for larger/non-finite noise domains, so
            // widening this gate cannot weaken soundness.
            const bool bUseFiniteWarpEnvelope = Ctx.bTightenWarpProof
                && FMath::Max3(BoxMaxX - BoxMinX, BoxMaxY - BoxMinY, BoxMaxZ - BoxMinZ)
                    <= 32.0f * static_cast<float>(FMath::Max(Ctx.Step, 1));
            if (bUseFiniteWarpEnvelope && P.CaveWarpStrength > 0.0f
                && P.CaveWarpFrequency > 0.0f)
            {
                const bool bCanReuseTightEnvelope = B.bTightWarpEnvelopeValid
                    && B.TightWarpEnvelopeBox.Min.X <= BoxMinX
                    && B.TightWarpEnvelopeBox.Min.Y <= BoxMinY
                    && B.TightWarpEnvelopeBox.Min.Z <= BoxMinZ
                    && B.TightWarpEnvelopeBox.Max.X >= BoxMaxX
                    && B.TightWarpEnvelopeBox.Max.Y >= BoxMaxY
                    && B.TightWarpEnvelopeBox.Max.Z >= BoxMaxZ;
                if (bCanReuseTightEnvelope)
                {
                    WarpEnvelope = B.TightWarpEnvelope;
                }
                else
                {
                    const float WarpAmplitude = P.CaveWarpStrength * VOXEL_NOISE_SCALE;
                    WarpEnvelope.X = WarpAmplitude * VF_PerlinAbsBoundOverWorldBox(
                        BoxMinX, BoxMaxX, BoxMinY, BoxMaxY, EffectiveMinZ, EffectiveMaxZ,
                        P.CaveWarpFrequency, VoxelHash::SeedOffset(SeedU, 0.37f), 1.3f, 5.7f);
                    WarpEnvelope.Y = WarpAmplitude * VF_PerlinAbsBoundOverWorldBox(
                        BoxMinX, BoxMaxX, BoxMinY, BoxMaxY, EffectiveMinZ, EffectiveMaxZ,
                        P.CaveWarpFrequency, 7.1f, VoxelHash::SeedOffset(SeedU, 0.59f), 2.3f);
                    WarpEnvelope.Z = WarpAmplitude * VF_PerlinAbsBoundOverWorldBox(
                        BoxMinX, BoxMaxX, BoxMinY, BoxMaxY, EffectiveMinZ, EffectiveMaxZ,
                        P.CaveWarpFrequency, 11.3f, 9.7f, VoxelHash::SeedOffset(SeedU, 0.41f));
                    B.TightWarpEnvelopeBox = VoxelBox;
                    B.TightWarpEnvelope = WarpEnvelope;
                    B.bTightWarpEnvelopeValid = true;
                }
                if (!Finite((float)WarpEnvelope.X) || !Finite((float)WarpEnvelope.Y)
                    || !Finite((float)WarpEnvelope.Z))
                {
                    Unknown();
                    return;
                }
            }
            // A tile cache is a graph cache, not a proof-result cache.  Its search envelope must
            // therefore be identical for the root and every block/child that asks about the
            // tile.  In particular, a finite tight warp envelope is allowed to sharpen the
            // interval proof below, but it must not make the first classifier block publish a
            // smaller morphology cache for later blocks to reuse.
            const float CacheWarpX = bUseTileCacheWindow
                ? GlobalWarp : FMath::Max((float)WarpEnvelope.X, 0.0f);
            const float CacheWarpY = bUseTileCacheWindow
                ? GlobalWarp : FMath::Max((float)WarpEnvelope.Y, 0.0f);
            const float CacheWarpZ = bUseTileCacheWindow
                ? GlobalWarp : FMath::Max((float)WarpEnvelope.Z, 0.0f);
            const float CacheSearchMinX = bUseTileCacheWindow
                ? static_cast<float>(RequestedTileOrigin.X)
                    - CacheWarpX
                    - static_cast<float>(RequestedTileStep) - 2.0f
                : BoxMinX - CacheWarpX - 2.0f;
            const float CacheSearchMinY = bUseTileCacheWindow
                ? static_cast<float>(RequestedTileOrigin.Y)
                    - CacheWarpY
                    - static_cast<float>(RequestedTileStep) - 2.0f
                : BoxMinY - CacheWarpY - 2.0f;
            const float CacheSearchMaxX = bUseTileCacheWindow
                ? static_cast<float>(RequestedTileOrigin.X) + RequestedTileExtent
                    + CacheWarpX
                    + static_cast<float>(RequestedTileStep) + 2.0f
                : BoxMaxX + CacheWarpX + 2.0f;
            const float CacheSearchMaxY = bUseTileCacheWindow
                ? static_cast<float>(RequestedTileOrigin.Y) + RequestedTileExtent
                    + CacheWarpY
                    + static_cast<float>(RequestedTileStep) + 2.0f
                : BoxMaxY + CacheWarpY + 2.0f;
            const float CacheSearchMinZ = bUseTileCacheWindow
                ? static_cast<float>(RequestedTileOrigin.Z)
                    - CacheWarpZ
                    - static_cast<float>(RequestedTileStep) - 2.0f
                : BoxMinZ - CacheWarpZ - 2.0f;
            const float CacheSearchMaxZ = bUseTileCacheWindow
                ? static_cast<float>(RequestedTileOrigin.Z) + RequestedTileExtent
                    + CacheWarpZ
                    + static_cast<float>(RequestedTileStep) + 2.0f
                : BoxMaxZ + CacheWarpZ + 2.0f;
            // The search window is a superset of every warped point in the box, with the same
            // two-voxel gradient margin used by Eval's cache path.  In lattice mode, retain the
            // first/root window and reuse it for every refined child.  A larger morphology
            // window is sound for a child because BuildChunkCache's collect/store contract is
            // explicitly window-invariant; it is also the only way to keep refinement from
            // turning into one graph build per child box.
            const bool bWindowKeyMatches = B.bCacheWindowValid
                && B.CacheWindowStrate == StrateIdx
                && B.CacheWindowSeed == SeedU
                && B.CacheWindowFingerprint == ParamsFingerprint
                && B.CacheWindowLayout == LV
                && B.CacheWindowManagerLifetimeId == ManagerLifetimeId
                && B.CacheWindowTerrainOps == TerrainOps
                && B.CacheWindowUsesLatticeProof == Ctx.bUseLatticeProof
                && B.CacheWindowUsesTileCacheWindow == bUseTileCacheWindow
                && (!bUseTileCacheWindow
                    || (B.CacheWindowTileOrigin == RequestedTileOrigin
                        && B.CacheWindowTileStep == RequestedTileStep
                        && B.CacheWindowTileCells == RequestedTileCells))
                // A non-tight cache is built from the global warp envelope and is therefore a
                // superset cache for a later tight retry.  The reverse is not safe: a tight cache
                // may omit primitives needed by the conservative global-envelope query.
                && (!Ctx.bUseLatticeProof
                    // Tight queries may use either an equally tight cache or the larger global
                    // envelope cache. A global query must not reuse a tight cache.
                    || Ctx.bTightenWarpProof
                    || !B.CacheWindowTightenWarpProof)
                 // The graph window is independent of which block's lattice origin requested it;
                 // retain only the step because it controls the conservative finite-envelope gate
                 // for non-tile proof windows.
                 && (!Ctx.bUseLatticeProof
                     || B.CacheWindowLatticeStep == Ctx.Step)
                 && (!B.bCacheWindowUsesShared || IsSharedCacheCurrent())
                 && B.CacheWindowBox.Min.X <= BoxMinX
                && B.CacheWindowBox.Min.Y <= BoxMinY
                && B.CacheWindowBox.Min.Z <= BoxMinZ
                && B.CacheWindowBox.Max.X >= BoxMaxX
                && B.CacheWindowBox.Max.Y >= BoxMaxY
                && B.CacheWindowBox.Max.Z >= BoxMaxZ;

            if (!bWindowKeyMatches)
            {
                const uint64 CacheWindowStartCycles = FPlatformTime::Cycles64();
                InvalidateExactLatticeScratch();
                B.ActiveCache = nullptr;
                B.CacheWindowSharedEntry.Reset();
                B.CacheWindowSharedRegionMinX = 0;
                B.CacheWindowSharedRegionMinY = 0;
                B.CacheWindowSharedRegionSize = 0;
                B.bCacheWindowUsesShared = false;
                B.bTightWarpEnvelopeValid = false;

                // The morphology builder is window-invariant.  For the bounded lattice boxes,
                // reuse the worker-local graph cache used by Eval, but at a 128/256-voxel region
                // granularity.  A classifier root otherwise rebuilds the same collect/NN graph
                // for every neighbouring tile before the regular density path has a chance to
                // populate its cache.  The shared window is always built from the global warp
                // envelope, so it is a superset of this tight query and can only add conservative
                // primitives to the interval.
                const bool bFeatureFree =
                    !VoxelCaveMorphology::MayHaveFeatureInSearchBox(
                        CacheSearchMinX,
                        CacheSearchMinY,
                        CacheSearchMaxX,
                        CacheSearchMaxY,
                        P, SeedU, StrateIdx, true,
                        CacheSearchMinZ,
                        CacheSearchMaxZ,
                        TerrainOps);
                const int64 MaxSharedChunkSpan = Ctx.Step >= 32 ? 32 : 16;
                bool bUsedSharedCache = false;
                const bool bTileWithinSharedBudget = bUseTileCacheWindow
                    && RequestedTileChunkSpan <= 64;
                if (!bFeatureFree
                    && Ctx.bUseLatticeProof
                    // Fine LOD0/LOD1 requests are latency-sensitive and each worker already has
                    // a bounded graph cache. Sharing their tiny 64-voxel windows makes otherwise
                    // sub-millisecond empty proofs wait behind a different worker's build (the
                    // build itself is cheap). Coarse samples keep the shared window, where its
                    // larger footprint prevents thousands of per-chunk rebuilds.
                    && Ctx.Step > 1
                    && (bTileWithinSharedBudget
                        || (SpanX <= MaxSharedChunkSpan
                            && SpanY <= MaxSharedChunkSpan)))
                {
                    // Coarser LODs retain a shared immutable cache. With the tile policy its key
                    // is the exact tile footprint; the fallback keeps the historical expanding
                    // four-chunk working set for point/non-tile callers.
                    int32 SharedRegionSize = bUseTileCacheWindow
                        ? static_cast<int32>(RequestedTileExtent)
                        : 4 * CHUNK_SIZE;
                    int32 SharedRegionMinX = bUseTileCacheWindow
                        ? RequestedTileOrigin.X : 0;
                    int32 SharedRegionMinY = bUseTileCacheWindow
                        ? RequestedTileOrigin.Y : 0;
                    bool bCoversBox = bUseTileCacheWindow
                        ? (CacheWindowCoverageBox.Min.X <= BoxMinX
                            && CacheWindowCoverageBox.Min.Y <= BoxMinY
                            && CacheWindowCoverageBox.Max.X >= BoxMaxX
                            && CacheWindowCoverageBox.Max.Y >= BoxMaxY)
                        : false;
                    for (int32 Attempt = 0; !bUseTileCacheWindow && Attempt < 6; ++Attempt)
                    {
                        SharedRegionMinX = FMath::FloorToInt(
                            BoxMinX / (float)SharedRegionSize) * SharedRegionSize;
                        SharedRegionMinY = FMath::FloorToInt(
                            BoxMinY / (float)SharedRegionSize) * SharedRegionSize;
                        bCoversBox = BoxMaxX <= (float)(SharedRegionMinX + SharedRegionSize)
                            && BoxMaxY <= (float)(SharedRegionMinY + SharedRegionSize);
                        if (bCoversBox) { break; }
                        SharedRegionSize *= 2;
                    }

                    if (bCoversBox
                        && (bUseTileCacheWindow
                            || SharedRegionSize
                               <= static_cast<int32>(MaxSharedChunkSpan) * CHUNK_SIZE))
                    {
                        TSharedPtr<FSharedRoomGraphCacheEntry, ESPMode::ThreadSafe> SharedCache =
                            FindOrBuildSharedRoomGraphCache(
                                SeedU, StrateIdx, ParamsFingerprint, LayoutVersion,
                                ManagerLifetimeId, TerrainOps,
                                SharedRegionMinX, SharedRegionMinY, SharedRegionSize,
                                bUseTileCacheWindow
                                    ? CacheSearchMinX
                                    : (float)SharedRegionMinX
                                        - (FMath::Abs(P.CaveWarpStrength)
                                            * VOXEL_NOISE_SCALE * VF_PerlinAbsBound + 2.0f),
                                bUseTileCacheWindow
                                    ? CacheSearchMinY
                                    : (float)SharedRegionMinY
                                        - (FMath::Abs(P.CaveWarpStrength)
                                            * VOXEL_NOISE_SCALE * VF_PerlinAbsBound + 2.0f),
                                bUseTileCacheWindow
                                    ? CacheSearchMaxX
                                    : (float)(SharedRegionMinX + SharedRegionSize)
                                        + (FMath::Abs(P.CaveWarpStrength)
                                            * VOXEL_NOISE_SCALE * VF_PerlinAbsBound + 2.0f),
                                bUseTileCacheWindow
                                    ? CacheSearchMaxY
                                    : (float)(SharedRegionMinY + SharedRegionSize)
                                        + (FMath::Abs(P.CaveWarpStrength)
                                            * VOXEL_NOISE_SCALE * VF_PerlinAbsBound + 2.0f),
                                P,
                                ERoomGraphBuildSite::ClassifierShared);
                        const TSharedPtr<const FChunkSDFCache, ESPMode::ThreadSafe>
                            SharedRoomGraphCache = SnapshotSharedRoomGraphCache(SharedCache);
                        B.ActiveCache = SharedRoomGraphCache.Get();
                        B.CacheWindowSharedEntry = SharedCache;
                        B.CacheWindowSharedRegionMinX = SharedRegionMinX;
                        B.CacheWindowSharedRegionMinY = SharedRegionMinY;
                        B.CacheWindowSharedRegionSize = SharedRegionSize;
                        B.bCacheWindowUsesShared = true;
                        bUsedSharedCache = true;
                    }
                }

                if (!bUsedSharedCache)
                {
                    if (bFeatureFree)
                    {
                        // Keep an empty worker-local cache object as the active
                        // window. Its arrays retain capacity, but no graph
                        // build or player-fit bake is paid for this box.
                        B.Cache.Release();
                    }
                    else
                    {
                        VoxelCaveMorphology::BuildChunkCache(
                            B.Cache,
                            CacheSearchMinX,
                            CacheSearchMinY,
                            CacheSearchMaxX,
                            CacheSearchMaxY,
                            P, SeedU, StrateIdx, TerrainOps,
                             ERoomGraphBuildSite::ClassifierLocal);
                    }
                    B.ActiveCache = &B.Cache;
                }

                B.CacheWindowBox = CacheWindowCoverageBox;
                B.CacheWindowStrate = StrateIdx;
                B.CacheWindowSeed = SeedU;
                B.CacheWindowFingerprint = ParamsFingerprint;
                B.CacheWindowLayout = LV;
                B.CacheWindowManagerLifetimeId = ManagerLifetimeId;
                B.CacheWindowTerrainOps = TerrainOps;
                B.CacheWindowUsesLatticeProof = Ctx.bUseLatticeProof;
                B.CacheWindowUsesTileCacheWindow = bUseTileCacheWindow;
                B.CacheWindowTileOrigin = bUseTileCacheWindow
                    ? RequestedTileOrigin : FIntVector::ZeroValue;
                B.CacheWindowTileStep = bUseTileCacheWindow ? RequestedTileStep : 0;
                B.CacheWindowTileCells = bUseTileCacheWindow ? RequestedTileCells : 0;
                B.CacheWindowTightenWarpProof = bUseTileCacheWindow
                    ? false
                    : bUsedSharedCache
                    ? false : Ctx.bTightenWarpProof;
                B.CacheWindowLatticeStep = Ctx.Step;
                B.bCacheWindowValid = true;
                B.CacheWindowCycles = FPlatformTime::Cycles64() - CacheWindowStartCycles;
            }
            else if (B.bCacheWindowUsesShared)
            {
                B.ActiveCache = SnapshotSharedRoomGraphCache(
                    B.CacheWindowSharedEntry).Get();
            }
            // Large root boxes are only a cache warm-up.  Their interval is intentionally still
            // unknown; proof resumes at bounded descendants where all bounds below are finite.
            if (SpanX * SpanY * SpanZ > 64)
            {
                // Keep the freshly built window alive for the descendants.  This is deliberately
                // different from Unknown(): the root is not invalid, it is merely too large to
                // publish a finite interval in one pass.
                B.bValid = false;
                B.bMayHaveTunnelCoreAir = false;
                B.bMayHaveTunnelSupportFloor = false;
                B.bMayHaveTunnelCoreTail = false;
                B.SdfInterval.SetUnknown();
                InOut.SetUnknown();
                return;
            }

            const FVector QMin(BoxMinX - (float)WarpEnvelope.X,
                               BoxMinY - (float)WarpEnvelope.Y,
                               EffectiveMinZ - (float)WarpEnvelope.Z);
            const FVector QMax(BoxMaxX + (float)WarpEnvelope.X,
                               BoxMaxY + (float)WarpEnvelope.Y,
                               EffectiveMaxZ + (float)WarpEnvelope.Z);
            const FVector RMin(BoxMinX, BoxMinY, BoxMinZ);
            const FVector RMax(BoxMaxX, BoxMaxY, BoxMaxZ);
            if (!Finite((float)QMin.X) || !Finite((float)QMin.Y) || !Finite((float)QMin.Z)
                || !Finite((float)QMax.X) || !Finite((float)QMax.Y) || !Finite((float)QMax.Z))
            {
                Unknown();
                return;
            }

            // In lattice mode the queried box is a proof domain for the exact MC samples, not
            // for every continuous point between them.  The room/tunnel bounds below therefore
            // use the nearest lattice coordinate in each axis and subtract the same per-axis warp
            // envelope as the continuous proof.  A sub-box containing no lattice point has no
            // samples to constrain and contributes the exact SDF identity.
            const bool bUseLatticeProof = Ctx.bUseLatticeProof;
            const float LatticeOriginX = static_cast<float>(Ctx.LatticeOriginVoxels.X);
            const float LatticeOriginY = static_cast<float>(Ctx.LatticeOriginVoxels.Y);
            const float LatticeOriginZ = (P.VerticalScale > 0.0f && P.VerticalScale != 1.0f)
                ? static_cast<float>(Ctx.LatticeOriginVoxels.Z) / P.VerticalScale
                : static_cast<float>(Ctx.LatticeOriginVoxels.Z);
            const float LatticeStepXY = static_cast<float>(Ctx.Step);
            const float LatticeStepZ = (P.VerticalScale > 0.0f && P.VerticalScale != 1.0f)
                ? static_cast<float>(Ctx.Step) / P.VerticalScale
                : static_cast<float>(Ctx.Step);
            if (bUseLatticeProof)
            {
                auto HasAxisSample = [](float Min, float Max, float Origin, float Step) -> bool
                {
                    return Step > 0.0f
                        && FMath::CeilToInt((Min - Origin) / Step - 1.0e-4f)
                           <= FMath::FloorToInt((Max - Origin) / Step + 1.0e-4f);
                };
                if (!HasAxisSample(BoxMinX, BoxMaxX, LatticeOriginX, LatticeStepXY)
                    || !HasAxisSample(BoxMinY, BoxMaxY, LatticeOriginY, LatticeStepXY)
                    || !HasAxisSample(BoxMinZ, BoxMaxZ, LatticeOriginZ, LatticeStepZ))
                {
                    InvalidateExactLatticeScratch();
                    B.bValid = false;
                    B.ActiveCache = nullptr;
                B.CacheWindowSharedEntry.Reset();
                    B.bCacheWindowUsesShared = false;
                    B.bCacheWindowValid = false;
                    B.SdfInterval.Set(FLT_MAX, FLT_MAX);
                    InOut = B.SdfInterval;
                    return;
                }
            }

            // A tight retry is a proof over the exact lattice the mesher will read.  Keep both
            // coordinate spaces: the room graph is queried in warped SDF space, while pits and
            // chimneys deliberately use the original world-space sample.  The old implementation
            // retained only an AABB at LOD0; that still admitted a tunnel when the warped samples
            // themselves missed it.  The exact primitive minima below remove that false overlap
            // without changing the lattice or the density evaluator used by the mesher.
            TArray<FVector>& ExactLatticeWarpedQueries = B.ExactWarpedLatticeScratch;
            TArray<FVector>& ExactLatticeWorldQueries = B.ExactWorldLatticeScratch;
            ExactLatticeWarpedQueries.Reset();
            ExactLatticeWorldQueries.Reset();
            FVector ExactLatticeQueryMin(FLT_MAX, FLT_MAX, FLT_MAX);
            FVector ExactLatticeQueryMax(-FLT_MAX, -FLT_MAX, -FLT_MAX);
            int32 ExactIX0 = 0, ExactIY0 = 0, ExactIZ0 = 0;
            int32 ExactNX = 0, ExactNY = 0, ExactNZ = 0;
            struct FExactLatticeBlock
            {
                int32 IX0 = 0, IX1 = -1;
                int32 IY0 = 0, IY1 = -1;
                int32 IZ0 = 0, IZ1 = -1;
                FVector WarpedMin = FVector(FLT_MAX, FLT_MAX, FLT_MAX);
                FVector WarpedMax = FVector(-FLT_MAX, -FLT_MAX, -FLT_MAX);
                FVector WorldMin = FVector(FLT_MAX, FLT_MAX, FLT_MAX);
                FVector WorldMax = FVector(-FLT_MAX, -FLT_MAX, -FLT_MAX);
            };
            TArray<FExactLatticeBlock, TInlineAllocator<128>> ExactLatticeBlocks;
            const float MaxBoxExtent = FMath::Max3(
                BoxMaxX - BoxMinX, BoxMaxY - BoxMinY, BoxMaxZ - BoxMinZ);
            const float ExactChildExtent = 16.0f * (float)FMath::Max(Ctx.Step, 1);
            const bool bTryExactLatticeWarp = bUseLatticeProof && Ctx.bTightenWarpProof
                // The root is a cache warm-up.  Exact warped lattice distances are only needed
                // once the refinement has reduced the domain to a child; this avoids rebuilding
                // a 33^3 query list before the eight 17^3 child proofs that actually decide the
                // residual solid tile.  A non-exact root remains conservative and cannot create a
                // skip on its own.
                // Include a box exactly one coarse child wide. This lets a depth-one LOD0 node
                // use the exact warped lattice certificate before it fans out into eight smaller
                // proofs; the certificate remains conservative because it samples only the same
                // lattice points the mesher reads.
                && MaxBoxExtent <= ExactChildExtent;
            if (bTryExactLatticeWarp)
            {
                 const int32 IX0 = FMath::CeilToInt(
                    (BoxMinX - LatticeOriginX) / LatticeStepXY - 1.0e-4f);
                const int32 IY0 = FMath::CeilToInt(
                    (BoxMinY - LatticeOriginY) / LatticeStepXY - 1.0e-4f);
                const int32 IZ0 = FMath::CeilToInt(
                    (BoxMinZ - static_cast<float>(Ctx.LatticeOriginVoxels.Z))
                    / static_cast<float>(Ctx.Step) - 1.0e-4f);
                const int32 IX1 = FMath::FloorToInt(
                    (BoxMaxX - LatticeOriginX) / LatticeStepXY + 1.0e-4f);
                const int32 IY1 = FMath::FloorToInt(
                    (BoxMaxY - LatticeOriginY) / LatticeStepXY + 1.0e-4f);
                const int32 IZ1 = FMath::FloorToInt(
                    (BoxMaxZ - static_cast<float>(Ctx.LatticeOriginVoxels.Z))
                    / static_cast<float>(Ctx.Step) + 1.0e-4f);
                const int64 Count = ((int64)IX1 - IX0 + 1)
                                 * ((int64)IY1 - IY0 + 1)
                                 * ((int64)IZ1 - IZ0 + 1);
                 if (IX1 >= IX0 && IY1 >= IY0 && IZ1 >= IZ0
                     && Count > 0 && Count <= 100000)
                 {
                    ExactIX0 = IX0;
                    ExactIY0 = IY0;
                    ExactIZ0 = IZ0;
                    ExactNX = IX1 - IX0 + 1;
                    ExactNY = IY1 - IY0 + 1;
                    ExactNZ = IZ1 - IZ0 + 1;
                    ExactLatticeWarpedQueries.Reserve(static_cast<int32>(Count));
                    ExactLatticeWorldQueries.Reserve(static_cast<int32>(Count));
                    const float WarpAmplitude = P.CaveWarpStrength * VOXEL_NOISE_SCALE;
                    auto MakeWarpedQuery = [&](int32 QueryIX, int32 QueryIY,
                                               int32 QueryIZ) -> FVector
                    {
                        const float WorldX = LatticeOriginX + (float)QueryIX * LatticeStepXY;
                        const float WorldY = LatticeOriginY + (float)QueryIY * LatticeStepXY;
                        const float WorldZ = static_cast<float>(Ctx.LatticeOriginVoxels.Z)
                            + (float)QueryIZ * (float)Ctx.Step;
                        const float EffectiveZ = (P.VerticalScale > 0.0f
                                                  && P.VerticalScale != 1.0f)
                                                ? WorldZ / P.VerticalScale : WorldZ;
                        FVector Query(WorldX, WorldY, EffectiveZ);
            if (!VoxelDensityAblation::IsCaveWarpOff() && P.CaveWarpStrength > 0.0f)
                        {
                            const float Frequency = P.CaveWarpFrequency;
                            float WarpX, WarpY, WarpZ;
                            VoxelNoise::Perlin3D_x3(
                                WorldX * Frequency + VoxelHash::SeedOffset(SeedU, 0.37f),
                                WorldY * Frequency + 1.3f,
                                EffectiveZ * Frequency + 5.7f,
                                WorldX * Frequency + 7.1f,
                                WorldY * Frequency + VoxelHash::SeedOffset(SeedU, 0.59f),
                                EffectiveZ * Frequency + 2.3f,
                                WorldX * Frequency + 11.3f,
                                WorldY * Frequency + 9.7f,
                                EffectiveZ * Frequency + VoxelHash::SeedOffset(SeedU, 0.41f),
                                WarpX, WarpY, WarpZ);
                            Query.X += WarpX * WarpAmplitude;
                            Query.Y += WarpY * WarpAmplitude;
                            Query.Z += WarpZ * WarpAmplitude;
                        }
                        return Query;
                    };

                    // All exact child boxes are contained in the classifier root held by the
                    // window cache. Cache only the deterministic warp samples, not any interval or
                    // verdict, so a cache miss can only cost work. The 100000-sample cap bounds
                    // this worker-local scratch allocation even for an unusually large root.
                    bool bCanUseWarpedQueryCache = false;
                    int32 CacheIX0 = 0;
                    int32 CacheIY0 = 0;
                    int32 CacheIZ0 = 0;
                    int32 CacheNX = 0;
                    int32 CacheNY = 0;
                    int32 CacheNZ = 0;
                    if (B.bCacheWindowValid
                        && Finite((float)B.CacheWindowBox.Min.X)
                        && Finite((float)B.CacheWindowBox.Min.Y)
                        && Finite((float)B.CacheWindowBox.Min.Z)
                        && Finite((float)B.CacheWindowBox.Max.X)
                        && Finite((float)B.CacheWindowBox.Max.Y)
                        && Finite((float)B.CacheWindowBox.Max.Z)
                        && B.CacheWindowBox.Min.X <= B.CacheWindowBox.Max.X
                        && B.CacheWindowBox.Min.Y <= B.CacheWindowBox.Max.Y
                        && B.CacheWindowBox.Min.Z <= B.CacheWindowBox.Max.Z)
                    {
                        CacheIX0 = FMath::CeilToInt(
                            ((float)B.CacheWindowBox.Min.X - LatticeOriginX)
                            / LatticeStepXY - 1.0e-4f);
                        CacheIY0 = FMath::CeilToInt(
                            ((float)B.CacheWindowBox.Min.Y - LatticeOriginY)
                            / LatticeStepXY - 1.0e-4f);
                        CacheIZ0 = FMath::CeilToInt(
                            ((float)B.CacheWindowBox.Min.Z
                             - static_cast<float>(Ctx.LatticeOriginVoxels.Z))
                            / static_cast<float>(Ctx.Step) - 1.0e-4f);
                        const int32 CacheIX1 = FMath::FloorToInt(
                            ((float)B.CacheWindowBox.Max.X - LatticeOriginX)
                            / LatticeStepXY + 1.0e-4f);
                        const int32 CacheIY1 = FMath::FloorToInt(
                            ((float)B.CacheWindowBox.Max.Y - LatticeOriginY)
                            / LatticeStepXY + 1.0e-4f);
                        const int32 CacheIZ1 = FMath::FloorToInt(
                            ((float)B.CacheWindowBox.Max.Z
                             - static_cast<float>(Ctx.LatticeOriginVoxels.Z))
                            / static_cast<float>(Ctx.Step) + 1.0e-4f);
                        const int64 CacheCount = ((int64)CacheIX1 - CacheIX0 + 1)
                                               * ((int64)CacheIY1 - CacheIY0 + 1)
                                               * ((int64)CacheIZ1 - CacheIZ0 + 1);
                        if (CacheIX1 >= CacheIX0 && CacheIY1 >= CacheIY0 && CacheIZ1 >= CacheIZ0
                            && CacheCount > 0 && CacheCount <= 100000
                            && IX0 >= CacheIX0 && IY0 >= CacheIY0 && IZ0 >= CacheIZ0
                            && IX1 <= CacheIX1 && IY1 <= CacheIY1 && IZ1 <= CacheIZ1)
                        {
                            CacheNX = CacheIX1 - CacheIX0 + 1;
                            CacheNY = CacheIY1 - CacheIY0 + 1;
                            CacheNZ = CacheIZ1 - CacheIZ0 + 1;
                            const bool bCacheKeyMatches = B.bExactWarpedLatticeCacheValid
                                && B.ExactWarpedLatticeCacheBox == B.CacheWindowBox
                                && B.ExactWarpedLatticeOrigin == Ctx.LatticeOriginVoxels
                                && B.ExactWarpedLatticeStep == Ctx.Step
                                && B.ExactWarpedLatticeSeed == SeedU
                                && B.ExactWarpedLatticeFingerprint == ParamsFingerprint
                                && B.ExactWarpedLatticeLayout == LV
                                && B.ExactWarpedLatticeManagerLifetimeId == ManagerLifetimeId
                                && B.ExactWarpedLatticeIX0 == CacheIX0
                                && B.ExactWarpedLatticeIY0 == CacheIY0
                                && B.ExactWarpedLatticeIZ0 == CacheIZ0
                                && B.ExactWarpedLatticeNX == CacheNX
                                && B.ExactWarpedLatticeNY == CacheNY
                                && B.ExactWarpedLatticeNZ == CacheNZ
                                && B.ExactWarpedLatticeQueries.Num() == CacheCount;
                            if (!bCacheKeyMatches)
                            {
                                B.ExactWarpedLatticeQueries.Reset();
                                B.ExactWarpedLatticeQueries.SetNumUninitialized(
                                    static_cast<int32>(CacheCount));
                                B.ExactTailStates.Init(0, static_cast<int32>(CacheCount));
                                for (int32 CacheIZ = CacheIZ0; CacheIZ <= CacheIZ1; ++CacheIZ)
                                for (int32 CacheIY = CacheIY0; CacheIY <= CacheIY1; ++CacheIY)
                                for (int32 CacheIX = CacheIX0; CacheIX <= CacheIX1; ++CacheIX)
                                {
                                    const int32 FlatIndex =
                                        ((CacheIZ - CacheIZ0) * CacheNY
                                         + (CacheIY - CacheIY0)) * CacheNX
                                        + (CacheIX - CacheIX0);
                                    B.ExactWarpedLatticeQueries[FlatIndex] =
                                        MakeWarpedQuery(CacheIX, CacheIY, CacheIZ);
                                }
                                B.ExactWarpedLatticeCacheBox = B.CacheWindowBox;
                                B.ExactWarpedLatticeIX0 = CacheIX0;
                                B.ExactWarpedLatticeIY0 = CacheIY0;
                                B.ExactWarpedLatticeIZ0 = CacheIZ0;
                                B.ExactWarpedLatticeNX = CacheNX;
                                B.ExactWarpedLatticeNY = CacheNY;
                                B.ExactWarpedLatticeNZ = CacheNZ;
                                B.ExactWarpedLatticeOrigin = Ctx.LatticeOriginVoxels;
                                B.ExactWarpedLatticeStep = Ctx.Step;
                                B.ExactWarpedLatticeSeed = SeedU;
                                B.ExactWarpedLatticeFingerprint = ParamsFingerprint;
                                B.ExactWarpedLatticeLayout = LV;
                                B.ExactWarpedLatticeManagerLifetimeId = ManagerLifetimeId;
                                B.bExactWarpedLatticeCacheValid = true;
                            }
                            else if (B.ExactTailStates.Num() != static_cast<int32>(CacheCount))
                            {
                                B.ExactTailStates.Init(0, static_cast<int32>(CacheCount));
                            }
                            bCanUseWarpedQueryCache = B.bExactWarpedLatticeCacheValid;
                        }
                    }

                    for (int32 IZ = IZ0; IZ <= IZ1; ++IZ)
                    for (int32 IY = IY0; IY <= IY1; ++IY)
                    for (int32 IX = IX0; IX <= IX1; ++IX)
                    {
                        const float WorldX = LatticeOriginX + (float)IX * LatticeStepXY;
                        const float WorldY = LatticeOriginY + (float)IY * LatticeStepXY;
                        const float WorldZ = static_cast<float>(Ctx.LatticeOriginVoxels.Z)
                            + (float)IZ * (float)Ctx.Step;
                        ExactLatticeWorldQueries.Add(FVector(WorldX, WorldY, WorldZ));
                        if (bCanUseWarpedQueryCache)
                        {
                            const int32 FlatIndex =
                                ((IZ - CacheIZ0) * CacheNY + (IY - CacheIY0)) * CacheNX
                                + (IX - CacheIX0);
                            ExactLatticeWarpedQueries.Add(
                                B.ExactWarpedLatticeQueries[FlatIndex]);
                        }
                        else
                        {
                            ExactLatticeWarpedQueries.Add(MakeWarpedQuery(IX, IY, IZ));
                        }
                        const FVector& Query = ExactLatticeWarpedQueries.Last();
                        ExactLatticeQueryMin.X = FMath::Min(ExactLatticeQueryMin.X, (float)Query.X);
                        ExactLatticeQueryMin.Y = FMath::Min(ExactLatticeQueryMin.Y, (float)Query.Y);
                        ExactLatticeQueryMin.Z = FMath::Min(ExactLatticeQueryMin.Z, (float)Query.Z);
                        ExactLatticeQueryMax.X = FMath::Max(ExactLatticeQueryMax.X, (float)Query.X);
                        ExactLatticeQueryMax.Y = FMath::Max(ExactLatticeQueryMax.Y, (float)Query.Y);
                        ExactLatticeQueryMax.Z = FMath::Max(ExactLatticeQueryMax.Z, (float)Query.Z);
                    }

                    // One envelope contains every warped lattice point in this refined child.
                    // The lower-bound helpers below are valid over any enclosing AABB, so using
                    // one envelope removes the repeated overlapping block traversal that made a
                    // single LOD0 classifier visit the same primitive geometry dozens of times.
                    // It is deliberately looser than the old sub-block decomposition: a looser
                    // lower bound can only retain Mixed and send work to the mesher; it cannot
                    // authorize a false empty-tile skip. The exact lattice itself is unchanged.
                    FExactLatticeBlock& Block = ExactLatticeBlocks.AddDefaulted_GetRef();
                    Block.IX0 = ExactIX0;
                    Block.IX1 = IX1;
                    Block.IY0 = ExactIY0;
                    Block.IY1 = IY1;
                    Block.IZ0 = ExactIZ0;
                    Block.IZ1 = IZ1;
                    Block.WarpedMin = ExactLatticeQueryMin;
                    Block.WarpedMax = ExactLatticeQueryMax;
                    Block.WorldMin = FVector(
                        LatticeOriginX + (float)ExactIX0 * LatticeStepXY,
                        LatticeOriginY + (float)ExactIY0 * LatticeStepXY,
                        (float)Ctx.LatticeOriginVoxels.Z
                            + (float)ExactIZ0 * (float)Ctx.Step);
                    Block.WorldMax = FVector(
                        LatticeOriginX + (float)IX1 * LatticeStepXY,
                        LatticeOriginY + (float)IY1 * LatticeStepXY,
                        (float)Ctx.LatticeOriginVoxels.Z
                            + (float)IZ1 * (float)Ctx.Step);
                }
            }

            const bool bUseExactLatticeWarp = ExactLatticeWarpedQueries.Num() > 0;
            const bool bUseExactLatticeBox = !bUseExactLatticeWarp
                && ExactLatticeQueryMin.X <= ExactLatticeQueryMax.X
                && ExactLatticeQueryMin.Y <= ExactLatticeQueryMax.Y
                && ExactLatticeQueryMin.Z <= ExactLatticeQueryMax.Z;
            bool bInvalidBound = false;
            float ExactSdfLower = FLT_MAX;
            float CheapSdfLower = FLT_MAX;
            int32 ExactRoomRefinements = 0;
            int32 ExactJoinRefinements = 0;
            int32 ExactTunnelRefinements = 0;
            int32 ExactPitRefinements = 0;
            int32 ExactChimneyRefinements = 0;

            auto ExactLatticePointDistance = [&](const FVector& Target) -> float
            {
                float Distance = FLT_MAX;
                // The block AABB contains every warped lattice point in the block.  Its distance
                // to the target is therefore a conservative lower bound for the exact point
                // minimum.  Do not rescan all 17^3 points for every room just to obtain this
                // broad-phase value; the same 2x2x2 block lattice is already built below for the
                // exact primitive walk.
                for (const FExactLatticeBlock& Block : ExactLatticeBlocks)
                {
                    Distance = FMath::Min(
                        Distance,
                        VF_DistanceBetweenBoxes(
                            Block.WarpedMin, Block.WarpedMax, Target, Target));
                }
                return FMath::Max(0.0f, Distance - 0.001f);
            };

            auto ExactLatticeBoxDistance = [&](const FVector& TargetMin,
                                               const FVector& TargetMax) -> float
            {
                float Distance = FLT_MAX;
                // As above, the block AABB distance is no larger than the distance from the
                // contained exact points to the target interval.  It can lose a skip, but can
                // never turn an unproven box into a proof.
                for (const FExactLatticeBlock& Block : ExactLatticeBlocks)
                {
                    Distance = FMath::Min(
                        Distance,
                        VF_DistanceBetweenBoxes(
                            Block.WarpedMin, Block.WarpedMax, TargetMin, TargetMax));
                }
                return FMath::Max(0.0f, Distance - 0.001f);
            };

            auto ForEachExactQueryInWorldBox = [&](const FVector& WorldMin,
                                                   const FVector& WorldMax,
                                                   auto&& Callback)
            {
                if (ExactNX <= 0 || ExactNY <= 0 || ExactNZ <= 0) { return; }
                const int32 IX0 = FMath::Max(
                    ExactIX0,
                    FMath::CeilToInt(
                        (WorldMin.X - LatticeOriginX) / LatticeStepXY - 1.0e-4f));
                const int32 IY0 = FMath::Max(
                    ExactIY0,
                    FMath::CeilToInt(
                        (WorldMin.Y - LatticeOriginY) / LatticeStepXY - 1.0e-4f));
                const int32 IZ0 = FMath::Max(
                    ExactIZ0,
                    FMath::CeilToInt(
                        (WorldMin.Z - static_cast<float>(Ctx.LatticeOriginVoxels.Z))
                        / static_cast<float>(Ctx.Step) - 1.0e-4f));
                const int32 IX1 = FMath::Min(
                    ExactIX0 + ExactNX - 1,
                    FMath::FloorToInt(
                        (WorldMax.X - LatticeOriginX) / LatticeStepXY + 1.0e-4f));
                const int32 IY1 = FMath::Min(
                    ExactIY0 + ExactNY - 1,
                    FMath::FloorToInt(
                        (WorldMax.Y - LatticeOriginY) / LatticeStepXY + 1.0e-4f));
                const int32 IZ1 = FMath::Min(
                    ExactIZ0 + ExactNZ - 1,
                    FMath::FloorToInt(
                        (WorldMax.Z - static_cast<float>(Ctx.LatticeOriginVoxels.Z))
                        / static_cast<float>(Ctx.Step) + 1.0e-4f));
                if (IX1 < IX0 || IY1 < IY0 || IZ1 < IZ0) { return; }

                for (int32 IZ = IZ0; IZ <= IZ1; ++IZ)
                for (int32 IY = IY0; IY <= IY1; ++IY)
                for (int32 IX = IX0; IX <= IX1; ++IX)
                {
                    const int32 FlatIndex =
                        ((IZ - ExactIZ0) * ExactNY + (IY - ExactIY0)) * ExactNX
                        + (IX - ExactIX0);
                    Callback(
                        ExactLatticeWarpedQueries[FlatIndex],
                        ExactLatticeWorldQueries[FlatIndex]);
                }
            };

            auto DistanceBlockToInfiniteLineLower = [&](const FExactLatticeBlock& Block,
                                                         const FVector& A,
                                                         const FVector& BPoint) -> float
            {
                const FVector Direction = BPoint - A;
                const float LengthSq = Direction.SizeSquared();
                if (!(LengthSq > KINDA_SMALL_NUMBER))
                {
                    return VF_DistanceBetweenBoxes(
                        Block.WarpedMin, Block.WarpedMax, A, A);
                }

                auto LinearRange = [](float Coefficient, float MinValue, float MaxValue,
                                      float Offset, float& OutMin, float& OutMax)
                {
                    const float AValue = Coefficient * (MinValue - Offset);
                    const float BValue = Coefficient * (MaxValue - Offset);
                    OutMin = FMath::Min(AValue, BValue);
                    OutMax = FMath::Max(AValue, BValue);
                };
                auto DistanceToZero = [](float MinValue, float MaxValue) -> float
                {
                    return MinValue > 0.0f ? MinValue
                         : MaxValue < 0.0f ? -MaxValue : 0.0f;
                };

                float XYMin = 0.0f, XYMax = 0.0f;
                float ZYMin = 0.0f, ZYMax = 0.0f;
                float XZMin = 0.0f, XZMax = 0.0f;
                float YXMin = 0.0f, YXMax = 0.0f;
                float ZXMin = 0.0f, ZXMax = 0.0f;
                float ZY2Min = 0.0f, ZY2Max = 0.0f;
                LinearRange(Direction.Y, Block.WarpedMin.Z, Block.WarpedMax.Z,
                            (float)A.Z, ZYMin, ZYMax);
                LinearRange(Direction.Z, Block.WarpedMin.Y, Block.WarpedMax.Y,
                            (float)A.Y, YXMin, YXMax);
                LinearRange(Direction.Z, Block.WarpedMin.X, Block.WarpedMax.X,
                            (float)A.X, XZMin, XZMax);
                LinearRange(Direction.X, Block.WarpedMin.Z, Block.WarpedMax.Z,
                            (float)A.Z, ZXMin, ZXMax);
                LinearRange(Direction.X, Block.WarpedMin.Y, Block.WarpedMax.Y,
                            (float)A.Y, XYMin, XYMax);
                LinearRange(Direction.Y, Block.WarpedMin.X, Block.WarpedMax.X,
                            (float)A.X, ZY2Min, ZY2Max);

                const float CrossXMin = ZYMin - YXMax;
                const float CrossXMax = ZYMax - YXMin;
                const float CrossYMin = XZMin - ZXMax;
                const float CrossYMax = XZMax - ZXMin;
                const float CrossZMin = XYMin - ZY2Max;
                const float CrossZMax = XYMax - ZY2Min;
                const float DX = DistanceToZero(CrossXMin, CrossXMax);
                const float DY = DistanceToZero(CrossYMin, CrossYMax);
                const float DZ = DistanceToZero(CrossZMin, CrossZMax);
                return FMath::Sqrt((DX * DX + DY * DY + DZ * DZ) / LengthSq);
            };

            auto LatticePointDistance = [&](const FVector& Target, const FVector& Extra) -> float
            {
                if (bUseExactLatticeWarp)
                {
                    return ExactLatticePointDistance(Target);
                }
                if (bUseExactLatticeBox)
                {
                    return VF_DistanceBetweenBoxes(
                        ExactLatticeQueryMin, ExactLatticeQueryMax, Target, Target);
                }
                const float DX = VF_LatticeAxisDistanceToPoint(
                    BoxMinX, BoxMaxX, LatticeOriginX, LatticeStepXY,
                    static_cast<float>(Target.X), (float)Extra.X);
                const float DY = VF_LatticeAxisDistanceToPoint(
                    BoxMinY, BoxMaxY, LatticeOriginY, LatticeStepXY,
                    static_cast<float>(Target.Y), (float)Extra.Y);
                const float DZ = VF_LatticeAxisDistanceToPoint(
                    BoxMinZ, BoxMaxZ, LatticeOriginZ, LatticeStepZ,
                    static_cast<float>(Target.Z), (float)Extra.Z);
                return FMath::Sqrt(DX * DX + DY * DY + DZ * DZ);
            };

            auto LatticeBoxDistance = [&](const FVector& TargetMin,
                                          const FVector& TargetMax,
                                          const FVector& Extra) -> float
            {
                if (bUseExactLatticeWarp)
                {
                    return ExactLatticeBoxDistance(TargetMin, TargetMax);
                }
                if (bUseExactLatticeBox)
                {
                    return VF_DistanceBetweenBoxes(
                        ExactLatticeQueryMin, ExactLatticeQueryMax, TargetMin, TargetMax);
                }
                const float DX = VF_LatticeAxisDistanceToInterval(
                    BoxMinX, BoxMaxX, LatticeOriginX, LatticeStepXY,
                    static_cast<float>(TargetMin.X), static_cast<float>(TargetMax.X),
                    (float)Extra.X);
                const float DY = VF_LatticeAxisDistanceToInterval(
                    BoxMinY, BoxMaxY, LatticeOriginY, LatticeStepXY,
                    static_cast<float>(TargetMin.Y), static_cast<float>(TargetMax.Y),
                    (float)Extra.Y);
                const float DZ = VF_LatticeAxisDistanceToInterval(
                    BoxMinZ, BoxMaxZ, LatticeOriginZ, LatticeStepZ,
                    static_cast<float>(TargetMin.Z), static_cast<float>(TargetMax.Z),
                    (float)Extra.Z);
                return FMath::Sqrt(DX * DX + DY * DY + DZ * DZ);
            };

            // The exact warped lattice list is also an exact AABB superset.  Use that cheap
            // distance as a first test and pay the primitive-by-query walk only when this bound is
            // close enough to affect the source's active threshold.  A far primitive can only
            // raise the lower bound; retaining its AABB bound is conservative and avoids scanning
            // 729 queries for every cached room/tunnel in every refined child.
            auto ExactLatticeAabbPointDistance = [&](const FVector& Target) -> float
            {
                return bUseExactLatticeWarp
                    && ExactLatticeQueryMin.X <= ExactLatticeQueryMax.X
                    && ExactLatticeQueryMin.Y <= ExactLatticeQueryMax.Y
                    && ExactLatticeQueryMin.Z <= ExactLatticeQueryMax.Z
                    ? VF_DistanceBetweenBoxes(
                        ExactLatticeQueryMin, ExactLatticeQueryMax, Target, Target)
                    : FLT_MAX;
            };
            auto ExactLatticeAabbBoxDistance = [&](const FVector& TargetMin,
                                                   const FVector& TargetMax) -> float
            {
                return bUseExactLatticeWarp
                    && ExactLatticeQueryMin.X <= ExactLatticeQueryMax.X
                    && ExactLatticeQueryMin.Y <= ExactLatticeQueryMax.Y
                    && ExactLatticeQueryMin.Z <= ExactLatticeQueryMax.Z
                    ? VF_DistanceBetweenBoxes(
                        ExactLatticeQueryMin, ExactLatticeQueryMax, TargetMin, TargetMax)
                    : FLT_MAX;
            };

            // Conservative lower bounds for primitive values on each warped lattice block.
            // Earlier versions walked every exact point in every refined child here. That was
            // sound, but it made an empty LOD0 tile pay thousands of room/tunnel evaluations.
            // These bounds are over the whole block AABB, which contains every warped lattice
            // point in the block. They are therefore allowed to be looser than the finite-point
            // minimum: looser costs a proof, never geometry. The smooth-min fold below remains
            // monotone in every primitive lower bound.
            auto ExactRoomShapeLower = [&](const FCachedRoom& Room,
                                           const FExactLatticeBlock& Block) -> float
            {
                auto Invalid = [&]() -> float
                {
                    bInvalidBound = true;
                    return 0.0f;
                };
                const FVector BlockMin = Block.WarpedMin;
                const FVector BlockMax = Block.WarpedMax;
                if (!Finite((float)BlockMin.X) || !Finite((float)BlockMin.Y)
                    || !Finite((float)BlockMin.Z) || !Finite((float)BlockMax.X)
                    || !Finite((float)BlockMax.Y) || !Finite((float)BlockMax.Z))
                {
                    return Invalid();
                }

                switch (Room.ShapeType)
                {
                case 0:
                {
                    // VoxelSDF::Ellipsoid uses the scaled Euclidean distance multiplied by
                    // the average radius. The closest scaled point to an axis-aligned box is
                    // obtained independently on each axis, so this is a direct lower bound for
                    // the exact approximation used by Eval.
                    const FVector& Radii = Room.ShapeA;
                    if (!Finite((float)Radii.X) || !Finite((float)Radii.Y)
                        || !Finite((float)Radii.Z) || Radii.X <= 0.0
                        || Radii.Y <= 0.0 || Radii.Z <= 0.0)
                    {
                        return Invalid();
                    }
                    const float Cx = (float)Room.Center.X;
                    const float Cy = (float)Room.Center.Y;
                    const float Cz = (float)Room.Center.Z;
                    const float Rx = (float)Radii.X;
                    const float Ry = (float)Radii.Y;
                    const float Rz = (float)Radii.Z;
                    const float DX = BlockMin.X > Cx ? BlockMin.X - Cx
                        : BlockMax.X < Cx ? Cx - BlockMax.X : 0.0f;
                    const float DY = BlockMin.Y > Cy ? BlockMin.Y - Cy
                        : BlockMax.Y < Cy ? Cy - BlockMax.Y : 0.0f;
                    const float DZ = BlockMin.Z > Cz ? BlockMin.Z - Cz
                        : BlockMax.Z < Cz ? Cz - BlockMax.Z : 0.0f;
                    const float AverageRadius = (Rx + Ry + Rz) / 3.0f;
                    const float ScaledDistance = FMath::Sqrt(
                        (DX / Rx) * (DX / Rx)
                        + (DY / Ry) * (DY / Ry)
                        + (DZ / Rz) * (DZ / Rz));
                    const float Lower = (ScaledDistance - 1.0f) * AverageRadius;
                    return Finite(Lower) ? Lower : Invalid();
                }
                case 1:
                {
                    // The rounded-box SDF is the exact SDF of a box dilated by a sphere. The
                    // enclosing sphere gives a cheap, unconditional lower bound for its signed
                    // distance, including points inside the box.
                    const FVector& HalfExtent = Room.ShapeA;
                    const float Rounding = (float)Room.ShapeR;
                    if (!Finite((float)HalfExtent.X) || !Finite((float)HalfExtent.Y)
                        || !Finite((float)HalfExtent.Z) || !Finite(Rounding)
                        || HalfExtent.X < 0.0 || HalfExtent.Y < 0.0
                        || HalfExtent.Z < 0.0 || Rounding < 0.0f)
                    {
                        return Invalid();
                    }
                    const FVector OuterExtent(
                        (float)HalfExtent.X + Rounding,
                        (float)HalfExtent.Y + Rounding,
                        (float)HalfExtent.Z + Rounding);
                    const float Lower = VF_DistanceBetweenBoxes(
                        BlockMin, BlockMax, Room.Center, Room.Center) - OuterExtent.Size();
                    return Finite(Lower) ? Lower : Invalid();
                }
                case 2:
                {
                    if (!Finite((float)Room.ShapeA.X) || !Finite((float)Room.ShapeA.Y)
                        || !Finite((float)Room.ShapeA.Z) || !Finite((float)Room.ShapeB.X)
                        || !Finite((float)Room.ShapeB.Y) || !Finite((float)Room.ShapeB.Z)
                        || !Finite((float)Room.ShapeR) || Room.ShapeR < 0.0f)
                    {
                        return Invalid();
                    }
                    const float Lower = DistanceBlockToInfiniteLineLower(
                        Block, Room.ShapeA, Room.ShapeB) - FMath::Abs((float)Room.ShapeR);
                    return Finite(Lower) ? Lower : Invalid();
                }
                default:
                    return Invalid();
                }
            };

            auto ExactRoomLower = [&](const FCachedRoom& Room) -> float
            {
                float LowerBound = FLT_MAX;
                const float CullRadiusSq = Room.CullRadiusSq;
                if (!Finite(CullRadiusSq) || CullRadiusSq < 0.0f)
                {
                    bInvalidBound = true;
                    return LowerBound;
                }
                const float CullRadius = FMath::Sqrt(CullRadiusSq);
                for (const FExactLatticeBlock& Block : ExactLatticeBlocks)
                {
                    const float BlockDistance = VF_DistanceBetweenBoxes(
                        Block.WarpedMin, Block.WarpedMax,
                        Room.Center, Room.Center);
                    if (!Finite(BlockDistance))
                    {
                        bInvalidBound = true;
                        continue;
                    }
                    // Every point in this block fails Eval's sphere cull when the block itself
                    // is farther than the cull radius. This reject is exact for that cull.
                    if (BlockDistance > CullRadius)
                    {
                        continue;
                    }

                    float RoomLower = ExactRoomShapeLower(Room, Block);
                    if (bInvalidBound) { continue; }
                    if (Room.FloorCutZ > -FLT_MAX)
                    {
                        if (!Finite((float)Room.FloorCutZ)
                            || !Finite((float)Room.FloorReliefStrength)
                            || (Room.FloorReliefStrength > 0.0f
                                && !Finite((float)Room.FloorReliefFrequency)))
                        {
                            bInvalidBound = true;
                            continue;
                        }
                        const float ReliefBound = Room.FloorReliefStrength > 0.0f
                            ? FMath::Abs((float)Room.FloorReliefStrength)
                                * VOXEL_NOISE_SCALE * VF_PerlinAbsBound
                            : 0.0f;
                        // SmoothMax(a,b) is never below max(a,b), so independently bounded
                        // shape and floor terms can be combined with an ordinary Max.
                        const float FloorLower = (float)Room.FloorCutZ - ReliefBound
                            - (float)Block.WarpedMax.Z;
                        RoomLower = FMath::Max(RoomLower, FloorLower);
                    }
                    LowerBound = FMath::Min(LowerBound, RoomLower);
                }
                return LowerBound;
            };

            auto ExactJoinLower = [&](const FCachedRoomFloorJoin& Join) -> float
            {
                float LowerBound = FLT_MAX;
                if (!Finite((float)Join.BoundRadiusSq) || Join.BoundRadiusSq < 0.0f
                    || !Finite((float)Join.BoundCenter.X)
                    || !Finite((float)Join.BoundCenter.Y)
                    || !Finite((float)Join.BoundCenter.Z)
                    || !Finite((float)Join.Start.X) || !Finite((float)Join.Start.Y)
                    || !Finite((float)Join.Start.Z) || !Finite((float)Join.End.X)
                    || !Finite((float)Join.End.Y) || !Finite((float)Join.End.Z)
                    || !Finite((float)Join.Radius) || !Finite((float)Join.FloorZ)
                    || !Finite((float)Join.CeilingZ) || Join.Radius < 0.0f)
                {
                    bInvalidBound = true;
                    return LowerBound;
                }
                const float BoundRadius = FMath::Sqrt(Join.BoundRadiusSq);
                for (const FExactLatticeBlock& Block : ExactLatticeBlocks)
                {
                    const float BlockDistance = VF_DistanceBetweenBoxes(
                        Block.WarpedMin, Block.WarpedMax,
                        Join.BoundCenter, Join.BoundCenter);
                    if (BlockDistance > BoundRadius)
                    {
                        continue;
                    }
                    const float HorizontalLower = DistanceBlockToInfiniteLineLower(
                        Block, Join.Start, Join.End) - FMath::Abs(Join.Radius);
                    const float VerticalLower = FMath::Max(
                        Join.FloorZ - (float)Block.WarpedMax.Z,
                        (float)Block.WarpedMin.Z - Join.CeilingZ);
                    const float Lower = FMath::Max(HorizontalLower, VerticalLower);
                    if (!Finite(Lower))
                    {
                        bInvalidBound = true;
                        continue;
                    }
                    LowerBound = FMath::Min(LowerBound, Lower);
                }
                return LowerBound;
            };

            auto ExactTunnelLower = [&](const FCachedTunnel& Tunnel) -> float
            {
                float LowerBound = FLT_MAX;
                if (!Finite((float)Tunnel.BoundRadiusSq)
                    || Tunnel.BoundRadiusSq < 0.0f
                    || !Finite((float)Tunnel.BoundCenter.X)
                    || !Finite((float)Tunnel.BoundCenter.Y)
                    || !Finite((float)Tunnel.BoundCenter.Z)
                    || !Finite((float)Tunnel.SDFInfluenceRadius)
                    || !Finite((float)Tunnel.SDFCenterlineMin.X)
                    || !Finite((float)Tunnel.SDFCenterlineMin.Y)
                    || !Finite((float)Tunnel.SDFCenterlineMin.Z)
                    || !Finite((float)Tunnel.SDFCenterlineMax.X)
                    || !Finite((float)Tunnel.SDFCenterlineMax.Y)
                    || !Finite((float)Tunnel.SDFCenterlineMax.Z)
                    || Tunnel.SDFCenterlineMin.X > Tunnel.SDFCenterlineMax.X
                    || Tunnel.SDFCenterlineMin.Y > Tunnel.SDFCenterlineMax.Y
                    || Tunnel.SDFCenterlineMin.Z > Tunnel.SDFCenterlineMax.Z)
                {
                    bInvalidBound = true;
                    return LowerBound;
                }
                const float BoundRadius = FMath::Sqrt(Tunnel.BoundRadiusSq);
                auto EvaluateSegment = [&](const FVector& A, const FVector& BPoint,
                                           float RadiusA, float RadiusB)
                {
                    if (!Finite((float)A.X) || !Finite((float)A.Y)
                        || !Finite((float)A.Z) || !Finite((float)BPoint.X)
                        || !Finite((float)BPoint.Y) || !Finite((float)BPoint.Z)
                        || !Finite(RadiusA) || !Finite(RadiusB))
                    {
                        bInvalidBound = true;
                        return;
                    }
                    const float SegmentRadius = FMath::Max(
                        FMath::Abs(RadiusA), FMath::Abs(RadiusB));
                    const FVector SegmentMin(
                        FMath::Min(A.X, BPoint.X), FMath::Min(A.Y, BPoint.Y),
                        FMath::Min(A.Z, BPoint.Z));
                    const FVector SegmentMax(
                        FMath::Max(A.X, BPoint.X), FMath::Max(A.Y, BPoint.Y),
                        FMath::Max(A.Z, BPoint.Z));
                    for (const FExactLatticeBlock& Block : ExactLatticeBlocks)
                    {
                        if (Tunnel.SDFInfluenceRadius > 0.0f
                            && VF_DistanceBetweenBoxes(
                                Block.WarpedMin, Block.WarpedMax,
                                Tunnel.SDFCenterlineMin, Tunnel.SDFCenterlineMax)
                                > Tunnel.SDFInfluenceRadius)
                        {
                            continue;
                        }
                        if (VF_DistanceBetweenBoxes(
                                Block.WarpedMin, Block.WarpedMax,
                                Tunnel.BoundCenter, Tunnel.BoundCenter)
                            > BoundRadius)
                        {
                            continue;
                        }

                        // Both terms are lower bounds for distance to the finite segment:
                        // the first uses the infinite supporting line, while the second uses
                        // the segment's AABB. Their maximum is still a lower bound and is much
                        // tighter near a segment endpoint than either term alone.
                        const float LineLower = DistanceBlockToInfiniteLineLower(
                            Block, A, BPoint);
                        const float SegmentBoxLower = VF_DistanceBetweenBoxes(
                            Block.WarpedMin, Block.WarpedMax, SegmentMin, SegmentMax);
                        const float Lower = FMath::Max(LineLower, SegmentBoxLower)
                            - SegmentRadius;
                        if (!Finite(Lower))
                        {
                            bInvalidBound = true;
                            continue;
                        }
                        LowerBound = FMath::Min(LowerBound, Lower);
                    }
                };

                if (Tunnel.ControlPoints.Num() >= 2
                    && Tunnel.ControlRadii.Num() == Tunnel.ControlPoints.Num())
                {
                    for (int32 SegmentIndex = 0;
                         SegmentIndex + 1 < Tunnel.ControlPoints.Num();
                         ++SegmentIndex)
                    {
                        EvaluateSegment(
                            Tunnel.ControlPoints[SegmentIndex],
                            Tunnel.ControlPoints[SegmentIndex + 1],
                            Tunnel.ControlRadii[SegmentIndex],
                            Tunnel.ControlRadii[SegmentIndex + 1]);
                    }
                }
                else if (Tunnel.bHasMidpoint)
                {
                    EvaluateSegment(Tunnel.EndpointA, Tunnel.Midpoint,
                                    Tunnel.RadiusA, Tunnel.RadiusMid);
                    EvaluateSegment(Tunnel.Midpoint, Tunnel.EndpointB,
                                    Tunnel.RadiusMid, Tunnel.RadiusB);
                }
                else
                {
                    EvaluateSegment(Tunnel.EndpointA, Tunnel.EndpointB,
                                    Tunnel.RadiusA, Tunnel.RadiusB);
                }
                return LowerBound;
            };

            auto ExactPitLower = [&](const FCachedPit& Pit) -> float
            {
                float LowerBound = FLT_MAX;
                const float Radius = FMath::Sqrt(FMath::Max(Pit.BoundXYRadiusSq, 0.0f));
                const FVector CandidateMin(
                    Pit.CenterX - Radius, Pit.CenterY - Radius,
                    Pit.TopZ - Pit.Depth - Pit.BlendK);
                const FVector CandidateMax(
                    Pit.CenterX + Radius, Pit.CenterY + Radius,
                    Pit.TopZ + Pit.BlendK);
                ForEachExactQueryInWorldBox(
                    CandidateMin, CandidateMax,
                    [&](const FVector&, const FVector& Query)
                {
                    const float DZ = (float)Query.Z - Pit.TopZ;
                    if (DZ >= Pit.BlendK || -DZ > Pit.Depth + Pit.BlendK)
                    {
                        return;
                    }
                    const float DX = (float)Query.X - Pit.CenterX;
                    const float DY = (float)Query.Y - Pit.CenterY;
                    const float XYDistSq = DX * DX + DY * DY;
                    if (XYDistSq > Pit.BoundXYRadiusSq) { return; }
                    float PitSDF;
                    if (DZ <= 0.0f)
                    {
                        float FlareFactor = FMath::Clamp(
                            1.0f - (-DZ) / Pit.FlareDist, 0.0f, 1.0f);
                        FlareFactor *= FlareFactor;
                        PitSDF = FMath::Sqrt(XYDistSq)
                               - (Pit.Radius + Pit.FlareExtra * FlareFactor);
                    }
                    else
                    {
                        PitSDF = FMath::Sqrt(XYDistSq)
                               - (Pit.Radius + Pit.FlareExtra);
                    }
                    LowerBound = FMath::Min(LowerBound, PitSDF);
                });
                return LowerBound;
            };

            auto ExactChimneyLower = [&](const FCachedChimney& Chimney) -> float
            {
                float LowerBound = FLT_MAX;
                const float Radius = FMath::Sqrt(FMath::Max(Chimney.BoundXYRadiusSq, 0.0f));
                const FVector CandidateMin(
                    Chimney.CenterX - Radius, Chimney.CenterY - Radius,
                    Chimney.BottomZ - Chimney.BlendK);
                const FVector CandidateMax(
                    Chimney.CenterX + Radius, Chimney.CenterY + Radius,
                    Chimney.BottomZ + Chimney.Height + Chimney.BlendK);
                ForEachExactQueryInWorldBox(
                    CandidateMin, CandidateMax,
                    [&](const FVector&, const FVector& Query)
                {
                    const float DZ = (float)Query.Z - Chimney.BottomZ;
                    if (-DZ >= Chimney.BlendK || DZ > Chimney.Height + Chimney.BlendK)
                    {
                        return;
                    }
                    const float DX = (float)Query.X - Chimney.CenterX;
                    const float DY = (float)Query.Y - Chimney.CenterY;
                    const float XYDistSq = DX * DX + DY * DY;
                    if (XYDistSq > Chimney.BoundXYRadiusSq) { return; }
                    float ChimneySDF;
                    if (DZ >= 0.0f)
                    {
                        float FlareFactor = FMath::Clamp(
                            1.0f - DZ / Chimney.FlareDist, 0.0f, 1.0f);
                        FlareFactor *= FlareFactor;
                        ChimneySDF = FMath::Sqrt(XYDistSq)
                                   - (Chimney.Radius + Chimney.FlareExtra * FlareFactor);
                    }
                    else
                    {
                        ChimneySDF = FMath::Sqrt(XYDistSq)
                                   - (Chimney.Radius + Chimney.FlareExtra);
                    }
                    LowerBound = FMath::Min(LowerBound, ChimneySDF);
                });
                return LowerBound;
            };

            auto ConsiderExact = [&](float PrimitiveLower, float BlendRadius)
            {
                if (!Finite(PrimitiveLower) || !Finite(BlendRadius))
                {
                    bInvalidBound = true;
                    return;
                }
                ExactSdfLower = VoxelSDF::SmoothMin(
                    ExactSdfLower, PrimitiveLower, BlendRadius);
            };
            auto ConsiderCheapExact = [&](float PrimitiveLower, float BlendRadius)
            {
                if (Finite(PrimitiveLower) && Finite(BlendRadius))
                {
                    CheapSdfLower = VoxelSDF::SmoothMin(
                        CheapSdfLower, PrimitiveLower, BlendRadius);
                }
            };

            auto LatticeXYPointDistance = [&](float X, float Y) -> float
            {
                const float DX = VF_LatticeAxisDistanceToPoint(
                    BoxMinX, BoxMaxX, LatticeOriginX, LatticeStepXY, X, 0.0f);
                const float DY = VF_LatticeAxisDistanceToPoint(
                    BoxMinY, BoxMaxY, LatticeOriginY, LatticeStepXY, Y, 0.0f);
                return FMath::Sqrt(DX * DX + DY * DY);
            };

            const float K = P.SDFBlendRadius;
            const float WormThreshold = VoxelMath::IsFinite(P.WormNetworkRange)
                                       ? FMath::Max(3.0f * K, P.WormNetworkRange) : FLT_MAX;
            const float ThresholdWithBlend = VF_SaturatingAdd(WormThreshold, K);
            float Lower = FLT_MAX;
            bool bAnyPrimitive = false;

            auto Consider = [&](float Candidate)
            {
                if (!Finite(Candidate))
                {
                    bInvalidBound = true;
                    return;
                }
                bAnyPrimitive = true;
                Lower = FMath::Min(Lower, Candidate);
            };
            auto CountThreshold = [&](float Candidate, int32& Count)
            {
                if (Finite(Candidate) && Candidate < ThresholdWithBlend) { ++Count; }
            };

            B.NumRooms = GetBoxCache().Rooms.Num();
            B.NumTunnels = GetBoxCache().Tunnels.Num();
            B.NumRoomFloorJoins = GetBoxCache().RoomFloorJoins.Num();
            B.NumPits = GetBoxCache().Pits.Num();
            B.NumChimneys = GetBoxCache().Chimneys.Num();
            B.HitRooms = B.HitTunnels = B.HitPits = B.HitChimneys = 0;
            B.HitRoomsNoWarp = B.HitTunnelsNoWarp = 0;
            B.WarpDilation = FMath::Max3(
                (float)WarpEnvelope.X, (float)WarpEnvelope.Y, (float)WarpEnvelope.Z);
            B.bMayHaveTunnelCoreAir = false;
            B.bMayHaveTunnelSupportFloor = false;
            B.bMayHaveTunnelCoreTail = false;

            auto MayHaveTunnelCoreTail = [&](const FCachedTunnel& Tunnel) -> uint8
            {
                const bool bHasWorldChain = Tunnel.WorldControlPoints.Num() >= 2
                    && Tunnel.WorldControlRadii.Num() == Tunnel.WorldControlPoints.Num();
                const FVector& CenterlineMin = bHasWorldChain
                    ? Tunnel.WorldCenterlineMin : Tunnel.SDFCenterlineMin;
                const FVector& CenterlineMax = bHasWorldChain
                    ? Tunnel.WorldCenterlineMax : Tunnel.SDFCenterlineMax;
                const float InfluenceRadius = bHasWorldChain
                    ? Tunnel.WorldInfluenceRadius : Tunnel.SDFInfluenceRadius;
                const FVector& BoundCenter = bHasWorldChain
                    ? Tunnel.WorldBoundCenter : Tunnel.BoundCenter;
                const float BoundRadiusSq = bHasWorldChain
                    ? Tunnel.WorldBoundRadiusSq : Tunnel.BoundRadiusSq;
                if (!Finite((float)CenterlineMin.X) || !Finite((float)CenterlineMin.Y)
                    || !Finite((float)CenterlineMin.Z) || !Finite((float)CenterlineMax.X)
                    || !Finite((float)CenterlineMax.Y) || !Finite((float)CenterlineMax.Z)
                    || !Finite(InfluenceRadius) || InfluenceRadius < 0.0f
                    || !Finite((float)BoundCenter.X) || !Finite((float)BoundCenter.Y)
                    || !Finite((float)BoundCenter.Z) || !Finite(BoundRadiusSq)
                    || BoundRadiusSq < 0.0f)
                {
                    return 3u;
                }

                // A post-air sample must lie within a tapered capsule's maximum radius of one
                // world-chain segment.  The segment AABB expanded by that radius is a cheap
                // superset of the exact capsule; unlike the old enclosing sphere it rejects a
                // long, thin tunnel from most unrelated LOD0 boxes.  The floor is part of this
                // same continuous field, so no second support-band candidate is needed.
                if (Ctx.bUseLatticeProof)
                {
                    const float WorldLatticeOriginX =
                        static_cast<float>(Ctx.LatticeOriginVoxels.X);
                    const float WorldLatticeOriginY =
                        static_cast<float>(Ctx.LatticeOriginVoxels.Y);
                    const float WorldLatticeOriginZ =
                        static_cast<float>(Ctx.LatticeOriginVoxels.Z);
                    const float WorldLatticeStep = static_cast<float>(Ctx.Step);
                    if (!(WorldLatticeStep > 0.0f)) { return 3u; }

                    auto TouchesLatticeAABB = [&](float MinX, float MaxX,
                                                   float MinY, float MaxY,
                                                   float MinZ, float MaxZ) -> bool
                    {
                        return VF_LatticeAxisDistanceToInterval(
                                   BoxMinX, BoxMaxX, WorldLatticeOriginX, WorldLatticeStep,
                                   MinX, MaxX, 0.0f) <= 0.0f
                            && VF_LatticeAxisDistanceToInterval(
                                   BoxMinY, BoxMaxY, WorldLatticeOriginY, WorldLatticeStep,
                                   MinY, MaxY, 0.0f) <= 0.0f
                            && VF_LatticeAxisDistanceToInterval(
                                   BoxMinZ, BoxMaxZ, WorldLatticeOriginZ, WorldLatticeStep,
                                   MinZ, MaxZ, 0.0f) <= 0.0f;
                    };

                    uint8 Result = 0u;
                    auto AddSegment = [&](const FVector& A, const FVector& BPoint,
                                           float RadiusA, float RadiusB,
                                           bool bWorldChain, int32 SegmentIndex)
                    {
                        if (!Finite((float)A.X) || !Finite((float)A.Y)
                            || !Finite((float)A.Z) || !Finite((float)BPoint.X)
                            || !Finite((float)BPoint.Y) || !Finite((float)BPoint.Z)
                            || !Finite(RadiusA) || !Finite(RadiusB))
                        {
                            Result = 3u;
                            return;
                        }
                        const float SafeRadiusA = FMath::Abs(RadiusA);
                        const float SafeRadiusB = FMath::Abs(RadiusB);
                        const float MaxRadius = FMath::Max(SafeRadiusA, SafeRadiusB);
                        if (!Finite(MaxRadius)) { Result = 3u; return; }

                        if (TouchesLatticeAABB(
                                FMath::Min(A.X, BPoint.X) - MaxRadius,
                                FMath::Max(A.X, BPoint.X) + MaxRadius,
                                FMath::Min(A.Y, BPoint.Y) - MaxRadius,
                                FMath::Max(A.Y, BPoint.Y) + MaxRadius,
                                FMath::Min(A.Z, BPoint.Z) - MaxRadius,
                                FMath::Max(A.Z, BPoint.Z) + MaxRadius))
                        {
                            Result |= 1u;
                        }

                        (void)bWorldChain;
                        (void)SegmentIndex;
                    };

                    if (bHasWorldChain)
                    {
                        for (int32 SegmentIndex = 0;
                             SegmentIndex + 1 < Tunnel.WorldControlPoints.Num();
                             ++SegmentIndex)
                        {
                            AddSegment(
                                Tunnel.WorldControlPoints[SegmentIndex],
                                Tunnel.WorldControlPoints[SegmentIndex + 1],
                                Tunnel.WorldControlRadii[SegmentIndex],
                                Tunnel.WorldControlRadii[SegmentIndex + 1], true, SegmentIndex);
                            if (Result == 3u) { return Result; }
                        }
                    }
                    else if (Tunnel.ControlPoints.Num() >= 2
                        && Tunnel.ControlRadii.Num() == Tunnel.ControlPoints.Num())
                    {
                        for (int32 SegmentIndex = 0;
                             SegmentIndex + 1 < Tunnel.ControlPoints.Num();
                             ++SegmentIndex)
                        {
                            AddSegment(
                                Tunnel.ControlPoints[SegmentIndex],
                                Tunnel.ControlPoints[SegmentIndex + 1],
                                Tunnel.ControlRadii[SegmentIndex],
                                Tunnel.ControlRadii[SegmentIndex + 1], false, SegmentIndex);
                            if (Result == 3u) { return Result; }
                        }
                    }
                    else if (Tunnel.bHasMidpoint)
                    {
                        AddSegment(Tunnel.EndpointA, Tunnel.Midpoint,
                                   Tunnel.RadiusA, Tunnel.RadiusMid, false, INDEX_NONE);
                        AddSegment(Tunnel.Midpoint, Tunnel.EndpointB,
                                   Tunnel.RadiusMid, Tunnel.RadiusB, false, INDEX_NONE);
                    }
                    else
                    {
                        AddSegment(Tunnel.EndpointA, Tunnel.EndpointB,
                                   Tunnel.RadiusA, Tunnel.RadiusB, false, INDEX_NONE);
                    }
                    return Result;
                }

                if (InfluenceRadius > 0.0f
                    && VF_DistanceBetweenBoxes(
                        VoxelBox.Min, VoxelBox.Max, CenterlineMin, CenterlineMax)
                       > InfluenceRadius)
                {
                    return 0u;
                }
                if (BoundRadiusSq > 0.0f
                    && VF_DistanceBetweenBoxes(
                        VoxelBox.Min, VoxelBox.Max, BoundCenter, BoundCenter)
                       > FMath::Sqrt(BoundRadiusSq))
                {
                    return 0u;
                }
                return 3u;
            };

            const FVector NoWarpMin(BoxMinX, BoxMinY, EffectiveMinZ);
            const FVector NoWarpMax(BoxMaxX, BoxMaxY, EffectiveMaxZ);

            const uint64 ExactPrimitiveStartCycles = FPlatformTime::Cycles64();
            for (const FCachedRoom& Room : GetBoxCache().Rooms)
            {
                const float CullRadiusSq = Room.CullRadiusSq;
                if (!Finite((float)Room.Center.X) || !Finite((float)Room.Center.Y)
                    || !Finite((float)Room.Center.Z) || !Finite(CullRadiusSq)
                    || CullRadiusSq < 0.0f)
                {
                    bInvalidBound = true;
                    continue;
                }

                const float RoomRadius = FMath::Sqrt(CullRadiusSq);
                const float FastRoomLower = bUseExactLatticeWarp
                    ? ExactLatticeAabbPointDistance(Room.Center) - RoomRadius
                    : (bUseLatticeProof
                        ? LatticePointDistance(Room.Center, WarpEnvelope) - RoomRadius
                        : VF_DistanceBetweenBoxes(QMin, QMax, Room.Center, Room.Center) - RoomRadius);
                const float RoomLower = bUseExactLatticeWarp
                    && FastRoomLower < ThresholdWithBlend
                    ? (ExactRoomRefinements++, ExactRoomLower(Room)) : FastRoomLower;
                const float RoomLowerNoWarp = VF_DistanceBetweenBoxes(
                    NoWarpMin, NoWarpMax, Room.Center, Room.Center) - RoomRadius;
                Consider(RoomLower);
                if (bUseExactLatticeWarp) { ConsiderCheapExact(FastRoomLower, K); }
                if (bUseExactLatticeWarp) { ConsiderExact(RoomLower, K); }
                CountThreshold(RoomLower, B.HitRooms);
                CountThreshold(RoomLowerNoWarp, B.HitRoomsNoWarp);
            }

            // Room-floor joins are part of EvaluateSDFCached's same smooth-min union.  They were
            // absent from the old box proof, which could only lose skips; include them here so the
            // exact retry remains a proof of the complete source rather than just rooms/tunnels.
            for (const FCachedRoomFloorJoin& Join : GetBoxCache().RoomFloorJoins)
            {
                const float JoinRadius = FMath::Sqrt(FMath::Max(Join.BoundRadiusSq, 0.0f));
                const float FastJoinLower = bUseExactLatticeWarp
                    ? ExactLatticeAabbPointDistance(Join.BoundCenter) - JoinRadius
                    : VF_DistanceBetweenBoxes(
                        QMin, QMax, Join.BoundCenter, Join.BoundCenter) - JoinRadius;
                const float JoinLower = bUseExactLatticeWarp
                    && FastJoinLower < ThresholdWithBlend
                    ? (ExactJoinRefinements++, ExactJoinLower(Join)) : FastJoinLower;
                Consider(JoinLower);
                if (bUseExactLatticeWarp) { ConsiderCheapExact(FastJoinLower, K); }
                if (bUseExactLatticeWarp) { ConsiderExact(JoinLower, K); }
            }

            for (const FCachedTunnel& Tunnel : GetBoxCache().Tunnels)
            {
                const uint8 TailMask = MayHaveTunnelCoreTail(Tunnel);
                B.bMayHaveTunnelCoreAir = B.bMayHaveTunnelCoreAir
                    || (TailMask & 1u) != 0;
                B.bMayHaveTunnelCoreTail = B.bMayHaveTunnelCoreTail
                    || TailMask != 0u;
                float MaxRadius = FMath::Max(
                    FMath::Abs(Tunnel.RadiusA), FMath::Abs(Tunnel.RadiusB));
                float TunnelLower = FLT_MAX;
                if (bUseExactLatticeWarp)
                {
                    if (Tunnel.ControlPoints.Num() >= 2
                        && Tunnel.ControlRadii.Num() == Tunnel.ControlPoints.Num())
                    {
                        for (int32 SegmentIndex = 0;
                             SegmentIndex + 1 < Tunnel.ControlPoints.Num();
                             ++SegmentIndex)
                        {
                            const FVector& A = Tunnel.ControlPoints[SegmentIndex];
                            const FVector& BPoint = Tunnel.ControlPoints[SegmentIndex + 1];
                            MaxRadius = FMath::Max(
                                MaxRadius,
                                FMath::Max(FMath::Abs(Tunnel.ControlRadii[SegmentIndex]),
                                           FMath::Abs(Tunnel.ControlRadii[SegmentIndex + 1])));
                            const FVector SegmentMin(
                                FMath::Min(A.X, BPoint.X),
                                FMath::Min(A.Y, BPoint.Y),
                                FMath::Min(A.Z, BPoint.Z));
                            const FVector SegmentMax(
                                FMath::Max(A.X, BPoint.X),
                                FMath::Max(A.Y, BPoint.Y),
                                FMath::Max(A.Z, BPoint.Z));
                            TunnelLower = FMath::Min(
                                TunnelLower,
                                ExactLatticeAabbBoxDistance(SegmentMin, SegmentMax));
                        }
                    }
                    else if (Tunnel.bHasMidpoint)
                    {
                        const FVector SegmentMin(
                            FMath::Min3(Tunnel.EndpointA.X, Tunnel.Midpoint.X,
                                        Tunnel.EndpointB.X),
                            FMath::Min3(Tunnel.EndpointA.Y, Tunnel.Midpoint.Y,
                                        Tunnel.EndpointB.Y),
                            FMath::Min3(Tunnel.EndpointA.Z, Tunnel.Midpoint.Z,
                                        Tunnel.EndpointB.Z));
                        const FVector SegmentMax(
                            FMath::Max3(Tunnel.EndpointA.X, Tunnel.Midpoint.X,
                                        Tunnel.EndpointB.X),
                            FMath::Max3(Tunnel.EndpointA.Y, Tunnel.Midpoint.Y,
                                        Tunnel.EndpointB.Y),
                            FMath::Max3(Tunnel.EndpointA.Z, Tunnel.Midpoint.Z,
                                        Tunnel.EndpointB.Z));
                        MaxRadius = FMath::Max(
                            MaxRadius,
                            FMath::Max(FMath::Abs(Tunnel.RadiusMid),
                                       FMath::Max(FMath::Abs(Tunnel.RadiusA),
                                                  FMath::Abs(Tunnel.RadiusB))));
                        TunnelLower = ExactLatticeAabbBoxDistance(SegmentMin, SegmentMax);
                    }
                    else
                    {
                        const FVector SegmentMin(
                            FMath::Min(Tunnel.EndpointA.X, Tunnel.EndpointB.X),
                            FMath::Min(Tunnel.EndpointA.Y, Tunnel.EndpointB.Y),
                            FMath::Min(Tunnel.EndpointA.Z, Tunnel.EndpointB.Z));
                        const FVector SegmentMax(
                            FMath::Max(Tunnel.EndpointA.X, Tunnel.EndpointB.X),
                            FMath::Max(Tunnel.EndpointA.Y, Tunnel.EndpointB.Y),
                            FMath::Max(Tunnel.EndpointA.Z, Tunnel.EndpointB.Z));
                        TunnelLower = ExactLatticeAabbBoxDistance(SegmentMin, SegmentMax);
                    }
                    TunnelLower -= MaxRadius;
                    const float FastTunnelLower = TunnelLower;
                    if (TunnelLower < ThresholdWithBlend)
                    {
                        ++ExactTunnelRefinements;
                        TunnelLower = ExactTunnelLower(Tunnel);
                    }
                    if (bUseExactLatticeWarp)
                    {
                        ConsiderCheapExact(FastTunnelLower, K);
                    }
                }
                else if (Tunnel.ControlPoints.Num() >= 2
                    && Tunnel.ControlRadii.Num() == Tunnel.ControlPoints.Num())
                {
                    for (int32 SegmentIndex = 0;
                         SegmentIndex + 1 < Tunnel.ControlPoints.Num();
                         ++SegmentIndex)
                    {
                        const FVector& A = Tunnel.ControlPoints[SegmentIndex];
                        const FVector& BPoint = Tunnel.ControlPoints[SegmentIndex + 1];
                        MaxRadius = FMath::Max(
                            MaxRadius,
                            FMath::Max(FMath::Abs(Tunnel.ControlRadii[SegmentIndex]),
                                       FMath::Abs(Tunnel.ControlRadii[SegmentIndex + 1])));
                        const FVector SegmentMin(
                            FMath::Min(A.X, BPoint.X),
                            FMath::Min(A.Y, BPoint.Y),
                            FMath::Min(A.Z, BPoint.Z));
                        const FVector SegmentMax(
                            FMath::Max(A.X, BPoint.X),
                            FMath::Max(A.Y, BPoint.Y),
                            FMath::Max(A.Z, BPoint.Z));
                        TunnelLower = FMath::Min(
                            TunnelLower,
                            bUseLatticeProof
                                ? LatticeBoxDistance(SegmentMin, SegmentMax, WarpEnvelope)
                                : VF_DistanceBetweenBoxes(QMin, QMax, SegmentMin, SegmentMax));
                    }
                    TunnelLower -= MaxRadius;
                }
                else
                {
                    const FVector SegmentMin(
                        FMath::Min(Tunnel.EndpointA.X, Tunnel.EndpointB.X),
                        FMath::Min(Tunnel.EndpointA.Y, Tunnel.EndpointB.Y),
                        FMath::Min(Tunnel.EndpointA.Z, Tunnel.EndpointB.Z));
                    const FVector SegmentMax(
                        FMath::Max(Tunnel.EndpointA.X, Tunnel.EndpointB.X),
                        FMath::Max(Tunnel.EndpointA.Y, Tunnel.EndpointB.Y),
                        FMath::Max(Tunnel.EndpointA.Z, Tunnel.EndpointB.Z));
                    TunnelLower = (bUseLatticeProof
                        ? LatticeBoxDistance(SegmentMin, SegmentMax, WarpEnvelope)
                        : VF_DistanceBetweenBoxes(QMin, QMax, SegmentMin, SegmentMax)) - MaxRadius;
                }

                const FVector NoWarpSegmentMin(
                    FMath::Min(Tunnel.EndpointA.X, Tunnel.EndpointB.X),
                    FMath::Min(Tunnel.EndpointA.Y, Tunnel.EndpointB.Y),
                    FMath::Min(Tunnel.EndpointA.Z, Tunnel.EndpointB.Z));
                const FVector NoWarpSegmentMax(
                    FMath::Max(Tunnel.EndpointA.X, Tunnel.EndpointB.X),
                    FMath::Max(Tunnel.EndpointA.Y, Tunnel.EndpointB.Y),
                    FMath::Max(Tunnel.EndpointA.Z, Tunnel.EndpointB.Z));
                const float TunnelLowerNoWarp = VF_DistanceBetweenBoxes(
                    NoWarpMin, NoWarpMax, NoWarpSegmentMin, NoWarpSegmentMax)
                    - MaxRadius;
                Consider(TunnelLower);
                if (bUseExactLatticeWarp) { ConsiderExact(TunnelLower, K); }
                CountThreshold(TunnelLower, B.HitTunnels);
                CountThreshold(TunnelLowerNoWarp, B.HitTunnelsNoWarp);
            }

            // The pointwise air probe above is intentionally narrower than the final generator
            // tail: a tunnel can write a solid swept floor at lattice points where its core SDF
            // is not negative.  Use the morphology module's shared broad reach predicate for the
            // complete world-space core/floor envelope; it is the same bound used by the mesher's
            // post-reach decision and keeps this diagnostic conservative for both polarities.
            if (!B.bMayHaveTunnelCoreTail)
            {
                B.bMayHaveTunnelCoreTail =
                    VoxelCaveMorphology::AnyTunnelCoreWorldNearLattice(
                        GetBoxCache(), VoxelBox, 1.0f);
            }

            if (B.bMayHaveTunnelCoreAir
                && bUseLatticeProof
                && ExactLatticeWorldQueries.Num() > 0)
            {
                // The bound test above is only a broad candidate.  The actual post is a
                // continuous writer: it opens a point when the world-core SDF is negative. This
                // is still a proof of the final lattice, never a density shortcut; if an exact
                // query is invalid, keep the conservative candidate.
                bool bExactAir = false;
                bool bExactTailUnknown = false;
                TArray<FBox, TInlineAllocator<16>> ExactTailDomains;
                bool bExactTailDomainCullSafe = true;
                const uint64 ExactTailStartCycles = FPlatformTime::Cycles64();
                for (const FCachedTunnel& Tunnel : GetBoxCache().Tunnels)
                {
                    const bool bHasWorldChain = Tunnel.WorldControlPoints.Num() >= 2
                        && Tunnel.WorldControlRadii.Num() == Tunnel.WorldControlPoints.Num();
                    const FVector& CenterlineMin = bHasWorldChain
                        ? Tunnel.WorldCenterlineMin : Tunnel.SDFCenterlineMin;
                    const FVector& CenterlineMax = bHasWorldChain
                        ? Tunnel.WorldCenterlineMax : Tunnel.SDFCenterlineMax;
                    const float InfluenceRadius = bHasWorldChain
                        ? Tunnel.WorldInfluenceRadius : Tunnel.SDFInfluenceRadius;
                    // EvaluateTunnel's first cull is exactly the distance to this AABB.  Turning
                    // it into a point-in-expanded-box test lets the exact tail reject points
                    // before it builds a support column or scans every tunnel.  A disabled,
                    // inverted, or non-finite bound cannot authorize a reject: fall back to the
                    // existing fully conservative evaluator for the whole query set.
                    if (!Finite((float)CenterlineMin.X) || !Finite((float)CenterlineMin.Y)
                        || !Finite((float)CenterlineMin.Z) || !Finite((float)CenterlineMax.X)
                        || !Finite((float)CenterlineMax.Y) || !Finite((float)CenterlineMax.Z)
                        || CenterlineMin.X > CenterlineMax.X
                        || CenterlineMin.Y > CenterlineMax.Y
                        || CenterlineMin.Z > CenterlineMax.Z
                        || !Finite(InfluenceRadius) || InfluenceRadius <= 0.0f)
                    {
                        bExactTailDomainCullSafe = false;
                        break;
                    }
                    const FVector Radius(InfluenceRadius, InfluenceRadius, InfluenceRadius);
                    ExactTailDomains.Emplace(CenterlineMin - Radius, CenterlineMax + Radius);
                }
                ForEachExactQueryInWorldBox(
                    VoxelBox.Min, VoxelBox.Max,
                    [&](const FVector&, const FVector& World)
                {
                    if (bExactAir || bExactTailUnknown) return;
                    ++B.ExactTailQueries;
                    if (bExactTailDomainCullSafe && ExactTailDomains.Num() > 0)
                    {
                        bool bInsideTailDomain = false;
                        for (const FBox& Domain : ExactTailDomains)
                        {
                            if (World.X >= Domain.Min.X && World.X <= Domain.Max.X
                                && World.Y >= Domain.Min.Y && World.Y <= Domain.Max.Y
                                && World.Z >= Domain.Min.Z && World.Z <= Domain.Max.Z)
                            {
                                bInsideTailDomain = true;
                                break;
                            }
                        }
                        if (!bInsideTailDomain) return;
                    }

                    uint8* CachedTailState = nullptr;
                    if (B.bExactWarpedLatticeCacheValid
                        && B.ExactTailStates.Num() == B.ExactWarpedLatticeQueries.Num())
                    {
                        const int32 TailIX = FMath::RoundToInt(
                            (World.X - LatticeOriginX) / LatticeStepXY);
                        const int32 TailIY = FMath::RoundToInt(
                            (World.Y - LatticeOriginY) / LatticeStepXY);
                        const int32 TailIZ = FMath::RoundToInt(
                            (World.Z - static_cast<float>(Ctx.LatticeOriginVoxels.Z))
                            / static_cast<float>(Ctx.Step));
                        if (TailIX >= B.ExactWarpedLatticeIX0
                            && TailIX < B.ExactWarpedLatticeIX0 + B.ExactWarpedLatticeNX
                            && TailIY >= B.ExactWarpedLatticeIY0
                            && TailIY < B.ExactWarpedLatticeIY0 + B.ExactWarpedLatticeNY
                            && TailIZ >= B.ExactWarpedLatticeIZ0
                            && TailIZ < B.ExactWarpedLatticeIZ0 + B.ExactWarpedLatticeNZ)
                        {
                            const int32 TailIndex =
                                ((TailIZ - B.ExactWarpedLatticeIZ0)
                                    * B.ExactWarpedLatticeNY
                                 + (TailIY - B.ExactWarpedLatticeIY0))
                                    * B.ExactWarpedLatticeNX
                                + (TailIX - B.ExactWarpedLatticeIX0);
                            CachedTailState = &B.ExactTailStates[TailIndex];
                        }
                    }
                    if (CachedTailState != nullptr && (*CachedTailState & 0x80u) != 0)
                    {
                        const uint8 State = *CachedTailState;
                        if ((State & 0x04u) != 0)
                        {
                            bExactTailUnknown = true;
                        }
                        else
                        {
                            bExactAir |= (State & 0x01u) != 0;
                        }
                        return;
                    }

                    const FTunnelCoreWorldEvaluation Evaluation =
                        VoxelCaveMorphology::EvaluateTunnelCoreWorld(
                            static_cast<float>(World.X), static_cast<float>(World.Y),
                            static_cast<float>(World.Z), GetBoxCache(), nullptr,
                            VoxelGenLOD::ShouldUseSpatialIndex(false));
                    ++B.ExactTailEvaluated;
                    if (!VoxelMath::IsFinite(Evaluation.SDF))
                    {
                        bExactTailUnknown = true;
                        if (CachedTailState != nullptr) { *CachedTailState = 0x84u; }
                    }
                    else
                    {
                        const bool bAir = Evaluation.SDF < 0.0f;
                        bExactAir |= bAir;
                        if (CachedTailState != nullptr)
                        {
                            *CachedTailState = static_cast<uint8>(
                                0x80u
                                | (bAir ? 0x01u : 0u));
                        }
                    }
                        });
                B.ExactTailCycles = FPlatformTime::Cycles64() - ExactTailStartCycles;
                if (!bExactTailUnknown)
                {
                    B.bMayHaveTunnelCoreAir = bExactAir;
                    B.bMayHaveTunnelSupportFloor = false;
                    // Keep the broad geometric tail candidate even when the exact lattice has
                    // no core-air vertex.  The final generator tail writes both polarities:
                    // it reopens tunnel air, but it can also write a solid swept floor below an
                    // otherwise-air stack box.  Collapsing this flag to bExactAir erased the
                    // latter reach and let the classifier certify a false AllAir child.
                }
            }

            for (const FCachedPit& Pit : GetBoxCache().Pits)
            {
                if (!(BoxMinZ < Pit.TopZ + Pit.BlendK)
                    || !(BoxMaxZ >= Pit.TopZ - Pit.Depth - Pit.BlendK))
                {
                    continue;
                }
                const float MaxRadius = FMath::Abs(Pit.Radius) + FMath::Abs(Pit.FlareExtra);
                const float FastPitLower = bUseExactLatticeWarp
                    ? LatticeXYPointDistance(Pit.CenterX, Pit.CenterY) - MaxRadius
                    : (bUseLatticeProof
                        ? LatticeXYPointDistance(Pit.CenterX, Pit.CenterY)
                        : VF_DistanceBoxToPointXY(FBox(RMin, RMax), Pit.CenterX, Pit.CenterY)) - MaxRadius;
                const float PitLower = bUseExactLatticeWarp
                    && FastPitLower < ThresholdWithBlend
                    ? (ExactPitRefinements++, ExactPitLower(Pit)) : FastPitLower;
                Consider(PitLower);
                if (bUseExactLatticeWarp) { ConsiderCheapExact(FastPitLower, Pit.BlendK); }
                if (bUseExactLatticeWarp) { ConsiderExact(PitLower, Pit.BlendK); }
                CountThreshold(PitLower, B.HitPits);
            }

            for (const FCachedChimney& Chimney : GetBoxCache().Chimneys)
            {
                if (!(BoxMaxZ > Chimney.BottomZ - Chimney.BlendK)
                    || !(BoxMinZ <= Chimney.BottomZ + Chimney.Height + Chimney.BlendK))
                {
                    continue;
                }
                const float MaxRadius = FMath::Abs(Chimney.Radius) + FMath::Abs(Chimney.FlareExtra);
                const float FastChimneyLower = bUseExactLatticeWarp
                    ? LatticeXYPointDistance(Chimney.CenterX, Chimney.CenterY) - MaxRadius
                    : (bUseLatticeProof
                        ? LatticeXYPointDistance(Chimney.CenterX, Chimney.CenterY)
                        : VF_DistanceBoxToPointXY(FBox(RMin, RMax), Chimney.CenterX, Chimney.CenterY)) - MaxRadius;
                const float ChimneyLower = bUseExactLatticeWarp
                    && FastChimneyLower < ThresholdWithBlend
                    ? (ExactChimneyRefinements++, ExactChimneyLower(Chimney)) : FastChimneyLower;
                Consider(ChimneyLower);
                if (bUseExactLatticeWarp) { ConsiderCheapExact(FastChimneyLower, Chimney.BlendK); }
                if (bUseExactLatticeWarp) { ConsiderExact(ChimneyLower, Chimney.BlendK); }
                CountThreshold(ChimneyLower, B.HitChimneys);
            }
            B.ExactPrimitiveCycles = FPlatformTime::Cycles64() - ExactPrimitiveStartCycles;

            if (bInvalidBound)
            {
                Unknown();
                return;
            }

            FVoxelBoxSdfInterval Own;
            if (bUseExactLatticeWarp)
            {
                // Exact per-primitive scans are a tightening only.  Their candidate-domain
                // filters can miss a warped lattice query near a primitive, so retain the
                // broad lower bound over the AABB that contains every warped sample.  The
                // minimum stays useful when both agree and remains safe when an exact scan is
                // incomplete.
                Own.Set(FMath::Min(ExactSdfLower, CheapSdfLower), FLT_MAX);
            }
            else if (!bAnyPrimitive)
            {
                Own.Set(FLT_MAX, FLT_MAX);
            }
            else
            {
                Own.Set(VF_SaturatingAdd(Lower, -K), FLT_MAX);
            }

            B.SdfInterval = Own;
            B.Verdict = Own.IsKnown() && Own.Min >= WormThreshold
                       ? EVoxelOpEffect::Identity : EVoxelOpEffect::Both;
            B.KeyBox = VoxelBox;
            B.KeyStrate = StrateIdx;
            B.KeySeed = SeedU;
            B.KeyFingerprint = ParamsFingerprint;
            B.KeyLayout = LV;
            B.KeyManagerLifetimeId = ManagerLifetimeId;
            B.KeyUsesLatticeProof = bUseLatticeProof;
            B.KeyTightenWarpProof = Ctx.bTightenWarpProof;
            B.KeyLatticeOrigin = Ctx.LatticeOriginVoxels;
            B.KeyLatticeStep = Ctx.Step;
            B.bValid = true;
            InOut = Own;
            B.PropagateCycles = FPlatformTime::Cycles64() - PropagateStartCycles;
        }

        const TCHAR* DebugName() const override { return TEXT("RoomGraphSource"); }

        void EvalBlock(const FVoxelOpBlock& Block) const override
        {
            VF_EvalBlockByScalar(*this, Block);
        }

    private:
        FStrateGenerationParams P;
        int32  Seed;
        uint32 SeedU;
        TWeakObjectPtr<const UVoxelStrateManager> Manager;
        uint64 ManagerLifetimeId = 0;
        uint32 ParamsFingerprint;
        uint32 LayoutVersion = 0;
    };

    //=========================================================================
    // LE GATE DE BOÎTE DES MODIFICATEURS DE CAVE
    //=========================================================================
    // The source publishes an SDF interval; detail modifiers consume that interval here. This is
    // deliberately a state test, never a call to the source's EffectOverBox: the latter is the
    // source-only answer and must remain valid when the composer chooses a different consumer.
    FORCEINLINE bool VF_CaveBoxIsFar(const FVoxelBoxHypotheses& H, float SDFBlendRadius)
    {
        const float Threshold = SDFBlendRadius * 3.0f;
        return VoxelMath::IsFinite(Threshold) && H.Sdf.IsKnown() && H.Sdf.Min >= Threshold;
    }

    FORCEINLINE EVoxelOpEffect VF_CaveDetailEffect(const FRoomGraphSource* Rooms,
                                                   const FVoxelBoxHypotheses& H,
                                                   float SDFBlendRadius,
                                                   EVoxelOpEffect Intrinsic)
    {
        // A detail op with no room source has the same no-op guard as Eval. An unknown interval is
        // not a reason to claim Identity: uncertainty costs a skip, while a false skip is a hole.
        if (Rooms == nullptr || VF_CaveBoxIsFar(H, SDFBlendRadius))
        {
            return EVoxelOpEffect::Identity;
        }
        // A terrain op is allowed to replace the strate-level activation fields for the room that
        // Eval selects.  Its exact direction is not part of the generic cache proof, so preserve
        // safety with Both whenever one is present.  The common no-override TunnelNetwork path
        // gets the real intrinsic direction and finite amplitude below.
        return Rooms->HasRoomTerrainOverrideForLastBox()
             ? EVoxelOpEffect::Both : Intrinsic;
    }

    FORCEINLINE float VF_CaveDetailMax(const FRoomGraphSource* Rooms,
                                       const FVoxelBoxHypotheses& H,
                                       float SDFBlendRadius,
                                       float IntrinsicMax)
    {
        // Per-room overrides can activate or enlarge a modifier even when the strate-level
        // parameter is zero.  A valid cache with no override makes the strate-level supremum
        // exact for these modifiers; an unknown/overridden cache keeps the old safe default.
        if (Rooms == nullptr || VF_CaveBoxIsFar(H, SDFBlendRadius)) { return 0.0f; }
        if (Rooms->HasRoomTerrainOverrideForLastBox()) { return FLT_MAX; }
        return IntrinsicMax;
    }

    //=========================================================================
    // RÔLE 3 — MODIFIER : RUGOSITÉ DE PAROI, ESPACE DENSITÉ  (TunnelNetwork, STEP 4b)
    //=========================================================================
    // ⚠️ CE N'EST PAS `FSdfRoughnessMod`, ET C'EST LE PIÈGE QUE `OPSTACK-DECOMPOSITION §1` SIGNALE.
    // Les deux s'appellent « rugosité de surface » et lisent le même champ de params, mais :
    //
    //   • variante SDF (Maze / VerticalShafts / FloatingIslands) : `Sdf += bruit·SCALE·Force`.
    //     Brut, sans fade, sans clamp, fréquence codée en dur au site d'appel. Déplace la SURFACE.
    //   • variante DENSITÉ (ici) : DEUX jeux d'octaves (principal + fin ×3), warp de domaine
    //     optionnel, QUATRE types de bruit, un `Min(…, 0)` anti-remplissage, et un fade QUADRATIQUE
    //     par distance à la surface. Déplace la MATIÈRE, mise à l'échelle par le gradient local.
    //
    // Les fusionner sous un enum `Space` était la suggestion du §1 ; en les portant, ils n'ont
    // presque aucune ligne en commun (le clamp, le fade et le second jeu d'octaves n'ont pas
    // d'équivalent dans l'autre). Deux opérateurs, un nom partagé — comme `FGridColumnMod` et le
    // futur `FRoomColumnMod`, que le §1 sépare pour la même raison.
    //
    // ⚠️⚠️ CET OPÉRATEUR EST **HORS** DE L'OVERRIDE D'OP PAR SALLE, et ce n'est pas un oubli.
    // Dans l'original, le shadow `const FStrateGenerationParams& Params = LocalTerrainParams;` est
    // déclaré à l'INTÉRIEUR du bloc `if (bNearCaveSurface)` qui commence APRÈS l'étape 4b. La
    // rugosité lit donc les params de la STRATE, jamais ceux de la salle la plus proche. Onze
    // modificateurs sur douze lisent la copie par salle ; celui-ci non. À NE PAS « uniformiser »
    // à l'étape C1.
    //
    // This op reads STRATE params, not the per-room copy: the original's shadow is declared inside
    // the `if (bNearCaveSurface)` block that starts AFTER step 4b. Eleven of twelve modifiers read
    // the shadowed copy; this one does not.
    class FCaveRoughnessMod final : public IVoxelDensityOp
    {
    public:
        FCaveRoughnessMod(const FStrateGenerationParams& InP, int32 Seed)
            : P(InP), SeedU((uint32)Seed) {}

        EVoxelOpRole GetRole() const override { return EVoxelOpRole::DetailModifier; }
        EVoxelOpChannelMask ChannelReads() const override
        {
            return VoxelOpChannels::Density | VoxelOpChannels::Sdf;
        }
        EVoxelOpChannelMask ChannelWrites() const override { return VoxelOpChannels::Density; }
        bool IsAdditive() const override { return true; }
        void PrepareChunk(const FVoxelOpContext&) override {}
        const TCHAR* DebugName() const override { return TEXT("CaveRoughnessMod"); }
        void EvalBlock(const FVoxelOpBlock& Block) const override { VF_EvalBlockByScalar(*this, Block); }

        void Eval(float WorldX, float WorldY, float WorldZ, FVoxelOpSample& InOut) const override
        {
            const float CaveSDF = InOut.Sdf;
            if (!VF_NearCaveSurface(CaveSDF, P.SDFBlendRadius)) { return; }
            if (!(P.SurfaceRoughness > 0.0f)) { return; }

            const float EffectiveZ = (P.VerticalScale != 1.0f && P.VerticalScale > 0.0f)
                                   ? (WorldZ / P.VerticalScale) : WorldZ;

            const float RoughnessDepth  = P.SurfaceRoughness * 2.0f;
            const float DistFromSurface = FMath::Abs(CaveSDF);
            if (!(DistFromSurface < RoughnessDepth)) { return; }

            const float RF = P.RoughnessFrequency;

            // Keep both hot noise-coordinate chains in float. The old FVector round-trip only
            // preserved legacy rounding; output compatibility with that field is abandoned.
            FVector3f MainPos(
                WorldX * RF + VoxelHash::SeedOffset(SeedU, 11.3f),
                WorldY * RF + VoxelHash::SeedOffset(SeedU, 13.7f),
                EffectiveZ * RF + VoxelHash::SeedOffset(SeedU, 17.1f)
            );
            FVector3f FinePos(
                WorldX * RF * 3.0f + VoxelHash::SeedOffset(SeedU, 19.1f) + 2000.0f,
                WorldY * RF * 3.0f + VoxelHash::SeedOffset(SeedU, 23.7f) + 2500.0f,
                EffectiveZ * RF * 3.0f + VoxelHash::SeedOffset(SeedU, 29.3f) + 3000.0f
            );

            // WARP DE DOMAINE : le MÊME offset est ajouté aux DEUX positions (une seule
            // `FVector WarpOffset`, deux `+=`). Transcrit tel quel — appliquer deux warps
            // indépendants serait plus « propre » et donnerait un autre monde.
            if (P.DomainWarpStrength > 0.0f)
            {
                const float WF = P.DomainWarpFrequency;
                const float WS = P.DomainWarpStrength;

                const float WarpX = VoxelNoise::Perlin3D(FVector3f(
                    WorldX * WF + VoxelHash::SeedOffset(SeedU, 5.2f),
                    WorldY * WF + VoxelHash::SeedOffset(SeedU, 1.3f),
                    EffectiveZ * WF + VoxelHash::SeedOffset(SeedU, 9.7f)
                )) * VOXEL_NOISE_SCALE * WS;

                const float WarpY = VoxelNoise::Perlin3D(FVector3f(
                    WorldX * WF + 100.0f + VoxelHash::SeedOffset(SeedU, 7.7f),
                    WorldY * WF + 200.0f + VoxelHash::SeedOffset(SeedU, 3.1f),
                    EffectiveZ * WF + 300.0f
                )) * VOXEL_NOISE_SCALE * WS;

                const float WarpZ = VoxelNoise::Perlin3D(FVector3f(
                    WorldX * WF + 400.0f,
                    WorldY * WF + 500.0f + VoxelHash::SeedOffset(SeedU, 11.9f),
                    EffectiveZ * WF + 600.0f + VoxelHash::SeedOffset(SeedU, 13.3f)
                )) * VOXEL_NOISE_SCALE * WS;

                const FVector3f WarpOffset(WarpX, WarpY, WarpZ);
                MainPos += WarpOffset;
                FinePos += WarpOffset;
            }

            // Les comptes d'octaves passent par VoxelGenLOD::Eff — contrat T2.b, les tuiles
            // lointaines perdent les octaves sous-cellulaires.
            float RoughNoise, FineNoise;
            const int32 Oct3 = VoxelGenLOD::Eff(3);
            const int32 Oct2 = VoxelGenLOD::Eff(2);

            switch (P.RoughnessNoiseType)
            {
            case EVoxelNoiseType::Ridged:
                RoughNoise = HRidged3D(MainPos, Oct3);
                FineNoise  = HRidged3D(FinePos, Oct2);
                break;

            case EVoxelNoiseType::Mixed:
                RoughNoise = HFractal3D(MainPos, Oct3) * 0.5f
                           + HRidged3D(MainPos, Oct3) * 0.5f;
                FineNoise  = HFractal3D(FinePos, Oct2) * 0.5f
                           + HRidged3D(FinePos, Oct2) * 0.5f;
                break;

            case EVoxelNoiseType::Cellular:
                RoughNoise = VoxelNoise::Cellular3D(MainPos);
                FineNoise  = VoxelNoise::Cellular3D(FinePos);
                break;

            case EVoxelNoiseType::FBM:
            default:
                RoughNoise = HFractal3D(MainPos, Oct3);
                FineNoise  = HFractal3D(FinePos, Oct2);
                break;
            }

            RoughNoise *= VOXEL_NOISE_SCALE;
            FineNoise  *= VOXEL_NOISE_SCALE;

            float TotalRough = RoughNoise * P.SurfaceRoughness
                             + FineNoise * P.SurfaceRoughness * 0.4f;

            // CLAMP ANTI-REMPLISSAGE : dans l'air certain (SDF < 0) la rugosité ne doit JAMAIS
            // rajouter du solide — sinon lucarnes, membranes, coutures aux jonctions et aux lèvres
            // de puits. Elle peut encore creuser plus loin dans la paroi.
            if (CaveSDF < 0.0f)
            {
                TotalRough = FMath::Min(TotalRough, 0.0f);
            }

            float SurfaceFade = 1.0f - (DistFromSurface / RoughnessDepth);
            SurfaceFade = SurfaceFade * SurfaceFade;   // quadratique : concentre près de la surface

            InOut.Density += TotalRough * SurfaceFade;
        }

        /**
         * `Both` : la rugosité peut pousser dans les deux sens (le clamp ne s'applique que dans
         * l'air certain). Conservatif et donc correct, mais coûteux — comme les vers, c'est un
         * opérateur dont l'AMPLITUDE est bornée alors que sa DIRECTION ne l'est pas :
         *   |TotalRough| ≤ 2.1 · SurfaceRoughness · VOXEL_NOISE_SCALE,  fade ∈ [0,1].
         * Deuxième client pour le pliage numérique de `OPSTACK-DECOMPOSITION §0.2`, noté au point
         * exact où la borne manque (le premier est `FWormFieldSource::MaxCarveAmplitude`).
         */
        EVoxelOpEffect EffectOverBox(const FBox&, const FVoxelOpContext&) const override
        {
            return (P.SurfaceRoughness > 0.0f) ? EVoxelOpEffect::Both : EVoxelOpEffect::Identity;
        }

        EVoxelOpEffect EffectOverBox(const FBox& VoxelBox, const FVoxelOpContext& Ctx,
                                     const FVoxelBoxHypotheses& H) const override
        {
            const EVoxelOpEffect Intrinsic = EffectOverBox(VoxelBox, Ctx);
            if (Intrinsic == EVoxelOpEffect::Identity) { return Intrinsic; }
            return IsInactiveFromSdf(H) ? EVoxelOpEffect::Identity : Intrinsic;
        }

        /**
         * ✅ Borne consommée par le pliage numérique. `RoughNoise` et `FineNoise` respectent tous
         * `FBM` est couvert par la borne prouvée `VF_PerlinAbsBound = 1.5`, mis à l'échelle par
         * `VOXEL_NOISE_SCALE`.  `Ridged` is in [-1,1] after its square/fold, while the unclamped
         * cellular mapping needs its own finite envelope below.  The two octaves contribute
         * `|Rough| + 0.4·|Fine|`; the fade is in `[0,1]`.  The anti-fill clamp only reduces the
         * positive side, so the same envelope is valid in both directions.
         */
        float MaxCarveOverBox(const FBox&, const FVoxelOpContext&) const override { return MaxAmplitude(); }
        float MaxFillOverBox (const FBox&, const FVoxelOpContext&) const override { return MaxAmplitude(); }

        float MaxCarveOverBox(const FBox& VoxelBox, const FVoxelOpContext& Ctx,
                              const FVoxelBoxHypotheses& H) const override
        {
            return IsInactiveFromSdf(H) ? 0.0f : MaxCarveOverBox(VoxelBox, Ctx);
        }

        float MaxFillOverBox(const FBox& VoxelBox, const FVoxelOpContext& Ctx,
                             const FVoxelBoxHypotheses& H) const override
        {
            return IsInactiveFromSdf(H) ? 0.0f : MaxFillOverBox(VoxelBox, Ctx);
        }

        /** La borne d'amplitude, en unités de densité. */
        float MaxAmplitude() const
        {
            if (!(P.SurfaceRoughness > 0.0f)) { return 0.0f; }
            if (!VoxelMath::IsFinite(P.SurfaceRoughness)) { return FLT_MAX; }

            float NoiseBound = VF_PerlinAbsBound;
            switch (P.RoughnessNoiseType)
            {
            case EVoxelNoiseType::Ridged:
                NoiseBound = 1.0f;
                break;
            case EVoxelNoiseType::Mixed:
                NoiseBound = 0.5f * (VF_PerlinAbsBound + 1.0f);
                break;
            case EVoxelNoiseType::Cellular:
                NoiseBound = VF_CellularAbsBound;
                break;
            case EVoxelNoiseType::FBM:
            default:
                NoiseBound = VF_PerlinAbsBound;
                break;
            }

            const float Result = 1.4f * NoiseBound * P.SurfaceRoughness * VOXEL_NOISE_SCALE;
            return VoxelMath::IsFinite(Result) ? Result : FLT_MAX;
        }

    private:
        bool IsInactiveFromSdf(const FVoxelBoxHypotheses& H) const
        {
            if (VF_CaveBoxIsFar(H, P.SDFBlendRadius)) { return true; }
            if (!(P.SurfaceRoughness > 0.0f) || !VoxelMath::IsFinite(P.SurfaceRoughness))
            {
                return P.SurfaceRoughness <= 0.0f;
            }
            const float RoughnessDepth = P.SurfaceRoughness * 2.0f;
            // Eval's second gate is abs(CaveSDF) < SurfaceRoughness*2.  A known positive
            // lower endpoint at or beyond that depth therefore proves the op is an identity even
            // when the broader shared near-surface gate (SDFBlendRadius*3) remains open.
            return VoxelMath::IsFinite(RoughnessDepth) && H.Sdf.IsKnown()
                && H.Sdf.Min >= RoughnessDepth;
        }

        FStrateGenerationParams P;
        uint32 SeedU;
    };

    //=========================================================================
    // RÔLE 3 — MODIFIER : TERRASSES / TERRACING  (TunnelNetwork, STEP 4c)
    //=========================================================================
    // Un escalier lissé en Z : `Offset = staircase(Z) − Z` ajouté à la densité. Positif ⇒ marche
    // solide sur laquelle marcher, négatif ⇒ vide sous la marche du dessus.
    //
    // ⚠️ LE SEUL MODIFICATEUR QUI RE-INTERROGE LE CHAMP SDF. Le facteur d'orientation vient de deux
    // sondes en Z±1 : un SDF a un gradient ≈ unitaire, donc |dSDF/dZ| EST déjà la composante
    // verticale normalisée — proche de 1 = sol/plafond, proche de 0 = paroi. Sans ce facteur, les
    // terrasses posent des bourrelets horizontaux dans les puits verticaux.
    //
    // C'est pour cette re-interrogation que `FRoomGraphSource` expose `ProbeSdfUnwarped` (et donc
    // que son cache est sorti de `Eval`) : le refaire ici voudrait dire un second cache SDF.
    class FCaveTerraceMod final : public IVoxelDensityOp
    {
    public:
        FCaveTerraceMod(const FStrateGenerationParams& InP, int32 Seed, const FRoomGraphSource* InRooms)
            : P(InP), SeedU((uint32)Seed), Rooms(InRooms) {}

        EVoxelOpRole GetRole() const override { return EVoxelOpRole::DetailModifier; }
        EVoxelOpChannelMask ChannelReads() const override
        {
            return VoxelOpChannels::Density | VoxelOpChannels::Sdf;
        }
        EVoxelOpChannelMask ChannelWrites() const override { return VoxelOpChannels::Density; }
        EVoxelOpResourceMask RequiredResources() const override { return VoxelOpResources::RoomGeometry; }
        bool IsAdditive() const override { return true; }
        void PrepareChunk(const FVoxelOpContext&) override {}
        void PrepareBlockSample(const FVoxelOpSample& Sample) const override
        {
            if (Rooms != nullptr) { Rooms->RestoreBlockSample(Sample); }
        }
        const TCHAR* DebugName() const override { return TEXT("CaveTerraceMod"); }
        void EvalBlock(const FVoxelOpBlock& Block) const override { VF_EvalBlockByScalar(*this, Block); }

        void Eval(float WorldX, float WorldY, float WorldZ, FVoxelOpSample& InOut) const override
        {
            const float CaveSDF = InOut.Sdf;
            if (!VF_NearCaveSurface(CaveSDF, P.SDFBlendRadius)) { return; }
            if (Rooms == nullptr) { return; }

            // `&& CaveSDF < FLT_MAX` est redondant sous le gate (qui le teste déjà) mais l'original
            // l'écrit, et une transcription littérale ne fait pas le tri.
            const FStrateGenerationParams& LP = Rooms->LocalParams();   // C1 : params PAR SALLE
            if (!(LP.TerraceStepHeight > 0.0f && CaveSDF < FLT_MAX)) { return; }

            const float StepH = LP.TerraceStepHeight;
            const float DistFromSurface = FMath::Abs(CaveSDF);
            const float TerraceRange = StepH * 3.0f;
            if (!(DistFromSurface < TerraceRange)) { return; }

            // Deux évaluations SDF de plus par voxel, seulement près d'une surface. Approximation
            // en Z seul : le gradient complet à 6 échantillons coûtait 3× pour le même signal.
            const float SDF_Zp1 = Rooms->ProbeSdfUnwarped(WorldX, WorldY, WorldZ + 1.0f);
            const float SDF_Zm1 = Rooms->ProbeSdfUnwarped(WorldX, WorldY, WorldZ - 1.0f);
            const float GZ = (SDF_Zp1 - SDF_Zm1) * 0.5f;
            const float SurfaceHorizontality = FMath::Clamp(FMath::Abs(GZ), 0.0f, 1.0f);
            const float TerraceOrientFactor = FMath::Clamp((SurfaceHorizontality - 0.3f) / 0.4f, 0.0f, 1.0f);

            // ⚠️ Z BRUT, PAS `EffectiveZ` : les terrasses sont géologiques, elles restent
            // horizontales quelle que soit l'échelle verticale de la strate. Idem pour les lignes de
            // strates et les nervures plus bas. La rugosité (4b), elle, utilise EffectiveZ. C'est
            // délibéré dans l'original et ça se lit dans son commentaire d'en-tête du STEP 3.
            float NoisedZ = WorldZ;
            if (LP.TerraceNoiseDisplacement > 0.0f)
            {
                const float DispNoise = HFractal3D(FVector3f(
                    WorldX * 0.04f + VoxelHash::SeedOffset(SeedU, 31.1f),
                    WorldY * 0.04f + VoxelHash::SeedOffset(SeedU, 37.3f),
                    WorldZ * 0.02f + VoxelHash::SeedOffset(SeedU, 41.7f)
                ), VoxelGenLOD::Eff(2)) * VOXEL_NOISE_SCALE;
                NoisedZ += DispNoise * LP.TerraceNoiseDisplacement * StepH;
            }

            const float K = NoisedZ / StepH;
            const float FloorK = FMath::FloorToFloat(K);
            const float Frac = K - FloorK;   // toujours [0, 1)

            const float Edge = FMath::Lerp(0.45f, 0.02f, LP.TerraceHardness);
            float StairValue;
            if (Frac < 0.5f - Edge)
            {
                StairValue = 0.0f;
            }
            else if (Frac > 0.5f + Edge)
            {
                StairValue = 1.0f;
            }
            else
            {
                const float T = (Frac - (0.5f - Edge)) / (2.0f * Edge);
                StairValue = SmoothStep01(T);
            }

            const float TerracedZ = (FloorK + StairValue) * StepH;
            const float Offset = TerracedZ - NoisedZ;   // ≈ [-StepH/2, +StepH/2]

            float Fade = 1.0f - (DistFromSurface / TerraceRange);
            Fade = Fade * Fade;

            InOut.Density += Offset * Fade * TerraceOrientFactor;
        }

        /** `Both` : `Offset` change de signe d'une demi-marche à l'autre. Amplitude bornée par
         *  `StepH/2` — troisième client du pliage numérique de `OPSTACK-DECOMPOSITION §0.2`. */
        EVoxelOpEffect EffectOverBox(const FBox&, const FVoxelOpContext&) const override
        {
            return (P.TerraceStepHeight > 0.0f) ? EVoxelOpEffect::Both : EVoxelOpEffect::Identity;
        }

        EVoxelOpEffect EffectOverBox(const FBox& VoxelBox, const FVoxelOpContext& Ctx,
                                     const FVoxelBoxHypotheses& H) const override
        {
            return VF_CaveDetailEffect(Rooms, H, P.SDFBlendRadius,
                                        EffectOverBox(VoxelBox, Ctx));
        }

        float MaxCarveOverBox(const FBox&, const FVoxelOpContext&) const override
        {
            return MaxAmplitude();
        }

        float MaxFillOverBox(const FBox&, const FVoxelOpContext&) const override
        {
            return MaxAmplitude();
        }

        float MaxCarveOverBox(const FBox& VoxelBox, const FVoxelOpContext& Ctx,
                              const FVoxelBoxHypotheses& H) const override
        {
            return VF_CaveDetailMax(Rooms, H, P.SDFBlendRadius,
                                    MaxCarveOverBox(VoxelBox, Ctx));
        }

        float MaxFillOverBox(const FBox& VoxelBox, const FVoxelOpContext& Ctx,
                             const FVoxelBoxHypotheses& H) const override
        {
            return VF_CaveDetailMax(Rooms, H, P.SDFBlendRadius,
                                    MaxFillOverBox(VoxelBox, Ctx));
        }

        /**
         * `TerracedZ` is one staircase step below or above `NoisedZ`; the modulo phase is in
         * [0,1], so the safe supremum of `abs(TerracedZ - NoisedZ)` is one full step, not StepH/2.
         * The noise displacement changes which phase is selected but cannot enlarge that interval.
         */
        float MaxAmplitude() const
        {
            return (P.TerraceStepHeight > 0.0f
                    && VoxelMath::IsFinite(P.TerraceStepHeight)
                    && VoxelMath::IsFinite(P.TerraceHardness)
                    && P.TerraceHardness >= 0.0f && P.TerraceHardness <= 1.0f)
                 ? P.TerraceStepHeight : (P.TerraceStepHeight > 0.0f ? FLT_MAX : 0.0f);
        }

    private:
        FStrateGenerationParams P;
        uint32 SeedU;
        const FRoomGraphSource* Rooms;   // NON possédant : la pile possède la source
    };

    //=========================================================================
    // RÔLE 3 — MODIFIER : LIGNES DE STRATES / LAYER LINES  (TunnelNetwork, STEP 4c)
    //=========================================================================
    // Rainures horizontales dans les parois — la strate sédimentaire vue en coupe. Une sinusoïde en
    // Z, rectifiée puis CUBÉE : les bosses larges du sinus deviennent des pointes fines, donc des
    // rainures étroites au lieu d'une ondulation.
    class FLayerLineMod final : public IVoxelDensityOp
    {
    public:
        FLayerLineMod(const FStrateGenerationParams& InP, const FRoomGraphSource* InRooms)
            : P(InP), Rooms(InRooms) {}

        EVoxelOpRole GetRole() const override { return EVoxelOpRole::DetailModifier; }
        EVoxelOpChannelMask ChannelReads() const override
        {
            return VoxelOpChannels::Density | VoxelOpChannels::Sdf;
        }
        EVoxelOpChannelMask ChannelWrites() const override { return VoxelOpChannels::Density; }
        EVoxelOpResourceMask RequiredResources() const override { return VoxelOpResources::RoomGeometry; }
        bool IsAdditive() const override { return true; }
        void PrepareChunk(const FVoxelOpContext&) override {}
        void PrepareBlockSample(const FVoxelOpSample& Sample) const override
        {
            if (Rooms != nullptr) { Rooms->RestoreBlockSample(Sample); }
        }
        const TCHAR* DebugName() const override { return TEXT("LayerLineMod"); }
        void EvalBlock(const FVoxelOpBlock& Block) const override { VF_EvalBlockByScalar(*this, Block); }

        void Eval(float, float, float WorldZ, FVoxelOpSample& InOut) const override
        {
            const float CaveSDF = InOut.Sdf;
            if (!VF_NearCaveSurface(CaveSDF, P.SDFBlendRadius)) { return; }
            if (Rooms == nullptr) { return; }
            const FStrateGenerationParams& LP = Rooms->LocalParams();   // C1 : params PAR SALLE
            if (!(LP.LayerLineSpacing > 0.0f && CaveSDF < FLT_MAX)) { return; }

            const float DistFromSurface = FMath::Abs(CaveSDF);
            const float LineRange = LP.LayerLineSpacing * 1.5f;
            if (!(DistFromSurface < LineRange)) { return; }

            const float LinePhase = WorldZ * (2.0f * PI) / LP.LayerLineSpacing;
            float LineValue = VoxelMath::DetSin(LinePhase);

            LineValue = FMath::Max(LineValue, 0.0f);
            LineValue = LineValue * LineValue * LineValue;   // affûtage cubique

            float Fade = 1.0f - (DistFromSurface / LineRange);
            Fade = Fade * Fade;

            InOut.Density -= LineValue * LP.LayerLineDepth * Fade;
        }

        /** Ne SOUSTRAIT que (`LineValue ≥ 0`, `Depth ≥ 0`) ⇒ `CarveOnly`, jamais `Both`. Un des
         *  rares modificateurs de détail qui garde une DIRECTION exploitable par le pliage. */
        EVoxelOpEffect EffectOverBox(const FBox&, const FVoxelOpContext&) const override
        {
            return (P.LayerLineSpacing > 0.0f && P.LayerLineDepth > 0.0f)
                 ? EVoxelOpEffect::CarveOnly : EVoxelOpEffect::Identity;
        }

        EVoxelOpEffect EffectOverBox(const FBox& VoxelBox, const FVoxelOpContext& Ctx,
                                     const FVoxelBoxHypotheses& H) const override
        {
            return VF_CaveDetailEffect(Rooms, H, P.SDFBlendRadius,
                                        EffectOverBox(VoxelBox, Ctx));
        }

        /** `LineValue = max(sin,0)³ ∈ [0,1]`, `Fade ∈ [0,1]` ⇒ retrait ≤ `LayerLineDepth`.
         *  ⚠️ Borne calculée sur les params de la STRATE : un op `LayerLines` par salle peut écrire
         *  une profondeur PLUS GRANDE, ce qui rendrait cette borne fausse dans le sens dangereux.
         *  C'est la même dette que la note d'`EffectOverBox` juste au-dessus, et elle doit être
         *  réglée par le même correctif — AVANT que `ClassifyTile` ne consomme `ClassifyBox`. */
        float MaxCarveOverBox(const FBox&, const FVoxelOpContext&) const override
        {
            return MaxAmplitude();
        }
        float MaxFillOverBox(const FBox&, const FVoxelOpContext&) const override { return 0.0f; }

        float MaxCarveOverBox(const FBox&, const FVoxelOpContext&,
                              const FVoxelBoxHypotheses& H) const override
        {
            return VF_CaveDetailMax(Rooms, H, P.SDFBlendRadius, MaxAmplitude());
        }
        float MaxFillOverBox(const FBox&, const FVoxelOpContext&,
                             const FVoxelBoxHypotheses&) const override { return 0.0f; }

        float MaxAmplitude() const
        {
            if (!(P.LayerLineSpacing > 0.0f)) { return 0.0f; }
            return VoxelMath::IsFinite(P.LayerLineDepth)
                 ? FMath::Max(P.LayerLineDepth, 0.0f) : FLT_MAX;
        }

    private:
        FStrateGenerationParams P;
        const FRoomGraphSource* Rooms;   // NON possédant
    };

    //=========================================================================
    // RÔLE 3 — MODIFIER : NERVURES / RIBBING  (TunnelNetwork, STEP 4c)
    //=========================================================================
    // La MÊME sinusoïde en Z que les lignes de strates, décalée d'un quart de période
    // (`+ PI · 0.5`), rectifiée puis CARRÉE au lieu de cubée, et AJOUTÉE au lieu d'être soustraite :
    // des bourrelets arrondis (tube de lave) au lieu de rainures fines.
    //
    // ⚠️ Deux opérateurs, pas un avec un signe : l'exposant diffère (3 contre 2), la phase diffère,
    // et le paramètre d'espacement est indépendant. Les fusionner demanderait trois paramètres pour
    // économiser dix lignes, et rendrait la correspondance avec l'original illisible.
    class FRibbingMod final : public IVoxelDensityOp
    {
    public:
        FRibbingMod(const FStrateGenerationParams& InP, const FRoomGraphSource* InRooms)
            : P(InP), Rooms(InRooms) {}

        EVoxelOpRole GetRole() const override { return EVoxelOpRole::DetailModifier; }
        EVoxelOpChannelMask ChannelReads() const override
        {
            return VoxelOpChannels::Density | VoxelOpChannels::Sdf;
        }
        EVoxelOpChannelMask ChannelWrites() const override { return VoxelOpChannels::Density; }
        EVoxelOpResourceMask RequiredResources() const override { return VoxelOpResources::RoomGeometry; }
        bool IsAdditive() const override { return true; }
        void PrepareChunk(const FVoxelOpContext&) override {}
        void PrepareBlockSample(const FVoxelOpSample& Sample) const override
        {
            if (Rooms != nullptr) { Rooms->RestoreBlockSample(Sample); }
        }
        const TCHAR* DebugName() const override { return TEXT("RibbingMod"); }
        void EvalBlock(const FVoxelOpBlock& Block) const override { VF_EvalBlockByScalar(*this, Block); }

        void Eval(float, float, float WorldZ, FVoxelOpSample& InOut) const override
        {
            const float CaveSDF = InOut.Sdf;
            if (!VF_NearCaveSurface(CaveSDF, P.SDFBlendRadius)) { return; }
            if (Rooms == nullptr) { return; }
            const FStrateGenerationParams& LP = Rooms->LocalParams();   // C1 : params PAR SALLE
            if (!(LP.RibbingSpacing > 0.0f && CaveSDF < FLT_MAX)) { return; }

            const float DistFromSurface = FMath::Abs(CaveSDF);
            const float RibRange = LP.RibbingSpacing * 1.5f;
            if (!(DistFromSurface < RibRange)) { return; }

            const float RibPhase = WorldZ * (2.0f * PI) / LP.RibbingSpacing + PI * 0.5f;
            float RibValue = VoxelMath::DetSin(RibPhase);

            RibValue = FMath::Max(RibValue, 0.0f);
            RibValue = RibValue * RibValue;   // profil de bosse arrondi

            float Fade = 1.0f - (DistFromSurface / RibRange);
            Fade = Fade * Fade;

            InOut.Density += RibValue * LP.RibbingDepth * Fade;
        }

        /** N'AJOUTE que du solide ⇒ `FillOnly`. */
        EVoxelOpEffect EffectOverBox(const FBox&, const FVoxelOpContext&) const override
        {
            return (P.RibbingSpacing > 0.0f && P.RibbingDepth > 0.0f)
                 ? EVoxelOpEffect::FillOnly : EVoxelOpEffect::Identity;
        }

        EVoxelOpEffect EffectOverBox(const FBox& VoxelBox, const FVoxelOpContext& Ctx,
                                     const FVoxelBoxHypotheses& H) const override
        {
            return VF_CaveDetailEffect(Rooms, H, P.SDFBlendRadius,
                                        EffectOverBox(VoxelBox, Ctx));
        }

        /** `RibValue = max(sin,0)² ∈ [0,1]`, `Fade ∈ [0,1]` ⇒ ajout ≤ `RibbingDepth`.
         *  Même réserve « params de strate » que `FLayerLineMod::MaxCarveOverBox`. */
        float MaxCarveOverBox(const FBox&, const FVoxelOpContext&) const override { return 0.0f; }
        float MaxFillOverBox(const FBox&, const FVoxelOpContext&) const override
        {
            return MaxAmplitude();
        }

        float MaxCarveOverBox(const FBox&, const FVoxelOpContext&,
                              const FVoxelBoxHypotheses&) const override { return 0.0f; }
        float MaxFillOverBox(const FBox&, const FVoxelOpContext&,
                             const FVoxelBoxHypotheses& H) const override
        {
            return VF_CaveDetailMax(Rooms, H, P.SDFBlendRadius, MaxAmplitude());
        }

        float MaxAmplitude() const
        {
            if (!(P.RibbingSpacing > 0.0f)) { return 0.0f; }
            return VoxelMath::IsFinite(P.RibbingDepth)
                 ? FMath::Max(P.RibbingDepth, 0.0f) : FLT_MAX;
        }

    private:
        FStrateGenerationParams P;
        const FRoomGraphSource* Rooms;   // NON possédant
    };

    //=========================================================================
    // RÔLE 3 — MODIFIER : SURPLOMBS DE GROTTE / CAVE OVERHANGS  (TunnelNetwork, STEP 4c)
    //=========================================================================
    // ⚠️ TROISIÈME OPÉRATEUR NOMMÉ « OVERHANG » DANS CE FICHIER, et ils n'ont rien en commun :
    //   • `FOverhangShelfMod` (SurfaceWorld) emprunte la hauteur de terrain amont — géométrique ;
    //   • celui-ci est un bruit fBM à FRÉQUENCE EN Z RÉDUITE (×0.15), dont on ne garde que le lobe
    //     POSITIF : la roche ne s'étend que VERS la grotte, jamais en creux. D'où des étagères
    //     éparses au lieu d'un déplacement uniforme.
    // Même nom dans l'éditeur, deux idées différentes. `OPSTACK-DECOMPOSITION §1` les sépare déjà.
    class FCaveOverhangMod final : public IVoxelDensityOp
    {
    public:
        FCaveOverhangMod(const FStrateGenerationParams& InP, int32 Seed, const FRoomGraphSource* InRooms)
            : P(InP), SeedU((uint32)Seed), Rooms(InRooms) {}

        EVoxelOpRole GetRole() const override { return EVoxelOpRole::DetailModifier; }
        EVoxelOpChannelMask ChannelReads() const override
        {
            return VoxelOpChannels::Density | VoxelOpChannels::Sdf;
        }
        EVoxelOpChannelMask ChannelWrites() const override { return VoxelOpChannels::Density; }
        EVoxelOpResourceMask RequiredResources() const override { return VoxelOpResources::RoomGeometry; }
        bool IsAdditive() const override { return true; }
        void PrepareChunk(const FVoxelOpContext&) override {}
        void PrepareBlockSample(const FVoxelOpSample& Sample) const override
        {
            if (Rooms != nullptr) { Rooms->RestoreBlockSample(Sample); }
        }
        const TCHAR* DebugName() const override { return TEXT("CaveOverhangMod"); }
        void EvalBlock(const FVoxelOpBlock& Block) const override { VF_EvalBlockByScalar(*this, Block); }

        void Eval(float WorldX, float WorldY, float WorldZ, FVoxelOpSample& InOut) const override
        {
            const float CaveSDF = InOut.Sdf;
            if (!VF_NearCaveSurface(CaveSDF, P.SDFBlendRadius)) { return; }
            if (Rooms == nullptr) { return; }
            const FStrateGenerationParams& LP = Rooms->LocalParams();   // C1 : params PAR SALLE
            if (!(LP.OverhangStrength > 0.0f && CaveSDF < FLT_MAX)) { return; }

            const float DistFromSurface = FMath::Abs(CaveSDF);
            const float OverhangRange = LP.OverhangDepth * 2.0f;
            if (!(DistFromSurface < OverhangRange)) { return; }

            const float EffectiveZ = (P.VerticalScale != 1.0f && P.VerticalScale > 0.0f)
                                   ? (WorldZ / P.VerticalScale) : WorldZ;

            // Fréquence en Z à 0.15× celle de XY ⇒ les motifs s'étirent horizontalement.
            const float OverhangNoise = HFractal3D(FVector3f(
                WorldX * LP.OverhangFrequency + VoxelHash::SeedOffset(SeedU, 53.1f),
                WorldY * LP.OverhangFrequency + VoxelHash::SeedOffset(SeedU, 59.3f),
                EffectiveZ * LP.OverhangFrequency * 0.15f + VoxelHash::SeedOffset(SeedU, 61.7f)
            ), VoxelGenLOD::Eff(2)) * VOXEL_NOISE_SCALE;

            if (OverhangNoise > 0.0f)
            {
                float Fade = 1.0f - (DistFromSurface / OverhangRange);
                Fade = Fade * Fade;

                InOut.Density += OverhangNoise * LP.OverhangDepth * LP.OverhangStrength * Fade;
            }
        }

        /** Lobe positif seulement ⇒ n'AJOUTE que du solide ⇒ `FillOnly`. */
        EVoxelOpEffect EffectOverBox(const FBox&, const FVoxelOpContext&) const override
        {
            return (P.OverhangStrength > 0.0f && P.OverhangDepth > 0.0f)
                 ? EVoxelOpEffect::FillOnly : EVoxelOpEffect::Identity;
        }

        EVoxelOpEffect EffectOverBox(const FBox& VoxelBox, const FVoxelOpContext& Ctx,
                                     const FVoxelBoxHypotheses& H) const override
        {
            return VF_CaveDetailEffect(Rooms, H, P.SDFBlendRadius,
                                        EffectOverBox(VoxelBox, Ctx));
        }

        /** fBM ∈ [-1,1] × `VOXEL_NOISE_SCALE`, lobe positif seulement, `Fade ∈ [0,1]` ⇒ ajout
         *  ≤ `SCALE · Depth · Strength`. Même réserve « params de strate ». */
        float MaxCarveOverBox(const FBox&, const FVoxelOpContext&) const override { return 0.0f; }
        float MaxFillOverBox(const FBox&, const FVoxelOpContext&) const override
        {
            return MaxAmplitude();
        }

        float MaxCarveOverBox(const FBox&, const FVoxelOpContext&,
                              const FVoxelBoxHypotheses&) const override { return 0.0f; }
        float MaxFillOverBox(const FBox&, const FVoxelOpContext&,
                             const FVoxelBoxHypotheses& H) const override
        {
            return VF_CaveDetailMax(Rooms, H, P.SDFBlendRadius, MaxAmplitude());
        }

        float MaxAmplitude() const
        {
            if (!(P.OverhangStrength > 0.0f && P.OverhangDepth > 0.0f)) { return 0.0f; }
            if (!VoxelMath::IsFinite(P.OverhangDepth)
                || !VoxelMath::IsFinite(P.OverhangStrength))
            {
                return FLT_MAX;
            }
            const float Result = VOXEL_NOISE_SCALE * VF_PerlinAbsBound
                 * P.OverhangDepth * P.OverhangStrength;
            return VoxelMath::IsFinite(Result) ? Result : FLT_MAX;
        }

    private:
        FStrateGenerationParams P;
        uint32 SeedU;
        const FRoomGraphSource* Rooms;   // NON possédant
    };

    //=========================================================================
    // RÔLE 3 — MODIFIER : AFFÛTAGE DE FALAISE / CLIFF SHARPENING  (TunnelNetwork, STEP 4c)
    //=========================================================================
    // ⚠️ TRANSCRIT TEL QUEL BIEN QUE LE COMMENTAIRE DE L'ORIGINAL DÉCRIVE AUTRE CHOSE.
    // Il annonce « échantillonner la densité en Z±1 et calculer le gradient vertical » ; le code, lui,
    // n'échantillonne RIEN : il tire un Perlin dont la fréquence en Z est 3× celle de XY et l'appelle
    // `VertGrad`. C'est un PROXY de gradient, pas un gradient — donc l'effet est décorrélé de la
    // pente réelle de la paroi. Le multiplier par `CaveSDF` lui donne quand même le bon SIGNE de part
    // et d'autre de la surface (plus solide côté roche, plus creusé côté air), ce qui suffit à
    // produire des faces plus raides.
    //
    // Corriger l'écart changerait le monde ; le taire le laisserait se faire « corriger » un jour par
    // quelqu'un qui lit le commentaire et pas le code. Noté ici ET dans OPSTACK-PROGRESS.
    //
    // Ported as written, not as commented: the original's comment promises a sampled vertical
    // gradient, the code uses a Z-stretched Perlin as a proxy. Fixing it would change the world.
    class FCaveCliffMod final : public IVoxelDensityOp
    {
    public:
        FCaveCliffMod(const FStrateGenerationParams& InP, int32 Seed, const FRoomGraphSource* InRooms)
            : P(InP), SeedU((uint32)Seed), Rooms(InRooms) {}

        EVoxelOpRole GetRole() const override { return EVoxelOpRole::DetailModifier; }
        EVoxelOpChannelMask ChannelReads() const override
        {
            return VoxelOpChannels::Density | VoxelOpChannels::Sdf;
        }
        EVoxelOpChannelMask ChannelWrites() const override { return VoxelOpChannels::Density; }
        EVoxelOpResourceMask RequiredResources() const override { return VoxelOpResources::RoomGeometry; }
        bool IsAdditive() const override { return true; }
        void PrepareChunk(const FVoxelOpContext&) override {}
        void PrepareBlockSample(const FVoxelOpSample& Sample) const override
        {
            if (Rooms != nullptr) { Rooms->RestoreBlockSample(Sample); }
        }
        const TCHAR* DebugName() const override { return TEXT("CaveCliffMod"); }
        void EvalBlock(const FVoxelOpBlock& Block) const override { VF_EvalBlockByScalar(*this, Block); }

        void Eval(float WorldX, float WorldY, float WorldZ, FVoxelOpSample& InOut) const override
        {
            const float CaveSDF = InOut.Sdf;
            if (!VF_NearCaveSurface(CaveSDF, P.SDFBlendRadius)) { return; }
            if (Rooms == nullptr) { return; }
            const FStrateGenerationParams& LP = Rooms->LocalParams();   // C1 : params PAR SALLE
            if (!(LP.CliffStrength > 0.0f && CaveSDF < FLT_MAX)) { return; }

            const float DistFromSurface = FMath::Abs(CaveSDF);
            const float CliffRange = 8.0f;   // constante en dur dans l'original
            if (!(DistFromSurface < CliffRange)) { return; }

            const float EffectiveZ = (P.VerticalScale != 1.0f && P.VerticalScale > 0.0f)
                                   ? (WorldZ / P.VerticalScale) : WorldZ;

            const float VertGrad = VoxelNoise::Perlin3D(FVector3f(
                WorldX * 0.05f + VoxelHash::SeedOffset(SeedU, 71.3f),
                WorldY * 0.05f + VoxelHash::SeedOffset(SeedU, 73.7f),
                EffectiveZ * 0.15f + VoxelHash::SeedOffset(SeedU, 79.1f)   // 3× plus vite en Z
            )) * VOXEL_NOISE_SCALE;

            const float CliffEffect = VertGrad * CaveSDF * LP.CliffStrength;

            if (FMath::Abs(VertGrad) > 0.3f)
            {
                float Fade = 1.0f - (DistFromSurface / CliffRange);
                Fade = Fade * Fade;
                InOut.Density += CliffEffect * Fade * 3.0f;
            }
        }

        /** `Both` : le signe suit celui de `VertGrad · CaveSDF`, donc les deux directions. */
        EVoxelOpEffect EffectOverBox(const FBox&, const FVoxelOpContext&) const override
        {
            return (P.CliffStrength > 0.0f) ? EVoxelOpEffect::Both : EVoxelOpEffect::Identity;
        }

        EVoxelOpEffect EffectOverBox(const FBox& VoxelBox, const FVoxelOpContext& Ctx,
                                     const FVoxelBoxHypotheses& H) const override
        {
            return VF_CaveDetailEffect(Rooms, H, P.SDFBlendRadius,
                                        EffectOverBox(VoxelBox, Ctx));
        }

        /** `|VertGrad| ≤ SCALE·1.5`, `|CaveSDF| < 8`, `Fade ≤ 1`, final multiplier `3`. */
        float MaxCarveOverBox(const FBox&, const FVoxelOpContext&) const override
        {
            return MaxAmplitude();
        }

        float MaxFillOverBox(const FBox&, const FVoxelOpContext&) const override
        {
            return MaxAmplitude();
        }

        float MaxCarveOverBox(const FBox& VoxelBox, const FVoxelOpContext& Ctx,
                              const FVoxelBoxHypotheses& H) const override
        {
            return VF_CaveDetailMax(Rooms, H, P.SDFBlendRadius,
                                    MaxCarveOverBox(VoxelBox, Ctx));
        }

        float MaxFillOverBox(const FBox& VoxelBox, const FVoxelOpContext& Ctx,
                             const FVoxelBoxHypotheses& H) const override
        {
            return VF_CaveDetailMax(Rooms, H, P.SDFBlendRadius,
                                    MaxFillOverBox(VoxelBox, Ctx));
        }

        float MaxAmplitude() const
        {
            if (!(P.CliffStrength > 0.0f)) { return 0.0f; }
            if (!VoxelMath::IsFinite(P.CliffStrength)) { return FLT_MAX; }
            const float Result = 8.0f * 3.0f * VOXEL_NOISE_SCALE
                               * VF_PerlinAbsBound * P.CliffStrength;
            return VoxelMath::IsFinite(Result) ? Result : FLT_MAX;
        }

    private:
        FStrateGenerationParams P;
        uint32 SeedU;
        const FRoomGraphSource* Rooms;   // NON possédant
    };

    //=========================================================================
    // RÔLE 3 — MODIFIER : FESTONS / SCALLOP  (TunnelNetwork, STEP 4c)
    //=========================================================================
    // Cuvettes concaves d'érosion hydraulique. Bruit cellulaire : la valeur est haute AU CENTRE
    // d'une cellule (loin des points-features), donc on y creuse — d'où des rangées de coupelles
    // lisses, la signature des grottes calcaires.
    //
    // Deuxième client de `VoxelNoise::Cellular3D` (le premier est la rugosité en mode Cellular) —
    // c'est-à-dire la deuxième raison pour laquelle ce corps devait être PARTAGÉ et non recopié.
    class FScallopMod final : public IVoxelDensityOp
    {
    public:
        FScallopMod(const FStrateGenerationParams& InP, int32 Seed, const FRoomGraphSource* InRooms)
            : P(InP), SeedU((uint32)Seed), Rooms(InRooms) {}

        EVoxelOpRole GetRole() const override { return EVoxelOpRole::DetailModifier; }
        EVoxelOpChannelMask ChannelReads() const override
        {
            return VoxelOpChannels::Density | VoxelOpChannels::Sdf;
        }
        EVoxelOpChannelMask ChannelWrites() const override { return VoxelOpChannels::Density; }
        EVoxelOpResourceMask RequiredResources() const override { return VoxelOpResources::RoomGeometry; }
        bool IsAdditive() const override { return true; }
        void PrepareChunk(const FVoxelOpContext&) override {}
        void PrepareBlockSample(const FVoxelOpSample& Sample) const override
        {
            if (Rooms != nullptr) { Rooms->RestoreBlockSample(Sample); }
        }
        const TCHAR* DebugName() const override { return TEXT("ScallopMod"); }
        void EvalBlock(const FVoxelOpBlock& Block) const override { VF_EvalBlockByScalar(*this, Block); }

        void Eval(float WorldX, float WorldY, float WorldZ, FVoxelOpSample& InOut) const override
        {
            const float CaveSDF = InOut.Sdf;
            if (!VF_NearCaveSurface(CaveSDF, P.SDFBlendRadius)) { return; }
            if (Rooms == nullptr) { return; }
            const FStrateGenerationParams& LP = Rooms->LocalParams();   // C1 : params PAR SALLE
            if (!(LP.ScallopStrength > 0.0f && CaveSDF < FLT_MAX)) { return; }

            const float DistFromSurface = FMath::Abs(CaveSDF);
            const float ScallopRange = LP.ScallopStrength * 4.0f;
            if (!(DistFromSurface < ScallopRange)) { return; }

            const float EffectiveZ = (P.VerticalScale != 1.0f && P.VerticalScale > 0.0f)
                                   ? (WorldZ / P.VerticalScale) : WorldZ;

            const float SF = LP.ScallopFrequency;
            const float ScallopNoise = VoxelNoise::Cellular3D(FVector3f(
                WorldX * SF + VoxelHash::SeedOffset(SeedU, 83.1f),
                WorldY * SF + VoxelHash::SeedOffset(SeedU, 89.3f),
                EffectiveZ * SF + VoxelHash::SeedOffset(SeedU, 97.7f)
            ));

            if (ScallopNoise > 0.0f)
            {
                float Fade = 1.0f - (DistFromSurface / ScallopRange);
                Fade = Fade * Fade;

                InOut.Density -= ScallopNoise * LP.ScallopStrength * Fade;
            }
        }

        /** Lobe positif seulement, SOUSTRAIT ⇒ `CarveOnly`. */
        EVoxelOpEffect EffectOverBox(const FBox&, const FVoxelOpContext&) const override
        {
            return (P.ScallopStrength > 0.0f) ? EVoxelOpEffect::CarveOnly : EVoxelOpEffect::Identity;
        }

        EVoxelOpEffect EffectOverBox(const FBox& VoxelBox, const FVoxelOpContext& Ctx,
                                     const FVoxelBoxHypotheses& H) const override
        {
            return VF_CaveDetailEffect(Rooms, H, P.SDFBlendRadius,
                                        EffectOverBox(VoxelBox, Ctx));
        }

        /** The implementation maps an unclamped F2-F1 distance to `2*d-1`; the proven finite
         *  envelope used here is `VF_CellularAbsBound`, not the approximate [-1,1] comment beside
         *  the noise function.  The positive lobe and `Fade ∈ [0,1]` then bound the carve. */
        float MaxCarveOverBox(const FBox&, const FVoxelOpContext&) const override
        {
            return MaxAmplitude();
        }
        float MaxFillOverBox(const FBox&, const FVoxelOpContext&) const override { return 0.0f; }

        float MaxCarveOverBox(const FBox&, const FVoxelOpContext&,
                              const FVoxelBoxHypotheses& H) const override
        {
            return VF_CaveDetailMax(Rooms, H, P.SDFBlendRadius, MaxAmplitude());
        }
        float MaxFillOverBox(const FBox&, const FVoxelOpContext&,
                             const FVoxelBoxHypotheses&) const override { return 0.0f; }

        float MaxAmplitude() const
        {
            if (!(P.ScallopStrength > 0.0f)) { return 0.0f; }
            if (!VoxelMath::IsFinite(P.ScallopStrength)) { return FLT_MAX; }
            const float Result = VF_CellularAbsBound * P.ScallopStrength;
            return VoxelMath::IsFinite(Result) ? Result : FLT_MAX;
        }

    private:
        FStrateGenerationParams P;
        uint32 SeedU;
        const FRoomGraphSource* Rooms;   // NON possédant
    };

    //=========================================================================
    // RÔLE 3 — MODIFIER : ARCHES / PONTS DE ROCHE  (TunnelNetwork, STEP 4c)
    //=========================================================================
    // ⚠️ PREMIER MODIFICATEUR RELATIF À LA SALLE. Il ne lit pas seulement `InOut.Sdf` : il lui faut
    // LA SALLE — son hash (pour tirer les arches de façon déterministe), son centre et ses rayons.
    // C'est l'unique consommateur de `NearestRoomIdx` avec les dômes, le pincement et le biais de
    // sol, et c'est ce qui rend `FRoomGraphSource::GetNearestRoomIdx()` nécessaire.
    //
    // ⚠️ SA PORTE N'EST PAS LE GATE COMMUN : `CaveSDF < SDFBlendRadius` (dans la grotte ou tout
    // près), pas `< SDFBlendRadius·3`. Une arche se pose dans le VIDE de la salle, pas dans sa paroi.
    class FCaveArchMod final : public IVoxelDensityOp
    {
    public:
        FCaveArchMod(const FStrateGenerationParams& InP, const FRoomGraphSource* InRooms)
            : P(InP), Rooms(InRooms) {}

        EVoxelOpRole GetRole() const override { return EVoxelOpRole::DetailModifier; }
        EVoxelOpChannelMask ChannelReads() const override
        {
            return VoxelOpChannels::Density | VoxelOpChannels::Sdf;
        }
        EVoxelOpChannelMask ChannelWrites() const override { return VoxelOpChannels::Density; }
        EVoxelOpResourceMask RequiredResources() const override { return VoxelOpResources::RoomGeometry; }
        bool IsAdditive() const override { return true; }
        void PrepareChunk(const FVoxelOpContext&) override {}
        void PrepareBlockSample(const FVoxelOpSample& Sample) const override
        {
            if (Rooms != nullptr) { Rooms->RestoreBlockSample(Sample); }
        }
        const TCHAR* DebugName() const override { return TEXT("CaveArchMod"); }
        void EvalBlock(const FVoxelOpBlock& Block) const override { VF_EvalBlockByScalar(*this, Block); }

        void Eval(float WorldX, float WorldY, float WorldZ, FVoxelOpSample& InOut) const override
        {
            const float CaveSDF = InOut.Sdf;
            if (!VF_NearCaveSurface(CaveSDF, P.SDFBlendRadius)) { return; }
            if (Rooms == nullptr) { return; }

            const int32 NearestRoomIdx = Rooms->GetNearestRoomIdx();
            const FStrateGenerationParams& LP = Rooms->LocalParams();   // C1 : params PAR SALLE
            if (!(LP.ArchDensity > 0.0f && CaveSDF < LP.SDFBlendRadius && CaveSDF < FLT_MAX
                  && NearestRoomIdx >= 0))
            {
                return;
            }

            // ⚠️ `IsValidIndex` AJOUTÉ : l'original indexe directement après le seul test `>= 0`.
            // L'index vient d'`EvaluateSDFCached` donc il est valide par construction — ce garde-fou
            // ne peut donc JAMAIS changer la sortie, seulement empêcher un crash si l'invariant se
            // cassait un jour. Même famille de décision que l'empreinte de params dans la clé de
            // cache : on se trompe du côté du coût, jamais du côté du résultat.
            const FChunkSDFCache& Cache = Rooms->GetCache();
            if (!Cache.Rooms.IsValidIndex(NearestRoomIdx)) { return; }
            const FCachedRoom& Room = Cache.Rooms[NearestRoomIdx];

            const FVector VoxPos(WorldX, WorldY, WorldZ);

            for (int32 i = 0; i < 3; i++)
            {
                const FCachedArch& Arch = Room.Arches[i];
                if (!Arch.bActive) { continue; }
                const float ArchSDF = VoxelSDF::Capsule(
                    VoxPos, Arch.EndpointA, Arch.EndpointB, Arch.Radius);

                const float ArchBlend = 2.0f;
                if (ArchSDF < ArchBlend)
                {
                    float Fill = FMath::Clamp((ArchBlend - ArchSDF) / (ArchBlend * 2.0f), 0.0f, 1.0f);
                    Fill = SmoothStep01(Fill);
                    InOut.Density += Fill * Arch.BaseDensity * 1.5f;
                }
            }
        }

        /** N'AJOUTE que du solide ⇒ `FillOnly`. La décision spatiale est indépendante : elle
         *  consomme l'intervalle SDF publié par la source, comme les autres détails de salle. */
        EVoxelOpEffect EffectOverBox(const FBox&, const FVoxelOpContext&) const override
        {
            if (!(P.ArchDensity > 0.0f)) { return EVoxelOpEffect::Identity; }
            if (P.BaseDensity > 0.0f) { return EVoxelOpEffect::FillOnly; }
            if (P.BaseDensity < 0.0f) { return EVoxelOpEffect::CarveOnly; }
            return EVoxelOpEffect::Identity;
        }

        EVoxelOpEffect EffectOverBox(const FBox& VoxelBox, const FVoxelOpContext& Ctx,
                                     const FVoxelBoxHypotheses& H) const override
        {
            return VF_CaveDetailEffect(Rooms, H, P.SDFBlendRadius,
                                        EffectOverBox(VoxelBox, Ctx));
        }

        float MaxCarveOverBox(const FBox&, const FVoxelOpContext&) const override
        {
            return P.BaseDensity < 0.0f ? MaxAmplitude() : 0.0f;
        }

        float MaxFillOverBox(const FBox&, const FVoxelOpContext&) const override
        {
            return P.BaseDensity > 0.0f ? MaxAmplitude() : 0.0f;
        }

        float MaxCarveOverBox(const FBox& VoxelBox, const FVoxelOpContext& Ctx,
                              const FVoxelBoxHypotheses& H) const override
        {
            return VF_CaveDetailMax(Rooms, H, P.SDFBlendRadius,
                                    MaxCarveOverBox(VoxelBox, Ctx));
        }

        float MaxFillOverBox(const FBox& VoxelBox, const FVoxelOpContext& Ctx,
                             const FVoxelBoxHypotheses& H) const override
        {
            return VF_CaveDetailMax(Rooms, H, P.SDFBlendRadius,
                                    MaxFillOverBox(VoxelBox, Ctx));
        }

        float MaxAmplitude() const
        {
            if (!(P.ArchDensity > 0.0f)) { return 0.0f; }
            if (!VoxelMath::IsFinite(P.BaseDensity)) { return FLT_MAX; }
            const float Result = FMath::Abs(P.BaseDensity) * 3.0f * 1.5f;
            return VoxelMath::IsFinite(Result) ? Result : FLT_MAX;
        }

    private:
        FStrateGenerationParams P;
        const FRoomGraphSource* Rooms;   // NON possédant
    };

    //=========================================================================
    // RÔLE 3 — MODIFIER : COLONNES DE SALLE / ROOM COLUMNS  (TunnelNetwork, STEP 4d)
    //=========================================================================
    // ⚠️ CE N'EST PAS `FGridColumnMod` (FlatPlain / CrystalChamber). Celui-là pose des cylindres sur
    // une GRILLE MONDE et se tire par cellule ; celui-ci PARCOURT une liste PRÉ-CUITE par
    // `BuildChunkCache`, salle par salle. `OPSTACK-DECOMPOSITION §1` les sépare explicitement.
    //
    // ⚠️⚠️ IL N'A **AUCUN** PARAMÈTRE DE STRATE, ET C'EST LE PIÈGE DE CE GROUPE.
    // Le code d'origine n'écrit aucun `if (Params.ColumnDensity > 0)` : il itère la liste cuite,
    // point. `FStrateGenerationParams::ColumnDensity` n'est JAMAIS lu par la cuisson non plus (elle
    // lit `OpParams`, un struct NEUF où seul l'op de la salle a été appliqué). Donc :
    //   • mettre `ColumnDensity = 0` dans les params N'ÉTEINT PAS les colonnes ;
    //   • la seule façon d'avoir des colonnes est un `UVoxelTerrainOpDefinition` de type `Column`
    //     dans le pool de la strate ;
    //   • et la seule façon de PROUVER qu'elles ont tiré est de regarder `SDFCache.Columns.Num()`.
    // C'est exactement la leçon des pits, une troisième fois. Le test l'applique au contrôle 3b.
    class FRoomColumnMod final : public IVoxelDensityOp
    {
    public:
        FRoomColumnMod(const FStrateGenerationParams& InP, const FRoomGraphSource* InRooms)
            : P(InP), Rooms(InRooms) {}

        EVoxelOpRole GetRole() const override { return EVoxelOpRole::DetailModifier; }
        EVoxelOpChannelMask ChannelReads() const override
        {
            return VoxelOpChannels::Density | VoxelOpChannels::Sdf;
        }
        EVoxelOpChannelMask ChannelWrites() const override { return VoxelOpChannels::Density; }
        EVoxelOpResourceMask RequiredResources() const override { return VoxelOpResources::RoomGeometry; }
        bool IsAdditive() const override { return true; }
        void PrepareChunk(const FVoxelOpContext&) override {}
        void PrepareBlockSample(const FVoxelOpSample& Sample) const override
        {
            if (Rooms != nullptr) { Rooms->RestoreBlockSample(Sample); }
        }
        const TCHAR* DebugName() const override { return TEXT("RoomColumnMod"); }
        void EvalBlock(const FVoxelOpBlock& Block) const override { VF_EvalBlockByScalar(*this, Block); }

        void Eval(float WorldX, float WorldY, float, FVoxelOpSample& InOut) const override
        {
            if (!VF_NearCaveSurface(InOut.Sdf, P.SDFBlendRadius)) { return; }
            if (Rooms == nullptr) { return; }

            auto EvaluateColumn = [&](int32 ColumnIndex)
            {
                const FCachedColumn& Col = Rooms->GetCache().Columns[ColumnIndex];
                const float DX = WorldX - Col.CenterX;
                const float DY = WorldY - Col.CenterY;
                const float XYDistSq = DX * DX + DY * DY;
                if (XYDistSq > Col.BoundXYRadiusSq) { return; }

                const float CylSDF = FMath::Sqrt(XYDistSq) - Col.Radius;

                const float ColBlend = 3.0f;
                if (CylSDF < ColBlend)
                {
                    float Fill = FMath::Clamp((ColBlend - CylSDF) / (ColBlend * 2.0f), 0.0f, 1.0f);
                    Fill = SmoothStep01(Fill);
                    InOut.Density += Fill * Col.BaseDensity * 1.5f;
                }
            };
            VF_ForEachChunkSDFSpatialCandidate(
                Rooms->GetCache().ColumnSpatialIndex,
                Rooms->GetCache().Columns.Num(), WorldX, WorldY, EvaluateColumn,
                VoxelGenLOD::ShouldUseSpatialIndex(false));
        }

        /** The cached column list is the complete source of this op's work.  A valid empty list is
         *  therefore an exact Identity result; an invalid box cache stays conservative. */
        EVoxelOpEffect EffectOverBox(const FBox&, const FVoxelOpContext&) const override
        {
            return EVoxelOpEffect::FillOnly;
        }

        EVoxelOpEffect EffectOverBox(const FBox& VoxelBox, const FVoxelOpContext& Ctx,
                                     const FVoxelBoxHypotheses& H) const override
        {
            if (Rooms == nullptr || VF_CaveBoxIsFar(H, P.SDFBlendRadius))
            {
                return EVoxelOpEffect::Identity;
            }
            const float Bound = Rooms->RoomColumnFillSupremumForLastBox();
            if (!(Bound > 0.0f)) { return EVoxelOpEffect::Identity; }
            return VoxelMath::IsFinite(Bound) ? EVoxelOpEffect::FillOnly : EVoxelOpEffect::Both;
        }

        float MaxCarveOverBox(const FBox&, const FVoxelOpContext&) const override { return 0.0f; }

        float MaxFillOverBox(const FBox&, const FVoxelOpContext&) const override
        {
            return FLT_MAX;
        }

        float MaxCarveOverBox(const FBox&, const FVoxelOpContext&,
                              const FVoxelBoxHypotheses& H) const override
        {
            if (Rooms == nullptr || VF_CaveBoxIsFar(H, P.SDFBlendRadius)) { return 0.0f; }
            const float Bound = Rooms->RoomColumnFillSupremumForLastBox();
            return VoxelMath::IsFinite(Bound) ? 0.0f : Bound;
        }

        float MaxFillOverBox(const FBox&, const FVoxelOpContext&,
                             const FVoxelBoxHypotheses& H) const override
        {
            if (Rooms == nullptr || VF_CaveBoxIsFar(H, P.SDFBlendRadius)) { return 0.0f; }
            return Rooms->RoomColumnFillSupremumForLastBox();
        }

    private:
        FStrateGenerationParams P;
        const FRoomGraphSource* Rooms;   // NON possédant
    };

    //=========================================================================
    // RÔLE 3 — MODIFIER : DÔMES / CATHEDRAL CEILINGS  (TunnelNetwork, STEP 4g)
    //=========================================================================
    // Un demi-ellipsoïde creusé VERS LE HAUT depuis un point ancré au-dessus du centre de la salle.
    // Relatif à la salle comme les arches, même porte `CaveSDF < SDFBlendRadius`.
    class FDomeMod final : public IVoxelDensityOp
    {
    public:
        FDomeMod(const FStrateGenerationParams& InP, const FRoomGraphSource* InRooms)
            : P(InP), Rooms(InRooms) {}

        EVoxelOpRole GetRole() const override { return EVoxelOpRole::DetailModifier; }
        EVoxelOpChannelMask ChannelReads() const override
        {
            return VoxelOpChannels::Density | VoxelOpChannels::Sdf;
        }
        EVoxelOpChannelMask ChannelWrites() const override { return VoxelOpChannels::Density; }
        EVoxelOpResourceMask RequiredResources() const override { return VoxelOpResources::RoomGeometry; }
        bool IsAdditive() const override { return true; }
        void PrepareChunk(const FVoxelOpContext&) override {}
        void PrepareBlockSample(const FVoxelOpSample& Sample) const override
        {
            if (Rooms != nullptr) { Rooms->RestoreBlockSample(Sample); }
        }
        const TCHAR* DebugName() const override { return TEXT("DomeMod"); }
        void EvalBlock(const FVoxelOpBlock& Block) const override { VF_EvalBlockByScalar(*this, Block); }

        void Eval(float WorldX, float WorldY, float WorldZ, FVoxelOpSample& InOut) const override
        {
            const float CaveSDF = InOut.Sdf;
            if (!VF_NearCaveSurface(CaveSDF, P.SDFBlendRadius)) { return; }
            if (Rooms == nullptr) { return; }

            const int32 NearestRoomIdx = Rooms->GetNearestRoomIdx();
            const FStrateGenerationParams& LP = Rooms->LocalParams();   // C1 : params PAR SALLE
            if (!(LP.DomeDensity > 0.0f && CaveSDF < LP.SDFBlendRadius && CaveSDF < FLT_MAX
                  && NearestRoomIdx >= 0))
            {
                return;
            }

            const FChunkSDFCache& Cache = Rooms->GetCache();
            if (!Cache.Rooms.IsValidIndex(NearestRoomIdx)) { return; }   // cf. FCaveArchMod
            const FCachedRoom& Room = Cache.Rooms[NearestRoomIdx];

            const int32 MaxDomes = 2;

            for (int32 i = 0; i < MaxDomes; i++)
            {
                const uint32 DH = VoxelHash::Mix(Room.Hash ^ (0xD0AE0u + (uint32)i * 8191u));

                if (VoxelHash::ToFloat01(DH) > LP.DomeDensity) { continue; }

                const uint32 DH2 = VoxelHash::Mix(DH ^ 0xD0A0u);
                const float DmX = Room.Center.X + VoxelHash::ToFloatSigned(DH2) * Room.RadiusXY * 0.4f;
                const float DmY = Room.Center.Y
                                + VoxelHash::ToFloatSigned(VoxelHash::Mix(DH2)) * Room.RadiusXY * 0.4f;

                const uint32 DH3 = VoxelHash::Mix(DH2 ^ 0x90DEu);
                const float DmRadius = FMath::Min(
                    FMath::Lerp(LP.DomeMinRadius, LP.DomeMaxRadius, VoxelHash::ToFloat01(DH3)),
                    Room.RadiusXY * 0.85f
                );

                const uint32 DH4 = VoxelHash::Mix(DH3 ^ 0xCAFEu);
                const float DmCenterZ = Room.Center.Z + Room.RadiusZ * 0.2f
                                      + VoxelHash::ToFloat01(DH4) * Room.RadiusZ * 0.3f;

                const float DmHeight = DmRadius * LP.DomeHeightRatio;

                if (WorldZ > DmCenterZ + DmHeight + 3.0f || WorldZ < DmCenterZ - 3.0f) { continue; }

                const float DXDm = WorldX - DmX;
                const float DYDm = WorldY - DmY;
                const float DZDm = WorldZ - DmCenterZ;

                if (DZDm < 0.0f) { continue; }   // ne creuse que vers le haut

                const float NormX = DXDm / DmRadius;
                const float NormY = DYDm / DmRadius;
                const float NormZ = DZDm / DmHeight;
                const float EllipDist = FMath::Sqrt(NormX * NormX + NormY * NormY + NormZ * NormZ) - 1.0f;
                const float DomeSDF = EllipDist * FMath::Min(DmRadius, DmHeight);

                const float DmBlend = 3.0f;
                if (DomeSDF < DmBlend)
                {
                    float Carve = FMath::Clamp((DmBlend - DomeSDF) / (DmBlend * 2.0f), 0.0f, 1.0f);
                    Carve = SmoothStep01(Carve);
                    InOut.Density -= Carve * LP.BaseDensity * 1.5f;
                }
            }
        }

        /** Ne SOUSTRAIT que ⇒ `CarveOnly`. */
        EVoxelOpEffect EffectOverBox(const FBox&, const FVoxelOpContext&) const override
        {
            return (P.DomeDensity > 0.0f) ? EVoxelOpEffect::CarveOnly : EVoxelOpEffect::Identity;
        }

        EVoxelOpEffect EffectOverBox(const FBox& VoxelBox, const FVoxelOpContext& Ctx,
                                     const FVoxelBoxHypotheses& H) const override
        {
            return VF_CaveDetailEffect(Rooms, H, P.SDFBlendRadius,
                                        EffectOverBox(VoxelBox, Ctx));
        }

        float MaxCarveOverBox(const FBox&, const FVoxelOpContext&) const override
        {
            return MaxAmplitude();
        }

        float MaxFillOverBox(const FBox&, const FVoxelOpContext&) const override
        {
            return 0.0f;
        }

        float MaxCarveOverBox(const FBox& VoxelBox, const FVoxelOpContext& Ctx,
                              const FVoxelBoxHypotheses& H) const override
        {
            return VF_CaveDetailMax(Rooms, H, P.SDFBlendRadius,
                                    MaxCarveOverBox(VoxelBox, Ctx));
        }

        float MaxFillOverBox(const FBox&, const FVoxelOpContext&,
                             const FVoxelBoxHypotheses&) const override
        {
            return 0.0f;
        }

        float MaxAmplitude() const
        {
            if (!(P.DomeDensity > 0.0f)) { return 0.0f; }
            if (!VoxelMath::IsFinite(P.BaseDensity)) { return FLT_MAX; }
            const float Result = FMath::Abs(P.BaseDensity) * 2.0f * 1.5f;
            return VoxelMath::IsFinite(Result) ? Result : FLT_MAX;
        }

    private:
        FStrateGenerationParams P;
        const FRoomGraphSource* Rooms;   // NON possédant
    };

    //=========================================================================
    // RÔLE 3 — MODIFIER : PINCEMENT / BOTTLENECK  (TunnelNetwork, STEP 4h)
    //=========================================================================
    // Resserre un passage PAR LES CÔTÉS. Placé sur le PÉRIMÈTRE de la salle (offset 0.85 · rayon) —
    // là où les tunnels débouchent — et pas au centre, sinon il boucherait la salle elle-même.
    // `SideFactor` (distance à l'axe, clampée) est ce qui laisse l'axe du passage libre : le
    // remplissage est nul sur l'axe et maximal sur les bords de l'ellipsoïde.
    class FPinchMod final : public IVoxelDensityOp
    {
    public:
        FPinchMod(const FStrateGenerationParams& InP, const FRoomGraphSource* InRooms)
            : P(InP), Rooms(InRooms) {}

        EVoxelOpRole GetRole() const override { return EVoxelOpRole::DetailModifier; }
        EVoxelOpChannelMask ChannelReads() const override
        {
            return VoxelOpChannels::Density | VoxelOpChannels::Sdf;
        }
        EVoxelOpChannelMask ChannelWrites() const override { return VoxelOpChannels::Density; }
        EVoxelOpResourceMask RequiredResources() const override { return VoxelOpResources::RoomGeometry; }
        bool IsAdditive() const override { return true; }
        void PrepareChunk(const FVoxelOpContext&) override {}
        void PrepareBlockSample(const FVoxelOpSample& Sample) const override
        {
            if (Rooms != nullptr) { Rooms->RestoreBlockSample(Sample); }
        }
        const TCHAR* DebugName() const override { return TEXT("PinchMod"); }
        void EvalBlock(const FVoxelOpBlock& Block) const override { VF_EvalBlockByScalar(*this, Block); }

        void Eval(float WorldX, float WorldY, float WorldZ, FVoxelOpSample& InOut) const override
        {
            const float CaveSDF = InOut.Sdf;
            if (!VF_NearCaveSurface(CaveSDF, P.SDFBlendRadius)) { return; }
            if (Rooms == nullptr) { return; }

            const int32 NearestRoomIdx = Rooms->GetNearestRoomIdx();
            const FStrateGenerationParams& LP = Rooms->LocalParams();   // C1 : params PAR SALLE
            if (!(LP.PinchDensity > 0.0f && CaveSDF < LP.SDFBlendRadius && CaveSDF < FLT_MAX
                  && NearestRoomIdx >= 0))
            {
                return;
            }

            const FChunkSDFCache& Cache = Rooms->GetCache();
            if (!Cache.Rooms.IsValidIndex(NearestRoomIdx)) { return; }   // cf. FCaveArchMod
            const FCachedRoom& Room = Cache.Rooms[NearestRoomIdx];

            for (int32 i = 0; i < 3; i++)
            {
                const FCachedPinch& Pinch = Room.Pinches[i];
                if (!Pinch.bActive) { continue; }
                const float DXPn = WorldX - Pinch.CenterX;
                const float DYPn = WorldY - Pinch.CenterY;
                const float DZPn = WorldZ - Pinch.CenterZ;

                if (FMath::Abs(DXPn) + FMath::Abs(DYPn) + FMath::Abs(DZPn) > Pinch.MaxExtent) { continue; }

                const float Along  =  DXPn * Pinch.CosAngle + DYPn * Pinch.SinAngle;
                const float Across = -DXPn * Pinch.SinAngle + DYPn * Pinch.CosAngle;

                const float NAlong   = Along   / Pinch.HalfLength;
                const float NAcross  = Across  / Pinch.HalfNarrow;
                const float NUp      = DZPn    / Pinch.HalfVertical;
                const float EllipDist = NAlong * NAlong + NAcross * NAcross + NUp * NUp;

                if (EllipDist < 1.0f)
                {
                    float Fill = 1.0f - EllipDist;
                    Fill = SmoothStep01(Fill);
                    const float AxisDist = FMath::Sqrt(NAcross * NAcross + NUp * NUp);
                    const float SideFactor = FMath::Clamp(AxisDist * 2.0f, 0.0f, 1.0f);
                    InOut.Density += Fill * SideFactor * Pinch.BaseDensity * 1.5f;
                }
            }
        }

        /** N'AJOUTE que du solide ⇒ `FillOnly`. */
        EVoxelOpEffect EffectOverBox(const FBox&, const FVoxelOpContext&) const override
        {
            return (P.PinchDensity > 0.0f) ? EVoxelOpEffect::FillOnly : EVoxelOpEffect::Identity;
        }

        EVoxelOpEffect EffectOverBox(const FBox& VoxelBox, const FVoxelOpContext& Ctx,
                                     const FVoxelBoxHypotheses& H) const override
        {
            return VF_CaveDetailEffect(Rooms, H, P.SDFBlendRadius,
                                        EffectOverBox(VoxelBox, Ctx));
        }

        float MaxCarveOverBox(const FBox&, const FVoxelOpContext&) const override { return 0.0f; }

        float MaxFillOverBox(const FBox&, const FVoxelOpContext&) const override
        {
            return MaxAmplitude();
        }

        float MaxCarveOverBox(const FBox&, const FVoxelOpContext&,
                              const FVoxelBoxHypotheses& H) const override
        {
            return VF_CaveDetailMax(Rooms, H, P.SDFBlendRadius, 0.0f);
        }

        float MaxFillOverBox(const FBox& VoxelBox, const FVoxelOpContext& Ctx,
                             const FVoxelBoxHypotheses& H) const override
        {
            return VF_CaveDetailMax(Rooms, H, P.SDFBlendRadius,
                                    MaxFillOverBox(VoxelBox, Ctx));
        }

        float MaxAmplitude() const
        {
            if (!(P.PinchDensity > 0.0f)) { return 0.0f; }
            if (!VoxelMath::IsFinite(P.BaseDensity)) { return FLT_MAX; }
            const float Result = FMath::Abs(P.BaseDensity) * 3.0f * 1.5f;
            return VoxelMath::IsFinite(Result) ? Result : FLT_MAX;
        }

    private:
        FStrateGenerationParams P;
        const FRoomGraphSource* Rooms;   // NON possédant
    };

    //=========================================================================
    // RÔLE 3 — MODIFIER : BIAIS DE SOL / FLOOR BIAS  (TunnelNetwork, fin de 4h)
    //=========================================================================
    // Rend de la densité dans la moitié BASSE de la salle pour contrer le relief que la rugosité
    // laisse sur les sols — un sol praticable au lieu d'un sol bosselé. Ne s'applique QUE dans l'air
    // certain (`CaveSDF < 0`) : dans la paroi, le clamp anti-remplissage de la rugosité tient déjà.
    //
    // ⚠️ DERNIER DE LA CHAÎNE, ET CE N'EST PAS INTERCHANGEABLE : il ajoute un biais indépendant à
    // la densité déjà accumulée par les étapes précédentes. Il ne lit ni ne soustrait la variation
    // de rugosité (4b). Le déplacer avant les autres étapes changerait donc leur entrée et leur
    // sortie, ce qui est la raison pour laquelle l'ordre de `BuildTunnelNetworkStack` reste celui
    // de l'original, ligne pour ligne.
    //
    // This op is intentionally separate from 4b. Ten operators lie
    // between 4b and this phase (4c through 4h), and every one reads+writes Density. Moving this
    // phase next to 4b would reorder those density updates, so a bit-identical fuse is not legal.
    class FFloorBiasMod final : public IVoxelDensityOp
    {
    public:
        FFloorBiasMod(const FStrateGenerationParams& InP, const FRoomGraphSource* InRooms)
            : P(InP), Rooms(InRooms) {}

        EVoxelOpRole GetRole() const override { return EVoxelOpRole::DetailModifier; }
        EVoxelOpChannelMask ChannelReads() const override
        {
            return VoxelOpChannels::Density | VoxelOpChannels::Sdf;
        }
        EVoxelOpChannelMask ChannelWrites() const override { return VoxelOpChannels::Density; }
        EVoxelOpResourceMask RequiredResources() const override { return VoxelOpResources::RoomGeometry; }
        bool IsAdditive() const override { return true; }
        void PrepareChunk(const FVoxelOpContext&) override {}
        void PrepareBlockSample(const FVoxelOpSample& Sample) const override
        {
            if (Rooms != nullptr) { Rooms->RestoreBlockSample(Sample); }
        }
        const TCHAR* DebugName() const override { return TEXT("FloorBiasMod"); }
        void EvalBlock(const FVoxelOpBlock& Block) const override { VF_EvalBlockByScalar(*this, Block); }

        void Eval(float, float, float WorldZ, FVoxelOpSample& InOut) const override
        {
            const float CaveSDF = InOut.Sdf;
            if (!VF_NearCaveSurface(CaveSDF, P.SDFBlendRadius)) { return; }
            if (Rooms == nullptr) { return; }

            const int32 NearestRoomIdx = Rooms->GetNearestRoomIdx();
            const FStrateGenerationParams& LP = Rooms->LocalParams();   // C1 : params PAR SALLE
            if (!(LP.FloorBias > 0.0f && NearestRoomIdx >= 0 && CaveSDF < 0.0f)) { return; }

            const FChunkSDFCache& Cache = Rooms->GetCache();
            if (!Cache.Rooms.IsValidIndex(NearestRoomIdx)) { return; }   // cf. FCaveArchMod
            const FCachedRoom& NR = Cache.Rooms[NearestRoomIdx];

            // NormZ : -1 = sol de la salle, 0 = centre, +1 = plafond.
            const float NormZ = (WorldZ - NR.Center.Z) / FMath::Max(NR.RadiusZ, 1.0f);

            if (NormZ < 0.0f)
            {
                const float FloorFactor = NormZ * NormZ;   // 0 au centre, 1 au sol
                InOut.Density += FloorFactor * LP.FloorBias;
            }
        }

        /** N'AJOUTE que du solide ⇒ `FillOnly`. */
        EVoxelOpEffect EffectOverBox(const FBox&, const FVoxelOpContext&) const override
        {
            return (P.FloorBias > 0.0f) ? EVoxelOpEffect::FillOnly : EVoxelOpEffect::Identity;
        }

        EVoxelOpEffect EffectOverBox(const FBox& VoxelBox, const FVoxelOpContext& Ctx,
                                     const FVoxelBoxHypotheses& H) const override
        {
            return VF_CaveDetailEffect(Rooms, H, P.SDFBlendRadius,
                                        EffectOverBox(VoxelBox, Ctx));
        }

        float MaxCarveOverBox(const FBox&, const FVoxelOpContext&) const override { return 0.0f; }

        float MaxFillOverBox(const FBox&, const FVoxelOpContext&) const override
        {
            return VoxelMath::IsFinite(P.FloorBias) && P.FloorBias >= 0.0f
                 ? P.FloorBias : (P.FloorBias < 0.0f ? 0.0f : FLT_MAX);
        }

        float MaxCarveOverBox(const FBox&, const FVoxelOpContext&,
                              const FVoxelBoxHypotheses& H) const override
        {
            return VF_CaveBoxIsFar(H, P.SDFBlendRadius) ? 0.0f : 0.0f;
        }

        float MaxFillOverBox(const FBox&, const FVoxelOpContext&,
                             const FVoxelBoxHypotheses& H) const override
        {
            if (Rooms == nullptr || VF_CaveBoxIsFar(H, P.SDFBlendRadius)) { return 0.0f; }
            return Rooms->FloorBiasFillSupremumForLastBox();
        }

    private:
        FStrateGenerationParams P;
        const FRoomGraphSource* Rooms;   // NON possédant
    };

    //=========================================================================
    // RÔLE 1 — SOURCE : VERS / WORM TUNNELS  (TunnelNetwork)
    //=========================================================================
    // Un carve par SEUIL sur du bruit 3D, masqué par la distance au réseau de salles. Il écrit la
    // DENSITÉ directement (pas le canal SDF) : c'est une source « fieldée », pas une primitive
    // placée — la distinction que `AUDIT §6.2` pose et que `OPSTACK-DECOMPOSITION §0.2` chiffre.
    //
    // ⚠️ IL LIT `InOut.Sdf` : le masque de réseau est une fonction de `CaveSDF` APRÈS pits et
    // cheminées. C'est encore le canal SDF utilisé comme ce pour quoi il existe — transporter une
    // information géométrique entre deux opérateurs au lieu de la recalculer.
    class FWormFieldSource final : public IVoxelDensityOp
    {
    public:
        FWormFieldSource(const FStrateGenerationParams& InP, int32 Seed)
            : P(InP)
            , SeedU((uint32)Seed)
        {}

        EVoxelOpRole GetRole() const override { return EVoxelOpRole::FieldSource; }
        EVoxelOpChannelMask ChannelReads() const override
        {
            return VoxelOpChannels::Density | VoxelOpChannels::Sdf;
        }
        EVoxelOpChannelMask ChannelWrites() const override { return VoxelOpChannels::Density; }
        bool IsAdditive() const override { return true; }
        void PrepareChunk(const FVoxelOpContext&) override {}

        void Eval(float WorldX, float WorldY, float WorldZ, FVoxelOpSample& InOut) const override
        {
            if (!(P.WormStrength > 0.0f && P.WormThreshold > 0.0f)) { return; }

            const float CaveSDF = InOut.Sdf;

            float NetworkMask = 1.0f;
            if (P.WormNetworkRange > 0.0f)
            {
                if (CaveSDF >= P.WormNetworkRange)   // vrai aussi quand il n'y a pas de réseau (FLT_MAX)
                {
                    NetworkMask = 0.0f;
                }
                else if (CaveSDF > 0.0f)
                {
                    NetworkMask = 1.0f - SmoothStep01(CaveSDF / P.WormNetworkRange);
                }
            }

            if (NetworkMask <= 0.0f) { return; }

            const VoxelWormField::FParameters WormParameters{
                P.WormFrequency,
                P.WormHorizontalBias,
                P.VerticalScale,
                SeedU};
            const float WormValue = VoxelWormField::Evaluate(
                WorldX, WorldY, WorldZ, P.WormThreshold,
                WormParameters);
            if (WormValue < P.WormThreshold)
            {
                const float t = 1.0f - (WormValue / P.WormThreshold);
                InOut.Density -= t * P.WormStrength * NetworkMask;
            }
        }

        /**
         * The worm is a density consumer, not an SDF source. Its intrinsic response is `CarveOnly`.
         * The state-aware fold may prove it inactive only when the interval already published by a
         * preceding SDF writer proves `Sdf >= WormNetworkRange` throughout the box. An unknown or
         * unrelated interval leaves the carve active, which is conservative for free composition.
         * Its magnitude is independently bounded by `WormStrength` because both `t` and
         * `NetworkMask` are in [0, 1].
         */
        EVoxelOpEffect EffectOverBox(const FBox&, const FVoxelOpContext&) const override
        {
            if (!(P.WormStrength > 0.0f && P.WormThreshold > 0.0f)) { return EVoxelOpEffect::Identity; }
            return EVoxelOpEffect::CarveOnly;
        }

        EVoxelOpEffect EffectOverBox(const FBox& VoxelBox, const FVoxelOpContext& Ctx,
                                     const FVoxelBoxHypotheses& H) const override
        {
            const EVoxelOpEffect Intrinsic = EffectOverBox(VoxelBox, Ctx);
            if (Intrinsic == EVoxelOpEffect::Identity) { return Intrinsic; }
            if (P.WormNetworkRange > 0.0f && H.Sdf.IsKnown()
                && H.Sdf.Min >= P.WormNetworkRange)
            {
                return EVoxelOpEffect::Identity;
            }
            return Intrinsic;
        }

        /**
         * ✅ **LA BORNE DE `§0.2`, MAINTENANT CONSOMMÉE.** Elle a passé plusieurs entrées de journal
         * écrite mais inutilisée, faute d'un pliage capable de porter un nombre ; ce pliage existe.
         *
         * La preuve tient en une ligne : `t = 1 − WormValue/WormThreshold ∈ [0,1]` (le bloc ne
         * s'exécute que sous le seuil) et `NetworkMask ∈ [0,1]` par construction, donc
         * `t · WormStrength · NetworkMask ≤ WormStrength`. C'est une borne PROUVÉE, pas prudente —
         * la seule sorte qui ait le droit d'être ici : sur-estimer coûte du CPU, sous-estimer fait
         * un trou.
         */
        float MaxCarveOverBox(const FBox&, const FVoxelOpContext&) const override
        {
            return MaxCarveAmplitude();
        }

        float MaxCarveOverBox(const FBox& VoxelBox, const FVoxelOpContext& Ctx,
                              const FVoxelBoxHypotheses& H) const override
        {
            const float Strength = MaxCarveOverBox(VoxelBox, Ctx);
            if (!(Strength > 0.0f) || !VoxelMath::IsFinite(Strength))
            {
                return Strength;
            }
            if (!(P.WormNetworkRange > 0.0f) || !VoxelMath::IsFinite(P.WormNetworkRange)
                || !H.Sdf.IsKnown() || !VoxelMath::IsFinite(H.Sdf.Min))
            {
                return Strength;
            }

            // The classifier's lattice proof is about the exact MC vertices, not the continuum
            // between them.  On a refined child, evaluate the worm's two threshold noises at
            // those vertices once and bound the actual carve factor there.  The root is left on
            // the cheap interval path; a refined step-32 child has at most 17^3 vertices and the
            // explicit count cap keeps this check bounded.  The network mask still uses the SDF
            // interval's worst case, so this never assumes a room value that the preceding source
            // did not prove.
            if (Ctx.bUseLatticeProof && Ctx.bTightenWarpProof && Ctx.Step >= 1
                && VoxelMath::IsFinite(P.WormFrequency)
                && VoxelMath::IsFinite(P.WormHorizontalBias)
                && VoxelMath::IsFinite(P.WormThreshold)
                && P.WormThreshold > 0.0f
                && VoxelMath::IsFinite(P.VerticalScale))
            {
                const int32 Step = FMath::Max(Ctx.Step, 1);
                const float OriginX = static_cast<float>(Ctx.LatticeOriginVoxels.X);
                const float OriginY = static_cast<float>(Ctx.LatticeOriginVoxels.Y);
                const float OriginZ = static_cast<float>(Ctx.LatticeOriginVoxels.Z);
                const int32 IX0 = FMath::CeilToInt(
                    ((float)VoxelBox.Min.X - OriginX) / (float)Step - 1.0e-4f);
                const int32 IY0 = FMath::CeilToInt(
                    ((float)VoxelBox.Min.Y - OriginY) / (float)Step - 1.0e-4f);
                const int32 IZ0 = FMath::CeilToInt(
                    ((float)VoxelBox.Min.Z - OriginZ) / (float)Step - 1.0e-4f);
                const int32 IX1 = FMath::FloorToInt(
                    ((float)VoxelBox.Max.X - OriginX) / (float)Step + 1.0e-4f);
                const int32 IY1 = FMath::FloorToInt(
                    ((float)VoxelBox.Max.Y - OriginY) / (float)Step + 1.0e-4f);
                const int32 IZ1 = FMath::FloorToInt(
                    ((float)VoxelBox.Max.Z - OriginZ) / (float)Step + 1.0e-4f);
                const int64 Count = ((int64)IX1 - IX0 + 1)
                                 * ((int64)IY1 - IY0 + 1)
                                 * ((int64)IZ1 - IZ0 + 1);
                // A full LOD0 root is intentionally left on the cheap interval path.  Refined
                // child boxes are at most 17^3 lattice vertices, so their exact threshold bound
                // couples the worm's spatial maximum to each child's SDF lower bound without
                // evaluating the room graph 35,937 times.
                if (IX1 >= IX0 && IY1 >= IY0 && IZ1 >= IZ0
                    && Count > 0 && Count <= 10000)
                {
                    float MaxThresholdFactor = 0.0f;
                    const float WormZFrequency = P.WormFrequency * P.WormHorizontalBias;
                    for (int32 IZ = IZ0; IZ <= IZ1; ++IZ)
                    for (int32 IY = IY0; IY <= IY1; ++IY)
                    for (int32 IX = IX0; IX <= IX1; ++IX)
                    {
                        const float WorldX = OriginX + (float)IX * (float)Step;
                        const float WorldY = OriginY + (float)IY * (float)Step;
                        const float WorldZ = OriginZ + (float)IZ * (float)Step;
                        const float EffectiveZ = (P.VerticalScale != 1.0f
                                                   && P.VerticalScale > 0.0f)
                                                ? WorldZ / P.VerticalScale : WorldZ;
                        const FVector3f NoiseBase(
                            WorldX * P.WormFrequency + VoxelHash::SeedOffset(SeedU, 1.0f),
                            WorldY * P.WormFrequency + VoxelHash::SeedOffset(SeedU, 1.7f),
                            EffectiveZ * WormZFrequency + VoxelHash::SeedOffset(SeedU, 2.3f));
                        const float N1 = FMath::Abs(VoxelNoise::Perlin3D(NoiseBase)
                                                   * VOXEL_NOISE_SCALE);
                        if (N1 >= P.WormThreshold) { continue; }

                        const FVector3f NoiseOffset(
                            137.0f, 259.0f, 431.0f);
                        const float N2 = FMath::Abs(VoxelNoise::Perlin3D(
                            NoiseBase + NoiseOffset) * VOXEL_NOISE_SCALE);
                        const float WormValue = N1 + N2;
                        if (WormValue < P.WormThreshold)
                        {
                            MaxThresholdFactor = FMath::Max(
                                MaxThresholdFactor,
                                1.0f - WormValue / P.WormThreshold);
                        }
                    }

                    if (!(MaxThresholdFactor > 0.0f)) { return 0.0f; }
                    float MaxNetworkMask = 1.0f;
                    if (H.Sdf.Min >= P.WormNetworkRange)
                    {
                        MaxNetworkMask = 0.0f;
                    }
                    else if (H.Sdf.Min > 0.0f)
                    {
                        MaxNetworkMask = 1.0f - SmoothStep01(
                            FMath::Clamp(H.Sdf.Min / P.WormNetworkRange, 0.0f, 1.0f));
                    }
                    return Strength * MaxThresholdFactor
                        * FMath::Clamp(MaxNetworkMask, 0.0f, 1.0f);
                }
            }

            // NetworkMask is monotone non-increasing in CaveSDF.  The source interval's lower
            // endpoint therefore gives a sound upper bound for the mask across the whole box,
            // not just the old all-or-nothing test at WormNetworkRange.  This matters in deep
            // solid: a positive SDF can leave only a fraction of the worm amplitude, enough for
            // the constant-rock margin to survive even when the full WormStrength would not.
            float MaxNetworkMask = 1.0f;
            if (H.Sdf.Min >= P.WormNetworkRange)
            {
                MaxNetworkMask = 0.0f;
            }
            else if (H.Sdf.Min > 0.0f)
            {
                MaxNetworkMask = 1.0f - SmoothStep01(
                    FMath::Clamp(H.Sdf.Min / P.WormNetworkRange, 0.0f, 1.0f));
            }
            return Strength * FMath::Clamp(MaxNetworkMask, 0.0f, 1.0f);
        }

        /** Le ver ne REMPLIT jamais : `InOut.Density -= …` avec un terme positif. */
        float MaxFillOverBox(const FBox&, const FVoxelOpContext&) const override
        {
            return 0.0f;
        }

        /** L'amplitude max de carve, en unités de densité. Borne BRUTE : elle ignore la portée du
         *  réseau, c'est `MaxCarveOverBox` qui l'applique. */
        float MaxCarveAmplitude() const
        {
            if (!(P.WormStrength > 0.0f && P.WormThreshold > 0.0f)) { return 0.0f; }
            return VoxelMath::IsFinite(P.WormStrength) ? P.WormStrength : FLT_MAX;
        }

        const TCHAR* DebugName() const override { return TEXT("WormFieldSource"); }
        void EvalBlock(const FVoxelOpBlock& Block) const override { VF_EvalBlockByScalar(*this, Block); }

    private:
        FStrateGenerationParams P;
        uint32 SeedU;
    };

#if WITH_EDITOR
    //=============================================================================
    // RÔLE 2 — COMBINER : RÉGIONS LATÉRALES
    //=============================================================================
    // The creative stacks stay independent.  This op owns only their density-level junction;
    // the parent FVoxelOpStack appends the global structural posts after it.  In particular, no
    // recipe can make the spine, vertical seal, passage carve, or XY edge seal region-local.
    class FLateralRegionBlendOp final : public IVoxelDensityOp
    {
    public:
        FLateralRegionBlendOp(const FVoxelStrateRegionManifest& InManifest,
                              TArray<FVoxelOpStack>&& InStacks)
            : RegionStacks(MoveTemp(InStacks))
        {
            // Keep only partition metadata here.  The native vectors/recipes have already been
            // consumed by the stacks and must not be duplicated in the voxel operator.
            PartitionManifest.bValid = true;
            PartitionManifest.Seed = InManifest.Seed;
            PartitionManifest.StrateIndex = InManifest.StrateIndex;
            PartitionManifest.RegionCount = InManifest.RegionCount;
            PartitionManifest.PartitionSeed = InManifest.PartitionSeed;
            PartitionManifest.LatticeCellSize = InManifest.LatticeCellSize;
            PartitionManifest.BlendWidth = InManifest.BlendWidth;
            PartitionManifest.Regions.SetNum(InManifest.RegionCount);
        }

        EVoxelOpRole GetRole() const override { return EVoxelOpRole::Combiner; }
        EVoxelOpChannelMask ChannelReads() const override { return VoxelOpChannels::None; }
        EVoxelOpChannelMask ChannelWrites() const override { return VoxelOpChannels::Density; }
        bool IsAdditive() const override { return false; }

        void PrepareChunk(const FVoxelOpContext& Ctx) override
        {
            PartitionCache.PrepareForChunk(PartitionManifest, Ctx.ChunkCoord);
            bPrepared = true;
            for (FVoxelOpStack& Stack : RegionStacks)
            {
                Stack.PrepareChunk(Ctx);
            }
        }

        void Eval(float WorldX, float WorldY, float WorldZ,
                  FVoxelOpSample& InOut) const override
        {
            if (RegionStacks.Num() == 0) { return; }

            const FVoxelStrateRegionQuery Query = bPrepared
                ? PartitionCache.Query(WorldX, WorldY)
                : VF_QueryStrateRegion(PartitionManifest, WorldX, WorldY);
            const int32 Primary = FMath::Clamp(Query.PrimaryRegion, 0, RegionStacks.Num() - 1);
            const float PrimaryDensity = RegionStacks[Primary].EvalInternal(
                WorldX, WorldY, WorldZ);
            float Density = PrimaryDensity;

            if (Query.NeighborWeight > 0.0f && Query.NeighborRegion != INDEX_NONE
                && RegionStacks.IsValidIndex(Query.NeighborRegion)
                && Query.NeighborRegion != Primary)
            {
                const float NeighborDensity = RegionStacks[Query.NeighborRegion].EvalInternal(
                    WorldX, WorldY, WorldZ);
                Density = FMath::Lerp(PrimaryDensity, NeighborDensity, Query.NeighborWeight);
            }
            InOut.Density = Density;
        }

        /**
         * A box wholly outside the band can use its one creative stack.  A box that may touch a
         * bisector or the band asks every region stack.  If any is Mixed, the parent is Mixed. If
         * all possible stacks agree, their convex density blend has the same sign everywhere, so
         * the blend band is proved uniform too.  This is deliberately stricter than sampling a
         * center point and is the protection against a cross-region false uniform tile.
         */
        EVoxelTileClass ClassifyBox(const FBox& VoxelBox,
                                    const FVoxelOpContext& Ctx) const override
        {
            if (RegionStacks.Num() == 0) { return EVoxelTileClass::Mixed; }

            const FVoxelStrateRegionBoxProof Proof =
                VF_AnalyzeStrateRegionBox(PartitionManifest, VoxelBox);
            if (Proof.bProvablySingleRegion)
            {
                const FVoxelStrateRegionQuery Query = VF_QueryStrateRegion(
                    PartitionManifest,
                    ((float)VoxelBox.Min.X + (float)VoxelBox.Max.X) * 0.5f,
                    ((float)VoxelBox.Min.Y + (float)VoxelBox.Max.Y) * 0.5f);
                const int32 Primary = FMath::Clamp(
                    Query.PrimaryRegion, 0, RegionStacks.Num() - 1);
                return RegionStacks[Primary].ClassifyBox(VoxelBox, Ctx);
            }

            EVoxelTileClass CommonVerdict = EVoxelTileClass::Mixed;
            for (const FVoxelOpStack& Stack : RegionStacks)
            {
                const EVoxelTileClass Verdict = Stack.ClassifyBox(VoxelBox, Ctx);
                if (Verdict == EVoxelTileClass::Mixed)
                {
                    return EVoxelTileClass::Mixed;
                }
                if (CommonVerdict == EVoxelTileClass::Mixed)
                {
                    CommonVerdict = Verdict;
                }
                else if (CommonVerdict != Verdict)
                {
                    return EVoxelTileClass::Mixed;
                }
            }
            return CommonVerdict;
        }

        EVoxelOpEffect EffectOverBox(const FBox&, const FVoxelOpContext&) const override
        {
            return EVoxelOpEffect::Both;
        }

        const TCHAR* DebugName() const override { return TEXT("LateralRegionBlendOp"); }

    private:
        FVoxelStrateRegionManifest PartitionManifest;
        TArray<FVoxelOpStack> RegionStacks;
        mutable FVoxelStrateRegionPartitionCache PartitionCache;
        mutable bool bPrepared = false;
    };
#endif

}   // ⚠️ FIN DU NAMESPACE ANONYME — TOUT NOUVEL OPÉRATEUR SE MET AU-DESSUS DE CETTE LIGNE.
    // Même piège que dans VoxelHeightOpStack.cpp : s'ancrer sur une bannière située plus bas
    // (« FVoxelOpStack », « FABRIQUES ») insère la classe HORS du namespace anonyme, et l'accolade
    // ajoutée avec elle ne ferme rien → C2059.
    // END OF THE ANONYMOUS NAMESPACE — new operators go ABOVE this line.

//=============================================================================
// DIAGNOSTIC — voir la déclaration dans VoxelDensityOpStack.h
//=============================================================================

VoxelDensityOps::FRoomBoxDiagnostic VoxelDensityOps::GetLastRoomBoxDiagnostic()
{
    const FRoomGraphSource::FBoxState& B = FRoomGraphSource::BoxState();

    FRoomBoxDiagnostic D;
    D.HitRooms    = B.HitRooms;
    D.HitTunnels  = B.HitTunnels;
    D.HitPits     = B.HitPits;
    D.HitChimneys = B.HitChimneys;
    D.NumRooms    = B.NumRooms;
    D.NumTunnels  = B.NumTunnels;
    D.NumPits     = B.NumPits;
    D.NumChimneys = B.NumChimneys;
    D.HitRoomsNoWarp   = B.HitRoomsNoWarp;
    D.HitTunnelsNoWarp = B.HitTunnelsNoWarp;
    D.WarpDilation     = B.WarpDilation;
    D.bMayHaveTunnelCoreAir = B.bMayHaveTunnelCoreAir;
    D.bMayHaveTunnelSupportFloor = B.bMayHaveTunnelSupportFloor;
    D.bMayHaveTunnelCoreTail = B.bMayHaveTunnelCoreTail;
    D.ExactTailQueries = B.ExactTailQueries;
    D.ExactTailEvaluated = B.ExactTailEvaluated;
    D.ExactTailCycles = B.ExactTailCycles;
    D.PropagateCycles = B.PropagateCycles;
    D.ExactPrimitiveCycles = B.ExactPrimitiveCycles;
    D.CacheWindowCycles = B.CacheWindowCycles;
    D.NumRooms = B.NumRooms;
    D.NumTunnels = B.NumTunnels;
    D.NumRoomFloorJoins = B.NumRoomFloorJoins;
    D.NumPits = B.NumPits;
    D.NumChimneys = B.NumChimneys;
    return D;
}

void VoxelDensityOps::ReportWorkerRoomGraphCacheFootprint()
{
    if (!VoxelDensityProfile::IsEnabled())
    {
        return;
    }

    uint64 DynamicBytes = 0;
    uint64 EntryBytes = 0;
    uint64 LargestEntryBytes = 0;
    uint64 ValidEntries = 0;
    VoxelDensityProfile::FCacheMemoryBreakdown Breakdown;
    for (int32 Bank = 0; Bank < RoomGraphCacheBankCount; ++Bank)
    {
        for (int32 Index = 0; Index < RoomGraphCacheSlotCount; ++Index)
        {
            const FRoomGraphCacheEntry& Entry = GRoomGraphCache[Bank][Index];
            if (!Entry.bValid)
            {
                continue;
            }

            ++ValidEntries;
            Breakdown += Entry.Cache.GetAllocatedSizeBreakdown();
            const uint64 EntryDynamicBytes = static_cast<uint64>(Entry.Cache.GetAllocatedSize());
            const uint64 FullEntryBytes = static_cast<uint64>(sizeof(FRoomGraphCacheEntry))
                + EntryDynamicBytes;
            DynamicBytes += EntryDynamicBytes;
            EntryBytes += FullEntryBytes;
            LargestEntryBytes = FMath::Max(LargestEntryBytes, FullEntryBytes);
        }
    }
    Breakdown.SlotStorageBytes = static_cast<uint64>(sizeof(GRoomGraphCache));

    VoxelDensityProfile::SetWorkerRoomGraphCacheFootprint(
        RoomGraphCacheBankCount * RoomGraphCacheSlotCount,
        ValidEntries,
        static_cast<uint64>(sizeof(GRoomGraphCache)),
        DynamicBytes,
        EntryBytes,
        LargestEntryBytes,
        ValidEntries,
        Breakdown);
}

void VoxelDensityOps::ReportSharedRoomGraphCacheFootprint()
{
    uint64 ReadyEntries = 0;
    uint64 BuildingEntries = 0;
    uint64 LargestEntryBytes = 0;
    uint64 CacheBytes = 0;
    {
        FScopeLock Lock(&GSharedRoomGraphCacheMutex);
        for (const TSharedPtr<FSharedRoomGraphCacheEntry, ESPMode::ThreadSafe>& Entry
             : GSharedRoomGraphCache)
        {
            if (Entry->bBuilding)
            {
                ++BuildingEntries;
            }
            else
            {
                ++ReadyEntries;
                LargestEntryBytes = FMath::Max(LargestEntryBytes, Entry->AllocatedBytes);
                CacheBytes += Entry->AllocatedBytes;
            }
        }
        UE_LOG(LogTemp, Display,
            TEXT("[VoxelForgeSharedCacheMemory] entries=%d ready=%llu building=%llu "
                 "resident_bytes=%llu resident_mib=%.2f budget_bytes=%llu "
                 "largest_entry_bytes=%llu"),
            GSharedRoomGraphCache.Num(),
            static_cast<unsigned long long>(ReadyEntries),
            static_cast<unsigned long long>(BuildingEntries),
            static_cast<unsigned long long>(CacheBytes),
            static_cast<double>(CacheBytes) / (1024.0 * 1024.0),
            static_cast<unsigned long long>(SharedRoomGraphCacheBudgetBytes),
            static_cast<unsigned long long>(LargestEntryBytes));
    }
}

void VoxelDensityOps::TrimSharedRoomGraphCache(uint64 KeepManagerLifetimeId)
{
    uint64 BeforeBytes = 0;
    uint64 AfterBytes = 0;
    int32 BeforeEntries = 0;
    int32 AfterEntries = 0;
    {
        FScopeLock Lock(&GSharedRoomGraphCacheMutex);
        BeforeBytes = GSharedRoomGraphCacheResidentBytes;
        BeforeEntries = GSharedRoomGraphCache.Num();
        RemoveStaleSharedRoomGraphCachesLocked(KeepManagerLifetimeId);
        AfterBytes = GSharedRoomGraphCacheResidentBytes;
        AfterEntries = GSharedRoomGraphCache.Num();
    }
    if (BeforeBytes != AfterBytes || BeforeEntries != AfterEntries)
    {
        UE_LOG(LogTemp, Display,
            TEXT("[VoxelForgeSharedCacheMemory] trim keep_lifetime=%llu entries=%d->%d "
                 "resident_bytes=%llu->%llu"),
            static_cast<unsigned long long>(KeepManagerLifetimeId),
            BeforeEntries, AfterEntries,
            static_cast<unsigned long long>(BeforeBytes),
            static_cast<unsigned long long>(AfterBytes));
    }
}

//=============================================================================
// FVoxelOpStack
//=============================================================================

SIZE_T FVoxelOpStack::GetAllocatedSize() const
{
    SIZE_T Bytes = static_cast<SIZE_T>(Ops.GetAllocatedSize());
    for (const FOpEntry& Entry : Ops)
    {
        if (Entry.Op.IsValid())
        {
            Bytes += FMemory::GetAllocSize(const_cast<IVoxelDensityOp*>(Entry.Op.Get()));
        }
    }
    return Bytes;
}

bool FVoxelOpStack::TryGetLastTunnelCoreWorldEvaluation(
    FTunnelCoreWorldEvaluation& OutEvaluation) const
{
    // The last provider wins in the same way the structural tail is ordered.  At present the
    // tunnel source is the only provider; walking backwards keeps this safe if a composed stack
    // later adds another structural core provider.
    for (int32 Index = Ops.Num() - 1; Index >= 0; --Index)
    {
        if (Ops[Index].Op->TryGetLastTunnelCoreWorldEvaluation(OutEvaluation))
        {
            return true;
        }
    }
    return false;
}

void FVoxelOpStack::DiagnoseBox(const FBox& VoxelBox, const FVoxelOpContext& Ctx,
                                int32 SampleStep, TArray<FOpBoxDiagnostic>& OutDiagnostics,
                                EVoxelTileClass* OutVerdict) const
{
    OutDiagnostics.Reset();
    OutDiagnostics.SetNum(Ops.Num());
    if (OutVerdict != nullptr)
    {
        *OutVerdict = EVoxelTileClass::Mixed;
    }

    if (!VoxelBox.IsValid || SampleStep <= 0)
    {
        return;
    }

    FVoxelBoxHypotheses H;
    for (int32 Index = 0; Index < Ops.Num(); ++Index)
    {
        const FOpEntry& Entry = Ops[Index];
        FOpBoxDiagnostic& Diagnostic = OutDiagnostics[Index];
        Diagnostic.Index = Index;
        Diagnostic.Name = Entry.Op.IsValid() ? FString(Entry.Op->DebugName()) : TEXT("(null op)");
        Diagnostic.bWritesSdf = (Entry.Writes & VoxelOpChannels::Sdf) != 0;

        if (!Entry.Op.IsValid())
        {
            continue;
        }

        // This is intentionally the same order and branch structure as VF_FoldOp. The diagnostic
        // must explain the production classifier, not an approximation of it.
        const EVoxelTileClass Forced = Entry.Op->ClassifyBox(VoxelBox, Ctx);
        Diagnostic.ForcedVerdict = Forced;
        Diagnostic.bForced = Forced != EVoxelTileClass::Mixed;
        if (Diagnostic.bForced)
        {
            Diagnostic.Effect = EVoxelOpEffect::Identity;
            Diagnostic.ForcedMargin = Entry.Op->ForcedMarginOverBox(VoxelBox, Ctx);
            VF_ForceHypotheses(H, Forced, Diagnostic.ForcedMargin);
            if (Diagnostic.bWritesSdf)
            {
                Entry.Op->PropagateSdfOverBox(H.Sdf, VoxelBox, Ctx);
                Diagnostic.bHasSdfBound = H.Sdf.IsKnown();
                if (Diagnostic.bHasSdfBound)
                {
                    Diagnostic.SdfBoundMin = H.Sdf.Min;
                    Diagnostic.SdfBoundMax = H.Sdf.Max;
                }
            }
        }
        else
        {
            Diagnostic.Effect = Entry.Op->EffectOverBox(VoxelBox, Ctx, H);
            Diagnostic.MaxCarve = Entry.Op->MaxCarveOverBox(VoxelBox, Ctx, H);
            Diagnostic.MaxFill = Entry.Op->MaxFillOverBox(VoxelBox, Ctx, H);
            VF_FoldEffect(H, Diagnostic.Effect, Diagnostic.MaxCarve, Diagnostic.MaxFill);
            if (Diagnostic.bWritesSdf)
            {
                Entry.Op->PropagateSdfOverBox(H.Sdf, VoxelBox, Ctx);
                Diagnostic.bHasSdfBound = H.Sdf.IsKnown();
                if (Diagnostic.bHasSdfBound)
                {
                    Diagnostic.SdfBoundMin = H.Sdf.Min;
                    Diagnostic.SdfBoundMax = H.Sdf.Max;
                }
            }
        }
    }

    if (OutVerdict != nullptr)
    {
        *OutVerdict = H.Resolve();
    }

    const int32 MinX = FMath::CeilToInt(static_cast<float>(VoxelBox.Min.X));
    const int32 MinY = FMath::CeilToInt(static_cast<float>(VoxelBox.Min.Y));
    const int32 MinZ = FMath::CeilToInt(static_cast<float>(VoxelBox.Min.Z));
    const int32 MaxX = FMath::FloorToInt(static_cast<float>(VoxelBox.Max.X));
    const int32 MaxY = FMath::FloorToInt(static_cast<float>(VoxelBox.Max.Y));
    const int32 MaxZ = FMath::FloorToInt(static_cast<float>(VoxelBox.Max.Z));
    if (MinX > MaxX || MinY > MaxY || MinZ > MaxZ)
    {
        return;
    }

    constexpr float BoundEpsilon = 1.0e-4f;
    for (int32 Z = MinZ; Z <= MaxZ; )
    {
        for (int32 Y = MinY; Y <= MaxY; )
        {
            for (int32 X = MinX; X <= MaxX; )
            {
                FVoxelOpSample Sample;
                for (int32 Index = 0; Index < Ops.Num(); ++Index)
                {
                    const FOpEntry& Entry = Ops[Index];
                    FOpBoxDiagnostic& Diagnostic = OutDiagnostics[Index];
                    if (!Entry.Op.IsValid())
                    {
                        continue;
                    }

                    const float DensityBefore = Sample.Density;
                    Entry.Op->Eval(static_cast<float>(X), static_cast<float>(Y),
                                   static_cast<float>(Z), Sample);
                    const float DensityDelta = Sample.Density - DensityBefore;
                    if (VoxelMath::IsFinite(DensityDelta))
                    {
                        Diagnostic.bHasDensityDelta = true;
                        Diagnostic.ActualDensityDeltaMin =
                            FMath::Min(Diagnostic.ActualDensityDeltaMin, DensityDelta);
                        Diagnostic.ActualDensityDeltaMax =
                            FMath::Max(Diagnostic.ActualDensityDeltaMax, DensityDelta);

                        // A forced classifier is a stronger statement than a directional delta
                        // bound. Its density can legitimately replace an arbitrary incoming
                        // value (the constant source is the canonical example), so do not call
                        // that replacement a MaxFill/MaxCarve violation. ForcedMargin records the
                        // proof margin separately for the report.
                        if (!Diagnostic.bForced)
                        {
                            const float CarveEpsilon = BoundEpsilon
                                * FMath::Max(1.0f, FMath::Abs(Diagnostic.MaxCarve));
                            const float FillEpsilon = BoundEpsilon
                                * FMath::Max(1.0f, FMath::Abs(Diagnostic.MaxFill));
                            if (VoxelMath::IsFinite(Diagnostic.MaxCarve)
                                && DensityDelta < -Diagnostic.MaxCarve - CarveEpsilon)
                            {
                                Diagnostic.bCarveBoundViolated = true;
                            }
                            if (VoxelMath::IsFinite(Diagnostic.MaxFill)
                                && DensityDelta > Diagnostic.MaxFill + FillEpsilon)
                            {
                                Diagnostic.bFillBoundViolated = true;
                            }
                        }
                    }

                    if (Diagnostic.bWritesSdf && VoxelMath::IsFinite(Sample.Sdf))
                    {
                        Diagnostic.bHasSdfValue = true;
                        Diagnostic.ActualSdfMin = FMath::Min(
                            Diagnostic.ActualSdfMin, Sample.Sdf);
                        Diagnostic.ActualSdfMax = FMath::Max(
                            Diagnostic.ActualSdfMax, Sample.Sdf);
                    }
                    ++Diagnostic.SampleCount;
                }

                if (X > MaxX - SampleStep) { break; }
                X += SampleStep;
            }
            if (Y > MaxY - SampleStep) { break; }
            Y += SampleStep;
        }
        if (Z > MaxZ - SampleStep) { break; }
        Z += SampleStep;
    }
}

bool FVoxelOpStack::ValidateChannelOrder(FString* OutError) const
{
    if (OutError != nullptr) { OutError->Reset(); }

    auto DescribeMask = [](EVoxelOpChannelMask Mask) -> FString
    {
        FString Result;
        if ((Mask & VoxelOpChannels::Density) != 0) { Result += TEXT("Density"); }
        if ((Mask & VoxelOpChannels::Sdf) != 0)
        {
            if (!Result.IsEmpty()) { Result += TEXT(", "); }
            Result += TEXT("Sdf");
        }
        return Result.IsEmpty() ? TEXT("none") : Result;
    };

    auto DescribeResourceMask = [](EVoxelOpResourceMask Mask) -> FString
    {
        FString Result;
        if ((Mask & VoxelOpResources::RoomGeometry) != 0) { Result += TEXT("RoomGeometry"); }
        if ((Mask & VoxelOpResources::ShaftGeometry) != 0)
        {
            if (!Result.IsEmpty()) { Result += TEXT(", "); }
            Result += TEXT("ShaftGeometry");
        }
        if ((Mask & VoxelOpResources::SurfaceColumn) != 0)
        {
            if (!Result.IsEmpty()) { Result += TEXT(", "); }
            Result += TEXT("SurfaceColumn");
        }
        return Result.IsEmpty() ? TEXT("none") : Result;
    };

    auto Fail = [&](int32 Index, const FOpEntry* Entry, const TCHAR* Rule) -> bool
    {
        if (OutError != nullptr)
        {
            const TCHAR* Name = (Entry != nullptr && Entry->Op.Get() != nullptr)
                              ? Entry->Op->DebugName() : TEXT("(null op)");
            const EVoxelOpChannelMask Reads = Entry != nullptr ? Entry->Reads : VoxelOpChannels::None;
            const EVoxelOpChannelMask Writes = Entry != nullptr ? Entry->Writes : VoxelOpChannels::None;
            *OutError = FString::Printf(
                TEXT("op %d (%s) violates stack DAG: %s; reads=[%s], writes=[%s], requires=[%s], provides=[%s]"),
                Index, Name, Rule, *DescribeMask(Reads), *DescribeMask(Writes),
                *DescribeResourceMask(Entry != nullptr ? Entry->RequiredResources : VoxelOpResources::None),
                *DescribeResourceMask(Entry != nullptr ? Entry->ProvidedResources : VoxelOpResources::None));
        }
        return false;
    };

    // The two array slots are deliberately explicit: FVoxelOpSample has exactly two fields, and
    // adding a third field requires extending EVoxelOpChannel and this validator together.
    int32 LastWriter[2] = { INDEX_NONE, INDEX_NONE };
    EVoxelOpResourceMask AvailableResources = VoxelOpResources::None;

    auto ChannelIndex = [](EVoxelOpChannelMask Channel) -> int32
    {
        return Channel == VoxelOpChannels::Density ? 0 : 1;
    };

    for (int32 Index = 0; Index < Ops.Num(); ++Index)
    {
        const FOpEntry& Entry = Ops[Index];
        if (Entry.Op.Get() == nullptr) { return Fail(Index, &Entry, TEXT("null operator")); }

        if ((Entry.RequiredResources & VoxelOpResources::All) != Entry.RequiredResources)
        {
            return Fail(Index, &Entry, TEXT("requires an unknown op resource"));
        }
        if ((Entry.ProvidedResources & VoxelOpResources::All) != Entry.ProvidedResources)
        {
            return Fail(Index, &Entry, TEXT("provides an unknown op resource"));
        }
        if ((Entry.RequiredResources
             & static_cast<EVoxelOpResourceMask>(~AvailableResources)) != 0)
        {
            return Fail(Index, &Entry,
                        TEXT("requires op state before an earlier provider published it"));
        }

        if ((Entry.Reads & VoxelOpChannels::All) != Entry.Reads)
        {
            return Fail(Index, &Entry, TEXT("reads an unknown sample channel"));
        }
        if ((Entry.Writes & VoxelOpChannels::All) != Entry.Writes)
        {
            return Fail(Index, &Entry, TEXT("writes an unknown sample channel"));
        }

        if (Entry.bAdditive && Entry.Writes == VoxelOpChannels::None)
        {
            return Fail(Index, &Entry, TEXT("an additive operator must publish a channel"));
        }
        if (Entry.bAdditive
            && (Entry.Writes & static_cast<EVoxelOpChannelMask>(~Entry.Reads)) != 0)
        {
            return Fail(Index, &Entry,
                        TEXT("an additive operator must read every channel it writes"));
        }

        const EVoxelOpChannelMask Channels[] = {
            VoxelOpChannels::Density, VoxelOpChannels::Sdf
        };
        for (const EVoxelOpChannelMask Channel : Channels)
        {
            if ((Entry.Reads & Channel) == 0) { continue; }

            const int32 Writer = LastWriter[ChannelIndex(Channel)];
            const bool bRootIdentityFold = Entry.Op->GetRole() == EVoxelOpRole::FieldSource
                                         && (Entry.Writes & Channel) != 0
                                         && Writer == INDEX_NONE;
            if (Writer == INDEX_NONE && !bRootIdentityFold)
            {
                return Fail(Index, &Entry,
                            TEXT("reads a channel before a producer has published it"));
            }
        }

        for (const EVoxelOpChannelMask Channel : Channels)
        {
            if ((Entry.Writes & Channel) == 0 || (Entry.Reads & Channel) != 0) { continue; }

            // A write-only op is an assignment/replacement. It may establish the first version,
            // but it may not erase a previously produced channel without declaring a read of it.
            if (LastWriter[ChannelIndex(Channel)] != INDEX_NONE)
            {
                return Fail(Index, &Entry,
                            TEXT("a write-only replacement would clobber an existing channel"));
            }
        }

        for (const EVoxelOpChannelMask Channel : Channels)
        {
            if ((Entry.Writes & Channel) != 0)
            {
                // Every read above has either consumed this writer's predecessor or the explicit
                // root identity. Updating the version here makes all later consumer edges forward.
                LastWriter[ChannelIndex(Channel)] = Index;
            }
        }

        AvailableResources |= Entry.ProvidedResources;
    }

    return true;
}

void FVoxelOpStack::AppendStructuralPost(float StrateTopWorldZ, float StrateBottomWorldZ,
                                         float SealThickness, float BaseDensity, float SpineRadius,
                                         const UVoxelStrateManager* StrateManager,
                                         bool bAppendEdgeSeal)
{
    // ORDRE NON NÉGOCIABLE : la spine creuse l'intérieur (et ne touche JAMAIS les bandes de seal),
    // le seal vertical re-solidifie ses bandes, les passages (tube + landing + support) percent
    // les seals, puis la limite XY gagne sur tout ce qui précède. Ainsi, même un passage placé
    // dans la rampe ou au-delà du rayon ne peut pas ouvrir la coque extérieure. Les éditions joueur
    // restent le dernier post de GetDensityAt, hors de cette pile, comme avant.
    Add(MakeUnique<FOriginSpineOp>(StrateTopWorldZ, StrateBottomWorldZ, SealThickness, BaseDensity, SpineRadius));
    Add(MakeUnique<FBoundarySealOp>(StrateTopWorldZ, StrateBottomWorldZ, SealThickness, BaseDensity));
    Add(MakeUnique<FPassageCarveOp>(StrateManager, BaseDensity, SealThickness,
                                    StrateTopWorldZ, StrateBottomWorldZ, SpineRadius));
    if (bAppendEdgeSeal)
    {
        Add(MakeUnique<FXYEdgeSealOp>(BaseDensity));
    }
}

//=============================================================================
// FABRIQUES / FACTORIES
//=============================================================================

namespace VoxelDensityOps
{
    bool GetStrateOpContract(EVoxelStrateOpClass OpClass, FVoxelStrateOpContract& OutContract)
    {
        const FStrateGenerationParams TunnelParams;
        const FSlabGenerationParams SlabParams;
        const FMazeGenerationParams MazeParams;
        const FSurfaceGenerationParams SurfaceParams;
        const FVerticalShaftParams ShaftParams;
        const FFloatingIslandParams IslandParams;

        TUniquePtr<IVoxelDensityOp> Probe;
        switch (OpClass)
        {
        case EVoxelStrateOpClass::ConstantRockSource:
            Probe = MakeConstantRockSource(TunnelParams.BaseDensity);
            break;
        case EVoxelStrateOpClass::ConstantVoidSource:
            Probe = MakeConstantVoidSource(IslandParams.BaseDensity);
            break;
        case EVoxelStrateOpClass::RoomGraphSource:
            Probe = MakeUnique<FRoomGraphSource>(TunnelParams, 0, nullptr);
            break;
        case EVoxelStrateOpClass::LatticeCorridorSource:
            Probe = MakeUnique<FLatticeCorridorSource>(MazeParams, 0);
            break;
        case EVoxelStrateOpClass::ShaftFieldSource:
            Probe = MakeUnique<FShaftFieldSource>(ShaftParams, 0, 14.0f);
            break;
        case EVoxelStrateOpClass::IslandBlobSource:
            Probe = MakeUnique<FIslandBlobSource>(IslandParams, 0);
            break;
        case EVoxelStrateOpClass::NoiseRibbonSource:
            Probe = MakeUnique<FNoiseRibbonSource>(MazeParams, 0);
            break;
        case EVoxelStrateOpClass::SdfRoughnessMod:
            Probe = MakeSdfRoughnessMod(MazeParams.SurfaceRoughness, 0.12f, 3, 8.0f);
            break;
        case EVoxelStrateOpClass::SdfCarve:
            Probe = MakeSdfCarve(2.0f, TunnelParams.BaseDensity);
            break;
        case EVoxelStrateOpClass::SdfFill:
            Probe = MakeSdfFill(2.0f, IslandParams.BaseDensity);
            break;
        case EVoxelStrateOpClass::GridColumnMod:
            Probe = MakeUnique<FGridColumnMod>(SlabParams, 0);
            break;
        case EVoxelStrateOpClass::CaveRoughnessMod:
            Probe = MakeUnique<FCaveRoughnessMod>(TunnelParams, 0);
            break;
        case EVoxelStrateOpClass::CaveTerraceMod:
            Probe = MakeUnique<FCaveTerraceMod>(TunnelParams, 0, nullptr);
            break;
        case EVoxelStrateOpClass::LayerLineMod:
            Probe = MakeUnique<FLayerLineMod>(TunnelParams, nullptr);
            break;
        case EVoxelStrateOpClass::RibbingMod:
            Probe = MakeUnique<FRibbingMod>(TunnelParams, nullptr);
            break;
        case EVoxelStrateOpClass::CaveOverhangMod:
            Probe = MakeUnique<FCaveOverhangMod>(TunnelParams, 0, nullptr);
            break;
        case EVoxelStrateOpClass::CaveCliffMod:
            Probe = MakeUnique<FCaveCliffMod>(TunnelParams, 0, nullptr);
            break;
        case EVoxelStrateOpClass::ScallopMod:
            Probe = MakeUnique<FScallopMod>(TunnelParams, 0, nullptr);
            break;
        case EVoxelStrateOpClass::CaveArchMod:
            Probe = MakeUnique<FCaveArchMod>(TunnelParams, nullptr);
            break;
        case EVoxelStrateOpClass::RoomColumnMod:
            Probe = MakeUnique<FRoomColumnMod>(TunnelParams, nullptr);
            break;
        case EVoxelStrateOpClass::DomeMod:
            Probe = MakeUnique<FDomeMod>(TunnelParams, nullptr);
            break;
        case EVoxelStrateOpClass::PinchMod:
            Probe = MakeUnique<FPinchMod>(TunnelParams, nullptr);
            break;
        case EVoxelStrateOpClass::FloorBiasMod:
            Probe = MakeUnique<FFloorBiasMod>(TunnelParams, nullptr);
            break;
        case EVoxelStrateOpClass::WormFieldSource:
            Probe = MakeUnique<FWormFieldSource>(TunnelParams, 0);
            break;
        case EVoxelStrateOpClass::ShaftLedgeMod:
            Probe = MakeUnique<FShaftLedgeMod>(ShaftParams, nullptr);
            break;
        case EVoxelStrateOpClass::DensityNoiseCarveMod:
            Probe = MakeUnique<FDensityNoiseMod>(1.0f, 0.02f, 3, 0, false);
            break;
        case EVoxelStrateOpClass::DensityNoiseFillMod:
            Probe = MakeUnique<FDensityNoiseMod>(1.0f, 0.02f, 3, 0, true);
            break;
        default:
            return false;
        }

        if (!Probe)
        {
            return false;
        }

        OutContract.Role = Probe->GetRole();
        OutContract.Reads = Probe->ChannelReads();
        OutContract.Writes = Probe->ChannelWrites();
        OutContract.bAdditive = Probe->IsAdditive();
        OutContract.RequiredResources = Probe->RequiredResources();
        OutContract.ProvidedResources = Probe->ProvidedResources();
        return true;
    }

    TUniquePtr<IVoxelDensityOp> MakeConstantRockSource(float BaseDensity)
    {
        return MakeUnique<FConstantFieldSource>(BaseDensity);
    }

    TUniquePtr<IVoxelDensityOp> MakeConstantVoidSource(float BaseDensity)
    {
        // `float Density = -Params.BaseDensity;  // start as open air (void)` — la négation unaire
        // est exacte, donc c'est littéralement la première ligne de GetFloatingIslandDensity.
        return MakeUnique<FConstantFieldSource>(-BaseDensity);
    }

    TUniquePtr<IVoxelDensityOp> MakeLatticeCorridorSource(const FMazeGenerationParams& P, int32 Seed)
    {
        return MakeUnique<FLatticeCorridorSource>(P, Seed);
    }

    TUniquePtr<IVoxelDensityOp> MakeSdfRoughnessMod(float Strength, float Frequency,
                                                    int32 BaseOctaves, float ApplyWithin)
    {
        return MakeUnique<FSdfRoughnessMod>(Strength, Frequency, BaseOctaves, ApplyWithin);
    }

    TUniquePtr<IVoxelDensityOp> MakeSdfCarve(float Blend, float BaseDensity, float MinDivisor)
    {
        return MakeUnique<FSdfConvertOp>(Blend, BaseDensity, -1.0f, MinDivisor);
    }

    TUniquePtr<IVoxelDensityOp> MakeSdfFill(float Blend, float BaseDensity)
    {
        return MakeUnique<FSdfConvertOp>(Blend, BaseDensity, +1.0f, 0.0f);
    }

    TUniquePtr<IVoxelDensityOp> MakeSlabVoidSource(const FSlabGenerationParams& P, int32 Seed)
    {
        return MakeUnique<FSlabVoidSource>(P, Seed);
    }

    TUniquePtr<IVoxelDensityOp> MakeGridColumnMod(const FSlabGenerationParams& P, int32 Seed)
    {
        return MakeUnique<FGridColumnMod>(P, Seed);
    }

    void BuildSurfaceStack(FVoxelOpStack& OutStack, const FSurfaceGenerationParams& P,
                           int32 Seed, float SpineRadius, const UVoxelStrateManager* StrateManager,
                           const TArray<FSurfaceGenerationParams>& PerBiomeParams,
                           TUniquePtr<IVoxelBiomeField> BiomeField,
                           bool bAppendStructuralPosts)
    {
        // ARCHÉTYPE COMPLET : vide + overhang + mélange de biomes.
        // `PerBiomeParams` vide ⇒ chemin sans biomes, strictement inchangé.
        TUniquePtr<FSurfaceColumnSource> ColumnSource =
            MakeUnique<FSurfaceColumnSource>(P, Seed, PerBiomeParams, MoveTemp(BiomeField));
        const FSurfaceColumnSource* ColumnPtr = ColumnSource.Get();
        OutStack.Add(MoveTemp(ColumnSource));

        // L'overhang lit la colonne de la source (mémo partagé, même XY par construction). Même
        // motif que cliff → structural : un modificateur qui a besoin de ce que la source a produit.
        OutStack.Add(MakeUnique<FOverhangShelfMod>(P, Seed, ColumnPtr));

        if (bAppendStructuralPosts)
        {
            OutStack.AppendStructuralPost(P.StrateTopWorldZ, P.StrateBottomWorldZ,
                                          P.BoundarySealThickness, P.BaseDensity,
                                          SpineRadius, StrateManager);
        }
    }

    void BuildSlabStack(FVoxelOpStack& OutStack, const FSlabGenerationParams& P,
                        int32 Seed, float SpineRadius, const UVoxelStrateManager* StrateManager,
                        bool bAppendStructuralPosts)
    {
        // DEUX archétypes entrent ici, aucun branchement ne les distingue — parce que
        // `GetSlabDensity` n'en fait aucun non plus. FlatPlain et CrystalChamber ne diffèrent que
        // par leurs valeurs par défaut, et c'est maintenant visible dans le code plutôt que dans
        // un commentaire. 8 archétypes → 7.
        OutStack.Add(MakeSlabVoidSource(P, Seed));
        OutStack.Add(MakeGridColumnMod(P, Seed));

        if (bAppendStructuralPosts)
        {
            OutStack.AppendStructuralPost(P.StrateTopWorldZ, P.StrateBottomWorldZ,
                                          P.BoundarySealThickness, P.BaseDensity,
                                          SpineRadius, StrateManager);
        }
    }

    void BuildVerticalShaftStack(FVoxelOpStack& OutStack, const FVerticalShaftParams& P,
                                 int32 Seed, float SpineRadius, const UVoxelStrateManager* StrateManager,
                                 bool bAppendStructuralPosts)
    {
        // ⚠️ LA PREUVE QUE L'ABSTRACTION EST RÉELLE, et elle vaut d'être dite : TROIS des cinq
        // opérateurs ci-dessous sont ceux de Maze, **repris sans une ligne de changement** —
        // `ConstantRock`, `SdfRoughness`, `SdfCarve`. Dans le `switch`, Maze et VerticalShafts sont
        // deux fonctions de ~100 lignes qui n'ont rien en commun à l'œil ; en opérateurs, ce sont
        // les MÊMES trois ops avec une source différente. C'est exactement ce que `§2.5` prédisait.
        //
        // THREE of the five ops below are Maze's, reused without a line changed. In the switch,
        // Maze and VerticalShafts are two unrelated ~100-line functions; as operators they are the
        // same three ops with a different source.
        constexpr float CarveBlend = 2.0f;

        TUniquePtr<FShaftFieldSource> ShaftSource =
            MakeUnique<FShaftFieldSource>(P, Seed, SpineRadius);
        const FShaftFieldSource* ShaftPtr = ShaftSource.Get();

        OutStack.Add(MakeConstantRockSource(P.BaseDensity));
        OutStack.Add(MoveTemp(ShaftSource));
        // Fréquence 0.1 et fenêtre `SurfaceRoughness + 4` — les constantes de
        // `GetVerticalShaftDensity`, PAS celles de Maze (0.12 / `R + rough + 2`). Même opérateur,
        // réglages différents : c'est le point.
        OutStack.Add(MakeSdfRoughnessMod(P.SurfaceRoughness, 0.1f, 3, P.SurfaceRoughness + 4.0f));
        OutStack.Add(MakeSdfCarve(CarveBlend, P.BaseDensity));
        OutStack.Add(MakeUnique<FShaftLedgeMod>(P, ShaftPtr));
        OutStack.Add(MakeUnique<FShaftConnectorAirMod>(ShaftPtr, P.BaseDensity));
        OutStack.Add(MakeUnique<FShaftConnectorFloorMod>(ShaftPtr, P.BaseDensity));

        if (bAppendStructuralPosts)
        {
            OutStack.AppendStructuralPost(P.StrateTopWorldZ, P.StrateBottomWorldZ,
                                          P.BoundarySealThickness, P.BaseDensity,
                                          SpineRadius, StrateManager);
        }
    }

    void BuildTunnelNetworkStack(FVoxelOpStack& OutStack, const FStrateGenerationParams& P,
                                 int32 Seed, float SpineRadius, const UVoxelStrateManager* StrateManager,
                                 bool bAppendStructuralPosts)
    {
        // ⚠️ ÉTAPES A + B + C1 — LA PILE EST COMPLÈTE POUR CET ARCHÉTYPE.
        // Portés : échelle verticale, roc de base, warp, graphe de salles (+ pits + cheminées),
        // carve, LES DOUZE MODIFICATEURS DE DÉTAIL (4b–4h), l'override d'op PAR SALLE, les vers,
        // le post structurel.
        //
        // L'override (C1) n'ajoute AUCUN opérateur : il change ce que ONZE d'entre eux LISENT.
        // `FRoomGraphSource::LocalParams()` publie les params de la strate avec l'op de la salle la
        // plus proche appliqué ; les onze modificateurs concernés y lisent leurs champs au lieu des
        // leurs. La rugosité (4b) NON — dans l'original elle précède la déclaration du shadow.
        //
        //---------------------------------------------------------------------
        // ⚠️ CE PORTAGE RETIRE L'IDÉE DE « FRAME OPS » (OPSTACK-DECOMPOSITION §1)
        //---------------------------------------------------------------------
        // `§2` décrivait deux frames imbriqués : `VerticalScale` et `CaveWarp`. En les portant pour
        // de vrai, les deux se sont dissous :
        //   • `CaveWarp` a une portée d'EXACTEMENT UN opérateur (le graphe de salles — pits et
        //     cheminées lisent explicitement les coordonnées non warpées). Une transformation qui
        //     n'enveloppe qu'un opérateur n'est pas un frame, c'est une variable locale.
        //   • `VerticalScale` est `Z / Scale` : une fonction PURE d'un scalaire et d'un param, que
        //     chaque opérateur qui en a besoin recalcule en une ligne. Un frame ne ferait
        //     qu'ajouter un canal pour éviter une division.
        // Il restait le warp d'îles (§7), déjà gardé local pour la même raison. **Zéro frame sur
        // trois candidats** : ce n'était pas une infrastructure manquante, c'était trois fois la
        // même chose vue de loin.
        constexpr float CarveMinDivisor = 1.0f;   // TunnelNetwork plancher son diviseur, cf. FSdfConvertOp

        // La source de salles est retenue par pointeur non possédant : les terrasses re-interrogent
        // son cache SDF en Z±1. Même motif que `FShaftFieldSource` → `FShaftLedgeMod`.
        TUniquePtr<FRoomGraphSource> RoomSource = MakeUnique<FRoomGraphSource>(
            P, Seed, StrateManager);
        const FRoomGraphSource* RoomPtr = RoomSource.Get();

        OutStack.Add(MakeConstantRockSource(P.BaseDensity));
        OutStack.Add(MoveTemp(RoomSource));
        OutStack.Add(MakeSdfCarve(P.SDFBlendRadius, P.BaseDensity, CarveMinDivisor));
        // ── ÉTAPE B : les modificateurs de détail (4b–4h), chacun gated sur
        //    `Sdf < SDFBlendRadius·3` via VF_NearCaveSurface. Voir la note de l'étape B5 là-bas.
        //    L'ORDRE EST CELUI DE L'ORIGINAL et il compte : chacun lit la densité que le précédent
        //    a laissée (le biais de sol est un ajout indépendant appliqué à cette accumulation).
        if (!VoxelDensityAblation::IsDetailOpsOff())
        {
        OutStack.Add(MakeUnique<FCaveRoughnessMod>(P, Seed));            // 4b
        OutStack.Add(MakeUnique<FCaveTerraceMod>(P, Seed, RoomPtr));     // 4c — terrasses
        OutStack.Add(MakeUnique<FLayerLineMod>(P, RoomPtr));             // 4c — lignes de strates
        OutStack.Add(MakeUnique<FRibbingMod>(P, RoomPtr));               // 4c — nervures
        OutStack.Add(MakeUnique<FCaveOverhangMod>(P, Seed, RoomPtr));    // 4c — surplombs
        OutStack.Add(MakeUnique<FCaveCliffMod>(P, Seed, RoomPtr));       // 4c — falaise
        OutStack.Add(MakeUnique<FScallopMod>(P, Seed, RoomPtr));         // 4c — festons
        OutStack.Add(MakeUnique<FCaveArchMod>(P, RoomPtr));              // 4c — arches
        OutStack.Add(MakeUnique<FRoomColumnMod>(P, RoomPtr));            // 4d — colonnes (pré-cuites)
        OutStack.Add(MakeUnique<FDomeMod>(P, RoomPtr));                  // 4g — dômes
        OutStack.Add(MakeUnique<FPinchMod>(P, RoomPtr));                 // 4h — pincement
        OutStack.Add(MakeUnique<FFloorBiasMod>(P, RoomPtr));             // fin 4h — biais de sol
        }
        if (P.WormStrength > 0.0f && P.WormThreshold > 0.0f)
        {
            OutStack.Add(MakeUnique<FWormFieldSource>(P, Seed));
        }

        if (bAppendStructuralPosts)
        {
            OutStack.AppendStructuralPost(P.StrateTopWorldZ, P.StrateBottomWorldZ,
                                          P.BoundarySealThickness, P.BaseDensity,
                                          SpineRadius, StrateManager, false);
            // The generator's shared MC post evaluates the prepared tunnel-core cache after
            // disturbances. Keeping these pre-disturbance graph scans here duplicated both the
            // support and air queries for every voxel; the feature remains in that shared post.
            OutStack.Add(MakeUnique<FXYEdgeSealOp>(P.BaseDensity));

            // Lower only the canonical native graph.  The old scalar TunnelNetwork evaluator is
            // the fused form of exactly these authored stages: it keeps density/SDF/room state in
            // locals and gates the complete 4b-4h detail suite per sample.  Marking the plan after
            // the final op is important: recipes and composer stacks keep the interpreted/block
            // fallback, even when they happen to contain a similar-looking subset of operators.
            OutStack.SetFusedEvaluator(EVoxelOpFusedEvaluator::TunnelNetwork);
        }
    }

    void BuildFloatingIslandStack(FVoxelOpStack& OutStack, const FFloatingIslandParams& P,
                                  int32 Seed, float SpineRadius, const UVoxelStrateManager* StrateManager,
                                  bool bAppendStructuralPosts)
    {
        // ⚠️ LA PILE QUI S'INVERSE, et c'est la mesure que ce portage-ci ajoute : les quatre autres
        // archétypes partent de ROC et CREUSENT ; celui-ci part du VIDE et REMPLIT. Aucune des deux
        // extrémités n'a demandé un opérateur neuf — la source constante et la conversion SDF→densité
        // sont les MÊMES classes, au signe près (`FConstantFieldSource`, `FSdfConvertOp`). Un
        // opérateur qui se réutilise en s'inversant est une preuve plus forte qu'un opérateur qui se
        // réutilise à l'identique : ça veut dire que l'axe abstrait (le signe de la densité) est le
        // bon, pas seulement que deux archétypes se ressemblaient.
        //
        // The stack that runs BACKWARDS: four archetypes start from rock and carve, this one starts
        // from void and fills — and neither end needed a new operator, only the opposite sign.
        const float BlendK = FMath::Max(P.SDFBlendRadius, 0.01f);

        OutStack.Add(MakeConstantVoidSource(P.BaseDensity));
        OutStack.Add(MakeUnique<FIslandBlobSource>(P, Seed));
        // Fréquence 0.08 et 4 octaves — les constantes de `GetFloatingIslandDensity`. Quatrième
        // archétype à réutiliser cet opérateur (Maze 0.12/3, VerticalShafts 0.1/3).
        OutStack.Add(MakeSdfRoughnessMod(P.SurfaceRoughness, 0.08f, 4,
                                         P.SurfaceRoughness + BlendK + 2.0f));
        OutStack.Add(MakeSdfFill(BlendK, P.BaseDensity));

        if (bAppendStructuralPosts)
        {
            OutStack.AppendStructuralPost(P.StrateTopWorldZ, P.StrateBottomWorldZ,
                                          P.BoundarySealThickness, P.BaseDensity,
                                          SpineRadius, StrateManager);
        }
    }

    void BuildMazeStack(FVoxelOpStack& OutStack, const FMazeGenerationParams& P,
                        int32 Seed, float SpineRadius, const UVoxelStrateManager* StrateManager,
                        bool bAppendStructuralPosts)
    {
        // Les constantes viennent telles quelles de GetMazeDensity — elles y étaient codées en dur.
        constexpr float CarveBlend      = 2.0f;
        constexpr float RoughFrequency  = 0.12f;
        constexpr int32 RoughOctaves    = 3;

        const float R = FMath::Max(P.CorridorRadius, 0.5f);

        // Fenêtre d'application de la rugosité : `MazeSDF < R + SurfaceRoughness + 2.0f` dans
        // l'original. Reproduite à l'identique pour que l'égalité binaire tienne.
        const float RoughApplyWithin = R + P.SurfaceRoughness + 2.0f;

        OutStack.Add(MakeConstantRockSource(P.BaseDensity));
        OutStack.Add(MakeLatticeCorridorSource(P, Seed));
        OutStack.Add(MakeSdfRoughnessMod(P.SurfaceRoughness, RoughFrequency, RoughOctaves, RoughApplyWithin));
        OutStack.Add(MakeSdfCarve(CarveBlend, P.BaseDensity));

        if (bAppendStructuralPosts)
        {
            OutStack.AppendStructuralPost(P.StrateTopWorldZ, P.StrateBottomWorldZ,
                                          P.BoundarySealThickness, P.BaseDensity,
                                          SpineRadius, StrateManager);
        }
    }
}

namespace VoxelStrateRecipePrivate
{
    const FStrateGenerationParams* Tunnel(const FVoxelStrateArchetypeParams& Params)
    {
        return &Params.TunnelNetworkParams;
    }

    const FSlabGenerationParams* Slab(const FVoxelStrateArchetypeParams& Params)
    {
        return &Params.SlabParams;
    }

    const FMazeGenerationParams* Maze(const FVoxelStrateArchetypeParams& Params)
    {
        return &Params.MazeParams;
    }

    const FSurfaceGenerationParams* Surface(const FVoxelStrateArchetypeParams& Params)
    {
        return &Params.SurfaceParams;
    }

    const FVerticalShaftParams* Shaft(const FVoxelStrateArchetypeParams& Params)
    {
        return &Params.VerticalShaftParams;
    }

    const FFloatingIslandParams* Island(const FVoxelStrateArchetypeParams& Params)
    {
        return &Params.FloatingIslandParams;
    }

    float BaseDensity(const FVoxelStrateArchetypeParams& Params, EVoxelStrateParamBlock Block)
    {
        switch (Block)
        {
        case EVoxelStrateParamBlock::TunnelNetwork: return Tunnel(Params)->BaseDensity;
        case EVoxelStrateParamBlock::Slab:          return Slab(Params)->BaseDensity;
        case EVoxelStrateParamBlock::Maze:          return Maze(Params)->BaseDensity;
        case EVoxelStrateParamBlock::Surface:       return Surface(Params)->BaseDensity;
        case EVoxelStrateParamBlock::VerticalShaft: return Shaft(Params)->BaseDensity;
        case EVoxelStrateParamBlock::FloatingIsland: return Island(Params)->BaseDensity;
        default:                                    return 8.0f;
        }
    }

    float BoundarySeal(const FVoxelStrateArchetypeParams& Params, EVoxelStrateParamBlock Block)
    {
        switch (Block)
        {
        case EVoxelStrateParamBlock::TunnelNetwork: return Tunnel(Params)->BoundarySealThickness;
        case EVoxelStrateParamBlock::Slab:          return Slab(Params)->BoundarySealThickness;
        case EVoxelStrateParamBlock::Maze:          return Maze(Params)->BoundarySealThickness;
        case EVoxelStrateParamBlock::Surface:       return Surface(Params)->BoundarySealThickness;
        case EVoxelStrateParamBlock::VerticalShaft: return Shaft(Params)->BoundarySealThickness;
        case EVoxelStrateParamBlock::FloatingIsland: return Island(Params)->BoundarySealThickness;
        default:                                    return 4.0f;
        }
    }

    float Top(const FVoxelStrateArchetypeParams& Params, EVoxelStrateParamBlock Block)
    {
        switch (Block)
        {
        case EVoxelStrateParamBlock::TunnelNetwork: return Tunnel(Params)->StrateTopWorldZ;
        case EVoxelStrateParamBlock::Slab:          return Slab(Params)->StrateTopWorldZ;
        case EVoxelStrateParamBlock::Maze:          return Maze(Params)->StrateTopWorldZ;
        case EVoxelStrateParamBlock::Surface:       return Surface(Params)->StrateTopWorldZ;
        case EVoxelStrateParamBlock::VerticalShaft: return Shaft(Params)->StrateTopWorldZ;
        case EVoxelStrateParamBlock::FloatingIsland: return Island(Params)->StrateTopWorldZ;
        default:                                    return 0.0f;
        }
    }

    float Bottom(const FVoxelStrateArchetypeParams& Params, EVoxelStrateParamBlock Block)
    {
        switch (Block)
        {
        case EVoxelStrateParamBlock::TunnelNetwork: return Tunnel(Params)->StrateBottomWorldZ;
        case EVoxelStrateParamBlock::Slab:          return Slab(Params)->StrateBottomWorldZ;
        case EVoxelStrateParamBlock::Maze:          return Maze(Params)->StrateBottomWorldZ;
        case EVoxelStrateParamBlock::Surface:       return Surface(Params)->StrateBottomWorldZ;
        case EVoxelStrateParamBlock::VerticalShaft: return Shaft(Params)->StrateBottomWorldZ;
        case EVoxelStrateParamBlock::FloatingIsland: return Island(Params)->StrateBottomWorldZ;
        default:                                    return 0.0f;
        }
    }

    EVoxelStrateParamBlock ShapeBlock(EVoxelStrateOpClass OpClass)
    {
        switch (OpClass)
        {
        case EVoxelStrateOpClass::RoomGraphSource:     return EVoxelStrateParamBlock::TunnelNetwork;
        case EVoxelStrateOpClass::LatticeCorridorSource:
        case EVoxelStrateOpClass::NoiseRibbonSource:   return EVoxelStrateParamBlock::Maze;
        case EVoxelStrateOpClass::ShaftFieldSource:    return EVoxelStrateParamBlock::VerticalShaft;
        case EVoxelStrateOpClass::IslandBlobSource:    return EVoxelStrateParamBlock::FloatingIsland;
        default:                                       return EVoxelStrateParamBlock::None;
        }
    }

    EVoxelStrateParamBlock FixedParamBlock(EVoxelStrateOpClass OpClass)
    {
        switch (OpClass)
        {
        case EVoxelStrateOpClass::GridColumnMod:
            return EVoxelStrateParamBlock::Slab;
        case EVoxelStrateOpClass::CaveRoughnessMod:
        case EVoxelStrateOpClass::CaveTerraceMod:
        case EVoxelStrateOpClass::LayerLineMod:
        case EVoxelStrateOpClass::RibbingMod:
        case EVoxelStrateOpClass::CaveOverhangMod:
        case EVoxelStrateOpClass::CaveCliffMod:
        case EVoxelStrateOpClass::ScallopMod:
        case EVoxelStrateOpClass::CaveArchMod:
        case EVoxelStrateOpClass::RoomColumnMod:
        case EVoxelStrateOpClass::DomeMod:
        case EVoxelStrateOpClass::PinchMod:
        case EVoxelStrateOpClass::FloorBiasMod:
        case EVoxelStrateOpClass::WormFieldSource:
            return EVoxelStrateParamBlock::TunnelNetwork;
        case EVoxelStrateOpClass::ShaftLedgeMod:
            return EVoxelStrateParamBlock::VerticalShaft;
        default:
            return EVoxelStrateParamBlock::None;
        }
    }

    float SdfBlend(const FVoxelStrateArchetypeParams& Params, EVoxelStrateParamBlock Block)
    {
        switch (Block)
        {
        case EVoxelStrateParamBlock::TunnelNetwork:
            return Params.TunnelNetworkParams.SDFBlendRadius;
        case EVoxelStrateParamBlock::Maze:
            return 2.0f;
        case EVoxelStrateParamBlock::VerticalShaft:
            return 2.0f;
        case EVoxelStrateParamBlock::FloatingIsland:
            return FMath::Max(Params.FloatingIslandParams.SDFBlendRadius, 0.01f);
        default:
            return 2.0f;
        }
    }

    float SdfRoughnessStrength(const FVoxelStrateArchetypeParams& Params,
                               EVoxelStrateParamBlock Block)
    {
        switch (Block)
        {
        case EVoxelStrateParamBlock::TunnelNetwork: return Params.TunnelNetworkParams.SurfaceRoughness;
        case EVoxelStrateParamBlock::Maze:          return Params.MazeParams.SurfaceRoughness;
        case EVoxelStrateParamBlock::VerticalShaft: return Params.VerticalShaftParams.SurfaceRoughness;
        case EVoxelStrateParamBlock::FloatingIsland: return Params.FloatingIslandParams.SurfaceRoughness;
        default:                                    return 2.0f;
        }
    }

    float SdfRoughnessFrequency(const FVoxelStrateArchetypeParams& Params,
                                EVoxelStrateParamBlock Block)
    {
        switch (Block)
        {
        case EVoxelStrateParamBlock::TunnelNetwork: return Params.TunnelNetworkParams.RoughnessFrequency;
        case EVoxelStrateParamBlock::Maze:          return 0.12f;
        case EVoxelStrateParamBlock::VerticalShaft: return 0.1f;
        case EVoxelStrateParamBlock::FloatingIsland: return 0.08f;
        default:                                    return 0.1f;
        }
    }

    int32 SdfRoughnessOctaves(EVoxelStrateParamBlock Block)
    {
        return Block == EVoxelStrateParamBlock::FloatingIsland ? 4 : 3;
    }

    float SdfRoughnessWindow(const FVoxelStrateArchetypeParams& Params,
                             EVoxelStrateParamBlock Block)
    {
        const float Strength = SdfRoughnessStrength(Params, Block);
        switch (Block)
        {
        case EVoxelStrateParamBlock::TunnelNetwork:
            return Params.TunnelNetworkParams.SDFBlendRadius * 3.0f + Strength + 2.0f;
        case EVoxelStrateParamBlock::Maze:
            return FMath::Max(Params.MazeParams.CorridorRadius, 0.5f) + Strength + 2.0f;
        case EVoxelStrateParamBlock::VerticalShaft:
            return Strength + 4.0f;
        case EVoxelStrateParamBlock::FloatingIsland:
            return Strength + SdfBlend(Params, Block) + 2.0f;
        default:
            return Strength + 4.0f;
        }
    }

    float GenericStrength(const FVoxelStrateArchetypeParams& Params,
                           EVoxelStrateParamBlock Block)
    {
        return FMath::Max(0.25f, 0.35f * FMath::Abs(BaseDensity(Params, Block)));
    }

    float GenericFrequency(const FVoxelStrateArchetypeParams& Params,
                           EVoxelStrateParamBlock Block)
    {
        switch (Block)
        {
        case EVoxelStrateParamBlock::Maze:
            return 1.0f / FMath::Max(Params.MazeParams.CellSize, 8.0f);
        case EVoxelStrateParamBlock::VerticalShaft:
            return 0.01f;
        case EVoxelStrateParamBlock::FloatingIsland:
            return 0.02f;
        case EVoxelStrateParamBlock::TunnelNetwork:
            return FMath::Max(Params.TunnelNetworkParams.RoughnessFrequency, 0.001f);
        default:
            return 0.02f;
        }
    }

    bool IsShapeSource(EVoxelStrateOpClass OpClass)
    {
        switch (OpClass)
        {
        case EVoxelStrateOpClass::RoomGraphSource:
        case EVoxelStrateOpClass::LatticeCorridorSource:
        case EVoxelStrateOpClass::ShaftFieldSource:
        case EVoxelStrateOpClass::IslandBlobSource:
        case EVoxelStrateOpClass::NoiseRibbonSource:
            return true;
        default:
            return false;
        }
    }

    bool ValidateRecipeContracts(const FVoxelOpStackRecipe& Recipe, FString& OutError)
    {
        TArray<EVoxelStrateOpClass> Classes;
        Classes.Reserve(3 + Recipe.Modifiers.Num());
        Classes.Add(Recipe.Root.OpClass);
        Classes.Add(Recipe.ShapeSource.OpClass);
        Classes.Add(Recipe.Conversion.OpClass);
        for (const FVoxelOpRecipeEntry& Entry : Recipe.Modifiers)
        {
            Classes.Add(Entry.OpClass);
        }

        auto ValidateParamBlock = [&](const FVoxelOpRecipeEntry& Entry, int32 EntryIndex)
        {
            if (Entry.ParamBlock == EVoxelStrateParamBlock::None)
            {
                OutError = FString::Printf(TEXT("recipe op %d has no parameter block"), EntryIndex);
                return false;
            }
            const EVoxelStrateParamBlock Fixed = FixedParamBlock(Entry.OpClass);
            if (Fixed != EVoxelStrateParamBlock::None && Entry.ParamBlock != Fixed)
            {
                OutError = FString::Printf(TEXT("recipe op %d uses the wrong parameter block"), EntryIndex);
                return false;
            }
            return true;
        };
        if (!ValidateParamBlock(Recipe.Root, 0)
            || !ValidateParamBlock(Recipe.ShapeSource, 1)
            || !ValidateParamBlock(Recipe.Conversion, 2))
        {
            return false;
        }
        for (int32 ModifierIndex = 0; ModifierIndex < Recipe.Modifiers.Num(); ++ModifierIndex)
        {
            if (!ValidateParamBlock(Recipe.Modifiers[ModifierIndex], 3 + ModifierIndex))
            {
                return false;
            }
        }

        int32 LastWriter[2] = { INDEX_NONE, INDEX_NONE };
        EVoxelOpResourceMask AvailableResources = VoxelOpResources::None;
        const EVoxelOpChannelMask Channels[] = { VoxelOpChannels::Density, VoxelOpChannels::Sdf };
        auto ChannelIndex = [](EVoxelOpChannelMask Channel) { return Channel == VoxelOpChannels::Density ? 0 : 1; };

        for (int32 Index = 0; Index < Classes.Num(); ++Index)
        {
            FVoxelStrateOpContract Contract;
            if (!VoxelDensityOps::GetStrateOpContract(Classes[Index], Contract))
            {
                OutError = FString::Printf(TEXT("recipe op %d has no declaration"), Index);
                return false;
            }
            if ((Contract.Reads & VoxelOpChannels::All) != Contract.Reads
                || (Contract.Writes & VoxelOpChannels::All) != Contract.Writes
                || (Contract.RequiredResources & VoxelOpResources::All) != Contract.RequiredResources
                || (Contract.ProvidedResources & VoxelOpResources::All) != Contract.ProvidedResources)
            {
                OutError = FString::Printf(TEXT("recipe op %d has an unknown declaration bit"), Index);
                return false;
            }
            if ((Contract.RequiredResources
                 & static_cast<EVoxelOpResourceMask>(~AvailableResources)) != 0)
            {
                OutError = FString::Printf(TEXT("recipe op %d requires unavailable state"), Index);
                return false;
            }
            if (Contract.bAdditive && Contract.Writes == VoxelOpChannels::None)
            {
                OutError = FString::Printf(TEXT("recipe op %d is additive but writes no channel"), Index);
                return false;
            }
            if (Contract.bAdditive
                && (Contract.Writes & static_cast<EVoxelOpChannelMask>(~Contract.Reads)) != 0)
            {
                OutError = FString::Printf(TEXT("recipe op %d is additive without reading its writes"), Index);
                return false;
            }
            for (const EVoxelOpChannelMask Channel : Channels)
            {
                if ((Contract.Reads & Channel) == 0) { continue; }
                const int32 CI = ChannelIndex(Channel);
                const bool bRootIdentity = Contract.Role == EVoxelOpRole::FieldSource
                    && (Contract.Writes & Channel) != 0 && LastWriter[CI] == INDEX_NONE;
                if (LastWriter[CI] == INDEX_NONE && !bRootIdentity)
                {
                    OutError = FString::Printf(TEXT("recipe op %d reads an unpublished channel"), Index);
                    return false;
                }
            }
            for (const EVoxelOpChannelMask Channel : Channels)
            {
                if ((Contract.Writes & Channel) == 0 || (Contract.Reads & Channel) != 0) { continue; }
                if (LastWriter[ChannelIndex(Channel)] != INDEX_NONE)
                {
                    OutError = FString::Printf(TEXT("recipe op %d clobbers a channel"), Index);
                    return false;
                }
            }
            for (const EVoxelOpChannelMask Channel : Channels)
            {
                if ((Contract.Writes & Channel) != 0)
                {
                    LastWriter[ChannelIndex(Channel)] = Index;
                }
            }
            AvailableResources |= Contract.ProvidedResources;
        }
        return true;
    }

    TUniquePtr<IVoxelDensityOp> BuildRecipeOp(const FVoxelOpRecipeEntry& Entry,
                                              const FVoxelStrateArchetypeParams& Params,
                                              int32 Seed,
                                              float SpineRadius,
                                              const UVoxelStrateManager* StrateManager,
                                              const FRoomGraphSource*& OutRoom,
                                              const FShaftFieldSource*& OutShaft)
    {
        const EVoxelStrateParamBlock Block = Entry.ParamBlock;
        switch (Entry.OpClass)
        {
        case EVoxelStrateOpClass::ConstantRockSource:
            return VoxelDensityOps::MakeConstantRockSource(BaseDensity(Params, Block));
        case EVoxelStrateOpClass::ConstantVoidSource:
            return VoxelDensityOps::MakeConstantVoidSource(BaseDensity(Params, Block));
        case EVoxelStrateOpClass::RoomGraphSource:
        {
            TUniquePtr<FRoomGraphSource> Op = MakeUnique<FRoomGraphSource>(*Tunnel(Params), Seed, StrateManager);
            OutRoom = Op.Get();
            return Op;
        }
        case EVoxelStrateOpClass::LatticeCorridorSource:
            return MakeUnique<FLatticeCorridorSource>(*Maze(Params), Seed);
        case EVoxelStrateOpClass::ShaftFieldSource:
        {
            TUniquePtr<FShaftFieldSource> Op = MakeUnique<FShaftFieldSource>(*Shaft(Params), Seed, SpineRadius);
            OutShaft = Op.Get();
            return Op;
        }
        case EVoxelStrateOpClass::IslandBlobSource:
            return MakeUnique<FIslandBlobSource>(*Island(Params), Seed);
        case EVoxelStrateOpClass::NoiseRibbonSource:
            return MakeUnique<FNoiseRibbonSource>(*Maze(Params), Seed);
        case EVoxelStrateOpClass::SdfRoughnessMod:
            return VoxelDensityOps::MakeSdfRoughnessMod(
                SdfRoughnessStrength(Params, Block),
                SdfRoughnessFrequency(Params, Block),
                SdfRoughnessOctaves(Block),
                SdfRoughnessWindow(Params, Block));
        case EVoxelStrateOpClass::SdfCarve:
            return VoxelDensityOps::MakeSdfCarve(SdfBlend(Params, Block), BaseDensity(Params, Block));
        case EVoxelStrateOpClass::SdfFill:
            return VoxelDensityOps::MakeSdfFill(SdfBlend(Params, Block), BaseDensity(Params, Block));
        case EVoxelStrateOpClass::GridColumnMod:
            return MakeUnique<FGridColumnMod>(*Slab(Params), Seed);
        case EVoxelStrateOpClass::CaveRoughnessMod:
            return MakeUnique<FCaveRoughnessMod>(*Tunnel(Params), Seed);
        case EVoxelStrateOpClass::CaveTerraceMod:
            return MakeUnique<FCaveTerraceMod>(*Tunnel(Params), Seed, OutRoom);
        case EVoxelStrateOpClass::LayerLineMod:
            return MakeUnique<FLayerLineMod>(*Tunnel(Params), OutRoom);
        case EVoxelStrateOpClass::RibbingMod:
            return MakeUnique<FRibbingMod>(*Tunnel(Params), OutRoom);
        case EVoxelStrateOpClass::CaveOverhangMod:
            return MakeUnique<FCaveOverhangMod>(*Tunnel(Params), Seed, OutRoom);
        case EVoxelStrateOpClass::CaveCliffMod:
            return MakeUnique<FCaveCliffMod>(*Tunnel(Params), Seed, OutRoom);
        case EVoxelStrateOpClass::ScallopMod:
            return MakeUnique<FScallopMod>(*Tunnel(Params), Seed, OutRoom);
        case EVoxelStrateOpClass::CaveArchMod:
            return MakeUnique<FCaveArchMod>(*Tunnel(Params), OutRoom);
        case EVoxelStrateOpClass::RoomColumnMod:
            return MakeUnique<FRoomColumnMod>(*Tunnel(Params), OutRoom);
        case EVoxelStrateOpClass::DomeMod:
            return MakeUnique<FDomeMod>(*Tunnel(Params), OutRoom);
        case EVoxelStrateOpClass::PinchMod:
            return MakeUnique<FPinchMod>(*Tunnel(Params), OutRoom);
        case EVoxelStrateOpClass::FloorBiasMod:
            return MakeUnique<FFloorBiasMod>(*Tunnel(Params), OutRoom);
        case EVoxelStrateOpClass::WormFieldSource:
            return MakeUnique<FWormFieldSource>(*Tunnel(Params), Seed);
        case EVoxelStrateOpClass::ShaftLedgeMod:
            return MakeUnique<FShaftLedgeMod>(*Shaft(Params), OutShaft);
        case EVoxelStrateOpClass::DensityNoiseCarveMod:
            return MakeUnique<FDensityNoiseMod>(GenericStrength(Params, Block),
                                                GenericFrequency(Params, Block), 3, Seed, false);
        case EVoxelStrateOpClass::DensityNoiseFillMod:
            return MakeUnique<FDensityNoiseMod>(GenericStrength(Params, Block),
                                                GenericFrequency(Params, Block), 3, Seed, true);
        default:
            return nullptr;
        }
    }
}

bool VF_BuildStackFromRecipe(const FVoxelOpStackRecipe& Recipe,
                             const FVoxelStrateArchetypeParams& Params,
                             int32 Seed, float SpineRadius,
                             const UVoxelStrateManager* StrateManager,
                             FVoxelOpStack& OutStack,
                             FVoxelOpContext& OutContext,
                             FString* OutError,
                             bool bAppendStructuralPosts)
{
    auto Fail = [&](const FString& Reason) -> bool
    {
        if (OutError != nullptr) { *OutError = Reason; }
        return false;
    };
    if (OutError != nullptr) { OutError->Reset(); }

    if (Recipe.Modifiers.Num() < 4 || Recipe.Modifiers.Num() > 8)
    {
        return Fail(TEXT("A structure recipe must draw between 4 and 8 modifiers."));
    }
    const EVoxelStrateOpClass ExpectedRoot = Recipe.RootPolarity == EVoxelStrateRootPolarity::VoidFill
        ? EVoxelStrateOpClass::ConstantVoidSource : EVoxelStrateOpClass::ConstantRockSource;
    const EVoxelStrateOpClass ExpectedConversion = Recipe.RootPolarity == EVoxelStrateRootPolarity::VoidFill
        ? EVoxelStrateOpClass::SdfFill : EVoxelStrateOpClass::SdfCarve;
    if (Recipe.Root.OpClass != ExpectedRoot || Recipe.Conversion.OpClass != ExpectedConversion)
    {
        return Fail(TEXT("Recipe polarity does not match its root/conversion ids."));
    }
    if (!VoxelStrateRecipePrivate::IsShapeSource(Recipe.ShapeSource.OpClass))
    {
        return Fail(TEXT("Recipe shape source is not a rollable SDF source."));
    }
    const EVoxelStrateParamBlock ShapeBlock =
        VoxelStrateRecipePrivate::ShapeBlock(Recipe.ShapeSource.OpClass);
    if (ShapeBlock == EVoxelStrateParamBlock::None
        || Recipe.StructuralParamBlock != ShapeBlock
        || Recipe.Root.ParamBlock == EVoxelStrateParamBlock::None
        || Recipe.ShapeSource.ParamBlock != ShapeBlock
        || Recipe.Conversion.ParamBlock != ShapeBlock)
    {
        return Fail(TEXT("Recipe parameter blocks do not match its shape source."));
    }

    FString ContractError;
    if (!VoxelStrateRecipePrivate::ValidateRecipeContracts(Recipe, ContractError))
    {
        return Fail(ContractError);
    }

    FVoxelOpStack Candidate;
    const FRoomGraphSource* RoomPtr = nullptr;
    const FShaftFieldSource* ShaftPtr = nullptr;

    FVoxelOpRecipeEntry Root = Recipe.Root;
    TUniquePtr<IVoxelDensityOp> RootOp =
        VoxelStrateRecipePrivate::BuildRecipeOp(Root, Params, Seed, SpineRadius, StrateManager, RoomPtr, ShaftPtr);
    if (!RootOp) { return Fail(TEXT("Recipe root could not be materialised.")); }
    Candidate.Add(MoveTemp(RootOp));

    TUniquePtr<IVoxelDensityOp> ShapeOp =
        VoxelStrateRecipePrivate::BuildRecipeOp(Recipe.ShapeSource, Params, Seed, SpineRadius,
                                                StrateManager, RoomPtr, ShaftPtr);
    if (!ShapeOp) { return Fail(TEXT("Recipe shape source could not be materialised.")); }
    Candidate.Add(MoveTemp(ShapeOp));

    TUniquePtr<IVoxelDensityOp> ConversionOp =
        VoxelStrateRecipePrivate::BuildRecipeOp(Recipe.Conversion, Params, Seed, SpineRadius,
                                                StrateManager, RoomPtr, ShaftPtr);
    if (!ConversionOp) { return Fail(TEXT("Recipe conversion could not be materialised.")); }
    Candidate.Add(MoveTemp(ConversionOp));

    for (const FVoxelOpRecipeEntry& Entry : Recipe.Modifiers)
    {
        if (Entry.OpClass == EVoxelStrateOpClass::WormFieldSource
            && !(Params.TunnelNetworkParams.WormStrength > 0.0f
                 && Params.TunnelNetworkParams.WormThreshold > 0.0f))
        {
            // A disabled strate has no worm operator at all. This makes the off switch cheap on
            // the interpreted path as well as exact in its interval/classifier fold.
            continue;
        }
        TUniquePtr<IVoxelDensityOp> Modifier =
            VoxelStrateRecipePrivate::BuildRecipeOp(Entry, Params, Seed, SpineRadius,
                                                    StrateManager, RoomPtr, ShaftPtr);
        if (!Modifier)
        {
            return Fail(TEXT("Recipe modifier could not be materialised."));
        }
        Candidate.Add(MoveTemp(Modifier));
    }

    const float Top = VoxelStrateRecipePrivate::Top(Params, Recipe.StructuralParamBlock);
    const float Bottom = VoxelStrateRecipePrivate::Bottom(Params, Recipe.StructuralParamBlock);
    const float Seal = VoxelStrateRecipePrivate::BoundarySeal(Params, Recipe.StructuralParamBlock);
    const float Base = VoxelStrateRecipePrivate::BaseDensity(Params, Recipe.StructuralParamBlock);
    if (!VoxelMath::IsFinite(Top) || !VoxelMath::IsFinite(Bottom) || Top <= Bottom
        || !VoxelMath::IsFinite(Seal) || !VoxelMath::IsFinite(Base))
    {
        return Fail(TEXT("Recipe structural parameters do not define a finite positive strate."));
    }

    OutContext = FVoxelOpContext();
    OutContext.Seed = Seed;
    OutContext.LayoutVersion = StrateManager != nullptr ? StrateManager->GetLayoutVersion() : 0;
    OutContext.WorldRadiusVoxels = 0.0f;
    OutContext.EdgeSealThickness = Seal;
    OutContext.StrateTopWorldZ = Top;
    OutContext.StrateBottomWorldZ = Bottom;

    // This is the only place a recipe can acquire world-law operators. There is no recipe field
    // for posts, so a serialised manifest cannot forget them or reorder them. Lateral callers
    // materialise this same creative recipe with posts disabled, then append the global post set
    // to the parent stack below.
    if (bAppendStructuralPosts)
    {
        Candidate.AppendStructuralPost(Top, Bottom, Seal, Base, SpineRadius, StrateManager);
    }

    FString ValidationError;
    if (!Candidate.ValidateChannelOrder(&ValidationError))
    {
        return Fail(FString::Printf(TEXT("Materialised recipe failed ValidateChannelOrder: %s"),
                                    *ValidationError));
    }

    OutStack = MoveTemp(Candidate);
    return true;
}

#if WITH_EDITOR
namespace
{
    static EVoxelStrateParamBlock VF_RegionParamBlock(ECaveGeneratorType Archetype)
    {
        switch (Archetype)
        {
        case ECaveGeneratorType::FlatPlain:
        case ECaveGeneratorType::CrystalChamber: return EVoxelStrateParamBlock::Slab;
        case ECaveGeneratorType::Maze:            return EVoxelStrateParamBlock::Maze;
        case ECaveGeneratorType::SurfaceWorld:   return EVoxelStrateParamBlock::Surface;
        case ECaveGeneratorType::VerticalShafts: return EVoxelStrateParamBlock::VerticalShaft;
        case ECaveGeneratorType::FloatingIslands:return EVoxelStrateParamBlock::FloatingIsland;
        case ECaveGeneratorType::Underwater:
        case ECaveGeneratorType::TunnelNetwork:
        default:                                  return EVoxelStrateParamBlock::TunnelNetwork;
        }
    }

    static float VF_RegionTop(const FVoxelStrateArchetypeParams& Params,
                              EVoxelStrateParamBlock Block)
    {
        switch (Block)
        {
        case EVoxelStrateParamBlock::Slab:           return Params.SlabParams.StrateTopWorldZ;
        case EVoxelStrateParamBlock::Maze:           return Params.MazeParams.StrateTopWorldZ;
        case EVoxelStrateParamBlock::Surface:        return Params.SurfaceParams.StrateTopWorldZ;
        case EVoxelStrateParamBlock::VerticalShaft:  return Params.VerticalShaftParams.StrateTopWorldZ;
        case EVoxelStrateParamBlock::FloatingIsland: return Params.FloatingIslandParams.StrateTopWorldZ;
        case EVoxelStrateParamBlock::TunnelNetwork:
        default:                                     return Params.TunnelNetworkParams.StrateTopWorldZ;
        }
    }

    static float VF_RegionBottom(const FVoxelStrateArchetypeParams& Params,
                                 EVoxelStrateParamBlock Block)
    {
        switch (Block)
        {
        case EVoxelStrateParamBlock::Slab:           return Params.SlabParams.StrateBottomWorldZ;
        case EVoxelStrateParamBlock::Maze:           return Params.MazeParams.StrateBottomWorldZ;
        case EVoxelStrateParamBlock::Surface:        return Params.SurfaceParams.StrateBottomWorldZ;
        case EVoxelStrateParamBlock::VerticalShaft:  return Params.VerticalShaftParams.StrateBottomWorldZ;
        case EVoxelStrateParamBlock::FloatingIsland: return Params.FloatingIslandParams.StrateBottomWorldZ;
        case EVoxelStrateParamBlock::TunnelNetwork:
        default:                                     return Params.TunnelNetworkParams.StrateBottomWorldZ;
        }
    }

    static float VF_RegionSeal(const FVoxelStrateArchetypeParams& Params,
                               EVoxelStrateParamBlock Block)
    {
        switch (Block)
        {
        case EVoxelStrateParamBlock::Slab:           return Params.SlabParams.BoundarySealThickness;
        case EVoxelStrateParamBlock::Maze:           return Params.MazeParams.BoundarySealThickness;
        case EVoxelStrateParamBlock::Surface:        return Params.SurfaceParams.BoundarySealThickness;
        case EVoxelStrateParamBlock::VerticalShaft:  return Params.VerticalShaftParams.BoundarySealThickness;
        case EVoxelStrateParamBlock::FloatingIsland: return Params.FloatingIslandParams.BoundarySealThickness;
        case EVoxelStrateParamBlock::TunnelNetwork:
        default:                                     return Params.TunnelNetworkParams.BoundarySealThickness;
        }
    }

    static float VF_RegionBase(const FVoxelStrateArchetypeParams& Params,
                               EVoxelStrateParamBlock Block)
    {
        switch (Block)
        {
        case EVoxelStrateParamBlock::Slab:           return Params.SlabParams.BaseDensity;
        case EVoxelStrateParamBlock::Maze:           return Params.MazeParams.BaseDensity;
        case EVoxelStrateParamBlock::Surface:        return Params.SurfaceParams.BaseDensity;
        case EVoxelStrateParamBlock::VerticalShaft:  return Params.VerticalShaftParams.BaseDensity;
        case EVoxelStrateParamBlock::FloatingIsland: return Params.FloatingIslandParams.BaseDensity;
        case EVoxelStrateParamBlock::TunnelNetwork:
        default:                                     return Params.TunnelNetworkParams.BaseDensity;
        }
    }

    static bool VF_BuildNativeRegionCore(
        const FVoxelStrateRegion& Region,
        const FVoxelStrateArchetypeParams& Params,
        float SpineRadius,
        const UVoxelStrateManager* StrateManager,
        bool bAppendStructuralPosts,
        FVoxelOpStack& OutStack,
        FVoxelOpContext& OutContext)
    {
        switch (Region.Archetype)
        {
        case ECaveGeneratorType::FlatPlain:
        case ECaveGeneratorType::CrystalChamber:
            if (Params.SlabParams.StrateTopWorldZ - Params.SlabParams.StrateBottomWorldZ <= 0.0f)
            {
                return false;
            }
            OutContext.StrateTopWorldZ = Params.SlabParams.StrateTopWorldZ;
            OutContext.StrateBottomWorldZ = Params.SlabParams.StrateBottomWorldZ;
            VoxelDensityOps::BuildSlabStack(OutStack, Params.SlabParams, Region.Seed,
                                            SpineRadius, StrateManager, bAppendStructuralPosts);
            return true;
        case ECaveGeneratorType::Maze:
            if (Params.MazeParams.StrateTopWorldZ - Params.MazeParams.StrateBottomWorldZ <= 0.0f)
            {
                return false;
            }
            OutContext.StrateTopWorldZ = Params.MazeParams.StrateTopWorldZ;
            OutContext.StrateBottomWorldZ = Params.MazeParams.StrateBottomWorldZ;
            VoxelDensityOps::BuildMazeStack(OutStack, Params.MazeParams, Region.Seed,
                                            SpineRadius, StrateManager, bAppendStructuralPosts);
            return true;
        case ECaveGeneratorType::SurfaceWorld:
            if (Params.SurfaceParams.StrateTopWorldZ - Params.SurfaceParams.StrateBottomWorldZ <= 0.0f)
            {
                return false;
            }
            OutContext.StrateTopWorldZ = Params.SurfaceParams.StrateTopWorldZ;
            OutContext.StrateBottomWorldZ = Params.SurfaceParams.StrateBottomWorldZ;
            VoxelDensityOps::BuildSurfaceStack(
                OutStack, Params.SurfaceParams, Region.Seed, SpineRadius, StrateManager,
                TArray<FSurfaceGenerationParams>(), nullptr, bAppendStructuralPosts);
            return true;
        case ECaveGeneratorType::VerticalShafts:
            if (Params.VerticalShaftParams.StrateTopWorldZ
                - Params.VerticalShaftParams.StrateBottomWorldZ <= 0.0f)
            {
                return false;
            }
            OutContext.StrateTopWorldZ = Params.VerticalShaftParams.StrateTopWorldZ;
            OutContext.StrateBottomWorldZ = Params.VerticalShaftParams.StrateBottomWorldZ;
            VoxelDensityOps::BuildVerticalShaftStack(
                OutStack, Params.VerticalShaftParams, Region.Seed,
                SpineRadius, StrateManager, bAppendStructuralPosts);
            return true;
        case ECaveGeneratorType::FloatingIslands:
            if (Params.FloatingIslandParams.StrateTopWorldZ
                - Params.FloatingIslandParams.StrateBottomWorldZ <= 0.0f)
            {
                return false;
            }
            OutContext.StrateTopWorldZ = Params.FloatingIslandParams.StrateTopWorldZ;
            OutContext.StrateBottomWorldZ = Params.FloatingIslandParams.StrateBottomWorldZ;
            VoxelDensityOps::BuildFloatingIslandStack(
                OutStack, Params.FloatingIslandParams, Region.Seed,
                SpineRadius, StrateManager, bAppendStructuralPosts);
            return true;
        case ECaveGeneratorType::Underwater:
        case ECaveGeneratorType::TunnelNetwork:
            OutContext.StrateTopWorldZ = Params.TunnelNetworkParams.StrateTopWorldZ;
            OutContext.StrateBottomWorldZ = Params.TunnelNetworkParams.StrateBottomWorldZ;
            VoxelDensityOps::BuildTunnelNetworkStack(
                OutStack, Params.TunnelNetworkParams, Region.Seed,
                SpineRadius, StrateManager, bAppendStructuralPosts);
            return true;
        default:
            return false;
        }
    }
}

bool VF_BuildStrateRegionStack(
    const FVoxelStrateRegionManifest& Manifest,
    float SpineRadius,
    const UVoxelStrateManager* StrateManager,
    FVoxelOpStack& OutStack,
    FVoxelOpContext& OutContext,
    FString* OutError)
{
    auto Fail = [&](const FString& Reason) -> bool
    {
        if (OutError != nullptr) { *OutError = Reason; }
        return false;
    };
    if (OutError != nullptr) { OutError->Reset(); }
    if (!Manifest.IsValid())
    {
        return Fail(Manifest.FailureReason.IsEmpty()
            ? TEXT("lateral region manifest is invalid") : Manifest.FailureReason);
    }
    if (Manifest.PartitionSeed != VF_GetStrateRegionPartitionSeed(
            Manifest.Seed, Manifest.StrateIndex))
    {
        return Fail(TEXT("lateral region partition seed is stale for its seed/strate index"));
    }

    EVoxelStrateParamBlock GlobalBlock = Manifest.StructuralParamBlock;
    if (GlobalBlock == EVoxelStrateParamBlock::None)
    {
        GlobalBlock = Manifest.Regions[0].bUsesRecipe
            ? Manifest.Regions[0].Recipe.StructuralParamBlock
            : VF_RegionParamBlock(Manifest.Regions[0].Archetype);
    }

    FVoxelStrateArchetypeParams FirstParams = Manifest.Regions[0].ArchetypeParams;
    float Top = Manifest.bHasGlobalStructuralParams
        ? Manifest.StrateTopWorldZ : VF_RegionTop(FirstParams, GlobalBlock);
    float Bottom = Manifest.bHasGlobalStructuralParams
        ? Manifest.StrateBottomWorldZ : VF_RegionBottom(FirstParams, GlobalBlock);
    float Seal = Manifest.bHasGlobalStructuralParams
        ? Manifest.BoundarySealThickness : VF_RegionSeal(FirstParams, GlobalBlock);
    float Base = Manifest.bHasGlobalStructuralParams
        ? Manifest.BaseDensity : VF_RegionBase(FirstParams, GlobalBlock);
    if (!VoxelMath::IsFinite(Top) || !VoxelMath::IsFinite(Bottom) || !(Top > Bottom)
        || !VoxelMath::IsFinite(Seal) || Seal < 0.0f
        || !VoxelMath::IsFinite(Base) || !(Base > 0.0f))
    {
        return Fail(TEXT("lateral region structural parameters are not a finite positive strate"));
    }

    auto MakeRegionParams = [&](const FVoxelStrateRegion& Region)
    {
        FVoxelStrateArchetypeParams Params = Region.ArchetypeParams;
        VF_SetStrateArchetypeRuntimeBounds(Params, Top, Bottom);
        return Params;
    };

    // The one-region form remains a normal stack.  It is useful to callers that consume a region
    // manifest, while production keeps its old branch entirely untouched for the hard identity
    // gate.
    if (Manifest.RegionCount == 1)
    {
        const FVoxelStrateRegion& Region = Manifest.Regions[0];
        const FVoxelStrateArchetypeParams Params = MakeRegionParams(Region);
        bool bBuilt = false;
        if (Region.bUsesRecipe)
        {
            bBuilt = VF_BuildStackFromRecipe(Region.Recipe, Params, Region.Seed,
                                              SpineRadius, StrateManager, OutStack,
                                              OutContext, OutError, true);
        }
        else
        {
            OutContext = FVoxelOpContext();
            OutContext.Seed = static_cast<uint32>(Region.Seed);
            OutContext.LayoutVersion = StrateManager != nullptr
                ? StrateManager->GetLayoutVersion() : 0;
            bBuilt = VF_BuildNativeRegionCore(Region, Params, SpineRadius, StrateManager,
                                              true, OutStack, OutContext);
            if (!bBuilt && OutError != nullptr)
            {
                *OutError = TEXT("single lateral region native stack could not be materialised");
            }
        }
        if (!bBuilt) { return false; }
        OutContext.WorldRadiusVoxels = 0.0f;
        OutContext.EdgeSealThickness = Seal;
        return true;
    }

    TArray<FVoxelOpStack> RegionStacks;
    RegionStacks.Reserve(Manifest.RegionCount);
    for (const FVoxelStrateRegion& Region : Manifest.Regions)
    {
        const FVoxelStrateArchetypeParams Params = MakeRegionParams(Region);
        FVoxelOpStack Core;
        FVoxelOpContext CoreContext;
        bool bBuilt = false;
        if (Region.bUsesRecipe)
        {
            bBuilt = VF_BuildStackFromRecipe(Region.Recipe, Params, Region.Seed,
                                              SpineRadius, StrateManager, Core,
                                              CoreContext, OutError, false);
        }
        else
        {
            CoreContext = FVoxelOpContext();
            CoreContext.Seed = static_cast<uint32>(Region.Seed);
            CoreContext.LayoutVersion = StrateManager != nullptr
                ? StrateManager->GetLayoutVersion() : 0;
            bBuilt = VF_BuildNativeRegionCore(Region, Params, SpineRadius, StrateManager,
                                              false, Core, CoreContext);
            if (!bBuilt && OutError != nullptr)
            {
                *OutError = FString::Printf(TEXT("region %d native stack could not be materialised"),
                                             Region.RegionIndex);
            }
        }
        if (!bBuilt)
        {
            return false;
        }
        RegionStacks.Add(MoveTemp(Core));
    }

    FVoxelOpStack Parent;
    Parent.Add(MakeUnique<FLateralRegionBlendOp>(Manifest, MoveTemp(RegionStacks)));
    Parent.AppendStructuralPost(Top, Bottom, Seal, Base, SpineRadius, StrateManager);

    FString ValidationError;
    if (!Parent.ValidateChannelOrder(&ValidationError))
    {
        return Fail(FString::Printf(TEXT("lateral region parent failed ValidateChannelOrder: %s"),
                                    *ValidationError));
    }

    OutContext = FVoxelOpContext();
    OutContext.Seed = static_cast<uint32>(Manifest.Seed);
    OutContext.LayoutVersion = StrateManager != nullptr
        ? StrateManager->GetLayoutVersion() : 0;
    OutContext.WorldRadiusVoxels = 0.0f;
    OutContext.EdgeSealThickness = Seal;
    OutContext.StrateTopWorldZ = Top;
    OutContext.StrateBottomWorldZ = Bottom;
    OutStack = MoveTemp(Parent);
    return true;
}
#endif // WITH_EDITOR — lateral regions remain behind the failed shippability gate
