// Deterministic editor/automation strate previews.

#include "VoxelStratePreview.h"

FString FVoxelStratePreviewWindow::Describe() const
{
    return FString::Printf(
        TEXT("interior margin=%d voxels; sample step=%d voxels; Z=[%d,%d) voxels; "
             "XY=[%.0f,%.0f) x [%.0f,%.0f) voxels; grid=%dx%dx%d"),
        ResolvedMarginVoxels,
        SampleStep,
        SampledMinZ,
        SampledMaxZ,
        MinX,
        MaxX,
        MinY,
        MaxY,
        NumX,
        NumY,
        NumZ);
}

FVoxelStratePreviewWindow VF_GetStratePreviewWindow(const FVoxelStrateSampleGrid& Grid)
{
    FVoxelStratePreviewWindow Window;
    Window.SampleStep = Grid.SampleStep;
    Window.ResolvedMarginVoxels = Grid.ResolvedMarginVoxels;
    Window.SampledMinZ = Grid.SampledMinZ;
    Window.SampledMaxZ = Grid.SampledMaxZ;
    Window.NumX = Grid.NumX;
    Window.NumY = Grid.NumY;
    Window.NumZ = Grid.NumZ;
    Window.MinX = Grid.MinX;
    Window.MaxX = Grid.MaxX;
    Window.MinY = Grid.MinY;
    Window.MaxY = Grid.MaxY;
    return Window;
}

#if WITH_EDITOR

#include "HAL/FileManager.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Misc/FileHelper.h"
#include "Modules/ModuleManager.h"

namespace
{
    constexpr int32 GMaxPreviewImageDimension = 512;
    constexpr int32 GPreviewScaleBarFooterPixels = 20;

    const FColor GSolidColor(18, 23, 30, 255);
    const FColor GAirColor(218, 225, 231, 255);
    const FColor GWalkableColor(245, 166, 54, 255);
    const FColor GScaleBarColor(245, 245, 245, 255);
    const FColor GScaleBarBackground(9, 12, 17, 255);

    struct FPlanSliceChoice
    {
        int32 CellZ = INDEX_NONE;
        int32 BoundaryTransitions = 0;
        int32 MixedCells = 0;
        int32 WalkableCells = 0;
        int32 SolidCells = 0;
        bool bHasMixedLayer = false;
    };

    FString VF_HtmlEscape(const FString& Source)
    {
        FString Escaped = Source;
        Escaped = Escaped.Replace(TEXT("&"), TEXT("&amp;"));
        Escaped = Escaped.Replace(TEXT("<"), TEXT("&lt;"));
        Escaped = Escaped.Replace(TEXT(">"), TEXT("&gt;"));
        Escaped = Escaped.Replace(TEXT("\""), TEXT("&quot;"));
        Escaped = Escaped.Replace(TEXT("'"), TEXT("&#39;"));
        return Escaped;
    }

    FString VF_FormatMetric(bool bValid, float Value)
    {
        return bValid ? FString::Printf(TEXT("%.6f"), Value) : TEXT("invalid");
    }

    FString VF_FormatIntegerMetric(bool bValid, int32 Value)
    {
        return bValid ? FString::Printf(TEXT("%d"), Value) : TEXT("invalid");
    }

    bool VF_IsWalkableCell(const FVoxelStrateSampleGrid& Grid,
                           int32 X, int32 Y, int32 Z, int32 HeadroomCells)
    {
        if (Grid.Air[Grid.Index(X, Y, Z)] == 0u || Z == 0
            || Grid.Air[Grid.Index(X, Y, Z - 1)] != 0u)
        {
            return false;
        }

        for (int32 H = 0; H < HeadroomCells; ++H)
        {
            const int64 HeadroomZ = static_cast<int64>(Z) + H;
            if (HeadroomZ >= Grid.NumZ
                || Grid.Air[Grid.Index(X, Y, static_cast<int32>(HeadroomZ))] == 0u)
            {
                return false;
            }
        }
        return true;
    }

    int32 VF_SourceBegin(int32 OutputIndex, int32 OutputCount, int32 SourceCount)
    {
        return static_cast<int32>(
            (static_cast<int64>(OutputIndex) * static_cast<int64>(SourceCount))
            / static_cast<int64>(OutputCount));
    }

