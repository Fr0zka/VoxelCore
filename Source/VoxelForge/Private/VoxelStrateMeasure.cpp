// Read-only, deterministic strate measurement and connectivity checks.

#include "VoxelStrateMeasure.h"

#if WITH_EDITOR

#include "VoxelGenerator.h"
#include "VoxelStrateManager.h"
#include "VoxelTypes.h"

namespace VoxelStrateMeasurePrivate
{
    struct FSampleGrid
    {
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
        float ExtentX = 0.0f;
        float ExtentY = 0.0f;
        float ExtentZ = 0.0f;

        int32 ResolvedMarginVoxels = 0;
        int32 SampledMinZ = 0;
        int32 SampledMaxZ = 0;

        // 1 = air, 0 = solid. The polarity is deliberately explicit at the sampling site below.
        TArray<uint8> Air;
        // Optional exact scalar samples retained only for an explicit preview capture.
        TArray<float> Density;

        FORCEINLINE int32 Index(int32 X, int32 Y, int32 Z) const
        {
            return static_cast<int32>(
                static_cast<int64>(X)
                + static_cast<int64>(NumX) * (static_cast<int64>(Y)
                    + static_cast<int64>(NumY) * static_cast<int64>(Z)));
        }

        FVector CellCenter(int32 X, int32 Y, int32 Z) const
        {
            const float Step = static_cast<float>(SampleStep);
            const float XOffset = FMath::Min(
                static_cast<float>(X) * Step + 0.5f * Step, ExtentX - 0.5f);
            const float YOffset = FMath::Min(
                static_cast<float>(Y) * Step + 0.5f * Step, ExtentY - 0.5f);
            const float ZOffset = FMath::Min(
                static_cast<float>(Z) * Step + 0.5f * Step, ExtentZ - 0.5f);
            return FVector(MinX + XOffset, MinY + YOffset, MinZ + ZOffset);
        }

        bool ContainsPoint(const FVector& Point) const
        {
            // The upper bounds are exclusive: a point on the box edge belongs to the next box,
            // not to the last coarse cell in this measurement.
            return Point.X >= MinX && Point.X < MaxX
                && Point.Y >= MinY && Point.Y < MaxY
                && Point.Z >= MinZ && Point.Z < MaxZ;
        }
    };

    int64 CeilDivPositive(int64 Numerator, int64 Denominator)
    {
        return (Numerator + Denominator - 1) / Denominator;
    }

    bool Refuse(FString& OutReason, const TCHAR* Reason)
    {
        OutReason = Reason;
        return false;
    }

