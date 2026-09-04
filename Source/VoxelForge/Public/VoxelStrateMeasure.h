// Read-only, headless measurements over a coarse sample of one strate.

#pragma once

#include "CoreMinimal.h"

class UVoxelGenerator;
class UVoxelStrateManager;

/** Read-only density source used by the offline Tier 2 pass for a materialised custom stack. */
class VOXELFORGE_API IVoxelStrateDensitySampler
{
public:
    virtual ~IVoxelStrateDensitySampler() = default;
    virtual float SampleDensity(float WorldX, float WorldY, float WorldZ) const = 0;
};

/** Explicit outcome of a coarse connectivity query plus its full-resolution route re-check. */
enum class EVoxelConnectivityResult : uint8
{
    Connected,
    NotConnectedAtThisResolution, // Evidence only at this grid's resolution; corridors thinner than SampleStep are invisible.
    StartCellSolid,
    GoalCellSolid,
    OutOfWindow,
    CoarseLiedBudgetExhausted // Unknown: retries are exhausted, or the refutation has no coarse edge to exclude.
};

/** Settings for the deterministic coarse grid used by the strate measurement pass. */
struct VOXELFORGE_API FVoxelStrateMeasureSettings
{
    int32     SampleStep     = 4;    // Voxels between samples.
    int32     RadiusInVoxels = 256;  // Half-extent in X and Y around CenterXY.
    FVector2D CenterXY       = FVector2D::ZeroVector; // Actor-space voxel coordinates.
    // When both points are set, replace the square centered on CenterXY with the XY AABB of the
    // two points expanded by CoverMarginVoxels. This keeps a mouth-to-mouth measurement sized by
    // the mouths' separation instead of by their distance from the world origin.
    TOptional<FVector2D> CoverPointA;
    TOptional<FVector2D> CoverPointB;
    float CoverMarginVoxels = 48.0f;
    int32     MaxCells       = 8000000; // Refuse a grid larger than this many cells.
    int32     MaxRouteRetries = 16; // Alternate coarse routes to full-resolution-check after the first route.
    int32     HeadroomCells  = 2;    // Air cells above a floor cell required for walkable.
    int32     InteriorMarginVoxels = -1; // <0 = derive from BoundarySealThickness (2x, clamped); explicit 0 includes the seal.
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

    // The exact sampled XY box and dimensions. For a fitted window these are the AABB-derived
    // dimensions; for the legacy window they describe CenterXY +/- RadiusInVoxels.
    int32 SampledNumX = 0;
    int32 SampledNumY = 0;
    int32 SampledNumZ = 0;
    float SampledMinX = 0.0f;
    float SampledMaxX = 0.0f;
    float SampledMinY = 0.0f;
    float SampledMaxY = 0.0f;

    // Air-component sizes in deterministic flood-fill discovery order. Keeping these facts from
    // the one required flood fill lets diagnostics classify small non-largest components without
    // sampling the source a second time.
    TArray<int64> AirComponentCells;
};

/**
 * Component facts for one coarse mouth-to-mouth query.
 *
 * The component shares are fractions of the sampled air cells. The two component distances are
 * exact spatial Euclidean distances in coarse-cell coordinates to the nearest cell of the other
 * mouth's component. Solid cells do not participate in that proximity calculation because it
 * measures geometric separation between air components, not an alternate route. A distance of
 * zero means both mouths resolve to the same component. Negative distances mean that the
 * requested grid or endpoint could not be analyzed.
 */
struct VOXELFORGE_API FVoxelConnectivityDiagnostics
{
    bool bValid = false;
    EVoxelConnectivityResult Result = EVoxelConnectivityResult::OutOfWindow;
    bool bStartSnapped = false;
    bool bGoalSnapped = false;
    int32 NumRouteRetries = 0; // Alternate routes full-resolution-checked after the first route.

    int64 NumAirCells = 0;
    int64 StartComponentCells = 0;
    int64 GoalComponentCells = 0;
    int64 LargestComponentCells = 0;
    float StartComponentShare = 0.0f;
    float GoalComponentShare = 0.0f;
    float LargestComponentShare = 0.0f;
    bool bStartComponentIsLargest = false;
    bool bGoalComponentIsLargest = false;

