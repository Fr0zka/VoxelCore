#pragma once

#include "VoxelTypes.h"

namespace VoxelClipmapDesiredTiles
{
    struct FParameters
    {
        int32 ClipRadius = 3;
        int32 MaxClipLevel = 4;
        int32 RenderDistanceChunks = 0;
        bool bFarSheetRing = true;
        int32 FarSheetSpanLevels = 2;
    };

    FORCEINLINE int32 FloorDiv(int32 Value, int32 Divisor)
    {
        return Value >= 0 ? (Value / Divisor) : -(((-Value) + Divisor - 1) / Divisor);
    }

    FORCEINLINE FIntVector FloorDiv(const FIntVector& Value, int32 Divisor)
    {
        return FIntVector(FloorDiv(Value.X, Divisor), FloorDiv(Value.Y, Divisor),
                          FloorDiv(Value.Z, Divisor));
    }

    FORCEINLINE int32 CeilDiv(int32 Value, int32 Divisor)
    {
        return -FloorDiv(-Value, Divisor);
    }

    FORCEINLINE void AlignedRingBounds(const FIntVector& Center, int32 Radius, int32 Alignment,
                                       FIntVector& OutMin, FIntVector& OutMax)
    {
        const auto AlignAxis = [Radius, Alignment](int32 CenterAxis,
                                                  int32& OutAxisMin, int32& OutAxisMax)
        {
            const int32 BaseMin = CenterAxis - Radius;
            const int32 BaseEnd = CenterAxis + Radius + 1;
            OutAxisMin = FloorDiv(BaseMin, Alignment) * Alignment;
            OutAxisMax = CeilDiv(BaseEnd, Alignment) * Alignment - 1;
        };
        AlignAxis(Center.X, OutMin.X, OutMax.X);
        AlignAxis(Center.Y, OutMin.Y, OutMax.Y);
        AlignAxis(Center.Z, OutMin.Z, OutMax.Z);
    }

    FORCEINLINE int32 OuterShellRadius(const FParameters& Parameters, int32 Radius, int32 MaxLevel)
    {
        if (Parameters.RenderDistanceChunks <= 0)
        {
            return Radius;
        }
        return FMath::Max(Radius,
            (Parameters.RenderDistanceChunks + (1 << MaxLevel) - 1) >> MaxLevel);
    }

    FORCEINLINE void OuterShell(const FParameters& Parameters, int32 Radius, int32 MaxLevel,
                                int32& OutLevel, int32& OutRadius)
    {
        OutLevel = MaxLevel;
        OutRadius = OuterShellRadius(Parameters, Radius, MaxLevel);
        if (Parameters.bFarSheetRing && OutRadius > Radius)
        {
            OutLevel = MaxLevel + FMath::Clamp(Parameters.FarSheetSpanLevels, 1, 4);
            OutRadius = FMath::Max(1,
                (Parameters.RenderDistanceChunks + (1 << OutLevel) - 1) >> OutLevel);
        }
    }

    FORCEINLINE void ResolveVerticalBounds(
        int32 CenterChunkZ, int32 ViewDistanceUp, int32 ViewDistanceDown,
        int32 StrateViewMarginChunks, bool bClampToStrate, bool bHasStrate,
        int32 StrateTopChunkZ, int32 StrateBottomChunkZ,
        int32& OutMinChunkZ, int32& OutMaxChunkZ)
    {
        OutMinChunkZ = MIN_int32;
        OutMaxChunkZ = MAX_int32;
        if (!bClampToStrate)
        {
            return;
        }

        OutMinChunkZ = CenterChunkZ - ViewDistanceDown;
        OutMaxChunkZ = CenterChunkZ + ViewDistanceUp;
        if (bHasStrate)
        {
            OutMinChunkZ = StrateBottomChunkZ - StrateViewMarginChunks;
            OutMaxChunkZ = StrateTopChunkZ + StrateViewMarginChunks;
        }
    }

