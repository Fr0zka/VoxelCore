// VoxelHeightOpStack.cpp
// Les cinq opérateurs d'espace-hauteur de SurfaceWorld.
// The five height-space operators of SurfaceWorld.
//
// FIDÉLITÉ / FIDELITY
// Chaque corps est une transcription LITTÉRALE du bloc correspondant de
// `SampleSurfaceStructuralZ` / `ComputeSurfaceTerrainZ` — mêmes offsets, mêmes octaves, même ordre
// d'opérations flottantes. Depuis que `FPSemantics = Precise` est posé (AUDIT §C9), l'égalité
// BIT À BIT est atteignable et atteinte pour Maze et Slab : c'est donc la barre ici aussi, et
// `VoxelForge.OpStack.SurfaceHeightEquivalence` la vérifie.
//
// ⚠️ LE DÉTOUR PAR `FVector` EST DÉLIBÉRÉ, comme ailleurs dans ce refactor : `FractalNoise3D` prend
// un `FVector` (donc des DOUBLES en UE5) et re-descend en float. Passer directement des floats
// saute un arrondi. Reproduire le détour, c'est reproduire l'arrondi.
// The FVector round-trip is deliberate: FVector is double in UE5, so the original rounds through a
// double. Going straight through floats skips a rounding step.

#include "VoxelHeightOp.h"

#include "VoxelCaveMorphology.h"   // VoxelHash::SeedOffset — AUDIT §C1 (bounded, site-salted)
#include "VoxelNoise.h"    // VoxelNoise::FBM / Ridged / Perlin3D
#include "VoxelTypes.h"    // SmoothStep01, VOXEL_NOISE_SCALE

namespace
{
    //=========================================================================
    // HELPERS — les mêmes enveloppes que VoxelGenerator.cpp, transcrites
    //=========================================================================
    // `FractalNoise3D` et `RidgedNoise3D` sont `static` dans VoxelGenerator.cpp, donc invisibles
    // ici. Elles sont recopiées à l'identique plutôt qu'exportées : les exporter changerait leur
    // contexte d'inlining, et sous /fp:precise comme sous /fp:fast la règle est la même — on ne
    // touche à rien de ce qui entoure une expression flottante qu'on veut reproduire.
    FORCEINLINE float HFractalNoise3D(const FVector& Position, int32 Octaves = 4,
                                      float Lacunarity = 2.0f, float Persistence = 0.5f)
    {
        return VoxelNoise::FBM((float)Position.X, (float)Position.Y, (float)Position.Z,
                               Octaves, Lacunarity, Persistence);
    }

    FORCEINLINE float HRidgedNoise3D(const FVector& Position, int32 Octaves = 4,
                                     float Lacunarity = 2.0f, float Persistence = 0.5f)
    {
        return VoxelNoise::Ridged((float)Position.X, (float)Position.Y, (float)Position.Z,
                                  Octaves, Lacunarity, Persistence);
    }

    /** Transcription de `UVoxelGenerator::SampleRelief`. Champ [0,1] partagé avec la carte de
     *  biomes, pour que la géographie et le terrain qu'elle module restent d'accord. */
    FORCEINLINE float HSampleRelief(float WorldX, float WorldY, uint32 SeedU,
                                    float Frequency, float Contrast)
    {
        float R = HFractalNoise3D(FVector(
            WorldX * Frequency + VoxelHash::SeedOffset(SeedU, 7.3f),
            WorldY * Frequency + VoxelHash::SeedOffset(SeedU, 2.1f),
            VoxelHash::SeedOffset(SeedU, 0.5f)), 2) * 0.5f + 0.5f;                 // [0,1]
        R = FMath::Clamp((R - 0.5f) * Contrast + 0.5f, 0.0f, 1.0f);
        return SmoothStep01(R);
    }

    //=========================================================================
    // SOURCE — CHAMP STRUCTUREL / STRUCTURAL HEIGHT FIELD
    //=========================================================================
    // Continents + montagnes + détail, sous une frame de domain-warp. Produit les DEUX canaux.
    class FStructuralHeightSource final : public IVoxelHeightOp
    {
    public:
        FStructuralHeightSource(const FSurfaceGenerationParams& InP, int32 InSeed)
            : P(InP), SeedU((uint32)InSeed) {}

        void Eval(float WorldX, float WorldY, FVoxelHeightSample& InOut) const override
        {
            float M = 1.0f;
            InOut.Height = SampleZ(WorldX, WorldY, M);   // Replace : racine de pile
            InOut.Relief = M;
        }

