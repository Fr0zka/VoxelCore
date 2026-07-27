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
#include "VoxelNoise.h"               // VoxelNoise::FBM
#include "VoxelStrateManager.h"       // EvaluateModifierSDF / AnyPassageNearBox
#include "VoxelTypes.h"               // SmoothStep01, VOXEL_NOISE_SCALE

namespace
{
    //=========================================================================
    // RÔLE 1 — SOURCE : ROC CONSTANT / CONSTANT ROCK
    //=========================================================================
    // `float Density = Params.BaseDensity;  // start solid` — la première ligne de TunnelNetwork,
    // de Maze ET de VerticalShafts. Trois archétypes, une ligne, désormais un opérateur.
    class FConstantRockSource final : public IVoxelDensityOp
    {
    public:
        explicit FConstantRockSource(float InBaseDensity) : BaseDensity(InBaseDensity) {}

        EVoxelOpRole GetRole() const override { return EVoxelOpRole::FieldSource; }
        void PrepareChunk(const FVoxelOpContext&) override {}
        bool IsXYPure() const override { return true; }   // constant ⇒ trivialement sans Z

        void Eval(float, float, float, FVoxelOpSample& InOut) const override
        {
            InOut.Density = BaseDensity;   // Replace : racine de pile, ignore l'entrée
        }

        // Exact et gratuit : une constante positive est solide partout. C'est ce qui donne aux
        // strates de grotte une hypothèse AllSolid de départ — elles n'en ont jamais eu.
        EVoxelTileClass ClassifyBox(const FBox&, const FVoxelOpContext&) const override
        {
            return (BaseDensity > 0.0f) ? EVoxelTileClass::AllSolid : EVoxelTileClass::Mixed;
        }

        EVoxelOpEffect EffectOverBox(const FBox&, const FVoxelOpContext&) const override
        {
            return EVoxelOpEffect::Both;   // jamais atteint : ClassifyBox répond avant
        }

    private:
        float BaseDensity;
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
            // VoxelNoise::FBM est exactement ce que FractalNoise3D appelle (VoxelGenerator.cpp) —
            // le wrapper ne fait que transtyper. T2.b : les octaves passent par Eff() pour que les
            // tuiles lointaines perdent les octaves sous-cellule.
            InOut.Sdf += VoxelNoise::FBM(WorldX * Frequency, WorldY * Frequency, WorldZ * Frequency,
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
    // RÔLE 2 — COMBINER : SDF → DENSITÉ (CARVE)
    //=========================================================================
    // Les six mêmes lignes dans TunnelNetwork, Maze et VerticalShafts. Une fois ici, plus jamais.
    class FSdfCarveOp final : public IVoxelDensityOp
    {
    public:
        FSdfCarveOp(float InBlend, float InBaseDensity) : Blend(InBlend), BaseDensity(InBaseDensity) {}

        EVoxelOpRole GetRole() const override { return EVoxelOpRole::Combiner; }
        void PrepareChunk(const FVoxelOpContext&) override {}

        void Eval(float, float, float, FVoxelOpSample& InOut) const override
        {
            if (InOut.Sdf >= Blend) { return; }
            float Carve = FMath::Clamp((Blend - InOut.Sdf) / (Blend * 2.0f), 0.0f, 1.0f);
            Carve = SmoothStep01(Carve);
            InOut.Density -= Carve * BaseDensity * 2.0f;   // interne : baisser = vers l'air
        }

        EVoxelOpEffect EffectOverBox(const FBox&, const FVoxelOpContext&) const override
        {
            return EVoxelOpEffect::Identity;   // la source a répondu pour la paire
        }

    private:
        float Blend, BaseDensity;
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
}

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
        return MakeUnique<FConstantRockSource>(BaseDensity);
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

    TUniquePtr<IVoxelDensityOp> MakeSdfCarve(float Blend, float BaseDensity)
    {
        return MakeUnique<FSdfCarveOp>(Blend, BaseDensity);
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