    int32 VF_SourceEnd(int32 OutputIndex, int32 OutputCount, int32 SourceCount)
    {
        const int32 Begin = VF_SourceBegin(OutputIndex, OutputCount, SourceCount);
        const int32 End = static_cast<int32>(
            (static_cast<int64>(OutputIndex + 1) * static_cast<int64>(SourceCount)
                + static_cast<int64>(OutputCount) - 1)
            / static_cast<int64>(OutputCount));
        return FMath::Clamp(FMath::Max(Begin + 1, End), 0, SourceCount);
    }

    FColor VF_AggregateVerticalPixel(const FVoxelStrateSampleGrid& Grid,
                                     int32 FixedY,
                                     int32 XBegin,
                                     int32 XEnd,
                                     int32 ZBegin,
                                     int32 ZEnd,
                                     int32 HeadroomCells)
    {
        bool bAnyAir = false;
        bool bAnyWalkable = false;
        for (int32 Z = ZBegin; Z < ZEnd; ++Z)
        {
            for (int32 X = XBegin; X < XEnd; ++X)
            {
                const int32 Cell = Grid.Index(X, FixedY, Z);
                if (Grid.Air[Cell] != 0u)
                {
                    bAnyAir = true;
                    bAnyWalkable |= VF_IsWalkableCell(Grid, X, FixedY, Z, HeadroomCells);
                }
            }
        }
        return bAnyWalkable ? GWalkableColor : bAnyAir ? GAirColor : GSolidColor;
    }

    FColor VF_AggregatePlanPixel(const FVoxelStrateSampleGrid& Grid,
                                 int32 FixedZ,
                                 int32 XBegin,
                                 int32 XEnd,
                                 int32 YBegin,
                                 int32 YEnd,
                                 int32 HeadroomCells)
    {
        bool bAnyAir = false;
        bool bAnyWalkable = false;
        for (int32 Y = YBegin; Y < YEnd; ++Y)
        {
            for (int32 X = XBegin; X < XEnd; ++X)
            {
                const int32 Cell = Grid.Index(X, Y, FixedZ);
                if (Grid.Air[Cell] != 0u)
                {
                    bAnyAir = true;
                    bAnyWalkable |= VF_IsWalkableCell(Grid, X, Y, FixedZ, HeadroomCells);
                }
            }
        }
        return bAnyWalkable ? GWalkableColor : bAnyAir ? GAirColor : GSolidColor;
    }

    FPlanSliceChoice VF_ChoosePlanSlice(const FVoxelStrateSampleGrid& Grid,
                                        int32 HeadroomCells)
    {
        FPlanSliceChoice Best;
        const int32 CentreZ = Grid.NumZ / 2;

        auto IsBetterMixed = [CentreZ](const FPlanSliceChoice& Candidate,
                                       const FPlanSliceChoice& Existing)
        {
            if (Candidate.BoundaryTransitions != Existing.BoundaryTransitions)
            {
                return Candidate.BoundaryTransitions > Existing.BoundaryTransitions;
            }
            if (Candidate.MixedCells != Existing.MixedCells)
            {
                return Candidate.MixedCells > Existing.MixedCells;
            }
            if (Candidate.WalkableCells != Existing.WalkableCells)
            {
                return Candidate.WalkableCells > Existing.WalkableCells;
            }
            return FMath::Abs(Candidate.CellZ - CentreZ)
                < FMath::Abs(Existing.CellZ - CentreZ);
        };

        auto IsBetterFallback = [CentreZ](const FPlanSliceChoice& Candidate,
                                           const FPlanSliceChoice& Existing)
        {
            if (Candidate.SolidCells != Existing.SolidCells)
            {
                return Candidate.SolidCells > Existing.SolidCells;
            }
            if (Candidate.MixedCells != Existing.MixedCells)
            {
                return Candidate.MixedCells > Existing.MixedCells;
            }
            return FMath::Abs(Candidate.CellZ - CentreZ)
                < FMath::Abs(Existing.CellZ - CentreZ);
        };

        for (int32 Z = 0; Z < Grid.NumZ; ++Z)
        {
            FPlanSliceChoice Candidate;
            Candidate.CellZ = Z;
            for (int32 Y = 0; Y < Grid.NumY; ++Y)
            {
                for (int32 X = 0; X < Grid.NumX; ++X)
                {
                    const bool bAir = Grid.Air[Grid.Index(X, Y, Z)] != 0u;
                    if (bAir)
                    {
                        ++Candidate.WalkableCells;
                        if (!VF_IsWalkableCell(Grid, X, Y, Z, HeadroomCells))
                        {
                            --Candidate.WalkableCells;
                        }
                    }
                    else
                    {
                        ++Candidate.SolidCells;
                    }
                    if (X + 1 < Grid.NumX
                        && bAir != (Grid.Air[Grid.Index(X + 1, Y, Z)] != 0u))
                    {
                        ++Candidate.BoundaryTransitions;
                    }
                    if (Y + 1 < Grid.NumY
                        && bAir != (Grid.Air[Grid.Index(X, Y + 1, Z)] != 0u))
                    {
                        ++Candidate.BoundaryTransitions;
                    }
                }
            }
            Candidate.MixedCells = FMath::Min(
                Candidate.SolidCells,
                Grid.NumX * Grid.NumY - Candidate.SolidCells);
            Candidate.bHasMixedLayer = Candidate.SolidCells > 0
                && Candidate.SolidCells < Grid.NumX * Grid.NumY;

            if (Candidate.bHasMixedLayer)
            {
                if (!Best.bHasMixedLayer || IsBetterMixed(Candidate, Best))
                {
                    Best = Candidate;
                }
            }
            else if (!Best.bHasMixedLayer
                     && (Best.CellZ == INDEX_NONE || IsBetterFallback(Candidate, Best)))
            {
                Best = Candidate;
            }
        }

        return Best;
    }

