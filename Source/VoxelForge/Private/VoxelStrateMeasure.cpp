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
        // Exact scalar samples retained when the player-fit stencil needs a bounded local
        // surface/clearance interpolation, or when an explicit preview capture requests them.
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
            if (Settings.bIncludeOriginInCoverWindow)
            {
                // The fitted box is a route-coverage contract, not merely an endpoint box. An
                // origin-rooted graph may have its only mouth-to-mouth path through this spine.
                // Expand the requested bounds before the MaxCells check below; never crop the
                // route after discovering that the full box is too large.
                MinX64 = FMath::Min(MinX64, 0.0);
                MaxX64 = FMath::Max(MaxX64, 0.0);
                MinY64 = FMath::Min(MinY64, 0.0);
                MaxY64 = FMath::Max(MaxY64, 0.0);
            }
            if (Settings.bForceOriginColumnInCoverWindow)
            {
                // Upper bounds are exclusive.  The ordinary origin policy can therefore stop at
                // exactly X=0/Y=0 when both mouths are on one side; a diagnostic column probe
                // needs one real sampled column straddling the origin instead.
                MinX64 = FMath::Min(MinX64, -1.0);
                MaxX64 = FMath::Max(MaxX64, 1.0);
                MinY64 = FMath::Min(MinY64, -1.0);
                MaxY64 = FMath::Max(MaxY64, 1.0);
            }
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
        TArray<int64>* OutComponentCells,
        const TArray<uint8>* EligibilityMask = nullptr)
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
            if (Grid.Air[Start] == 0u
                || (EligibilityMask != nullptr && (*EligibilityMask)[Start] == 0u)
                || OutComponents[Start] >= 0)
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
                    if (Grid.Air[Neighbor] != 0u
                        && (EligibilityMask == nullptr || (*EligibilityMask)[Neighbor] != 0u)
                        && OutComponents[Neighbor] < 0)
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

    struct FPlayerCapsuleStencilRow
    {
        int32 RelativeZ = 0;
        TArray<FIntPoint> HorizontalOffsets;
    };

    bool HasScalarDensity(const FSampleGrid& Grid)
    {
        return Grid.Density.Num() == Grid.CellCount;
    }

    float CellDensity(const FSampleGrid& Grid, int32 Cell)
    {
        return HasScalarDensity(Grid)
            ? Grid.Density[Cell]
            : (Grid.Air[Cell] != 0u ? 1.0f : -1.0f);
    }

    bool SampleGridDensity(
        const FSampleGrid& Grid,
        const FVector& Point,
        float& OutDensity)
    {
        if (!Grid.ContainsPoint(Point))
        {
            return false;
        }

        const float Step = static_cast<float>(Grid.SampleStep);
        const float LocalX = (Point.X - Grid.MinX) / Step - 0.5f;
        const float LocalY = (Point.Y - Grid.MinY) / Step - 0.5f;
        const float LocalZ = (Point.Z - Grid.MinZ) / Step - 0.5f;

        if (!HasScalarDensity(Grid))
        {
            const int32 X = FMath::Clamp(
                FMath::FloorToInt((Point.X - Grid.MinX) / Step), 0, Grid.NumX - 1);
            const int32 Y = FMath::Clamp(
                FMath::FloorToInt((Point.Y - Grid.MinY) / Step), 0, Grid.NumY - 1);
            const int32 Z = FMath::Clamp(
                FMath::FloorToInt((Point.Z - Grid.MinZ) / Step), 0, Grid.NumZ - 1);
            OutDensity = CellDensity(Grid, Grid.Index(X, Y, Z));
            return FMath::IsFinite(OutDensity);
        }

        const int32 X0 = FMath::Clamp(FMath::FloorToInt(LocalX), 0, Grid.NumX - 1);
        const int32 Y0 = FMath::Clamp(FMath::FloorToInt(LocalY), 0, Grid.NumY - 1);
        const int32 Z0 = FMath::Clamp(FMath::FloorToInt(LocalZ), 0, Grid.NumZ - 1);
        const int32 X1 = FMath::Min(X0 + 1, Grid.NumX - 1);
        const int32 Y1 = FMath::Min(Y0 + 1, Grid.NumY - 1);
        const int32 Z1 = FMath::Min(Z0 + 1, Grid.NumZ - 1);
        const float AlphaX = FMath::Clamp(LocalX - static_cast<float>(X0), 0.0f, 1.0f);
        const float AlphaY = FMath::Clamp(LocalY - static_cast<float>(Y0), 0.0f, 1.0f);
        const float AlphaZ = FMath::Clamp(LocalZ - static_cast<float>(Z0), 0.0f, 1.0f);

        const float D000 = CellDensity(Grid, Grid.Index(X0, Y0, Z0));
        const float D100 = CellDensity(Grid, Grid.Index(X1, Y0, Z0));
        const float D010 = CellDensity(Grid, Grid.Index(X0, Y1, Z0));
        const float D110 = CellDensity(Grid, Grid.Index(X1, Y1, Z0));
        const float D001 = CellDensity(Grid, Grid.Index(X0, Y0, Z1));
        const float D101 = CellDensity(Grid, Grid.Index(X1, Y0, Z1));
        const float D011 = CellDensity(Grid, Grid.Index(X0, Y1, Z1));
        const float D111 = CellDensity(Grid, Grid.Index(X1, Y1, Z1));
        const float D00 = FMath::Lerp(D000, D100, AlphaX);
        const float D10 = FMath::Lerp(D010, D110, AlphaX);
        const float D01 = FMath::Lerp(D001, D101, AlphaX);
        const float D11 = FMath::Lerp(D011, D111, AlphaX);
        const float D0 = FMath::Lerp(D00, D10, AlphaY);
        const float D1 = FMath::Lerp(D01, D11, AlphaY);
        OutDensity = FMath::Lerp(D0, D1, AlphaZ);
        return FMath::IsFinite(OutDensity);
    }

    bool FindSupportSurfaceBelow(
        const FSampleGrid& Grid,
        int32 X,
        int32 Y,
        int32 CandidateAnchorZ,
        int32 MaxDownwardSearchCells,
        float MaxStepHeightVoxels,
        float& OutSurfaceHeight,
        int32& OutSurfaceAnchorZ)
    {
        const float Step = static_cast<float>(Grid.SampleStep);
        const float CandidateBottom = Grid.MinZ
            + static_cast<float>(CandidateAnchorZ) * Step;
        const float LowestAllowedSurface = CandidateBottom - MaxStepHeightVoxels;
        const int32 LowestAnchorZ = FMath::Max(
            1, CandidateAnchorZ - MaxDownwardSearchCells);

        // Search from the free pose downwards. The first transition in a column is the nearest
        // supporting surface; lower transitions are relevant only after the candidate has been
        // lifted by the bounded step-height tolerance.
        for (int32 SurfaceAnchorZ = CandidateAnchorZ;
             SurfaceAnchorZ >= LowestAnchorZ;
             --SurfaceAnchorZ)
        {
            const int32 SolidZ = SurfaceAnchorZ - 1;
            if (SolidZ < 0 || SurfaceAnchorZ >= Grid.NumZ)
            {
                continue;
            }
            const int32 SolidCell = Grid.Index(X, Y, SolidZ);
            const int32 AirCell = Grid.Index(X, Y, SurfaceAnchorZ);
            if (Grid.Air[SolidCell] != 0u || Grid.Air[AirCell] == 0u)
            {
                continue;
            }

            float SurfaceHeight = Grid.MinZ + static_cast<float>(SurfaceAnchorZ) * Step;
            if (HasScalarDensity(Grid))
            {
                const float LowerDensity = CellDensity(Grid, SolidCell);
                const float UpperDensity = CellDensity(Grid, AirCell);
                const float Denominator = UpperDensity - LowerDensity;
                if (!FMath::IsFinite(LowerDensity) || !FMath::IsFinite(UpperDensity)
                    || !FMath::IsFinite(Denominator) || Denominator <= 0.0f)
                {
                    continue;
                }
                const float Fraction = FMath::Clamp(
                    -LowerDensity / Denominator, 0.0f, 1.0f);
                SurfaceHeight = Grid.CellCenter(X, Y, SolidZ).Z + Fraction * Step;
            }

            if (FMath::IsFinite(SurfaceHeight)
                && SurfaceHeight <= CandidateBottom + KINDA_SMALL_NUMBER
                && SurfaceHeight >= LowestAllowedSurface - KINDA_SMALL_NUMBER)
            {
                OutSurfaceHeight = SurfaceHeight;
                OutSurfaceAnchorZ = SurfaceAnchorZ;
                return true;
            }
        }
        return false;
    }

    bool BuildPlayerSupportPatchOffsets(
        const FVoxelStrateMeasureSettings& Settings,
        TArray<FIntPoint>& OutOffsets,
        int32& OutCentreOffsetIndex,
        FString& OutReason)
    {
        OutOffsets.Reset();
        OutCentreOffsetIndex = INDEX_NONE;
        const double Radius = static_cast<double>(Settings.PlayerCapsuleRadiusVoxels);
        if (!FMath::IsFinite(Radius) || Radius <= 0.0)
        {
            return Refuse(OutReason,
                          TEXT("Player capsule radius must be finite and greater than zero."));
        }

        const double MaxOffsetReal = FMath::CeilToDouble(Radius);
        constexpr double MaxSupportPatchOffsets = static_cast<double>(1ll << 20);
        if (!FMath::IsFinite(MaxOffsetReal) || MaxOffsetReal > 4096.0)
        {
            return Refuse(OutReason,
                          TEXT("The player support patch radius exceeds the bounded fit stencil."));
        }
        const int32 MaxOffset = static_cast<int32>(MaxOffsetReal);
        int64 NumOffsets = 0;
        for (int32 OffsetY = -MaxOffset; OffsetY <= MaxOffset; ++OffsetY)
        {
            for (int32 OffsetX = -MaxOffset; OffsetX <= MaxOffset; ++OffsetX)
            {
                if (static_cast<double>(OffsetX * OffsetX + OffsetY * OffsetY)
                    > Radius * Radius + KINDA_SMALL_NUMBER)
                {
                    continue;
                }
                if (NumOffsets >= static_cast<int64>(MaxSupportPatchOffsets))
                {
                    return Refuse(
                        OutReason,
                        TEXT("The player support patch exceeds its bounded work limit."));
                }
                if (OffsetX == 0 && OffsetY == 0)
                {
                    OutCentreOffsetIndex = OutOffsets.Num();
                }
                OutOffsets.Add(FIntPoint(OffsetX, OffsetY));
                ++NumOffsets;
            }
        }
        if (OutCentreOffsetIndex == INDEX_NONE || OutOffsets.IsEmpty())
        {
            return Refuse(OutReason, TEXT("The player support patch has no footprint samples."));
        }
        return true;
    }

    bool IsWalkableSupportPatch(
        const TArray<FIntPoint>& SupportOffsets,
        const TArray<float>& SupportHeights,
        const TArray<uint8>& bSupported,
        int32 CentreOffsetIndex,
        int32 RequiredSupportCount,
        float MinimumWalkableNormalZ,
        int32& OutSupportCount,
        float& OutSupportHeight,
        int32& OutSupportAnchorZ,
        const TArray<int32>& SupportAnchorZs)
    {
        OutSupportCount = 0;
        OutSupportHeight = -FLT_MAX;
        OutSupportAnchorZ = INDEX_NONE;
        if (!bSupported.IsValidIndex(CentreOffsetIndex)
            || bSupported[CentreOffsetIndex] == 0u)
        {
            return false;
        }

        for (int32 Index = 0; Index < SupportOffsets.Num(); ++Index)
        {
            if (bSupported[Index] == 0u)
            {
                continue;
            }
            ++OutSupportCount;
            if (SupportHeights[Index] > OutSupportHeight
                || (SupportHeights[Index] == OutSupportHeight
                    && (OutSupportAnchorZ == INDEX_NONE
                        || SupportAnchorZs[Index] > OutSupportAnchorZ)))
            {
                OutSupportHeight = SupportHeights[Index];
                OutSupportAnchorZ = SupportAnchorZs[Index];
            }
        }
        if (OutSupportCount < RequiredSupportCount || OutSupportAnchorZ == INDEX_NONE)
        {
            return false;
        }

        // The worst pairwise secant is intentionally conservative: an isolated height jump in
        // the footprint cannot hide behind an average plane normal. For the default 1.36-voxel
        // capsule this checks the centre and the four cardinal support samples.
        float MaximumGradient = 0.0f;
        for (int32 First = 0; First < SupportOffsets.Num(); ++First)
        {
            if (bSupported[First] == 0u)
            {
                continue;
            }
            for (int32 Second = First + 1; Second < SupportOffsets.Num(); ++Second)
            {
                if (bSupported[Second] == 0u)
                {
                    continue;
                }
                const float DX = static_cast<float>(
                    SupportOffsets[Second].X - SupportOffsets[First].X);
                const float DY = static_cast<float>(
                    SupportOffsets[Second].Y - SupportOffsets[First].Y);
                const float HorizontalDistance = FMath::Sqrt(DX * DX + DY * DY);
                if (HorizontalDistance <= KINDA_SMALL_NUMBER)
                {
                    continue;
                }
                MaximumGradient = FMath::Max(
                    MaximumGradient,
                    FMath::Abs(SupportHeights[Second] - SupportHeights[First])
                        / HorizontalDistance);
            }
        }
        const float SupportNormalZ = 1.0f / FMath::Sqrt(
            1.0f + MaximumGradient * MaximumGradient);
        return FMath::IsFinite(SupportNormalZ)
            && SupportNormalZ + KINDA_SMALL_NUMBER >= MinimumWalkableNormalZ;
    }

    bool CapsuleClearAtSupportHeight(
        const FSampleGrid& Grid,
        const TArray<FPlayerCapsuleStencilRow>& StencilRows,
        int32 X,
        int32 Y,
        float SupportHeight)
    {
        const float Step = static_cast<float>(Grid.SampleStep);
        const float BaseX = Grid.MinX + (static_cast<float>(X) + 0.5f) * Step;
        const float BaseY = Grid.MinY + (static_cast<float>(Y) + 0.5f) * Step;
        for (const FPlayerCapsuleStencilRow& Row : StencilRows)
        {
            const float SampleZ = SupportHeight + static_cast<float>(Row.RelativeZ) + 0.5f * Step;
            for (const FIntPoint& Offset : Row.HorizontalOffsets)
            {
                float Density = 0.0f;
                if (!SampleGridDensity(
                        Grid,
                        FVector(
                            BaseX + static_cast<float>(Offset.X) * Step,
                            BaseY + static_cast<float>(Offset.Y) * Step,
                            SampleZ),
                        Density)
                    || !(Density > 0.0f))
                {
                    return false;
                }
            }
        }
        return true;
    }

    bool IsPointInsideCapsule(
        float RelativeZ,
        int32 OffsetX,
        int32 OffsetY,
        float Radius,
        float HalfHeight)
    {
        // The capsule is upright and this row is evaluated relative to the resolved support
        // surface. This is a direct capsule test in the fine voxel lattice; it deliberately does
        // not infer occupancy from Manhattan distance.
        const float AxisHalfLength = FMath::Max(0.0f, HalfHeight - Radius);
        const float DistanceToAxis = FMath::Max(
            FMath::Abs(RelativeZ) - AxisHalfLength, 0.0f);
        const float DistanceSquared = static_cast<float>(OffsetX * OffsetX + OffsetY * OffsetY)
            + DistanceToAxis * DistanceToAxis;
        return DistanceSquared <= Radius * Radius + KINDA_SMALL_NUMBER;
    }

    bool BuildPlayerCapsuleStencil(
        const FVoxelStrateMeasureSettings& Settings,
        TArray<FPlayerCapsuleStencilRow>& OutRows,
        FString& OutReason)
    {
        OutRows.Reset();
        if (!FMath::IsFinite(Settings.PlayerCapsuleRadiusVoxels)
            || Settings.PlayerCapsuleRadiusVoxels <= 0.0f
            || !FMath::IsFinite(Settings.PlayerCapsuleHalfHeightVoxels)
            || Settings.PlayerCapsuleHalfHeightVoxels <= 0.0f)
        {
            return Refuse(OutReason,
                          TEXT("Player capsule radius and half-height must be finite and greater than zero."));
        }

        const double HeightCellsReal = FMath::CeilToDouble(
            2.0 * static_cast<double>(Settings.PlayerCapsuleHalfHeightVoxels));
        // This is a stencil-size guard, not a gameplay threshold. It prevents a malformed
        // caller-supplied capsule from allocating an unbounded per-anchor loop.
        constexpr double MaxStencilHeightCells = 4096.0;
        if (!FMath::IsFinite(HeightCellsReal) || HeightCellsReal <= 0.0
            || HeightCellsReal > MaxStencilHeightCells)
        {
            return Refuse(OutReason,
                          TEXT("The player capsule height exceeds the bounded fit stencil."));
        }

        const double MaxHorizontalOffsetReal = FMath::CeilToDouble(
            static_cast<double>(Settings.PlayerCapsuleRadiusVoxels));
        constexpr double MaxStencilHorizontalRadiusCells = 4096.0;
        if (!FMath::IsFinite(MaxHorizontalOffsetReal)
            || MaxHorizontalOffsetReal < 0.0
            || MaxHorizontalOffsetReal > MaxStencilHorizontalRadiusCells)
        {
            return Refuse(OutReason,
                          TEXT("The player capsule radius exceeds the bounded fit stencil."));
        }
        const int32 HeightCells = static_cast<int32>(HeightCellsReal);
        const int32 MaxHorizontalOffset = static_cast<int32>(MaxHorizontalOffsetReal);
        // This bounds both the retained stencil and the per-anchor occupancy work. It is
        // deliberately far above the confirmed capsule's handful of offsets, but prevents a
        // malformed caller from turning a valid-looking finite radius into an enormous array.
        constexpr int64 MaxStencilOffsets = 1ll << 20;
        int64 NumStencilOffsets = 0;

        OutRows.Reserve(HeightCells);
        for (int32 RelativeZ = 0; RelativeZ < HeightCells; ++RelativeZ)
        {
            FPlayerCapsuleStencilRow& Row = OutRows.AddDefaulted_GetRef();
            Row.RelativeZ = RelativeZ;
            const float CapsuleRelativeZ = static_cast<float>(RelativeZ) + 0.5f
                - Settings.PlayerCapsuleHalfHeightVoxels;
            for (int32 OffsetY = -MaxHorizontalOffset;
                 OffsetY <= MaxHorizontalOffset;
                 ++OffsetY)
            {
                for (int32 OffsetX = -MaxHorizontalOffset;
                     OffsetX <= MaxHorizontalOffset;
                     ++OffsetX)
                {
                    if (IsPointInsideCapsule(
                            CapsuleRelativeZ, OffsetX, OffsetY,
                            Settings.PlayerCapsuleRadiusVoxels,
                            Settings.PlayerCapsuleHalfHeightVoxels))
                    {
                        if (NumStencilOffsets >= MaxStencilOffsets)
                        {
                            return Refuse(
                                OutReason,
                                TEXT("The player capsule occupancy stencil exceeds its bounded work limit."));
                        }
                        Row.HorizontalOffsets.Add(FIntPoint(OffsetX, OffsetY));
                        ++NumStencilOffsets;
                    }
                }
            }

            // Keep a centre sample for every row. This matters for the top partial row of a
            // capsule whose height is not an integer number of voxels: the full-height clearance
            // requirement must not silently become a seven-centre-point approximation.
            if (Row.HorizontalOffsets.IsEmpty())
            {
                Row.HorizontalOffsets.Add(FIntPoint::ZeroValue);
            }
        }
        return true;
    }

    bool BuildPlayerFitMask(
        const FSampleGrid& Grid,
        const FVoxelStrateMeasureSettings& Settings,
        TArray<uint8>& OutPlayerFit,
        int64& OutNumPlayerFitCells,
        FString& OutReason)
    {
        OutPlayerFit.Reset();
        OutNumPlayerFitCells = 0;
        if (Grid.SampleStep != 1)
        {
            return Refuse(
                OutReason,
                *FString::Printf(
                    TEXT("Player-fit metrics refused: SampleStep=%d; exact capsule fit requires SampleStep=1."),
                    Grid.SampleStep));
        }

        if (!FMath::IsFinite(Settings.PlayerMaxStepHeightMeters)
            || Settings.PlayerMaxStepHeightMeters < 0.0f)
        {
            return Refuse(
                OutReason,
                TEXT("PlayerMaxStepHeightMeters must be finite and non-negative."));
        }
        if (!FMath::IsFinite(Settings.PlayerWalkableFloorAngleDegrees)
            || Settings.PlayerWalkableFloorAngleDegrees < 0.0f
            || Settings.PlayerWalkableFloorAngleDegrees > 90.0f)
        {
            return Refuse(
                OutReason,
                TEXT("PlayerWalkableFloorAngleDegrees must be finite and within [0,90]."));
        }
        if (!FMath::IsFinite(Settings.PlayerSupportPatchMinCoverageFraction)
            || Settings.PlayerSupportPatchMinCoverageFraction <= 0.0f
            || Settings.PlayerSupportPatchMinCoverageFraction > 1.0f)
        {
            return Refuse(
                OutReason,
                TEXT("PlayerSupportPatchMinCoverageFraction must be within (0,1]."));
        }

        const double MaxStepHeightVoxelsReal =
            static_cast<double>(Settings.PlayerMaxStepHeightMeters)
            / static_cast<double>(FVoxelPlayerCapsuleConstants::VoxelSizeMeters);
        constexpr double MaxSupportSearchHeightVoxels = 4096.0;
        if (!FMath::IsFinite(MaxStepHeightVoxelsReal)
            || MaxStepHeightVoxelsReal < 0.0
            || MaxStepHeightVoxelsReal > MaxSupportSearchHeightVoxels)
        {
            return Refuse(
                OutReason,
                TEXT("PlayerMaxStepHeightMeters exceeds the bounded support search."));
        }
        const int32 MaxDownwardSearchCells = static_cast<int32>(
            FMath::CeilToDouble(MaxStepHeightVoxelsReal));
        const float MaxStepHeightVoxels = static_cast<float>(MaxStepHeightVoxelsReal);
        const float MinimumWalkableNormalZ = FMath::Cos(FMath::DegreesToRadians(
            Settings.PlayerWalkableFloorAngleDegrees));

        TArray<FIntPoint> SupportOffsets;
        int32 CentreOffsetIndex = INDEX_NONE;
        if (!BuildPlayerSupportPatchOffsets(
                Settings, SupportOffsets, CentreOffsetIndex, OutReason))
        {
            return false;
        }
        const int32 RequiredSupportCount = FMath::Clamp(
            FMath::CeilToInt(
                Settings.PlayerSupportPatchMinCoverageFraction
                    * static_cast<float>(SupportOffsets.Num())),
            1,
            SupportOffsets.Num());

        TArray<FPlayerCapsuleStencilRow> StencilRows;
        if (!BuildPlayerCapsuleStencil(Settings, StencilRows, OutReason))
        {
            return false;
        }

        OutPlayerFit.Init(0u, Grid.CellCount);
        TArray<float> SupportHeights;
        SupportHeights.SetNumUninitialized(SupportOffsets.Num());
        TArray<int32> SupportAnchorZs;
        SupportAnchorZs.SetNumUninitialized(SupportOffsets.Num());
        TArray<uint8> bSupported;
        bSupported.Init(0u, SupportOffsets.Num());
        for (int32 Z = 1; Z < Grid.NumZ; ++Z)
        {
            for (int32 Y = 0; Y < Grid.NumY; ++Y)
            {
                for (int32 X = 0; X < Grid.NumX; ++X)
                {
                    const int32 Current = Grid.Index(X, Y, Z);
                    // This is the free-pose candidate. Its support is resolved independently
                    // below, so an air cell one or two voxels above a surface can be lowered to
                    // that surface when the character's named step height permits it.
                    if (Grid.Air[Current] == 0u)
                    {
                        continue;
                    }

                    int32 NumSupported = 0;
                    for (int32 SupportIndex = 0;
                         SupportIndex < SupportOffsets.Num();
                         ++SupportIndex)
                    {
                        const FIntPoint& Offset = SupportOffsets[SupportIndex];
                        const int32 SupportX = X + Offset.X;
                        const int32 SupportY = Y + Offset.Y;
                        bSupported[SupportIndex] = 0u;
                        if (SupportX < 0 || SupportX >= Grid.NumX
                            || SupportY < 0 || SupportY >= Grid.NumY)
                        {
                            continue;
                        }

                        float SurfaceHeight = 0.0f;
                        int32 SurfaceAnchorZ = INDEX_NONE;
                        if (FindSupportSurfaceBelow(
                                Grid,
                                SupportX,
                                SupportY,
                                Z,
                                MaxDownwardSearchCells,
                                MaxStepHeightVoxels,
                                SurfaceHeight,
                                SurfaceAnchorZ))
                        {
                            bSupported[SupportIndex] = 1u;
                            SupportHeights[SupportIndex] = SurfaceHeight;
                            SupportAnchorZs[SupportIndex] = SurfaceAnchorZ;
                            ++NumSupported;
                        }
                    }

                    if (NumSupported < RequiredSupportCount
                        || bSupported[CentreOffsetIndex] == 0u)
                    {
                        continue;
                    }

                    int32 SupportCount = 0;
                    float SupportHeight = 0.0f;
                    int32 SupportAnchorZ = INDEX_NONE;
                    if (!IsWalkableSupportPatch(
                            SupportOffsets,
                            SupportHeights,
                            bSupported,
                            CentreOffsetIndex,
                            RequiredSupportCount,
                            MinimumWalkableNormalZ,
                            SupportCount,
                            SupportHeight,
                            SupportAnchorZ,
                            SupportAnchorZs))
                    {
                        continue;
                    }

                    // Resolve the pose to the highest coherent support in the patch and run the
                    // full capsule stencil again there. The support search alone is not allowed
                    // to turn a ledge or an overhang into a fit result. Keep the original free
                    // pose cell in the mask: every accepted pose is a valid point in the
                    // existing 6-neighbour traversal graph, and retaining the bounded vertical
                    // band is what lets adjacent gently sloped surfaces overlap instead of
                    // becoming diagonal, disconnected floor anchors.
                    if (!CapsuleClearAtSupportHeight(
                            Grid, StencilRows, X, Y, SupportHeight))
                    {
                        continue;
                    }

                    if (SupportAnchorZ < 1 || SupportAnchorZ >= Grid.NumZ)
                    {
                        continue;
                    }
                    if (OutPlayerFit[Current] != 0u)
                    {
                        continue;
                    }
                    OutPlayerFit[Current] = 1u;
                    ++OutNumPlayerFitCells;
                }
            }
        }
        return true;
    }

    float MinimumPlayerClearanceChebyshev(
        const FSampleGrid& Grid,
        const TArray<uint8>& PlayerFit)
    {
        if (PlayerFit.Num() != Grid.CellCount)
        {
            return 0.0f;
        }

        const int32 PlaneCells = Grid.NumX * Grid.NumY;
        constexpr int32 Infinity = INT32_MAX / 4;
        TArray<int32> Distances;
        Distances.SetNumUninitialized(PlaneCells);
        int32 MinimumDistance = Infinity;
        for (int32 Z = 0; Z < Grid.NumZ; ++Z)
        {
            for (int32 Y = 0; Y < Grid.NumY; ++Y)
            {
                for (int32 X = 0; X < Grid.NumX; ++X)
                {
                    const int32 PlaneIndex = X + Grid.NumX * Y;
                    Distances[PlaneIndex] = Grid.Air[Grid.Index(X, Y, Z)] == 0u
                        ? 0 : Infinity;
                }
            }

            auto Relax = [&](int32 Current, int32 Neighbor)
            {
                if (Distances[Neighbor] < Infinity)
                {
                    Distances[Current] = FMath::Min(
                        Distances[Current], Distances[Neighbor] + 1);
                }
            };

            // Two raster passes with the complete 8-neighbour stencil give a true 2D
            // Chebyshev distance to solid in this horizontal slice. It is used only for the
            // reported minimum; capsule occupancy above is the direct stencil check.
            for (int32 Y = 0; Y < Grid.NumY; ++Y)
            {
                for (int32 X = 0; X < Grid.NumX; ++X)
                {
                    const int32 Current = X + Grid.NumX * Y;
                    if (Grid.Air[Grid.Index(X, Y, Z)] == 0u) continue;
                    if (X > 0) Relax(Current, Current - 1);
                    if (Y > 0)
                    {
                        Relax(Current, Current - Grid.NumX);
                        if (X > 0) Relax(Current, Current - Grid.NumX - 1);
                        if (X + 1 < Grid.NumX) Relax(Current, Current - Grid.NumX + 1);
                    }
                }
            }
            for (int32 Y = Grid.NumY - 1; Y >= 0; --Y)
            {
                for (int32 X = Grid.NumX - 1; X >= 0; --X)
                {
                    const int32 Current = X + Grid.NumX * Y;
                    if (Grid.Air[Grid.Index(X, Y, Z)] == 0u) continue;
                    if (X + 1 < Grid.NumX) Relax(Current, Current + 1);
                    if (Y + 1 < Grid.NumY)
                    {
                        Relax(Current, Current + Grid.NumX);
                        if (X > 0) Relax(Current, Current + Grid.NumX - 1);
                        if (X + 1 < Grid.NumX) Relax(Current, Current + Grid.NumX + 1);
                    }
                }
            }

            for (int32 Y = 0; Y < Grid.NumY; ++Y)
            {
                for (int32 X = 0; X < Grid.NumX; ++X)
                {
                    const int32 Cell = Grid.Index(X, Y, Z);
                    if (PlayerFit[Cell] != 0u)
                    {
                        MinimumDistance = FMath::Min(
                            MinimumDistance, Distances[X + Grid.NumX * Y]);
                    }
                }
            }
        }

        return MinimumDistance == Infinity
            ? 0.0f : static_cast<float>(MinimumDistance);
    }

    void BuildPlayerFitMetrics(
        const FSampleGrid& Grid,
        const FVoxelStrateMeasureSettings& Settings,
        FVoxelStrateMetrics& InOutMetrics,
        TArray<uint8>* OutPlayerFitMask = nullptr)
    {
        InOutMetrics.bPlayerFitResolved = false;
        InOutMetrics.PlayerFitRefusalReason.Reset();
        InOutMetrics.NumPlayerFitCells = 0;
        InOutMetrics.PlayerFitFraction = 0.0f;
        InOutMetrics.NumTraversableComponents = 0;
        InOutMetrics.LargestTraversableComponentCells = 0;
        InOutMetrics.TraversableComponentShare = 0.0f;
        InOutMetrics.MinimumPlayerClearanceVoxels = 0.0f;

        TArray<uint8> PlayerFit;
        if (!BuildPlayerFitMask(
                Grid, Settings, PlayerFit, InOutMetrics.NumPlayerFitCells,
                InOutMetrics.PlayerFitRefusalReason))
        {
            if (OutPlayerFitMask != nullptr)
            {
                OutPlayerFitMask->Reset();
            }
            return;
        }
        InOutMetrics.bPlayerFitResolved = true;
        InOutMetrics.PlayerFitFraction = InOutMetrics.NumAir > 0
            ? static_cast<float>(static_cast<double>(InOutMetrics.NumPlayerFitCells)
                / static_cast<double>(InOutMetrics.NumAir))
            : 0.0f;

        TArray<int32> Components;
        int32 NumComponents = 0;
        int64 LargestComponentCells = 0;
        int32 LargestComponentLowestCell = INDEX_NONE;
        int32 NumComponentsAtLeast1Pct = 0;
        FloodFillAir(
            Grid,
            Components,
            NumComponents,
            InOutMetrics.NumPlayerFitCells,
            LargestComponentCells,
            LargestComponentLowestCell,
            NumComponentsAtLeast1Pct,
            nullptr,
            &PlayerFit);
        InOutMetrics.NumTraversableComponents = NumComponents;
        InOutMetrics.LargestTraversableComponentCells = LargestComponentCells;
        InOutMetrics.TraversableComponentShare = InOutMetrics.NumPlayerFitCells > 0
            ? static_cast<float>(static_cast<double>(LargestComponentCells)
                / static_cast<double>(InOutMetrics.NumPlayerFitCells))
            : 0.0f;
        InOutMetrics.MinimumPlayerClearanceVoxels = MinimumPlayerClearanceChebyshev(
            Grid, PlayerFit);
        if (OutPlayerFitMask != nullptr)
        {
            *OutPlayerFitMask = MoveTemp(PlayerFit);
        }
    }

    void BuildPlayerFitConnectivityMetrics(
        const FSampleGrid& Grid,
        const FVoxelStrateMeasureSettings& Settings,
        FVoxelStrateMetrics& OutMetrics,
        TArray<uint8>& OutPlayerFitMask)
    {
        OutMetrics = FVoxelStrateMetrics();
        OutMetrics.ResolvedMarginVoxels = Grid.ResolvedMarginVoxels;
        OutMetrics.SampledMinZ = Grid.SampledMinZ;
        OutMetrics.SampledMaxZ = Grid.SampledMaxZ;
        OutMetrics.SampledNumX = Grid.NumX;
        OutMetrics.SampledNumY = Grid.NumY;
        OutMetrics.SampledNumZ = Grid.NumZ;
        OutMetrics.SampledMinX = Grid.MinX;
        OutMetrics.SampledMaxX = Grid.MaxX;
        OutMetrics.SampledMinY = Grid.MinY;
        OutMetrics.SampledMaxY = Grid.MaxY;
        OutMetrics.NumSampled = Grid.CellCount;
        for (const uint8 bAir : Grid.Air)
        {
            OutMetrics.NumAir += bAir != 0u ? 1 : 0;
        }
        OutMetrics.NumSolid = static_cast<int64>(Grid.CellCount) - OutMetrics.NumAir;
        OutMetrics.AirFraction = OutMetrics.NumSampled > 0
            ? static_cast<float>(static_cast<double>(OutMetrics.NumAir)
                / static_cast<double>(OutMetrics.NumSampled))
            : 0.0f;
        BuildPlayerFitMetrics(Grid, Settings, OutMetrics, &OutPlayerFitMask);
        OutMetrics.bValid = OutMetrics.NumSampled > 0;
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
        FVoxelStrateMetrics& InOutMetrics,
        TArray<uint8>* OutPlayerFitMask = nullptr)
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

        // This is intentionally a separate fine-resolution pass. The legacy metrics above may
        // be useful at SampleStep=4/8, but they cannot answer whether the confirmed player capsule
        // fits. A coarse grid therefore receives an explicit refusal instead of an estimate.
        BuildPlayerFitMetrics(Grid, Settings, InOutMetrics, OutPlayerFitMask);
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
        Solid,
        NotPlayerFit
    };

    EEndpointCellResult ResolveEndpointCell(
        const FSampleGrid& Grid,
        const FVector& Point,
        int32& OutCell,
        bool& bOutSnapped,
        const TArray<uint8>* EligibilityMask = nullptr)
    {
        bOutSnapped = false;
        int32 OriginalCell = -1;
        if (!FindCellForPoint(Grid, Point, OriginalCell))
        {
            return EEndpointCellResult::OutOfWindow;
        }
        const auto IsEligible = [&, EligibilityMask](int32 Cell)
        {
            return Grid.Air[Cell] != 0u
                && (EligibilityMask == nullptr || (*EligibilityMask)[Cell] != 0u);
        };
        if (IsEligible(OriginalCell))
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
                    if (!IsEligible(Candidate))
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
            if (EligibilityMask != nullptr)
            {
                // Passage mouth points are authored on the centreline of a carved connector, not
                // necessarily on the floor-anchor lattice used by PlayerFit. Resolve them to the
                // nearest player-fitting floor cell over the bounded fine grid, rather than
                // declaring an in-air mouth unreachable just because its floor is several cells
                // below. The endpoint is reported as snapped, and the route re-check below still
                // validates the endpoint-to-anchor segment against density.
                for (int32 Candidate = 0; Candidate < Grid.CellCount; ++Candidate)
                {
                    if ((*EligibilityMask)[Candidate] == 0u)
                    {
                        continue;
                    }

                    int32 CandidateX = 0;
                    int32 CandidateY = 0;
                    int32 CandidateZ = 0;
                    DecodeIndex(Grid, Candidate, CandidateX, CandidateY, CandidateZ);
                    const float DistanceSquared = FVector::DistSquared(
                        Point, Grid.CellCenter(CandidateX, CandidateY, CandidateZ));
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
            return EligibilityMask != nullptr
                ? EEndpointCellResult::NotPlayerFit
                : EEndpointCellResult::Solid;
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
        FCoarseEdge& OutFailedEdge,
        const TArray<uint8>* EligibilityMask = nullptr)
    {
        OutFailedEdge = FCoarseEdge();

        if (EligibilityMask != nullptr)
        {
            // A player-fit route is deliberately a graph over supported free poses, not a second
            // air route with a coarse-cell illusion. The mask was built at SampleStep=1 and every
            // edge below is between adjacent player-fitting cells.
            for (int32 PathIndex = 0; PathIndex < Path.Num(); ++PathIndex)
            {
                if (!EligibilityMask->IsValidIndex(Path[PathIndex])
                    || (*EligibilityMask)[Path[PathIndex]] == 0u)
                {
                    if (PathIndex > 0)
                    {
                        OutFailedEdge = MakeCoarseEdge(
                            Path[PathIndex - 1], Path[PathIndex]);
                    }
                    return false;
                }
            }
            int32 StartX = 0;
            int32 StartY = 0;
            int32 StartZ = 0;
            DecodeIndex(Grid, Path[0], StartX, StartY, StartZ);
            int32 EndX = 0;
            int32 EndY = 0;
            int32 EndZ = 0;
            DecodeIndex(Grid, Path.Last(), EndX, EndY, EndZ);
            if (!FullResolutionAirOnSegment(
                    Generator, Sampler, A, Grid.CellCenter(StartX, StartY, StartZ))
                || !FullResolutionAirOnSegment(
                    Generator, Sampler, Grid.CellCenter(EndX, EndY, EndZ), B))
            {
                // Endpoint approaches have no graph edge to exclude. The player-fit route is
                // therefore unknown if either mouth-to-anchor segment contains solid density.
                return false;
            }
            return true;
        }

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
        TArray<int32>& OutPath,
        const TArray<uint8>* EligibilityMask = nullptr)
    {
        const auto IsEligible = [&, EligibilityMask](int32 Cell)
        {
            return Grid.Air[Cell] != 0u
                && (EligibilityMask == nullptr || (*EligibilityMask)[Cell] != 0u);
        };
        if (!IsEligible(Start) || !IsEligible(Goal))
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
                if (IsEligible(Neighbor)
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
        int32 MaxRouteRetries,
        const TArray<uint8>* EligibilityMask = nullptr)
    {
        OutStart = -1;
        OutGoal = -1;
        bOutStartSnapped = false;
        bOutGoalSnapped = false;
        OutNumRouteRetries = 0;
        const EEndpointCellResult StartCell = ResolveEndpointCell(
            Grid, AVoxel, OutStart, bOutStartSnapped, EligibilityMask);
        const EEndpointCellResult GoalCell = ResolveEndpointCell(
            Grid, BVoxel, OutGoal, bOutGoalSnapped, EligibilityMask);
        if (StartCell == EEndpointCellResult::OutOfWindow
            || GoalCell == EEndpointCellResult::OutOfWindow)
        {
            return EVoxelConnectivityResult::OutOfWindow;
        }
        if (StartCell == EEndpointCellResult::Solid)
        {
            return EVoxelConnectivityResult::StartCellSolid;
        }
        if (StartCell == EEndpointCellResult::NotPlayerFit)
        {
            return EVoxelConnectivityResult::StartCellNotPlayerFit;
        }
        if (GoalCell == EEndpointCellResult::Solid)
        {
            return EVoxelConnectivityResult::GoalCellSolid;
        }
        if (GoalCell == EEndpointCellResult::NotPlayerFit)
        {
            return EVoxelConnectivityResult::GoalCellNotPlayerFit;
        }

        TArray<FCoarseEdge> BlockedEdges;
        TArray<int32> Path;
        for (;;)
        {
            if (!FindCoarsePath(
                    Grid, OutStart, OutGoal, BlockedEdges, Path, EligibilityMask))
            {
                // This is a negative verdict only after all edges excluded by earlier
                // full-resolution failures leave no coarse route at this resolution.
                return EVoxelConnectivityResult::NotConnectedAtThisResolution;
            }

            // The coarse route is evidence, not a pass. Re-check every candidate at one-voxel
            // spacing in the original voxel field. A single solid sample refutes only this
            // route, so exclude its precise coarse edge and search again.
            FCoarseEdge FailedEdge;
            if (FullResolutionPathIsAir(
                    Generator, Sampler, Grid, AVoxel, BVoxel, Path, FailedEdge,
                    EligibilityMask))
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
        FVoxelConnectivityDiagnostics& OutDiagnostics,
        const TArray<uint8>* EligibilityMask = nullptr,
        TArray<int32>* OutComponents = nullptr,
        int32* OutNumComponents = nullptr)
    {
        OutDiagnostics = FVoxelConnectivityDiagnostics();
        if (OutComponents != nullptr)
        {
            OutComponents->Reset();
        }
        if (OutNumComponents != nullptr)
        {
            *OutNumComponents = 0;
        }
        OutDiagnostics.Result = ConnectivityResult;
        OutDiagnostics.bPlayerFitRestricted = EligibilityMask != nullptr;
        OutDiagnostics.bPlayerFitResolved = EligibilityMask != nullptr;
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
        for (int32 Index = 0; Index < Grid.CellCount; ++Index)
        {
            NumAir += Grid.Air[Index] != 0u
                && (EligibilityMask == nullptr || (*EligibilityMask)[Index] != 0u)
                ? 1 : 0;
        }
        FloodFillAir(
            Grid,
            Components,
            NumComponents,
            NumAir,
            LargestComponentCells,
            LargestComponentLowestCell,
            NumComponentsAtLeast1Pct,
            nullptr,
            EligibilityMask);

        if (OutComponents != nullptr)
        {
            *OutComponents = Components;
        }
        if (OutNumComponents != nullptr)
        {
            *OutNumComponents = NumComponents;
        }

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
        FVoxelConnectivityDiagnostics* OutDiagnostics,
        bool bPlayerFitRestricted = false,
        FVoxelStrateMetrics* OutPlayerMetrics = nullptr)
    {
        bOutStartSnapped = false;
        bOutGoalSnapped = false;
        OutNumRouteRetries = 0;
        if (OutPlayerMetrics != nullptr)
        {
            *OutPlayerMetrics = FVoxelStrateMetrics();
        }
        auto RefusePlayerFit = [&](const FString& Reason)
        {
            if (OutDiagnostics != nullptr)
            {
                *OutDiagnostics = FVoxelConnectivityDiagnostics();
                OutDiagnostics->bPlayerFitRestricted = bPlayerFitRestricted;
                OutDiagnostics->RefusalReason = Reason;
            }
            if (OutPlayerMetrics != nullptr)
            {
                OutPlayerMetrics->RefusalReason = Reason;
                OutPlayerMetrics->PlayerFitRefusalReason = Reason;
            }
        };
        if (!FMath::IsFinite(AVoxel.X) || !FMath::IsFinite(AVoxel.Y)
            || !FMath::IsFinite(AVoxel.Z) || !FMath::IsFinite(BVoxel.X)
            || !FMath::IsFinite(BVoxel.Y) || !FMath::IsFinite(BVoxel.Z))
        {
            if (OutDiagnostics != nullptr)
            {
                *OutDiagnostics = FVoxelConnectivityDiagnostics();
                OutDiagnostics->bPlayerFitRestricted = bPlayerFitRestricted;
            }
            return EVoxelConnectivityResult::OutOfWindow;
        }
        if (Settings.MaxRouteRetries < 0)
        {
            if (OutDiagnostics != nullptr)
            {
                *OutDiagnostics = FVoxelConnectivityDiagnostics();
                OutDiagnostics->bPlayerFitRestricted = bPlayerFitRestricted;
            }
            return EVoxelConnectivityResult::OutOfWindow;
        }

        if (bPlayerFitRestricted && Settings.SampleStep != 1)
        {
            RefusePlayerFit(FString::Printf(
                TEXT("Player-fit connectivity refused: SampleStep=%d; exact capsule fit requires SampleStep=1."),
                Settings.SampleStep));
            return EVoxelConnectivityResult::OutOfWindow;
        }

        FSampleGrid Grid;
        FString RefusalReason;
        if (!BuildSampleGrid(Generator, Manager, Sampler, StrateIndex,
                             ExplicitBottomWorldZ, ExplicitTopWorldZ,
                             ExplicitBoundarySealThickness, bUseExplicitBounds,
                             Settings, bPlayerFitRestricted, Grid, RefusalReason))
        {
            if (OutDiagnostics != nullptr)
            {
                *OutDiagnostics = FVoxelConnectivityDiagnostics();
                OutDiagnostics->bPlayerFitRestricted = bPlayerFitRestricted;
                OutDiagnostics->RefusalReason = RefusalReason;
            }
            if (OutPlayerMetrics != nullptr)
            {
                OutPlayerMetrics->RefusalReason = RefusalReason;
                OutPlayerMetrics->PlayerFitRefusalReason = RefusalReason;
            }
            return EVoxelConnectivityResult::OutOfWindow;
        }

        TArray<uint8> PlayerFitMask;
        FVoxelStrateMetrics PlayerMetrics;
        if (bPlayerFitRestricted)
        {
            // A player-fit query needs only the fine-grid air count plus the direct capsule
            // stencil. Do not pay for the legacy all-air flood fill, walkable-column scan, or
            // Manhattan feature transform here: those values belong to the separate legacy
            // measurement pass, and recomputing them for every fine candidate made the gate
            // needlessly expensive without changing its answer.
            BuildPlayerFitConnectivityMetrics(Grid, Settings, PlayerMetrics, PlayerFitMask);
            if (OutPlayerMetrics != nullptr)
            {
                *OutPlayerMetrics = PlayerMetrics;
            }
            if (!PlayerMetrics.bPlayerFitResolved)
            {
                RefusePlayerFit(PlayerMetrics.PlayerFitRefusalReason);
                return EVoxelConnectivityResult::OutOfWindow;
            }
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
            Settings.MaxRouteRetries,
            bPlayerFitRestricted ? &PlayerFitMask : nullptr);
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
                *OutDiagnostics,
                bPlayerFitRestricted ? &PlayerFitMask : nullptr);
            if (bPlayerFitRestricted &&
                (Result == EVoxelConnectivityResult::StartCellNotPlayerFit
                 || Result == EVoxelConnectivityResult::GoalCellNotPlayerFit))
            {
                OutDiagnostics->RefusalReason = Result == EVoxelConnectivityResult::StartCellNotPlayerFit
                    ? TEXT("arrival endpoint is not a player-fitting floor cell")
                    : TEXT("departure endpoint is not a player-fitting floor cell");
            }
        }
        return Result;
    }

    int32 GetSixNeighbour(const FSampleGrid& Grid, int32 Cell, int32 Direction)
    {
        int32 X = 0;
        int32 Y = 0;
        int32 Z = 0;
        DecodeIndex(Grid, Cell, X, Y, Z);
        switch (Direction)
        {
        case 0: return X + 1 < Grid.NumX ? Grid.Index(X + 1, Y, Z) : INDEX_NONE;
        case 1: return X > 0 ? Grid.Index(X - 1, Y, Z) : INDEX_NONE;
        case 2: return Y + 1 < Grid.NumY ? Grid.Index(X, Y + 1, Z) : INDEX_NONE;
        case 3: return Y > 0 ? Grid.Index(X, Y - 1, Z) : INDEX_NONE;
        case 4: return Z + 1 < Grid.NumZ ? Grid.Index(X, Y, Z + 1) : INDEX_NONE;
        case 5: return Z > 0 ? Grid.Index(X, Y, Z - 1) : INDEX_NONE;
        default: return INDEX_NONE;
        }
    }

    bool IsEligiblePlayerFitCell(
        const FSampleGrid& Grid,
        const TArray<uint8>& PlayerFitMask,
        int32 Cell)
    {
        return Cell >= 0
            && Cell < Grid.CellCount
            && Grid.Air[Cell] != 0u
            && PlayerFitMask[Cell] != 0u;
    }

    int32 CountPlayerFitNeighbours(
        const FSampleGrid& Grid,
        const TArray<uint8>& PlayerFitMask,
        int32 Cell)
    {
        int32 Count = 0;
        for (int32 Direction = 0; Direction < 6; ++Direction)
        {
            if (IsEligiblePlayerFitCell(
                    Grid, PlayerFitMask, GetSixNeighbour(Grid, Cell, Direction)))
            {
                ++Count;
            }
        }
        return Count;
    }

    int32 LocalAirSpanVoxels(
        const FSampleGrid& Grid,
        int32 Cell,
        int32 Axis,
        int32 MaxRelevantSpanVoxels)
    {
        if (Cell < 0 || Cell >= Grid.CellCount || Grid.Air[Cell] == 0u)
        {
            return 0;
        }

        int32 CentreX = 0;
        int32 CentreY = 0;
        int32 CentreZ = 0;
        DecodeIndex(Grid, Cell, CentreX, CentreY, CentreZ);
        int32 Span = 1;
        bool bTouchesWindowBoundary = false;
        for (const int32 Sign : { -1, 1 })
        {
            int32 Offset = 1;
            for (;; ++Offset)
            {
                if (Span >= MaxRelevantSpanVoxels)
                {
                    return Span;
                }
                const int32 X = Axis == 0 ? CentreX + Sign * Offset : CentreX;
                const int32 Y = Axis == 1 ? CentreY + Sign * Offset : CentreY;
                const int32 Z = CentreZ;
                if (X < 0 || X >= Grid.NumX || Y < 0 || Y >= Grid.NumY)
                {
                    bTouchesWindowBoundary = true;
                    break;
                }
                if (Grid.Air[Grid.Index(X, Y, Z)] == 0u)
                {
                    break;
                }
                ++Span;
            }
        }
        // A window edge is a lower bound, not a solid wall. Do not label a corridor narrow merely
        // because the fitted diagnostic box clipped the air span before reaching rock.
        return bTouchesWindowBoundary ? MAX_int32 : Span * Grid.SampleStep;
    }

    float LocalWidestHorizontalAirSpanVoxels(
        const FSampleGrid& Grid,
        int32 Cell,
        float NarrowGapThresholdVoxels)
    {
        const int32 MaxRelevantSpanVoxels = FMath::Max(
            1,
            FMath::CeilToInt(NarrowGapThresholdVoxels));
        return static_cast<float>(FMath::Max(
            LocalAirSpanVoxels(Grid, Cell, 0, MaxRelevantSpanVoxels),
            LocalAirSpanVoxels(Grid, Cell, 1, MaxRelevantSpanVoxels)));
    }

    void WalkPlayerFitGraph(
        const FSampleGrid& Grid,
        const TArray<uint8>& PlayerFitMask,
        int32 Start,
        int32 Goal,
        float NarrowGapThresholdVoxels,
        FVoxelPlayerFitWalkReport& InOutReport,
        int32& OutFinalCell,
        int32& OutLastReachedCell)
    {
        OutFinalCell = INDEX_NONE;
        OutLastReachedCell = INDEX_NONE;
        if (!IsEligiblePlayerFitCell(Grid, PlayerFitMask, Start)
            || !IsEligiblePlayerFitCell(Grid, PlayerFitMask, Goal))
        {
            return;
        }

        OutFinalCell = Start;
        OutLastReachedCell = Start;

        TArray<uint8> Visited;
        Visited.Init(0u, Grid.CellCount);
        TArray<uint8> NarrowCell;
        NarrowCell.Init(255u, Grid.CellCount);
        TArray<int32> Stack;
        TArray<int32> NextDirection;
        // Reserve the full bounded capacity up front. TArray growth slack is otherwise allowed
        // to exceed the walker's stated memory budget while the DFS discovers a large component.
        Stack.Reserve(Grid.CellCount);
        NextDirection.Reserve(Grid.CellCount);
        Stack.Add(Start);
        NextDirection.Add(0);
        Visited[Start] = 1u;

        auto RecordTraversal = [&](int32 From, int32 To)
        {
            ++InOutReport.AgentGraphTraversals;
            auto IsNarrowCell = [&](int32 Cell)
            {
                if (NarrowCell[Cell] == 255u)
                {
                    NarrowCell[Cell] = LocalWidestHorizontalAirSpanVoxels(
                        Grid, Cell, NarrowGapThresholdVoxels)
                        < NarrowGapThresholdVoxels ? 1u : 0u;
                }
                return NarrowCell[Cell] != 0u;
            };
            const bool bNarrow = IsNarrowCell(From) || IsNarrowCell(To);
            if (bNarrow)
            {
                ++InOutReport.NarrowGapTraversals;
            }
        };

        while (Stack.Num() > 0)
        {
            const int32 Current = Stack.Last();
            OutFinalCell = Current;
            if (Current == Goal)
            {
                InOutReport.bCanReachDeparture = true;
                break;
            }

            bool bAdvanced = false;
            while (NextDirection.Last() < 6)
            {
                const int32 Direction = NextDirection.Last()++;
                const int32 Neighbour = GetSixNeighbour(Grid, Current, Direction);
                if (!IsEligiblePlayerFitCell(Grid, PlayerFitMask, Neighbour)
                    || Visited[Neighbour] != 0u)
                {
                    continue;
                }

                Visited[Neighbour] = 1u;
                RecordTraversal(Current, Neighbour);
                Stack.Add(Neighbour);
                NextDirection.Add(0);
                OutLastReachedCell = Neighbour;
                bAdvanced = true;
                break;
            }

            if (bAdvanced)
            {
                continue;
            }

            // A degree-one pose is an actual topological dead end. A higher-degree pose with all
            // branches already visited is merely a DFS backtrack through a loop/junction.
            if (Current != Start && Current != Goal
                && CountPlayerFitNeighbours(Grid, PlayerFitMask, Current) <= 1)
            {
                ++InOutReport.DeadEndsEncountered;
            }

            Stack.Pop();
            NextDirection.Pop();
            if (Stack.Num() > 0)
            {
                RecordTraversal(Current, Stack.Last());
            }
        }

        const float DistanceMeters = static_cast<float>(InOutReport.AgentGraphTraversals)
            * FVoxelPlayerCapsuleConstants::VoxelSizeMeters;
        InOutReport.DistanceTravelledMeters = DistanceMeters;
        InOutReport.NarrowGapFraction = InOutReport.AgentGraphTraversals > 0
            ? static_cast<float>(static_cast<double>(InOutReport.NarrowGapTraversals)
                / static_cast<double>(InOutReport.AgentGraphTraversals))
            : 0.0f;
        InOutReport.NarrowGapEventsPer100m = DistanceMeters > KINDA_SMALL_NUMBER
            ? static_cast<float>(static_cast<double>(InOutReport.NarrowGapTraversals)
                * 100.0 / static_cast<double>(DistanceMeters))
            : 0.0f;
        InOutReport.DeadEndsPer100m = DistanceMeters > KINDA_SMALL_NUMBER
            ? static_cast<float>(static_cast<double>(InOutReport.DeadEndsEncountered)
                * 100.0 / static_cast<double>(DistanceMeters))
            : 0.0f;
        InOutReport.DeadEndsEncountered = FMath::Max<int64>(
            InOutReport.DeadEndsEncountered, 0);
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
            Settings, OutSampleGrid != nullptr || Settings.SampleStep == 1,
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
            BoundarySealThickness, true, Settings,
            OutSampleGrid != nullptr || Settings.SampleStep == 1,
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

EVoxelConnectivityResult VF_ArePlayerFitConnectedWithSampler(
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
        NumRouteRetries, OutDiagnostics, true, nullptr);
}

FVoxelConnectivityDiagnostics VF_DiagnosePlayerFitConnectivityWithSampler(
    const IVoxelStrateDensitySampler& Sampler,
    int32 StrateBottomWorldZ,
    int32 StrateTopWorldZ,
    float BoundarySealThickness,
    const FVector& AVoxel,
    const FVector& BVoxel,
    const FVoxelStrateMeasureSettings& Settings,
    FVoxelStrateMetrics* OutPlayerMetrics)
{
    FVoxelConnectivityDiagnostics Result;
    int32 NumRouteRetries = 0;
    VoxelStrateMeasurePrivate::QueryConnectivity(
        nullptr, nullptr, &Sampler, INDEX_NONE,
        StrateBottomWorldZ, StrateTopWorldZ, BoundarySealThickness, true,
        AVoxel, BVoxel, Settings, Result.bStartSnapped, Result.bGoalSnapped,
        NumRouteRetries, &Result, true, OutPlayerMetrics);
    return Result;
}

bool VF_MeasurePlayerFitWalkWithSampler(
    const IVoxelStrateDensitySampler& Sampler,
    int32 StrateBottomWorldZ,
    int32 StrateTopWorldZ,
    float BoundarySealThickness,
    const FVector& AVoxel,
    const FVector& BVoxel,
    const FVoxelStrateMeasureSettings& Settings,
    FVoxelPlayerFitWalkReport& OutReport)
{
    using namespace VoxelStrateMeasurePrivate;

    OutReport = FVoxelPlayerFitWalkReport();
    OutReport.CapsuleRadiusMeters = Settings.PlayerCapsuleRadiusVoxels
        * FVoxelPlayerCapsuleConstants::VoxelSizeMeters;
    OutReport.CapsuleWidthMeters = 2.0f * OutReport.CapsuleRadiusMeters;
    OutReport.CapsuleHeightMeters = 2.0f * Settings.PlayerCapsuleHalfHeightVoxels
        * FVoxelPlayerCapsuleConstants::VoxelSizeMeters;
    OutReport.NarrowGapThresholdMeters = 1.5f * OutReport.CapsuleWidthMeters;
    OutReport.NarrowGapDefinition = TEXT(
        "widest axis-aligned horizontal air span at each traversed fit-graph cell");

    const bool bFiniteEndpoints = FMath::IsFinite(AVoxel.X)
        && FMath::IsFinite(AVoxel.Y)
        && FMath::IsFinite(AVoxel.Z)
        && FMath::IsFinite(BVoxel.X)
        && FMath::IsFinite(BVoxel.Y)
        && FMath::IsFinite(BVoxel.Z);
    if (!bFiniteEndpoints)
    {
        OutReport.RefusalReason = TEXT("Arrival and departure points must be finite.");
        return false;
    }
    if (Settings.MaxRouteRetries < 0)
    {
        OutReport.RefusalReason = TEXT("MaxRouteRetries cannot be negative.");
        return false;
    }
    if (Settings.SampleStep != 1)
    {
        OutReport.RefusalReason = TEXT(
            "Player-fit walk refused: exact capsule fit requires SampleStep=1.");
        return false;
    }

    FSampleGrid Grid;
    FString RefusalReason;
    if (!BuildSampleGrid(
            nullptr,
            nullptr,
            &Sampler,
            INDEX_NONE,
            StrateBottomWorldZ,
            StrateTopWorldZ,
            BoundarySealThickness,
            true,
            Settings,
            true,
            Grid,
            RefusalReason))
    {
        OutReport.RefusalReason = RefusalReason;
        return false;
    }

    TArray<uint8> PlayerFitMask;
    int64 NumPlayerFitCells = 0;
    if (!BuildPlayerFitMask(
            Grid,
            Settings,
            PlayerFitMask,
            NumPlayerFitCells,
            RefusalReason))
    {
        OutReport.RefusalReason = RefusalReason;
        return false;
    }

    int32 Start = INDEX_NONE;
    int32 Goal = INDEX_NONE;
    int32 NumRouteRetries = 0;
    const EVoxelConnectivityResult Result = EvaluateConnectivityOnGrid(
        nullptr,
        &Sampler,
        Grid,
        AVoxel,
        BVoxel,
        Start,
        Goal,
        OutReport.bStartSnapped,
        OutReport.bGoalSnapped,
        NumRouteRetries,
        Settings.MaxRouteRetries,
        &PlayerFitMask);

    FVoxelConnectivityDiagnostics Diagnostics;
    TArray<int32> Components;
    int32 NumComponents = 0;
    PopulateConnectivityDiagnostics(
        Grid,
        Start,
        Goal,
        Result,
        OutReport.bStartSnapped,
        OutReport.bGoalSnapped,
        NumRouteRetries,
        Diagnostics,
        &PlayerFitMask,
        &Components,
        &NumComponents);

    OutReport.bValid = true;
    OutReport.Result = Result;
    OutReport.NumRouteRetries = NumRouteRetries;
    OutReport.SampledNumX = Grid.NumX;
    OutReport.SampledNumY = Grid.NumY;
    OutReport.SampledNumZ = Grid.NumZ;
    OutReport.SampledMinZ = Grid.SampledMinZ;
    OutReport.SampledMaxZ = Grid.SampledMaxZ;
    OutReport.SampledMinX = Grid.MinX;
    OutReport.SampledMaxX = Grid.MaxX;
    OutReport.SampledMinY = Grid.MinY;
    OutReport.SampledMaxY = Grid.MaxY;
    OutReport.PlayerFitVolumeCells = NumPlayerFitCells;
    OutReport.ReachablePlayerFitCells = FMath::Max<int64>(
        Diagnostics.StartComponentCells, 0);
    OutReport.ReachablePlayerFitFraction = OutReport.PlayerFitVolumeCells > 0
        ? static_cast<float>(static_cast<double>(OutReport.ReachablePlayerFitCells)
            / static_cast<double>(OutReport.PlayerFitVolumeCells))
        : 0.0f;
    OutReport.PlayerFitComponents = NumComponents;
    OutReport.LargestPlayerFitComponentCells = Diagnostics.LargestComponentCells;
    OutReport.ArrivalComponentCells = Diagnostics.StartComponentCells;
    OutReport.DepartureComponentCells = Diagnostics.GoalComponentCells;
    OutReport.MouthComponentGapVoxels = Diagnostics.StartToGoalComponentDistanceCells;
    OutReport.StraightLineMeters = FVector::Dist(AVoxel, BVoxel)
        * FVoxelPlayerCapsuleConstants::VoxelSizeMeters;

    int32 AgentFinalCell = INDEX_NONE;
    int32 LastReachedCell = INDEX_NONE;
    if (Start >= 0 && Goal >= 0)
    {
        const float NarrowGapThresholdVoxels = OutReport.NarrowGapThresholdMeters
            / FVoxelPlayerCapsuleConstants::VoxelSizeMeters;
        WalkPlayerFitGraph(
            Grid,
            PlayerFitMask,
            Start,
            Goal,
            NarrowGapThresholdVoxels,
            OutReport,
            AgentFinalCell,
            LastReachedCell);
    }

    auto RecordCellPosition = [&Grid](
        int32 Cell,
        bool& bOutHasPosition,
        FVector& OutPosition)
    {
        if (Cell >= 0 && Cell < Grid.CellCount)
        {
            int32 X = 0;
            int32 Y = 0;
            int32 Z = 0;
            DecodeIndex(Grid, Cell, X, Y, Z);
            bOutHasPosition = true;
            OutPosition = Grid.CellCenter(X, Y, Z);
        }
    };
    RecordCellPosition(
        AgentFinalCell,
        OutReport.bHasAgentFinalPosition,
        OutReport.AgentFinalVoxels);
    RecordCellPosition(
        LastReachedCell,
        OutReport.bHasLastReachedPosition,
        OutReport.LastReachedVoxels);

    const int32 StartComponent = Start >= 0 && Components.IsValidIndex(Start)
        ? Components[Start] : INDEX_NONE;
    const int32 GoalComponent = Goal >= 0 && Components.IsValidIndex(Goal)
        ? Components[Goal] : INDEX_NONE;
    if (StartComponent >= 0)
    {
        float BestOriginDistanceSquared = FLT_MAX;
        for (int32 Cell = 0; Cell < Grid.CellCount; ++Cell)
        {
            if (!Components.IsValidIndex(Cell) || Components[Cell] != StartComponent)
            {
                continue;
            }
            int32 X = 0;
            int32 Y = 0;
            int32 Z = 0;
            DecodeIndex(Grid, Cell, X, Y, Z);
            const FVector Position = Grid.CellCenter(X, Y, Z);
            BestOriginDistanceSquared = FMath::Min(
                BestOriginDistanceSquared,
                Position.X * Position.X + Position.Y * Position.Y);
        }
        if (BestOriginDistanceSquared < FLT_MAX)
        {
            OutReport.ReachableSetToOriginColumnVoxels = FMath::Sqrt(
                BestOriginDistanceSquared);
        }
    }

    const bool bOriginPointInWindow = 0.0f >= Grid.MinX && 0.0f < Grid.MaxX
        && 0.0f >= Grid.MinY && 0.0f < Grid.MaxY;
    if (bOriginPointInWindow)
    {
        const int32 OriginX = FMath::Clamp(
            FMath::FloorToInt((0.0f - Grid.MinX) / static_cast<float>(Grid.SampleStep)),
            0,
            Grid.NumX - 1);
        const int32 OriginY = FMath::Clamp(
            FMath::FloorToInt((0.0f - Grid.MinY) / static_cast<float>(Grid.SampleStep)),
            0,
            Grid.NumY - 1);
        OutReport.bOriginColumnInSampledWindow = true;
        for (int32 Z = 0; Z < Grid.NumZ; ++Z)
        {
            const int32 Cell = Grid.Index(OriginX, OriginY, Z);
            if (PlayerFitMask[Cell] == 0u)
            {
                continue;
            }
            ++OutReport.OriginColumnPlayerFitCells;
            OutReport.bOriginColumnHasPlayerFit = true;
            if (Components.IsValidIndex(Cell) && Components[Cell] == StartComponent)
            {
                OutReport.bOriginColumnReachable = true;
            }
        }
    }

    if (AgentFinalCell >= 0 && AgentFinalCell < Grid.CellCount && GoalComponent >= 0)
    {
        const FVector AgentPosition = OutReport.AgentFinalVoxels;
        float BestTargetDistanceSquared = FLT_MAX;
        int32 BestTargetCell = INDEX_NONE;
        for (int32 Cell = 0; Cell < Grid.CellCount; ++Cell)
        {
            if (!Components.IsValidIndex(Cell) || Components[Cell] != GoalComponent)
            {
                continue;
            }
            int32 X = 0;
            int32 Y = 0;
            int32 Z = 0;
            DecodeIndex(Grid, Cell, X, Y, Z);
            const float DistanceSquared = FVector::DistSquared(
                AgentPosition, Grid.CellCenter(X, Y, Z));
            if (DistanceSquared < BestTargetDistanceSquared
                || (DistanceSquared == BestTargetDistanceSquared
                    && (BestTargetCell == INDEX_NONE || Cell < BestTargetCell)))
            {
                BestTargetDistanceSquared = DistanceSquared;
                BestTargetCell = Cell;
            }
        }
        if (BestTargetCell >= 0)
        {
            RecordCellPosition(
                BestTargetCell,
                OutReport.bHasTargetComponentNearestCell,
                OutReport.TargetComponentNearestVoxels);
            OutReport.AgentToTargetComponentGapVoxels = FMath::Sqrt(
                BestTargetDistanceSquared);
        }
    }

    OutReport.bCanReachDeparture = Result == EVoxelConnectivityResult::Connected;
    OutReport.Tortuosity = OutReport.StraightLineMeters > KINDA_SMALL_NUMBER
        ? OutReport.DistanceTravelledMeters / OutReport.StraightLineMeters
        : 0.0f;
    OutReport.DeadEndsEncountered = FMath::Max<int64>(
        OutReport.DeadEndsEncountered, 0);
    OutReport.NarrowGapEventsPer100m = OutReport.DistanceTravelledMeters
        > KINDA_SMALL_NUMBER
        ? static_cast<float>(static_cast<double>(OutReport.NarrowGapTraversals)
            * 100.0 / static_cast<double>(OutReport.DistanceTravelledMeters))
        : 0.0f;
    OutReport.DeadEndsPer100m = OutReport.DistanceTravelledMeters
        > KINDA_SMALL_NUMBER
        ? static_cast<float>(static_cast<double>(OutReport.DeadEndsEncountered)
            * 100.0 / static_cast<double>(OutReport.DistanceTravelledMeters))
        : 0.0f;

    if (Result == EVoxelConnectivityResult::StartCellNotPlayerFit)
    {
        OutReport.RefusalReason = TEXT("arrival endpoint is not a player-fitting floor cell");
    }
    else if (Result == EVoxelConnectivityResult::GoalCellNotPlayerFit)
    {
        OutReport.RefusalReason = TEXT("departure endpoint is not a player-fitting floor cell");
    }
    else if (Result == EVoxelConnectivityResult::OutOfWindow)
    {
        OutReport.RefusalReason = TEXT("arrival or departure point is outside the fitted walk window");
    }
    else
    {
        OutReport.RefusalReason = Diagnostics.RefusalReason;
    }
    return true;
}

#endif // WITH_EDITOR — measurement never ships