    bool BuildSampleGrid(
        const UVoxelGenerator* Generator,
        const UVoxelStrateManager* Manager,
        const IVoxelStrateDensitySampler* Sampler,
        int32 StrateIndex,
        int32 ExplicitBottomWorldZ,
        int32 ExplicitTopWorldZ,
        float ExplicitBoundarySealThickness,
        bool bUseExplicitBounds,
        const FVoxelStrateMeasureSettings& Settings,
        bool bCaptureDensity,
        FSampleGrid& OutGrid,
        FString& OutReason)
    {
        if (bUseExplicitBounds)
        {
            if (Sampler == nullptr)
            {
                return Refuse(OutReason, TEXT("An explicit measurement window requires a density sampler."));
            }
        }
        else if (Generator == nullptr || Manager == nullptr)
        {
            return Refuse(OutReason, TEXT("A generator and manager are required for a layout measurement."));
        }
        if (Settings.SampleStep <= 0)
        {
            return Refuse(OutReason, TEXT("SampleStep must be greater than zero."));
        }
        const bool bHasCoverPointA = Settings.CoverPointA.IsSet();
        const bool bHasCoverPointB = Settings.CoverPointB.IsSet();
        if (bHasCoverPointA != bHasCoverPointB)
        {
            return Refuse(OutReason, TEXT("CoverPointA and CoverPointB must be set together."));
        }
        const bool bUseFittedWindow = bHasCoverPointA && bHasCoverPointB;
        if (!bUseFittedWindow && Settings.RadiusInVoxels <= 0)
        {
            return Refuse(OutReason, TEXT("RadiusInVoxels must be greater than zero."));
        }
        if (Settings.MaxCells <= 0)
        {
            return Refuse(OutReason, TEXT("MaxCells must be greater than zero."));
        }
        if (Settings.HeadroomCells < 0)
        {
            return Refuse(OutReason, TEXT("HeadroomCells cannot be negative."));
        }
        if (!bUseFittedWindow
            && (!FMath::IsFinite(Settings.CenterXY.X) || !FMath::IsFinite(Settings.CenterXY.Y)))
        {
            return Refuse(OutReason, TEXT("CenterXY must contain finite voxel coordinates."));
        }
        if (bUseFittedWindow)
        {
            const FVector2D& PointA = Settings.CoverPointA.GetValue();
            const FVector2D& PointB = Settings.CoverPointB.GetValue();
            if (!FMath::IsFinite(PointA.X) || !FMath::IsFinite(PointA.Y)
                || !FMath::IsFinite(PointB.X) || !FMath::IsFinite(PointB.Y))
            {
                return Refuse(OutReason, TEXT("Cover points must contain finite voxel coordinates."));
            }
            if (!FMath::IsFinite(Settings.CoverMarginVoxels)
                || Settings.CoverMarginVoxels < 0.0f)
            {
                return Refuse(OutReason, TEXT("CoverMarginVoxels must be finite and non-negative."));
            }
        }
        int64 StrateBottomZ = 0;
        int64 StrateTopZ = 0;
        if (bUseExplicitBounds)
        {
            StrateBottomZ = static_cast<int64>(ExplicitBottomWorldZ);
            StrateTopZ = static_cast<int64>(ExplicitTopWorldZ);
            if (StrateTopZ <= StrateBottomZ)
            {
                return Refuse(OutReason, TEXT("The explicit strate has no positive vertical extent."));
            }
        }
        else
        {
            const TArray<FStrateSlot>& Layout = Manager->GetLayout();
            if (!Layout.IsValidIndex(StrateIndex))
            {
                return Refuse(OutReason, TEXT("StrateIndex is outside the manager layout."));
            }

            const FStrateSlot& Slot = Layout[StrateIndex];
            if (Slot.Definition == nullptr)
            {
                return Refuse(OutReason, TEXT("The requested strate has no definition."));
            }
            if (Slot.TopChunkZ <= Slot.BottomChunkZ)
            {
                return Refuse(OutReason, TEXT("The requested strate has no positive vertical extent."));
            }
            StrateBottomZ = static_cast<int64>(Slot.BottomChunkZ) * CHUNK_SIZE;
            StrateTopZ = (static_cast<int64>(Slot.TopChunkZ) + 1) * CHUNK_SIZE;
        }
        const int64 StrateHeight = StrateTopZ - StrateBottomZ;
        if (StrateHeight <= 0)
        {
            return Refuse(OutReason, TEXT("The requested strate has no positive voxel height."));
        }

        // Resolve the margin from the same blended parameter query used by generation. The
        // representative chunk is deliberately in the middle of this slot and uses the sample
        // window centre's XY chunk, so a future non-Hard transition resolves through the manager
        // exactly as generation does at that representative location.
        FVector2D WindowCenter = Settings.CenterXY;
        double MinX64 = 0.0;
        double MaxX64 = 0.0;
        double MinY64 = 0.0;
        double MaxY64 = 0.0;
        if (bUseFittedWindow)
        {
            const FVector2D& PointA = Settings.CoverPointA.GetValue();
            const FVector2D& PointB = Settings.CoverPointB.GetValue();
            const double Margin = static_cast<double>(Settings.CoverMarginVoxels);
            MinX64 = FMath::Min(static_cast<double>(PointA.X), static_cast<double>(PointB.X)) - Margin;
            MaxX64 = FMath::Max(static_cast<double>(PointA.X), static_cast<double>(PointB.X)) + Margin;
            MinY64 = FMath::Min(static_cast<double>(PointA.Y), static_cast<double>(PointB.Y)) - Margin;
            MaxY64 = FMath::Max(static_cast<double>(PointA.Y), static_cast<double>(PointB.Y)) + Margin;
            WindowCenter = FVector2D(
                static_cast<float>(0.5 * (MinX64 + MaxX64)),
                static_cast<float>(0.5 * (MinY64 + MaxY64)));
        }
        else
        {
            MinX64 = static_cast<double>(Settings.CenterXY.X)
                - static_cast<double>(Settings.RadiusInVoxels);
            MaxX64 = static_cast<double>(Settings.CenterXY.X)
                + static_cast<double>(Settings.RadiusInVoxels);
            MinY64 = static_cast<double>(Settings.CenterXY.Y)
                - static_cast<double>(Settings.RadiusInVoxels);
            MaxY64 = static_cast<double>(Settings.CenterXY.Y)
                + static_cast<double>(Settings.RadiusInVoxels);
        }
        const double XYExtentX = MaxX64 - MinX64;
        const double XYExtentY = MaxY64 - MinY64;
        if (!FMath::IsFinite(MinX64) || !FMath::IsFinite(MaxX64)
            || !FMath::IsFinite(MinY64) || !FMath::IsFinite(MaxY64)
            || !FMath::IsFinite(XYExtentX) || !FMath::IsFinite(XYExtentY)
            || XYExtentX < 1.0 || XYExtentY < 1.0)
        {
            return Refuse(OutReason, TEXT("The requested XY window is not a finite positive voxel box."));
        }

        FStrateGenerationParams RepresentativeParams;
        if (bUseExplicitBounds)
        {
            RepresentativeParams.BoundarySealThickness = ExplicitBoundarySealThickness;
        }
        else
        {
            const TArray<FStrateSlot>& Layout = Manager->GetLayout();
            const FStrateSlot& Slot = Layout[StrateIndex];
            const int32 MidChunkZ = Slot.BottomChunkZ
                + (Slot.TopChunkZ - Slot.BottomChunkZ) / 2;
            const FIntVector RepresentativeChunk(
                FMath::FloorToInt(WindowCenter.X / static_cast<float>(CHUNK_SIZE)),
                FMath::FloorToInt(WindowCenter.Y / static_cast<float>(CHUNK_SIZE)),
                MidChunkZ);
            RepresentativeParams = Manager->GetGenerationParams(RepresentativeChunk);
        }

        const int64 MaxMargin64 = FMath::Max<int64>(1, StrateHeight / 4);
        const int32 MaxMargin = static_cast<int32>(
            FMath::Min<int64>(MaxMargin64, static_cast<int64>(INT32_MAX)));
        int32 RequestedMargin = Settings.InteriorMarginVoxels;
        if (RequestedMargin < 0)
        {
            const float DoubleSealThickness = 2.0f * RepresentativeParams.BoundarySealThickness;
            if (FMath::IsNaN(RepresentativeParams.BoundarySealThickness))
            {
                return Refuse(OutReason, TEXT("BoundarySealThickness is NaN; the interior margin cannot be derived."));
            }

            // Avoid converting an infinite value to int. A positive overflow is still an
            // intentionally absurd seal request and therefore resolves to the safe maximum;
            // negative infinity resolves to the minimum.
            if (!FMath::IsFinite(DoubleSealThickness))
            {
                RequestedMargin = DoubleSealThickness > 0.0f ? MaxMargin : 1;
            }
            else if (DoubleSealThickness >= static_cast<float>(INT32_MAX))
            {
                RequestedMargin = MaxMargin;
            }
            else if (DoubleSealThickness <= static_cast<float>(INT32_MIN))
            {
                RequestedMargin = 0;
            }
            else
            {
                RequestedMargin = FMath::CeilToInt(DoubleSealThickness);
            }
        }
        const int32 ResolvedMargin = FMath::Clamp(RequestedMargin, 0, MaxMargin);

        const int64 InteriorMinZ = StrateBottomZ + static_cast<int64>(ResolvedMargin);
        const int64 InteriorMaxZ = StrateTopZ - static_cast<int64>(ResolvedMargin);
        const int64 InteriorHeight = InteriorMaxZ - InteriorMinZ;
        if (InteriorHeight <= 0)
        {
            return Refuse(OutReason, TEXT("The strate has no measurable height after applying its interior margin."));
        }
        if (InteriorMinZ < static_cast<int64>(INT32_MIN)
            || InteriorMinZ > static_cast<int64>(INT32_MAX)
            || InteriorMaxZ < static_cast<int64>(INT32_MIN)
            || InteriorMaxZ > static_cast<int64>(INT32_MAX))
        {
            return Refuse(OutReason, TEXT("The sampled Z window is outside the metrics struct's int32 range."));
        }

        const int64 Step = static_cast<int64>(Settings.SampleStep);
        const double NumXReal = XYExtentX / static_cast<double>(Step);
        const double NumYReal = XYExtentY / static_cast<double>(Step);
        if (!FMath::IsFinite(NumXReal) || !FMath::IsFinite(NumYReal)
            || NumXReal <= 0.0 || NumYReal <= 0.0
            || NumXReal > static_cast<double>(INT32_MAX)
            || NumYReal > static_cast<double>(INT32_MAX))
        {
            return Refuse(OutReason, TEXT("A sample-grid dimension exceeds the supported array size."));
        }
        const int64 NumX64 = FMath::CeilToInt64(NumXReal);
        const int64 NumY64 = FMath::CeilToInt64(NumYReal);
        const int64 NumZ64 = CeilDivPositive(InteriorHeight, Step);
        const int64 MaxCells = static_cast<int64>(Settings.MaxCells);

        if (NumX64 > INT32_MAX || NumY64 > INT32_MAX || NumZ64 > INT32_MAX)
        {
            return Refuse(OutReason, TEXT("A sample-grid dimension exceeds the supported array size."));
        }
        if (NumX64 > MaxCells || NumY64 > MaxCells || NumZ64 > MaxCells
            || NumX64 > MaxCells / NumY64)
        {
            return Refuse(OutReason, TEXT("The requested sample grid exceeds MaxCells; measurement refused."));
        }

        const int64 XYCells = NumX64 * NumY64;
        if (XYCells > MaxCells / NumZ64)
        {
            return Refuse(OutReason, TEXT("The requested sample grid exceeds MaxCells; measurement refused."));
        }
        const int64 CellCount64 = XYCells * NumZ64;
        if (CellCount64 <= 0 || CellCount64 > MaxCells || CellCount64 > INT32_MAX)
        {
            return Refuse(OutReason, TEXT("The requested sample grid exceeds the bounded cell count."));
        }

        const float MinX = static_cast<float>(MinX64);
        const float MaxX = static_cast<float>(MaxX64);
        const float MinY = static_cast<float>(MinY64);
        const float MaxY = static_cast<float>(MaxY64);
        const float MinZ = static_cast<float>(InteriorMinZ);
        const float MaxZ = static_cast<float>(InteriorMaxZ);
        if (!FMath::IsFinite(MinX) || !FMath::IsFinite(MaxX)
            || !FMath::IsFinite(MinY) || !FMath::IsFinite(MaxY)
            || !FMath::IsFinite(MinZ) || !FMath::IsFinite(MaxZ)
            || MaxX <= MinX || MaxY <= MinY || MaxZ <= MinZ)
        {
            return Refuse(OutReason, TEXT("The requested measurement bounds are not representable as finite voxel coordinates."));
        }

        OutGrid.NumX = static_cast<int32>(NumX64);
        OutGrid.NumY = static_cast<int32>(NumY64);
        OutGrid.NumZ = static_cast<int32>(NumZ64);
        OutGrid.SampleStep = Settings.SampleStep;
        OutGrid.CellCount = static_cast<int32>(CellCount64);
        OutGrid.MinX = MinX;
        OutGrid.MinY = MinY;
        OutGrid.MinZ = MinZ;
        OutGrid.MaxX = MaxX;
        OutGrid.MaxY = MaxY;
        OutGrid.MaxZ = MaxZ;
        OutGrid.ExtentX = MaxX - MinX;
        OutGrid.ExtentY = MaxY - MinY;
        OutGrid.ExtentZ = MaxZ - MinZ;
        OutGrid.ResolvedMarginVoxels = ResolvedMargin;
        OutGrid.SampledMinZ = static_cast<int32>(InteriorMinZ);
        OutGrid.SampledMaxZ = static_cast<int32>(InteriorMaxZ);
        OutGrid.Air.SetNumUninitialized(OutGrid.CellCount);
        if (bCaptureDensity)
        {
            OutGrid.Density.SetNumUninitialized(OutGrid.CellCount);
        }

        bool bSawNonFiniteDensity = false;
        int32 CellIndex = 0;
        for (int32 Z = 0; Z < OutGrid.NumZ; ++Z)
        {
            for (int32 Y = 0; Y < OutGrid.NumY; ++Y)
            {
                for (int32 X = 0; X < OutGrid.NumX; ++X)
                {
                    const FVector Sample = OutGrid.CellCenter(X, Y, Z);
                    const float Density = Sampler != nullptr
                        ? Sampler->SampleDensity(Sample.X, Sample.Y, Sample.Z)
                        : Generator->GetDensityAt(Sample.X, Sample.Y, Sample.Z);
                    if (!FMath::IsFinite(Density))
                    {
                        bSawNonFiniteDensity = true;
                    }

                    // MC convention in this codebase: negative is solid, positive is air.
                    if (bCaptureDensity)
                    {
                        OutGrid.Density[CellIndex] = Density;
                    }
                    OutGrid.Air[CellIndex++] = Density > 0.0f ? 1u : 0u;
                }
            }
        }

        if (bSawNonFiniteDensity)
        {
            return Refuse(OutReason, TEXT("At least one density sample was non-finite."));
        }
        return true;
    }