        /**
         * Le champ nu, exposé parce que `FCliffHeightMod` doit le RÉ-ÉCHANTILLONNER en différences
         * centrées. C'est une dépendance réelle du code d'origine (`ComputeSurfaceTerrainZ` appelle
         * `SampleSurfaceStructuralZ` quatre fois de plus), pas un raccourci : la pente doit venir du
         * champ STRUCTUREL, sans rétroaction des ops, sinon le cliff se nourrirait de lui-même.
         */
        float SampleZ(float WorldX, float WorldY, float& OutM) const
        {
            const float H = P.StrateTopWorldZ - P.StrateBottomWorldZ;
            const float BottomZ = P.StrateBottomWorldZ;

            const float GroundBase = BottomZ + H * P.BaseGroundRelative;

            // Domain-warp des coords STRUCTURELLES (continents + montagnes). Le bruit de détail
            // reste sur le vrai XY pour que les bosses fines restent nettes et décorrélées.
            float QX = WorldX, QY = WorldY;
            if (P.HeightWarpStrength > 0.0f)
            {
                const float WF = P.HeightWarpFrequency;
                const float wx = VoxelNoise::Perlin3D(FVector(WorldX * WF + VoxelHash::SeedOffset(SeedU, 0.31f), WorldY * WF + 4.2f, VoxelHash::SeedOffset(SeedU, 1.7f)));
                const float wy = VoxelNoise::Perlin3D(FVector(WorldX * WF + 8.6f, WorldY * WF + VoxelHash::SeedOffset(SeedU, 0.53f), VoxelHash::SeedOffset(SeedU, 2.9f)));
                QX += wx * VOXEL_NOISE_SCALE * P.HeightWarpStrength;
                QY += wy * VOXEL_NOISE_SCALE * P.HeightWarpStrength;
            }

            const float Relief = HSampleRelief(WorldX, WorldY, SeedU, P.ReliefFrequency, P.ReliefContrast);
            const float M = FMath::Lerp(1.0f, Relief, P.ReliefStrength);

            float Cont = HFractalNoise3D(FVector(
                QX * P.ContinentFrequency + VoxelHash::SeedOffset(SeedU, 3.1f),
                QY * P.ContinentFrequency + VoxelHash::SeedOffset(SeedU, 5.7f),
                VoxelHash::SeedOffset(SeedU, 0.7f)), 4);  // [-1,1]

            float Detail = HFractalNoise3D(FVector(
                WorldX * P.DetailFrequency + 11.0f,
                WorldY * P.DetailFrequency + 22.0f,
                VoxelHash::SeedOffset(SeedU, 1.3f)), 3);  // [-1,1]

            float Mountain = 0.0f;
            if (P.MountainStrength > 0.0f)
            {
                float Ridge = HRidgedNoise3D(FVector(
                    QX * P.MountainFrequency + 99.0f,
                    QY * P.MountainFrequency + 77.0f,
                    VoxelHash::SeedOffset(SeedU, 0.9f)), 4);     // [-1,1]
                Ridge = Ridge * 0.5f + 0.5f;  // [0,1] sommets
                Mountain = Ridge * P.MountainStrength * M;   // les montagnes ne montent qu'en haut relief
            }

            // Les plaines gardent une fraction du gonflement continental ; les hautes terres tout.
            const float ContScale = FMath::Lerp(0.45f, 1.0f, M);

            float Terrain = GroundBase
                + Cont * P.ElevationRange * 0.5f * ContScale
                + Mountain * P.ElevationRange
                + Detail * P.SurfaceRoughness;

            OutM = M;
            return Terrain;
        }

        // Une SOURCE pose l'altitude, elle ne la déplace pas : la notion de « déplacement max » ne
        // s'applique pas. La borne d'une colonne se calcule à partir de la source elle-même
        // (GroundBase ± ElevationRange ± SurfaceRoughness), pas ici — d'où FLT_MAX, honnête.
        float MaxDisplacement() const override { return FLT_MAX; }

    private:
        FSurfaceGenerationParams P;
        uint32 SeedU;
    };

    //=========================================================================
    // MOD — FALAISE / CLIFF (raidissement conditionné par la pente)
    //=========================================================================
    class FCliffHeightMod final : public IVoxelHeightOp
    {
    public:
        FCliffHeightMod(const FSurfaceGenerationParams& InP, const FStructuralHeightSource* InSrc)
            : P(InP), Src(InSrc) {}

