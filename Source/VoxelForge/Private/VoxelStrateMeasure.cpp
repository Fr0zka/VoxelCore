// Read-only, deterministic strate measurement and connectivity checks.

#include "VoxelStrateMeasure.h"

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
        const UVoxelGenerator& Generator,
        const UVoxelStrateManager& Manager,
        int32 StrateIndex,
        const FVoxelStrateMeasureSettings& Settings,
        FSampleGrid& OutGrid,
        FString& OutReason)
    {
        const TArray<FStrateSlot>& Layout = Manager.GetLayout();
        if (!Layout.IsValidIndex(StrateIndex))
        {
            return Refuse(OutReason, TEXT("StrateIndex is outside the manager layout."));
        }

        const FStrateSlot& Slot = Layout[StrateIndex];
        if (Slot.Definition == nullptr)
        {
            return Refuse(OutReason, TEXT("The requested strate has no definition."));
        }
        if (Settings.SampleStep <= 0)
        {
            return Refuse(OutReason, TEXT("SampleStep must be greater than zero."));
        }
        if (Settings.RadiusInVoxels <= 0)
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
        if (!FMath::IsFinite(Settings.CenterXY.X) || !FMath::IsFinite(Settings.CenterXY.Y))
        {
            return Refuse(OutReason, TEXT("CenterXY must contain finite voxel coordinates."));
        }
        if (Slot.TopChunkZ <= Slot.BottomChunkZ)
        {
            return Refuse(OutReason, TEXT("The requested strate has no positive vertical extent."));
        }

        const int64 StrateBottomZ = static_cast<int64>(Slot.BottomChunkZ) * CHUNK_SIZE;
        const int64 StrateTopZ = (static_cast<int64>(Slot.TopChunkZ) + 1) * CHUNK_SIZE;
        const int64 StrateHeight = StrateTopZ - StrateBottomZ;
        if (StrateHeight <= 0)
        {
            return Refuse(OutReason, TEXT("The requested strate has no positive voxel height."));
        }

        // Resolve the margin from the same blended parameter query used by generation. The
        // representative chunk is deliberately in the middle of this slot and uses the sample
        // centre's XY chunk, so a future non-Hard transition resolves through the manager exactly
        // as generation does at that representative location.
        const int32 MidChunkZ = Slot.BottomChunkZ
            + (Slot.TopChunkZ - Slot.BottomChunkZ) / 2;
        const FIntVector RepresentativeChunk(
            FMath::FloorToInt(Settings.CenterXY.X / static_cast<float>(CHUNK_SIZE)),
            FMath::FloorToInt(Settings.CenterXY.Y / static_cast<float>(CHUNK_SIZE)),
            MidChunkZ);
        const FStrateGenerationParams RepresentativeParams =
            Manager.GetGenerationParams(RepresentativeChunk);

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
                RequestedMargin = 1;
            }
            else
            {
                RequestedMargin = FMath::CeilToInt(DoubleSealThickness);
            }
        }
        const int32 ResolvedMargin = FMath::Clamp(RequestedMargin, 1, MaxMargin);

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

        const int64 XYExtent = static_cast<int64>(Settings.RadiusInVoxels) * 2;
        const int64 Step = static_cast<int64>(Settings.SampleStep);
        const int64 NumX64 = CeilDivPositive(XYExtent, Step);
        const int64 NumY64 = NumX64;
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

        const double MinX64 = static_cast<double>(Settings.CenterXY.X)
            - static_cast<double>(Settings.RadiusInVoxels);
        const double MaxX64 = static_cast<double>(Settings.CenterXY.X)
            + static_cast<double>(Settings.RadiusInVoxels);
        const double MinY64 = static_cast<double>(Settings.CenterXY.Y)
            - static_cast<double>(Settings.RadiusInVoxels);
        const double MaxY64 = static_cast<double>(Settings.CenterXY.Y)
            + static_cast<double>(Settings.RadiusInVoxels);
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

        bool bSawNonFiniteDensity = false;
        int32 CellIndex = 0;
        for (int32 Z = 0; Z < OutGrid.NumZ; ++Z)
        {
            for (int32 Y = 0; Y < OutGrid.NumY; ++Y)
            {
                for (int32 X = 0; X < OutGrid.NumX; ++X)
                {
                    const FVector Sample = OutGrid.CellCenter(X, Y, Z);
                    const float Density = Generator.GetDensityAt(Sample.X, Sample.Y, Sample.Z);
                    if (!FMath::IsFinite(Density))
                    {
                        bSawNonFiniteDensity = true;
                    }

                    // MC convention in this codebase: negative is solid, positive is air.
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
        int32& OutLargestComponentCells)
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

            OutLargestComponentCells = FMath::Max(OutLargestComponentCells, ComponentCells);
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
        int32 LargestComponentCells = 0;
        FloodFillAir(Grid, Components, NumComponents, LargestComponentCells);

        int64 NumAir = 0;
        for (const uint8 bAir : Grid.Air)
        {
            NumAir += bAir != 0u ? 1 : 0;
        }

        InOutMetrics.NumSampled = Grid.CellCount;
        InOutMetrics.NumAir = NumAir;
        InOutMetrics.NumSolid = static_cast<int64>(Grid.CellCount) - NumAir;
        InOutMetrics.AirFraction = static_cast<float>(
            static_cast<double>(NumAir) / static_cast<double>(Grid.CellCount));
        InOutMetrics.NumAirComponents = NumComponents;
        InOutMetrics.LargestComponentShare = NumAir > 0
            ? static_cast<float>(static_cast<double>(LargestComponentCells)
                / static_cast<double>(NumAir))
            : 0.0f;

        TArray<int32> ClearanceValues;
        ClearanceValues.Reserve(static_cast<int32>(NumAir));
        int64 NumWalkable = 0;

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

    bool FullResolutionAirOnSegment(
        const UVoxelGenerator& Generator,
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
            const float Density = Generator.GetDensityAt(Sample.X, Sample.Y, Sample.Z);
            if (!FMath::IsFinite(Density) || !(Density > 0.0f))
            {
                return false;
            }
        }
        return true;
    }

    bool FullResolutionPathIsAir(
        const UVoxelGenerator& Generator,
        const FSampleGrid& Grid,
        const FVector& A,
        const FVector& B,
        const TArray<int32>& Path)
    {
        int32 StartX = 0;
        int32 StartY = 0;
        int32 StartZ = 0;
        DecodeIndex(Grid, Path[0], StartX, StartY, StartZ);
        int32 EndX = 0;
        int32 EndY = 0;
        int32 EndZ = 0;
        DecodeIndex(Grid, Path.Last(), EndX, EndY, EndZ);

        if (!FullResolutionAirOnSegment(Generator, A, Grid.CellCenter(StartX, StartY, StartZ)))
        {
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
                    Generator,
                    Grid.CellCenter(PreviousX, PreviousY, PreviousZ),
                    Grid.CellCenter(CurrentX, CurrentY, CurrentZ)))
            {
                return false;
            }
        }

        return FullResolutionAirOnSegment(Generator, Grid.CellCenter(EndX, EndY, EndZ), B);
    }

    bool FindCoarsePath(
        const FSampleGrid& Grid,
        int32 Start,
        int32 Goal,
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
                if (Grid.Air[Neighbor] != 0u && Parent[Neighbor] == -2)
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
}