    void ExportSampleGrid(FSampleGrid& Source, FVoxelStrateSampleGrid& Destination)
    {
        Destination = FVoxelStrateSampleGrid();
        Destination.bValid = true;
        Destination.NumX = Source.NumX;
        Destination.NumY = Source.NumY;
        Destination.NumZ = Source.NumZ;
        Destination.SampleStep = Source.SampleStep;
        Destination.CellCount = Source.CellCount;
        Destination.MinX = Source.MinX;
        Destination.MinY = Source.MinY;
        Destination.MinZ = Source.MinZ;
        Destination.MaxX = Source.MaxX;
        Destination.MaxY = Source.MaxY;
        Destination.MaxZ = Source.MaxZ;
        Destination.ResolvedMarginVoxels = Source.ResolvedMarginVoxels;
        Destination.SampledMinZ = Source.SampledMinZ;
        Destination.SampledMaxZ = Source.SampledMaxZ;
        // Move, rather than copy, the already-built polarity grid. The caller owns this buffer
        // only when it explicitly requested a capture.
        Destination.Air = MoveTemp(Source.Air);
        Destination.Density = MoveTemp(Source.Density);
    }

    void DecodeIndex(const FSampleGrid& Grid, int32 Index, int32& OutX, int32& OutY, int32& OutZ)
    {
        const int32 Plane = Grid.NumX * Grid.NumY;
        OutZ = Index / Plane;
        const int32 InPlane = Index - OutZ * Plane;
        OutY = InPlane / Grid.NumX;
        OutX = InPlane - OutY * Grid.NumX;
    }

    void FloodFillAir(
        const FSampleGrid& Grid,
        TArray<int32>& OutComponents,
        int32& OutNumComponents,
        int64 NumAir,
        int64& OutLargestComponentCells,
        int32& OutLargestComponentLowestCell,
        int32& OutNumComponentsAtLeast1Pct,
        TArray<int64>* OutComponentCells)
    {
        OutComponents.SetNumUninitialized(Grid.CellCount);
        for (int32 Index = 0; Index < Grid.CellCount; ++Index)
        {
            OutComponents[Index] = -1;
        }

        TArray<int32> Queue;
        Queue.SetNumUninitialized(Grid.CellCount);
        OutNumComponents = 0;
        OutLargestComponentCells = 0;
        OutLargestComponentLowestCell = INDEX_NONE;
        OutNumComponentsAtLeast1Pct = 0;
        if (OutComponentCells != nullptr)
        {
            OutComponentCells->Reset();
        }

        for (int32 Start = 0; Start < Grid.CellCount; ++Start)
        {
            if (Grid.Air[Start] == 0u || OutComponents[Start] >= 0)
            {
                continue;
            }

            const int32 ComponentId = OutNumComponents++;
            int32 Head = 0;
            int32 Tail = 0;
            int32 ComponentCells = 0;
            OutComponents[Start] = ComponentId;
            Queue[Tail++] = Start;

            while (Head < Tail)
            {
                const int32 Current = Queue[Head++];
                ++ComponentCells;

                int32 X = 0;
                int32 Y = 0;
                int32 Z = 0;
                DecodeIndex(Grid, Current, X, Y, Z);

                auto Visit = [&](int32 Neighbor)
                {
                    if (Grid.Air[Neighbor] != 0u && OutComponents[Neighbor] < 0)
                    {
                        OutComponents[Neighbor] = ComponentId;
                        Queue[Tail++] = Neighbor;
                    }
                };

                // Fixed order keeps every derived value independent of container iteration order.
                if (X + 1 < Grid.NumX) Visit(Grid.Index(X + 1, Y, Z));
                if (X > 0)            Visit(Grid.Index(X - 1, Y, Z));
                if (Y + 1 < Grid.NumY) Visit(Grid.Index(X, Y + 1, Z));
                if (Y > 0)             Visit(Grid.Index(X, Y - 1, Z));
                if (Z + 1 < Grid.NumZ) Visit(Grid.Index(X, Y, Z + 1));
                if (Z > 0)             Visit(Grid.Index(X, Y, Z - 1));
            }

            const int64 ComponentCells64 = static_cast<int64>(ComponentCells);
            if (OutComponentCells != nullptr)
            {
                OutComponentCells->Add(ComponentCells64);
            }
            if (NumAir > 0 && ComponentCells64 * 100 >= NumAir)
            {
                ++OutNumComponentsAtLeast1Pct;
            }

            if (ComponentCells64 > OutLargestComponentCells
                || (ComponentCells64 == OutLargestComponentCells
                    && (OutLargestComponentLowestCell == INDEX_NONE
                        || Start < OutLargestComponentLowestCell)))
            {
                OutLargestComponentCells = ComponentCells64;
                OutLargestComponentLowestCell = Start;
            }
        }
    }

    void BuildManhattanDistanceToSolid(const FSampleGrid& Grid, TArray<int32>& OutDistances)
    {
        // Two raster passes with 6-neighbour Manhattan relaxation. The result is an approximation
        // of distance-to-solid in the original field: each coarse grid unit is SampleStep VOXELS.
        constexpr int32 Infinity = INT32_MAX;
        OutDistances.SetNumUninitialized(Grid.CellCount);
        for (int32 Index = 0; Index < Grid.CellCount; ++Index)
        {
            OutDistances[Index] = Grid.Air[Index] == 0u ? 0 : Infinity;
        }

        auto Relax = [&](int32 Current, int32 Neighbor)
        {
            if (OutDistances[Neighbor] == Infinity)
            {
                return;
            }
            const int32 Candidate = OutDistances[Neighbor] == INT32_MAX - 1
                ? Infinity : OutDistances[Neighbor] + 1;
            OutDistances[Current] = FMath::Min(OutDistances[Current], Candidate);
        };

        for (int32 Z = 0; Z < Grid.NumZ; ++Z)
        {
            for (int32 Y = 0; Y < Grid.NumY; ++Y)
            {
                for (int32 X = 0; X < Grid.NumX; ++X)
                {
                    const int32 Current = Grid.Index(X, Y, Z);
                    if (Grid.Air[Current] == 0u) continue;
                    if (X > 0) Relax(Current, Grid.Index(X - 1, Y, Z));
                    if (Y > 0) Relax(Current, Grid.Index(X, Y - 1, Z));
                    if (Z > 0) Relax(Current, Grid.Index(X, Y, Z - 1));
                }
            }
        }

        for (int32 Z = Grid.NumZ - 1; Z >= 0; --Z)
        {
            for (int32 Y = Grid.NumY - 1; Y >= 0; --Y)
            {
                for (int32 X = Grid.NumX - 1; X >= 0; --X)
                {
                    const int32 Current = Grid.Index(X, Y, Z);
                    if (Grid.Air[Current] == 0u) continue;
                    if (X + 1 < Grid.NumX) Relax(Current, Grid.Index(X + 1, Y, Z));
                    if (Y + 1 < Grid.NumY) Relax(Current, Grid.Index(X, Y + 1, Z));
                    if (Z + 1 < Grid.NumZ) Relax(Current, Grid.Index(X, Y, Z + 1));
                }
            }
        }
    }

