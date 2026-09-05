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
    const FColor GContourBackground(12, 19, 25, 255);
    const FColor GContourLineColor(77, 226, 190, 255);
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

    FString VF_FormatInteger64Metric(bool bValid, int64 Value)
    {
        return bValid ? FString::Printf(TEXT("%lld"), static_cast<long long>(Value)) : TEXT("invalid");
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

    bool VF_HasContentVariation(const TArray<FColor>& Pixels,
                                int32 Width,
                                int32 ContentHeight)
    {
        if (Width <= 0 || ContentHeight <= 0 || Pixels.Num() < Width * ContentHeight)
        {
            return false;
        }
        const FColor First = Pixels[0];
        for (int32 Y = 0; Y < ContentHeight; ++Y)
        {
            for (int32 X = 0; X < Width; ++X)
            {
                if (Pixels[Y * Width + X] != First)
                {
                    return true;
                }
            }
        }
        return false;
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

    struct FContourPoint
    {
        float X = 0.0f;
        float Y = 0.0f;
    };

    FContourPoint VF_InterpolateContourPoint(const FContourPoint& A,
                                             const FContourPoint& B,
                                             float ValueA,
                                             float ValueB)
    {
        const float Denominator = ValueA - ValueB;
        const float Alpha = FMath::IsNearlyZero(Denominator)
            ? 0.5f
            : FMath::Clamp(ValueA / Denominator, 0.0f, 1.0f);
        return {
            FMath::Lerp(A.X, B.X, Alpha),
            FMath::Lerp(A.Y, B.Y, Alpha)
        };
    }

    void VF_DrawContourLine(TArray<FColor>& Pixels, int32 Width, int32 Height,
                            const FContourPoint& A, const FContourPoint& B)
    {
        // The caller passes the content height so the scale-bar footer is never painted over.
        // The pixel buffer is intentionally taller than this region.
        if (Width <= 0 || Height <= 0 || Pixels.Num() < Width * Height)
        {
            return;
        }

        int32 X0 = FMath::Clamp(FMath::RoundToInt(A.X), 0, Width - 1);
        int32 Y0 = FMath::Clamp(FMath::RoundToInt(A.Y), 0, Height - 1);
        const int32 X1 = FMath::Clamp(FMath::RoundToInt(B.X), 0, Width - 1);
        const int32 Y1 = FMath::Clamp(FMath::RoundToInt(B.Y), 0, Height - 1);
        const int32 DeltaX = FMath::Abs(X1 - X0);
        const int32 StepX = X0 < X1 ? 1 : -1;
        const int32 DeltaY = -FMath::Abs(Y1 - Y0);
        const int32 StepY = Y0 < Y1 ? 1 : -1;
        int32 Error = DeltaX + DeltaY;

        for (;;)
        {
            Pixels[Y0 * Width + X0] = GContourLineColor;
            if (X0 == X1 && Y0 == Y1)
            {
                break;
            }
            const int32 DoubleError = 2 * Error;
            if (DoubleError >= DeltaY)
            {
                Error += DeltaY;
                X0 += StepX;
            }
            if (DoubleError <= DeltaX)
            {
                Error += DeltaX;
                Y0 += StepY;
            }
        }
    }

    void VF_DrawContourQuad(TArray<FColor>& Pixels, int32 Width, int32 Height,
                            const FContourPoint& A, const FContourPoint& B,
                            const FContourPoint& C, const FContourPoint& D,
                            float ValueA, float ValueB, float ValueC, float ValueD)
    {
        if (!FMath::IsFinite(ValueA) || !FMath::IsFinite(ValueB)
            || !FMath::IsFinite(ValueC) || !FMath::IsFinite(ValueD))
        {
            return;
        }

        // A/B/C/D wind around one scalar quad. The bit mask is deliberately based on the same
        // strict sign rule as Air: density > 0 is air, density <= 0 is solid.
        const int32 Mask = (ValueA > 0.0f ? 1 : 0)
            | (ValueB > 0.0f ? 2 : 0)
            | (ValueC > 0.0f ? 4 : 0)
            | (ValueD > 0.0f ? 8 : 0);
        if (Mask == 0 || Mask == 15)
        {
            return;
        }

        const FContourPoint Edge0 = VF_InterpolateContourPoint(A, B, ValueA, ValueB);
        const FContourPoint Edge1 = VF_InterpolateContourPoint(B, C, ValueB, ValueC);
        const FContourPoint Edge2 = VF_InterpolateContourPoint(C, D, ValueC, ValueD);
        const FContourPoint Edge3 = VF_InterpolateContourPoint(D, A, ValueD, ValueA);
        const float CentreValue = 0.25f * (ValueA + ValueB + ValueC + ValueD);

        auto Draw = [&](const FContourPoint& First, const FContourPoint& Second)
        {
            VF_DrawContourLine(Pixels, Width, Height, First, Second);
        };

        switch (Mask)
        {
        case 1:  Draw(Edge3, Edge0); break;
        case 2:  Draw(Edge0, Edge1); break;
        case 3:  Draw(Edge3, Edge1); break;
        case 4:  Draw(Edge1, Edge2); break;
        case 5:
            // The diagonal case is decided from the scalar centre, not from the binary image.
            // This makes the saddle deterministic while keeping the isoline tied to the field.
            if (CentreValue > 0.0f)
            {
                Draw(Edge0, Edge1);
                Draw(Edge2, Edge3);
            }
            else
            {
                Draw(Edge3, Edge0);
                Draw(Edge1, Edge2);
            }
            break;
        case 6:  Draw(Edge0, Edge2); break;
        case 7:  Draw(Edge2, Edge3); break;
        case 8:  Draw(Edge2, Edge3); break;
        case 9:  Draw(Edge0, Edge2); break;
        case 10:
            if (CentreValue > 0.0f)
            {
                Draw(Edge3, Edge0);
                Draw(Edge1, Edge2);
            }
            else
            {
                Draw(Edge0, Edge1);
                Draw(Edge2, Edge3);
            }
            break;
        case 11: Draw(Edge1, Edge2); break;
        case 12: Draw(Edge1, Edge3); break;
        case 13: Draw(Edge0, Edge1); break;
        case 14: Draw(Edge0, Edge3); break;
        default: break;
        }
    }

    FContourPoint VF_MapVerticalContourPoint(int32 X, int32 Z,
                                              int32 SourceWidth, int32 SourceHeight,
                                              int32 OutputWidth, int32 ContentHeight)
    {
        return {
            SourceWidth > 1
                ? static_cast<float>(X) * static_cast<float>(OutputWidth - 1)
                    / static_cast<float>(SourceWidth - 1)
                : 0.0f,
            SourceHeight > 1
                ? static_cast<float>(SourceHeight - 1 - Z)
                    * static_cast<float>(ContentHeight - 1)
                    / static_cast<float>(SourceHeight - 1)
                : 0.0f
        };
    }

    FContourPoint VF_MapPlanContourPoint(int32 X, int32 Y,
                                          int32 SourceWidth, int32 SourceHeight,
                                          int32 OutputWidth, int32 ContentHeight)
    {
        return {
            SourceWidth > 1
                ? static_cast<float>(X) * static_cast<float>(OutputWidth - 1)
                    / static_cast<float>(SourceWidth - 1)
                : 0.0f,
            SourceHeight > 1
                ? static_cast<float>(Y) * static_cast<float>(ContentHeight - 1)
                    / static_cast<float>(SourceHeight - 1)
                : 0.0f
        };
    }

    bool VF_RenderVerticalContour(const FVoxelStrateSampleGrid& Grid, int32 FixedY,
                                  TArray<FColor>& OutPixels,
                                  int32& OutWidth, int32& OutHeight)
    {
        if (!Grid.HasScalarDensity())
        {
            return false;
        }

        const int32 MaxContentDimension = GMaxPreviewImageDimension
            - GPreviewScaleBarFooterPixels;
        OutWidth = FMath::Min(Grid.NumX, MaxContentDimension);
        const int32 ContentHeight = FMath::Min(Grid.NumZ, MaxContentDimension);
        OutHeight = ContentHeight + GPreviewScaleBarFooterPixels;
        OutPixels.Init(GContourBackground, OutWidth * OutHeight);
        if (Grid.NumX > 1 && Grid.NumZ > 1)
        {
            for (int32 Z = 0; Z + 1 < Grid.NumZ; ++Z)
            {
                for (int32 X = 0; X + 1 < Grid.NumX; ++X)
                {
                    const FContourPoint A = VF_MapVerticalContourPoint(
                        X, Z, Grid.NumX, Grid.NumZ, OutWidth, ContentHeight);
                    const FContourPoint B = VF_MapVerticalContourPoint(
                        X + 1, Z, Grid.NumX, Grid.NumZ, OutWidth, ContentHeight);
                    const FContourPoint C = VF_MapVerticalContourPoint(
                        X + 1, Z + 1, Grid.NumX, Grid.NumZ, OutWidth, ContentHeight);
                    const FContourPoint D = VF_MapVerticalContourPoint(
                        X, Z + 1, Grid.NumX, Grid.NumZ, OutWidth, ContentHeight);
                    VF_DrawContourQuad(
                        OutPixels, OutWidth, ContentHeight, A, B, C, D,
                        Grid.Density[Grid.Index(X, FixedY, Z)],
                        Grid.Density[Grid.Index(X + 1, FixedY, Z)],
                        Grid.Density[Grid.Index(X + 1, FixedY, Z + 1)],
                        Grid.Density[Grid.Index(X, FixedY, Z + 1)]);
                }
            }
        }
        VF_DrawScaleBar(OutPixels, OutWidth, ContentHeight, Grid.NumX, Grid.SampleStep);
        return OutWidth > 0 && OutHeight > 0;
    }

    bool VF_RenderPlanContour(const FVoxelStrateSampleGrid& Grid, int32 FixedZ,
                              TArray<FColor>& OutPixels,
                              int32& OutWidth, int32& OutHeight)
    {
        if (!Grid.HasScalarDensity())
        {
            return false;
        }

        const int32 MaxContentDimension = GMaxPreviewImageDimension
            - GPreviewScaleBarFooterPixels;
        OutWidth = FMath::Min(Grid.NumX, MaxContentDimension);
        const int32 ContentHeight = FMath::Min(Grid.NumY, MaxContentDimension);
        OutHeight = ContentHeight + GPreviewScaleBarFooterPixels;
        OutPixels.Init(GContourBackground, OutWidth * OutHeight);
        if (Grid.NumX > 1 && Grid.NumY > 1)
        {
            for (int32 Y = 0; Y + 1 < Grid.NumY; ++Y)
            {
                for (int32 X = 0; X + 1 < Grid.NumX; ++X)
                {
                    const FContourPoint A = VF_MapPlanContourPoint(
                        X, Y, Grid.NumX, Grid.NumY, OutWidth, ContentHeight);
                    const FContourPoint B = VF_MapPlanContourPoint(
                        X + 1, Y, Grid.NumX, Grid.NumY, OutWidth, ContentHeight);
                    const FContourPoint C = VF_MapPlanContourPoint(
                        X + 1, Y + 1, Grid.NumX, Grid.NumY, OutWidth, ContentHeight);
                    const FContourPoint D = VF_MapPlanContourPoint(
                        X, Y + 1, Grid.NumX, Grid.NumY, OutWidth, ContentHeight);
                    VF_DrawContourQuad(
                        OutPixels, OutWidth, ContentHeight, A, B, C, D,
                        Grid.Density[Grid.Index(X, Y, FixedZ)],
                        Grid.Density[Grid.Index(X + 1, Y, FixedZ)],
                        Grid.Density[Grid.Index(X + 1, Y + 1, FixedZ)],
                        Grid.Density[Grid.Index(X, Y + 1, FixedZ)]);
                }
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
    OutCandidate.WalkableFloorColumns = Metrics.WalkableFloorColumns;
    OutCandidate.WalkableFloorAreaFraction = Metrics.WalkableFloorAreaFraction;
    OutCandidate.NumWalkableSurfaceComponents = Metrics.NumWalkableSurfaceComponents;
    OutCandidate.LargestWalkableSurfaceColumns = Metrics.LargestWalkableSurfaceColumns;
    OutCandidate.LargestWalkableSurfaceShare = Metrics.LargestWalkableSurfaceShare;
    OutCandidate.MedianFeatureScale = Metrics.MedianFeatureScale;
    OutCandidate.MedianVerticalClearance = Metrics.MedianVerticalClearance;
    OutCandidate.bPlayerFitResolved = Metrics.bPlayerFitResolved;
    OutCandidate.PlayerFitRefusalReason = Metrics.PlayerFitRefusalReason;
    OutCandidate.NumPlayerFitCells = Metrics.NumPlayerFitCells;
    OutCandidate.PlayerFitFraction = Metrics.PlayerFitFraction;
    OutCandidate.NumTraversableComponents = Metrics.NumTraversableComponents;
    OutCandidate.LargestTraversableComponentCells =
        Metrics.LargestTraversableComponentCells;
    OutCandidate.TraversableComponentShare = Metrics.TraversableComponentShare;
    OutCandidate.MinimumPlayerClearanceVoxels = Metrics.MinimumPlayerClearanceVoxels;
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

    if (!IFileManager::Get().MakeDirectory(*OutputDirectory, true))
    {
        OutError = FString::Printf(TEXT("failed to create preview directory %s"),
                                   *OutputDirectory);
        return false;
    }

    const FString Stem = FString::Printf(TEXT("candidate_%03d"), CandidateIndex);
    OutCandidate.VerticalImageFile = Stem + TEXT("_vertical.png");
    OutCandidate.VerticalContourImageFile = Stem + TEXT("_vertical_contour.png");
    OutCandidate.PlanImageFile = Stem + TEXT("_plan.png");
    OutCandidate.PlanContourImageFile = Stem + TEXT("_plan_contour.png");
    const FString VerticalPath = OutputDirectory / OutCandidate.VerticalImageFile;
    if (!VF_WritePng(VerticalPath, VerticalPixels, VerticalWidth, VerticalHeight, OutError))
    {
        return false;
    }

    const bool bHasContour = Grid.HasScalarDensity();
    if (bHasContour)
    {
        int32 ContourWidth = 0;
        int32 ContourHeight = 0;
        if (!VF_RenderVerticalContour(Grid, FixedY, VerticalPixels,
                                      ContourWidth, ContourHeight)
            || ContourWidth != VerticalWidth || ContourHeight != VerticalHeight
            || !VF_WritePng(OutputDirectory / OutCandidate.VerticalContourImageFile,
                            VerticalPixels, ContourWidth, ContourHeight, OutError))
        {
            OutError = FString::Printf(TEXT("failed to write vertical density contour for candidate %d"),
                                       CandidateIndex);
            return false;
        }
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

    const FString PlanPath = OutputDirectory / OutCandidate.PlanImageFile;
    if (!VF_WritePng(PlanPath, PlanPixels, PlanWidth, PlanHeight, OutError))
    {
        return false;
    }
    if (bHasContour)
    {
        int32 ContourWidth = 0;
        int32 ContourHeight = 0;
        if (!VF_RenderPlanContour(Grid, Plan.CellZ, PlanPixels,
                                  ContourWidth, ContourHeight)
            || ContourWidth != PlanWidth || ContourHeight != PlanHeight
            || !VF_WritePng(OutputDirectory / OutCandidate.PlanContourImageFile,
                            PlanPixels, ContourWidth, ContourHeight, OutError))
        {
            OutError = FString::Printf(TEXT("failed to write plan density contour for candidate %d"),
                                       CandidateIndex);
            return false;
        }
    }

    OutCandidate.bRendered = true;
    OutCandidate.bContourRendered = bHasContour;
    if (!bHasContour)
    {
        OutCandidate.RenderFailureReason = TEXT(
            "no scalar density capture was supplied; filled cells were rendered, but no density=0 contour is available");
    }
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

bool VF_WriteStratePreviewFineCandidate(
    const FString& OutputDirectory,
    int32 CandidateIndex,
    const FVoxelStrateSampleGrid& FineGrid,
    int32 HeadroomCells,
    const FString& FineFailureReason,
    FVoxelStratePreviewCandidate& InOutCandidate,
    FString& OutError)
{
    InOutCandidate.bFineRequested = true;
    InOutCandidate.FineRenderFailureReason = FineFailureReason;
    if (!FineGrid.IsValid())
    {
        if (InOutCandidate.FineRenderFailureReason.IsEmpty())
        {
            InOutCandidate.FineRenderFailureReason = TEXT("fine ROI measurement was refused");
        }
        return true;
    }
    InOutCandidate.FineWindow = VF_GetStratePreviewWindow(FineGrid);
    if (HeadroomCells < 0)
    {
        OutError = TEXT("preview HeadroomCells cannot be negative");
        return false;
    }

    const FPlanSliceChoice Plan = VF_ChoosePlanSlice(FineGrid, HeadroomCells);
    const float CentreY = 0.5f * (FineGrid.MinY + FineGrid.MaxY);
    const float Step = static_cast<float>(FineGrid.SampleStep);
    const int32 FixedY = FMath::Clamp(
        FMath::FloorToInt((CentreY - FineGrid.MinY) / Step), 0, FineGrid.NumY - 1);

    if (!IFileManager::Get().MakeDirectory(*OutputDirectory, true))
    {
        OutError = FString::Printf(TEXT("failed to create preview directory %s"),
                                   *OutputDirectory);
        return false;
    }

    const FString Stem = FString::Printf(TEXT("candidate_%03d_fine_step_%d"),
                                          CandidateIndex, FineGrid.SampleStep);
    InOutCandidate.FineVerticalImageFile = Stem + TEXT("_vertical_filled.png");
    InOutCandidate.FineVerticalContourImageFile = Stem + TEXT("_vertical_contour.png");
    InOutCandidate.FinePlanImageFile = Stem + TEXT("_plan_filled.png");
    InOutCandidate.FinePlanContourImageFile = Stem + TEXT("_plan_contour.png");

    TArray<FColor> Pixels;
    int32 Width = 0;
    int32 Height = 0;
    if (!VF_RenderVertical(FineGrid, HeadroomCells, FixedY, Pixels, Width, Height)
        || !VF_WritePng(OutputDirectory / InOutCandidate.FineVerticalImageFile,
                        Pixels, Width, Height, OutError))
    {
        OutError = FString::Printf(TEXT("failed to write fine vertical filled view for candidate %d"),
                                   CandidateIndex);
        return false;
    }
    InOutCandidate.bFineVerticalContentVaries = VF_HasContentVariation(
        Pixels, Width, Height - GPreviewScaleBarFooterPixels);

    const bool bHasContour = FineGrid.HasScalarDensity();
    if (bHasContour)
    {
        int32 ContourWidth = 0;
        int32 ContourHeight = 0;
        if (!VF_RenderVerticalContour(FineGrid, FixedY, Pixels,
                                       ContourWidth, ContourHeight)
            || ContourWidth != Width || ContourHeight != Height
            || !VF_WritePng(OutputDirectory / InOutCandidate.FineVerticalContourImageFile,
                            Pixels, ContourWidth, ContourHeight, OutError))
        {
            OutError = FString::Printf(TEXT("failed to write fine vertical density contour for candidate %d"),
                                       CandidateIndex);
            return false;
        }
    }

    InOutCandidate.FineVerticalSliceCellY = FixedY;
    InOutCandidate.FineVerticalSliceWorldY = VF_CellWorldCoordinate(
        FineGrid.MinY, FineGrid.MaxY, FineGrid.SampleStep, FixedY);
    InOutCandidate.FineVerticalImageWidth = Width;
    InOutCandidate.FineVerticalImageHeight = Height;

    if (!VF_RenderPlan(FineGrid, HeadroomCells, Plan.CellZ, Pixels, Width, Height)
        || !VF_WritePng(OutputDirectory / InOutCandidate.FinePlanImageFile,
                        Pixels, Width, Height, OutError))
    {
        OutError = FString::Printf(TEXT("failed to write fine plan filled view for candidate %d"),
                                   CandidateIndex);
        return false;
    }
    InOutCandidate.bFinePlanContentVaries = VF_HasContentVariation(
        Pixels, Width, Height - GPreviewScaleBarFooterPixels);
    if (bHasContour)
    {
        int32 ContourWidth = 0;
        int32 ContourHeight = 0;
        if (!VF_RenderPlanContour(FineGrid, Plan.CellZ, Pixels,
                                  ContourWidth, ContourHeight)
            || ContourWidth != Width || ContourHeight != Height
            || !VF_WritePng(OutputDirectory / InOutCandidate.FinePlanContourImageFile,
                            Pixels, ContourWidth, ContourHeight, OutError))
        {
            OutError = FString::Printf(TEXT("failed to write fine plan density contour for candidate %d"),
                                       CandidateIndex);
            return false;
        }
    }

    InOutCandidate.bFineRendered = true;
    InOutCandidate.bFineContourRendered = bHasContour;
    InOutCandidate.bFineBlank = !InOutCandidate.bFineVerticalContentVaries
        && !InOutCandidate.bFinePlanContentVaries;
    InOutCandidate.FinePlanSliceCellZ = Plan.CellZ;
    InOutCandidate.FinePlanSliceWorldZ = VF_CellWorldCoordinate(
        FineGrid.MinZ, FineGrid.MaxZ, FineGrid.SampleStep, Plan.CellZ);
    InOutCandidate.FinePlanImageWidth = Width;
    InOutCandidate.FinePlanImageHeight = Height;
    if (!bHasContour && InOutCandidate.FineRenderFailureReason.IsEmpty())
    {
        InOutCandidate.FineRenderFailureReason = TEXT(
            "no scalar density capture was supplied; filled cells were rendered, but no density=0 contour is available");
    }
    return true;
}

bool VF_WriteStratePreviewIndex(
    const FString& OutputDirectory,
    const FString& RunTitle,
    const FVoxelStratePreviewWindow& Window,
    const TArray<FVoxelStratePreviewCandidate>& Candidates,
    FString& OutIndexPath,
    FString& OutError,
    const TArray<FVoxelStratePreviewArchetypeSummary>* ArchetypeSummaries)
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
        if (A.bShowcaseCard != B.bShowcaseCard)
        {
            return A.bShowcaseCard;
        }
        if (A.bShowcaseCard && B.bShowcaseCard
            && A.ArchetypeName != B.ArchetypeName)
        {
            return A.ArchetypeName < B.ArchetypeName;
        }
        if (A.bSeasonOrder != B.bSeasonOrder)
        {
            return A.bSeasonOrder;
        }
        if (A.bSeasonOrder && B.bSeasonOrder)
        {
            if (A.DepthIndex != B.DepthIndex)
            {
                return A.DepthIndex < B.DepthIndex;
            }
            return A.CandidateIndex < B.CandidateIndex;
        }
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
        ".window{font-size:11px;color:#8b949e;margin:5px 0 10px}.view-section{margin:12px 0}.view-section h3{font-size:12px;letter-spacing:.03em;color:#e6edf3;margin:10px 0 3px}.pair{display:grid;grid-template-columns:1fr 1fr;gap:8px}.pair figure{margin:0}.pair img{display:block;width:100%;height:auto;background:#090c11;image-rendering:pixelated;border:1px solid #30363d}.pair figcaption{font-size:11px;color:#aab4c0;margin-top:3px}.fine-section{border-top:1px solid #6e4e1f;margin-top:14px;padding-top:4px}.fine-section h3{color:#ffd580}.not-requested{color:#8b949e;font-size:12px;margin:8px 0}.contour-note{color:#8cebd1}\n"
        ".no-image{min-height:40px;border:1px dashed #8b3b42;color:#ffb4b8;padding:12px;font-size:12px}\n"
        ".pie-banner{border:2px solid #f5a636;background:#2b2112;color:#ffe5b0;border-radius:8px;padding:14px;margin:16px 0;font-size:15px}\n"
        ".pie-card{border:2px solid #f5a636;background:#2b2112;color:#ffe5b0;border-radius:6px;padding:10px;margin:8px 0 12px;font-size:13px;line-height:1.6}\n"
        ".pie-card code{font:13px ui-monospace,SFMono-Regular,monospace;color:#fff1ce}\n"
        ".sources a{color:#8cebd1}.summary{display:block;overflow-x:auto;white-space:nowrap}.summary th,.summary td{vertical-align:top}.summary-window{white-space:normal;min-width:260px}\n"
        "table{width:100%;border-collapse:collapse;margin-top:10px;font-size:12px}th,td{padding:3px 4px;border-bottom:1px solid #30363d;text-align:left}th{color:#aab4c0;font-weight:500}td{text-align:right;font-variant-numeric:tabular-nums}.verdict{font-weight:650}\n"
        ".footer{color:#8b949e;font-size:11px;margin-top:18px}\n"
        "</style></head><body>\n");
    Html += FString::Printf(TEXT("<h1>%s</h1>\n"), *VF_HtmlEscape(RunTitle));
    const bool bShowcase = ArchetypeSummaries != nullptr && !ArchetypeSummaries->IsEmpty();
    if (bShowcase)
    {
        Html += TEXT(
            "<div class=\"pie-banner\"><strong>PIE WALK-THROUGH — exact values per card</strong> "
            "Set the four displayed properties on the VoxelWorld actor, enter PIE, then click "
            "<strong>Apply Composer Candidate</strong>. The candidate seed/index are the same "
            "offline inputs used to produce these images. This page never changes runtime "
            "generation by itself.</div>\n"
            "<p class=\"note\"><strong>Showcase scope:</strong> all cards use the native "
            "single-region path with <code>bComposerRollStructure=false</code>; lateral regions "
            "are deliberately gated off. The target slot is reported explicitly on each card. "
            "The candidate archetype is the density replacement, while the live slot layout, "
            "height, passages, placement, and world seed remain unchanged.</p>\n"
            "<h2>Part A — what the numbers mean</h2>\n"
            "<p class=\"note\">Walkable fraction is <em>walkable sampled air cells / all sampled air "
            "cells</em>; it is a volumetric occupancy ratio and is expected to understate a large "
            "room. Walkable floor area is <em>distinct XY columns containing at least one "
            "walkable cell / sampled XY footprint</em>. Largest connected surface is the largest "
            "four-neighbour component of those projected columns / all walkable columns. It is "
            "not a 3D traversal proof; the arrival → departure route check is separate.</p>\n"
            "<p class=\"note\"><strong>Part A measurement settings:</strong> legacy fields use "
            "step=4 voxels, HeadroomCells=2, XY radius=256 voxels, MaxCells=8,000,000. The "
            "player-fit gate uses a separate step-1 fine ROI and refuses to answer at any coarser "
            "step. Each summary row lists the exact target, derived interior margin, Z range, XY "
            "range, and grid for every hard-gate survivor; each card repeats its selected window.</p>\n"
            "<p class=\"note\"><strong>Reality check:</strong> real caves mix long, narrow passages "
            "with rooms, branches, multiple levels, and occasional vertical shafts; there is no "
            "universal target floor-to-volume ratio. The quantitative table below is therefore a "
            "diagnostic comparison across this corpus, not a tuned magic number.</p>\n");
        Html += TEXT(
            "<p class=\"note sources\"><strong>References used:</strong> "
            "<a href=\"https://www.nps.gov/subjects/caves/solution-caves.htm\">NPS Solution Caves</a>; "
            "<a href=\"https://home.nps.gov/maca/learn/nature/how-mammoth-cave-formed.htm\">NPS Mammoth Cave morphology</a>; "
            "<a href=\"https://www.nps.gov/grba/learn/nature/lehman-caves-dimensions.htm\">NPS Lehman Caves dimensions</a>; "
            "<a href=\"https://store.steampowered.com/news/posts/?appgroupname=Deep+Rock+Galactic&amp;appids=548430&amp;enddate=1729500950&amp;feed=steam_community_announcements\">Deep Rock Galactic procedural cave design</a>; "
            "<a href=\"https://www.pcgamer.com/how-to-design-a-great-metroidvania-map/\">Team Cherry map-design interview</a>.</p>\n"
            "<table class=\"summary\"><caption>Part A — survivor distributions; every value uses the exact window printed in its row</caption>\n"
            "<tr><th>archetype</th><th>eligible single-region rolls / hard-gate survivors</th><th>walkable fraction<br>(median; min–max)</th>"
            "<th>floor area fraction<br>(median; min–max)</th><th>median clearance over walkable cells<br>(median; min–max voxels)</th>"
            "<th>largest connected surface<br>(median; min–max)</th><th>player-fit fraction<br>(selected fine ROI)</th>"
            "<th>traversable component share<br>(selected fine ROI)</th><th>window</th></tr>\n");
        for (const FVoxelStratePreviewArchetypeSummary& Summary : *ArchetypeSummaries)
        {
            const FString SummaryWindow = !Summary.WindowSummary.IsEmpty()
                ? Summary.WindowSummary
                : Summary.Window.IsValid() ? Summary.Window.Describe() : TEXT("unavailable");
            Html += FString::Printf(
                TEXT("<tr><th>%s</th><td>%d / %d</td><td>%s</td><td>%s</td><td>%s</td><td>%s</td><td>%s</td><td>%s</td><td class=\"summary-window\">%s</td></tr>\n"),
                *VF_HtmlEscape(Summary.ArchetypeName), Summary.EvaluatedCandidates,
                Summary.HardGateSurvivors,
                *VF_HtmlEscape(Summary.WalkableFractionSummary),
                *VF_HtmlEscape(Summary.WalkableFloorAreaSummary),
                *VF_HtmlEscape(Summary.MedianVerticalClearanceSummary),
                *VF_HtmlEscape(Summary.LargestWalkableSurfaceSummary),
                *VF_HtmlEscape(Summary.PlayerFitFractionSummary),
                *VF_HtmlEscape(Summary.TraversableComponentShareSummary),
                *VF_HtmlEscape(SummaryWindow));
        }
        Html += TEXT("</table>\n");
    }
    Html += FString::Printf(
        TEXT("<p class=\"note\"><strong>How to read coarse images:</strong> At sample step %d, "
             "hard edges can come from sampling, not necessarily hard world walls. A "
             "lattice-sourced strate is genuinely rectilinear at step 1; that is expected for a maze.</p>\n"),
        Window.SampleStep);
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
        "<span><i class=\"swatch\" style=\"background:#f5a636\"></i>walkable cell</span>"
        "<span><i class=\"swatch\" style=\"background:#4de2be\"></i>density=0 contour</span></div>\n"
        "<p class=\"note\"><strong>Fine ROI — player-fit audit:</strong> the separate step-1 pass "
        "is used by the player-fit gate for every candidate that reaches the legacy screen, and "
        "is rendered for the selected candidate. The caller supplies its bounded XY window; the "
        "showcase fits that window to the deterministic arrival/departure mouth pair so the gate "
        "cannot pass or fail because its endpoints were cropped out. Its exact window is printed "
        "on the card. If its cell cap is exceeded, no fine allocation is made and the refusal is "
        "shown on that card.</p>\n"
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
        const int32 DisplayIndex = Candidate.bSeasonOrder && Candidate.DepthIndex != INDEX_NONE
            ? Candidate.DepthIndex : Candidate.CandidateIndex;
        const FString CardTitle = Candidate.bShowcaseCard && !Candidate.ArchetypeName.IsEmpty()
            ? FString::Printf(TEXT("%s · candidate %d"), *Candidate.ArchetypeName, Candidate.CandidateIndex)
            : FString::Printf(TEXT("#%d"), DisplayIndex);
        Html += FString::Printf(
            TEXT("<section class=\"%s\"><h2>%s <span class=\"badge %s\">%s</span></h2>\n"),
            *CardClass, *VF_HtmlEscape(CardTitle), *StatusClass, *Status);
        if (Candidate.bShowcaseCard)
        {
            Html += FString::Printf(
                TEXT("<div class=\"pie-card\"><strong>PIE — type exactly, then click Apply Composer Candidate</strong>"
                     "<br><code>ComposerSeed = %d</code>"
                     "<br><code>ComposerCandidateIndex = %d</code>"
                     "<br><code>ComposerTargetStrateIndex = %d</code>"
                     "<br><code>bComposerRollStructure = %s</code></div>\n"),
                Candidate.ComposerSeed, Candidate.CandidateIndex,
                Candidate.ComposerTargetStrateIndex,
                Candidate.bComposerRollStructure ? TEXT("true") : TEXT("false"));
        }
        Html += FString::Printf(TEXT("<div class=\"recipe\"><code>%s</code></div>\n"),
                                *VF_HtmlEscape(Candidate.RecipeString));
        if (Candidate.bSeasonOrder || Candidate.bShowcaseCard)
        {
            const FString SelectionExtra = Candidate.bSeasonOrder
                ? FString::Printf(TEXT("<br><strong>Boss slot:</strong> %s"),
                                  Candidate.bBossSlot ? TEXT("yes") : TEXT("no"))
                : FString();
            Html += FString::Printf(
                TEXT("<p class=\"reason\" style=\"color:#d7e3f4\"><strong>Why selected:</strong> %s"
                     "%s</p>\n"),
                *VF_HtmlEscape(Candidate.SelectionReason.IsEmpty()
                    ? (Candidate.bShowcaseCard
                        ? TEXT("selected from the hard-gate survivor set by floor area, then vertical clearance")
                        : TEXT("selected by the provisional season policy"))
                    : Candidate.SelectionReason),
                *SelectionExtra);
        }
        if (Candidate.bRejected)
        {
            Html += FString::Printf(TEXT("<p class=\"reason\"><strong>Why rejected:</strong> %s</p>\n"),
                                    *VF_HtmlEscape(Candidate.RejectionReason.IsEmpty()
                                        ? TEXT("unspecified") : Candidate.RejectionReason));
        }
        Html += FString::Printf(TEXT("<p class=\"window\">Numbers and slice choices use: %s</p>\n"),
                                *VF_HtmlEscape(MetricWindow));
        Html += TEXT("<div class=\"view-section\"><h3>COARSE XZ — filled cells and density=0 contour</h3>\n");
        if (Candidate.bRendered)
        {
            Html += FString::Printf(
                TEXT("<p class=\"window\">Exact coarse window: %s; slice Y=%d voxels.</p>\n"
                     "<div class=\"pair\"><figure><img src=\"%s\" alt=\"candidate %d coarse XZ filled cells\"><figcaption>"
                     "COARSE · filled cells · step=%d</figcaption></figure>\n"),
                *VF_HtmlEscape(MetricWindow), Candidate.VerticalSliceWorldY,
                *VF_HtmlEscape(Candidate.VerticalImageFile), Candidate.CandidateIndex,
                Candidate.Window.SampleStep);
            if (Candidate.bContourRendered)
            {
                Html += FString::Printf(
                    TEXT("<figure><img src=\"%s\" alt=\"candidate %d coarse XZ density zero contour\"><figcaption>"
                         "COARSE · density=0 contour · scalar field · step=%d</figcaption></figure></div>\n"),
                    *VF_HtmlEscape(Candidate.VerticalContourImageFile), Candidate.CandidateIndex,
                    Candidate.Window.SampleStep);
            }
            else
            {
                Html += FString::Printf(
                    TEXT("<div class=\"no-image\"><span class=\"contour-note\">No coarse contour:</span> %s</div></div>\n"),
                    *VF_HtmlEscape(Candidate.RenderFailureReason));
            }
        }
        else
        {
            Html += FString::Printf(TEXT("<div class=\"no-image\">No coarse image: %s</div>\n"),
                                    *VF_HtmlEscape(Candidate.RenderFailureReason));
        }
        Html += TEXT("</div>\n<div class=\"view-section\"><h3>COARSE XY — filled cells and density=0 contour</h3>\n");
        if (Candidate.bRendered)
        {
            Html += FString::Printf(
                TEXT("<p class=\"window\">Exact coarse window: %s; selected Z=%d voxels; %d solid/air boundary transitions.</p>\n"
                     "<div class=\"pair\"><figure><img src=\"%s\" alt=\"candidate %d coarse XY filled cells\"><figcaption>"
                     "COARSE · filled cells · step=%d</figcaption></figure>\n"),
                *VF_HtmlEscape(MetricWindow), Candidate.PlanSliceWorldZ,
                Candidate.PlanSliceBoundaryTransitions,
                *VF_HtmlEscape(Candidate.PlanImageFile), Candidate.CandidateIndex,
                Candidate.Window.SampleStep);
            if (Candidate.bContourRendered)
            {
                Html += FString::Printf(
                    TEXT("<figure><img src=\"%s\" alt=\"candidate %d coarse XY density zero contour\"><figcaption>"
                         "COARSE · density=0 contour · scalar field · step=%d</figcaption></figure></div>\n"),
                    *VF_HtmlEscape(Candidate.PlanContourImageFile), Candidate.CandidateIndex,
                    Candidate.Window.SampleStep);
            }
            else
            {
                Html += FString::Printf(
                    TEXT("<div class=\"no-image\"><span class=\"contour-note\">No coarse contour:</span> %s</div></div>\n"),
                    *VF_HtmlEscape(Candidate.RenderFailureReason));
            }
        }
        else
        {
            Html += FString::Printf(TEXT("<div class=\"no-image\">No coarse image: %s</div>\n"),
                                    *VF_HtmlEscape(Candidate.RenderFailureReason));
        }
        Html += TEXT("</div>\n<div class=\"fine-section\"><h3>FINE ROI — player-fit audit; filled cells and density=0 contour</h3>\n");
        if (!Candidate.bFineRequested)
        {
            Html += TEXT("<p class=\"not-requested\">Fine ROI not requested for this candidate; it did not pass the coarse survivor screen.</p>\n");
        }
        else if (!Candidate.bFineRendered)
        {
            const FString FineFailureText = Candidate.FineRenderFailureReason.IsEmpty()
                ? TEXT("unspecified fine preview refusal") : Candidate.FineRenderFailureReason;
            Html += FString::Printf(
                TEXT("<div class=\"no-image\">Fine ROI requested but refused/not rendered: %s</div>\n"),
                *VF_HtmlEscape(FineFailureText));
        }
        else if (Candidate.bFineBlank)
        {
            Html += TEXT("<div class=\"no-image\"><strong>Fine ROI is blank at both selected slices.</strong> "
                        "That is reported as a content finding; empty images are intentionally not "
                        "shown as evidence of a visible room.</div>\n");
        }
        else
        {
            const FString FineWindowDescription = Candidate.FineWindow.Describe();
            Html += FString::Printf(
                TEXT("<p class=\"window\">Player-fit fine ROI; exact window: %s; slice Y=%d voxels.</p>\n"
                     "<div class=\"pair\"><figure><img src=\"%s\" alt=\"candidate %d fine XZ filled cells\"><figcaption>"
                     "FINE ROI · filled cells · step=%d</figcaption></figure>\n"),
                *VF_HtmlEscape(FineWindowDescription), Candidate.FineVerticalSliceWorldY,
                *VF_HtmlEscape(Candidate.FineVerticalImageFile), Candidate.CandidateIndex,
                Candidate.FineWindow.SampleStep);
            if (Candidate.bFineContourRendered)
            {
                Html += FString::Printf(
                    TEXT("<figure><img src=\"%s\" alt=\"candidate %d fine XZ density zero contour\"><figcaption>"
                         "FINE ROI · density=0 contour · scalar field · step=%d</figcaption></figure></div>\n"),
                    *VF_HtmlEscape(Candidate.FineVerticalContourImageFile), Candidate.CandidateIndex,
                    Candidate.FineWindow.SampleStep);
            }
            else
            {
                Html += FString::Printf(
                    TEXT("<div class=\"no-image\"><span class=\"contour-note\">No fine contour:</span> %s</div></div>\n"),
                    *VF_HtmlEscape(Candidate.FineRenderFailureReason));
            }

            Html += FString::Printf(
                TEXT("<p class=\"window\">Player-fit fine ROI; exact window: %s; selected Z=%d voxels.</p>\n"
                     "<div class=\"pair\"><figure><img src=\"%s\" alt=\"candidate %d fine XY filled cells\"><figcaption>"
                     "FINE ROI · filled cells · step=%d</figcaption></figure>\n"),
                *VF_HtmlEscape(FineWindowDescription), Candidate.FinePlanSliceWorldZ,
                *VF_HtmlEscape(Candidate.FinePlanImageFile), Candidate.CandidateIndex,
                Candidate.FineWindow.SampleStep);
            if (Candidate.bFineContourRendered)
            {
                Html += FString::Printf(
                    TEXT("<figure><img src=\"%s\" alt=\"candidate %d fine XY density zero contour\"><figcaption>"
                         "FINE ROI · density=0 contour · scalar field · step=%d</figcaption></figure></div>\n"),
                    *VF_HtmlEscape(Candidate.FinePlanContourImageFile), Candidate.CandidateIndex,
                    Candidate.FineWindow.SampleStep);
            }
            else
            {
                Html += FString::Printf(
                    TEXT("<div class=\"no-image\"><span class=\"contour-note\">No fine contour:</span> %s</div></div>\n"),
                    *VF_HtmlEscape(Candidate.FineRenderFailureReason));
            }
        }
        Html += TEXT("</div>\n<table><caption class=\"window\">Grid measurements use the window above; "
                    "centroid distance is roll-space</caption>\n");
        Html += FString::Printf(
            TEXT("<tr><th>air fraction</th><td>%s</td></tr>\n"
                 "<tr><th>largest component share</th><td>%s</td></tr>\n"
                 "<tr><th>walkable fraction</th><td>%s</td></tr>\n"
                 "<tr><th>walkable floor area fraction</th><td>%s</td></tr>\n"
                 "<tr><th>walkable floor columns</th><td>%s</td></tr>\n"
                 "<tr><th>largest connected surface share</th><td>%s</td></tr>\n"
                 "<tr><th>walkable surface components</th><td>%s</td></tr>\n"
                 "<tr><th>feature scale (voxels)</th><td>%s</td></tr>\n"
                 "<tr><th>vertical clearance (voxels)</th><td>%s</td></tr>\n"
                 "<tr><th>player-fit resolved</th><td>%s</td></tr>\n"
                 "<tr><th>player-fit fraction (fine ROI)</th><td>%s</td></tr>\n"
                 "<tr><th>player-fitting floor cells</th><td>%s</td></tr>\n"
                 "<tr><th>traversable component share (fine ROI)</th><td>%s</td></tr>\n"
                 "<tr><th>traversable components</th><td>%s</td></tr>\n"
                 "<tr><th>minimum player clearance (voxels)</th><td>%s</td></tr>\n"
                 "<tr><th>arrival → departure</th><td class=\"verdict\">%s</td></tr>\n"
                 "<tr><th>distance from corpus centroid (roll-space)</th><td>%.6f</td></tr>\n</table>\n</section>\n"),
            *VF_FormatMetric(Candidate.bMetricsValid, Candidate.AirFraction),
            *VF_FormatMetric(Candidate.bMetricsValid, Candidate.LargestComponentShare),
            *VF_FormatMetric(Candidate.bMetricsValid, Candidate.WalkableFraction),
            *VF_FormatMetric(Candidate.bMetricsValid, Candidate.WalkableFloorAreaFraction),
            *VF_FormatInteger64Metric(Candidate.bMetricsValid, Candidate.WalkableFloorColumns),
            *VF_FormatMetric(Candidate.bMetricsValid, Candidate.LargestWalkableSurfaceShare),
            *VF_FormatIntegerMetric(Candidate.bMetricsValid, Candidate.NumWalkableSurfaceComponents),
            *VF_FormatMetric(Candidate.bMetricsValid, Candidate.MedianFeatureScale),
            *VF_FormatIntegerMetric(Candidate.bMetricsValid, Candidate.MedianVerticalClearance),
            Candidate.bPlayerFitResolved ? TEXT("yes") : TEXT("no"),
            *VF_FormatMetric(Candidate.bPlayerFitResolved, Candidate.PlayerFitFraction),
            *VF_FormatInteger64Metric(Candidate.bPlayerFitResolved, Candidate.NumPlayerFitCells),
            *VF_FormatMetric(Candidate.bPlayerFitResolved, Candidate.TraversableComponentShare),
            *VF_FormatIntegerMetric(Candidate.bPlayerFitResolved, Candidate.NumTraversableComponents),
            *VF_FormatMetric(Candidate.bPlayerFitResolved, Candidate.MinimumPlayerClearanceVoxels),
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

bool VF_WriteStratePreviewFineCandidate(
    const FString& OutputDirectory,
    int32 CandidateIndex,
    const FVoxelStrateSampleGrid& FineGrid,
    int32 HeadroomCells,
    const FString& FineFailureReason,
    FVoxelStratePreviewCandidate& InOutCandidate,
    FString& OutError)
{
    (void)OutputDirectory;
    (void)CandidateIndex;
    (void)FineGrid;
    (void)HeadroomCells;
    (void)FineFailureReason;
    (void)InOutCandidate;
    OutError = TEXT("strate preview rendering is editor-only");
    return false;
}

bool VF_WriteStratePreviewIndex(
    const FString& OutputDirectory,
    const FString& RunTitle,
    const FVoxelStratePreviewWindow& Window,
    const TArray<FVoxelStratePreviewCandidate>& Candidates,
    FString& OutIndexPath,
    FString& OutError,
    const TArray<FVoxelStratePreviewArchetypeSummary>* ArchetypeSummaries)
{
    (void)OutputDirectory;
    (void)RunTitle;
    (void)Window;
    (void)Candidates;
    (void)ArchetypeSummaries;
    OutIndexPath.Reset();
    OutError = TEXT("strate preview rendering is editor-only");
    return false;
}

#endif
