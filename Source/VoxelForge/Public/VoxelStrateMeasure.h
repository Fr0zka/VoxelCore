// Read-only, headless measurements over a coarse sample of one strate.

#pragma once

#include "CoreMinimal.h"
#include "VoxelPassageGeometry.h"

class UVoxelGenerator;
class UVoxelStrateManager;

/**
 * The pinned metric capsule dimensions in voxel-space.
 *
 * 25 cm/voxel is the authored world scale: radius = 34 cm / 25 = 1.36 voxels and
 * half-height = 88 cm / 25 = 3.52 voxels, hence a 176 cm / 25 = 7.04 voxel total height.
 * The measure settings copy these values so a future character can change the query without
 * changing generation. The diagnosis test separately reads movement limits from the project's
 * character CDO.
 */
struct VOXELFORGE_API FVoxelPlayerCapsuleConstants
{
    static constexpr float VoxelSizeCentimeters = 25.0f;
    static constexpr float VoxelSizeMeters = VoxelPassageGeometry::VoxelSizeMeters;
    static constexpr float RadiusCentimeters = 34.0f;
    static constexpr float HalfHeightCentimeters = 88.0f;
    // UCharacterMovementComponent's UE 5.7 default. Callers that have a character CDO should
    // copy its authored value into PlayerMaxStepHeightMeters.
    static constexpr float MaxStepHeightCentimeters = 45.0f;
    static constexpr float MaxStepHeightMeters = MaxStepHeightCentimeters / 100.0f;
    // UE 5.7's WalkableFloorZ=0.71 resolves to about 44.8 degrees; a character CDO may expose
    // the precise angle. The diagnosis test copies that precise project value when available.
    static constexpr float WalkableFloorAngleDegrees = 44.8f;
    static constexpr float RadiusVoxels = VoxelPassageGeometry::PlayerRadiusVoxels;
    static constexpr float HalfHeightVoxels = VoxelPassageGeometry::PlayerHalfHeightVoxels;
    static constexpr float HeightVoxels = VoxelPassageGeometry::PlayerHeightVoxels;
};

/**
 * Read-only density source used by the offline Tier 2 pass for a materialised custom stack.
 *
 * SampleDensity is called concurrently during grid sampling; implementations must be const,
 * re-entrant, and safe for concurrent read-only calls, just like UVoxelGenerator::GetDensityAt.
 */
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
    CoarseLiedBudgetExhausted, // Unknown: retries are exhausted, or the refutation has no coarse edge to exclude.
    // Appended to preserve the underlying values of the legacy result codes above.
    StartCellNotPlayerFit,
    GoalCellNotPlayerFit
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
    // Origin-rooted topologies can route from one mouth to the other through the finite (0,0)
    // landing room and its local room/join geometry. There is no hidden radial connector network.
    // When true, include (0,0) in the fitted AABB before applying MaxCells. A caller must keep
    // this enabled for that topology; an over-cap route window is then refused honestly instead
    // of being cropped back to a mouth-only box.
    bool bIncludeOriginInCoverWindow = false;
    // Diagnostic-only strengthening of the preceding policy: ensure the sampled box contains an
    // actual one-voxel column around (0,0), rather than ending exactly at the exclusive upper
    // bound when both mouths lie on one side of the origin.
    bool bForceOriginColumnInCoverWindow = false;
    int32     MaxCells       = 8000000; // Refuse a grid larger than this many cells.
    int32     MaxRouteRetries = 16; // Alternate coarse routes to full-resolution-check after the first route.
    int32     HeadroomCells  = 2;    // Legacy walkable-column metric only.
    int32     InteriorMarginVoxels = -1; // <0 = derive from BoundarySealThickness (2x, clamped); explicit 0 includes the seal.

    // Base ACharacter defaults at 25 cm/voxel: 34 cm radius / 25 = 1.36 voxels and
    // 88 cm half-height / 25 = 3.52 voxels (7.04 voxels total). These are parameters, not
    // generation values, so another playable capsule can be measured without a code change.
    float PlayerCapsuleRadiusVoxels = FVoxelPlayerCapsuleConstants::RadiusVoxels;
    float PlayerCapsuleHalfHeightVoxels = FVoxelPlayerCapsuleConstants::HalfHeightVoxels;

    // Character-derived fit parameters. MaxStepHeight is deliberately named and stored in
    // metres, matching UCharacterMovementComponent's authored unit. The measurement converts it
    // to voxels only for the bounded downward support search.
    float PlayerMaxStepHeightMeters = FVoxelPlayerCapsuleConstants::MaxStepHeightMeters;
    float PlayerWalkableFloorAngleDegrees = FVoxelPlayerCapsuleConstants::WalkableFloorAngleDegrees;
    // A support patch is sampled at the integer columns covered by the capsule radius. 0.75 with
    // the default 1.36-voxel radius requires 4 of 5 columns, including the centre column.
    float PlayerSupportPatchMinCoverageFraction = 0.75f;

    // Traversal is still constrained by the exact player-fit mask, but a planned climbing
    // mechanic may cross a higher supported neighbouring column. This is a measurement policy,
    // not a runtime movement change. The surface remains required to be supported; the extra
    // height only changes whether that supported edge is traversable.
    bool bPlayerClimbingEnabled = true;
    float PlayerMaxClimbHeightMeters = 3.0f;
};