    float MedianFloat(TArray<float>& Values)
    {
        if (Values.Num() == 0) return 0.0f;
        Values.Sort();
        const int32 Middle = Values.Num() / 2;
        if ((Values.Num() & 1) != 0)
        {
            return Values[Middle];
        }
        return (Values[Middle - 1] + Values[Middle]) * 0.5f;
    }

    void BuildMetricsFromGrid(
        const FSampleGrid& Grid,
        const FVoxelStrateMeasureSettings& Settings,
        FVoxelStrateMetrics& InOutMetrics)
    {
        TArray<int32> Components;
        int32 NumComponents = 0;

        int64 NumAir = 0;
        for (const uint8 bAir : Grid.Air)
        {
            NumAir += bAir != 0u ? 1 : 0;
        }

        int64 LargestComponentCells = 0;
        int32 LargestComponentLowestCell = INDEX_NONE;
        int32 NumComponentsAtLeast1Pct = 0;
        TArray<int64> ComponentCells;
        FloodFillAir(
            Grid,
            Components,
            NumComponents,
            NumAir,
            LargestComponentCells,
            LargestComponentLowestCell,
            NumComponentsAtLeast1Pct,
            &ComponentCells);

        InOutMetrics.NumSampled = Grid.CellCount;
        InOutMetrics.NumAir = NumAir;
        InOutMetrics.NumSolid = static_cast<int64>(Grid.CellCount) - NumAir;
        InOutMetrics.AirFraction = static_cast<float>(
            static_cast<double>(NumAir) / static_cast<double>(Grid.CellCount));
        InOutMetrics.NumAirComponents = NumComponents;
        InOutMetrics.AirComponentCells = MoveTemp(ComponentCells);
        InOutMetrics.LargestComponentCells = LargestComponentCells;
        InOutMetrics.NumComponentsAtLeast1Pct = NumComponentsAtLeast1Pct;
        InOutMetrics.LargestComponentShare = NumAir > 0
            ? static_cast<float>(static_cast<double>(LargestComponentCells)
                / static_cast<double>(NumAir))
            : 0.0f;
        if (NumAir > 0 && LargestComponentLowestCell != INDEX_NONE)
        {
            int32 LargestX = 0;
            int32 LargestY = 0;
            int32 LargestZ = 0;
            DecodeIndex(Grid, LargestComponentLowestCell, LargestX, LargestY, LargestZ);
            InOutMetrics.LargestComponentPoint = Grid.CellCenter(LargestX, LargestY, LargestZ);
        }

        TArray<int32> ClearanceValues;
        ClearanceValues.Reserve(static_cast<int32>(NumAir));
        int64 NumWalkable = 0;
        const int64 XYColumnCount64 = static_cast<int64>(Grid.NumX)
            * static_cast<int64>(Grid.NumY);
        TArray<uint8> WalkableColumns;
        WalkableColumns.Init(0u, static_cast<int32>(XYColumnCount64));

        for (int32 Z = 0; Z < Grid.NumZ; ++Z)
        {
            for (int32 Y = 0; Y < Grid.NumY; ++Y)
            {
                for (int32 X = 0; X < Grid.NumX; ++X)
                {
                    const int32 Current = Grid.Index(X, Y, Z);
                    if (Grid.Air[Current] == 0u || Z == 0
                        || Grid.Air[Grid.Index(X, Y, Z - 1)] != 0u)
                    {
                        continue;
                    }

                    bool bHasHeadroom = true;
                    for (int32 H = 0; H < Settings.HeadroomCells; ++H)
                    {
                        const int64 HeadroomZ = static_cast<int64>(Z) + H;
                        if (HeadroomZ >= Grid.NumZ
                            || Grid.Air[Grid.Index(X, Y, static_cast<int32>(HeadroomZ))] == 0u)
                        {
                            bHasHeadroom = false;
                            break;
                        }
                    }
                    if (!bHasHeadroom) continue;

                    ++NumWalkable;
                    WalkableColumns[X + Grid.NumX * Y] = 1u;
                    int32 ClearanceCells = 0;
                    while (static_cast<int64>(Z) + ClearanceCells < Grid.NumZ
                        && Grid.Air[Grid.Index(X, Y, Z + ClearanceCells)] != 0u)
                    {
                        ++ClearanceCells;
                    }
                    const int64 ClearanceVoxels = static_cast<int64>(ClearanceCells)
                        * Settings.SampleStep;
                    ClearanceValues.Add(ClearanceVoxels > INT32_MAX
                        ? INT32_MAX : static_cast<int32>(ClearanceVoxels));
                }
            }
        }

        InOutMetrics.WalkableFraction = NumAir > 0
            ? static_cast<float>(static_cast<double>(NumWalkable)
                / static_cast<double>(NumAir))
            : 0.0f;
        for (const uint8 bWalkable : WalkableColumns)
        {
            InOutMetrics.WalkableFloorColumns += bWalkable != 0u ? 1 : 0;
        }
        InOutMetrics.WalkableFloorAreaFraction = XYColumnCount64 > 0
            ? static_cast<float>(static_cast<double>(InOutMetrics.WalkableFloorColumns)
                / static_cast<double>(XYColumnCount64))
            : 0.0f;

        // The area graph is intentionally projected to XY: each column contributes one floor
        // footprint even when several sampled Z cells in it meet the walkability predicate.
        // Keep the flood fill deterministic and bounded by the already bounded XY grid.
        TArray<uint8> VisitedWalkableColumns;
        VisitedWalkableColumns.Init(0u, static_cast<int32>(XYColumnCount64));
        TArray<int32> SurfaceQueue;
        SurfaceQueue.Reserve(static_cast<int32>(XYColumnCount64));
        for (int32 Column = 0; Column < WalkableColumns.Num(); ++Column)
        {
            if (WalkableColumns[Column] == 0u || VisitedWalkableColumns[Column] != 0u)
            {
                continue;
            }

            ++InOutMetrics.NumWalkableSurfaceComponents;
            VisitedWalkableColumns[Column] = 1u;
            SurfaceQueue.Reset();
            SurfaceQueue.Add(Column);
            int64 ComponentColumns = 0;
            for (int32 QueueIndex = 0; QueueIndex < SurfaceQueue.Num(); ++QueueIndex)
            {
                const int32 CurrentColumn = SurfaceQueue[QueueIndex];
                ++ComponentColumns;
                const int32 CurrentX = CurrentColumn % Grid.NumX;
                const int32 CurrentY = CurrentColumn / Grid.NumX;
                const int32 NeighbourColumns[4] = {
                    CurrentX > 0 ? CurrentColumn - 1 : INDEX_NONE,
                    CurrentX + 1 < Grid.NumX ? CurrentColumn + 1 : INDEX_NONE,
                    CurrentY > 0 ? CurrentColumn - Grid.NumX : INDEX_NONE,
                    CurrentY + 1 < Grid.NumY ? CurrentColumn + Grid.NumX : INDEX_NONE
                };
                for (const int32 Neighbour : NeighbourColumns)
                {
                    if (Neighbour != INDEX_NONE
                        && WalkableColumns[Neighbour] != 0u
                        && VisitedWalkableColumns[Neighbour] == 0u)
                    {
                        VisitedWalkableColumns[Neighbour] = 1u;
                        SurfaceQueue.Add(Neighbour);
                    }
                }
            }
            InOutMetrics.LargestWalkableSurfaceColumns = FMath::Max(
                InOutMetrics.LargestWalkableSurfaceColumns, ComponentColumns);
        }
        InOutMetrics.LargestWalkableSurfaceShare = InOutMetrics.WalkableFloorColumns > 0
            ? static_cast<float>(static_cast<double>(InOutMetrics.LargestWalkableSurfaceColumns)
                / static_cast<double>(InOutMetrics.WalkableFloorColumns))
            : 0.0f;

        if (ClearanceValues.Num() > 0)
        {
            ClearanceValues.Sort();
            // The public metric is integral, so use the lower median for an even sample count.
            InOutMetrics.MedianVerticalClearance = ClearanceValues[(ClearanceValues.Num() - 1) / 2];
        }

        if (InOutMetrics.NumSolid > 0 && NumAir > 0)
        {
            TArray<int32> Distances;
            BuildManhattanDistanceToSolid(Grid, Distances);

            TArray<float> FeatureScales;
            FeatureScales.Reserve(static_cast<int32>(NumAir));
            for (int32 Index = 0; Index < Grid.CellCount; ++Index)
            {
                if (Grid.Air[Index] != 0u)
                {
                    FeatureScales.Add(static_cast<float>(Distances[Index])
                        * static_cast<float>(Settings.SampleStep));
                }
            }
            InOutMetrics.MedianFeatureScale = MedianFloat(FeatureScales);
        }
    }