    int32 VF_CellWorldCoordinate(float Min, float Max, int32 SampleStep, int32 Cell)
    {
        const float Extent = Max - Min;
        const float Offset = FMath::Min(
            static_cast<float>(Cell) * static_cast<float>(SampleStep)
                + 0.5f * static_cast<float>(SampleStep),
            Extent - 0.5f);
        return FMath::RoundToInt(Min + Offset);
    }

    void VF_DrawScaleBar(TArray<FColor>& Pixels, int32 Width, int32 ContentHeight,
                         int32 SourceWidth, int32 SampleStep)
    {
        if (Width <= 0 || ContentHeight < 0 || Pixels.Num() <= 0 || SourceWidth <= 0
            || SampleStep <= 0)
        {
            return;
        }

        const int32 ImageHeight = Pixels.Num() / Width;
        const int32 BarY = FMath::Clamp(ContentHeight + 8, 0, ImageHeight - 1);
        const int32 BarHeight = FMath::Min(4, ImageHeight - BarY);
        const int32 BarX = FMath::Min(8, Width - 1);
        const double ChunkPixels = static_cast<double>(Width) * 32.0
            / (static_cast<double>(SourceWidth) * static_cast<double>(SampleStep));
        const int32 BarWidth = FMath::Clamp(
            FMath::RoundToInt(static_cast<float>(ChunkPixels)), 1, FMath::Max(1, Width - BarX));

        for (int32 Y = 0; Y < ImageHeight; ++Y)
        {
            for (int32 X = 0; X < Width; ++X)
            {
                if (Y >= ContentHeight)
                {
                    Pixels[Y * Width + X] = GScaleBarBackground;
                }
            }
        }
        for (int32 Y = BarY; Y < BarY + BarHeight; ++Y)
        {
            for (int32 X = BarX; X < BarX + BarWidth && X < Width; ++X)
            {
                Pixels[Y * Width + X] = GScaleBarColor;
            }
        }
    }

