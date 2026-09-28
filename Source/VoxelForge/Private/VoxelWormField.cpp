// VoxelWormField.cpp
// Shared deterministic exact worm-field evaluation.

#include "VoxelWormField.h"

#include "VoxelCaveMorphology.h" // VoxelHash::SeedOffset
#include "VoxelNoise.h"
#include "VoxelTypes.h"           // VOXEL_NOISE_SCALE

namespace
{
    FORCEINLINE FVector3f MakeWormNoiseBase(
        float WorldX,
        float WorldY,
        float WorldZ,
        const VoxelWormField::FParameters& Parameters)
    {
        const float EffectiveZ = (Parameters.VerticalScale != 1.0f
                                  && Parameters.VerticalScale > 0.0f)
            ? WorldZ / Parameters.VerticalScale : WorldZ;
        const float WormZFrequency = Parameters.Frequency * Parameters.HorizontalBias;
        return FVector3f(
            WorldX * Parameters.Frequency
                + VoxelHash::SeedOffset(Parameters.Seed, 1.0f),
            WorldY * Parameters.Frequency
                + VoxelHash::SeedOffset(Parameters.Seed, 1.7f),
            EffectiveZ * WormZFrequency
                + VoxelHash::SeedOffset(Parameters.Seed, 2.3f));
    }

    FORCEINLINE float EvaluateN1Field(
        float WorldX,
        float WorldY,
        float WorldZ,
        const VoxelWormField::FParameters& Parameters,
        float NoiseScale)
    {
        const FVector3f NoiseBase = MakeWormNoiseBase(
            WorldX, WorldY, WorldZ, Parameters);
        return FMath::Abs(VoxelNoise::Perlin3D(NoiseBase) * NoiseScale);
    }

    FORCEINLINE float EvaluateN2Field(
        float WorldX,
        float WorldY,
        float WorldZ,
        const VoxelWormField::FParameters& Parameters,
        float NoiseScale)
    {
        const FVector3f NoiseBase = MakeWormNoiseBase(
            WorldX, WorldY, WorldZ, Parameters);
        const FVector3f N2Position = NoiseBase + FVector3f(137.0f, 259.0f, 431.0f);
        return FMath::Abs(VoxelNoise::Perlin3D(N2Position) * NoiseScale);
    }

    FORCEINLINE float EvaluateExactField(
        float WorldX,
        float WorldY,
        float WorldZ,
        float Threshold,
        const VoxelWormField::FParameters& Parameters)
    {
        const float NoiseScale = VOXEL_NOISE_SCALE;
        const float N1 = EvaluateN1Field(
            WorldX, WorldY, WorldZ, Parameters, NoiseScale);

        // N2 is non-negative, so this preserves the exact evaluator's short-circuit.
        if (N1 >= Threshold)
        {
            return N1;
        }
        return N1 + EvaluateN2Field(
            WorldX, WorldY, WorldZ, Parameters, NoiseScale);
    }
}

namespace VoxelWormField
{
    float Evaluate(
        float WorldX,
        float WorldY,
        float WorldZ,
        float Threshold,
        const FParameters& Parameters)
    {
        return EvaluateExactField(WorldX, WorldY, WorldZ, Threshold, Parameters);
    }
}