    bool FindCellForPoint(const FSampleGrid& Grid, const FVector& Point, int32& OutCell)
    {
        if (!Grid.ContainsPoint(Point)) return false;

        const float Step = static_cast<float>(Grid.SampleStep);
        const int32 X = FMath::Clamp(FMath::FloorToInt((Point.X - Grid.MinX) / Step), 0, Grid.NumX - 1);
        const int32 Y = FMath::Clamp(FMath::FloorToInt((Point.Y - Grid.MinY) / Step), 0, Grid.NumY - 1);
        const int32 Z = FMath::Clamp(FMath::FloorToInt((Point.Z - Grid.MinZ) / Step), 0, Grid.NumZ - 1);
        OutCell = Grid.Index(X, Y, Z);
        return true;
    }

    enum class EEndpointCellResult : uint8
    {
        OutOfWindow,
        Air,
        Solid
    };

    EEndpointCellResult ResolveEndpointCell(
        const FSampleGrid& Grid,
        const FVector& Point,
        int32& OutCell,
        bool& bOutSnapped)
    {
        bOutSnapped = false;
        int32 OriginalCell = -1;
        if (!FindCellForPoint(Grid, Point, OriginalCell))
        {
            return EEndpointCellResult::OutOfWindow;
        }
        if (Grid.Air[OriginalCell] != 0u)
        {
            OutCell = OriginalCell;
            return EEndpointCellResult::Air;
        }

        int32 OriginalX = 0;
        int32 OriginalY = 0;
        int32 OriginalZ = 0;
        DecodeIndex(Grid, OriginalCell, OriginalX, OriginalY, OriginalZ);

        int32 BestCell = INDEX_NONE;
        float BestDistanceSquared = FLT_MAX;
        const int32 MaxX = Grid.NumX - 1;
        const int32 MaxY = Grid.NumY - 1;
        const int32 MaxZ = Grid.NumZ - 1;
        const int32 MinX = OriginalX > 0 ? OriginalX - 1 : 0;
        const int32 MinY = OriginalY > 0 ? OriginalY - 1 : 0;
        const int32 MinZ = OriginalZ > 0 ? OriginalZ - 1 : 0;
        const int32 CandidateMaxX = OriginalX < MaxX ? OriginalX + 1 : MaxX;
        const int32 CandidateMaxY = OriginalY < MaxY ? OriginalY + 1 : MaxY;
        const int32 CandidateMaxZ = OriginalZ < MaxZ ? OriginalZ + 1 : MaxZ;
        for (int32 Z = MinZ;
             Z <= CandidateMaxZ;
             ++Z)
        {
            for (int32 Y = MinY;
                 Y <= CandidateMaxY;
                 ++Y)
            {
                for (int32 X = MinX;
                     X <= CandidateMaxX;
                     ++X)
                {
                    if (X == OriginalX && Y == OriginalY && Z == OriginalZ)
                    {
                        continue;
                    }

                    const int32 Candidate = Grid.Index(X, Y, Z);
                    if (Grid.Air[Candidate] == 0u)
                    {
                        continue;
                    }

                    const float DistanceSquared = FVector::DistSquared(
                        Point, Grid.CellCenter(X, Y, Z));
                    if (DistanceSquared < BestDistanceSquared
                        || (DistanceSquared == BestDistanceSquared
                            && (BestCell == INDEX_NONE || Candidate < BestCell)))
                    {
                        BestDistanceSquared = DistanceSquared;
                        BestCell = Candidate;
                    }
                }
            }
        }

        if (BestCell == INDEX_NONE)
        {
            return EEndpointCellResult::Solid;
        }

        OutCell = BestCell;
        bOutSnapped = true;
        return EEndpointCellResult::Air;
    }

    struct FCoarseEdge
    {
        int32 First = INDEX_NONE;
        int32 Second = INDEX_NONE;

        bool IsValid() const
        {
            return First >= 0 && Second >= 0 && First != Second;
        }
    };

    FCoarseEdge MakeCoarseEdge(int32 A, int32 B)
    {
        FCoarseEdge Edge;
        Edge.First = FMath::Min(A, B);
        Edge.Second = FMath::Max(A, B);
        return Edge;
    }

    bool IsCoarseEdgeBlocked(
        const TArray<FCoarseEdge>& BlockedEdges,
        int32 A,
        int32 B)
    {
        const FCoarseEdge Candidate = MakeCoarseEdge(A, B);
        for (const FCoarseEdge& Blocked : BlockedEdges)
        {
            if (Blocked.First == Candidate.First && Blocked.Second == Candidate.Second)
            {
                return true;
            }
        }
        return false;
    }

    bool FullResolutionAirOnSegment(
        const UVoxelGenerator* Generator,
        const IVoxelStrateDensitySampler* Sampler,
        const FVector& Start,
        const FVector& End)
    {
        const FVector Delta = End - Start;
        const float MaxDelta = FMath::Max3(
            FMath::Abs(Delta.X), FMath::Abs(Delta.Y), FMath::Abs(Delta.Z));
        const int32 NumSteps = FMath::Max(1, FMath::CeilToInt(MaxDelta));
        for (int32 Step = 0; Step <= NumSteps; ++Step)
        {
            const float Alpha = static_cast<float>(Step) / static_cast<float>(NumSteps);
            const FVector Sample = Start + Delta * Alpha;
            const float Density = Sampler != nullptr
                ? Sampler->SampleDensity(Sample.X, Sample.Y, Sample.Z)
                : Generator->GetDensityAt(Sample.X, Sample.Y, Sample.Z);
            if (!FMath::IsFinite(Density) || !(Density > 0.0f))
            {
                return false;
            }
        }
        return true;
    }

