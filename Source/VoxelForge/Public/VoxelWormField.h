// VoxelWormField.h
// Shared deterministic evaluation for the TunnelNetwork worm field.

#pragma once

#include "CoreMinimal.h"

namespace VoxelWormField
{
    /** The scalar inputs used by both the fused and operator-stack evaluators. */
    struct FParameters
    {
        float Frequency = 0.0f;
        float HorizontalBias = 0.0f;
        float VerticalScale = 1.0f;
        uint32 Seed = 0;
    };

    /** Evaluate the exact two-noise field, retaining the N1 threshold short-circuit. */
    VOXELFORGE_API float EvaluateExact(
        float WorldX,
        float WorldY,
        float WorldZ,
        float Threshold,
        const FParameters& Parameters);

    /** Evaluate the exact two-noise field. */
    VOXELFORGE_API float Evaluate(
        float WorldX,
        float WorldY,
        float WorldZ,
        float Threshold,
        const FParameters& Parameters);
}
