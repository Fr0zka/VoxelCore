// Deterministic editor/automation previews for one measured strate sample grid.
//
// The renderer never calls a density function. It consumes the optional capture emitted by
// VF_MeasureStrate/VF_MeasureStrateWithSampler, so the pictures and the reported metrics describe
// the same sampled world.

#pragma once

#include "CoreMinimal.h"
#include "VoxelStrateMeasure.h"

/** The window facts printed on the contact sheet beside every measured number. */
struct VOXELFORGE_API FVoxelStratePreviewWindow
{
    int32 SampleStep = 0;
    int32 ResolvedMarginVoxels = 0;
    int32 SampledMinZ = 0;
    int32 SampledMaxZ = 0;
    int32 NumX = 0;
    int32 NumY = 0;
    int32 NumZ = 0;
    float MinX = 0.0f;
    float MaxX = 0.0f;
    float MinY = 0.0f;
    float MaxY = 0.0f;

    bool IsValid() const
    {
        return SampleStep > 0 && NumX > 0 && NumY > 0 && NumZ > 0
            && SampledMaxZ > SampledMinZ;
    }

    FString Describe() const;
};

/** Parameters for the optional selected-candidate fine pass. */
struct VOXELFORGE_API FVoxelStrateFinePreviewSettings
{
    // The default is deliberately one voxel: the pass exists to expose detail that step 4 cannot.
    int32 SampleStep = 1;
    // radius=64 gives a 128x128 XY ROI and stays below the default two-million-cell cap for the
    // four-chunk fixture. The caller may re-centre it on the deterministic largest open space
    // point found by the coarse air flood fill; it is not inherently the strate midpoint.
    int32 RadiusInVoxels = 64;
    int32 MaxCells = 2000000;
    FVector2D CenterXY = FVector2D::ZeroVector;
    int32 InteriorMarginVoxels = -1;

    bool IsValid() const
    {
        return SampleStep > 0 && RadiusInVoxels > 0 && MaxCells > 0
            && FMath::IsFinite(CenterXY.X) && FMath::IsFinite(CenterXY.Y);
    }

    FVoxelStrateMeasureSettings MakeMeasureSettings(
        const FVoxelStrateMeasureSettings& BaseSettings) const
    {
        FVoxelStrateMeasureSettings Settings = BaseSettings;
        Settings.SampleStep = SampleStep;
        Settings.RadiusInVoxels = RadiusInVoxels;
        Settings.CenterXY = CenterXY;
        Settings.CoverPointA.Reset();
        Settings.CoverPointB.Reset();
        Settings.MaxCells = MaxCells;
        Settings.InteriorMarginVoxels = InteriorMarginVoxels;
        return Settings;
    }
};

/** One lightweight row/card record; sampled cells are rendered immediately and are not retained. */
struct VOXELFORGE_API FVoxelStratePreviewCandidate
{
    int32 CandidateIndex = INDEX_NONE;
    // Showcase cards carry their archetype and the exact editor hand-off values. Ordinary
    // contact-sheet callers may leave these unset and retain the legacy candidate heading.
    FString ArchetypeName;
    bool bShowcaseCard = false;
    int32 ComposerSeed = INDEX_NONE;
    int32 ComposerTargetStrateIndex = INDEX_NONE;
    bool bComposerRollStructure = false;
    // Season pages deliberately opt into descent order. The ordinary candidate contact sheet
    // keeps its survivor/outlier ordering; a season review must read top-to-bottom like play.
    bool bSeasonOrder = false;
    int32 DepthIndex = INDEX_NONE;
    FString SelectionReason;
    bool bBossSlot = false;
    FVoxelStratePreviewWindow Window;
    FString RecipeString;
    FString ArrivalDepartureVerdict;
    FString RejectionReason;
    FString RenderFailureReason;
    FString VerticalImageFile;
    FString VerticalContourImageFile;
    FString PlanImageFile;
    FString PlanContourImageFile;

    // Fine files are intentionally separate from the coarse names, even when the ROI happens to
    // have the same raster dimensions. The exact fine window is repeated on the card.
    FString FineRenderFailureReason;
    FString FineVerticalImageFile;
    FString FineVerticalContourImageFile;
    FString FinePlanImageFile;
    FString FinePlanContourImageFile;
    FVoxelStratePreviewWindow FineWindow;

    bool bRejected = false;
    bool bMetricsValid = false;
    bool bRendered = false;
    bool bContourRendered = false;
    bool bFineRequested = false;
    bool bFineRendered = false;
    bool bFineContourRendered = false;
    bool bFineVerticalContentVaries = false;
    bool bFinePlanContentVaries = false;
    bool bFineBlank = false;

