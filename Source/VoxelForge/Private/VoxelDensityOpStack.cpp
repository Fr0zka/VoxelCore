// VoxelDensityOpStack.cpp
// Les opérateurs concrets de la Phase 1 : la décomposition de Maze + le post-traitement structurel.
// The concrete Phase 1 operators: the Maze decomposition + the structural post-process.
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
#include "VoxelCaveMorphology.h"      // VoxelSDF::Capsule, VoxelHash
#include "VoxelGenerator.h"           // VoxelGenLOD::Eff
#include "VoxelHeightOp.h"            // FVoxelHeightStack — SurfaceWorld's two height stacks
#include "VoxelNoise.h"               // VoxelNoise::FBM
#include "VoxelStrateDefinition.h"    // TerrainOperations — le pool que BuildChunkCache tire par salle
#include "VoxelTerrainOpDefinition.h" // ApplyTo — l'override d'op PAR SALLE (étape C1)
#include "VoxelStrateManager.h"       // EvaluateModifierSDF / AnyPassageNearBox
#include "VoxelTypes.h"               // SmoothStep01, VOXEL_NOISE_SCALE
#include "VoxelStats.h"

#include <atomic>                     // l'id d'instance non recyclé du mémo de colonne

namespace
{
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
     * ⚠️⚠️ **CORRIGÉ DE 2.0 À 1.5 LE 2026-07-28, ET CETTE CONSTANTE ÉTAIT LE TERME DOMINANT DE
     * TOUTE LA FONCTION PENDANT TROIS BUILDS.** À lire avant d'y retoucher.
     *
     * La dilatation vaut `CaveWarpStrength · VOXEL_NOISE_SCALE · CETTE BORNE`. Avec les défauts
     * (`CaveWarpStrength = 8`, `SCALE = 1.25`) elle valait **20 voxels** — appliquée des deux
     * côtés de chaque axe d'une tuile de **10 voxels**, soit une boîte de requête de 50 voxels,
     * **125× le volume de la tuile**. Trois passes de resserrement (le ver, les colonnes,
     * l'échantillonneur, la disjonction des tunnels) ont été faites AUTOUR de ce terme sans que
     * personne ne le mesure. Le test des tunnels, annoncé « un ordre de grandeur plus serré », ne
     * gagnait en pratique que 25 % — exactement parce que `BoxHalfDiag` était dominé par cette
     * dilatation et non par la géométrie.
     *
     * ⚠️ ET LE RESTE DU PLUGIN N'A JAMAIS ÉTÉ AUSSI PRUDENT : `BuildChunkCache` est appelée avec
     * `Expansion = CaveWarpStrength + 2` (ici comme dans `GetDensityWithParams`), ce qui suppose
     * `|Perlin3D| · SCALE ≤ CaveWarpStrength`, donc `|Perlin3D| ≤ 0.8`. Le code qui tourne en
     * production depuis toujours parie déjà là-dessus. Prendre 2.0 était 2,5× plus conservateur
     * que l'hypothèse dont dépend déjà la correction du cache.
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
     * Was 2.0, and that constant was the dominant term of this whole function for three builds:
     * it inflated a 10-voxel tile into a 50-voxel query box (125x the volume), which is why the
     * "order of magnitude tighter" tunnel test only won 25%. The rest of the plugin has always
     * assumed |Perlin3D| <= 0.8 (BuildChunkCache's Expansion = CaveWarpStrength + 2). 1.5 is
     * PROVED above from GradDot's two-distinct-axes form and the per-axis weighted bound of 0.5.
     */
    static constexpr float VF_PerlinAbsBound = 1.5f;

    /** La même enveloppe que `FractalNoise3D` de VoxelGenerator.cpp (qui y est `static`, donc
     *  invisible ici). Le détour par `FVector` est délibéré — voir l'en-tête de ce fichier. */
    FORCEINLINE float HFractal3D(const FVector& Position, int32 Octaves = 4,
                                 float Lacunarity = 2.0f, float Persistence = 0.5f)
    {
        return VoxelNoise::FBM((float)Position.X, (float)Position.Y, (float)Position.Z,
                               Octaves, Lacunarity, Persistence);
    }

    /** Idem pour `RidgedNoise3D` (également `static` dans VoxelGenerator.cpp). Le bruit cellulaire,
     *  lui, n'a pas besoin d'enveloppe : son corps a migré dans VoxelCaveMorphology.h et s'appelle
     *  `VoxelNoise::Cellular3D`, avec la MÊME signature `const FVector&` que l'original. */
    FORCEINLINE float HRidged3D(const FVector& Position, int32 Octaves = 4,
                                float Lacunarity = 2.0f, float Persistence = 0.5f)
    {
        return VoxelNoise::Ridged((float)Position.X, (float)Position.Y, (float)Position.Z,
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
    // d'optimiser. Noté dans OPSTACK-PROGRESS comme poste de perf, pas « corrigé » à l'aveugle.
    //
    // STAGE B5 DECISION: repeated early-out in each op, NOT a scoping container — the stack is a flat
    // list that ClassifyBox folds op by op, and an op that only exists inside a container is not
    // composable. Cost stated honestly: twelve predictable compares instead of one branch.
    /** Distance d'un point à un SEGMENT (pas à une droite). Écrite ici plutôt que prise dans
     *  `FMath` : cinq lignes, aucune ambiguïté d'API, et elle sert une borne de correction — le
     *  genre d'endroit où « je crois que cette fonction fait ça » n'est pas suffisant. */
    FORCEINLINE float VF_DistPointSegment(const FVector& P, const FVector& A, const FVector& B)
    {
        const FVector AB = B - A;
        const double LenSq = FVector::DotProduct(AB, AB);
        const double T = (LenSq > KINDA_SMALL_NUMBER)
                       ? FMath::Clamp(FVector::DotProduct(P - A, AB) / LenSq, 0.0, 1.0)
                       : 0.0;
        return (float)FVector::Dist(P, A + AB * T);
    }

    /** La même chose en 2D, pour les connecteurs de puits : ce sont des capsules HORIZONTALES, donc
     *  Z se teste exactement et seul XY demande une distance point-segment. */
    FORCEINLINE float VF_DistPointSegment2D(const FVector2D& P, const FVector2D& A, const FVector2D& B)
    {
        const FVector2D AB = B - A;
        const double LenSq = (double)AB.X * AB.X + (double)AB.Y * AB.Y;
        const double T = (LenSq > KINDA_SMALL_NUMBER)
                       ? FMath::Clamp(((double)(P.X - A.X) * AB.X + (double)(P.Y - A.Y) * AB.Y) / LenSq, 0.0, 1.0)
                       : 0.0;
        const double DX = (double)P.X - ((double)A.X + AB.X * T);
        const double DY = (double)P.Y - ((double)A.Y + AB.Y * T);
        return (float)FMath::Sqrt(DX * DX + DY * DY);
    }

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

    FORCEINLINE float VF_SaturatingAdd(float A, float B)
    {
        if (!FMath::IsFinite(A) || !FMath::IsFinite(B)) { return FLT_MAX; }
        if (B > 0.0f && A > FLT_MAX - B) { return FLT_MAX; }
        if (B < 0.0f && A < -FLT_MAX - B) { return -FLT_MAX; }
        return A + B;
    }

    // Structural tree capsules must meet shaft axes without landing inside a ledge band. This is
    // the same pure helper as the generator path: one deterministic safe interval is shared by the
    // whole strate, so a link cannot be blocked when it crosses an unrelated shaft's ledge.
    FORCEINLINE float VF_SelectVerticalTreeConnectorZ(
        const FVerticalShaftParams& Params,
        float BottomZ,
        float TopZ,
        uint32 LinkHash)
    {
        if (Params.LedgeSpacing > 0.0f && Params.LedgeDepth > 0.0f)
        {
            const float Period = Params.LedgeSpacing;
            const float RelativeBottom = BottomZ - Params.StrateBottomWorldZ;
            const float RelativeTop = TopZ - Params.StrateBottomWorldZ;
            const int32 FirstPeriod = FMath::FloorToInt(RelativeBottom / Period);
            const float Random01 = VoxelHash::ToFloat01(VoxelHash::Mix(LinkHash));

            for (int32 PeriodOffset = 0; PeriodOffset <= 1; ++PeriodOffset)
            {
                const float PeriodStart = static_cast<float>(FirstPeriod + PeriodOffset) * Period;
                const float SafeStart = FMath::Max(
                    RelativeBottom, PeriodStart + Params.LedgeDepth + 0.01f);
                const float SafeEnd = FMath::Min(
                    RelativeTop, PeriodStart + Period - Params.LedgeDepth - 0.01f);
                if (SafeEnd > SafeStart)
                {
                    return Params.StrateBottomWorldZ
                        + FMath::Lerp(SafeStart, SafeEnd, Random01);
                }
            }
        }

        return FMath::Lerp(BottomZ, TopZ,
                           VoxelHash::ToFloat01(VoxelHash::Mix(LinkHash)));
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
            const FVector NoisePos(
                WorldX * Frequency + VoxelHash::SeedOffset(SeedU, 3.17f),
                WorldY * Frequency + VoxelHash::SeedOffset(SeedU, 7.31f),
                WorldZ * Frequency + VoxelHash::SeedOffset(SeedU, 11.47f));
            const float Ribbon = FMath::Clamp(
                FMath::Abs(VoxelNoise::FBM((float)NoisePos.X, (float)NoisePos.Y, (float)NoisePos.Z,
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
            if (!FMath::IsFinite(CellSize) || !FMath::IsFinite(CorridorRadius)
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
            const FVector NoisePos(
                WorldX * Frequency + VoxelHash::SeedOffset(SeedU, 13.2f),
                WorldY * Frequency + VoxelHash::SeedOffset(SeedU, 17.8f),
                WorldZ * Frequency + VoxelHash::SeedOffset(SeedU, 23.4f));
            const float Noise01 = FMath::Clamp(
                VoxelNoise::FBM((float)NoisePos.X, (float)NoisePos.Y, (float)NoisePos.Z,
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

        // Sur-approximation volontaire : boîte englobante du segment contre la boîte élargie.
        // Un test capsule/AABB exact serait plus serré ; il coûterait plus cher pour un gain nul
        // ici, car la réponse ne sert qu'à un rejet grossier par tuile.
        static bool SegmentHitsBox(const FVector& A, const FVector& B, const FVector& Min, const FVector& Max)
        {
            return FMath::Min(A.X, B.X) <= Max.X && FMath::Max(A.X, B.X) >= Min.X
                && FMath::Min(A.Y, B.Y) <= Max.Y && FMath::Max(A.Y, B.Y) >= Min.Y
                && FMath::Min(A.Z, B.Z) <= Max.Z && FMath::Max(A.Z, B.Z) >= Min.Z;
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

        // ⚠️⚠️ CORRIGÉ 2026-07-27 : c'était `true`, ET C'ÉTAIT FAUX.
        //
        // Le contrat de `IsXYPure` est « **`Eval`** ne dépend pas de Z » — pas « les surfaces ne
        // dépendent pas de Z ». Or `Eval` calcule `min(Z - sol, plafond - Z)` : il dépend de Z de
        // la façon la plus directe qui soit. §3.1 a rendu les SURFACES pures en XY ; la DENSITÉ,
        // elle, ne l'a jamais été et ne peut pas l'être — c'est une distance à une surface.
        //
        // Latent seulement parce que personne ne lit encore ce drapeau. Le jour où le cache de
        // colonnes T1.a devient générique (l'étape suivante), un `true` ici ferait partager UNE
        // valeur de densité sur TOUTE la pile verticale de chunks — un monde silencieusement faux,
        // que `ValidateDeterminism` ne verrait pas parce qu'il échantillonne le long d'un bord X.
        // C'est exactement le piège que l'avertissement de `VoxelDensityOp.h` décrit, et je suis
        // tombé dedans en écrivant l'opérateur qui le cite.
        //
        // ⚠️ FIXED: this said `true` and was WRONG. The contract is "**Eval** does not depend on Z",
        // and Eval computes min(Z - floor, ceil - Z). §3.1 made the SURFACES XY-pure; the DENSITY
        // never was and cannot be — it is a distance to a surface. Latent only because nothing reads
        // the flag yet; a generic T1.a column cache would have shared one density down the whole
        // vertical chunk stack.
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
        // Les deux surfaces, transcrites au caractère près depuis GetSlabDensity — y compris le
        // détour par FVector, qui est le même piège d'arrondi que dans FSdfRoughnessMod
        // (float → double → float sous /fp:fast). Ne pas « simplifier ».
        float SurfaceFloor(float WorldX, float WorldY) const
        {
            if (FloorRoughness <= 0.0f) { return FloorZ; }
            const float FF = FloorFrequency;
            const FVector NoisePos(WorldX * FF + VoxelHash::SeedOffset(SeedU, 7.3f),
                                   WorldY * FF + VoxelHash::SeedOffset(SeedU, 11.1f),
                                   0.0f);
            const float N = VoxelNoise::FBM((float)NoisePos.X, (float)NoisePos.Y, (float)NoisePos.Z,
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
                const FVector NoisePos(WorldX * CF + VoxelHash::SeedOffset(SeedU, 17.3f) + 1000.0f,
                                       WorldY * CF + VoxelHash::SeedOffset(SeedU, 19.7f) + 2000.0f,
                                       3000.0f);
                const float Raw = VoxelNoise::FBM((float)NoisePos.X, (float)NoisePos.Y, (float)NoisePos.Z,
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

        /** Sans biomes — délègue, pour qu'il n'existe qu'UN corps de construction et UN compteur
         *  d'identité. Deux constructeurs qui s'initialisent chacun de leur côté, c'est deux
         *  endroits où oublier un membre. */
        FSurfaceColumnSource(const FSurfaceGenerationParams& InP, int32 Seed)
            : FSurfaceColumnSource(InP, Seed, TArray<FSurfaceGenerationParams>(), nullptr) {}

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

        const FSurfaceGenerationParams& GetParams() const { return P; }

        EVoxelOpRole GetRole() const override { return EVoxelOpRole::FieldSource; }
        EVoxelOpChannelMask ChannelReads() const override { return VoxelOpChannels::None; }
        EVoxelOpChannelMask ChannelWrites() const override { return VoxelOpChannels::Density; }
        EVoxelOpResourceMask ProvidedResources() const override { return VoxelOpResources::SurfaceColumn; }
        bool IsAdditive() const override { return false; }
        bool IsXYPure() const override { return false; }   // voir le bloc ci-dessus

        /**
         * ⚠️ C'EST ICI QUE SE JOUE LA PERF DE CET ARCHÉTYPE — corrigé 2026-07-27 après mesure.
         *
         * Le mémo de colonne était clé sur `InstanceId`, qui change à CHAQUE reconstruction de pile,
         * c'est-à-dire à chaque chunk. Résultat : une strate haute de 4 chunks recalculait ses
         * colonnes **4 fois**, resamples du cliff compris. Le chemin d'origine ne fait pas ça —
         * `GSurfColCache` est clé sur `(boîte XY, StrateKey, Seed, LayoutVersion)` **SANS ChunkZ**,
         * délibérément, « shared down the whole vertical strate stack ». Cette pile reprend la
         * même identité de strate/layout/seed, en ajoutant l'empreinte obligatoire des params pour
         * protéger ses sorties propres ; son mémo est maintenant un LRU spatial de six boîtes.
         *
         * Donc la clé garde l'identité partagée : ce qui rend deux colonnes interchangeables, c'est
         * la STRATE, le seed, la version de layout et les params, pas le chunk. Le mémo étant
         * `thread_local`, il SURVIT à la reconstruction de la pile — seule la clé l'invalidait.
         *
         * POURQUOI C'EST SÛR : les hauteurs sont XY-pures par construction (c'est tout l'objet de
         * `VoxelHeightOp.h`, où le type n'a pas de Z), et le champ de biomes est documenté
         * XY-pur — « ZERO Z dependence: the climate/Voronoi fields are pure-XY ». C'est exactement
         * la justification sur laquelle `GSurfColCache` repose déjà.
         *
         * The memo was keyed on InstanceId, which changes every chunk, so a 4-chunk strate recomputed
         * every column 4x. GSurfColCache deliberately omits ChunkZ and shares down the whole vertical
         * stack; this now shares the same strate/layout/seed identity and adds the required params
         * fingerprint for its own outputs. The six-box LRU keeps independent XY regions alive. Safe
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
            const float Ns = HFractal3D(FVector(
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

            // ⚠️ LE DÉTOUR PAR FVector EST DÉLIBÉRÉ — ne pas « simplifier ».
            // L'original écrit `FractalNoise3D(FVector(WorldX * 0.12f, ...), Eff(3))`, et
            // FractalNoise3D fait `VoxelNoise::FBM((float)Position.X, ...)`. FVector étant en
            // DOUBLE (UE5), le produit flottant y transite par un double avant d'être re-arrondi
            // en float. Passer directement des floats saute cet aller-retour, et sous /fp:fast
            // les deux chemins ne s'arrondissent pas au même endroit : ~1 ULP d'écart sur le SDF,
            // qui ressort en 1 ULP sur la densité finale. Reproduire le détour, c'est reproduire
            // l'arrondi. VERIFIE par MazeEquivalence : le détour conserve l'identité binaire avec
            // GetMazeDensity (0 écart sur 20,000 échantillons). Si cela change, le candidat suivant
            // est une contraction FMA entre unités de compilation.
            //
            // THE FVector ROUND-TRIP IS DELIBERATE — do not "simplify" it. MazeEquivalence
            // verifies the resulting path against GetMazeDensity (0 differences / 20,000 samples).
            // The original goes float -> double (FVector is double in UE5) -> float; going straight
            // through floats skips a rounding step, and under /fp:fast the two paths round in
            // different places. Reproducing the detour reproduces the rounding.
            const FVector NoisePos(WorldX * Frequency, WorldY * Frequency, WorldZ * Frequency);
            InOut.Sdf += VoxelNoise::FBM((float)NoisePos.X, (float)NoisePos.Y, (float)NoisePos.Z,
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
            if (!FMath::IsFinite(Strength) || !FMath::IsFinite(Frequency)
                || !FMath::IsFinite(ApplyWithin) || BaseOctaves <= 0)
            {
                InOut.SetUnknown();
                return;
            }
            if (Strength <= 0.0f) { return; }

            // The noise input is float after the deliberate FVector round-trip in Eval. If an
            // authored frequency/box product overflows, Eval can publish a non-finite SDF and no
            // finite interval is a proof. Unknown costs the skip and protects the geometry.
            const float MaxAbsCoord = FMath::Max3(
                FMath::Max(FMath::Abs((float)VoxelBox.Min.X), FMath::Abs((float)VoxelBox.Max.X)),
                FMath::Max(FMath::Abs((float)VoxelBox.Min.Y), FMath::Abs((float)VoxelBox.Max.Y)),
                FMath::Max(FMath::Abs((float)VoxelBox.Min.Z), FMath::Abs((float)VoxelBox.Max.Z)));
            if (!FMath::IsFinite(MaxAbsCoord) || !FMath::IsFinite(MaxAbsCoord * FMath::Abs(Frequency)))
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
            if (!FMath::IsFinite(Coefficient) || !FMath::IsFinite(Blend)
                || !FMath::IsFinite(MinDivisor) || Blend <= 0.0f)
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
            if (!FMath::IsFinite(Coefficient) || !FMath::IsFinite(Blend)
                || !FMath::IsFinite(MinDivisor) || Blend <= 0.0f)
            {
                return FLT_MAX;
            }
            return Coefficient < 0.0f ? FMath::Abs(Coefficient) : 0.0f;
        }

        float MaxFillOverBox(const FBox&, const FVoxelOpContext&) const override
        {
            const float Coefficient = Sign * BaseDensity * 2.0f;
            if (!FMath::IsFinite(Coefficient) || !FMath::IsFinite(Blend)
                || !FMath::IsFinite(MinDivisor) || Blend <= 0.0f)
            {
                return FLT_MAX;
            }
            return Coefficient > 0.0f ? FMath::Abs(Coefficient) : 0.0f;
        }

        float MaxCarveOverBox(const FBox& VoxelBox, const FVoxelOpContext& Ctx,
                              const FVoxelBoxHypotheses& H) const override
        {
            const float Coefficient = Sign * BaseDensity * 2.0f;
            if (!FMath::IsFinite(Coefficient)) { return FLT_MAX; }
            if (!FMath::IsFinite(Blend) || !FMath::IsFinite(MinDivisor) || Blend <= 0.0f)
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
            if (!FMath::IsFinite(Coefficient)) { return FLT_MAX; }
            if (!FMath::IsFinite(Blend) || !FMath::IsFinite(MinDivisor) || Blend <= 0.0f)
            {
                return FLT_MAX;
            }
            if (Coefficient <= 0.0f) { return 0.0f; }
            return IsInactive(H) ? 0.0f : FMath::Abs(Coefficient) * MaxFactor(H);
        }

        const TCHAR* DebugName() const override { return TEXT("SdfConvertOp"); }

    private:
        bool IsInactive(const FVoxelBoxHypotheses& H) const
        {
            return !H.Sdf.IsKnown() ? false : H.Sdf.Min >= Blend;
        }

        float MaxFactor(const FVoxelBoxHypotheses& H) const
        {
            if (!H.Sdf.IsKnown() || !FMath::IsFinite(Blend)
                || !FMath::IsFinite(MinDivisor) || Blend <= 0.0f)
            {
                return 1.0f;
            }

            const float Denom = FMath::Max(Blend * 2.0f, MinDivisor);
            if (!(Denom > 0.0f) || !FMath::IsFinite(Denom)) { return 1.0f; }

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

        // Ne fait QUE de l'air ⇒ tue AllSolid, jamais AllAir. Identity quand le cercle XY rate la
        // boîte, ou quand la boîte est entièrement hors de l'intérieur de la strate.
        // ≡ le test cercle/boîte écrit à la main dans ClassifyTile aujourd'hui.
        EVoxelOpEffect EffectOverBox(const FBox& VoxelBox, const FVoxelOpContext&) const override
        {
            if (Radius <= 0.0f) { return EVoxelOpEffect::Identity; }

            const float InnerTop = TopZ - Seal;
            const float InnerBot = BotZ + Seal;
            if (VoxelBox.Max.Z <= InnerBot || VoxelBox.Min.Z >= InnerTop) { return EVoxelOpEffect::Identity; }

            const float Reach = Radius + VoxelDensityReach::SpineBlend;
            const float CX = FMath::Clamp(0.0f, (float)VoxelBox.Min.X, (float)VoxelBox.Max.X);
            const float CY = FMath::Clamp(0.0f, (float)VoxelBox.Min.Y, (float)VoxelBox.Max.Y);
            if (CX * CX + CY * CY > Reach * Reach) { return EVoxelOpEffect::Identity; }

            return EVoxelOpEffect::CarveOnly;
        }

        const TCHAR* DebugName() const override { return TEXT("OriginSpineOp"); }

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
         * mais elle est réelle ; notée plutôt que corrigée en douce, puisque le chemin d'aujourd'hui
         * n'est pas touché par cette Phase 1.)
         */
        EVoxelTileClass ClassifyBox(const FBox& VoxelBox, const FVoxelOpContext&) const override
        {
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
            if (Thickness <= 0.0f) { return EVoxelOpEffect::Identity; }
            const bool bTouchesTopBand = ((float)VoxelBox.Max.Z >= TopZ - Thickness) && ((float)VoxelBox.Min.Z <= TopZ);
            const bool bTouchesBotBand = ((float)VoxelBox.Min.Z <= BotZ + Thickness) && ((float)VoxelBox.Max.Z >= BotZ);
            if (!bTouchesTopBand && !bTouchesBotBand) { return EVoxelOpEffect::Identity; }
            return EVoxelOpEffect::FillOnly;
        }

        const TCHAR* DebugName() const override { return TEXT("BoundarySealOp"); }

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
            return VF_XYEdgeSealBoxTouchesBand(
                VoxelBox, Ctx.WorldRadiusVoxels, Ctx.EdgeSealThickness)
                ? EVoxelOpEffect::FillOnly : EVoxelOpEffect::Identity;
        }

        bool IsXYPure() const override { return true; }
        const TCHAR* DebugName() const override { return TEXT("XYEdgeSealOp"); }

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
        FPassageCarveOp(const UVoxelStrateManager* InManager, float InBase, float InSeal)
            : Manager(InManager), Base(InBase), Seal(InSeal) {}

        EVoxelOpRole GetRole() const override { return EVoxelOpRole::StructuralPost; }
        EVoxelOpChannelMask ChannelReads() const override { return VoxelOpChannels::Density; }
        EVoxelOpChannelMask ChannelWrites() const override { return VoxelOpChannels::Density; }
        bool IsAdditive() const override { return false; }
        void PrepareChunk(const FVoxelOpContext&) override {}

        void Eval(float X, float Y, float Z, FVoxelOpSample& InOut) const override
        {
            if (!Manager) { return; }
            const float ModSDF = Manager->EvaluateModifierSDF(X, Y, Z);
            VF_ApplyPassageCarving(InOut.Density, ModSDF, Base, Seal);
        }

        // ≡ la garde `AnyPassageNearBox` écrite à la main dans ClassifyTile — déjà écrite, ici
        // simplement branchée au bon endroit au lieu d'être un cas particulier du classifieur.
        EVoxelOpEffect EffectOverBox(const FBox& VoxelBox, const FVoxelOpContext&) const override
        {
            if (!Manager) { return EVoxelOpEffect::Identity; }
            return Manager->AnyPassageNearBox(VoxelBox.Min, VoxelBox.Max)
                 ? EVoxelOpEffect::CarveOnly : EVoxelOpEffect::Identity;
        }

        const TCHAR* DebugName() const override { return TEXT("PassageCarveOp"); }

    private:
        const UVoxelStrateManager* Manager;
        float Base, Seal;
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
        struct FConn  { FVector A, B; float Radius; };

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
            if (!FMath::IsFinite((float)VoxelBox.Min.X) || !FMath::IsFinite((float)VoxelBox.Min.Y)
                || !FMath::IsFinite((float)VoxelBox.Min.Z)
                || !FMath::IsFinite((float)VoxelBox.Max.X) || !FMath::IsFinite((float)VoxelBox.Max.Y)
                || !FMath::IsFinite((float)VoxelBox.Max.Z)
                || VoxelBox.Min.X > VoxelBox.Max.X || VoxelBox.Min.Y > VoxelBox.Max.Y
                || VoxelBox.Min.Z > VoxelBox.Max.Z
                || !FMath::IsFinite(P.ShaftSpacing) || P.ShaftSpacing <= 0.0f)
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
                                 VS_BotZ = FLT_MAX, VS_TopZ = FLT_MAX, VS_Seal = -1.0f;

            if (CX != VS_CX || CY != VS_CY || Salt != VS_Salt || Spacing != VS_Spacing ||
                P.ShaftDensity != VS_Dens || P.ShaftMinRadius != VS_MinR || P.ShaftMaxRadius != VS_MaxR ||
                P.CrossConnectChance != VS_Cross || P.ConnectorRadius != VS_ConnR ||
                P.SurfaceRoughness != VS_Rough || SpineRadius != VS_SpineR ||
                P.StrateBottomWorldZ != VS_BotZ || P.StrateTopWorldZ != VS_TopZ ||
                P.BoundarySealThickness != VS_Seal)
            {
                VS_CX = CX;  VS_CY = CY;  VS_Salt = Salt;  VS_Spacing = Spacing;
                VS_Dens = P.ShaftDensity;  VS_MinR = P.ShaftMinRadius;  VS_MaxR = P.ShaftMaxRadius;
                VS_Cross = P.CrossConnectChance;  VS_ConnR = P.ConnectorRadius;
                VS_Rough = P.SurfaceRoughness;    VS_SpineR = SpineRadius;
                VS_BotZ = P.StrateBottomWorldZ;  VS_TopZ = P.StrateTopWorldZ;
                VS_Seal = P.BoundarySealThickness;
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
                    const int32 ParentCellX = Parent != nullptr ? Parent->CellX : 0;
                    const int32 ParentCellY = Parent != nullptr ? Parent->CellY : 0;
                    const uint32 LinkHash = VoxelHash::Pair(
                        Child.CellX, Child.CellY, ParentCellX, ParentCellY, Salt ^ TreeSalt);
                    const float Zc = VF_SelectVerticalTreeConnectorZ(
                        P, BottomZ, TopZ, LinkHash);
                    const FVector ParentPoint = Parent != nullptr
                        ? FVector(Parent->X, Parent->Y, Zc)
                        : FVector(0.0f, 0.0f, Zc);
                    const FConn Conn{
                        FVector(Child.X, Child.Y, Zc), ParentPoint, TreeRadius };
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
                            FVector(A.X, A.Y, Zc), FVector(B.X, B.Y, Zc), ConnectorR };
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
    // usages de frames (îles, caves de TunnelNetwork, tunnels), mais **deux d'entre eux ne sont pas
    // encore portés**. Inventer l'infrastructure de frame pour son unique utilisateur actuel, c'est
    // la concevoir contre un seul exemple — précisément ce que ce refactor a évité jusqu'ici en
    // n'abstrayant qu'à la deuxième occurrence (cf. `IVoxelBiomeField`, né d'un besoin réel).
    // ✅ RÉPONDU (2026-08-16) : TunnelNetwork EST porté, et le deuxième usage réel a dissous la
    // question au lieu de la trancher. Voir `BuildTunnelNetworkStack` : `CaveWarp` n'enveloppe
    // qu'UN opérateur (donc c'est une variable locale, pas un frame) et `VerticalScale` est une
    // fonction pure d'un scalaire. **Zéro frame sur trois candidats.** Ne pas rouvrir : le warp
    // reste local ICI pour la même raison qu'il est resté local là-bas.
    //
    // The warp stays INSIDE the op against §7's FRAME suggestion. ✅ ANSWERED 2026-08-16: this said
    // "revisit when TunnelNetwork brings the second real use" — TunnelNetwork is ported, and the
    // second use dissolved the question rather than settling it. See BuildTunnelNetworkStack:
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
            const float WX = WorldX + HFractal3D(FVector(WorldX * 0.04f + VoxelHash::SeedOffset(Salt, 0.0007f),
                                                         WorldY * 0.04f, WorldZ * 0.012f), VoxelGenLOD::Eff(3))
                                      * VOXEL_NOISE_SCALE * WarpAmp;
            const float WY = WorldY + HFractal3D(FVector(WorldX * 0.04f + 31.0f, WorldY * 0.04f + 7.0f,
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
            if (!FMath::IsFinite((float)VoxelBox.Min.X) || !FMath::IsFinite((float)VoxelBox.Min.Y)
                || !FMath::IsFinite((float)VoxelBox.Min.Z)
                || !FMath::IsFinite((float)VoxelBox.Max.X) || !FMath::IsFinite((float)VoxelBox.Max.Y)
                || !FMath::IsFinite((float)VoxelBox.Max.Z)
                || VoxelBox.Min.X > VoxelBox.Max.X || VoxelBox.Min.Y > VoxelBox.Max.Y
                || VoxelBox.Min.Z > VoxelBox.Max.Z
                || !FMath::IsFinite(P.IslandSpacing) || !FMath::IsFinite(P.IslandDensity)
                || !FMath::IsFinite(P.IslandMinRadius) || !FMath::IsFinite(P.IslandMaxRadius)
                || !FMath::IsFinite(P.ThicknessRatio) || !FMath::IsFinite(P.VerticalJitter)
                || !FMath::IsFinite(P.TopFlatten) || !FMath::IsFinite(P.SDFBlendRadius)
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
         * que `AUDIT §C2` et que la régression d'overhang du 2026-07-27 : une clé de cache
         * incomplète ne se voit pas, elle produit du terrain plausible. Ajouter le champ ne coûte
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
        // ⚠️ `static` (donc PARTAGÉ ENTRE INSTANCES), pas un membre : c'est exactement ce que fait
        // l'original, et le contrôle 3 du test en DÉPEND — deux piles construites côte à côte se
        // partagent ce cache, et c'est l'empreinte de params dans la clé (pas une copie par pile) qui
        // les empêche de se servir mutuellement leurs salles. En faire un membre ferait passer ce
        // contrôle pour de mauvaises raisons.
        //
        // ⚠️ MÊME MOTIF QUE `FOverhangShelfMod` ← `FSurfaceColumnSource` et `FShaftLedgeMod` ←
        // `FShaftFieldSource` : un opérateur possède l'état, les autres le lisent par pointeur non
        // possédant remis à la construction. C'est un motif ÉTABLI dans ce fichier, pas une invention.
        //
        // Per-worker state, deliberately `static` (shared between instances) because that is what the
        // original does and what the test's stale-cache check rests on. Detail ops read it through a
        // non-owning pointer, the same way the surface and shaft ports already do.
        struct FState
        {
            FChunkSDFCache Cache;
            float  CachedSMinX = 1.0f, CachedSMaxX = -1.0f;   // invalide au départ (min > max)
            float  CachedSMinY = 0.0f, CachedSMaxY = 0.0f;
            int32  CachedStrate = INT32_MIN;
            uint32 CachedSeed = 0;
            uint32 CachedFingerprint = 0xFFFFFFFFu;
            uint32 CachedLayout = 0xFFFFFFFFu;

            /** La salle de SDF minimal pour le dernier voxel évalué. -1 = aucune. */
            int32 NearestRoom = -1;

            /** ÉTAPE C1 — les params de la strate avec l'op de CETTE salle appliqué par-dessus.
             *  Mémo par voxel : invalidé au début de chaque `Eval`, calculé au PREMIER modificateur
             *  qui le demande. C'est ce qui reproduit le coût de l'original (une copie de struct par
             *  voxel PRÈS D'UNE SURFACE, pas partout) sans que onze opérateurs la refassent chacun. */
            FStrateGenerationParams LocalParams;
            bool bLocalParamsValid = false;
        };

        static FState& State()
        {
            thread_local FState S;
            return S;
        }

        /** Le cache que la source vient de bâtir/servir pour ce voxel. Lu par les colonnes (4d). */
        const FChunkSDFCache& GetCache() const { return State().Cache; }

        /** L'index de la salle la plus proche pour le dernier voxel évalué. -1 = aucune.
         *  Lu par les arches, les dômes, le pincement et le biais de sol. */
        int32 GetNearestRoomIdx() const { return State().NearestRoom; }

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
                if (S.NearestRoom >= 0 && S.Cache.Rooms.IsValidIndex(S.NearestRoom))
                {
                    const FCachedRoom& NR = S.Cache.Rooms[S.NearestRoom];
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
            return VoxelCaveMorphology::EvaluateSDFCached(X, Y, Z, State().Cache, P.SDFBlendRadius);
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
            FState& S = State();

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

            if (!(P.RoomDensity > 0.0f && P.RoomSpacing > 0.0f)) { return; }   // Sdf reste FLT_MAX

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
            if (P.CaveWarpStrength > 0.0f)
            {
                const float WF = P.CaveWarpFrequency;
                const float WS = P.CaveWarpStrength;
                WarpedX += VoxelNoise::Perlin3D(FVector(
                    WorldX * WF + VoxelHash::SeedOffset(SeedU, 0.37f),
                    WorldY * WF + 1.3f,
                    EffectiveZ * WF + 5.7f)) * VOXEL_NOISE_SCALE * WS;
                WarpedY += VoxelNoise::Perlin3D(FVector(
                    WorldX * WF + 7.1f,
                    WorldY * WF + VoxelHash::SeedOffset(SeedU, 0.59f),
                    EffectiveZ * WF + 2.3f)) * VOXEL_NOISE_SCALE * WS;
                WarpedZ += VoxelNoise::Perlin3D(FVector(
                    WorldX * WF + 11.3f,
                    WorldY * WF + 9.7f,
                    EffectiveZ * WF + VoxelHash::SeedOffset(SeedU, 0.41f))) * VOXEL_NOISE_SCALE * WS;
            }

            //---------------------------------------------------------------
            // LE CACHE PAR BOÎTE DE RECHERCHE
            //---------------------------------------------------------------
            // ⚠️ CLÉ PAR BOÎTE, PAS PAR CHUNK, et c'est un INVARIANT DE PERF (§8.10) : les
            // échantillons de gradient interrogent `WorldX ± 1` et le warp déplace encore, donc une
            // clé « égalité de chunk » se retournait à chaque cellule de bord et reconstruisait le
            // cache (coûteux) en boucle. Comme le cache couvre la boîte + MaxInfluence, toute requête
            // DANS la boîte est correcte. Ne pas « simplifier » en clé de chunk.
            // (Les champs de cache vivent maintenant dans `FState`, au-dessus, pour que les
            // modificateurs de détail de l'étape B puissent les lire. Même durée de vie, même
            // partage entre instances qu'avant : ce sont les mêmes `thread_local`, déménagés.)
            // ⚠️ AJOUTÉ PAR RAPPORT À L'ORIGINAL — la leçon du 2026-07-27 (régression d'overhang).
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
            if (Manager)
            {
                thread_local int32  SI_ChunkZ  = INT32_MAX;
                thread_local uint32 SI_Version = 0xFFFFFFFFu;
                thread_local int32  SI_Index   = 0;
                const int32 QZ = FMath::FloorToInt(WorldZ / (float)CHUNK_SIZE);
                const uint32 LV = Manager->GetLayoutVersion();
                if (QZ != SI_ChunkZ || LV != SI_Version)
                {
                    SI_ChunkZ  = QZ;
                    SI_Version = LV;
                    SI_Index = Manager->GetStrateIndex(((float)QZ + 0.5f) * CHUNK_SIZE * VOXEL_SIZE);
                }
                StrateIdx = SI_Index;
            }

            const bool bNeedRebuild =
                StrateIdx != S.CachedStrate || SeedU != S.CachedSeed ||
                ParamsFingerprint != S.CachedFingerprint || LayoutVersion != S.CachedLayout ||
                WarpedX < S.CachedSMinX || WarpedX > S.CachedSMaxX ||
                WarpedY < S.CachedSMinY || WarpedY > S.CachedSMaxY;

            if (bNeedRebuild)
            {
                const int32 CacheChunkX = FMath::FloorToInt(WorldX / (float)CHUNK_SIZE);
                const int32 CacheChunkY = FMath::FloorToInt(WorldY / (float)CHUNK_SIZE);
                const float ChunkMinX = CacheChunkX * (float)CHUNK_SIZE;
                const float ChunkMinY = CacheChunkY * (float)CHUNK_SIZE;
                const float ChunkMaxX = ChunkMinX + (float)CHUNK_SIZE;
                const float ChunkMaxY = ChunkMinY + (float)CHUNK_SIZE;
                const float Expansion = P.CaveWarpStrength + 2.0f;

                const float SMinX = ChunkMinX - Expansion;
                const float SMinY = ChunkMinY - Expansion;
                const float SMaxX = ChunkMaxX + Expansion;
                const float SMaxY = ChunkMaxY + Expansion;

                const TArray<FStrateTerrainOpEntry>* TerrainOps = nullptr;
                if (Manager)
                {
                    const int32 ChunkZ = FMath::FloorToInt(WorldZ / (float)CHUNK_SIZE);
                    UVoxelStrateDefinition* Def = Manager->GetStrateForChunk(
                        FIntVector(CacheChunkX, CacheChunkY, ChunkZ));
                    if (Def) { TerrainOps = &Def->TerrainOperations; }
                }

                VoxelCaveMorphology::BuildChunkCache(
                    S.Cache, SMinX, SMinY, SMaxX, SMaxY, P, SeedU, StrateIdx, TerrainOps);

                S.CachedSMinX = SMinX; S.CachedSMaxX = SMaxX;
                S.CachedSMinY = SMinY; S.CachedSMaxY = SMaxY;
                S.CachedStrate = StrateIdx;
                S.CachedSeed = SeedU;
                S.CachedFingerprint = ParamsFingerprint;
                S.CachedLayout = LayoutVersion;
            }

            float CaveSDF = VoxelCaveMorphology::EvaluateSDFCached(
                WarpedX, WarpedY, WarpedZ, S.Cache, P.SDFBlendRadius, &S.NearestRoom);

            //---------------------------------------------------------------
            // PITS & CHEMINÉES — coordonnées RÉELLES, SmoothMin dans le même canal SDF
            //---------------------------------------------------------------
            // C'est le point que `OPSTACK-DECOMPOSITION §2` annonçait comme « le plus retors de toute
            // la décomposition » : deux primitives qui écrivent le MÊME canal que le graphe de salles
            // mais à des coordonnées NON warpées. Sous un modèle de frames il aurait fallu les sortir
            // du frame tout en gardant le canal — exprimable, mais tordu. Dans un opérateur unique la
            // difficulté disparaît : le warp est une variable locale, pas un contexte hérité.
            for (const FCachedPit& Pit : S.Cache.Pits)
            {
                const float DZ = WorldZ - Pit.TopZ;
                if (DZ >= Pit.BlendK) { continue; }
                if (-DZ > Pit.Depth + Pit.BlendK) { continue; }

                const float DX = WorldX - Pit.CenterX;
                const float DY = WorldY - Pit.CenterY;
                const float XYDistSq = DX * DX + DY * DY;
                if (XYDistSq > Pit.BoundXYRadiusSq) { continue; }

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
            }

            for (const FCachedChimney& Chim : S.Cache.Chimneys)
            {
                const float DZ = WorldZ - Chim.BottomZ;
                if (-DZ >= Chim.BlendK) { continue; }
                if (DZ > Chim.Height + Chim.BlendK) { continue; }

                const float DX = WorldX - Chim.CenterX;
                const float DY = WorldY - Chim.CenterY;
                const float XYDistSq = DX * DX + DY * DY;
                if (XYDistSq > Chim.BoundXYRadiusSq) { continue; }

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
            }

            InOut.Sdf = CaveSDF;
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
            FBox   KeyBox = FBox(ForceInit);
            int32  KeyStrate = INT32_MIN;
            uint32 KeySeed = 0;
            uint32 KeyFingerprint = 0xFFFFFFFFu;
            uint32 KeyLayout = 0xFFFFFFFFu;
            bool   bValid = false;
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
        };

        static FBoxState& BoxState()
        {
            thread_local FBoxState S;
            return S;
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

            auto Unknown = [&]()
            {
                B.bValid = false;
                B.SdfInterval.SetUnknown();
                InOut.SetUnknown();
            };
            auto Finite = [](float V) { return FMath::IsFinite(V); };

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
                B.bValid = false;
                B.NumRooms = B.NumTunnels = B.NumPits = B.NumChimneys = 0;
                B.HitRooms = B.HitTunnels = B.HitPits = B.HitChimneys = 0;
                B.HitRoomsNoWarp = B.HitTunnelsNoWarp = 0;
                B.WarpDilation = 0.0f;
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
            if (SpanX <= 0 || SpanY <= 0 || SpanZ <= 0 || SpanX * SpanY * SpanZ > 64)
            {
                Unknown();
                return;
            }

            int32 StrateIdx = 0;
            const TArray<FStrateTerrainOpEntry>* TerrainOps = nullptr;
            if (Manager)
            {
                StrateIdx = Manager->GetStrateIndex(
                    ((float)CZ0 + 0.5f) * CHUNK_SIZE * VOXEL_SIZE);
                for (int32 CZ = CZ0 + 1; CZ <= CZ1; ++CZ)
                {
                    if (Manager->GetStrateIndex(
                            ((float)CZ + 0.5f) * CHUNK_SIZE * VOXEL_SIZE) != StrateIdx)
                    {
                        Unknown();
                        return;
                    }
                }

                // The room-op pool is part of the SDF: BuildChunkCache bakes pits, chimneys, and
                // columns from it. Every chunk touched by this query must therefore resolve to the
                // same definition before one cache can represent the box.
                UVoxelStrateDefinition* Def0 =
                    Manager->GetStrateForChunk(FIntVector(CX0, CY0, CZ0));
                for (int32 CZ = CZ0; CZ <= CZ1; ++CZ)
                for (int32 CY = CY0; CY <= CY1; ++CY)
                for (int32 CX = CX0; CX <= CX1; ++CX)
                {
                    if (Manager->GetStrateForChunk(FIntVector(CX, CY, CZ)) != Def0)
                    {
                        Unknown();
                        return;
                    }
                }
                if (Def0) { TerrainOps = &Def0->TerrainOperations; }
            }

            const uint32 LV = Ctx.LayoutVersion;
            if (B.bValid && B.KeyBox == VoxelBox && B.KeyStrate == StrateIdx
                && B.KeySeed == SeedU && B.KeyFingerprint == ParamsFingerprint && B.KeyLayout == LV)
            {
                InOut = B.SdfInterval;
                return;
            }

            const float Warp = (P.CaveWarpStrength > 0.0f)
                             ? P.CaveWarpStrength * VOXEL_NOISE_SCALE * VF_PerlinAbsBound
                             : 0.0f;
            if (!Finite(Warp))
            {
                Unknown();
                return;
            }

            // The search window is a superset of every warped point in the box, with the same
            // two-voxel gradient margin used by Eval's cache path.
            VoxelCaveMorphology::BuildChunkCache(
                B.Cache,
                BoxMinX - Warp - 2.0f, BoxMinY - Warp - 2.0f,
                BoxMaxX + Warp + 2.0f, BoxMaxY + Warp + 2.0f,
                P, SeedU, StrateIdx, TerrainOps);

            const float EffectiveMinZ = (P.VerticalScale > 0.0f && P.VerticalScale != 1.0f)
                                      ? BoxMinZ / P.VerticalScale : BoxMinZ;
            const float EffectiveMaxZ = (P.VerticalScale > 0.0f && P.VerticalScale != 1.0f)
                                      ? BoxMaxZ / P.VerticalScale : BoxMaxZ;
            const FVector QMin(BoxMinX - Warp, BoxMinY - Warp, EffectiveMinZ - Warp);
            const FVector QMax(BoxMaxX + Warp, BoxMaxY + Warp, EffectiveMaxZ + Warp);
            const FVector RMin(BoxMinX, BoxMinY, BoxMinZ);
            const FVector RMax(BoxMaxX, BoxMaxY, BoxMaxZ);
            if (!Finite((float)QMin.X) || !Finite((float)QMin.Y) || !Finite((float)QMin.Z)
                || !Finite((float)QMax.X) || !Finite((float)QMax.Y) || !Finite((float)QMax.Z))
            {
                Unknown();
                return;
            }

            const float K = P.SDFBlendRadius;
            const float WormThreshold = FMath::IsFinite(P.WormNetworkRange)
                                       ? FMath::Max(3.0f * K, P.WormNetworkRange) : FLT_MAX;
            const float ThresholdWithBlend = VF_SaturatingAdd(WormThreshold, K);
            float Lower = FLT_MAX;
            bool bAnyPrimitive = false;
            bool bInvalidBound = false;

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

            B.NumRooms = B.Cache.Rooms.Num();
            B.NumTunnels = B.Cache.Tunnels.Num();
            B.NumPits = B.Cache.Pits.Num();
            B.NumChimneys = B.Cache.Chimneys.Num();
            B.HitRooms = B.HitTunnels = B.HitPits = B.HitChimneys = 0;
            B.HitRoomsNoWarp = B.HitTunnelsNoWarp = 0;
            B.WarpDilation = Warp;

            const FVector NoWarpMin(BoxMinX, BoxMinY, EffectiveMinZ);
            const FVector NoWarpMax(BoxMaxX, BoxMaxY, EffectiveMaxZ);

            for (const FCachedRoom& Room : B.Cache.Rooms)
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
                const float RoomLower = VF_DistanceBetweenBoxes(
                    QMin, QMax, Room.Center, Room.Center) - RoomRadius;
                const float RoomLowerNoWarp = VF_DistanceBetweenBoxes(
                    NoWarpMin, NoWarpMax, Room.Center, Room.Center) - RoomRadius;
                Consider(RoomLower);
                CountThreshold(RoomLower, B.HitRooms);
                CountThreshold(RoomLowerNoWarp, B.HitRoomsNoWarp);
            }

            for (const FCachedTunnel& Tunnel : B.Cache.Tunnels)
            {
                const float MaxRadius = FMath::Max3(
                    FMath::Abs(Tunnel.RadiusA), FMath::Abs(Tunnel.RadiusB),
                    Tunnel.bHasMidpoint ? FMath::Abs(Tunnel.RadiusMid) : 0.0f);
                const FVector SegmentAMin(
                    FMath::Min(Tunnel.EndpointA.X, Tunnel.bHasMidpoint ? Tunnel.Midpoint.X : Tunnel.EndpointB.X),
                    FMath::Min(Tunnel.EndpointA.Y, Tunnel.bHasMidpoint ? Tunnel.Midpoint.Y : Tunnel.EndpointB.Y),
                    FMath::Min(Tunnel.EndpointA.Z, Tunnel.bHasMidpoint ? Tunnel.Midpoint.Z : Tunnel.EndpointB.Z));
                const FVector SegmentAMax(
                    FMath::Max(Tunnel.EndpointA.X, Tunnel.bHasMidpoint ? Tunnel.Midpoint.X : Tunnel.EndpointB.X),
                    FMath::Max(Tunnel.EndpointA.Y, Tunnel.bHasMidpoint ? Tunnel.Midpoint.Y : Tunnel.EndpointB.Y),
                    FMath::Max(Tunnel.EndpointA.Z, Tunnel.bHasMidpoint ? Tunnel.Midpoint.Z : Tunnel.EndpointB.Z));

                float TunnelLower = VF_DistanceBetweenBoxes(
                    QMin, QMax, SegmentAMin, SegmentAMax) - MaxRadius;
                if (Tunnel.bHasMidpoint)
                {
                    const FVector SegmentBMin(
                        FMath::Min(Tunnel.Midpoint.X, Tunnel.EndpointB.X),
                        FMath::Min(Tunnel.Midpoint.Y, Tunnel.EndpointB.Y),
                        FMath::Min(Tunnel.Midpoint.Z, Tunnel.EndpointB.Z));
                    const FVector SegmentBMax(
                        FMath::Max(Tunnel.Midpoint.X, Tunnel.EndpointB.X),
                        FMath::Max(Tunnel.Midpoint.Y, Tunnel.EndpointB.Y),
                        FMath::Max(Tunnel.Midpoint.Z, Tunnel.EndpointB.Z));
                    TunnelLower = FMath::Min(
                        TunnelLower,
                        VF_DistanceBetweenBoxes(QMin, QMax, SegmentBMin, SegmentBMax) - MaxRadius);
                }

                const float TunnelLowerNoWarp = VF_DistanceBetweenBoxes(
                    NoWarpMin, NoWarpMax, SegmentAMin, SegmentAMax) - MaxRadius;
                Consider(TunnelLower);
                CountThreshold(TunnelLower, B.HitTunnels);
                CountThreshold(TunnelLowerNoWarp, B.HitTunnelsNoWarp);
            }

            for (const FCachedPit& Pit : B.Cache.Pits)
            {
                if (!(BoxMinZ < Pit.TopZ + Pit.BlendK)
                    || !(BoxMaxZ >= Pit.TopZ - Pit.Depth - Pit.BlendK))
                {
                    continue;
                }
                const float MaxRadius = FMath::Abs(Pit.Radius) + FMath::Abs(Pit.FlareExtra);
                const float PitLower = VF_DistanceBoxToPointXY(
                    FBox(RMin, RMax), Pit.CenterX, Pit.CenterY) - MaxRadius;
                Consider(PitLower);
                CountThreshold(PitLower, B.HitPits);
            }

            for (const FCachedChimney& Chimney : B.Cache.Chimneys)
            {
                if (!(BoxMaxZ > Chimney.BottomZ - Chimney.BlendK)
                    || !(BoxMinZ <= Chimney.BottomZ + Chimney.Height + Chimney.BlendK))
                {
                    continue;
                }
                const float MaxRadius = FMath::Abs(Chimney.Radius) + FMath::Abs(Chimney.FlareExtra);
                const float ChimneyLower = VF_DistanceBoxToPointXY(
                    FBox(RMin, RMax), Chimney.CenterX, Chimney.CenterY) - MaxRadius;
                Consider(ChimneyLower);
                CountThreshold(ChimneyLower, B.HitChimneys);
            }

            if (bInvalidBound)
            {
                Unknown();
                return;
            }

            FVoxelBoxSdfInterval Own;
            if (!bAnyPrimitive)
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
            B.bValid = true;
            InOut = Own;
        }

        const TCHAR* DebugName() const override { return TEXT("RoomGraphSource"); }

    private:
        FStrateGenerationParams P;
        int32  Seed;
        uint32 SeedU;
        const UVoxelStrateManager* Manager;   // NON possédant
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
        return FMath::IsFinite(Threshold) && H.Sdf.IsKnown() && H.Sdf.Min >= Threshold;
    }

    FORCEINLINE EVoxelOpEffect VF_CaveDetailEffect(const FRoomGraphSource* Rooms,
                                                   const FVoxelBoxHypotheses& H,
                                                   float SDFBlendRadius,
                                                   EVoxelOpEffect Intrinsic)
    {
        // A detail op with no room source has the same no-op guard as Eval. An unknown interval is
        // not a reason to claim Identity: uncertainty costs a skip, while a false skip is a hole.
        return (Rooms == nullptr || VF_CaveBoxIsFar(H, SDFBlendRadius))
             ? EVoxelOpEffect::Identity : Intrinsic;
    }

    FORCEINLINE float VF_CaveDetailMax(const FRoomGraphSource* Rooms,
                                       const FVoxelBoxHypotheses& H,
                                       float SDFBlendRadius)
    {
        // Per-room overrides can activate or enlarge a modifier even when the strate-level
        // parameter is zero. Near/unknown therefore returns the safe default amplitude.
        return (Rooms == nullptr || VF_CaveBoxIsFar(H, SDFBlendRadius)) ? 0.0f : FLT_MAX;
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

            // ⚠️ LE DÉTOUR PAR FVector EST DÉLIBÉRÉ (même raison que dans FSdfRoughnessMod) :
            // FVector est en DOUBLE, donc chaque produit transite par un double avant d'être
            // re-arrondi en float à l'appel du bruit. Sauter l'aller-retour change l'arrondi.
            FVector MainPos(
                WorldX * RF + VoxelHash::SeedOffset(SeedU, 11.3f),
                WorldY * RF + VoxelHash::SeedOffset(SeedU, 13.7f),
                EffectiveZ * RF + VoxelHash::SeedOffset(SeedU, 17.1f)
            );
            FVector FinePos(
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

                const float WarpX = VoxelNoise::Perlin3D(FVector(
                    WorldX * WF + VoxelHash::SeedOffset(SeedU, 5.2f),
                    WorldY * WF + VoxelHash::SeedOffset(SeedU, 1.3f),
                    EffectiveZ * WF + VoxelHash::SeedOffset(SeedU, 9.7f)
                )) * VOXEL_NOISE_SCALE * WS;

                const float WarpY = VoxelNoise::Perlin3D(FVector(
                    WorldX * WF + 100.0f + VoxelHash::SeedOffset(SeedU, 7.7f),
                    WorldY * WF + 200.0f + VoxelHash::SeedOffset(SeedU, 3.1f),
                    EffectiveZ * WF + 300.0f
                )) * VOXEL_NOISE_SCALE * WS;

                const float WarpZ = VoxelNoise::Perlin3D(FVector(
                    WorldX * WF + 400.0f,
                    WorldY * WF + 500.0f + VoxelHash::SeedOffset(SeedU, 11.9f),
                    EffectiveZ * WF + 600.0f + VoxelHash::SeedOffset(SeedU, 13.3f)
                )) * VOXEL_NOISE_SCALE * WS;

                const FVector WarpOffset(WarpX, WarpY, WarpZ);
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
            return VF_CaveBoxIsFar(H, P.SDFBlendRadius) ? EVoxelOpEffect::Identity : Intrinsic;
        }

        /**
         * ✅ Borne consommée par le pliage numérique. `RoughNoise` et `FineNoise` respectent tous
         * deux sont couverts par la borne prouvée `VF_PerlinAbsBound = 1.5`, mis à l'échelle par
         * `VOXEL_NOISE_SCALE`, et `TotalRough = Rough·S + Fine·S·0.4` ⇒
         * `|TotalRough| ≤ 2.1 · S · SCALE`. Le fade est
         * dans `[0,1]`. Le clamp anti-remplissage ne fait que RÉDUIRE côté fill ; on ne s'appuie pas
         * dessus (il ne s'applique que dans l'air certain), donc la borne fill reste la même.
         */
        float MaxCarveOverBox(const FBox&, const FVoxelOpContext&) const override { return MaxAmplitude(); }
        float MaxFillOverBox (const FBox&, const FVoxelOpContext&) const override { return MaxAmplitude(); }

        float MaxCarveOverBox(const FBox& VoxelBox, const FVoxelOpContext& Ctx,
                              const FVoxelBoxHypotheses& H) const override
        {
            return VF_CaveBoxIsFar(H, P.SDFBlendRadius) ? 0.0f : MaxCarveOverBox(VoxelBox, Ctx);
        }

        float MaxFillOverBox(const FBox& VoxelBox, const FVoxelOpContext& Ctx,
                             const FVoxelBoxHypotheses& H) const override
        {
            return VF_CaveBoxIsFar(H, P.SDFBlendRadius) ? 0.0f : MaxFillOverBox(VoxelBox, Ctx);
        }

        /** La borne d'amplitude, en unités de densité. */
        float MaxAmplitude() const
        {
            return (P.SurfaceRoughness > 0.0f)
                 ? (2.1f * P.SurfaceRoughness * VOXEL_NOISE_SCALE) : 0.0f;
        }

    private:
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
                const float DispNoise = HFractal3D(FVector(
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
                                        EVoxelOpEffect::Both);
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
            float LineValue = FMath::Sin(LinePhase);

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
                                        EVoxelOpEffect::CarveOnly);
        }

        /** `LineValue = max(sin,0)³ ∈ [0,1]`, `Fade ∈ [0,1]` ⇒ retrait ≤ `LayerLineDepth`.
         *  ⚠️ Borne calculée sur les params de la STRATE : un op `LayerLines` par salle peut écrire
         *  une profondeur PLUS GRANDE, ce qui rendrait cette borne fausse dans le sens dangereux.
         *  C'est la même dette que la note d'`EffectOverBox` juste au-dessus, et elle doit être
         *  réglée par le même correctif — AVANT que `ClassifyTile` ne consomme `ClassifyBox`. */
        float MaxCarveOverBox(const FBox&, const FVoxelOpContext&) const override
        {
            return (P.LayerLineSpacing > 0.0f) ? FMath::Max(P.LayerLineDepth, 0.0f) : 0.0f;
        }
        float MaxFillOverBox(const FBox&, const FVoxelOpContext&) const override { return 0.0f; }

        float MaxCarveOverBox(const FBox&, const FVoxelOpContext&,
                              const FVoxelBoxHypotheses& H) const override
        {
            return VF_CaveDetailMax(Rooms, H, P.SDFBlendRadius);
        }
        float MaxFillOverBox(const FBox&, const FVoxelOpContext&,
                             const FVoxelBoxHypotheses&) const override { return 0.0f; }

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
            float RibValue = FMath::Sin(RibPhase);

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
                                        EVoxelOpEffect::FillOnly);
        }

        /** `RibValue = max(sin,0)² ∈ [0,1]`, `Fade ∈ [0,1]` ⇒ ajout ≤ `RibbingDepth`.
         *  Même réserve « params de strate » que `FLayerLineMod::MaxCarveOverBox`. */
        float MaxCarveOverBox(const FBox&, const FVoxelOpContext&) const override { return 0.0f; }
        float MaxFillOverBox(const FBox&, const FVoxelOpContext&) const override
        {
            return (P.RibbingSpacing > 0.0f) ? FMath::Max(P.RibbingDepth, 0.0f) : 0.0f;
        }

        float MaxCarveOverBox(const FBox&, const FVoxelOpContext&,
                              const FVoxelBoxHypotheses&) const override { return 0.0f; }
        float MaxFillOverBox(const FBox&, const FVoxelOpContext&,
                             const FVoxelBoxHypotheses& H) const override
        {
            return VF_CaveDetailMax(Rooms, H, P.SDFBlendRadius);
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
            const float OverhangNoise = HFractal3D(FVector(
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
                                        EVoxelOpEffect::FillOnly);
        }

        /** fBM ∈ [-1,1] × `VOXEL_NOISE_SCALE`, lobe positif seulement, `Fade ∈ [0,1]` ⇒ ajout
         *  ≤ `SCALE · Depth · Strength`. Même réserve « params de strate ». */
        float MaxCarveOverBox(const FBox&, const FVoxelOpContext&) const override { return 0.0f; }
        float MaxFillOverBox(const FBox&, const FVoxelOpContext&) const override
        {
            return VOXEL_NOISE_SCALE * VF_PerlinAbsBound
                 * FMath::Max(P.OverhangDepth, 0.0f)
                 * FMath::Max(P.OverhangStrength, 0.0f);
        }

        float MaxCarveOverBox(const FBox&, const FVoxelOpContext&,
                              const FVoxelBoxHypotheses&) const override { return 0.0f; }
        float MaxFillOverBox(const FBox&, const FVoxelOpContext&,
                             const FVoxelBoxHypotheses& H) const override
        {
            return VF_CaveDetailMax(Rooms, H, P.SDFBlendRadius);
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

            const float VertGrad = VoxelNoise::Perlin3D(FVector(
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
                                        EVoxelOpEffect::Both);
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
            const float ScallopNoise = VoxelNoise::Cellular3D(FVector(
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
                                        EVoxelOpEffect::CarveOnly);
        }

        /** `Cellular3D ∈ [-1,1]`, lobe positif seulement, `Fade ∈ [0,1]` ⇒ retrait ≤
         *  `ScallopStrength`. Même réserve « params de strate ». */
        float MaxCarveOverBox(const FBox&, const FVoxelOpContext&) const override
        {
            return VF_PerlinAbsBound * FMath::Max(P.ScallopStrength, 0.0f);
        }
        float MaxFillOverBox(const FBox&, const FVoxelOpContext&) const override { return 0.0f; }

        float MaxCarveOverBox(const FBox&, const FVoxelOpContext&,
                              const FVoxelBoxHypotheses& H) const override
        {
            return VF_CaveDetailMax(Rooms, H, P.SDFBlendRadius);
        }
        float MaxFillOverBox(const FBox&, const FVoxelOpContext&,
                             const FVoxelBoxHypotheses&) const override { return 0.0f; }

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

            const int32 MaxArches = 3;
            const FVector VoxPos(WorldX, WorldY, WorldZ);

            for (int32 i = 0; i < MaxArches; i++)
            {
                const uint32 AH = VoxelHash::Mix(Room.Hash ^ (0xA4C400u + (uint32)i * 7369u));

                if (VoxelHash::ToFloat01(AH) > LP.ArchDensity) { continue; }

                const uint32 AH2 = VoxelHash::Mix(AH ^ 0xA4C4u);
                const float ArcCX = Room.Center.X + VoxelHash::ToFloatSigned(AH2) * Room.RadiusXY * 0.3f;
                const float ArcCY = Room.Center.Y
                                  + VoxelHash::ToFloatSigned(VoxelHash::Mix(AH2)) * Room.RadiusXY * 0.3f;

                const uint32 AH3 = VoxelHash::Mix(AH2 ^ 0xB41Du);
                const float ArcCZ = Room.Center.Z + VoxelHash::ToFloatSigned(AH3) * Room.RadiusZ * 0.4f;

                const uint32 AH4 = VoxelHash::Mix(AH3 ^ 0xCAFEu);
                const float Angle    = VoxelHash::ToFloat01(AH4) * PI;
                const float HalfSpan = Room.RadiusXY
                                     * (0.5f + VoxelHash::ToFloat01(VoxelHash::Mix(AH4)) * 0.35f);

                const float CosA = FMath::Cos(Angle);
                const float SinA = FMath::Sin(Angle);
                const FVector ArchA(ArcCX - CosA * HalfSpan, ArcCY - SinA * HalfSpan, ArcCZ);
                const FVector ArchB(ArcCX + CosA * HalfSpan, ArcCY + SinA * HalfSpan, ArcCZ);

                const uint32 AH5 = VoxelHash::Mix(AH4 ^ 0xF00Du);
                const float ArchRadius = FMath::Lerp(LP.ArchMinRadius, LP.ArchMaxRadius,
                                                     VoxelHash::ToFloat01(AH5));

                const float ArchSDF = VoxelSDF::Capsule(VoxPos, ArchA, ArchB, ArchRadius);

                const float ArchBlend = 2.0f;
                if (ArchSDF < ArchBlend)
                {
                    float Fill = FMath::Clamp((ArchBlend - ArchSDF) / (ArchBlend * 2.0f), 0.0f, 1.0f);
                    Fill = SmoothStep01(Fill);
                    InOut.Density += Fill * LP.BaseDensity * 1.5f;
                }
            }
        }

        /** N'AJOUTE que du solide ⇒ `FillOnly`. La décision spatiale est indépendante : elle
         *  consomme l'intervalle SDF publié par la source, comme les autres détails de salle. */
        EVoxelOpEffect EffectOverBox(const FBox&, const FVoxelOpContext&) const override
        {
            return (P.ArchDensity > 0.0f) ? EVoxelOpEffect::FillOnly : EVoxelOpEffect::Identity;
        }

        EVoxelOpEffect EffectOverBox(const FBox& VoxelBox, const FVoxelOpContext& Ctx,
                                     const FVoxelBoxHypotheses& H) const override
        {
            return VF_CaveDetailEffect(Rooms, H, P.SDFBlendRadius,
                                        EVoxelOpEffect::FillOnly);
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

        void Eval(float WorldX, float WorldY, float, FVoxelOpSample& InOut) const override
        {
            if (!VF_NearCaveSurface(InOut.Sdf, P.SDFBlendRadius)) { return; }
            if (Rooms == nullptr) { return; }

            for (const FCachedColumn& Col : Rooms->GetCache().Columns)
            {
                const float DX = WorldX - Col.CenterX;
                const float DY = WorldY - Col.CenterY;
                const float XYDistSq = DX * DX + DY * DY;
                if (XYDistSq > Col.BoundXYRadiusSq) { continue; }

                const float CylSDF = FMath::Sqrt(XYDistSq) - Col.Radius;

                const float ColBlend = 3.0f;
                if (CylSDF < ColBlend)
                {
                    float Fill = FMath::Clamp((ColBlend - CylSDF) / (ColBlend * 2.0f), 0.0f, 1.0f);
                    Fill = SmoothStep01(Fill);
                    InOut.Density += Fill * Col.BaseDensity * 1.5f;
                }
            }
        }

        /** N'AJOUTE que du solide ⇒ `FillOnly`. On ne peut pas rendre `Identity` sans consulter le
         *  cache pour la boîte interrogée — même dette que le graphe de salles. */
        EVoxelOpEffect EffectOverBox(const FBox&, const FVoxelOpContext&) const override
        {
            return EVoxelOpEffect::FillOnly;
        }

        EVoxelOpEffect EffectOverBox(const FBox& VoxelBox, const FVoxelOpContext& Ctx,
                                     const FVoxelBoxHypotheses& H) const override
        {
            return VF_CaveDetailEffect(Rooms, H, P.SDFBlendRadius,
                                        EVoxelOpEffect::FillOnly);
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
                                        EVoxelOpEffect::CarveOnly);
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

            const float Spread = 0.85f;
            const int32 MaxPinches = 3;

            for (int32 i = 0; i < MaxPinches; i++)
            {
                const uint32 PnH = VoxelHash::Mix(Room.Hash ^ (0xF1C400u + (uint32)i * 5417u));

                if (VoxelHash::ToFloat01(PnH) > LP.PinchDensity) { continue; }

                const uint32 PnH2 = VoxelHash::Mix(PnH ^ 0xF1C4u);
                const float PnX = Room.Center.X + VoxelHash::ToFloatSigned(PnH2) * Room.RadiusXY * Spread;
                const float PnY = Room.Center.Y
                                + VoxelHash::ToFloatSigned(VoxelHash::Mix(PnH2)) * Room.RadiusXY * Spread;

                const uint32 PnH3 = VoxelHash::Mix(PnH2 ^ 0x5432u);
                const float PnZ = Room.Center.Z + VoxelHash::ToFloatSigned(PnH3) * Room.RadiusZ * 0.5f;

                const uint32 PnH4 = VoxelHash::Mix(PnH3 ^ 0x9A3Bu);
                const float PnAngle = VoxelHash::ToFloat01(PnH4) * PI;
                const float CosPN = FMath::Cos(PnAngle);
                const float SinPN = FMath::Sin(PnAngle);

                const float DXPn = WorldX - PnX;
                const float DYPn = WorldY - PnY;
                const float DZPn = WorldZ - PnZ;

                const float MaxExtent = FMath::Max(LP.PinchLength, LP.PinchStrength) + 5.0f;
                if (FMath::Abs(DXPn) + FMath::Abs(DYPn) + FMath::Abs(DZPn) > MaxExtent) { continue; }

                const float Along  =  DXPn * CosPN + DYPn * SinPN;
                const float Across = -DXPn * SinPN + DYPn * CosPN;

                const float HalfLength   = LP.PinchLength * 0.5f;
                const float HalfNarrow   = LP.PinchStrength;
                const float HalfVertical = LP.PinchStrength * 1.5f;

                const float NAlong   = Along   / HalfLength;
                const float NAcross  = Across  / HalfNarrow;
                const float NUp      = DZPn    / HalfVertical;
                const float EllipDist = NAlong * NAlong + NAcross * NAcross + NUp * NUp;

                if (EllipDist < 1.0f)
                {
                    float Fill = 1.0f - EllipDist;
                    Fill = SmoothStep01(Fill);
                    const float AxisDist = FMath::Sqrt(NAcross * NAcross + NUp * NUp);
                    const float SideFactor = FMath::Clamp(AxisDist * 2.0f, 0.0f, 1.0f);
                    InOut.Density += Fill * SideFactor * LP.BaseDensity * 1.5f;
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
                                        EVoxelOpEffect::FillOnly);
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
    // TIER 3b AUDIT (2026-09-04): this op is intentionally still separate. Ten operators lie
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
                                        EVoxelOpEffect::FillOnly);
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
            : P(InP), SeedU((uint32)Seed) {}

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

            const float EffectiveZ = (P.VerticalScale != 1.0f && P.VerticalScale > 0.0f)
                                   ? (WorldZ / P.VerticalScale) : WorldZ;
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

            const float WormZFreq = P.WormFrequency * P.WormHorizontalBias;

            const float N1 = FMath::Abs(VoxelNoise::Perlin3D(FVector(
                WorldX * P.WormFrequency + VoxelHash::SeedOffset(SeedU, 1.0f),
                WorldY * P.WormFrequency + VoxelHash::SeedOffset(SeedU, 1.7f),
                EffectiveZ * WormZFreq + VoxelHash::SeedOffset(SeedU, 2.3f)
            )) * VOXEL_NOISE_SCALE);

            // N2 ≥ 0, donc si N1 dépasse déjà le seuil la somme ne peut plus creuser — on saute le
            // second Perlin (le cas courant ; sortie bit-identique). Transcrit tel quel.
            if (N1 >= P.WormThreshold) { return; }

            const float N2 = FMath::Abs(VoxelNoise::Perlin3D(FVector(
                WorldX * P.WormFrequency + VoxelHash::SeedOffset(SeedU, 1.0f) + 137.0f,
                WorldY * P.WormFrequency + VoxelHash::SeedOffset(SeedU, 1.7f) + 259.0f,
                EffectiveZ * WormZFreq + VoxelHash::SeedOffset(SeedU, 2.3f) + 431.0f
            )) * VOXEL_NOISE_SCALE);

            const float WormValue = N1 + N2;
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
            return (P.WormNetworkRange > 0.0f && H.Sdf.IsKnown()
                    && H.Sdf.Min >= P.WormNetworkRange)
                 ? 0.0f : MaxCarveOverBox(VoxelBox, Ctx);
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
            return (P.WormStrength > 0.0f && P.WormThreshold > 0.0f) ? P.WormStrength : 0.0f;
        }

        const TCHAR* DebugName() const override { return TEXT("WormFieldSource"); }

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
    return D;
}

//=============================================================================
// FVoxelOpStack
//=============================================================================

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
                                         const UVoxelStrateManager* StrateManager)
{
    // ORDRE NON NÉGOCIABLE : la spine creuse l'intérieur (et ne touche JAMAIS les bandes de seal),
    // le seal vertical re-solidifie ses bandes, les passages percent les seals, puis la limite XY
    // gagne sur tout ce qui précède. Ainsi, même un passage placé dans la rampe ou au-delà du rayon
    // ne peut pas ouvrir la coque extérieure. Les éditions joueur restent le dernier post de
    // GetDensityAt, hors de cette pile, comme avant.
    Add(MakeUnique<FOriginSpineOp>(StrateTopWorldZ, StrateBottomWorldZ, SealThickness, BaseDensity, SpineRadius));
    Add(MakeUnique<FBoundarySealOp>(StrateTopWorldZ, StrateBottomWorldZ, SealThickness, BaseDensity));
    Add(MakeUnique<FPassageCarveOp>(StrateManager, BaseDensity, SealThickness));
    Add(MakeUnique<FXYEdgeSealOp>(BaseDensity));
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

    TUniquePtr<IVoxelDensityOp> MakeSurfaceColumnSource(const FSurfaceGenerationParams& P, int32 Seed)
    {
        return MakeUnique<FSurfaceColumnSource>(P, Seed);
    }

    void BuildSurfaceStack(FVoxelOpStack& OutStack, const FSurfaceGenerationParams& P,
                           int32 Seed, float SpineRadius, const UVoxelStrateManager* StrateManager,
                           const TArray<FSurfaceGenerationParams>& PerBiomeParams,
                           TUniquePtr<IVoxelBiomeField> BiomeField,
                           bool bAppendStructuralPosts)
    {
        // ARCHÉTYPE COMPLET depuis l'étape 2c : vide + overhang + mélange de biomes.
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
        // les MÊMES trois ops avec une source différente. C'est exactement ce que `§2.5` prédisait
        // et ce que la Phase 1 avait parié.
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
        // ⚠️ CE PARAGRAPHE ÉTAIT PÉRIMÉ ET DISAIT LE CONTRAIRE DU CODE (corrigé 2026-08-16).
        // Il annonçait « étape A sur trois, les douze modificateurs et l'override par salle ne sont
        // pas encore portés, c'est pour ça que `UsesOperatorStackForChunk` rend **false** pour
        // TunnelNetwork ». Les trois étapes sont terminées : les douze modificateurs sont ajoutés
        // douze lignes plus bas, l'override C1 est en place, et `UsesOperatorStackForChunk` rend
        // **true** pour TunnelNetwork. Un commentaire qui contredit le code sous lui est
        // exactement le piège « lire le code, pas le commentaire » à l'envers.
        //
        // STALE PARAGRAPH REMOVED 2026-08-16. It claimed "stage A of three, the detail modifiers and
        // the per-room override are not ported yet, which is why the archetype is still off in
        // UsesOperatorStackForChunk". All three stages are done, the twelve modifiers are added a
        // dozen lines below, and that function returns TRUE for TunnelNetwork. A comment that
        // contradicts the code beneath it is the "read the code, not the comment" trap in reverse.
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
        // même chose vue de loin. Noté ici plutôt que laissé en TODO permanent.
        constexpr float CarveMinDivisor = 1.0f;   // TunnelNetwork plancher son diviseur, cf. FSdfConvertOp

        // La source de salles est retenue par pointeur non possédant : les terrasses re-interrogent
        // son cache SDF en Z±1. Même motif que `FShaftFieldSource` → `FShaftLedgeMod`.
        TUniquePtr<FRoomGraphSource> RoomSource = MakeUnique<FRoomGraphSource>(P, Seed, StrateManager);
        const FRoomGraphSource* RoomPtr = RoomSource.Get();

        OutStack.Add(MakeConstantRockSource(P.BaseDensity));
        OutStack.Add(MoveTemp(RoomSource));
        OutStack.Add(MakeSdfCarve(P.SDFBlendRadius, P.BaseDensity, CarveMinDivisor));
        // ── ÉTAPE B : les modificateurs de détail (4b–4h), chacun gated sur
        //    `Sdf < SDFBlendRadius·3` via VF_NearCaveSurface. Voir la note de l'étape B5 là-bas.
        //    L'ORDRE EST CELUI DE L'ORIGINAL et il compte : chacun lit la densité que le précédent
        //    a laissée (le biais de sol est un ajout indépendant appliqué à cette accumulation).
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
        // The worm consumes the SDF interval carried by the fold; it has no pointer to, and no
        // dependency on, this particular room source. That keeps the same operator safe behind a
        // different SDF writer assembled by the composer.
        OutStack.Add(MakeUnique<FWormFieldSource>(P, Seed));

        if (bAppendStructuralPosts)
        {
            OutStack.AppendStructuralPost(P.StrateTopWorldZ, P.StrateBottomWorldZ,
                                          P.BoundarySealThickness, P.BaseDensity,
                                          SpineRadius, StrateManager);
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
    if (!FMath::IsFinite(Top) || !FMath::IsFinite(Bottom) || Top <= Bottom
        || !FMath::IsFinite(Seal) || !FMath::IsFinite(Base))
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
    if (!FMath::IsFinite(Top) || !FMath::IsFinite(Bottom) || !(Top > Bottom)
        || !FMath::IsFinite(Seal) || Seal < 0.0f
        || !FMath::IsFinite(Base) || !(Base > 0.0f))
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
