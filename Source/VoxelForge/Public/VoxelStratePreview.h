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

/** One lightweight row/card record; sampled cells are rendered immediately and are not retained. */
struct VOXELFORGE_API FVoxelStratePreviewCandidate
{
    int32 CandidateIndex = INDEX_NONE;
    FVoxelStratePreviewWindow Window;
    FString RecipeString;
    FString ArrivalDepartureVerdict;
    FString RejectionReason;
    FString RenderFailureReason;
    FString VerticalImageFile;
    FString PlanImageFile;

    bool bRejected = false;
    bool bMetricsValid = false;
    bool bRendered = false;

    float AirFraction = 0.0f;
    float LargestComponentShare = 0.0f;
    float WalkableFraction = 0.0f;
    float MedianFeatureScale = 0.0f;
    int32 MedianVerticalClearance = 0;
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

/** Write one self-contained, offline contact sheet for all candidate records. */
VOXELFORGE_API bool VF_WriteStratePreviewIndex(
    const FString& OutputDirectory,
    const FString& RunTitle,
    const FVoxelStratePreviewWindow& Window,
    const TArray<FVoxelStratePreviewCandidate>& Candidates,
    FString& OutIndexPath,
    FString& OutError);