    bool FullResolutionPathIsAir(
        const UVoxelGenerator* Generator,
        const IVoxelStrateDensitySampler* Sampler,
        const FSampleGrid& Grid,
        const FVector& A,
        const FVector& B,
        const TArray<int32>& Path,
        FCoarseEdge& OutFailedEdge)
    {
        OutFailedEdge = FCoarseEdge();

        int32 StartX = 0;
        int32 StartY = 0;
        int32 StartZ = 0;
        DecodeIndex(Grid, Path[0], StartX, StartY, StartZ);
        int32 EndX = 0;
        int32 EndY = 0;
        int32 EndZ = 0;
        DecodeIndex(Grid, Path.Last(), EndX, EndY, EndZ);

        if (!FullResolutionAirOnSegment(Generator, Sampler, A, Grid.CellCenter(StartX, StartY, StartZ)))
        {
            // The endpoint-to-cell-center segment has no coarse cell pair to exclude. It is
            // common to every route from this endpoint, so report unknown rather than claiming
            // that the pair is disconnected at this resolution.
            return false;
        }

        for (int32 PathIndex = 1; PathIndex < Path.Num(); ++PathIndex)
        {
            int32 PreviousX = 0;
            int32 PreviousY = 0;
            int32 PreviousZ = 0;
            DecodeIndex(Grid, Path[PathIndex - 1], PreviousX, PreviousY, PreviousZ);
            int32 CurrentX = 0;
            int32 CurrentY = 0;
            int32 CurrentZ = 0;
            DecodeIndex(Grid, Path[PathIndex], CurrentX, CurrentY, CurrentZ);
            if (!FullResolutionAirOnSegment(
                    Generator, Sampler,
                    Grid.CellCenter(PreviousX, PreviousY, PreviousZ),
                    Grid.CellCenter(CurrentX, CurrentY, CurrentZ)))
            {
                OutFailedEdge = MakeCoarseEdge(Path[PathIndex - 1], Path[PathIndex]);
                return false;
            }
        }

        if (!FullResolutionAirOnSegment(Generator, Sampler, Grid.CellCenter(EndX, EndY, EndZ), B))
        {
            // As above, the final endpoint segment is shared by every route that reaches the
            // goal cell and therefore has no precise coarse edge to exclude.
            return false;
        }
        return true;
    }

    bool FindCoarsePath(
        const FSampleGrid& Grid,
        int32 Start,
        int32 Goal,
        const TArray<FCoarseEdge>& BlockedEdges,
        TArray<int32>& OutPath)
    {
        if (Grid.Air[Start] == 0u || Grid.Air[Goal] == 0u)
        {
            return false;
        }

        TArray<int32> Parent;
        Parent.SetNumUninitialized(Grid.CellCount);
        for (int32 Index = 0; Index < Grid.CellCount; ++Index)
        {
            Parent[Index] = -2; // unvisited; -1 is the root's parent
        }

        TArray<int32> Queue;
        Queue.SetNumUninitialized(Grid.CellCount);
        int32 Head = 0;
        int32 Tail = 0;
        Parent[Start] = -1;
        Queue[Tail++] = Start;

        while (Head < Tail && Parent[Goal] == -2)
        {
            const int32 Current = Queue[Head++];
            int32 X = 0;
            int32 Y = 0;
            int32 Z = 0;
            DecodeIndex(Grid, Current, X, Y, Z);

            auto Visit = [&](int32 Neighbor)
            {
                if (Grid.Air[Neighbor] != 0u
                    && Parent[Neighbor] == -2
                    && !IsCoarseEdgeBlocked(BlockedEdges, Current, Neighbor))
                {
                    Parent[Neighbor] = Current;
                    Queue[Tail++] = Neighbor;
                }
            };

            // The order is fixed so the recovered route is deterministic. This is a real BFS
            // parent chain, not just a component-membership answer.
            if (X + 1 < Grid.NumX) Visit(Grid.Index(X + 1, Y, Z));
            if (X > 0)            Visit(Grid.Index(X - 1, Y, Z));
            if (Y + 1 < Grid.NumY) Visit(Grid.Index(X, Y + 1, Z));
            if (Y > 0)             Visit(Grid.Index(X, Y - 1, Z));
            if (Z + 1 < Grid.NumZ) Visit(Grid.Index(X, Y, Z + 1));
            if (Z > 0)             Visit(Grid.Index(X, Y, Z - 1));
        }

        if (Parent[Goal] == -2)
        {
            return false;
        }

        OutPath.Reset();
        OutPath.Reserve(Tail);
        for (int32 Current = Goal; Current >= 0; Current = Parent[Current])
        {
            OutPath.Add(Current);
            if (Current == Start) break;
        }
        if (OutPath.Last() != Start)
        {
            OutPath.Reset();
            return false;
        }

        for (int32 Left = 0, Right = OutPath.Num() - 1; Left < Right; ++Left, --Right)
        {
            Swap(OutPath[Left], OutPath[Right]);
        }
        return true;
    }

    EVoxelConnectivityResult EvaluateConnectivityOnGrid(
        const UVoxelGenerator* Generator,
        const IVoxelStrateDensitySampler* Sampler,
        const FSampleGrid& Grid,
        const FVector& AVoxel,
        const FVector& BVoxel,
        int32& OutStart,
        int32& OutGoal,
        bool& bOutStartSnapped,
        bool& bOutGoalSnapped,
        int32& OutNumRouteRetries,
        int32 MaxRouteRetries)
    {
        OutStart = -1;
        OutGoal = -1;
        bOutStartSnapped = false;
        bOutGoalSnapped = false;
        OutNumRouteRetries = 0;

        const EEndpointCellResult StartCell = ResolveEndpointCell(
            Grid, AVoxel, OutStart, bOutStartSnapped);
        const EEndpointCellResult GoalCell = ResolveEndpointCell(
            Grid, BVoxel, OutGoal, bOutGoalSnapped);
        if (StartCell == EEndpointCellResult::OutOfWindow
            || GoalCell == EEndpointCellResult::OutOfWindow)
        {
            return EVoxelConnectivityResult::OutOfWindow;
        }
        if (StartCell == EEndpointCellResult::Solid)
        {
            return EVoxelConnectivityResult::StartCellSolid;
        }
        if (GoalCell == EEndpointCellResult::Solid)
        {
            return EVoxelConnectivityResult::GoalCellSolid;
        }

        TArray<FCoarseEdge> BlockedEdges;
        TArray<int32> Path;
        for (;;)
        {
            if (!FindCoarsePath(Grid, OutStart, OutGoal, BlockedEdges, Path))
            {
                // This is a negative verdict only after all edges excluded by earlier
                // full-resolution failures leave no coarse route at this resolution.
                return EVoxelConnectivityResult::NotConnectedAtThisResolution;
            }

            // The coarse route is evidence, not a pass. Re-check every candidate at one-voxel
            // spacing in the original voxel field. A single solid sample refutes only this
            // route, so exclude its precise coarse edge and search again.
            FCoarseEdge FailedEdge;
            if (FullResolutionPathIsAir(Generator, Sampler, Grid, AVoxel, BVoxel, Path, FailedEdge))
            {
                return EVoxelConnectivityResult::Connected;
            }

            if (OutNumRouteRetries >= MaxRouteRetries || !FailedEdge.IsValid())
            {
                // A route can be refuted without a retryable pair when the failure is in an
                // endpoint-to-cell segment (or when the explicit retry budget is spent). Both
                // cases are unknown, never evidence of disconnection.
                return EVoxelConnectivityResult::CoarseLiedBudgetExhausted;
            }

            check(FailedEdge.First != FailedEdge.Second);
            check(!IsCoarseEdgeBlocked(
                BlockedEdges, FailedEdge.First, FailedEdge.Second));
            BlockedEdges.Add(FailedEdge);
            ++OutNumRouteRetries;
        }
    }