FVoxelStrateMetrics VF_MeasureStrate(
    const UVoxelGenerator& Generator,
    const UVoxelStrateManager& Manager,
    int32 StrateIndex,
    const FVoxelStrateMeasureSettings& Settings)
{
    FVoxelStrateMetrics Result;
    VoxelStrateMeasurePrivate::FSampleGrid Grid;
    if (!VoxelStrateMeasurePrivate::BuildSampleGrid(
            Generator, Manager, StrateIndex, Settings, Grid, Result.RefusalReason))
    {
        return Result;
    }

    Result.ResolvedMarginVoxels = Grid.ResolvedMarginVoxels;
    Result.SampledMinZ = Grid.SampledMinZ;
    Result.SampledMaxZ = Grid.SampledMaxZ;
    VoxelStrateMeasurePrivate::BuildMetricsFromGrid(Grid, Settings, Result);
    Result.bValid = Result.NumSampled > 0;
    if (!Result.bValid)
    {
        Result.RefusalReason = TEXT("The measurement grid was empty.");
    }
    return Result;
}

bool VF_AreConnected(
    const UVoxelGenerator& Generator,
    const UVoxelStrateManager& Manager,
    int32 StrateIndex,
    const FVector& AVoxel,
    const FVector& BVoxel,
    const FVoxelStrateMeasureSettings& Settings,
    bool& bOutCoarseLied)
{
    bOutCoarseLied = false;
    if (!FMath::IsFinite(AVoxel.X) || !FMath::IsFinite(AVoxel.Y) || !FMath::IsFinite(AVoxel.Z)
        || !FMath::IsFinite(BVoxel.X) || !FMath::IsFinite(BVoxel.Y) || !FMath::IsFinite(BVoxel.Z))
    {
        return false;
    }

    VoxelStrateMeasurePrivate::FSampleGrid Grid;
    FString RefusalReason;
    if (!VoxelStrateMeasurePrivate::BuildSampleGrid(
            Generator, Manager, StrateIndex, Settings, Grid, RefusalReason))
    {
        return false;
    }

    int32 Start = -1;
    int32 Goal = -1;
    if (!VoxelStrateMeasurePrivate::FindCellForPoint(Grid, AVoxel, Start)
        || !VoxelStrateMeasurePrivate::FindCellForPoint(Grid, BVoxel, Goal))
    {
        return false;
    }

    TArray<int32> Path;
    if (!VoxelStrateMeasurePrivate::FindCoarsePath(Grid, Start, Goal, Path))
    {
        return false;
    }

    // The coarse route is evidence, not a pass. Re-check this one BFS route at one-voxel spacing
    // in the original voxel field. A single solid sample is the dangerous coarse false positive.
    if (!VoxelStrateMeasurePrivate::FullResolutionPathIsAir(
            Generator, Grid, AVoxel, BVoxel, Path))
    {
        bOutCoarseLied = true;
        return false;
    }
    return true;
}