    float StartToGoalComponentDistanceCells = -1.0f;
    float GoalToStartComponentDistanceCells = -1.0f;
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

/** The same Tier 2 pass over an explicit voxel Z window and a custom read-only sampler. */
VOXELFORGE_API FVoxelStrateMetrics VF_MeasureStrateWithSampler(
    const IVoxelStrateDensitySampler& Sampler,
    int32 StrateBottomWorldZ,
    int32 StrateTopWorldZ,
    float BoundarySealThickness,
    const FVoxelStrateMeasureSettings& Settings);

/**
 * Test coarse connectivity and re-check recovered coarse routes at full resolution.
 *
 * If an endpoint's own coarse cell is solid, the query searches the 26 neighbouring cells and
 * uses the nearest air cell when one exists. The snap flags report those repairs. Solid-cell
 * results are returned only when no adjacent air cell exists. When a route fails its
 * full-resolution re-check, the failed coarse edge is excluded from a deterministic local
 * blocklist and BFS is retried up to MaxRouteRetries. A missing route after exclusions returns
 * NotConnectedAtThisResolution. If the retry budget is exhausted while routes are still being
 * refuted, the result is CoarseLiedBudgetExhausted: unknown, not disconnected. The same unknown
 * result is used when a refutation is in an endpoint-to-cell segment with no coarse edge to
 * exclude. Every Connected result has a fully re-walked air route.
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

/** The same query as the simple overload, with the number of alternate routes actually tried. */
VOXELFORGE_API EVoxelConnectivityResult VF_AreConnected(
    const UVoxelGenerator& Generator,
    const UVoxelStrateManager& Manager,
    int32 StrateIndex,
    const FVector& AVoxel,
    const FVector& BVoxel,
    const FVoxelStrateMeasureSettings& Settings,
    bool& bOutStartSnapped,
    bool& bOutGoalSnapped,
    int32& OutNumRouteRetries);

/**
 * The same connectivity query with component facts returned from the same sampled grid. This
 * overload exists for diagnostics that need both the required route verdict and the component
 * containing one endpoint without sampling the strate a second time.
 */
VOXELFORGE_API EVoxelConnectivityResult VF_AreConnected(
    const UVoxelGenerator& Generator,
    const UVoxelStrateManager& Manager,
    int32 StrateIndex,
    const FVector& AVoxel,
    const FVector& BVoxel,
    const FVoxelStrateMeasureSettings& Settings,
    bool& bOutStartSnapped,
    bool& bOutGoalSnapped,
    FVoxelConnectivityDiagnostics& OutDiagnostics);

/**
 * Return the component sizes and spatial separation facts for the same query performed by
 * VF_AreConnected. The returned Result uses the same retrying full-resolution route re-check,
 * and NumRouteRetries reports the number of alternate routes actually checked. bValid means the
 * sample grid was built and both endpoint coordinates were in its bounds; component fields
 * remain zero/negative when an endpoint has no air cell to analyze. Largest-component membership
 * treats equal-sized components as tied for largest.
 */
VOXELFORGE_API FVoxelConnectivityDiagnostics VF_DiagnoseConnectivity(
    const UVoxelGenerator& Generator,
    const UVoxelStrateManager& Manager,
    int32 StrateIndex,
    const FVector& AVoxel,
    const FVector& BVoxel,
    const FVoxelStrateMeasureSettings& Settings);

/** Connectivity variant for the explicit custom-stack measurement window. */
VOXELFORGE_API EVoxelConnectivityResult VF_AreConnectedWithSampler(
    const IVoxelStrateDensitySampler& Sampler,
    int32 StrateBottomWorldZ,
    int32 StrateTopWorldZ,
    float BoundarySealThickness,
    const FVector& AVoxel,
    const FVector& BVoxel,
    const FVoxelStrateMeasureSettings& Settings,
    bool& bOutStartSnapped,
    bool& bOutGoalSnapped,
    FVoxelConnectivityDiagnostics* OutDiagnostics = nullptr);

/** Full diagnostics variant for a custom stack. */
VOXELFORGE_API FVoxelConnectivityDiagnostics VF_DiagnoseConnectivityWithSampler(
    const IVoxelStrateDensitySampler& Sampler,
    int32 StrateBottomWorldZ,
    int32 StrateTopWorldZ,
    float BoundarySealThickness,
    const FVector& AVoxel,
    const FVector& BVoxel,
    const FVoxelStrateMeasureSettings& Settings);