    bool VF_RenderVertical(const FVoxelStrateSampleGrid& Grid, int32 HeadroomCells,
                           int32 FixedY, TArray<FColor>& OutPixels,
                           int32& OutWidth, int32& OutHeight)
    {
        const int32 MaxContentDimension = GMaxPreviewImageDimension
            - GPreviewScaleBarFooterPixels;
        OutWidth = FMath::Min(Grid.NumX, MaxContentDimension);
        const int32 ContentHeight = FMath::Min(Grid.NumZ, MaxContentDimension);
        OutHeight = ContentHeight + GPreviewScaleBarFooterPixels;
        OutPixels.SetNumUninitialized(OutWidth * OutHeight);

        for (int32 PixelZ = 0; PixelZ < ContentHeight; ++PixelZ)
        {
            // Put the highest sampled cells at the top of the image, as a person reading an XZ
            // section expects; the source grid itself remains in its deterministic Z order.
            const int32 ImageZ = ContentHeight - 1 - PixelZ;
            const int32 ZBegin = VF_SourceBegin(ImageZ, ContentHeight, Grid.NumZ);
            const int32 ZEnd = VF_SourceEnd(ImageZ, ContentHeight, Grid.NumZ);
            for (int32 PixelX = 0; PixelX < OutWidth; ++PixelX)
            {
                const int32 XBegin = VF_SourceBegin(PixelX, OutWidth, Grid.NumX);
                const int32 XEnd = VF_SourceEnd(PixelX, OutWidth, Grid.NumX);
                OutPixels[PixelZ * OutWidth + PixelX] = VF_AggregateVerticalPixel(
                    Grid, FixedY, XBegin, XEnd, ZBegin, ZEnd, HeadroomCells);
            }
        }
        VF_DrawScaleBar(OutPixels, OutWidth, ContentHeight, Grid.NumX, Grid.SampleStep);
        return OutWidth > 0 && OutHeight > 0;
    }

    bool VF_RenderPlan(const FVoxelStrateSampleGrid& Grid, int32 HeadroomCells,
                       int32 FixedZ, TArray<FColor>& OutPixels,
                       int32& OutWidth, int32& OutHeight)
    {
        const int32 MaxContentDimension = GMaxPreviewImageDimension
            - GPreviewScaleBarFooterPixels;
        OutWidth = FMath::Min(Grid.NumX, MaxContentDimension);
        const int32 ContentHeight = FMath::Min(Grid.NumY, MaxContentDimension);
        OutHeight = ContentHeight + GPreviewScaleBarFooterPixels;
        OutPixels.SetNumUninitialized(OutWidth * OutHeight);

        for (int32 PixelY = 0; PixelY < ContentHeight; ++PixelY)
        {
            const int32 YBegin = VF_SourceBegin(PixelY, ContentHeight, Grid.NumY);
            const int32 YEnd = VF_SourceEnd(PixelY, ContentHeight, Grid.NumY);
            for (int32 PixelX = 0; PixelX < OutWidth; ++PixelX)
            {
                const int32 XBegin = VF_SourceBegin(PixelX, OutWidth, Grid.NumX);
                const int32 XEnd = VF_SourceEnd(PixelX, OutWidth, Grid.NumX);
                OutPixels[PixelY * OutWidth + PixelX] = VF_AggregatePlanPixel(
                    Grid, FixedZ, XBegin, XEnd, YBegin, YEnd, HeadroomCells);
            }
        }
        VF_DrawScaleBar(OutPixels, OutWidth, ContentHeight, Grid.NumX, Grid.SampleStep);
        return OutWidth > 0 && OutHeight > 0;
    }

    bool VF_WritePng(const FString& Path, const TArray<FColor>& Pixels,
                     int32 Width, int32 Height, FString& OutError)
    {
        IImageWrapperModule& Module = FModuleManager::LoadModuleChecked<IImageWrapperModule>(
            FName("ImageWrapper"));
        const TSharedPtr<IImageWrapper> Wrapper = Module.CreateImageWrapper(EImageFormat::PNG);
        if (!Wrapper.IsValid()
            || !Wrapper->SetRaw(Pixels.GetData(),
                                static_cast<int64>(Pixels.Num()) * sizeof(FColor),
                                Width, Height, ERGBFormat::BGRA, 8))
        {
            OutError = FString::Printf(TEXT("failed to encode PNG %s"), *Path);
            return false;
        }

        const TArray64<uint8>& Png = Wrapper->GetCompressed(100);
        if (!FFileHelper::SaveArrayToFile(Png, *Path))
        {
            OutError = FString::Printf(TEXT("failed to write PNG %s"), *Path);
            return false;
        }
        return true;
    }
}

