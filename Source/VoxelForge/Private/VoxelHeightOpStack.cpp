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
    FORCEINLINE float HSampleRelief(float WorldX, float WorldY, float SeedF,
                                    float Frequency, float Contrast)
    {
        float R = HFractalNoise3D(FVector(
            WorldX * Frequency + SeedF * 7.3f,
            WorldY * Frequency + SeedF * 2.1f,
            SeedF * 0.5f), 2) * 0.5f + 0.5f;                 // [0,1]
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
            : P(InP), SeedF((float)InSeed) {}

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
                const float wx = VoxelNoise::Perlin3D(FVector(WorldX * WF + SeedF * 0.31f, WorldY * WF + 4.2f, SeedF * 1.7f));
                const float wy = VoxelNoise::Perlin3D(FVector(WorldX * WF + 8.6f, WorldY * WF + SeedF * 0.53f, SeedF * 2.9f));
                QX += wx * VOXEL_NOISE_SCALE * P.HeightWarpStrength;
                QY += wy * VOXEL_NOISE_SCALE * P.HeightWarpStrength;
            }

            const float Relief = HSampleRelief(WorldX, WorldY, SeedF, P.ReliefFrequency, P.ReliefContrast);
            const float M = FMath::Lerp(1.0f, Relief, P.ReliefStrength);

            float Cont = HFractalNoise3D(FVector(
                QX * P.ContinentFrequency + SeedF * 3.1f,
                QY * P.ContinentFrequency + SeedF * 5.7f,
                SeedF * 0.7f), 4);  // [-1,1]

            float Detail = HFractalNoise3D(FVector(
                WorldX * P.DetailFrequency + 11.0f,
                WorldY * P.DetailFrequency + 22.0f,
                SeedF * 1.3f), 3);  // [-1,1]

            float Mountain = 0.0f;
            if (P.MountainStrength > 0.0f)
            {
                float Ridge = HRidgedNoise3D(FVector(
                    QX * P.MountainFrequency + 99.0f,
                    QY * P.MountainFrequency + 77.0f,
                    SeedF * 0.9f), 4);     // [-1,1]
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
        float SeedF;
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
            InOut.Height -= FMath::Sin(Phase) * P.LayerLineDepth;
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
}

//=============================================================================
// FABRIQUES / FACTORIES
//=============================================================================

namespace VoxelHeightOps
{
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
