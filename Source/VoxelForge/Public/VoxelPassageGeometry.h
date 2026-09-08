// Shared dimensions and construction laws for walkable passages and origin landings.
//
// This header is deliberately free of UObjects.  The density path, the operator stack, the
// passage generator, and the headless measurements all include the same arithmetic so a geometry
// change cannot leave one of those paths with a different room or slope contract.

#pragma once

#include "CoreMinimal.h"

namespace VoxelPassageGeometry
{
    // Authored scale and player body used by the §6.5 landing calculation.
    constexpr float VoxelSizeMeters = 0.25f;
    constexpr float PlayerRadiusVoxels = 34.0f / 25.0f;
    constexpr float PlayerHalfHeightVoxels = 88.0f / 25.0f;
    constexpr float PlayerHeightVoxels = 2.0f * PlayerHalfHeightVoxels;
    constexpr float HeadroomVoxels = 4.0f;                 // 1 m
    constexpr float MinimumTurnFloorWidthVoxels = 12.0f;  // 3 m
    constexpr float MinimumLandingHalfWidthVoxels =
        MinimumTurnFloorWidthVoxels * 0.5f;
    constexpr float MinimumRoomHeightVoxels = 12.0f;       // 3 m
    constexpr float LandingFloorThicknessVoxels = 3.0f;
    constexpr float SealSafetyMarginVoxels = 4.0f;
    constexpr float RoomRoundingVoxels = 1.5f;
    constexpr float LandingCarveBlendVoxels = 4.0f;
    // Level tunnel apron outside each room-floor edge.  It covers the room/tunnel SDF blend and
    // keeps the ramp from starting while the route is still over the room's square support patch.
    constexpr float WalkableTunnelLandingApronVoxels = 6.0f; // 1.5 m
    // A level transition after a bend lets the route turn before its sloped segment begins.
    constexpr float WalkableTunnelTurnTransitionVoxels = 6.0f; // 1.5 m
    // Keep the tunnel-air post a half voxel above the authored support plane.  This is a numerical
    // ownership band: the floor writer owns the plane, while the air writer owns the capsule
    // volume above it even when two overlapping passage projections differ by a fraction.
    constexpr float WalkableTunnelFloorAirClearanceVoxels = 0.5f;

    // A 15 degree ramp is comfortably below the 44 degree engine walkability ceiling.  The
    // latter is a scramble limit, not a sensible default for a tunnel the player walks through.
    constexpr float WalkableTunnelMaxGradientDegrees = 15.0f;
    constexpr float WalkableTunnelMaxGradient = 0.2679491924311227f; // tan(15 degrees)

    // The network connector retains the §6.5 single-file dimensions: 2.5 m clear width and 3 m
    // clear height.  These values are in voxels because all passage descriptors are voxel-space.
    constexpr float ConnectorRadiusVoxels = 5.0f;
    constexpr float ConnectorHeightVoxels = 12.0f;
    constexpr float RootOverlapVoxels = 2.25f;

    // The walkability contract is about the floor carried by the tube, not only its centreline.
    // A tapered tube changes that floor by the radius delta, so the construction and the audit
    // use this same conservative floor delta.
    FORCEINLINE float TunnelFloorZ(const FVector& Centreline, float Radius)
    {
        return Centreline.Z - FMath::Abs(Radius);
    }

    FORCEINLINE float TunnelFloorGradient(
        const FVector& Start, float StartRadius,
        const FVector& End, float EndRadius)
    {
        const float HorizontalRun = FVector2D(
            End.X - Start.X, End.Y - Start.Y).Size();
        if (HorizontalRun <= KINDA_SMALL_NUMBER)
        {
            return FMath::Abs(TunnelFloorZ(End, EndRadius)
                - TunnelFloorZ(Start, StartRadius)) > KINDA_SMALL_NUMBER
                ? FLT_MAX : 0.0f;
        }
        return FMath::Abs(TunnelFloorZ(End, EndRadius)
            - TunnelFloorZ(Start, StartRadius)) / HorizontalRun;
    }

    FORCEINLINE float RequiredHorizontalRunForFloorDrop(
        float FirstFloorDrop, float SecondFloorDrop)
    {
        return 2.0f * FMath::Max(
            FMath::Abs(FirstFloorDrop), FMath::Abs(SecondFloorDrop))
            / WalkableTunnelMaxGradient;
    }

