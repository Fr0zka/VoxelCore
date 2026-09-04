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

    //=========================================================================
    // RÔLE 1 — SOURCE : COULOIRS SUR TREILLIS 3D / 3D LATTICE CORRIDORS
    //=========================================================================
    // Chaque nœud du treillis est au centre d'une cellule ; l'arête vers son voisin +X/+Y/+Z est
    // « ouverte » quand un hash de (nœud inférieur, axe) passe BranchProbability (Verticality pour
    // Z). Le couloir est une capsule fine.
    //
    // ⚠️ LA propriété qui fait de Maze le bon premier portage : l'identité d'une arête est
    // (nœud INFÉRIEUR, axe). Deux chunks adjacents calculent donc littéralement le même hash pour
    // l'arête qu'ils partagent — ils NE PEUVENT PAS être en désaccord. Pas de cache de chunk, pas
    // de région COLLECT, pas de discipline d'invariance de fenêtre à maintenir (AUDIT §6.4).
    class FLatticeCorridorSource final : public IVoxelDensityOp
    {
    public:
        FLatticeCorridorSource(const FMazeGenerationParams& P, int32 Seed, float InExtraReach)
            : CellSize(FMath::Max(P.CellSize, 1.0f))
            , CorridorRadius(FMath::Max(P.CorridorRadius, 0.5f))
            , BranchProbability(P.BranchProbability)
            , Verticality(P.Verticality)
            , Salt((uint32)Seed ^ 0x4D617A65u)   // 'Maze' — identique à GetMazeDensity
            , ExtraReach(InExtraReach)
        {}

        EVoxelOpRole GetRole() const override { return EVoxelOpRole::FieldSource; }
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

        // Répond pour la paire source + conversion (SIMPLIFICATION DE PHASE 1, cf. VoxelDensityOp.h).
        // Conservatif par construction : on sur-approxime la boîte de chaque capsule, donc on peut
        // dire CarveOnly à tort (coût CPU) mais jamais Identity à tort (ce serait un trou).
        EVoxelOpEffect EffectOverBox(const FBox& VoxelBox, const FVoxelOpContext&) const override
        {
            const float Reach = CorridorRadius + ExtraReach;
            const FVector Min = VoxelBox.Min - FVector(Reach);
            const FVector Max = VoxelBox.Max + FVector(Reach);

            // Nœuds dont une arête peut atteindre la boîte élargie. Les arêtes partent du nœud
            // INFÉRIEUR vers +1, d'où le -1 sur la borne basse.
            const int32 LoX = FMath::FloorToInt(Min.X / CellSize) - 1;
            const int32 LoY = FMath::FloorToInt(Min.Y / CellSize) - 1;
            const int32 LoZ = FMath::FloorToInt(Min.Z / CellSize) - 1;
            const int32 HiX = FMath::FloorToInt(Max.X / CellSize);
            const int32 HiY = FMath::FloorToInt(Max.Y / CellSize);
            const int32 HiZ = FMath::FloorToInt(Max.Z / CellSize);

            // Garde-fou : une boîte énorme face à une petite CellSize ferait exploser la boucle.
            // Au-delà, on renonce à prouver quoi que ce soit — CarveOnly est toujours SÛR.
            constexpr int64 MaxNodesScanned = 32 * 32 * 32;
            const int64 NodeCount = (int64)(HiX - LoX + 1) * (HiY - LoY + 1) * (HiZ - LoZ + 1);
            if (NodeCount <= 0 || NodeCount > MaxNodesScanned) { return EVoxelOpEffect::CarveOnly; }

            for (int32 nz = LoZ; nz <= HiZ; ++nz)
            for (int32 ny = LoY; ny <= HiY; ++ny)
            for (int32 nx = LoX; nx <= HiX; ++nx)
            {
                const FVector A = NodeCenter(nx, ny, nz);
                if (EdgeOpen(nx, ny, nz, 0xA1u, BranchProbability) && SegmentHitsBox(A, NodeCenter(nx + 1, ny, nz), Min, Max)) return EVoxelOpEffect::CarveOnly;
                if (EdgeOpen(nx, ny, nz, 0xB2u, BranchProbability) && SegmentHitsBox(A, NodeCenter(nx, ny + 1, nz), Min, Max)) return EVoxelOpEffect::CarveOnly;
                if (EdgeOpen(nx, ny, nz, 0xC3u, Verticality)       && SegmentHitsBox(A, NodeCenter(nx, ny, nz + 1), Min, Max)) return EVoxelOpEffect::CarveOnly;
            }

            // Aucun couloir n'atteint cette boîte ⇒ la pile ne peut rien y creuser.
            // C'est le premier saut de tuile que Maze ait jamais eu.
            return EVoxelOpEffect::Identity;
        }

    private:
        struct FEdge { FVector A, B; };

        FVector NodeCenter(int32 X, int32 Y, int32 Z) const
        {
            return FVector((X + 0.5f) * CellSize, (Y + 0.5f) * CellSize, (Z + 0.5f) * CellSize);
        }

        bool EdgeOpen(int32 X, int32 Y, int32 Z, uint32 AxisSalt, float Threshold) const
        {
            uint32 H = VoxelHash::Cell(X, Y, Salt ^ AxisSalt);
            H ^= VoxelHash::Mix((uint32)(Z * 73856093) ^ AxisSalt);
            return VoxelHash::ToFloat01(VoxelHash::Mix(H)) < Threshold;
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
         * Le cache par CELLULE, repris tel quel de GetMazeDensity. Il est `thread_local` et non
         * membre parce que la pile est PARTAGÉE entre workers en lecture — un membre mutable serait
         * une course. C'est aussi exactement ce que fait le code d'aujourd'hui.
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

                // Nodes in {-1,0} per axis cover every edge that can reach this voxel's cell.
                for (int32 dz = -1; dz <= 0; dz++)
                for (int32 dy = -1; dy <= 0; dy++)
                for (int32 dx = -1; dx <= 0; dx++)
                {
                    const int32 nx = Cell.X + dx, ny = Cell.Y + dy, nz = Cell.Z + dz;
                    const FVector A = NodeCenter(nx, ny, nz);

                    if (EdgeOpen(nx, ny, nz, 0xA1u, BranchProbability))
                        MZ_Edges.Add({ A, NodeCenter(nx + 1, ny, nz) });
                    if (EdgeOpen(nx, ny, nz, 0xB2u, BranchProbability))
                        MZ_Edges.Add({ A, NodeCenter(nx, ny + 1, nz) });
                    if (EdgeOpen(nx, ny, nz, 0xC3u, Verticality))
                        MZ_Edges.Add({ A, NodeCenter(nx, ny, nz + 1) });
                }
            }
            return MZ_Edges;
        }

        float  CellSize, CorridorRadius, BranchProbability, Verticality;
        uint32 Salt;
        float  ExtraReach;
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

            // Amplitudes MAXIMALES des deux bruits. Le contrat de `VoxelNoise::FBM` est [-1,1]
            // (noté à sa définition), donc ces bornes sont des garanties, pas des estimations —
            // c'est exactement ce qui autorise un verdict de boîte SÛR.
            // FBM's contract is [-1,1], so these bounds are guarantees, not estimates.
            FloorAmp = VOXEL_NOISE_SCALE * FMath::Max(FloorRoughness, 0.0f);
            CeilAmp  = VOXEL_NOISE_SCALE * FMath::Max(CeilRoughness,  0.0f);
        }

        EVoxelOpRole GetRole() const override { return EVoxelOpRole::FieldSource; }
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
            // l'arrondi. HYPOTHÈSE NON ENCORE VÉRIFIÉE : elle prédit que MazeEquivalence passe de
            // 454 écarts à 0. Si le prochain run montre encore des écarts, c'est que la divergence
            // vient d'ailleurs (candidat suivant : contraction FMA entre unités de compilation).
            //
            // THE FVector ROUND-TRIP IS DELIBERATE — do not "simplify" it. The original goes
            // float -> double (FVector is double in UE5) -> float; going straight through floats
            // skips a rounding step, and under /fp:fast the two paths round in different places.
            // Reproducing the detour reproduces the rounding.
            const FVector NoisePos(WorldX * Frequency, WorldY * Frequency, WorldZ * Frequency);
            InOut.Sdf += VoxelNoise::FBM((float)NoisePos.X, (float)NoisePos.Y, (float)NoisePos.Z,
                                         VoxelGenLOD::Eff(BaseOctaves), 2.0f, 0.5f)
                       * VOXEL_NOISE_SCALE * Strength;
        }

        // Ne touche pas la densité par lui-même ; la source amont a déjà compté son amplitude dans
        // sa portée (`ExtraReach`). Cf. SIMPLIFICATION DE PHASE 1 dans VoxelDensityOp.h.
        EVoxelOpEffect EffectOverBox(const FBox&, const FVoxelOpContext&) const override
        {
            return EVoxelOpEffect::Identity;
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
            return EVoxelOpEffect::Identity;   // la source a répondu pour la paire
        }

        const TCHAR* DebugName() const override { return TEXT("SdfConvertOp"); }

    private:
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
    // Ce qui est perdu : le verdict de boîte exact sur la seule moitié cylindrique. Ce qui est
    // gardé : un `EffectOverBox` conservatif qui teste cercles ET capsules, ce que la version
    // séparée aurait dû faire aussi. À revoir si le profil montre que ça compte.
    //
    // Kept as ONE op against §6's suggestion: the per-voxel connectors derive from the same inner
    // 3×3 roll as the shafts, while a rebuild-only 9×9 collection resolves each 5×5 tree window
    // and fixed ±3 fallback;
    // the downstream ledge mod needs the inner shaft list anyway. What is forfeited is an exact XY
    // box verdict on the cylinder half alone.
    class FShaftFieldSource final : public IVoxelDensityOp
    {
    public:
        FShaftFieldSource(const FVerticalShaftParams& InP, int32 Seed, float InExtraReach,
                          float InSpineRadius)
            : P(InP), Salt((uint32)Seed ^ 0x53686674u)   // 'Shft' — identique à GetVerticalShaftDensity
            , ExtraReach(InExtraReach), SpineRadius(InSpineRadius) {}

        EVoxelOpRole GetRole() const override { return EVoxelOpRole::FieldSource; }
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

        EVoxelOpEffect EffectOverBox(const FBox& VoxelBox, const FVoxelOpContext&) const override
        {
            // La source répond pour la paire source+carve (SIMPLIFICATION DE PHASE 1) : `CarveOnly`
            // si une primitive atteint la boîte, `Identity` sinon. `ExtraReach` couvre la rugosité
            // et le blend en aval — le sous-estimer serait un TROU.
            // L'enveloppe doit couvrir les deux bornes de `Lerp(ShaftMinRadius, ShaftMaxRadius, t)`,
            // pas `ShaftMaxRadius` seul si l'asset inverse les paramètres. The bound must cover
            // both radius endpoints before adding connector and downstream reach.
            const float Pad = FMath::Max3(P.ShaftMinRadius, P.ShaftMaxRadius,
                                          SpineConnectorRadius()) + ExtraReach;
            const FBox Padded = VoxelBox.ExpandBy(Pad);

            const float Spacing = FMath::Max(P.ShaftSpacing, 1.0f);
            const int32 CX0 = FMath::FloorToInt((float)Padded.Min.X / Spacing);
            const int32 CX1 = FMath::FloorToInt((float)Padded.Max.X / Spacing);
            const int32 CY0 = FMath::FloorToInt((float)Padded.Min.Y / Spacing);
            const int32 CY1 = FMath::FloorToInt((float)Padded.Max.Y / Spacing);

            for (int32 cy = CY0; cy <= CY1; ++cy)
            for (int32 cx = CX0; cx <= CX1; ++cx)
            {
                FShaft Sh;
                if (!RollShaft(cx, cy, Sh)) { continue; }
                // Cercle (rayon + marge) contre le rectangle XY : un cylindre est infini en Z, donc
                // la question est purement XY.
                const float R  = Sh.R + ExtraReach;
                const float QX = FMath::Max(0.0f, FMath::Max((float)VoxelBox.Min.X - Sh.X,
                                                             Sh.X - (float)VoxelBox.Max.X));
                const float QY = FMath::Max(0.0f, FMath::Max((float)VoxelBox.Min.Y - Sh.Y,
                                                             Sh.Y - (float)VoxelBox.Max.Y));
                if (QX * QX + QY * QY < R * R) { return EVoxelOpEffect::CarveOnly; }
            }

            //-----------------------------------------------------------------
            // THE STRUCTURAL TREE — enumerate every inner cell that the box can query, then
            // collect its complete 5×5 parent window. This is the EffectOverBox mirror of GetCells:
            // it may over-enumerate, but it must never miss a cached tree capsule.
            //-----------------------------------------------------------------
            constexpr int32 CandidateRadius = 2;
            constexpr int32 FallbackRadius = 3;
            constexpr uint32 TreeSalt = 0x7A11u;
            const float TreeRadius = SpineConnectorRadius();
            const int32 ConnectorCellPad = FMath::Max(
                1, FMath::CeilToInt(TreeRadius / Spacing));
            const int32 TreeEmitRadius = FallbackRadius + ConnectorCellPad;
            const int32 QueryX0 = FMath::FloorToInt((float)VoxelBox.Min.X / Spacing) - 1;
            const int32 QueryX1 = FMath::FloorToInt((float)VoxelBox.Max.X / Spacing) + 1;
            const int32 QueryY0 = FMath::FloorToInt((float)VoxelBox.Min.Y / Spacing) - 1;
            const int32 QueryY1 = FMath::FloorToInt((float)VoxelBox.Max.Y / Spacing) + 1;
            const int32 TreeEmitX0 = FMath::FloorToInt((float)VoxelBox.Min.X / Spacing)
                - TreeEmitRadius;
            const int32 TreeEmitX1 = FMath::FloorToInt((float)VoxelBox.Max.X / Spacing)
                + TreeEmitRadius;
            const int32 TreeEmitY0 = FMath::FloorToInt((float)VoxelBox.Min.Y / Spacing)
                - TreeEmitRadius;
            const int32 TreeEmitY1 = FMath::FloorToInt((float)VoxelBox.Max.Y / Spacing)
                + TreeEmitRadius;

            TArray<FShaft, TInlineAllocator<225>> TreeCandidates;
            TArray<FShaft, TInlineAllocator<81>> TreeEmit;
            for (int32 cy = TreeEmitY0 - FallbackRadius;
                 cy <= TreeEmitY1 + FallbackRadius; ++cy)
            for (int32 cx = TreeEmitX0 - FallbackRadius;
                 cx <= TreeEmitX1 + FallbackRadius; ++cx)
            {
                FShaft Sh;
                if (!RollShaft(cx, cy, Sh)) { continue; }
                TreeCandidates.Add(Sh);
                if (cx >= TreeEmitX0 && cx <= TreeEmitX1
                    && cy >= TreeEmitY0 && cy <= TreeEmitY1)
                {
                    TreeEmit.Add(Sh);
                }
            }

            const float BottomZ = P.StrateBottomWorldZ + P.BoundarySealThickness;
            const float TopZ    = P.StrateTopWorldZ    - P.BoundarySealThickness;
            const float RMinZ = (float)VoxelBox.Min.Z, RMaxZ = (float)VoxelBox.Max.Z;
            const FVector2D CtrXY(0.5f * (float)(VoxelBox.Min.X + VoxelBox.Max.X),
                                  0.5f * (float)(VoxelBox.Min.Y + VoxelBox.Max.Y));
            const float HalfDiagXY = 0.5f * FMath::Sqrt(
                FMath::Square((float)(VoxelBox.Max.X - VoxelBox.Min.X)) +
                FMath::Square((float)(VoxelBox.Max.Y - VoxelBox.Min.Y)));
            auto ConnectorMayReachBox = [&](const FConn& Conn)
            {
                const float ConnReach = Conn.Radius + ExtraReach;
                const float Zc = Conn.A.Z;
                if (RMinZ > Zc + ConnReach || RMaxZ < Zc - ConnReach) { return false; }
                const float DistXY = VF_DistPointSegment2D(
                    CtrXY, FVector2D(Conn.A.X, Conn.A.Y), FVector2D(Conn.B.X, Conn.B.Y));
                return DistXY - HalfDiagXY < ConnReach;
            };

            for (const FShaft& Child : TreeEmit)
            {
                const float ChildOriginSq = FMath::Square(Child.X) + FMath::Square(Child.Y);
                const FShaft* Parent = nullptr;
                float BestDistanceSq = FLT_MAX;
                for (const FShaft& Candidate : TreeCandidates)
                {
                    if (Candidate.CellX == Child.CellX && Candidate.CellY == Child.CellY)
                    {
                        continue;
                    }
                    if (FMath::Abs(Candidate.CellX - Child.CellX) > CandidateRadius
                        || FMath::Abs(Candidate.CellY - Child.CellY) > CandidateRadius)
                    {
                        continue;
                    }

                    const float CandidateOriginSq = FMath::Square(Candidate.X)
                                                   + FMath::Square(Candidate.Y);
                    if (!(CandidateOriginSq < ChildOriginSq))
                    {
                        continue;
                    }

                    const float DistanceSq = FMath::Square(Child.X - Candidate.X)
                                           + FMath::Square(Child.Y - Candidate.Y);
                    const bool bLowerCell = Parent == nullptr
                        || Candidate.CellY < Parent->CellY
                        || (Candidate.CellY == Parent->CellY
                            && Candidate.CellX < Parent->CellX);
                    if (DistanceSq < BestDistanceSq
                        || (DistanceSq == BestDistanceSq && bLowerCell))
                    {
                        BestDistanceSq = DistanceSq;
                        Parent = &Candidate;
                    }
                }

                if (Parent == nullptr
                    && !(FMath::Abs(Child.CellX) <= CandidateRadius
                        && FMath::Abs(Child.CellY) <= CandidateRadius))
                {
                    // Keep EffectOverBox's parent resolver identical to GetCells. A local
                    // minimum chooses the nearest lower-origin shaft in the collected halo; the
                    // monotone origin-distance key proves that the resulting tree reaches the
                    // spine and cannot cycle. If the finite halo is empty, the direct spine is the
                    // conservative final fallback.
                    for (const FShaft& Candidate : TreeCandidates)
                    {
                        if (Candidate.CellX == Child.CellX && Candidate.CellY == Child.CellY)
                        {
                            continue;
                        }
                        if (FMath::Abs(Candidate.CellX - Child.CellX) > FallbackRadius
                            || FMath::Abs(Candidate.CellY - Child.CellY) > FallbackRadius)
                        {
                            continue;
                        }

                        const float CandidateOriginSq = FMath::Square(Candidate.X)
                                                       + FMath::Square(Candidate.Y);
                        if (!(CandidateOriginSq < ChildOriginSq))
                        {
                            continue;
                        }

                        const float DistanceSq = FMath::Square(Child.X - Candidate.X)
                                               + FMath::Square(Child.Y - Candidate.Y);
                        const bool bLowerCell = Parent == nullptr
                            || Candidate.CellY < Parent->CellY
                            || (Candidate.CellY == Parent->CellY
                                && Candidate.CellX < Parent->CellX);
                        if (DistanceSq < BestDistanceSq
                            || (DistanceSq == BestDistanceSq && bLowerCell))
                        {
                            BestDistanceSq = DistanceSq;
                            Parent = &Candidate;
                        }
                    }
                }

                const int32 ParentCellX = Parent != nullptr ? Parent->CellX : 0;
                const int32 ParentCellY = Parent != nullptr ? Parent->CellY : 0;
                const uint32 LinkHash = VoxelHash::Pair(
                    Child.CellX, Child.CellY, ParentCellX, ParentCellY, Salt ^ TreeSalt);
                const float Zc = VF_SelectVerticalTreeConnectorZ(
                    P,
                    P.StrateBottomWorldZ + P.BoundarySealThickness,
                    P.StrateTopWorldZ - P.BoundarySealThickness,
                    LinkHash);
                const FVector ParentPoint = Parent != nullptr
                    ? FVector(Parent->X, Parent->Y, Zc)
                    : FVector(0.0f, 0.0f, Zc);
                const FConn TreeConn{
                    FVector(Child.X, Child.Y, Zc), ParentPoint, TreeRadius };
                if (ConnectorMayReachBox(TreeConn)) { return EVoxelOpEffect::CarveOnly; }
            }

            //-----------------------------------------------------------------
            // LES CONNECTEURS — LES VRAIES CAPSULES, PLUS « un puits existe dans le coin »
            //-----------------------------------------------------------------
            // ⚠️ CE BLOC RENDAIT `CarveOnly` DÈS QU'UN PUITS **EXISTAIT** dans la boîte élargie de
            // `Spacing·1.6 + Pad`, sans jamais regarder un connecteur. Avec les défauts
            // (`ShaftSpacing = 55`, `ShaftDensity = 0.6`) cette boîte élargie couvre ~4×4 cellules,
            // donc une dizaine de puits : la condition était vraie PARTOUT et l'archétype prouvait
            // 0 tuile sur 60. Conservatif, jamais faux — et totalement stérile.
            //
            // Ce qu'on fait à la place : reconstruire les connecteurs comme `GetCells` les
            // construit, et tester la capsule réelle.
            //
            // ⚠️ POURQUOI L'ÉNUMÉRATION EST UN SUR-ENSEMBLE (donc sûre). `Eval` lit les connecteurs
            // du voisinage inner 3×3 de la cellule DE SA REQUÊTE. Une paire random visible depuis
            // un point de la boîte a donc ses deux puits dans [cellules de la boîte] ± 1, qui est
            // exactement la plage balayée ici. Les connecteurs de l'arbre sont testés séparément
            // au-dessus avec leur collecte élargie/émission du halo géométrique. On peut produire des paires que
            // personne ne voit jamais : c'est du `CarveOnly` en trop, pas un trou.
            //
            // ⚠️ ET POURQUOI L'ORDRE (A,B) EST LE MÊME QUE CELUI DE `GetCells`. Le hash de paire est
            // pris sur (A puis B) dans l'ordre d'insertion, et `GetCells` insère en `(dy, dx)`,
            // c'est-à-dire en balayage ligne par ligne. On balaie ici `(cy, cx)`, le même ordre — et
            // un ordre ligne par ligne restreint à une sous-grille garde l'ordre relatif de deux
            // cellules. Donc la même paire reçoit le même `VoxelHash::Pair`, sans supposer que
            // celui-ci soit symétrique.
            //
            // Was: return CarveOnly as soon as any shaft EXISTED within Spacing*1.6 + Pad, which at
            // ShaftSpacing 55 / ShaftDensity 0.6 is true everywhere -- 0 of 60 tiles proved. Now it
            // rebuilds the random links the way GetCells does and tests the real capsule. The pair
            // enumeration is a superset (safe), and the row-major cell order reproduces GetCells'
            // insertion order, so each pair gets the same hash without assuming Pair() is symmetric.
            if (P.CrossConnectChance > 0.0f)
            {
                const int32 QX0 = QueryX0;
                const int32 QX1 = QueryX1;
                const int32 QY0 = QueryY0;
                const int32 QY1 = QueryY1;

                TArray<FShaft, TInlineAllocator<32>> Near;
                for (int32 cy = QY0; cy <= QY1; ++cy)
                for (int32 cx = QX0; cx <= QX1; ++cx)
                {
                    FShaft Sh;
                    if (RollShaft(cx, cy, Sh)) { Near.Add(Sh); }
                }

                // Match GetCells' rule conservatively: any query cell in the central 3x3 may see
                // a spine connector. This is a superset for a box, so it can only make the tile
                // less likely to be classified AllSolid.
                if (SpineRadius > 0.0f
                    && QX0 <= 1 && QX1 >= -1
                    && QY0 <= 1 && QY1 >= -1)
                {
                    Near.Add({0.0f, 0.0f, SpineRadius, 0, 0, true});
                }

                // Z est traité EXACTEMENT (le connecteur est une capsule horizontale à `Zc`), XY de
                // façon conservative. Séparer les deux est bien plus serré qu'une demi-diagonale 3D.
                for (int32 i = 0; i < Near.Num(); ++i)
                for (int32 j = i + 1; j < Near.Num(); ++j)
                {
                    const FShaft& A = Near[i];
                    const FShaft& B = Near[j];
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

                    const float ConnRadius = bSpineConnector
                        ? SpineConnectorRadius() : P.ConnectorRadius;
                    const float Zc = FMath::Lerp(BottomZ, TopZ,
                                                 VoxelHash::ToFloat01(VoxelHash::Mix(PH)));
                    const FConn RandomConn{
                        FVector(A.X, A.Y, Zc), FVector(B.X, B.Y, Zc), ConnRadius };
                    if (ConnectorMayReachBox(RandomConn)) { return EVoxelOpEffect::CarveOnly; }
                }
            }

            return EVoxelOpEffect::Identity;
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
        float  ExtraReach;
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
            return (P.LedgeSpacing > 0.0f && P.LedgeDepth > 0.0f)
                 ? EVoxelOpEffect::FillOnly : EVoxelOpEffect::Identity;
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
        FIslandBlobSource(const FFloatingIslandParams& InP, int32 Seed, float InExtraReach)
            : P(InP), Salt((uint32)Seed ^ 0x49736C64u)   // 'Isld' — identique à GetFloatingIslandDensity
            , ExtraReach(InExtraReach) {}

        EVoxelOpRole GetRole() const override { return EVoxelOpRole::FieldSource; }
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
         * `FillOnly` si une île peut atteindre la boîte, `Identity` sinon — et sur une strate d'îles
         * `Identity` est le cas COURANT, ce qui est tout l'intérêt : combiné à l'`AllAir` de la
         * source constante, c'est la première fois qu'un archétype de grotte peut prouver « tout air »
         * (`OPSTACK-DECOMPOSITION §7`).
         *
         * BORNE, et pourquoi elle est sûre dans les deux directions :
         *   • en XY, `Sdf ≥ DistXY − Rxy` (l'enveloppe ne dépasse jamais `Rxy`), et le warp déplace
         *     le POINT de `WarpAmp · VOXEL_NOISE_SCALE · √2` au plus (FBM ∈ [−1,1] sur DEUX axes
         *     indépendants — voir la note √2 dans le corps) ;
         *   • en Z, `Sdf ≥ WorldZ − TopSurf ≥ WorldZ − TopZ`, donc au-dessus du sommet + marge il
         *     n'y a plus rien à faire. **En dessous, il n'y a PAS de borne** : sous une île, le SDF
         *     vaut ≈ `DistXY` à toute profondeur, donc un mince fil de matière descend le long de
         *     l'axe. C'est le comportement de l'original ; le confondre avec « rien en dessous »
         *     serait un TROU, et c'est pourquoi seule la borne HAUTE est testée.
         *   • `ExtraReach` couvre l'aval (rugosité, blend du fill, creux du SmoothMin ≤ K/6).
         */
        EVoxelOpEffect EffectOverBox(const FBox& VoxelBox, const FVoxelOpContext&) const override
        {
            // ⚠️ √2, PAS 1×. Le warp déplace X et Y par DEUX échantillons de bruit INDÉPENDANTS,
            // chacun borné par `WarpAmp · VOXEL_NOISE_SCALE`. Le déplacement du POINT est donc la
            // diagonale, `WarpMax·√2`, et non `WarpMax`. Une marge à 1× serait fausse de 41 % dans
            // le pire cas — c'est-à-dire un trou dans le coin exact où les deux bruits saturent
            // ensemble. Rare, et c'est précisément ce qui rendrait le bug injoignable en test.
            // TWO independent noise samples ⇒ the point displacement is the diagonal, not one axis.
            constexpr float Sqrt2 = 1.4142136f;
            const float WarpMax = (P.IslandMinRadius + P.IslandMaxRadius) * 0.5f * 0.35f
                                  * VOXEL_NOISE_SCALE * Sqrt2;
            const float Pad = ExtraReach + FMath::Abs(WarpMax);
            const float MaxR = FMath::Max(P.IslandMinRadius, P.IslandMaxRadius);

            const float Spacing = FMath::Max(P.IslandSpacing, 1.0f);
            const FBox Padded = VoxelBox.ExpandBy(MaxR + Pad);
            const int32 CX0 = FMath::FloorToInt((float)Padded.Min.X / Spacing);
            const int32 CX1 = FMath::FloorToInt((float)Padded.Max.X / Spacing);
            const int32 CY0 = FMath::FloorToInt((float)Padded.Min.Y / Spacing);
            const int32 CY1 = FMath::FloorToInt((float)Padded.Max.Y / Spacing);

            for (int32 cy = CY0; cy <= CY1; ++cy)
            for (int32 cx = CX0; cx <= CX1; ++cx)
            {
                FIsland Isl;
                if (!RollIsland(cx, cy, Isl)) { continue; }

                // Entièrement au-dessus du sommet de l'île (+ marge) ⇒ hors d'atteinte.
                if ((float)VoxelBox.Min.Z > Isl.TopZ + Pad) { continue; }

                const float R  = Isl.Rxy + Pad;
                const float QX = FMath::Max(0.0f, FMath::Max((float)VoxelBox.Min.X - Isl.X,
                                                             Isl.X - (float)VoxelBox.Max.X));
                const float QY = FMath::Max(0.0f, FMath::Max((float)VoxelBox.Min.Y - Isl.Y,
                                                             Isl.Y - (float)VoxelBox.Max.Y));
                if (QX * QX + QY * QY < R * R) { return EVoxelOpEffect::FillOnly; }
            }

            return EVoxelOpEffect::Identity;
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
        float  ExtraReach;
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
        // Le VERDICT est mémoïsé, et ce n'est pas du confort : `VF_NoCaveOverBox` fait poser la
        // question par les DOUZE modificateurs de détail pour la même boîte. Sans mémo, une tuile
        // coûterait treize `BuildChunkCache` au lieu d'un.
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
         * Ce que ça débloque, en un mot : `FSdfConvertOp` renvoie déjà `Identity` (« la source a
         * répondu pour la paire ») et les douze modificateurs de détail héritent de ce verdict par
         * `VF_NoCaveOverBox`. Le jour où cette fonction rend `Identity` pour une boîte, **quatorze
         * opérateurs deviennent l'identité d'un coup** et la tuile est prouvable — c'est pour ça que
         * le câblage a été posé à UN endroit et pas treize.
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
         * The criterion is the per-voxel cull lifted from point to box: if no cached primitive can
         * survive its own cull anywhere in the box, Sdf stays FLT_MAX across the whole box and the
         * source — with the converter and all twelve modifiers behind it — is the identity.
         */
        EVoxelOpEffect EffectOverBox(const FBox& VoxelBox, const FVoxelOpContext& Ctx) const override
        {
            if (!(P.RoomDensity > 0.0f && P.RoomSpacing > 0.0f)) { return EVoxelOpEffect::Identity; }

            //-----------------------------------------------------------------
            // 1. LA BOÎTE DOIT TENIR DANS UNE SEULE STRATE, PARAMS COMPRIS
            //-----------------------------------------------------------------
            // ⚠️ C'est la moitié « boîte » de la garde d'AUDIT §C2. `Eval` résout l'index de strate
            // et le pool d'ops PAR CHUNK ; une boîte qui traverse une frontière verrait donc deux
            // graphes de salles différents, et un cache unique n'en représenterait aucun. On ne
            // devine pas lequel : on rend `Both`. Ça arrive au plus sur les tuiles de bord.
            const int32 CZ0 = FMath::FloorToInt((float)VoxelBox.Min.Z / (float)CHUNK_SIZE);
            const int32 CZ1 = FMath::FloorToInt((float)VoxelBox.Max.Z / (float)CHUNK_SIZE);
            const int32 CX0 = FMath::FloorToInt((float)VoxelBox.Min.X / (float)CHUNK_SIZE);
            const int32 CX1 = FMath::FloorToInt((float)VoxelBox.Max.X / (float)CHUNK_SIZE);
            const int32 CY0 = FMath::FloorToInt((float)VoxelBox.Min.Y / (float)CHUNK_SIZE);
            const int32 CY1 = FMath::FloorToInt((float)VoxelBox.Max.Y / (float)CHUNK_SIZE);

            // Une boîte qui couvre des dizaines de chunks n'est de toute façon jamais prouvable ;
            // la borne évite qu'un appelant futur transforme ce test en boucle coûteuse.
            if ((int64)(CX1 - CX0 + 1) * (CY1 - CY0 + 1) * (CZ1 - CZ0 + 1) > 64)
            {
                return EVoxelOpEffect::Both;
            }

            int32 StrateIdx = 0;
            const TArray<FStrateTerrainOpEntry>* TerrainOps = nullptr;
            if (Manager)
            {
                StrateIdx = Manager->GetStrateIndex(((float)CZ0 + 0.5f) * CHUNK_SIZE * VOXEL_SIZE);
                for (int32 CZ = CZ0 + 1; CZ <= CZ1; ++CZ)
                {
                    if (Manager->GetStrateIndex(((float)CZ + 0.5f) * CHUNK_SIZE * VOXEL_SIZE) != StrateIdx)
                    {
                        return EVoxelOpEffect::Both;
                    }
                }

                // ⚠️ LE POOL D'OPS FAIT PARTIE DE LA GÉOMÉTRIE, contrairement à ce qu'on croit en
                // lisant `FCachedRoom` : `BuildChunkCache` s'en sert pour cuire les PITS et les
                // CHEMINÉES (`OpParams` y lit `PitDensity`, `PitMinRadius`…). Passer `nullptr`
                // « puisque la forme des salles n'en dépend pas » sous-bornerait le cache et
                // pourrait rendre `Identity` au-dessus d'un pit réel. Un trou, exactement.
                UVoxelStrateDefinition* Def0 = Manager->GetStrateForChunk(FIntVector(CX0, CY0, CZ0));
                for (int32 CZ = CZ0; CZ <= CZ1; ++CZ)
                for (int32 CY = CY0; CY <= CY1; ++CY)
                for (int32 CX = CX0; CX <= CX1; ++CX)
                {
                    if (Manager->GetStrateForChunk(FIntVector(CX, CY, CZ)) != Def0)
                    {
                        return EVoxelOpEffect::Both;
                    }
                }
                if (Def0) { TerrainOps = &Def0->TerrainOperations; }
            }

            //-----------------------------------------------------------------
            // 2. LE MÉMO — clé complète (§C2 : jamais de clé sans params ni LayoutVersion)
            //-----------------------------------------------------------------
            // `Ctx.LayoutVersion` plutôt que le membre rempli par `PrepareChunk` : rien ne garantit
            // qu'un appelant de `ClassifyBox` ait ouvert un chunk, et une version périmée dans une
            // clé de cache est précisément la régression du 2026-07-27.
            const uint32 LV = Ctx.LayoutVersion;

            FBoxState& B = BoxState();
            if (B.bValid && B.KeyBox == VoxelBox && B.KeyStrate == StrateIdx
                && B.KeySeed == SeedU && B.KeyFingerprint == ParamsFingerprint && B.KeyLayout == LV)
            {
                return B.Verdict;
            }

            //-----------------------------------------------------------------
            // 3. LE CACHE POUR LA BOÎTE INTERROGÉE
            //-----------------------------------------------------------------
            const float Warp = (P.CaveWarpStrength > 0.0f)
                             ? P.CaveWarpStrength * VOXEL_NOISE_SCALE * VF_PerlinAbsBound
                             : 0.0f;

            // `+ 2` : la même marge de gradient que la boîte de recherche de `Eval`.
            VoxelCaveMorphology::BuildChunkCache(
                B.Cache,
                (float)VoxelBox.Min.X - Warp - 2.0f, (float)VoxelBox.Min.Y - Warp - 2.0f,
                (float)VoxelBox.Max.X + Warp + 2.0f, (float)VoxelBox.Max.Y + Warp + 2.0f,
                P, SeedU, StrateIdx, TerrainOps);

            //-----------------------------------------------------------------
            // 4. LE CULL PAR VOXEL, ÉLEVÉ DU POINT À LA BOÎTE
            //-----------------------------------------------------------------
            // Espace de REQUÊTE des salles et des tunnels : XY dilaté du warp, Z passé par `EffZ`
            // (monotone croissante tant que `VerticalScale > 0`, donc min et max se conservent)
            // puis dilaté du warp lui aussi — `Eval` warpe bien les trois axes.
            const FVector QMin((float)VoxelBox.Min.X - Warp,
                               (float)VoxelBox.Min.Y - Warp,
                               EffZ((float)VoxelBox.Min.Z) - Warp);
            const FVector QMax((float)VoxelBox.Max.X + Warp,
                               (float)VoxelBox.Max.Y + Warp,
                               EffZ((float)VoxelBox.Max.Z) + Warp);

            auto SphereHitsBox = [](const FVector& C, float RSq, const FVector& Mn, const FVector& Mx)
            {
                const float dx = FMath::Max3((float)(Mn.X - C.X), 0.0f, (float)(C.X - Mx.X));
                const float dy = FMath::Max3((float)(Mn.Y - C.Y), 0.0f, (float)(C.Y - Mx.Y));
                const float dz = FMath::Max3((float)(Mn.Z - C.Z), 0.0f, (float)(C.Z - Mx.Z));
                return (dx * dx + dy * dy + dz * dz) <= RSq;
            };

            // Pits, cheminées et colonnes : coordonnées RÉELLES, donc boîte NON dilatée.
            const float RMinX = (float)VoxelBox.Min.X, RMaxX = (float)VoxelBox.Max.X;
            const float RMinY = (float)VoxelBox.Min.Y, RMaxY = (float)VoxelBox.Max.Y;
            const float RMinZ = (float)VoxelBox.Min.Z, RMaxZ = (float)VoxelBox.Max.Z;

            auto CircleHitsBoxXY = [&](float CX, float CY, float RSq)
            {
                const float dx = FMath::Max3(RMinX - CX, 0.0f, CX - RMaxX);
                const float dy = FMath::Max3(RMinY - CY, 0.0f, CY - RMaxY);
                return (dx * dx + dy * dy) <= RSq;
            };

            //-----------------------------------------------------------------
            // ⚠️ PAS D'EARLY-OUT : ON COMPTE PAR CLASSE, ET C'EST DÉLIBÉRÉ
            //-----------------------------------------------------------------
            // La version d'origine s'arrêtait à la première primitive atteinte. Elle donnait le bon
            // verdict et AUCUNE information : quand `AllSolid killed by: RoomGraphSource x40` est
            // tombé, il n'y avait aucun moyen de dire si le coupable était les salles, les tunnels
            // ou les pits — donc aucun moyen de savoir quoi resserrer. Compter les cinq classes
            // sépare les causes, et c'est la règle que ce projet a payée plusieurs fois : quand un
            // zéro a plusieurs causes possibles, chacune a son propre nombre.
            //
            // Le coût est nul à l'échelle qui compte : on vient d'appeler `BuildChunkCache`, qui
            // est de plusieurs ordres de grandeur au-dessus d'un parcours de ~100 structs, et le
            // verdict est mémoïsé donc ce parcours arrive UNE fois par boîte, pas treize.
            //
            // No early-out on purpose: stopping at the first hit gives the right verdict and no
            // information. When a zero has several possible causes, each gets its own number.
            //-----------------------------------------------------------------
            // ⚠️⚠️ LE SEUIL `T` — CE QUE `Identity` VEUT DIRE ICI, ET SA CONDITION DE VALIDITÉ
            //-----------------------------------------------------------------
            // Jusqu'ici `Identity` signifiait « `Sdf` reste `FLT_MAX` sur toute la boîte ». C'est
            // vrai, mais c'est plus fort que nécessaire, et cette force coûtait la quasi-totalité du
            // gain : aucun consommateur ne regarde `Sdf` au-delà d'un seuil.
            //
            // Les TROIS consommateurs du canal SDF de cette pile, RELUS un par un (pas supposés) :
            //   • `FSdfConvertOp::Eval`   → `if (InOut.Sdf >= Blend) return;`  et
            //     `BuildTunnelNetworkStack` l'instancie par `MakeSdfCarve(P.SDFBlendRadius, …)`
            //     ⇒ seuil = `K`.
            //   • les DOUZE modificateurs  → `VF_NearCaveSurface` ⇒ seuil = `3·K`.
            //   • `FWormFieldSource::Eval` → `if (CaveSDF >= P.WormNetworkRange) NetworkMask = 0;`
            //     puis `if (NetworkMask <= 0) return;` ⇒ seuil = `WormNetworkRange`.
            // (`FCaveTerraceMod` re-sonde le SDF en Z±1, donc HORS de la boîte — mais son gate
            //  `VF_NearCaveSurface` est testé AVANT la sonde, vérifié ligne par ligne. Un gate faux
            //  partout ⇒ aucune sonde n'est jamais émise.)
            //
            // Donc `Sdf ≥ T` avec `T = max(K, 3K, WormNetworkRange)` suffit à éteindre les trois.
            //
            // ⚠️ **TOUT NOUVEAU CONSOMMATEUR DU CANAL `Sdf` DOIT AVOIR UN SEUIL ≤ T, OU ÊTRE AJOUTÉ
            // À CE `Max`.** C'est la seule dette de couplage de cette fonction, et elle est réelle :
            // un opérateur qui regarderait `Sdf < 100` verrait des verdicts `Identity` faux, donc
            // des tuiles sans géométrie ET SANS COLLISION. Écrit ici parce que c'est ici qu'on
            // atterrit en l'ajoutant.
            //
            // ⚠️ ET LA RAISON POUR LAQUELLE `− K` SUFFIT MALGRÉ N PRIMITIVES. `SmoothMin(A,B,K)`
            // vaut `min(A,B) − H³K/6` avec `H = max(K − |A−B|, 0)/K`. Deux conséquences lues sur la
            // formule : la pénalité est EXACTEMENT nulle dès que `|A−B| ≥ K`, et le minimum courant
            // ne peut donc jamais descendre plus de `K` sous le plus petit des termes — arrivé là,
            // `H = 0` et les plis suivants le laissent intact. D'où `Sdf ≥ min_i(SDF_i) − K` pour un
            // nombre QUELCONQUE de primitives, et non `− N·K/6`. C'est ce qui rend ce critère
            // utilisable au lieu d'être noyé sous le nombre de tunnels.
            //
            // Identity now means "Sdf >= T over the box", not "Sdf stays FLT_MAX" — no consumer
            // looks past its own threshold, and the three that exist were read one by one. ANY NEW
            // CONSUMER OF THE Sdf CHANNEL MUST HAVE A THRESHOLD <= T OR BE ADDED TO THIS MAX.
            // The -K slack covers any number of primitives because SmoothMin's penalty is exactly
            // zero once |A-B| >= K, so the running minimum saturates at K below the true minimum.
            const float K = FMath::Max(P.SDFBlendRadius, 0.0f);
            const float T = FMath::Max(3.0f * K, P.WormNetworkRange);

            B.NumRooms    = B.Cache.Rooms.Num();
            B.NumTunnels  = B.Cache.Tunnels.Num();
            B.NumPits     = B.Cache.Pits.Num();
            B.NumChimneys = B.Cache.Chimneys.Num();
            B.HitRooms = B.HitTunnels = B.HitPits = B.HitChimneys = 0;

            // La MÊME boîte sans dilatation de warp — diagnostic seulement, voir plus bas.
            const FVector NWMin((float)VoxelBox.Min.X, (float)VoxelBox.Min.Y, EffZ((float)VoxelBox.Min.Z));
            const FVector NWMax((float)VoxelBox.Max.X, (float)VoxelBox.Max.Y, EffZ((float)VoxelBox.Max.Z));
            const float NoWarpHalfDiag = 0.5f * (float)(NWMax - NWMin).Size();
            B.HitRoomsNoWarp = B.HitTunnelsNoWarp = 0;
            B.WarpDilation = Warp;

            for (const FCachedRoom& R : B.Cache.Rooms)
            {
                if (SphereHitsBox(R.Center, R.CullRadiusSq, QMin, QMax)) { ++B.HitRooms; }
                if (SphereHitsBox(R.Center, R.CullRadiusSq, NWMin, NWMax)) { ++B.HitRoomsNoWarp; }
            }
            //-----------------------------------------------------------------
            // LES TUNNELS ONT DROIT À UN SECOND TEST, ET C'EST LÀ QUE SE TROUVE LE GAIN
            //-----------------------------------------------------------------
            // ⚠️ CECI CHANGE LE SENS D'`Identity` POUR CET OPÉRATEUR — lire la note « LE SEUIL T »
            // ci-dessus avant de toucher quoi que ce soit ici.
            //
            // Le cull par voxel d'un tunnel est sa SPHÈRE ENGLOBANTE. Pour une capsule longue et
            // fine c'est une sur-estimation énorme : avec `MaxTunnelLength = 200` et
            // `TunnelMaxRadius = 7`, la sphère a un rayon jusqu'à ~107 pour un tube de rayon 7. La
            // mesure le disait sans ambiguïté — 32 tuiles bloquées sur 34 par des tunnels, contre
            // 21 par des salles.
            //
            // Donc : soit le tunnel rate son cull (il ne s'exécute pas), soit son PROPRE SDF reste
            // ≥ `T + K` sur toute la boîte (il s'exécute mais ne peut pas descendre le champ assez
            // bas pour qu'un consommateur s'allume). L'un ou l'autre suffit.
            //
            // La borne est exacte, pas prudente : `TaperedCapsule` rend
            // `Dist(P, PlusProcheSurSegment) − Lerp(Ra, Rb, t)`, donc
            // `SDF ≥ dist(P, segment) − max(Ra, Rb)` — RELU dans `VoxelCaveMorphology.h`, pas supposé.
            // Et `dist(boîte, segment) ≥ dist(centre, segment) − demi-diagonale` par inégalité
            // triangulaire : conservatif du bon côté, et trivialement vrai.
            //
            // A tunnel's per-voxel cull is its BOUNDING SPHERE — for a 200-long tube of radius 7
            // that sphere has radius ~107. So a tunnel does not matter if it fails that cull OR if
            // its own SDF stays >= T + K over the box. The bound is exact: TaperedCapsule is
            // genuinely dist-to-segment minus an interpolated radius, and box-to-segment distance is
            // bounded below by centre-to-segment minus the half-diagonal.
            const float TunnelClear = T + K;
            const FVector QCenter   = (QMin + QMax) * 0.5;
            const float BoxHalfDiag = 0.5f * (float)(QMax - QMin).Size();

            for (const FCachedTunnel& Tn : B.Cache.Tunnels)
            {
                if (!SphereHitsBox(Tn.BoundCenter, Tn.BoundRadiusSq, QMin, QMax)) { continue; }

                float MaxR = FMath::Max(Tn.RadiusA, Tn.RadiusB);
                float DistToAxis;
                if (Tn.bHasMidpoint)
                {
                    // Deux segments : le SDF du tunnel est le `Min` des deux, donc sa borne
                    // inférieure est le `Min` des deux bornes.
                    MaxR = FMath::Max(MaxR, Tn.RadiusMid);
                    DistToAxis = FMath::Min(
                        VF_DistPointSegment(QCenter, Tn.EndpointA, Tn.Midpoint),
                        VF_DistPointSegment(QCenter, Tn.Midpoint,  Tn.EndpointB));
                }
                else
                {
                    DistToAxis = VF_DistPointSegment(QCenter, Tn.EndpointA, Tn.EndpointB);
                }

                if (DistToAxis - BoxHalfDiag - MaxR >= TunnelClear) { continue; }

                ++B.HitTunnels;

                // DIAGNOSTIC — le MÊME test avec une dilatation de warp NULLE. Ne participe à aucun
                // verdict ; il répond à la seule question que trois builds de resserrement n'ont
                // jamais posée : « combien de ce blocage est de la géométrie, et combien est ma
                // propre boîte dilatée ? ». `HitTunnels - HitTunnelsNoWarp` est exactement la part
                // que le warp coûte.
                if (DistToAxis - NoWarpHalfDiag - MaxR < TunnelClear) { ++B.HitTunnelsNoWarp; }
            }
            // Miroir exact des deux `continue` de `Eval` : actif si `Z < TopZ + BlendK` ET
            // `Z >= TopZ - Depth - BlendK`.
            for (const FCachedPit& Pit : B.Cache.Pits)
            {
                if (!(RMinZ < Pit.TopZ + Pit.BlendK))              { continue; }
                if (!(RMaxZ >= Pit.TopZ - Pit.Depth - Pit.BlendK)) { continue; }
                if (CircleHitsBoxXY(Pit.CenterX, Pit.CenterY, Pit.BoundXYRadiusSq)) { ++B.HitPits; }
            }
            // Miroir exact : actif si `Z > BottomZ - BlendK` ET `Z <= BottomZ + Height + BlendK`.
            for (const FCachedChimney& Ch : B.Cache.Chimneys)
            {
                if (!(RMaxZ > Ch.BottomZ - Ch.BlendK))              { continue; }
                if (!(RMinZ <= Ch.BottomZ + Ch.Height + Ch.BlendK)) { continue; }
                if (CircleHitsBoxXY(Ch.CenterX, Ch.CenterY, Ch.BoundXYRadiusSq)) { ++B.HitChimneys; }
            }

            //-----------------------------------------------------------------
            // ✅ LES COLONNES NE SONT PLUS TESTÉES — ET C'EST PROUVÉ, PAS RELÂCHÉ
            //-----------------------------------------------------------------
            // La première version les traitait en cylindres INFINIS en Z (le cache ne leur donne
            // aucune borne verticale), ce qui rendait `Both` pour une boîte située des centaines de
            // voxels sous la salle propriétaire. Inutile : le seul consommateur des colonnes est
            // `FRoomColumnMod`, dont l'`Eval` commence par
            //     `if (!VF_NearCaveSurface(InOut.Sdf, P.SDFBlendRadius)) { return; }`
            // Si aucune salle, aucun tunnel, aucun pit et aucune cheminée n'atteint la boîte, `Sdf`
            // y reste `FLT_MAX`, le gate est faux à chaque voxel, et **aucune colonne ne peut
            // s'exécuter** — quelle que soit sa position XY. Le test était donc REDONDANT, pas
            // prudent. Le retirer resserre le verdict sans toucher à sa correction.
            //
            // Columns are not tested: their only consumer gates on Sdf being near a cave surface,
            // which cannot happen in a box no room/tunnel/pit/chimney reaches. The test was
            // redundant rather than conservative, and it was the loosest one here.
            const bool bReached = (B.HitRooms + B.HitTunnels + B.HitPits + B.HitChimneys) > 0;

            B.Verdict = bReached ? EVoxelOpEffect::Both : EVoxelOpEffect::Identity;
            B.KeyBox = VoxelBox;
            B.KeyStrate = StrateIdx;
            B.KeySeed = SeedU;
            B.KeyFingerprint = ParamsFingerprint;
            B.KeyLayout = LV;
            B.bValid = true;
            return B.Verdict;
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
    // LA CLÉ QUI REND LE PLIAGE NUMÉRIQUE PAYANT : hériter du verdict de la source
    //=========================================================================
    // ⚠️ SANS CECI, LES BORNES D'AMPLITUDE NE SERVENT À RIEN SUR CET ARCHÉTYPE, et c'est le point
    // que `OPSTACK-DECOMPOSITION §0.2` ne dit pas explicitement.
    //
    // Les douze modificateurs de détail sont TOUS gated sur `bNearCaveSurface`, c'est-à-dire sur
    // `Sdf < SDFBlendRadius·3`. Or `Sdf` ne devient fini que si le graphe de salles a écrit quelque
    // chose. **Là où la source prouve qu'aucune salle ni tunnel n'atteint la boîte, `Sdf` reste
    // `FLT_MAX` sur toute la boîte, donc les douze sont l'IDENTITÉ** — et pas seulement « bornés ».
    //
    // Chacun le sait déjà par voxel (son premier `if`) mais le déclarait `Both`/`FillOnly` par boîte,
    // ce qui tuait l'hypothèse `AllSolid` du roc profond aussi sûrement qu'un opérateur réellement
    // actif. Douze déclarations trop prudentes, une seule cause : ils ne consultaient pas la source
    // dont ils dépendent, alors qu'ils en tiennent déjà le pointeur (étape C1).
    //
    // ⚠️ AUJOURD'HUI CE SHORT-CIRCUIT NE TIRE PRESQUE JAMAIS : `FRoomGraphSource::EffectOverBox`
    // rend `Identity` uniquement quand `RoomDensity <= 0`. Il devient l'interrupteur du bedrock
    // profond le jour où la source répond SPATIALEMENT (ses bornes de salles et de tunnels sont déjà
    // dans le cache — il faut le construire pour la boîte interrogée, ce qui ne se paie qu'une fois
    // `ClassifyTile` branché sur `ClassifyBox`). Le câblage est posé maintenant pour que ce jour-là
    // il n'y ait qu'UN endroit à changer, pas treize.
    //
    // Without this, the amplitude bounds buy nothing here: all twelve modifiers are gated on the SDF
    // the room source writes, so where the source proves no cave reaches the box they are IDENTITY,
    // not merely bounded. They already hold the pointer; they simply were not asking.
    FORCEINLINE bool VF_NoCaveOverBox(const FRoomGraphSource* Rooms, const FBox& VoxelBox,
                                      const FVoxelOpContext& Ctx)
    {
        // Pas de source ⇒ `Sdf` reste FLT_MAX ⇒ le gate est faux partout ⇒ identité. Conservatif
        // dans le bon sens : on ne rend `Identity` que quand la source elle-même le rend.
        return Rooms == nullptr || Rooms->EffectOverBox(VoxelBox, Ctx) == EVoxelOpEffect::Identity;
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
        /** @param InRoomsForBox  ⚠️ UNIQUEMENT pour `EffectOverBox`. Cet opérateur lit délibérément
         *  les params de la STRATE et NON `LocalParams()` (voir la note d'en-tête) ; le pointeur ne
         *  sert qu'à hériter du verdict de boîte de la source. Ne pas s'en servir dans `Eval`. */
        FCaveRoughnessMod(const FStrateGenerationParams& InP, int32 Seed,
                          const FRoomGraphSource* InRoomsForBox)
            : P(InP), SeedU((uint32)Seed), RoomsForBox(InRoomsForBox) {}

        EVoxelOpRole GetRole() const override { return EVoxelOpRole::DetailModifier; }
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
         *   |TotalRough| ≤ 1.4 · SurfaceRoughness · VOXEL_NOISE_SCALE,  fade ∈ [0,1].
         * Deuxième client pour le pliage numérique de `OPSTACK-DECOMPOSITION §0.2`, noté au point
         * exact où la borne manque (le premier est `FWormFieldSource::MaxCarveAmplitude`).
         */
        EVoxelOpEffect EffectOverBox(const FBox& VoxelBox, const FVoxelOpContext& Ctx) const override
        {
            if (VF_NoCaveOverBox(RoomsForBox, VoxelBox, Ctx)) { return EVoxelOpEffect::Identity; }
            return (P.SurfaceRoughness > 0.0f) ? EVoxelOpEffect::Both : EVoxelOpEffect::Identity;
        }

        /**
         * ✅ Borne consommée par le pliage numérique. `RoughNoise` et `FineNoise` respectent tous
         * deux le contrat `[-1, 1]` de fBM/Ridged/Cellular, mis à l'échelle par `VOXEL_NOISE_SCALE`,
         * et `TotalRough = Rough·S + Fine·S·0.4` ⇒ `|TotalRough| ≤ 1.4 · S · SCALE`. Le fade est
         * dans `[0,1]`. Le clamp anti-remplissage ne fait que RÉDUIRE côté fill ; on ne s'appuie pas
         * dessus (il ne s'applique que dans l'air certain), donc la borne fill reste la même.
         */
        float MaxCarveOverBox(const FBox&, const FVoxelOpContext&) const override { return MaxAmplitude(); }
        float MaxFillOverBox (const FBox&, const FVoxelOpContext&) const override { return MaxAmplitude(); }

        /** La borne d'amplitude, en unités de densité. */
        float MaxAmplitude() const
        {
            return (P.SurfaceRoughness > 0.0f)
                 ? (1.4f * P.SurfaceRoughness * VOXEL_NOISE_SCALE) : 0.0f;
        }

    private:
        FStrateGenerationParams P;
        uint32 SeedU;
        const FRoomGraphSource* RoomsForBox;   // NON possédant, et NON lu par Eval
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
        EVoxelOpEffect EffectOverBox(const FBox& VoxelBox, const FVoxelOpContext& Ctx) const override
        {
            // Borne au niveau STRATE — voir la note d'`FLayerLineMod::EffectOverBox`.
            if (VF_NoCaveOverBox(Rooms, VoxelBox, Ctx)) { return EVoxelOpEffect::Identity; }
            return (P.TerraceStepHeight > 0.0f) ? EVoxelOpEffect::Both : EVoxelOpEffect::Identity;
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
        EVoxelOpEffect EffectOverBox(const FBox& VoxelBox, const FVoxelOpContext& Ctx) const override
        {
            // ⚠️ VERDICT DE BOÎTE ET OVERRIDE PAR SALLE : la borne se lit sur les params de la
            // STRATE, pas sur ceux d'une salle — une boîte couvre plusieurs salles, donc aucune
            // copie par voxel n'y a de sens. Une salle ne peut qu'ACTIVER un modificateur éteint au
            // niveau strate, jamais l'inverse… sauf que `ApplyTo` écrit la valeur de l'op, y compris
            // quand la strate valait 0. Donc quand une strate a un pool d'ops, ce verdict-ci peut
            // être TROP OPTIMISTE. Aucun risque aujourd'hui : rien ne consomme `ClassifyBox` en
            // production (cf. la file d'attente post-8/8), et il faudra le régler AVANT que
            // `ClassifyTile` ne le consomme. Noté dans OPSTACK-PROGRESS.
            if (VF_NoCaveOverBox(Rooms, VoxelBox, Ctx)) { return EVoxelOpEffect::Identity; }
            return (P.LayerLineSpacing > 0.0f && P.LayerLineDepth > 0.0f)
                 ? EVoxelOpEffect::CarveOnly : EVoxelOpEffect::Identity;
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
        EVoxelOpEffect EffectOverBox(const FBox& VoxelBox, const FVoxelOpContext& Ctx) const override
        {
            // Borne au niveau STRATE — voir la note d'`FLayerLineMod::EffectOverBox`.
            if (VF_NoCaveOverBox(Rooms, VoxelBox, Ctx)) { return EVoxelOpEffect::Identity; }
            return (P.RibbingSpacing > 0.0f && P.RibbingDepth > 0.0f)
                 ? EVoxelOpEffect::FillOnly : EVoxelOpEffect::Identity;
        }

        /** `RibValue = max(sin,0)² ∈ [0,1]`, `Fade ∈ [0,1]` ⇒ ajout ≤ `RibbingDepth`.
         *  Même réserve « params de strate » que `FLayerLineMod::MaxCarveOverBox`. */
        float MaxCarveOverBox(const FBox&, const FVoxelOpContext&) const override { return 0.0f; }
        float MaxFillOverBox(const FBox&, const FVoxelOpContext&) const override
        {
            return (P.RibbingSpacing > 0.0f) ? FMath::Max(P.RibbingDepth, 0.0f) : 0.0f;
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
        EVoxelOpEffect EffectOverBox(const FBox& VoxelBox, const FVoxelOpContext& Ctx) const override
        {
            // Borne au niveau STRATE — voir la note d'`FLayerLineMod::EffectOverBox`.
            if (VF_NoCaveOverBox(Rooms, VoxelBox, Ctx)) { return EVoxelOpEffect::Identity; }
            return (P.OverhangStrength > 0.0f && P.OverhangDepth > 0.0f)
                 ? EVoxelOpEffect::FillOnly : EVoxelOpEffect::Identity;
        }

        /** fBM ∈ [-1,1] × `VOXEL_NOISE_SCALE`, lobe positif seulement, `Fade ∈ [0,1]` ⇒ ajout
         *  ≤ `SCALE · Depth · Strength`. Même réserve « params de strate ». */
        float MaxCarveOverBox(const FBox&, const FVoxelOpContext&) const override { return 0.0f; }
        float MaxFillOverBox(const FBox&, const FVoxelOpContext&) const override
        {
            return VOXEL_NOISE_SCALE * FMath::Max(P.OverhangDepth, 0.0f)
                                     * FMath::Max(P.OverhangStrength, 0.0f);
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
        EVoxelOpEffect EffectOverBox(const FBox& VoxelBox, const FVoxelOpContext& Ctx) const override
        {
            // Borne au niveau STRATE — voir la note d'`FLayerLineMod::EffectOverBox`.
            if (VF_NoCaveOverBox(Rooms, VoxelBox, Ctx)) { return EVoxelOpEffect::Identity; }
            return (P.CliffStrength > 0.0f) ? EVoxelOpEffect::Both : EVoxelOpEffect::Identity;
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
        EVoxelOpEffect EffectOverBox(const FBox& VoxelBox, const FVoxelOpContext& Ctx) const override
        {
            // Borne au niveau STRATE — voir la note d'`FLayerLineMod::EffectOverBox`.
            if (VF_NoCaveOverBox(Rooms, VoxelBox, Ctx)) { return EVoxelOpEffect::Identity; }
            return (P.ScallopStrength > 0.0f) ? EVoxelOpEffect::CarveOnly : EVoxelOpEffect::Identity;
        }

        /** `Cellular3D ∈ [-1,1]`, lobe positif seulement, `Fade ∈ [0,1]` ⇒ retrait ≤
         *  `ScallopStrength`. Même réserve « params de strate ». */
        float MaxCarveOverBox(const FBox&, const FVoxelOpContext&) const override
        {
            return FMath::Max(P.ScallopStrength, 0.0f);
        }
        float MaxFillOverBox(const FBox&, const FVoxelOpContext&) const override { return 0.0f; }

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

        /** N'AJOUTE que du solide ⇒ `FillOnly`. Une vraie borne spatiale existe (les arches vivent
         *  dans le rayon d'une salle) mais elle demande le cache pour la boîte interrogée — même
         *  dette que `FRoomGraphSource::EffectOverBox`, et elle se paiera au même moment. */
        EVoxelOpEffect EffectOverBox(const FBox& VoxelBox, const FVoxelOpContext& Ctx) const override
        {
            // Borne au niveau STRATE — voir la note d'`FLayerLineMod::EffectOverBox`.
            if (VF_NoCaveOverBox(Rooms, VoxelBox, Ctx)) { return EVoxelOpEffect::Identity; }
            return (P.ArchDensity > 0.0f) ? EVoxelOpEffect::FillOnly : EVoxelOpEffect::Identity;
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
        EVoxelOpEffect EffectOverBox(const FBox& VoxelBox, const FVoxelOpContext& Ctx) const override
        {
            if (VF_NoCaveOverBox(Rooms, VoxelBox, Ctx)) { return EVoxelOpEffect::Identity; }
            return EVoxelOpEffect::FillOnly;
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
        EVoxelOpEffect EffectOverBox(const FBox& VoxelBox, const FVoxelOpContext& Ctx) const override
        {
            // Borne au niveau STRATE — voir la note d'`FLayerLineMod::EffectOverBox`.
            if (VF_NoCaveOverBox(Rooms, VoxelBox, Ctx)) { return EVoxelOpEffect::Identity; }
            return (P.DomeDensity > 0.0f) ? EVoxelOpEffect::CarveOnly : EVoxelOpEffect::Identity;
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
        EVoxelOpEffect EffectOverBox(const FBox& VoxelBox, const FVoxelOpContext& Ctx) const override
        {
            // Borne au niveau STRATE — voir la note d'`FLayerLineMod::EffectOverBox`.
            if (VF_NoCaveOverBox(Rooms, VoxelBox, Ctx)) { return EVoxelOpEffect::Identity; }
            return (P.PinchDensity > 0.0f) ? EVoxelOpEffect::FillOnly : EVoxelOpEffect::Identity;
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
    // ⚠️ DERNIER DE LA CHAÎNE, ET CE N'EST PAS INTERCHANGEABLE : il corrige ce que la rugosité (4b)
    // a fait. Le déplacer avant elle le rendrait sans objet. C'est la raison pour laquelle l'ordre
    // des opérateurs dans `BuildTunnelNetworkStack` est celui de l'original, ligne pour ligne.
    class FFloorBiasMod final : public IVoxelDensityOp
    {
    public:
        FFloorBiasMod(const FStrateGenerationParams& InP, const FRoomGraphSource* InRooms)
            : P(InP), Rooms(InRooms) {}

        EVoxelOpRole GetRole() const override { return EVoxelOpRole::DetailModifier; }
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
        EVoxelOpEffect EffectOverBox(const FBox& VoxelBox, const FVoxelOpContext& Ctx) const override
        {
            // Borne au niveau STRATE — voir la note d'`FLayerLineMod::EffectOverBox`.
            if (VF_NoCaveOverBox(Rooms, VoxelBox, Ctx)) { return EVoxelOpEffect::Identity; }
            return (P.FloorBias > 0.0f) ? EVoxelOpEffect::FillOnly : EVoxelOpEffect::Identity;
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
        /** @param InRooms  ⚠️ UNIQUEMENT pour `EffectOverBox` / `MaxCarveOverBox`. `Eval` lit le
         *                  canal SDF de `InOut`, pas ce pointeur — le ver n'interroge jamais la
         *                  source directement, il consomme ce qu'elle a écrit. Peut être nullptr. */
        FWormFieldSource(const FStrateGenerationParams& InP, int32 Seed,
                         const FRoomGraphSource* InRooms = nullptr)
            : P(InP), SeedU((uint32)Seed), Rooms(InRooms) {}

        EVoxelOpRole GetRole() const override { return EVoxelOpRole::FieldSource; }
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
         * ⚠️ `CarveOnly` PARTOUT quand les vers sont actifs — et c'est exactement le problème que
         * `OPSTACK-DECOMPOSITION §0.2` isole : un carve fieldé n'a AUCUNE borne spatiale, donc il tue
         * l'hypothèse `AllSolid` sur CHAQUE tuile de CHAQUE strate à vers. La direction seule ne peut
         * pas le récupérer.
         *
         * **Mais l'amplitude, elle, est bornée et triviale** : `t ∈ [0,1]`, `NetworkMask ∈ [0,1]`,
         * donc ce ver ne peut déplacer la densité vers l'air que de `WormStrength` au plus. Dès que
         * le pliage saura porter un INTERVALLE numérique et pas seulement une direction, « le rocher
         * est solide de plus que la somme des carves restants » redevient prouvable — et c'est le
         * plus gros poste de perf du plan. Noté ici, au point exact où la borne manque.
         */
        /**
         * ✅ **LE VER HÉRITE DU VERDICT DE LA SOURCE DE SALLES — ET C'EST CE QUI DÉBLOQUE TOUT.**
         *
         * La note ci-dessus (« aucune borne spatiale, donc il tue `AllSolid` sur CHAQUE tuile »)
         * était vraie, et pourtant elle passait à côté de ce que son propre `Eval` fait trois
         * lignes plus haut :
         *
         * ```
         * if (CaveSDF >= P.WormNetworkRange)   // vrai aussi quand il n'y a pas de réseau (FLT_MAX)
         * {   NetworkMask = 0.0f;   }
         * ...
         * if (NetworkMask <= 0.0f) { return; }
         * ```
         *
         * **Le ver EST spatialement borné** — pas par une borne à lui, mais par celle de la source
         * de salles, exactement comme les douze modificateurs de détail. Là où `FRoomGraphSource`
         * prouve `Identity`, `Sdf` reste `FLT_MAX` sur toute la boîte, donc `NetworkMask` vaut 0
         * partout, donc ce `return` est pris à chaque voxel. Le ver est l'identité, pas « un carve
         * borné » : il ne s'exécute pas.
         *
         * ⚠️ POURQUOI CE CONTRÔLE COMPTAIT AUTANT. `BaseDensity = 8` et `WormStrength = 10` sont
         * les DÉFAUTS, et le commentaire de `WormStrength` dit pourquoi (« must exceed BaseDensity
         * to create air »). Donc `SolidMargin = 8 − 10 < 0` : tant que le ver rendait `CarveOnly`
         * partout, il tuait `AllSolid` sur **toutes** les tuiles, et la réponse spatiale de la
         * source de salles ne pouvait rien prouver derrière lui. Le premier build l'a montré —
         * 0 tuile prouvée sur 40, la source ayant pourtant appris à répondre.
         *
         * ⚠️ ET POURQUOI ON N'UTILISE **PAS** `VF_NoCaveOverBox` ICI. Cet assistant rend `true`
         * quand `Rooms == nullptr` — correct pour les douze modificateurs, qui n'existent que dans
         * une pile où la source de salles est le seul écrivain du canal SDF. Le ver, lui, est un
         * opérateur dont un futur assemblage pourrait le placer derrière un AUTRE écrivain de SDF
         * (`FLatticeCorridorSource` en écrit un). Sans source de salles, on ne sait pas : on rend
         * `CarveOnly`. Ne pas savoir doit coûter du CPU, jamais un trou.
         *
         * The worm IS spatially bounded — by the room source's bound, not one of its own, exactly
         * like the twelve detail modifiers. Where the room source proves Identity, Sdf stays
         * FLT_MAX, NetworkMask is 0 everywhere and Eval returns immediately. This mattered because
         * BaseDensity=8 < WormStrength=10 BY DEFAULT, so an unconditional CarveOnly killed AllSolid
         * on every tile. Deliberately not VF_NoCaveOverBox: its null-Rooms case answers "identity",
         * which is wrong for an op that could sit behind a different SDF writer.
         */
        EVoxelOpEffect EffectOverBox(const FBox& VoxelBox, const FVoxelOpContext& Ctx) const override
        {
            if (!(P.WormStrength > 0.0f && P.WormThreshold > 0.0f)) { return EVoxelOpEffect::Identity; }

            if (P.WormNetworkRange > 0.0f && Rooms != nullptr
                && Rooms->EffectOverBox(VoxelBox, Ctx) == EVoxelOpEffect::Identity)
            {
                return EVoxelOpEffect::Identity;
            }
            return EVoxelOpEffect::CarveOnly;
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
        float MaxCarveOverBox(const FBox& VoxelBox, const FVoxelOpContext& Ctx) const override
        {
            // Cohérent avec `EffectOverBox` PAR CONSTRUCTION plutôt que par relecture : deux
            // conditions écrites deux fois finiraient par diverger. Le mémo de verdict de
            // `FRoomGraphSource` rend ce second appel gratuit.
            if (EffectOverBox(VoxelBox, Ctx) == EVoxelOpEffect::Identity) { return 0.0f; }
            return MaxCarveAmplitude();
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
        const FRoomGraphSource* Rooms;   // NON possédant — peut être nullptr (voir EffectOverBox)
    };

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

    TUniquePtr<IVoxelDensityOp> MakeLatticeCorridorSource(const FMazeGenerationParams& P, int32 Seed, float ExtraReach)
    {
        return MakeUnique<FLatticeCorridorSource>(P, Seed, ExtraReach);
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
                           TUniquePtr<IVoxelBiomeField> BiomeField)
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

        OutStack.AppendStructuralPost(P.StrateTopWorldZ, P.StrateBottomWorldZ,
                                      P.BoundarySealThickness, P.BaseDensity, SpineRadius, StrateManager);
    }

    void BuildSlabStack(FVoxelOpStack& OutStack, const FSlabGenerationParams& P,
                        int32 Seed, float SpineRadius, const UVoxelStrateManager* StrateManager)
    {
        // DEUX archétypes entrent ici, aucun branchement ne les distingue — parce que
        // `GetSlabDensity` n'en fait aucun non plus. FlatPlain et CrystalChamber ne diffèrent que
        // par leurs valeurs par défaut, et c'est maintenant visible dans le code plutôt que dans
        // un commentaire. 8 archétypes → 7.
        OutStack.Add(MakeSlabVoidSource(P, Seed));
        OutStack.Add(MakeGridColumnMod(P, Seed));

        OutStack.AppendStructuralPost(P.StrateTopWorldZ, P.StrateBottomWorldZ,
                                      P.BoundarySealThickness, P.BaseDensity, SpineRadius, StrateManager);
    }

    void BuildVerticalShaftStack(FVoxelOpStack& OutStack, const FVerticalShaftParams& P,
                                 int32 Seed, float SpineRadius, const UVoxelStrateManager* StrateManager)
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

        // Portée que la source doit déclarer pour la paire source+carve : la rugosité peut élargir
        // `FBM` est normalisé (`Total / MaxValue`), donc sup|FBM| = sup|Perlin3D| = la borne
        // prouvée `VF_PerlinAbsBound` ; la rugosité peut élargir le puits, puis vient le blend du
        // carve. Sur-estimer coûte du CPU ; sous-estimer serait un trou.
        const float ExtraReach = FMath::Abs(P.SurfaceRoughness) * VOXEL_NOISE_SCALE
                               * VF_PerlinAbsBound + CarveBlend + 1.0f;

        TUniquePtr<FShaftFieldSource> ShaftSource =
            MakeUnique<FShaftFieldSource>(P, Seed, ExtraReach, SpineRadius);
        const FShaftFieldSource* ShaftPtr = ShaftSource.Get();

        OutStack.Add(MakeConstantRockSource(P.BaseDensity));
        OutStack.Add(MoveTemp(ShaftSource));
        // Fréquence 0.1 et fenêtre `SurfaceRoughness + 4` — les constantes de
        // `GetVerticalShaftDensity`, PAS celles de Maze (0.12 / `R + rough + 2`). Même opérateur,
        // réglages différents : c'est le point.
        OutStack.Add(MakeSdfRoughnessMod(P.SurfaceRoughness, 0.1f, 3, P.SurfaceRoughness + 4.0f));
        OutStack.Add(MakeSdfCarve(CarveBlend, P.BaseDensity));
        OutStack.Add(MakeUnique<FShaftLedgeMod>(P, ShaftPtr));

        OutStack.AppendStructuralPost(P.StrateTopWorldZ, P.StrateBottomWorldZ,
                                      P.BoundarySealThickness, P.BaseDensity, SpineRadius, StrateManager);
    }

    void BuildTunnelNetworkStack(FVoxelOpStack& OutStack, const FStrateGenerationParams& P,
                                 int32 Seed, float SpineRadius, const UVoxelStrateManager* StrateManager)
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
        //    a laissée (le biais de sol, en particulier, existe pour rattraper la rugosité).
        OutStack.Add(MakeUnique<FCaveRoughnessMod>(P, Seed, RoomPtr));   // 4b
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
        // ⚠️ `RoomPtr` N'EST PAS DÉCORATIF ICI. Le ver hérite du verdict de boîte de la source de
        // salles, faute de quoi il rend `CarveOnly` partout et tue `AllSolid` sur chaque tuile —
        // avec les défauts (`BaseDensity = 8`, `WormStrength = 10`) la marge part négative, donc
        // aucune tuile n'est prouvable, quoi que la source de salles ait réussi à prouver.
        OutStack.Add(MakeUnique<FWormFieldSource>(P, Seed, RoomPtr));

        OutStack.AppendStructuralPost(P.StrateTopWorldZ, P.StrateBottomWorldZ,
                                      P.BoundarySealThickness, P.BaseDensity, SpineRadius, StrateManager);
    }

    void BuildFloatingIslandStack(FVoxelOpStack& OutStack, const FFloatingIslandParams& P,
                                  int32 Seed, float SpineRadius, const UVoxelStrateManager* StrateManager)
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

        // Portée que la source doit déclarer pour la paire source+fill : la rugosité peut abaisser
        // `FBM` est normalisé (`Total / MaxValue`), donc sup|FBM| = sup|Perlin3D| = la borne
        // prouvée `VF_PerlinAbsBound` ; le SDF peut être abaissé par la rugosité, puis par le
        // SmoothMin de `K/6`, et le fill s'applique dès `Sdf < BlendK`. Sur-estimer coûte du CPU ;
        // sous-estimer serait un trou.
        const float ExtraReach = FMath::Abs(P.SurfaceRoughness) * VOXEL_NOISE_SCALE
                               * VF_PerlinAbsBound
                               + BlendK * 2.0f + 1.0f;

        OutStack.Add(MakeConstantVoidSource(P.BaseDensity));
        OutStack.Add(MakeUnique<FIslandBlobSource>(P, Seed, ExtraReach));
        // Fréquence 0.08 et 4 octaves — les constantes de `GetFloatingIslandDensity`. Quatrième
        // archétype à réutiliser cet opérateur (Maze 0.12/3, VerticalShafts 0.1/3).
        OutStack.Add(MakeSdfRoughnessMod(P.SurfaceRoughness, 0.08f, 4,
                                         P.SurfaceRoughness + BlendK + 2.0f));
        OutStack.Add(MakeSdfFill(BlendK, P.BaseDensity));

        OutStack.AppendStructuralPost(P.StrateTopWorldZ, P.StrateBottomWorldZ,
                                      P.BoundarySealThickness, P.BaseDensity, SpineRadius, StrateManager);
    }

    void BuildMazeStack(FVoxelOpStack& OutStack, const FMazeGenerationParams& P,
                        int32 Seed, float SpineRadius, const UVoxelStrateManager* StrateManager)
    {
        // Les constantes viennent telles quelles de GetMazeDensity — elles y étaient codées en dur.
        constexpr float CarveBlend      = 2.0f;
        constexpr float RoughFrequency  = 0.12f;
        constexpr int32 RoughOctaves    = 3;

        const float R = FMath::Max(P.CorridorRadius, 0.5f);

        // Fenêtre d'application de la rugosité : `MazeSDF < R + SurfaceRoughness + 2.0f` dans
        // l'original. Reproduite à l'identique pour que l'égalité binaire tienne.
        const float RoughApplyWithin = R + P.SurfaceRoughness + 2.0f;

        // Portée que la source doit déclarer pour la paire source+carve : le rayon du couloir peut
        // `FBM` est normalisé (`Total / MaxValue`), donc sup|FBM| = sup|Perlin3D| = la borne
        // prouvée `VF_PerlinAbsBound` ; le rayon du couloir peut être élargi par la rugosité puis
        // par le blend du carve. Sur-estimer coûte du CPU ; sous-estimer serait un trou.
        const float ExtraReach = FMath::Abs(P.SurfaceRoughness) * VOXEL_NOISE_SCALE
                               * VF_PerlinAbsBound + CarveBlend + 1.0f;

        OutStack.Add(MakeConstantRockSource(P.BaseDensity));
        OutStack.Add(MakeLatticeCorridorSource(P, Seed, ExtraReach));
        OutStack.Add(MakeSdfRoughnessMod(P.SurfaceRoughness, RoughFrequency, RoughOctaves, RoughApplyWithin));
        OutStack.Add(MakeSdfCarve(CarveBlend, P.BaseDensity));

        OutStack.AppendStructuralPost(P.StrateTopWorldZ, P.StrateBottomWorldZ,
                                      P.BoundarySealThickness, P.BaseDensity, SpineRadius, StrateManager);
    }
}