/**
 * Optional capture of the exact grid used by one measurement.
 *
 * Air is deliberately stored as a polarity bit: 1 means density > 0 (air), 0 means solid.
 * Density retains the scalar sample that produced that bit, so an editor preview can draw a
 * density=0 contour without inventing geometry by blurring the binary view. Both arrays are
 * opt-in so ordinary metrics callers do not retain a multi-million-cell buffer.
 * It is an editor/automation hand-off for tools such as the composer preview, not a runtime
 * generation cache.
 */
struct VOXELFORGE_API FVoxelStrateSampleGrid
{
    bool bValid = false;
    int32 NumX = 0;
    int32 NumY = 0;
    int32 NumZ = 0;
    int32 SampleStep = 1;
    int32 CellCount = 0;

    float MinX = 0.0f;
    float MinY = 0.0f;
    float MinZ = 0.0f;
    float MaxX = 0.0f;
    float MaxY = 0.0f;
    float MaxZ = 0.0f;

    int32 ResolvedMarginVoxels = 0;
    int32 SampledMinZ = 0;
    int32 SampledMaxZ = 0;

    // 1 = air (density > 0), 0 = solid (density <= 0), matching the measurement polarity.
    TArray<uint8> Air;

    // Exact scalar density at the same cell centre as Air. Present on measurement captures;
    // empty on legacy/binary-only callers. This is never used by generation.
    TArray<float> Density;

    // 1 = the exact player-fit pose predicate accepted this cell, 0 = rejected. This is
    // populated when a player-fit walk explicitly captures its shared grid. It is kept beside
    // Air so render, walk, and diagnostics can consume one sampled field and one fit mask.
    TArray<uint8> PlayerFitMask;
    int64 PlayerFitCellCount = 0;

    FORCEINLINE int32 Index(int32 X, int32 Y, int32 Z) const
    {
        return static_cast<int32>(
            static_cast<int64>(X)
            + static_cast<int64>(NumX) * (static_cast<int64>(Y)
                + static_cast<int64>(NumY) * static_cast<int64>(Z)));
    }

    bool IsValid() const
    {
        return bValid && NumX > 0 && NumY > 0 && NumZ > 0
            && static_cast<int64>(CellCount)
                == static_cast<int64>(NumX) * static_cast<int64>(NumY)
                    * static_cast<int64>(NumZ)
            && Air.Num() == CellCount;
    }

    bool HasScalarDensity() const
    {
        return IsValid() && Density.Num() == CellCount;
    }

    bool HasPlayerFitMask() const
    {
        return IsValid() && PlayerFitMask.Num() == CellCount;
    }

    bool ContainsPoint(const FVector& Point) const
    {
        return IsValid()
            && Point.X >= MinX && Point.X < MaxX
            && Point.Y >= MinY && Point.Y < MaxY
            && Point.Z >= MinZ && Point.Z < MaxZ;
    }
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
    // Distinct sampled XY columns that contain at least one walkable cell, divided by the
    // sampled XY footprint. This is a floor-area proxy, not a volume ratio: a large chamber
    // can have a small WalkableFraction while retaining a substantial walkable floor.
    int64 WalkableFloorColumns = 0;
    float WalkableFloorAreaFraction = 0.f;
    // Four-neighbour connected components of the projected walkable columns. This deliberately
    // remains a 2D surface indicator; it does not prove that floors at different Z values are
    // traversable in 3D. The separate arrival/departure route check is the traversal gate.
    int32 NumWalkableSurfaceComponents = 0;
    int64 LargestWalkableSurfaceColumns = 0;
    float LargestWalkableSurfaceShare = 0.f;
    float MedianFeatureScale   = 0.f;
    int32 MedianVerticalClearance = 0;