    float DistanceToComponent(
        const FSampleGrid& Grid,
        int32 StartCell,
        int32 TargetComponent,
        const TArray<int32>& Components)
    {
        if (Components[StartCell] == TargetComponent)
        {
            return 0.0f;
        }

        int32 StartX = 0;
        int32 StartY = 0;
        int32 StartZ = 0;
        DecodeIndex(Grid, StartCell, StartX, StartY, StartZ);
        int64 BestDistanceSquared = MAX_int64;
        for (int32 Index = 0; Index < Grid.CellCount; ++Index)
        {
            if (Components[Index] != TargetComponent)
            {
                continue;
            }

            int32 X = 0;
            int32 Y = 0;
            int32 Z = 0;
            DecodeIndex(Grid, Index, X, Y, Z);
            const int64 DX = static_cast<int64>(X) - static_cast<int64>(StartX);
            const int64 DY = static_cast<int64>(Y) - static_cast<int64>(StartY);
            const int64 DZ = static_cast<int64>(Z) - static_cast<int64>(StartZ);
            const int64 DistanceSquared = DX * DX + DY * DY + DZ * DZ;
            BestDistanceSquared = FMath::Min(BestDistanceSquared, DistanceSquared);
        }

        return BestDistanceSquared == MAX_int64
            ? -1.0f
            : FMath::Sqrt(static_cast<float>(BestDistanceSquared));
    }

    void PopulateConnectivityDiagnostics(
        const FSampleGrid& Grid,
        int32 Start,
        int32 Goal,
        EVoxelConnectivityResult ConnectivityResult,
        bool bStartSnapped,
        bool bGoalSnapped,
        int32 NumRouteRetries,
        FVoxelConnectivityDiagnostics& OutDiagnostics)
    {
        OutDiagnostics = FVoxelConnectivityDiagnostics();
        OutDiagnostics.Result = ConnectivityResult;
        OutDiagnostics.bStartSnapped = bStartSnapped;
        OutDiagnostics.bGoalSnapped = bGoalSnapped;
        OutDiagnostics.NumRouteRetries = NumRouteRetries;
        OutDiagnostics.bValid = ConnectivityResult != EVoxelConnectivityResult::OutOfWindow
            || (Start >= 0 && Goal >= 0);
        if (Start < 0 || Goal < 0)
        {
            return;
        }

        TArray<int32> Components;
        int32 NumComponents = 0;
        int64 LargestComponentCells = 0;
        int32 LargestComponentLowestCell = INDEX_NONE;
        int32 NumComponentsAtLeast1Pct = 0;
        int64 NumAir = 0;
        for (const uint8 bAir : Grid.Air)
        {
            NumAir += bAir != 0u ? 1 : 0;
        }
        FloodFillAir(
            Grid,
            Components,
            NumComponents,
            NumAir,
            LargestComponentCells,
            LargestComponentLowestCell,
            NumComponentsAtLeast1Pct,
            nullptr);

        const int32 StartComponent = Components[Start];
        const int32 GoalComponent = Components[Goal];
        if (StartComponent < 0 || GoalComponent < 0)
        {
            return;
        }

        TArray<int32> ComponentCells;
        ComponentCells.Init(0, NumComponents);
        for (const int32 Component : Components)
        {
            if (Component >= 0)
            {
                ++ComponentCells[Component];
            }
        }

        OutDiagnostics.NumAirCells = NumAir;
        OutDiagnostics.StartComponentCells = ComponentCells[StartComponent];
        OutDiagnostics.GoalComponentCells = ComponentCells[GoalComponent];
        OutDiagnostics.LargestComponentCells = LargestComponentCells;
        if (NumAir > 0)
        {
            OutDiagnostics.StartComponentShare = static_cast<float>(
                static_cast<double>(OutDiagnostics.StartComponentCells)
                / static_cast<double>(NumAir));
            OutDiagnostics.GoalComponentShare = static_cast<float>(
                static_cast<double>(OutDiagnostics.GoalComponentCells)
                / static_cast<double>(NumAir));
            OutDiagnostics.LargestComponentShare = static_cast<float>(
                static_cast<double>(LargestComponentCells)
                / static_cast<double>(NumAir));
        }
        OutDiagnostics.bStartComponentIsLargest =
            OutDiagnostics.StartComponentCells == LargestComponentCells;
        OutDiagnostics.bGoalComponentIsLargest =
            OutDiagnostics.GoalComponentCells == LargestComponentCells;
        OutDiagnostics.StartToGoalComponentDistanceCells = DistanceToComponent(
            Grid, Start, GoalComponent, Components);
        OutDiagnostics.GoalToStartComponentDistanceCells = DistanceToComponent(
            Grid, Goal, StartComponent, Components);
    }

    EVoxelConnectivityResult QueryConnectivity(
        const UVoxelGenerator* Generator,
        const UVoxelStrateManager* Manager,
        const IVoxelStrateDensitySampler* Sampler,
        int32 StrateIndex,
        int32 ExplicitBottomWorldZ,
        int32 ExplicitTopWorldZ,
        float ExplicitBoundarySealThickness,
        bool bUseExplicitBounds,
        const FVector& AVoxel,
        const FVector& BVoxel,
        const FVoxelStrateMeasureSettings& Settings,
        bool& bOutStartSnapped,
        bool& bOutGoalSnapped,
        int32& OutNumRouteRetries,
        FVoxelConnectivityDiagnostics* OutDiagnostics)
    {
        bOutStartSnapped = false;
        bOutGoalSnapped = false;
        OutNumRouteRetries = 0;
        if (!FMath::IsFinite(AVoxel.X) || !FMath::IsFinite(AVoxel.Y)
            || !FMath::IsFinite(AVoxel.Z) || !FMath::IsFinite(BVoxel.X)
            || !FMath::IsFinite(BVoxel.Y) || !FMath::IsFinite(BVoxel.Z))
        {
            if (OutDiagnostics != nullptr)
            {
                *OutDiagnostics = FVoxelConnectivityDiagnostics();
            }
            return EVoxelConnectivityResult::OutOfWindow;
        }
        if (Settings.MaxRouteRetries < 0)
        {
            if (OutDiagnostics != nullptr)
            {
                *OutDiagnostics = FVoxelConnectivityDiagnostics();
            }
            return EVoxelConnectivityResult::OutOfWindow;
        }

        FSampleGrid Grid;
        FString RefusalReason;
        if (!BuildSampleGrid(Generator, Manager, Sampler, StrateIndex,
                             ExplicitBottomWorldZ, ExplicitTopWorldZ,
                             ExplicitBoundarySealThickness, bUseExplicitBounds,
                             Settings, false, Grid, RefusalReason))
        {
            if (OutDiagnostics != nullptr)
            {
                *OutDiagnostics = FVoxelConnectivityDiagnostics();
            }
            return EVoxelConnectivityResult::OutOfWindow;
        }

        int32 Start = -1;
        int32 Goal = -1;
        const EVoxelConnectivityResult Result = EvaluateConnectivityOnGrid(
            Generator,
            Sampler,
            Grid,
            AVoxel,
            BVoxel,
            Start,
            Goal,
            bOutStartSnapped,
            bOutGoalSnapped,
            OutNumRouteRetries,
            Settings.MaxRouteRetries);
        if (OutDiagnostics != nullptr)
        {
            PopulateConnectivityDiagnostics(
                Grid,
                Start,
                Goal,
                Result,
                bOutStartSnapped,
                bOutGoalSnapped,
                OutNumRouteRetries,
                *OutDiagnostics);
        }
        return Result;
    }
}