        void Eval(float WorldX, float WorldY, FVoxelHeightSample& InOut) const override
        {
            if (P.CliffStrength <= 0.0f || Src == nullptr) { return; }

            const float D = FMath::Max(P.CliffSampleDist, 0.5f);
            float Ms;   // relief scratch — on ne veut que les hauteurs
            const float Zxp = Src->SampleZ(WorldX + D, WorldY, Ms);
            const float Zxm = Src->SampleZ(WorldX - D, WorldY, Ms);
            const float Zyp = Src->SampleZ(WorldX, WorldY + D, Ms);
            const float Zym = Src->SampleZ(WorldX, WorldY - D, Ms);
            const float dZdX = (Zxp - Zxm) / (2.0f * D);
            const float dZdY = (Zyp - Zym) / (2.0f * D);
            const float Slope = FMath::Sqrt(dZdX * dZdX + dZdY * dZdY);

            const float Thr = FMath::Max(P.CliffSlopeThreshold, 0.05f);
            const float SlopeGate = FMath::Clamp((Slope - Thr) / Thr, 0.0f, 1.0f);
            if (SlopeGate > 0.0f)
            {
                const float Ref  = 0.25f * (Zxp + Zxm + Zyp + Zym);
                const float Gain = P.CliffStrength * SlopeGate * P.CliffSharpness;
                InOut.Height += (InOut.Height - Ref) * Gain;
            }
        }

    private:
        FSurfaceGenerationParams P;
        const FStructuralHeightSource* Src;
    };

    //=========================================================================
    // MOD — TERRASSES / TERRACE (gaté par le relief : le canal Relief sert ICI)
    //=========================================================================
    class FTerraceHeightMod final : public IVoxelHeightOp
    {
    public:
        explicit FTerraceHeightMod(const FSurfaceGenerationParams& InP) : P(InP) {}

        void Eval(float, float, FVoxelHeightSample& InOut) const override
        {
            if (P.TerraceStrength <= 0.0f || P.TerraceHeight <= 0.0f) { return; }

            const float StepH = P.TerraceHeight;
            const float T     = InOut.Height / StepH;
            const float K     = FMath::FloorToFloat(T);
            const float Frac  = T - K;
            const float W  = FMath::Lerp(0.5f, 0.03f, FMath::Clamp(P.TerraceHardness, 0.0f, 1.0f));
            const float Fs = SmoothStep01(FMath::Clamp((Frac - (0.5f - W)) / (2.0f * W), 0.0f, 1.0f));
            const float Stepped = (K + Fs) * StepH;
            // `* InOut.Relief` : c'est le `* M` de l'original — la raison d'être du second canal.
            InOut.Height = FMath::Lerp(InOut.Height, Stepped, P.TerraceStrength * InOut.Relief);
        }

        // Le terrace interpole VERS une hauteur quantifiée : l'écart ne dépasse jamais un palier.
        float MaxDisplacement() const override
        {
            return (P.TerraceStrength > 0.0f) ? FMath::Max(P.TerraceHeight, 0.0f) : 0.0f;
        }

    private:
        FSurfaceGenerationParams P;
    };

    //=========================================================================
    // MOD — LIGNES DE STRATES / LAYER LINES
    //=========================================================================
    class FLayerLineHeightMod final : public IVoxelHeightOp
    {
    public:
        explicit FLayerLineHeightMod(const FSurfaceGenerationParams& InP) : P(InP) {}

        void Eval(float, float, FVoxelHeightSample& InOut) const override
        {
            if (P.LayerLineDepth <= 0.0f || P.LayerLineSpacing <= 0.0f) { return; }

            const float Phase = InOut.Height * (2.0f * PI / P.LayerLineSpacing);
            InOut.Height -= VoxelMath::DetSin(Phase) * P.LayerLineDepth;
        }

        // `sin` ∈ [-1,1] ⇒ borne exacte.
        float MaxDisplacement() const override
        {
            return (P.LayerLineSpacing > 0.0f) ? FMath::Max(P.LayerLineDepth, 0.0f) : 0.0f;
        }

    private:
        FSurfaceGenerationParams P;
    };

    //=========================================================================
    // MOD — PLAGE / BEACH (aplatissement vers la ligne d'eau)
    //=========================================================================
    class FBeachHeightMod final : public IVoxelHeightOp
    {
    public:
        explicit FBeachHeightMod(const FSurfaceGenerationParams& InP) : P(InP) {}