    float AirFraction = 0.0f;
    float LargestComponentShare = 0.0f;
    float WalkableFraction = 0.0f;
    int64 WalkableFloorColumns = 0;
    float WalkableFloorAreaFraction = 0.0f;
    int32 NumWalkableSurfaceComponents = 0;
    int64 LargestWalkableSurfaceColumns = 0;
    float LargestWalkableSurfaceShare = 0.0f;
    float MedianFeatureScale = 0.0f;
    int32 MedianVerticalClearance = 0;
    bool bPlayerFitResolved = false;
    FString PlayerFitRefusalReason;
    int64 NumPlayerFitCells = 0;
    float PlayerFitFraction = 0.0f;
    int32 NumTraversableComponents = 0;
    int64 LargestTraversableComponentCells = 0;
    float TraversableComponentShare = 0.0f;
    float MinimumPlayerClearanceVoxels = 0.0f;
    double DistanceFromCorpusCentroid = 0.0;

    int32 VerticalSliceCellY = INDEX_NONE;
    int32 VerticalSliceWorldY = 0;
    int32 VerticalImageWidth = 0;
    int32 VerticalImageHeight = 0;
    int32 PlanSliceCellZ = INDEX_NONE;
    int32 PlanSliceWorldZ = 0;
    int32 PlanImageWidth = 0;
    int32 PlanImageHeight = 0;
    int32 PlanSliceBoundaryTransitions = 0;
    int32 PlanSliceMixedCells = 0;

    int32 FineVerticalSliceCellY = INDEX_NONE;
    int32 FineVerticalSliceWorldY = 0;
    int32 FineVerticalImageWidth = 0;
    int32 FineVerticalImageHeight = 0;
    int32 FinePlanSliceCellZ = INDEX_NONE;
    int32 FinePlanSliceWorldZ = 0;
    int32 FinePlanImageWidth = 0;
    int32 FinePlanImageHeight = 0;
};

/** Aggregate Part A row for a stable archetype-level review table. */
struct VOXELFORGE_API FVoxelStratePreviewArchetypeSummary
{
    FString ArchetypeName;
    int32 EvaluatedCandidates = 0;
    int32 HardGateSurvivors = 0;
    FVoxelStratePreviewWindow Window;
    FString WindowSummary;

    FString WalkableFractionSummary;
    FString WalkableFloorAreaSummary;
    FString MedianVerticalClearanceSummary;
    FString LargestWalkableSurfaceSummary;
    FString PlayerFitFractionSummary;
    FString TraversableComponentShareSummary;
};

/** Copy the exact measurement-window metadata needed by the index page. */
VOXELFORGE_API FVoxelStratePreviewWindow VF_GetStratePreviewWindow(
    const FVoxelStrateSampleGrid& Grid);

/**
 * Render one candidate's XZ and XY views and fill its contact-sheet record.
 *
 * The source grid is the measurement capture, not a sampler. The renderer is deterministic and
 * has no random stream. It returns true for a valid measurement with no grid only when it can
 * record a non-rendered candidate (for example an invalid recipe); PNG failures return false.
 */
VOXELFORGE_API bool VF_WriteStratePreviewCandidate(
    const FString& OutputDirectory,
    int32 CandidateIndex,
    const FString& RecipeString,
    const FVoxelStrateSampleGrid& Grid,
    const FVoxelStrateMetrics& Metrics,
    int32 HeadroomCells,
    double DistanceFromCorpusCentroid,
    const FString& ArrivalDepartureVerdict,
    bool bRejected,
    const FString& RejectionReason,
    FVoxelStratePreviewCandidate& OutCandidate,
    FString& OutError);

/**
 * Attach a separately measured fine ROI to an existing candidate record.
 *
 * The caller chooses which candidates receive this call. An invalid grid is recorded as a
 * refused/not-rendered fine preview and returns true, so a MaxCells refusal remains visible in
 * the contact sheet without being mistaken for a PNG failure. The renderer consumes the exact
 * captured Air and Density arrays and never calls a sampler.
 */
VOXELFORGE_API bool VF_WriteStratePreviewFineCandidate(
    const FString& OutputDirectory,
    int32 CandidateIndex,
    const FVoxelStrateSampleGrid& FineGrid,
    int32 HeadroomCells,
    const FString& FineFailureReason,
    FVoxelStratePreviewCandidate& InOutCandidate,
    FString& OutError);

/** Write one self-contained, offline contact sheet for all candidate records. */
VOXELFORGE_API bool VF_WriteStratePreviewIndex(
    const FString& OutputDirectory,
    const FString& RunTitle,
    const FVoxelStratePreviewWindow& Window,
    const TArray<FVoxelStratePreviewCandidate>& Candidates,
    FString& OutIndexPath,
    FString& OutError,
    const TArray<FVoxelStratePreviewArchetypeSummary>* ArchetypeSummaries = nullptr);