bool VF_WriteStratePreviewCandidate(
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
    FString& OutError)
{
    OutCandidate = FVoxelStratePreviewCandidate();
    OutCandidate.CandidateIndex = CandidateIndex;
    OutCandidate.RecipeString = RecipeString;
    OutCandidate.ArrivalDepartureVerdict = ArrivalDepartureVerdict;
    OutCandidate.RejectionReason = RejectionReason;
    OutCandidate.bRejected = bRejected;
    OutCandidate.bMetricsValid = Metrics.bValid;
    OutCandidate.AirFraction = Metrics.AirFraction;
    OutCandidate.LargestComponentShare = Metrics.LargestComponentShare;
    OutCandidate.WalkableFraction = Metrics.WalkableFraction;
    OutCandidate.MedianFeatureScale = Metrics.MedianFeatureScale;
    OutCandidate.MedianVerticalClearance = Metrics.MedianVerticalClearance;
    OutCandidate.DistanceFromCorpusCentroid = FMath::IsFinite(DistanceFromCorpusCentroid)
        ? DistanceFromCorpusCentroid : 0.0;

    if (!Grid.IsValid())
    {
        // An invalid recipe/measurement still gets a visible row in the contact sheet; there is
        // simply no honest grid from which to manufacture images.
        OutCandidate.RenderFailureReason = Metrics.RefusalReason.IsEmpty()
            ? TEXT("no sampled grid was available") : Metrics.RefusalReason;
        return true;
    }
    OutCandidate.Window = VF_GetStratePreviewWindow(Grid);
    if (HeadroomCells < 0)
    {
        OutError = TEXT("preview HeadroomCells cannot be negative");
        return false;
    }

    const FPlanSliceChoice Plan = VF_ChoosePlanSlice(Grid, HeadroomCells);
    const float CentreY = 0.5f * (Grid.MinY + Grid.MaxY);
    const float Step = static_cast<float>(Grid.SampleStep);
    const int32 FixedY = FMath::Clamp(
        FMath::FloorToInt((CentreY - Grid.MinY) / Step), 0, Grid.NumY - 1);

    TArray<FColor> VerticalPixels;
    int32 VerticalWidth = 0;
    int32 VerticalHeight = 0;
    if (!VF_RenderVertical(Grid, HeadroomCells, FixedY, VerticalPixels,
                           VerticalWidth, VerticalHeight))
    {
        OutError = FString::Printf(TEXT("failed to rasterise vertical slice for candidate %d"),
                                   CandidateIndex);
        return false;
    }

    TArray<FColor> PlanPixels;
    int32 PlanWidth = 0;
    int32 PlanHeight = 0;
    if (!VF_RenderPlan(Grid, HeadroomCells, Plan.CellZ, PlanPixels, PlanWidth, PlanHeight))
    {
        OutError = FString::Printf(TEXT("failed to rasterise plan slice for candidate %d"),
                                   CandidateIndex);
        return false;
    }

    if (!IFileManager::Get().MakeDirectory(*OutputDirectory, true))
    {
        OutError = FString::Printf(TEXT("failed to create preview directory %s"),
                                   *OutputDirectory);
        return false;
    }

    const FString Stem = FString::Printf(TEXT("candidate_%03d"), CandidateIndex);
    OutCandidate.VerticalImageFile = Stem + TEXT("_vertical.png");
    OutCandidate.PlanImageFile = Stem + TEXT("_plan.png");
    const FString VerticalPath = OutputDirectory / OutCandidate.VerticalImageFile;
    const FString PlanPath = OutputDirectory / OutCandidate.PlanImageFile;
    if (!VF_WritePng(VerticalPath, VerticalPixels, VerticalWidth, VerticalHeight, OutError)
        || !VF_WritePng(PlanPath, PlanPixels, PlanWidth, PlanHeight, OutError))
    {
        return false;
    }

    OutCandidate.bRendered = true;
    OutCandidate.VerticalSliceCellY = FixedY;
    OutCandidate.VerticalSliceWorldY = VF_CellWorldCoordinate(
        Grid.MinY, Grid.MaxY, Grid.SampleStep, FixedY);
    OutCandidate.VerticalImageWidth = VerticalWidth;
    OutCandidate.VerticalImageHeight = VerticalHeight;
    OutCandidate.PlanSliceCellZ = Plan.CellZ;
    OutCandidate.PlanSliceWorldZ = VF_CellWorldCoordinate(
        Grid.MinZ, Grid.MaxZ, Grid.SampleStep, Plan.CellZ);
    OutCandidate.PlanImageWidth = PlanWidth;
    OutCandidate.PlanImageHeight = PlanHeight;
    OutCandidate.PlanSliceBoundaryTransitions = Plan.BoundaryTransitions;
    OutCandidate.PlanSliceMixedCells = Plan.MixedCells;
    return true;
}

