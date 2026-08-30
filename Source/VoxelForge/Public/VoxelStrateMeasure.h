// Read-only, headless measurements over a coarse sample of one strate.

#pragma once

#include "CoreMinimal.h"

class UVoxelGenerator;
class UVoxelStrateManager;

/** Explicit outcome of a coarse connectivity query plus its full-resolution route re-check. */
enum class EVoxelConnectivityResult : uint8
{
    Connected,
    NotConnectedAtThisResolution, // Evidence only at this grid's resolution; corridors thinner than SampleStep are invisible.
    StartCellSolid,
    GoalCellSolid,
    OutOfWindow,
    CoarseLied
};

/** Settings for the deterministic coarse grid used by the strate measurement pass. */
struct VOXELFORGE_API FVoxelStrateMeasureSettings
{
    int32     SampleStep     = 4;    // Voxels between samples.
    int32     RadiusInVoxels = 256;  // Half-extent in X and Y around CenterXY.
    FVector2D CenterXY       = FVector2D::ZeroVector; // Actor-space voxel coordinates.
    int32     MaxCells       = 8000000; // Refuse a grid larger than this many cells.
    int32     HeadroomCells  = 2;    // Air cells above a floor cell required for walkable.
    int32     InteriorMarginVoxels = -1; // <0 = derive from BoundarySealThickness (2x, clamped).
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
    FVector LargestComponentPoint = FVector::ZeroVector; // Actor-space voxel coords; valid iff NumAir > 0.
    int64 LargestComponentCells = 0;
    int32 NumComponentsAtLeast1Pct = 0;
    float WalkableFraction      = 0.f;
    float MedianFeatureScale   = 0.f;
    int32 MedianVerticalClearance = 0;

    // The exact vertical window used by the measurement: [SampledMinZ, SampledMaxZ).
    int32 ResolvedMarginVoxels = 0;
    int32 SampledMinZ = 0;
    int32 SampledMaxZ = 0;
};

/**
 * Measure one strate in actor-space voxel coordinates.
 *
 * The pass samples the requested strate between a resolved margin inside its top and bottom
 * boundaries, flood-fills its air once, and derives all returned metrics from that same grid.
 * It never mutates either input object.
 */
VOXELFORGE_API FVoxelStrateMetrics VF_MeasureStrate(
    const UVoxelGenerator& Generator,
    const UVoxelStrateManager& Manager,
    int32 StrateIndex,
    const FVoxelStrateMeasureSettings& Settings);

/**
 * Test coarse connectivity and then re-check the one recovered coarse route at full resolution.
 *
 * If an endpoint's own coarse cell is solid, the query searches the 26 neighbouring cells and
 * uses the nearest air cell when one exists. The snap flags report those repairs. Solid-cell
 * results are returned only when no adjacent air cell exists. A coarse route that fails its
 * full-resolution re-check returns CoarseLied; a missing coarse route returns
 * NotConnectedAtThisResolution because a corridor thinner than SampleStep is invisible to the
 * grid. A bare "not connected" result is evidence only at the grid's own resolution.
 */
VOXELFORGE_API EVoxelConnectivityResult VF_AreConnected(
    const UVoxelGenerator& Generator,
    const UVoxelStrateManager& Manager,
    int32 StrateIndex,
    const FVector& AVoxel,
    const FVector& BVoxel,
    const FVoxelStrateMeasureSettings& Settings,
    bool& bOutStartSnapped,
    bool& bOutGoalSnapped);