    FORCEINLINE void Build(const FIntVector& Center, int32 MinChunkZ, int32 MaxChunkZ,
                           const FParameters& Parameters, TArray<FVoxelTileKey>& OutTiles)
    {
        OutTiles.Reset();
        const int32 Radius = FMath::Max(1, Parameters.ClipRadius);
        const int32 MaxLevel = FMath::Clamp(Parameters.MaxClipLevel, 0, 8);
        const int32 OuterRadius = OuterShellRadius(Parameters, Radius, MaxLevel);
        int32 SheetLevel = MaxLevel;
        int32 SheetRadius = OuterRadius;
        OuterShell(Parameters, Radius, MaxLevel, SheetLevel, SheetRadius);
        const bool bSheetRing = SheetLevel > MaxLevel;
        const int32 MaxMCRingLevel = bSheetRing ? SheetLevel - 1 : MaxLevel;
        FIntVector PreviousRingMin = FIntVector::ZeroValue;
        FIntVector PreviousRingMax = FIntVector::ZeroValue;
        FIntVector MCRingMin = FIntVector::ZeroValue;
        FIntVector MCRingMax = FIntVector::ZeroValue;

        for (int32 Level = 0; Level <= MaxMCRingLevel; ++Level)
        {
            const int32 Pow = 1 << Level;
            const FIntVector CenterLevel = FloorDiv(Center, Pow);
            const int32 LevelRadius = (Level == MaxLevel && !bSheetRing)
                ? OuterRadius : Radius;

            // Every finer shell must end on a complete child-pair boundary so a coarser tile is
            // either wholly inside it or wholly outside it. The final MC ring uses the sheet's
            // grouping width when a sheet ring follows it; otherwise the ordinary LOD boundary
            // only needs pair alignment.
            const int32 RingAlignment = Level < MaxMCRingLevel
                ? 2
                : (bSheetRing ? (1 << (SheetLevel - MaxMCRingLevel)) : 1);
            FIntVector RingMin, RingMax;
            AlignedRingBounds(CenterLevel, LevelRadius, RingAlignment, RingMin, RingMax);
            if (Level == MaxMCRingLevel)
            {
                MCRingMin = RingMin;
                MCRingMax = RingMax;
            }

            int32 DeltaZMin = RingMin.Z - CenterLevel.Z;
            int32 DeltaZMax = RingMax.Z - CenterLevel.Z;
            if (LevelRadius > Radius && MinChunkZ != MIN_int32)
            {
                DeltaZMin = FMath::Max(DeltaZMin, FloorDiv(MinChunkZ, Pow) - CenterLevel.Z);
                DeltaZMax = FMath::Min(DeltaZMax, FloorDiv(MaxChunkZ, Pow) - CenterLevel.Z);
            }

            for (int32 DeltaZ = DeltaZMin; DeltaZ <= DeltaZMax; ++DeltaZ)
            for (int32 DeltaY = RingMin.Y - CenterLevel.Y;
                 DeltaY <= RingMax.Y - CenterLevel.Y; ++DeltaY)
            for (int32 DeltaX = RingMin.X - CenterLevel.X;
                 DeltaX <= RingMax.X - CenterLevel.X; ++DeltaX)
            {
                const FIntVector Tile = CenterLevel + FIntVector(DeltaX, DeltaY, DeltaZ);
                const int32 TileMinZ = Tile.Z << Level;
                const int32 TileMaxZ = ((Tile.Z + 1) << Level) - 1;
                if (TileMaxZ < MinChunkZ || TileMinZ > MaxChunkZ)
                {
                    continue;
                }

                if (Level > 0)
                {
                    const bool bCovered =
                        (2 * Tile.X >= PreviousRingMin.X)
                        && (2 * Tile.X + 1 <= PreviousRingMax.X)
                        && (2 * Tile.Y >= PreviousRingMin.Y)
                        && (2 * Tile.Y + 1 <= PreviousRingMax.Y)
                        && (2 * Tile.Z >= PreviousRingMin.Z)
                        && (2 * Tile.Z + 1 <= PreviousRingMax.Z);
                    if (bCovered)
                    {
                        continue;
                    }
                }

                OutTiles.Emplace(Tile, Level);
            }

            PreviousRingMin = RingMin;
            PreviousRingMax = RingMax;
        }

        if (bSheetRing)
        {
            const int32 SheetPow = 1 << SheetLevel;
            const FIntVector CenterSheet = FloorDiv(Center, SheetPow);
            const int32 LevelDelta = SheetLevel - MaxMCRingLevel;
            const int32 MCPerSheet = 1 << LevelDelta;

            int32 DeltaZMin = -SheetRadius;
            int32 DeltaZMax = SheetRadius;
            if (MinChunkZ != MIN_int32)
            {
                DeltaZMin = FMath::Max(DeltaZMin,
                    FloorDiv(MinChunkZ, SheetPow) - CenterSheet.Z);
                DeltaZMax = FMath::Min(DeltaZMax,
                    FloorDiv(MaxChunkZ, SheetPow) - CenterSheet.Z);
            }

            for (int32 DeltaZ = DeltaZMin; DeltaZ <= DeltaZMax; ++DeltaZ)
            for (int32 DeltaY = -SheetRadius; DeltaY <= SheetRadius; ++DeltaY)
            for (int32 DeltaX = -SheetRadius; DeltaX <= SheetRadius; ++DeltaX)
            {
                const FIntVector Tile = CenterSheet + FIntVector(DeltaX, DeltaY, DeltaZ);
                const int32 TileMinZ = Tile.Z << SheetLevel;
                const int32 TileMaxZ = ((Tile.Z + 1) << SheetLevel) - 1;
                if (TileMaxZ < MinChunkZ || TileMinZ > MaxChunkZ)
                {
                    continue;
                }

                const bool bCovered =
                    (Tile.X * MCPerSheet >= MCRingMin.X)
                    && ((Tile.X + 1) * MCPerSheet - 1 <= MCRingMax.X)
                    && (Tile.Y * MCPerSheet >= MCRingMin.Y)
                    && ((Tile.Y + 1) * MCPerSheet - 1 <= MCRingMax.Y)
                    && (Tile.Z * MCPerSheet >= MCRingMin.Z)
                    && ((Tile.Z + 1) * MCPerSheet - 1 <= MCRingMax.Z);
                if (!bCovered)
                {
                    OutTiles.Emplace(Tile, SheetLevel);
                }
            }
        }
    }
}