        void Eval(float, float, FVoxelHeightSample& InOut) const override
        {
            // Le niveau d'eau est GLOBAL à la strate (forcé depuis la strate) pour que le plan
            // d'eau reste continu — d'où le calcul depuis les bornes de strate, pas depuis un param
            // par biome.
            const float H = P.StrateTopWorldZ - P.StrateBottomWorldZ;
            const float WaterZ = P.StrateBottomWorldZ + H * P.WaterLevelRelative;
            if (P.WaterLevelRelative <= 0.0f || P.BeachWidth <= 0.0f) { return; }

            const float DAbs = FMath::Abs(InOut.Height - WaterZ);
            if (DAbs < P.BeachWidth)
            {
                float T = SmoothStep01(DAbs / P.BeachWidth);
                InOut.Height = FMath::Lerp(WaterZ, InOut.Height, T);
            }
        }

        // N'agit que dans `BeachWidth` de l'eau, et ne fait qu'y RAPPROCHER.
        float MaxDisplacement() const override
        {
            return (P.WaterLevelRelative > 0.0f) ? FMath::Max(P.BeachWidth, 0.0f) : 0.0f;
        }

    private:
        FSurfaceGenerationParams P;
    };

    //=========================================================================
    // SOURCE — LE CIEL / SKY CAP  (c'est une ALTITUDE, donc c'est un op de hauteur)
    //=========================================================================
    // `ComputeSurfaceCeiling` rend un Z, exactement comme le terrain. `OPSTACK-DECOMPOSITION §5` le
    // range en `FSkyCapSource` côté DENSITÉ (« Subtract »), mais c'est le même glissement que pour
    // les ops de terrain : ce que la fonction produit est une hauteur, et la soustraction n'arrive
    // qu'après, dans le combine. Le mettre ici lui donne gratuitement l'invariance de fenêtre
    // testée, la pureté XY garantie par le type, et le cache de colonne.
    // The sky cap returns a Z, so it belongs in height space; the subtraction happens later, in the
    // density-side combine.
    class FSkyCapHeightSource final : public IVoxelHeightOp
    {
    public:
        FSkyCapHeightSource(const FSurfaceGenerationParams& InP, int32 InSeed)
            : P(InP), SeedU((uint32)InSeed) {}

        void Eval(float WorldX, float WorldY, FVoxelHeightSample& InOut) const override
        {
            const float H = P.StrateTopWorldZ - P.StrateBottomWorldZ;
            float CeilZ = P.StrateBottomWorldZ + H * P.CeilingRelative;

            // Domain-warp des coords larges/ridge (miroir du HeightWarp du sol). Les bosses fines
            // restent sur le vrai XY pour rester nettes et décorrélées. 0 ⇒ pas de warp.
            float QX = WorldX, QY = WorldY;
            if (P.CeilingWarpStrength > 0.0f)
            {
                const float WF = P.CeilingWarpFrequency;
                const float wx = VoxelNoise::Perlin3D(FVector(WorldX * WF + VoxelHash::SeedOffset(SeedU, 0.71f), WorldY * WF + 2.3f,  VoxelHash::SeedOffset(SeedU, 3.3f)));
                const float wy = VoxelNoise::Perlin3D(FVector(WorldX * WF + 6.1f,          WorldY * WF + VoxelHash::SeedOffset(SeedU, 0.19f), VoxelHash::SeedOffset(SeedU, 4.7f)));
                QX += wx * VOXEL_NOISE_SCALE * P.CeilingWarpStrength;
                QY += wy * VOXEL_NOISE_SCALE * P.CeilingWarpStrength;
            }

            // Gonflement large SIGNÉ : monte/descend toute la voûte.
            if (P.CeilingUndulation > 0.0f)
            {
                const float Swell = HFractalNoise3D(FVector(
                    QX * P.CeilingUndulationFrequency + VoxelHash::SeedOffset(SeedU, 1.9f),
                    QY * P.CeilingUndulationFrequency + 13.0f,
                    VoxelHash::SeedOffset(SeedU, 0.5f)), 3);   // [-1,1]
                CeilZ += Swell * VOXEL_NOISE_SCALE * P.CeilingUndulation;
            }

            // Pendage vers le BAS uniquement : tout est >= 0, donc rien ne perce vers le haut dans
            // le seal. Bosses fines + lames ridgées s'additionnent.
            float Hang = 0.0f;
            if (P.CeilingRoughness > 0.0f)
            {
                Hang += FMath::Abs(HFractalNoise3D(FVector(
                    WorldX * P.CeilingRoughnessFrequency + 5.0f,
                    WorldY * P.CeilingRoughnessFrequency + 6.0f,
                    VoxelHash::SeedOffset(SeedU, 2.1f)), 3)) * VOXEL_NOISE_SCALE * P.CeilingRoughness;
            }
            if (P.CeilingRidgeStrength > 0.0f)
            {
                float Ridge = HRidgedNoise3D(FVector(
                    QX * P.CeilingRidgeFrequency + 31.0f,
                    QY * P.CeilingRidgeFrequency + 47.0f,
                    VoxelHash::SeedOffset(SeedU, 1.1f)), 4);            // [-1,1]
                Ridge = Ridge * 0.5f + 0.5f;      // [0,1] lignes de crête pendantes
                Hang += Ridge * P.CeilingRidgeStrength;
            }

            InOut.Height = CeilZ - Hang;   // Replace : racine de sa propre pile
            // Relief laissé intact : le ciel n'en produit pas et personne ne le lui demande.
        }