    // Player-fit metrics are answered only by a one-voxel grid. A coarse grid refuses to infer
    // support or clearance because SampleStep=4 cannot resolve a 1.36-voxel radius capsule.
    bool bPlayerFitResolved = false;
    FString PlayerFitRefusalReason;
    int64 NumPlayerFitCells = 0;
    float PlayerFitFraction = 0.0f; // Accepted free supported poses / all air cells.
    int32 NumTraversableComponents = 0;
    int64 LargestTraversableComponentCells = 0;
    float TraversableComponentShare = 0.0f; // Largest player-fit component / player-fit cells.
    // Minimum horizontal Chebyshev clearance, in voxels, among accepted supported poses. This is
    // a diagnostic only; player occupancy itself uses a direct capsule stencil, never the old
    // Manhattan distance transform.
    float MinimumPlayerClearanceVoxels = 0.0f;

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
    bool bPlayerFitRestricted = false;
    bool bPlayerFitResolved = false;
    FString RefusalReason;
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
 * Experiential traversal facts over the exact VF player-fit mask.
 *
 * This is intentionally a report, not a new movement implementation. The mask is built by the
 * same direct capsule stencil used by VF_DiagnosePlayerFitConnectivityWithSampler; the explorer's
 * deterministic graph walk only instruments that already-resolved six-neighbour pose graph.
 * Distances are metres so the result can be read beside a human-scale screenshot.
 */
struct VOXELFORGE_API FVoxelPlayerFitWalkReport
{
    bool bValid = false;
    FString RefusalReason;
    EVoxelConnectivityResult Result = EVoxelConnectivityResult::OutOfWindow;
    bool bCanReachDeparture = false;
    bool bStartSnapped = false;
    bool bGoalSnapped = false;
    int32 NumRouteRetries = 0;
    bool bClimbingEnabled = false;
    float MaxClimbHeightMeters = 0.0f;
    // The primordial capability contract is walk-only.  The existing reachable fields below
    // therefore remain walk-only; climb is reported as a separate tier and never makes the hard
    // connectivity verdict pass.
    bool bWalkOnlyCanReachDeparture = false;
    bool bClimbCanReachDeparture = false;

    int64 PlayerFitVolumeCells = 0;
    int64 ReachablePlayerFitCells = 0;
    int64 WalkOnlyReachablePlayerFitCells = 0;
    int64 ClimbReachablePlayerFitCells = 0;
    int64 ClimbEdges = 0;
    // Fit poses outside the walk+climb reachable set need an edit/mining action to become
    // reachable. This is a reporting remainder, not a promise that arbitrary mining can reach
    // every such pose without a separate excavation model.
    int64 MineRequiredPlayerFitCells = 0;
    float ReachablePlayerFitFraction = 0.0f;
    float ClimbReachablePlayerFitFraction = 0.0f;
    int32 SampledNumX = 0;
    int32 SampledNumY = 0;
    int32 SampledNumZ = 0;
    int32 SampledMinZ = 0;
    int32 SampledMaxZ = 0;
    float SampledMinX = 0.0f;
    float SampledMaxX = 0.0f;
    float SampledMinY = 0.0f;
    float SampledMaxY = 0.0f;

    // The agent follows a fixed-order depth-first walk over the already-resolved fit graph. A
    // branch edge is counted twice when the agent backtracks, which makes this an instrument of
    // traversal experience rather than a shortest-path length.
    int64 AgentGraphTraversals = 0;
    int64 ClimbTraversals = 0;
    int64 DeadEndsEncountered = 0;
    float DeadEndsPer100m = 0.0f;
    float DistanceTravelledMeters = 0.0f;
    float StraightLineMeters = 0.0f;
    float Tortuosity = 0.0f;

    // A narrow-gap event is a traversed graph edge touching a route cell whose widest
    // axis-aligned horizontal air span is below the threshold. This deliberately reports the
    // operational proxy instead of pretending the voxel mask contains an oriented corridor width.
    int64 NarrowGapTraversals = 0;
    float NarrowGapFraction = 0.0f;
    float NarrowGapThresholdMeters = 0.0f;
    float NarrowGapEventsPer100m = 0.0f;