bool VF_WriteStratePreviewIndex(
    const FString& OutputDirectory,
    const FString& RunTitle,
    const FVoxelStratePreviewWindow& Window,
    const TArray<FVoxelStratePreviewCandidate>& Candidates,
    FString& OutIndexPath,
    FString& OutError)
{
    if (!Window.IsValid())
    {
        OutError = TEXT("cannot write preview index for an invalid measurement window");
        return false;
    }
    if (!IFileManager::Get().MakeDirectory(*OutputDirectory, true))
    {
        OutError = FString::Printf(TEXT("failed to create preview directory %s"),
                                   *OutputDirectory);
        return false;
    }

    TArray<FVoxelStratePreviewCandidate> SortedCandidates = Candidates;
    SortedCandidates.Sort([](const FVoxelStratePreviewCandidate& A,
                             const FVoxelStratePreviewCandidate& B)
    {
        if (A.bRejected != B.bRejected)
        {
            return !A.bRejected;
        }
        if (A.DistanceFromCorpusCentroid != B.DistanceFromCorpusCentroid)
        {
            // Outliers should be visible near the top of each group.
            return A.DistanceFromCorpusCentroid > B.DistanceFromCorpusCentroid;
        }
        return A.CandidateIndex < B.CandidateIndex;
    });

    const FString WindowDescription = Window.Describe();
    bool bAllWindowsSame = true;
    for (const FVoxelStratePreviewCandidate& Candidate : SortedCandidates)
    {
        if (Candidate.Window.IsValid()
            && Candidate.Window.Describe() != WindowDescription)
        {
            bAllWindowsSame = false;
            break;
        }
    }
    const FString WindowPreamble = bAllWindowsSame
        ? FString::Printf(TEXT("<strong>Measurement window for every metric below:</strong> %s."),
                          *VF_HtmlEscape(WindowDescription))
        : TEXT("<strong>Measurement windows:</strong> candidates resolve their own exact window "
               "from the same measurement settings; each card repeats its window.");
    FString Html;
    Html.Reserve(256 * FMath::Max(1, SortedCandidates.Num()));
    Html += TEXT("<!doctype html>\n<html lang=\"en\"><head><meta charset=\"utf-8\">\n");
    Html += FString::Printf(TEXT("<title>%s</title>\n"), *VF_HtmlEscape(RunTitle));
    Html += TEXT(
        "<style>\n"
        ":root{color-scheme:dark;background:#0d1117;color:#e6edf3;font:14px/1.4 system-ui,sans-serif}\n"
        "body{margin:24px;max-width:1800px}\n"
        "h1{margin:0 0 8px;font-size:24px}.note{color:#aab4c0;max-width:1100px}\n"
        ".legend{display:flex;gap:16px;flex-wrap:wrap;margin:16px 0}.swatch{display:inline-block;width:14px;height:14px;margin-right:5px;vertical-align:-2px;border:1px solid #68717c}\n"
        ".grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(390px,1fr));gap:16px}\n"
        ".candidate{background:#161b22;border:1px solid #30363d;border-radius:8px;padding:14px;min-width:0}\n"
        ".candidate.rejected{border-color:#8b3b42;background:#21171a}.candidate h2{margin:0 0 6px;font-size:18px}\n"
        ".badge{border-radius:999px;padding:2px 7px;font-size:11px;font-weight:700;letter-spacing:.04em}.survivor{background:#1f6f43;color:#d7ffe5}.rejected-badge{background:#9b3440;color:#fff0f1}\n"
        ".reason{color:#ffb4b8;margin:6px 0}.recipe{color:#c9d1d9;overflow-wrap:anywhere;margin-bottom:8px}.recipe code{font:12px ui-monospace,SFMono-Regular,monospace}\n"
        ".window{font-size:11px;color:#8b949e;margin:5px 0 10px}.images{display:grid;grid-template-columns:1fr 1fr;gap:8px}.images figure{margin:0}.images img{display:block;width:100%;height:auto;background:#090c11;image-rendering:pixelated;border:1px solid #30363d}.images figcaption{font-size:11px;color:#aab4c0;margin-top:3px}\n"
        ".no-image{min-height:40px;border:1px dashed #8b3b42;color:#ffb4b8;padding:12px;font-size:12px}\n"
        "table{width:100%;border-collapse:collapse;margin-top:10px;font-size:12px}th,td{padding:3px 4px;border-bottom:1px solid #30363d;text-align:left}th{color:#aab4c0;font-weight:500}td{text-align:right;font-variant-numeric:tabular-nums}.verdict{font-weight:650}\n"
        ".footer{color:#8b949e;font-size:11px;margin-top:18px}\n"
        "</style></head><body>\n");
    Html += FString::Printf(TEXT("<h1>%s</h1>\n"), *VF_HtmlEscape(RunTitle));
    Html += FString::Printf(
        TEXT("<p class=\"note\">%s "
             "Density polarity is explicit: density &gt; 0 is air. One chunk = 32 voxels = 8 m "
             "(at this sample step, one chunk spans %d sampled cells). The image footer contains "
             "a one-chunk scale bar.</p>\n"),
        *WindowPreamble,
        32 / Window.SampleStep);
    Html += TEXT(
        "<p class=\"note\"><strong>Connectivity caveat:</strong> these are only 2D slices from the "
        "measurement grid. Two regions that look joined in one slice may still be disconnected "
        "in 3D. The arrival→departure verdict is the separate coarse-route/full-resolution "
        "connectivity check, not something the pictures prove.</p>\n"
        "<div class=\"legend\"><span><i class=\"swatch\" style=\"background:#12171e\"></i>solid</span>"
        "<span><i class=\"swatch\" style=\"background:#dae1e7\"></i>air</span>"
        "<span><i class=\"swatch\" style=\"background:#f5a636\"></i>walkable cell</span></div>\n"
        "<p class=\"note\">Ordering: survivors first, then each group by descending distance from "
        "the corpus centroid so the strangest candidates appear early. The preview raster cap is "
        "512 px per image dimension including a 20 px scale-bar footer; if a source grid is larger, "
        "pixels aggregate its already-sampled cells and never resample the world.</p>\n"
        "<p class=\"note\">The corpus-centroid distance is a normalized roll-space ordering aid, "
        "not a sampled-world measurement. Every world metric and slice choice uses the exact "
        "measurement window printed on its card.</p>\n"
        "<div class=\"grid\">\n");

    for (const FVoxelStratePreviewCandidate& Candidate : SortedCandidates)
    {
        const FString Status = Candidate.bRejected ? TEXT("REJECTED") : TEXT("SURVIVOR");
        const FString StatusClass = Candidate.bRejected ? TEXT("rejected-badge") : TEXT("survivor");
        const FString CardClass = Candidate.bRejected ? TEXT("candidate rejected") : TEXT("candidate");
        const FString MetricWindow = Candidate.Window.IsValid()
            ? Candidate.Window.Describe() : WindowDescription;
        Html += FString::Printf(
            TEXT("<section class=\"%s\"><h2>#%d <span class=\"badge %s\">%s</span></h2>\n"),
            *CardClass, Candidate.CandidateIndex, *StatusClass, *Status);
        Html += FString::Printf(TEXT("<div class=\"recipe\"><code>%s</code></div>\n"),
                                *VF_HtmlEscape(Candidate.RecipeString));
        if (Candidate.bRejected)
        {
            Html += FString::Printf(TEXT("<p class=\"reason\"><strong>Why rejected:</strong> %s</p>\n"),
                                    *VF_HtmlEscape(Candidate.RejectionReason.IsEmpty()
                                        ? TEXT("unspecified") : Candidate.RejectionReason));
        }
        Html += FString::Printf(TEXT("<p class=\"window\">Numbers and slice choices use: %s</p>\n"),
                                *VF_HtmlEscape(MetricWindow));
        Html += TEXT("<div class=\"images\">\n");
        if (Candidate.bRendered)
        {
            Html += FString::Printf(
                TEXT("<figure><img src=\"%s\" alt=\"candidate %d vertical XZ slice\"><figcaption>"
                     "Vertical XZ @ centre Y=%d voxels</figcaption></figure>\n"
                     "<figure><img src=\"%s\" alt=\"candidate %d plan XY slice\"><figcaption>"
                     "Plan XY @ selected Z=%d voxels; %d solid/air boundary transitions</figcaption></figure>\n"),
                *VF_HtmlEscape(Candidate.VerticalImageFile), Candidate.CandidateIndex,
                Candidate.VerticalSliceWorldY,
                *VF_HtmlEscape(Candidate.PlanImageFile), Candidate.CandidateIndex,
                Candidate.PlanSliceWorldZ, Candidate.PlanSliceBoundaryTransitions);
        }
        else
        {
            Html += FString::Printf(TEXT("<div class=\"no-image\">No image: %s</div>\n"),
                                    *VF_HtmlEscape(Candidate.RenderFailureReason));
        }
        Html += TEXT("</div>\n<table><caption class=\"window\">Grid measurements use the window above; "
                    "centroid distance is roll-space</caption>\n");
        Html += FString::Printf(
            TEXT("<tr><th>air fraction</th><td>%s</td></tr>\n"
                 "<tr><th>largest component share</th><td>%s</td></tr>\n"
                 "<tr><th>walkable fraction</th><td>%s</td></tr>\n"
                 "<tr><th>feature scale (voxels)</th><td>%s</td></tr>\n"
                 "<tr><th>vertical clearance (voxels)</th><td>%s</td></tr>\n"
                 "<tr><th>arrival → departure</th><td class=\"verdict\">%s</td></tr>\n"
                 "<tr><th>distance from corpus centroid (roll-space)</th><td>%.6f</td></tr>\n</table>\n</section>\n"),
            *VF_FormatMetric(Candidate.bMetricsValid, Candidate.AirFraction),
            *VF_FormatMetric(Candidate.bMetricsValid, Candidate.LargestComponentShare),
            *VF_FormatMetric(Candidate.bMetricsValid, Candidate.WalkableFraction),
            *VF_FormatMetric(Candidate.bMetricsValid, Candidate.MedianFeatureScale),
            *VF_FormatIntegerMetric(Candidate.bMetricsValid, Candidate.MedianVerticalClearance),
            *VF_HtmlEscape(Candidate.ArrivalDepartureVerdict),
            Candidate.DistanceFromCorpusCentroid);
    }
    Html += TEXT("</div>\n<p class=\"footer\">Generated offline by VoxelForge's editor/automation "
                "composer preview. No CDN, JavaScript framework, or runtime generation hook is "
                "required.</p>\n</body></html>\n");

    OutIndexPath = OutputDirectory / TEXT("index.html");
    if (!FFileHelper::SaveStringToFile(
            Html, *OutIndexPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
    {
        OutError = FString::Printf(TEXT("failed to write preview index %s"), *OutIndexPath);
        return false;
    }
    return true;
}

#else

bool VF_WriteStratePreviewCandidate(
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
    FString& OutError)
{
    (void)OutputDirectory;
    (void)CandidateIndex;
    (void)RecipeString;
    (void)Grid;
    (void)Metrics;
    (void)HeadroomCells;
    (void)DistanceFromCorpusCentroid;
    (void)ArrivalDepartureVerdict;
    (void)bRejected;
    (void)RejectionReason;
    OutCandidate = FVoxelStratePreviewCandidate();
    OutError = TEXT("strate preview rendering is editor-only");
    return false;
}

bool VF_WriteStratePreviewIndex(
    const FString& OutputDirectory,
    const FString& RunTitle,
    const FVoxelStratePreviewWindow& Window,
    const TArray<FVoxelStratePreviewCandidate>& Candidates,
    FString& OutIndexPath,
    FString& OutError)
{
    (void)OutputDirectory;
    (void)RunTitle;
    (void)Window;
    (void)Candidates;
    OutIndexPath.Reset();
    OutError = TEXT("strate preview rendering is editor-only");
    return false;
}

#endif
