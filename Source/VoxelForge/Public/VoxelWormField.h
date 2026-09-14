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

    /**
     * Return the selected development experiment mode.
     *
     * 0 is the exact field.  2 and 4 evaluate the exact field on that many-voxel lattice and
     * trilinearly interpolate it.  This is intentionally a WORLD-CHANGING development switch:
     * it must not be changed between multiplayer peers or for an already generated world.
     */
    VOXELFORGE_API int32 GetLatticeStep();
    VOXELFORGE_API int32 GetNoiseMode();

    /** Evaluate the exact two-noise field, retaining the N1 threshold short-circuit. */
    VOXELFORGE_API float EvaluateExact(
        float WorldX,
        float WorldY,
        float WorldZ,
        float Threshold,
        const FParameters& Parameters);

    /** Evaluate the exact field or its selected cached lattice approximation. */
    VOXELFORGE_API float Evaluate(
        float WorldX,
        float WorldY,
        float WorldZ,
        float Threshold,
        const FParameters& Parameters,
        int32 LatticeStep);
}