    float CapsuleRadiusMeters = 0.0f;
    float CapsuleWidthMeters = 0.0f;
    float CapsuleHeightMeters = 0.0f;
    FString NarrowGapDefinition;

    // Component facts are retained from the same fit-mask flood fill that drives the route
    // verdict.  They are exposed here so an editor diagnostic does not need to sample the field
    // a second time just to explain a failed mouth-to-mouth walk.
    int32 PlayerFitComponents = 0;
    int64 LargestPlayerFitComponentCells = 0;
    int64 ArrivalComponentCells = 0;
    int64 DepartureComponentCells = 0;
    float MouthComponentGapVoxels = -1.0f;

    // Optional room probes evaluated against this same grid and start component.  The arrays are
    // empty for ordinary callers; when supplied, each entry is 1 only when the probe is both a
    // player-fit cell and in the arrival/start component.
    TArray<uint8> ComponentProbePlayerFit;
    TArray<uint8> ComponentProbeInStartComponent;

    // The DFS returns to its root when the target component is unreachable.  Keep both the
    // physical end of that walk and the last newly reached pose: the latter is the useful
    // failure-frontier point for a render, while the former is the literal final pose.
    bool bHasAgentFinalPosition = false;
    FVector AgentFinalVoxels = FVector::ZeroVector;
    bool bHasLastReachedPosition = false;
    FVector LastReachedVoxels = FVector::ZeroVector;
    bool bHasTargetComponentNearestCell = false;
    FVector TargetComponentNearestVoxels = FVector::ZeroVector;
    float AgentToTargetComponentGapVoxels = -1.0f;

    // Origin checks are facts about the sampled fit mask.  An origin-inclusive secondary probe
    // can populate these even when the primary mouth-sized window deliberately excludes (0,0).
    bool bOriginColumnInSampledWindow = false;
    bool bOriginColumnHasPlayerFit = false;
    int64 OriginColumnPlayerFitCells = 0;
    bool bOriginColumnReachable = false;
    float ReachableSetToOriginColumnVoxels = -1.0f;
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
    const FVoxelStrateMeasureSettings& Settings,
    FVoxelStrateSampleGrid* OutSampleGrid = nullptr);

/** The same Tier 2 pass over an explicit voxel Z window and a custom read-only sampler. */
VOXELFORGE_API FVoxelStrateMetrics VF_MeasureStrateWithSampler(
    const IVoxelStrateDensitySampler& Sampler,
    int32 StrateBottomWorldZ,
    int32 StrateTopWorldZ,
    float BoundarySealThickness,
    const FVoxelStrateMeasureSettings& Settings,
    FVoxelStrateSampleGrid* OutSampleGrid = nullptr);

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

/**
 * Player-fit connectivity over a custom stack. This is deliberately a separate API from the
 * legacy air route: it refuses unless Settings.SampleStep == 1, resolves endpoints against the
 * direct capsule-occupancy/floor mask, and flood-fills/routes only through player-fit cells.
 */
VOXELFORGE_API EVoxelConnectivityResult VF_ArePlayerFitConnectedWithSampler(
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

/** Full diagnostics variant for the player-fit custom-stack route. */
VOXELFORGE_API FVoxelConnectivityDiagnostics VF_DiagnosePlayerFitConnectivityWithSampler(
    const IVoxelStrateDensitySampler& Sampler,
    int32 StrateBottomWorldZ,
    int32 StrateTopWorldZ,
    float BoundarySealThickness,
    const FVector& AVoxel,
    const FVector& BVoxel,
    const FVoxelStrateMeasureSettings& Settings,
    FVoxelStrateMetrics* OutPlayerMetrics = nullptr);

/**
 * Instrument a deterministic agent over the same exact player-fit graph used by the route gate.
 * The return value is false only when the bounded fine grid/stencil could not be built; a valid
 * report may still say NotConnectedAtThisResolution or another explicit connectivity result.
 */
VOXELFORGE_API bool VF_MeasurePlayerFitWalkWithSampler(
    const IVoxelStrateDensitySampler& Sampler,
    int32 StrateBottomWorldZ,
    int32 StrateTopWorldZ,
    float BoundarySealThickness,
    const FVector& AVoxel,
    const FVector& BVoxel,
    const FVoxelStrateMeasureSettings& Settings,
    FVoxelPlayerFitWalkReport& OutReport,
    const TArray<FVector>* ComponentProbePoints = nullptr,
    FVoxelStrateSampleGrid* OutSampleGrid = nullptr);