FVoxelStrateMetrics VF_MeasureStrate(
    const UVoxelGenerator& Generator,
    const UVoxelStrateManager& Manager,
    int32 StrateIndex,
    const FVoxelStrateMeasureSettings& Settings,
    FVoxelStrateSampleGrid* OutSampleGrid)
{
    FVoxelStrateMetrics Result;
    if (OutSampleGrid != nullptr)
    {
        *OutSampleGrid = FVoxelStrateSampleGrid();
    }
    VoxelStrateMeasurePrivate::FSampleGrid Grid;
    if (!VoxelStrateMeasurePrivate::BuildSampleGrid(
            &Generator, &Manager, nullptr, StrateIndex, 0, 0, 0.0f, false,
            Settings, OutSampleGrid != nullptr, Grid, Result.RefusalReason))
    {
        return Result;
    }

    Result.ResolvedMarginVoxels = Grid.ResolvedMarginVoxels;
    Result.SampledMinZ = Grid.SampledMinZ;
    Result.SampledMaxZ = Grid.SampledMaxZ;
    Result.SampledNumX = Grid.NumX;
    Result.SampledNumY = Grid.NumY;
    Result.SampledNumZ = Grid.NumZ;
    Result.SampledMinX = Grid.MinX;
    Result.SampledMaxX = Grid.MaxX;
    Result.SampledMinY = Grid.MinY;
    Result.SampledMaxY = Grid.MaxY;
    VoxelStrateMeasurePrivate::BuildMetricsFromGrid(Grid, Settings, Result);
    Result.bValid = Result.NumSampled > 0;
    if (!Result.bValid)
    {
        Result.RefusalReason = TEXT("The measurement grid was empty.");
    }
    if (OutSampleGrid != nullptr && Result.bValid)
    {
        VoxelStrateMeasurePrivate::ExportSampleGrid(Grid, *OutSampleGrid);
    }
    return Result;
}

FVoxelStrateMetrics VF_MeasureStrateWithSampler(
    const IVoxelStrateDensitySampler& Sampler,
    int32 StrateBottomWorldZ,
    int32 StrateTopWorldZ,
    float BoundarySealThickness,
    const FVoxelStrateMeasureSettings& Settings,
    FVoxelStrateSampleGrid* OutSampleGrid)
{
    FVoxelStrateMetrics Result;
    if (OutSampleGrid != nullptr)
    {
        *OutSampleGrid = FVoxelStrateSampleGrid();
    }
    VoxelStrateMeasurePrivate::FSampleGrid Grid;
    if (!VoxelStrateMeasurePrivate::BuildSampleGrid(
            nullptr, nullptr, &Sampler, INDEX_NONE, StrateBottomWorldZ, StrateTopWorldZ,
            BoundarySealThickness, true, Settings, OutSampleGrid != nullptr,
            Grid, Result.RefusalReason))
    {
        return Result;
    }

    Result.ResolvedMarginVoxels = Grid.ResolvedMarginVoxels;
    Result.SampledMinZ = Grid.SampledMinZ;
    Result.SampledMaxZ = Grid.SampledMaxZ;
    Result.SampledNumX = Grid.NumX;
    Result.SampledNumY = Grid.NumY;
    Result.SampledNumZ = Grid.NumZ;
    Result.SampledMinX = Grid.MinX;
    Result.SampledMaxX = Grid.MaxX;
    Result.SampledMinY = Grid.MinY;
    Result.SampledMaxY = Grid.MaxY;
    VoxelStrateMeasurePrivate::BuildMetricsFromGrid(Grid, Settings, Result);
    Result.bValid = Result.NumSampled > 0;
    if (!Result.bValid)
    {
        Result.RefusalReason = TEXT("The measurement grid was empty.");
    }
    if (OutSampleGrid != nullptr && Result.bValid)
    {
        VoxelStrateMeasurePrivate::ExportSampleGrid(Grid, *OutSampleGrid);
    }
    return Result;
}

EVoxelConnectivityResult VF_AreConnected(
    const UVoxelGenerator& Generator,
    const UVoxelStrateManager& Manager,
    int32 StrateIndex,
    const FVector& AVoxel,
    const FVector& BVoxel,
    const FVoxelStrateMeasureSettings& Settings,
    bool& bOutStartSnapped,
    bool& bOutGoalSnapped)
{
    int32 NumRouteRetries = 0;
    return VoxelStrateMeasurePrivate::QueryConnectivity(
        &Generator,
        &Manager,
        nullptr,
        StrateIndex,
        0, 0, 0.0f, false,
        AVoxel,
        BVoxel,
        Settings,
        bOutStartSnapped,
        bOutGoalSnapped,
        NumRouteRetries,
        nullptr);
}

EVoxelConnectivityResult VF_AreConnected(
    const UVoxelGenerator& Generator,
    const UVoxelStrateManager& Manager,
    int32 StrateIndex,
    const FVector& AVoxel,
    const FVector& BVoxel,
    const FVoxelStrateMeasureSettings& Settings,
    bool& bOutStartSnapped,
    bool& bOutGoalSnapped,
    int32& OutNumRouteRetries)
{
    return VoxelStrateMeasurePrivate::QueryConnectivity(
        &Generator,
        &Manager,
        nullptr,
        StrateIndex,
        0, 0, 0.0f, false,
        AVoxel,
        BVoxel,
        Settings,
        bOutStartSnapped,
        bOutGoalSnapped,
        OutNumRouteRetries,
        nullptr);
}

EVoxelConnectivityResult VF_AreConnected(
    const UVoxelGenerator& Generator,
    const UVoxelStrateManager& Manager,
    int32 StrateIndex,
    const FVector& AVoxel,
    const FVector& BVoxel,
    const FVoxelStrateMeasureSettings& Settings,
    bool& bOutStartSnapped,
    bool& bOutGoalSnapped,
    FVoxelConnectivityDiagnostics& OutDiagnostics)
{
    int32 NumRouteRetries = 0;
    return VoxelStrateMeasurePrivate::QueryConnectivity(
        &Generator,
        &Manager,
        nullptr,
        StrateIndex,
        0, 0, 0.0f, false,
        AVoxel,
        BVoxel,
        Settings,
        bOutStartSnapped,
        bOutGoalSnapped,
        NumRouteRetries,
        &OutDiagnostics);
}

FVoxelConnectivityDiagnostics VF_DiagnoseConnectivity(
    const UVoxelGenerator& Generator,
    const UVoxelStrateManager& Manager,
    int32 StrateIndex,
    const FVector& AVoxel,
    const FVector& BVoxel,
    const FVoxelStrateMeasureSettings& Settings)
{
    FVoxelConnectivityDiagnostics Result;
    int32 NumRouteRetries = 0;
    VoxelStrateMeasurePrivate::QueryConnectivity(
        &Generator,
        &Manager,
        nullptr,
        StrateIndex,
        0, 0, 0.0f, false,
        AVoxel,
        BVoxel,
        Settings,
        Result.bStartSnapped,
        Result.bGoalSnapped,
        NumRouteRetries,
        &Result);
    return Result;
}

EVoxelConnectivityResult VF_AreConnectedWithSampler(
    const IVoxelStrateDensitySampler& Sampler,
    int32 StrateBottomWorldZ,
    int32 StrateTopWorldZ,
    float BoundarySealThickness,
    const FVector& AVoxel,
    const FVector& BVoxel,
    const FVoxelStrateMeasureSettings& Settings,
    bool& bOutStartSnapped,
    bool& bOutGoalSnapped,
    FVoxelConnectivityDiagnostics* OutDiagnostics)
{
    int32 NumRouteRetries = 0;
    return VoxelStrateMeasurePrivate::QueryConnectivity(
        nullptr, nullptr, &Sampler, INDEX_NONE,
        StrateBottomWorldZ, StrateTopWorldZ, BoundarySealThickness, true,
        AVoxel, BVoxel, Settings, bOutStartSnapped, bOutGoalSnapped,
        NumRouteRetries, OutDiagnostics);
}

FVoxelConnectivityDiagnostics VF_DiagnoseConnectivityWithSampler(
    const IVoxelStrateDensitySampler& Sampler,
    int32 StrateBottomWorldZ,
    int32 StrateTopWorldZ,
    float BoundarySealThickness,
    const FVector& AVoxel,
    const FVector& BVoxel,
    const FVoxelStrateMeasureSettings& Settings)
{
    FVoxelConnectivityDiagnostics Result;
    VF_AreConnectedWithSampler(Sampler, StrateBottomWorldZ, StrateTopWorldZ,
                                BoundarySealThickness, AVoxel, BVoxel, Settings,
                                Result.bStartSnapped, Result.bGoalSnapped, &Result);
    return Result;
}

#endif // WITH_EDITOR — measurement never ships
