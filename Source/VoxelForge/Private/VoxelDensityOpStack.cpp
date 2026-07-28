// VoxelDensityOpStack.cpp
// Les opérateurs concrets de la Phase 1 : la décomposition de Maze + le post-traitement structurel.
// The concrete Phase 1 operators: the Maze decomposition + the structural post-process.
//
// ⚠️ AUCUN de ces opérateurs n'alimente le jeu. Voir l'en-tête de VoxelDensityOpStack.h.
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

#include "VoxelDensityPrimitives.h"   // VF_ApplyOriginSpine / Seal / PassageCarving
#include "VoxelCaveMorphology.h"      // VoxelSDF::Capsule, VoxelHash
#include "VoxelGenerator.h"           // VoxelGenLOD::Eff
#include "VoxelHeightOp.h"            // FVoxelHeightStack — SurfaceWorld's two height stacks
#include "VoxelNoise.h"               // VoxelNoise::FBM
#include "VoxelStrateDefinition.h"    // TerrainOperations — le pool que BuildChunkCache tire par salle
#include "VoxelStrateManager.h"       // EvaluateModifierSDF / AnyPassageNearBox
#include "VoxelTypes.h"               // SmoothStep01, VOXEL_NOISE_SCALE

#include <atomic>                     // l'id d'instance non recyclé du mémo de colonne