    /** Project a query into the nearest horizontal control segment of a walkable tunnel. */
    FORCEINLINE bool ProjectWalkableTunnelFloor(
        const TArray<FVector>& ControlPoints,
        const TArray<float>& ControlRadii,
        const FVector& Position,
        float& OutFloorZ,
        float& OutSupportRadius,
        int32* OutSegmentIndex = nullptr)
    {
        if (OutSegmentIndex != nullptr)
        {
            *OutSegmentIndex = INDEX_NONE;
        }
        if (ControlPoints.Num() < 2
            || ControlRadii.Num() != ControlPoints.Num())
        {
            return false;
        }

        const FVector2D QueryXY(Position.X, Position.Y);
        float BestDistanceSquared = FLT_MAX;
        bool bFoundSegment = false;
        for (int32 SegmentIndex = 0;
             SegmentIndex + 1 < ControlPoints.Num();
             ++SegmentIndex)
        {
            const FVector& A = ControlPoints[SegmentIndex];
            const FVector& B = ControlPoints[SegmentIndex + 1];
            const FVector2D AXY(A.X, A.Y);
            const FVector2D Delta = FVector2D(B.X, B.Y) - AXY;
            const float LengthSquared = Delta.SizeSquared();
            if (LengthSquared <= KINDA_SMALL_NUMBER)
            {
                continue;
            }

            const float T = FMath::Clamp(
                FVector2D::DotProduct(QueryXY - AXY, Delta) / LengthSquared,
                0.0f, 1.0f);
            const FVector2D ClosestXY = AXY + Delta * T;
            const float DistanceSquared = (QueryXY - ClosestXY).SizeSquared();
            if (DistanceSquared >= BestDistanceSquared)
            {
                continue;
            }

            const float StartRadius = FMath::Abs(ControlRadii[SegmentIndex]);
            const float EndRadius = FMath::Abs(ControlRadii[SegmentIndex + 1]);
            OutFloorZ = FMath::Lerp(
                TunnelFloorZ(A, StartRadius),
                TunnelFloorZ(B, EndRadius),
                T);
            OutSupportRadius = FMath::Max(
                FMath::Min(StartRadius, EndRadius) - 0.5f,
                PlayerRadiusVoxels);
            if (OutSegmentIndex != nullptr)
            {
                *OutSegmentIndex = SegmentIndex;
            }
            BestDistanceSquared = DistanceSquared;
            bFoundSegment = true;
        }

        return bFoundSegment
            && FMath::IsFinite(OutFloorZ)
            && FMath::IsFinite(OutSupportRadius)
            && OutSupportRadius > 0.0f
            && BestDistanceSquared <= FMath::Square(OutSupportRadius);
    }

    struct FOriginLandingGeometry
    {
        bool bValid = false;
        float FloorZ = 0.0f;
        float CeilingZ = 0.0f;
        float HalfWidth = 0.0f;
        float FloorThickness = LandingFloorThicknessVoxels;
    };

    FORCEINLINE float LandingHalfWidthForRadius(float Radius)
    {
        const float SafeRadius = FMath::Max(FMath::Abs(Radius), 1.0f);
        return FMath::Max(MinimumLandingHalfWidthVoxels, SafeRadius + 2.0f);
    }

    FORCEINLINE float LandingHeightForRadius(float Radius)
    {
        const float SafeRadius = FMath::Max(FMath::Abs(Radius), 1.0f);
        return FMath::Max(
            FMath::Max(MinimumRoomHeightVoxels, PlayerHeightVoxels + HeadroomVoxels),
            2.0f * SafeRadius + 2.0f);
    }

    /**
     * Build the one origin landing in a strate.
     *
     * `OriginRadius` is the old spine setting and becomes the room's authored mouth radius.  The
     * room dimensions therefore reuse the §6.5 formula exactly:
     *   half-width = max(7, radius + 2),
     *   height = max(12, capsule height + 4, 2 * radius + 2).
     * The floor is placed against the top of the safe interior interval.  This makes the first
     * landing the place reached from above, while every lower landing remains sealed above until
     * progression opens its inter-strate passage.
     */
    FORCEINLINE FOriginLandingGeometry BuildOriginLandingGeometry(
        float StrateTopZ, float StrateBottomZ, float SealThickness, float OriginRadius)
    {
        FOriginLandingGeometry Result;
        if (!(OriginRadius > 0.0f)
            || !FMath::IsFinite(StrateTopZ)
            || !FMath::IsFinite(StrateBottomZ)
            || !FMath::IsFinite(SealThickness)
            || !FMath::IsFinite(OriginRadius))
        {
            return Result;
        }

        Result.HalfWidth = LandingHalfWidthForRadius(OriginRadius);
        const float DesiredHeight = LandingHeightForRadius(OriginRadius);
        const float SafeSeal = FMath::Max(SealThickness, 0.0f);
        const float InnerBottom = StrateBottomZ + SafeSeal;
        const float InnerTop = StrateTopZ - SafeSeal;
        const float MinimumFloor = InnerBottom
            + Result.FloorThickness + SealSafetyMarginVoxels;
        const float MaximumFloor = InnerTop
            - DesiredHeight - SealSafetyMarginVoxels;
        if (!FMath::IsFinite(InnerBottom) || !FMath::IsFinite(InnerTop)
            || !(InnerTop > InnerBottom)
            || !FMath::IsFinite(MinimumFloor)
            || !FMath::IsFinite(MaximumFloor)
            || MaximumFloor < MinimumFloor)
        {
            Result.HalfWidth = 0.0f;
            return Result;
        }

        Result.FloorZ = MaximumFloor;
        Result.CeilingZ = Result.FloorZ + DesiredHeight;
        Result.bValid = true;
        return Result;
    }

