// Read-only, headless measurements over a coarse sample of one strate.

#pragma once

#include "CoreMinimal.h"

class UVoxelGenerator;
class UVoxelStrateManager;

/** Settings for the deterministic coarse grid used by the strate measurement pass. */
struct VOXELFORGE_API FVoxelStrateMeasureSettings
{
    int32     SampleStep     = 4;    // Voxels between samples.
    int32     RadiusInVoxels = 256;  // Half-extent in X and Y around CenterXY.
    FVector2D CenterXY       = FVector2D::ZeroVector; // Actor-space voxel coordinates.
    int32     MaxCells       = 8000000; // Refuse a grid larger than this many cells.
    int32     HeadroomCells  = 2;    // Air cells above a floor cell required for walkable.
};

/** Metrics derived from one coarse grid and its single air flood fill. */
struct VOXELFORGE_API FVoxelStrateMetrics
{
    bool    bValid = false;
    FString RefusalReason;

    int64 NumSampled = 0;
    int64 NumAir     = 0;
    int64 NumSolid   = 0;

    float AirFraction           = 0.f;
    int32 NumAirComponents      = 0;
    float LargestComponentShare = 0.f;
    float WalkableFraction      = 0.f;
    float MedianFeatureScale   = 0.f;
    int32 MedianVerticalClearance = 0;
};

/**
 * Measure one strate in actor-space voxel coordinates.
 *
 * The pass samples only the interior chunks of the requested strate, flood-fills its air once,
 * and derives all returned metrics from that same grid. It never mutates either input object.
 */
VOXELFORGE_API FVoxelStrateMetrics VF_MeasureStrate(
    const UVoxelGenerator& Generator,
    const UVoxelStrateManager& Manager,
    int32 StrateIndex,
    const FVoxelStrateMeasureSettings& Settings);

/**
 * Test coarse connectivity and then re-check the one recovered coarse route at full resolution.
 *
 * bOutCoarseLied is true only when the coarse route existed but a full-resolution sample on that
 * route was solid (or non-finite), in which case this function always returns false.
 */
VOXELFORGE_API bool VF_AreConnected(
    const UVoxelGenerator& Generator,
    const UVoxelStrateManager& Manager,
    int32 StrateIndex,
    const FVector& AVoxel,
    const FVector& BVoxel,
    const FVoxelStrateMeasureSettings& Settings,
    bool& bOutCoarseLied);