        float MaxDisplacement() const override { return FLT_MAX; }   // source, pas modificateur

    private:
        FSurfaceGenerationParams P;
        uint32 SeedU;
    };

    //=========================================================================
    // COMBINER `Mask` — MÉLANGE DE BIOMES / BIOME BLEND
    //=========================================================================
    // Une pile complète par biome ; le champ dit lequel domine ; on interpole les HAUTEURS.
    //
    // ⚠️ Chaque pile calcule SON PROPRE relief `M` en interne et l'utilise pour son propre gate de
    // terrace — exactement comme l'original, où `ComputeSurfaceTerrainZ(X, Y, *PD)` et
    // `(…, *PN)` sont deux appels complets et indépendants dont seules les SORTIES sont mêlées.
    // Le canal `Relief` qui ressort ici est celui du DOMINANT : il est informatif, personne en aval
    // ne s'en sert pour re-gater quoi que ce soit.
    //
    // Each biome stack computes its own relief internally and gates its own terrace with it, exactly
    // as the original makes two independent full calls and blends only the OUTPUTS.
    class FBiomeBlendHeightSource final : public IVoxelHeightOp
    {
    public:
        FBiomeBlendHeightSource(const TArray<FSurfaceGenerationParams>& PerBiome, int32 Seed,
                                const IVoxelBiomeField* InField, bool bCeilingOnly)
            : Field(InField)
        {
            Stacks.Reserve(PerBiome.Num());
            for (const FSurfaceGenerationParams& BP : PerBiome)
            {
                FVoxelHeightStack S;
                if (bCeilingOnly) { VoxelHeightOps::BuildSurfaceCeilingStack(S, BP, Seed); }
                else              { VoxelHeightOps::BuildSurfaceHeightStack(S, BP, Seed); }
                Stacks.Add(MoveTemp(S));
            }
            bBlend = !bCeilingOnly;   // le plafond SÉLECTIONNE, il ne mélange pas
        }

        void Eval(float WorldX, float WorldY, FVoxelHeightSample& InOut) const override
        {
            if (Stacks.Num() == 0) { return; }

            FVoxelBiomeWeights W;
            if (Field) { W = Field->SampleAt(WorldX, WorldY); }

            const int32 D = Stacks.IsValidIndex(W.Dominant) ? W.Dominant : 0;
            InOut = Stacks[D].EvalSample(WorldX, WorldY);

            // Le plafond ne se mélange pas (voir la fabrique) ; le sol si, et seulement dans la
            // bande de frontière où le poids est non nul.
            if (bBlend && W.NeighborWeight > 0.0f && Stacks.IsValidIndex(W.Neighbor))
            {
                const float HN = Stacks[W.Neighbor].EvalHeight(WorldX, WorldY);
                InOut.Height = FMath::Lerp(InOut.Height, HN, W.NeighborWeight);
            }
        }

        float MaxDisplacement() const override { return FLT_MAX; }   // source composite

    private:
        TArray<FVoxelHeightStack> Stacks;
        const IVoxelBiomeField*   Field;
        bool                      bBlend = true;
    };

}   // ⚠️ FIN DU NAMESPACE ANONYME — TOUT NOUVEL OPÉRATEUR SE MET AU-DESSUS DE CETTE LIGNE.
    // En dessous commence `namespace VoxelHeightOps` (les fabriques). Y insérer une classe la sort
    // de la liaison interne, et l'accolade qu'on ajoute avec elle ne ferme rien → C2059. Erreur
    // commise DEUX fois (7cd2bed, puis à nouveau ici) en s'ancrant sur la bannière « FABRIQUES »,
    // qui est de l'autre côté de cette accolade.
    // END OF THE ANONYMOUS NAMESPACE — new operators go ABOVE this line. Anchoring on the FACTORIES
    // banner below puts them outside it, and the brace added with them closes nothing.