    FORCEINLINE bool IntersectsClosedInterval(float MinA, float MaxA,
                                               float MinB, float MaxB)
    {
        return MaxA >= MinB && MinA <= MaxB;
    }

    /** Conservative influence box for the origin room's air carve. */
    FORCEINLINE bool OriginLandingRoomTouchesBox(
        const FBox& VoxelBox,
        float StrateTopZ, float StrateBottomZ, float SealThickness, float OriginRadius)
    {
        if (!VoxelBox.IsValid) return false;
        const FOriginLandingGeometry Geometry = BuildOriginLandingGeometry(
            StrateTopZ, StrateBottomZ, SealThickness, OriginRadius);
        if (!Geometry.bValid) return false;
        const float Pad = LandingCarveBlendVoxels;
        return IntersectsClosedInterval(
                   VoxelBox.Min.X, VoxelBox.Max.X,
                   -Geometry.HalfWidth - Pad, Geometry.HalfWidth + Pad)
            && IntersectsClosedInterval(
                   VoxelBox.Min.Y, VoxelBox.Max.Y,
                   -Geometry.HalfWidth - Pad, Geometry.HalfWidth + Pad)
            && IntersectsClosedInterval(
                   VoxelBox.Min.Z, VoxelBox.Max.Z,
                   Geometry.FloorZ - Pad, Geometry.CeilingZ + Pad);
    }

    /** Conservative influence box for the origin room's guaranteed solid floor. */
    FORCEINLINE bool OriginLandingFloorTouchesBox(
        const FBox& VoxelBox,
        float StrateTopZ, float StrateBottomZ, float SealThickness, float OriginRadius)
    {
        if (!VoxelBox.IsValid) return false;
        const FOriginLandingGeometry Geometry = BuildOriginLandingGeometry(
            StrateTopZ, StrateBottomZ, SealThickness, OriginRadius);
        if (!Geometry.bValid) return false;

        // One voxel is a proof margin, not a write margin. It keeps ClassifyTile conservative at
        // an exact lattice edge and mirrors the existing passage-floor guard.
        constexpr float ProofPad = 1.0f;
        const float FloorHalfWidth = FMath::Max(
            Geometry.HalfWidth - 1.0f, 0.0f);
        return IntersectsClosedInterval(
                   VoxelBox.Min.X, VoxelBox.Max.X,
                   -FloorHalfWidth - ProofPad, FloorHalfWidth + ProofPad)
            && IntersectsClosedInterval(
                   VoxelBox.Min.Y, VoxelBox.Max.Y,
                   -FloorHalfWidth - ProofPad, FloorHalfWidth + ProofPad)
            && IntersectsClosedInterval(
                   VoxelBox.Min.Z, VoxelBox.Max.Z,
                   Geometry.FloorZ - Geometry.FloorThickness - ProofPad,
                   Geometry.FloorZ + ProofPad);
    }

    /** Signed distance for a rounded origin room with a hard flat floor. */
    FORCEINLINE float OriginLandingRoomSDF(
        const FVector& Position, const FOriginLandingGeometry& Geometry)
    {
        if (!Geometry.bValid || Geometry.CeilingZ <= Geometry.FloorZ)
        {
            return FLT_MAX;
        }

        const FVector Center(0.0f, 0.0f,
            (Geometry.FloorZ + Geometry.CeilingZ) * 0.5f);
        const float Rounding = RoomRoundingVoxels;
        const float HalfZ = (Geometry.CeilingZ - Geometry.FloorZ) * 0.5f;
        const FVector HalfExtent(
            FMath::Max(Geometry.HalfWidth - Rounding, 0.25f),
            FMath::Max(Geometry.HalfWidth - Rounding, 0.25f),
            FMath::Max(HalfZ - Rounding, 0.25f));
        const FVector D(
            FMath::Abs(Position.X - Center.X) - HalfExtent.X,
            FMath::Abs(Position.Y - Center.Y) - HalfExtent.Y,
            FMath::Abs(Position.Z - Center.Z) - HalfExtent.Z);
        const FVector Outside(
            FMath::Max(D.X, 0.0f),
            FMath::Max(D.Y, 0.0f),
            FMath::Max(D.Z, 0.0f));
        const float Inside = FMath::Min(
            FMath::Max(D.X, FMath::Max(D.Y, D.Z)), 0.0f);
        const float RoundedBoxSDF = Outside.Size() + Inside - Rounding;

        // Below the floor is rock. Above it the rounded-box air volume is authoritative.
        return FMath::Max(RoundedBoxSDF, Geometry.FloorZ - Position.Z);
    }
}