namespace
{
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
    FORCEINLINE bool VF_NearCaveSurface(float Sdf, float SDFBlendRadius)
    {
        // Transcrit tel quel, ordre des comparaisons compris :
        //   const float DetailThreshold = Params.SDFBlendRadius * 3.0f;
        //   const bool bNearCaveSurface = (CaveSDF < DetailThreshold) && (CaveSDF < FLT_MAX);
        const float DetailThreshold = SDFBlendRadius * 3.0f;
        return (Sdf < DetailThreshold) && (Sdf < FLT_MAX);
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
         *  Mémoïsée par (instance, X, Y) : la pile évalue tous les Z d'une colonne au même XY, donc
         *  le taux de succès est ~1 et l'overhang lit la MÊME colonne que la source, par
         *  construction plutôt que par convention. */
        struct FColumn { float TerrainZ, CeilSurf, OverhangAmp, DirX, DirY; };

        const FColumn& GetColumn(float WorldX, float WorldY) const
        {
            // ⚠️ POURQUOI UNE TABLE ET PAS UNE SEULE ENTRÉE. Un mémo à une entrée n'est correct que
            // si l'appelant descend une colonne Z avant de changer de XY. Le mesher n'en promet
            // RIEN — s'il itère X en premier dans une tranche Z, chaque voxel raterait et on
            // relancerait toute la pile de hauteur par voxel, cliff compris (4 resamples
            // structurels). Ce n'est pas « un peu plus lent », c'est un ordre de grandeur sur
            // l'archétype le plus cher du plugin.
            //
            // Table à correspondance directe, clé COMPLÈTE comparée sur touche : une collision ne
            // peut que coûter un recalcul, jamais rendre une mauvaise colonne.
            //
            // TAILLE : un chunk fait CHUNK_SIZE² colonnes (1024 à 32³). Les 256 entrées du premier
            // jet ne tenaient donc même pas UN chunk — la table se piétinait elle-même à
            // l'intérieur d'une seule tuile. 4096 entrées couvrent quatre chunks de front, pour
            // ~150 Ko par worker : du même ordre qu'une boîte de `GSurfColCache` (~59 Ko × 6).
            //
            // A chunk is CHUNK_SIZE² columns (1024), so the first draft's 256 entries could not
            // even hold one chunk and thrashed inside a single tile. 4096 covers four chunks.
            struct FSlot { uint64 Key; float X, Y; FColumn C; };
            thread_local FSlot Slots[4096] = {};

            const uint32 HX = *reinterpret_cast<const uint32*>(&WorldX);
            const uint32 HY = *reinterpret_cast<const uint32*>(&WorldY);
            const uint32 Idx = ((HX * 0x9E3779B9u) ^ (HY * 0x85EBCA6Bu)) >> 20;   // [0,4095]

            FSlot& S = Slots[Idx];
            if (S.Key != ColumnKey || S.X != WorldX || S.Y != WorldY)
            {
                S.Key = ColumnKey;  S.X = WorldX;  S.Y = WorldY;
                FColumn& C = S.C;

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
            }
            return S.C;
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
         * délibérément, « shared down the whole vertical strate stack ».
         *
         * Donc la clé devient la même identité : ce qui rend deux colonnes interchangeables, c'est
         * la STRATE et la version de layout, pas le chunk. Le mémo étant `thread_local`, il SURVIT
         * à la reconstruction de la pile — seule la clé l'invalidait.
         *
         * POURQUOI C'EST SÛR : les hauteurs sont XY-pures par construction (c'est tout l'objet de
         * `VoxelHeightOp.h`, où le type n'a pas de Z), et le champ de biomes est documenté
         * XY-pur — « ZERO Z dependence: the climate/Voronoi fields are pure-XY ». C'est exactement
         * la justification sur laquelle `GSurfColCache` repose déjà.
         *
         * The memo was keyed on InstanceId, which changes every chunk, so a 4-chunk strate recomputed
         * every column 4x. GSurfColCache deliberately omits ChunkZ and shares down the whole vertical
         * stack; this now keys on the same identity. Safe because heights are XY-pure by type and the
         * biome field is documented Z-independent.
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
            // ⚠️ PAS de mémo par colonne ICI, délibérément. Le cache T1.a existe déjà UN NIVEAU
            // AU-DESSUS (`GSurfColCache` dans `GetDensityAt`), clé sur (boîte XY, StrateKey, Seed).
            // En rajouter un ici demanderait une seconde clé de cache à tenir juste — et une clé de
            // cache fausse dans un op partagé sur toute la pile verticale est précisément le mode de
            // défaillance qu'`AUDIT §6.3` décrit. Le branchement (étape 2b) réutilise le cache
            // existant plutôt que d'en inventer un second.
            // No per-column memo here on purpose: T1.a already exists one level up, and a second
            // cache key is a second thing to get wrong.
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

            // Marge : le centre d'une colonne vit dans sa cellule, son influence porte au plus
            // MaxRadius + ColBlend. Sur-estimer coûte du CPU ; sous-estimer serait un trou.
            const float Reach = FMath::Max(MaxRadius, 0.0f) + ColBlend;

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

    private:
        float TopZ, BotZ, Thickness, Base;
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

    private:
        const UVoxelStrateManager* Manager;
        float Base, Seal;
    };

    //=========================================================================
    // RÔLE 1 — SOURCE : PUITS VERTICAUX / VERTICAL SHAFTS
    //=========================================================================
    // Cylindres infinis sur une grille XY jitterée + connecteurs horizontaux entre paires proches,
    // ouverts par un hash de paire symétrique. Écrit le canal SDF uniquement.
    //
    // ⚠️ ÉCART ASSUMÉ AVEC `OPSTACK-DECOMPOSITION §6`, qui suggérait DEUX sources (colonnes XY-pures
    // + connecteurs) pour que la moitié cylindrique reçoive le traitement du cache de colonne et un
    // `ClassifyBox` exact en XY. Gardé en UN opérateur, et voici pourquoi :
    //   • les connecteurs se dérivent de la MÊME liste 3×3 que les puits (il faut les paires), donc
    //     séparer imposerait soit de rouler les cellules deux fois, soit un cache partagé entre
    //     deux ops — c'est-à-dire la complexité qu'on voulait éviter ;
    //   • le `FShaftLedgeMod` en aval a de toute façon besoin de la liste des puits, donc il faut
    //     l'exposer depuis une source ; l'exposer depuis deux serait pire.
    // Ce qui est perdu : le verdict de boîte exact sur la seule moitié cylindrique. Ce qui est
    // gardé : un `EffectOverBox` conservatif qui teste cercles ET capsules, ce que la version
    // séparée aurait dû faire aussi. À revoir si le profil montre que ça compte.
    //
    // Kept as ONE op against §6's suggestion: the connectors derive from the same 3×3 roll as the
    // shafts, and the downstream ledge mod needs the shaft list anyway. What is forfeited is an
    // exact XY box verdict on the cylinder half alone.
    class FShaftFieldSource final : public IVoxelDensityOp
    {
    public:
        FShaftFieldSource(const FVerticalShaftParams& InP, int32 Seed, float InExtraReach)
            : P(InP), Salt((uint32)Seed ^ 0x53686674u)   // 'Shft' — identique à GetVerticalShaftDensity
            , ExtraReach(InExtraReach) {}

        EVoxelOpRole GetRole() const override { return EVoxelOpRole::FieldSource; }
        void PrepareChunk(const FVoxelOpContext&) override {}

        struct FShaft { float X, Y, R; };
        struct FConn  { FVector A, B; };

        // ⚠️ DÉCLARÉE ICI, avant toute fonction qui la renvoie. Un type imbriqué doit exister au
        // moment où le COMPILATEUR lit la SIGNATURE — les corps de méthodes sont différés, pas les
        // types de retour. La mettre en bas de la classe donne un C4430 « int par défaut » suivi
        // d'une cascade illisible, ce qui masque une cause pourtant triviale.
        // Declared here, before any function returning it: a nested type must exist when the
        // compiler reads the SIGNATURE — bodies are deferred, return types are not.
        struct FCells
        {
            TArray<FShaft, TInlineAllocator<9>> Shafts;
            TArray<FConn,  TInlineAllocator<8>> Conns;
        };

        void Eval(float WorldX, float WorldY, float WorldZ, FVoxelOpSample& InOut) const override
        {
            const FCells& C = GetCells(WorldX, WorldY);

            float CaveSDF = FLT_MAX;
            for (const FShaft& Sh : C.Shafts)
            {
                const float DX = WorldX - Sh.X;
                const float DY = WorldY - Sh.Y;
                CaveSDF = FMath::Min(CaveSDF, FMath::Sqrt(DX * DX + DY * DY) - Sh.R);
            }
            const FVector Pos(WorldX, WorldY, WorldZ);
            for (const FConn& Cn : C.Conns)
            {
                CaveSDF = FMath::Min(CaveSDF, VoxelSDF::Capsule(Pos, Cn.A, Cn.B, P.ConnectorRadius));
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
            const float Pad = FMath::Max(P.ShaftMaxRadius, P.ConnectorRadius) + ExtraReach;
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

            // ⚠️ Les connecteurs ne sont PAS testés ici, et c'est délibérément conservatif dans le
            // mauvais sens si on n'y prend pas garde : un connecteur ne peut exister qu'entre deux
            // puits d'un voisinage, donc si AUCUN puits n'atteint la boîte élargie de `Spacing*1.6`
            // (la portée max d'une paire), aucun connecteur ne peut l'atteindre non plus.
            const FBox ConnBox = VoxelBox.ExpandBy(Spacing * 1.6f + Pad);
            const int32 KX0 = FMath::FloorToInt((float)ConnBox.Min.X / Spacing);
            const int32 KX1 = FMath::FloorToInt((float)ConnBox.Max.X / Spacing);
            const int32 KY0 = FMath::FloorToInt((float)ConnBox.Min.Y / Spacing);
            const int32 KY1 = FMath::FloorToInt((float)ConnBox.Max.Y / Spacing);
            for (int32 cy = KY0; cy <= KY1; ++cy)
            for (int32 cx = KX0; cx <= KX1; ++cx)
            {
                FShaft Sh;
                if (RollShaft(cx, cy, Sh)) { return EVoxelOpEffect::CarveOnly; }   // prudent
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
            return true;
        }

        /** Le voisinage 3×3 + ses connecteurs, mémoïsés par worker. Clé = cellule + tous les params
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
                                 VS_MaxR = -1.0f, VS_Cross = -1.0f,
                                 VS_BotZ = FLT_MAX, VS_TopZ = FLT_MAX, VS_Seal = -1.0f;

            if (CX != VS_CX || CY != VS_CY || Salt != VS_Salt || Spacing != VS_Spacing ||
                P.ShaftDensity != VS_Dens || P.ShaftMinRadius != VS_MinR || P.ShaftMaxRadius != VS_MaxR ||
                P.CrossConnectChance != VS_Cross ||
                P.StrateBottomWorldZ != VS_BotZ || P.StrateTopWorldZ != VS_TopZ ||
                P.BoundarySealThickness != VS_Seal)
            {
                VS_CX = CX;  VS_CY = CY;  VS_Salt = Salt;  VS_Spacing = Spacing;
                VS_Dens = P.ShaftDensity;  VS_MinR = P.ShaftMinRadius;  VS_MaxR = P.ShaftMaxRadius;
                VS_Cross = P.CrossConnectChance;
                VS_BotZ = P.StrateBottomWorldZ;  VS_TopZ = P.StrateTopWorldZ;
                VS_Seal = P.BoundarySealThickness;
                Cache.Shafts.Reset();
                Cache.Conns.Reset();

                for (int32 dy = -1; dy <= 1; dy++)
                for (int32 dx = -1; dx <= 1; dx++)
                {
                    FShaft Sh;
                    if (RollShaft(CX + dx, CY + dy, Sh)) { Cache.Shafts.Add(Sh); }
                }

                if (P.CrossConnectChance > 0.0f && Cache.Shafts.Num() >= 2)
                {
                    const float BottomZ = P.StrateBottomWorldZ + P.BoundarySealThickness;
                    const float TopZ    = P.StrateTopWorldZ    - P.BoundarySealThickness;
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
                        if (VoxelHash::ToFloat01(PH) >= P.CrossConnectChance) { continue; }

                        const float Zc = FMath::Lerp(BottomZ, TopZ, VoxelHash::ToFloat01(VoxelHash::Mix(PH)));
                        Cache.Conns.Add({ FVector(A.X, A.Y, Zc), FVector(B.X, B.Y, Zc) });
                    }
                }
            }
            return Cache;
        }

        FVerticalShaftParams P;
        uint32 Salt;
        float  ExtraReach;
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
                const float D2 = FMath::Square(WorldX - Sh.X) + FMath::Square(WorldY - Sh.Y);
                if (D2 < BestSq) { BestSq = D2; Near = &Sh; }
            }
            if (Near && (WorldX - Near->X) + (WorldY - Near->Y) > 0.0f)
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
    // À reprendre quand TunnelNetwork arrivera avec le deuxième usage réel.
    //
    // The warp stays INSIDE the op against §7's FRAME suggestion: two of the three frame users are
    // not ported yet, and designing the abstraction against a single example is what this refactor
    // has deliberately avoided. Revisit when TunnelNetwork brings the second real use.
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
            thread_local FChunkSDFCache SDFCache;
            thread_local float  CachedSMinX = 1.0f, CachedSMaxX = -1.0f;   // invalide au départ
            thread_local float  CachedSMinY = 0.0f, CachedSMaxY = 0.0f;
            thread_local int32  CachedStrate = INT32_MIN;
            thread_local uint32 CachedSeed = 0;
            // ⚠️ AJOUTÉ PAR RAPPORT À L'ORIGINAL — la leçon du 2026-07-27 (régression d'overhang).
            // L'original ne clé QUE sur (boîte, strate, seed) : deux jeux de params différents dans
            // la MÊME strate au MÊME seed se servent mutuellement leur cache. En production
            // `RebuildStrates` masque le trou en bougeant la strate ; en test, deux piles construites
            // côte à côte le déclenchent immédiatement. Empreinte CRC des params + LayoutVersion.
            // `FStrateGenerationParams` est du POD pur (aucun TArray/FString/pointeur), donc une CRC
            // mémoire ne peut pas donner un FAUX POSITIF ; au pire un padding donne un faux MANQUE,
            // c'est-à-dire un recalcul. On se trompe du côté du CPU, jamais du côté d'une salle fausse.
            thread_local uint32 CachedFingerprint = 0xFFFFFFFFu;
            thread_local uint32 CachedLayout = 0xFFFFFFFFu;

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
                StrateIdx != CachedStrate || SeedU != CachedSeed ||
                ParamsFingerprint != CachedFingerprint || LayoutVersion != CachedLayout ||
                WarpedX < CachedSMinX || WarpedX > CachedSMaxX ||
                WarpedY < CachedSMinY || WarpedY > CachedSMaxY;

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
                    SDFCache, SMinX, SMinY, SMaxX, SMaxY, P, SeedU, StrateIdx, TerrainOps);

                CachedSMinX = SMinX; CachedSMaxX = SMaxX;
                CachedSMinY = SMinY; CachedSMaxY = SMaxY;
                CachedStrate = StrateIdx;
                CachedSeed = SeedU;
                CachedFingerprint = ParamsFingerprint;
                CachedLayout = LayoutVersion;
            }

            int32 NearestRoom = -1;
            float CaveSDF = VoxelCaveMorphology::EvaluateSDFCached(
                WarpedX, WarpedY, WarpedZ, SDFCache, P.SDFBlendRadius, &NearestRoom);

            //---------------------------------------------------------------
            // PITS & CHEMINÉES — coordonnées RÉELLES, SmoothMin dans le même canal SDF
            //---------------------------------------------------------------
            // C'est le point que `OPSTACK-DECOMPOSITION §2` annonçait comme « le plus retors de toute
            // la décomposition » : deux primitives qui écrivent le MÊME canal que le graphe de salles
            // mais à des coordonnées NON warpées. Sous un modèle de frames il aurait fallu les sortir
            // du frame tout en gardant le canal — exprimable, mais tordu. Dans un opérateur unique la
            // difficulté disparaît : le warp est une variable locale, pas un contexte hérité.
            for (const FCachedPit& Pit : SDFCache.Pits)
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

            for (const FCachedChimney& Chim : SDFCache.Chimneys)
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

        /**
         * ⚠️ `Both` POUR L'INSTANT, ET C'EST UNE DETTE ASSUMÉE, PAS UN OUBLI.
         *
         * Les bornes existent pourtant : `FCachedRoom` / `FCachedTunnel` portent déjà leurs
         * `Bound*` (c'est ce dont `§2` dit qu'il rend le bedrock profond prouvable, « le plus gros
         * poste de perf de tout le plan »). Ce qui manque, c'est que répondre honnêtement demande de
         * consulter le cache — donc de le CONSTRUIRE pour la boîte interrogée, sur le thread qui
         * interroge, ce qui n'est raisonnable qu'une fois `ClassifyBox` réellement branché dans
         * `ClassifyTile` (il ne l'est toujours pas). Rendre `Both` coûte du CPU et ne peut pas faire
         * de trou ; rendre le mauvais en ferait un.
         *
         * Conservative placeholder: the room/tunnel bounds needed for a real answer are already in
         * the cache, but answering means building that cache for the queried box, which only pays
         * once ClassifyTile actually consumes ClassifyBox. Both is always safe.
         */
        EVoxelOpEffect EffectOverBox(const FBox&, const FVoxelOpContext&) const override
        {
            return (P.RoomDensity > 0.0f && P.RoomSpacing > 0.0f) ? EVoxelOpEffect::Both
                                                                  : EVoxelOpEffect::Identity;
        }

    private:
        FStrateGenerationParams P;
        int32  Seed;
        uint32 SeedU;
        const UVoxelStrateManager* Manager;   // NON possédant
        uint32 ParamsFingerprint;
        uint32 LayoutVersion = 0;
    };

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
        EVoxelOpEffect EffectOverBox(const FBox&, const FVoxelOpContext&) const override
        {
            return (P.SurfaceRoughness > 0.0f) ? EVoxelOpEffect::Both : EVoxelOpEffect::Identity;
        }

        /** La borne d'amplitude, en unités de densité. Pas encore consommée par le pliage. */
        float MaxAmplitude() const
        {
            return (P.SurfaceRoughness > 0.0f)
                 ? (1.4f * P.SurfaceRoughness * VOXEL_NOISE_SCALE) : 0.0f;
        }

    private:
        FStrateGenerationParams P;
        uint32 SeedU;
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
        EVoxelOpEffect EffectOverBox(const FBox&, const FVoxelOpContext&) const override
        {
            return (P.WormStrength > 0.0f && P.WormThreshold > 0.0f) ? EVoxelOpEffect::CarveOnly
                                                                     : EVoxelOpEffect::Identity;
        }

        /** L'amplitude max de carve, en unités de densité. Pas encore consommée par le pliage —
         *  posée ici pour que la borne de `§0.2` ait déjà un domicile quand les intervalles
         *  arriveront. / The bound §0.2 needs, given a home before it has a consumer. */
        float MaxCarveAmplitude() const
        {
            return (P.WormStrength > 0.0f && P.WormThreshold > 0.0f) ? P.WormStrength : 0.0f;
        }

    private:
        FStrateGenerationParams P;
        uint32 SeedU;
    };

}   // ⚠️ FIN DU NAMESPACE ANONYME — TOUT NOUVEL OPÉRATEUR SE MET AU-DESSUS DE CETTE LIGNE.
    // Même piège que dans VoxelHeightOpStack.cpp : s'ancrer sur une bannière située plus bas
    // (« FVoxelOpStack », « FABRIQUES ») insère la classe HORS du namespace anonyme, et l'accolade
    // ajoutée avec elle ne ferme rien → C2059.
    // END OF THE ANONYMOUS NAMESPACE — new operators go ABOVE this line.

//=============================================================================
// FVoxelOpStack
//=============================================================================

void FVoxelOpStack::AppendStructuralPost(float StrateTopWorldZ, float StrateBottomWorldZ,
                                         float SealThickness, float BaseDensity, float SpineRadius,
                                         const UVoxelStrateManager* StrateManager)
{
    // ORDRE NON NÉGOCIABLE, et c'est l'ordre que les six fonctions de densité utilisent déjà :
    // la spine creuse l'intérieur (et ne touche JAMAIS les bandes de seal), le seal re-solidifie
    // ses bandes, les passages percent tout — seal compris —, le joueur gagne en dernier.
    Add(MakeUnique<FOriginSpineOp>(StrateTopWorldZ, StrateBottomWorldZ, SealThickness, BaseDensity, SpineRadius));
    Add(MakeUnique<FBoundarySealOp>(StrateTopWorldZ, StrateBottomWorldZ, SealThickness, BaseDensity));
    Add(MakeUnique<FPassageCarveOp>(StrateManager, BaseDensity, SealThickness));
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
        // le puits (FBM ∈ [-1,1] ⇒ ±Strength·VOXEL_NOISE_SCALE), puis le blend du carve. Sur-estimer
        // coûte du CPU ; sous-estimer serait un trou.
        const float ExtraReach = FMath::Abs(P.SurfaceRoughness) * VOXEL_NOISE_SCALE + CarveBlend + 1.0f;

        TUniquePtr<FShaftFieldSource> ShaftSource = MakeUnique<FShaftFieldSource>(P, Seed, ExtraReach);
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
        // ⚠️ ÉTAPES A + B1 — LA PILE EST ENCORE INCOMPLÈTE, ET DÉLIBÉRÉMENT.
        // Sont portés : l'échelle verticale, le roc de base, le warp, le graphe de salles (+ pits
        // + cheminées), le carve, **la rugosité de paroi (4b)**, les vers, le post structurel.
        // **NE SONT PAS ENCORE PORTÉS** les onze modificateurs restants de l'étape 4c-4h (terrasses,
        // lignes de strates, nervures, surplombs, falaise, festons, arches, colonnes, dômes,
        // pincement, biais de sol), ni l'override d'op PAR SALLE.
        //
        // C'est pour cela que `UsesOperatorStackForChunk` rend encore **false** pour TunnelNetwork :
        // brancher une pile incomplète sur le monde en retirerait tout le détail. Le test compare
        // avec ces amplitudes MISES À ZÉRO, donc l'étape A est entièrement vérifiable dès
        // maintenant au lieu d'attendre ~600 lignes de plus — c'est la même discipline que la passe
        // « défauts puis tous les ops ON » du test de la pile de hauteur.
        //
        // STAGE A OF THREE, deliberately incomplete: the 13 detail modifiers and the per-room op
        // override are not ported yet, which is why the archetype is still off in
        // UsesOperatorStackForChunk. The test zeroes those amplitudes so stage A is verifiable now.
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

        OutStack.Add(MakeConstantRockSource(P.BaseDensity));
        OutStack.Add(MakeUnique<FRoomGraphSource>(P, Seed, StrateManager));
        OutStack.Add(MakeSdfCarve(P.SDFBlendRadius, P.BaseDensity, CarveMinDivisor));
        // ── ÉTAPE B : les modificateurs de détail (4b–4h), chacun gated sur
        //    `Sdf < SDFBlendRadius·3` via VF_NearCaveSurface. Voir la note de l'étape B5 là-bas.
        OutStack.Add(MakeUnique<FCaveRoughnessMod>(P, Seed));       // 4b
        // [ÉTAPES B2–B4 iront ici : terrasses, lignes, nervures, surplombs, falaise, festons,
        //  arches, colonnes, dômes, pincement, biais de sol]
        OutStack.Add(MakeUnique<FWormFieldSource>(P, Seed));

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
        // le SDF de `Rough·VOXEL_NOISE_SCALE` (FBM ∈ [-1,1]), le SmoothMin de `K/6` de plus, et le
        // fill s'applique dès `Sdf < BlendK`. Sur-estimer coûte du CPU ; sous-estimer serait un trou.
        const float ExtraReach = FMath::Abs(P.SurfaceRoughness) * VOXEL_NOISE_SCALE
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
        // être élargi par la rugosité (FBM ∈ [-1,1] ⇒ ±Strength·VOXEL_NOISE_SCALE) puis par le blend
        // du carve. Sur-estimer coûte du CPU ; sous-estimer serait un trou.
        const float ExtraReach = FMath::Abs(P.SurfaceRoughness) * VOXEL_NOISE_SCALE + CarveBlend + 1.0f;

        OutStack.Add(MakeConstantRockSource(P.BaseDensity));
        OutStack.Add(MakeLatticeCorridorSource(P, Seed, ExtraReach));
        OutStack.Add(MakeSdfRoughnessMod(P.SurfaceRoughness, RoughFrequency, RoughOctaves, RoughApplyWithin));
        OutStack.Add(MakeSdfCarve(CarveBlend, P.BaseDensity));

        OutStack.AppendStructuralPost(P.StrateTopWorldZ, P.StrateBottomWorldZ,
                                      P.BoundarySealThickness, P.BaseDensity, SpineRadius, StrateManager);
    }
}