//=============================================================================
// FABRIQUES / FACTORIES
//=============================================================================

namespace VoxelHeightOps
{
    TUniquePtr<IVoxelHeightOp> MakeBiomeBlendHeightSource(
        const TArray<FSurfaceGenerationParams>& PerBiomeParams, int32 Seed,
        const IVoxelBiomeField* Field)
    {
        return MakeUnique<FBiomeBlendHeightSource>(PerBiomeParams, Seed, Field, /*bCeilingOnly*/false);
    }

    TUniquePtr<IVoxelHeightOp> MakeBiomeSelectCeilingSource(
        const TArray<FSurfaceGenerationParams>& PerBiomeParams, int32 Seed,
        const IVoxelBiomeField* Field)
    {
        return MakeUnique<FBiomeBlendHeightSource>(PerBiomeParams, Seed, Field, /*bCeilingOnly*/true);
    }

    TUniquePtr<IVoxelHeightOp> MakeSkyCapHeightSource(const FSurfaceGenerationParams& P, int32 Seed)
    {
        return MakeUnique<FSkyCapHeightSource>(P, Seed);
    }

    void BuildSurfaceCeilingStack(FVoxelHeightStack& OutStack, const FSurfaceGenerationParams& P, int32 Seed)
    {
        // Un seul op aujourd'hui — et c'est une information, pas un manque : le plafond n'a pas
        // d'équivalent des quatre modificateurs du sol. Le jour où on veut des terrasses au
        // plafond, on ajoute la ligne ; c'est exactement le genre de composition que le refactor
        // existe pour rendre possible.
        OutStack.Add(MakeSkyCapHeightSource(P, Seed));
    }
    TUniquePtr<IVoxelHeightOp> MakeStructuralHeightSource(const FSurfaceGenerationParams& P, int32 Seed,
                                                          const IVoxelHeightOp** OutSource)
    {
        TUniquePtr<FStructuralHeightSource> Src = MakeUnique<FStructuralHeightSource>(P, Seed);
        if (OutSource) { *OutSource = Src.Get(); }
        return Src;
    }

    TUniquePtr<IVoxelHeightOp> MakeCliffHeightMod(const FSurfaceGenerationParams& P,
                                                  const IVoxelHeightOp* StructuralSource)
    {
        // `static_cast` plutôt que `Cast<>` : ce ne sont pas des UObject, et le contrat de la
        // fabrique est qu'on lui rend exactement le pointeur sorti de MakeStructuralHeightSource.
        return MakeUnique<FCliffHeightMod>(
            P, static_cast<const FStructuralHeightSource*>(StructuralSource));
    }

    TUniquePtr<IVoxelHeightOp> MakeTerraceHeightMod(const FSurfaceGenerationParams& P)
    {
        return MakeUnique<FTerraceHeightMod>(P);
    }

    TUniquePtr<IVoxelHeightOp> MakeLayerLineHeightMod(const FSurfaceGenerationParams& P)
    {
        return MakeUnique<FLayerLineHeightMod>(P);
    }

    TUniquePtr<IVoxelHeightOp> MakeBeachHeightMod(const FSurfaceGenerationParams& P)
    {
        return MakeUnique<FBeachHeightMod>(P);
    }

    void BuildSurfaceHeightStack(FVoxelHeightStack& OutStack, const FSurfaceGenerationParams& P, int32 Seed)
    {
        // L'ORDRE EST CELUI DE `ComputeSurfaceTerrainZ`, et il porte du sens :
        // le cliff raidit le champ brut, le terrace quantifie le résultat raidi, les lignes de
        // strates se posent dessus, et la plage écrase tout près de l'eau.
        const IVoxelHeightOp* Structural = nullptr;
        OutStack.Add(MakeStructuralHeightSource(P, Seed, &Structural));
        OutStack.Add(MakeCliffHeightMod(P, Structural));
        OutStack.Add(MakeTerraceHeightMod(P));
        OutStack.Add(MakeLayerLineHeightMod(P));
        OutStack.Add(MakeBeachHeightMod(P));
    }
}
