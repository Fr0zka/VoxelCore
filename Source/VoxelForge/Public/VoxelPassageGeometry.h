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
    constexpr float MaxStepHeightVoxels = 1.8f;              // 45 cm at 25 cm/voxel
    constexpr float SealSafetyMarginVoxels = 4.0f;
    constexpr float RoomRoundingVoxels = 1.5f;
    constexpr float LandingCarveBlendVoxels = 4.0f;
    // The origin landing is reserved for the same straight spine that the vertical-shaft
    // generator uses: OriginSpineRadius on each side, with a three-voxel / 0.75 m floor ledge
    // around it after the support patch's one-voxel inset. At the default radius (14 voxels) this
    // is a 28-voxel / 7 m future shaft diameter.
    constexpr float OriginShaftFloorClearanceVoxels = 4.0f;
    // Level tunnel apron outside each room-floor edge.  It covers the room/tunnel SDF blend and
    // keeps the ramp from starting while the route is still over the room's square support patch.
    constexpr float WalkableTunnelLandingApronVoxels = 6.0f; // 1.5 m
    // A level transition after a bend lets the route turn before its sloped segment begins.
    constexpr float WalkableTunnelTurnTransitionVoxels = 6.0f; // 1.5 m
    // Keep the tunnel-air post a half voxel above the authored support plane.  This is a numerical
    // ownership band: the floor writer owns the plane, while the air writer owns the capsule
    // volume above it even when two overlapping passage projections differ by a fraction.
    constexpr float WalkableTunnelFloorAirClearanceVoxels = 0.5f;
    // The graph-tunnel post owns the interior above its floor after terrain and passage-floor
    // writers have run. The support-floor predicate is also an exclusion mask for the air post,
    // so overlapping graph tubes cannot erase one another's walkable support band.
    constexpr float CaveTunnelAirCoreInsetVoxels = 0.5f;

    // A 15 degree ramp is comfortably below the 44 degree engine walkability ceiling.  The
    // latter is a scramble limit, not a sensible default for a tunnel the player walks through.
    constexpr float WalkableTunnelMaxGradientDegrees = 15.0f;
    constexpr float WalkableTunnelMaxGradient = 0.2679491924311227f; // tan(15 degrees)
    // Player-fit measurements use the engine's 44 degree walkability contract.  The 15 degree
    // value above remains the preferred authored tunnel ramp; this wider bound is only the
    // fallback threshold at which a room-to-room floor must become a terrace.
    constexpr float PlayerWalkableFloorMaxGradient = 0.9656887748070738f; // tan(44 degrees)

    // The walkability contract is about the floor carried by the tube, not only its centreline.
    // A tapered tube changes that floor by the radius delta, so the construction and the audit
    // use this same conservative floor delta.
    FORCEINLINE float TunnelFloorZ(const FVector& Centreline, float Radius)
    {
        return Centreline.Z - FMath::Abs(Radius);
    }

    // Vertical-shaft tree links are round air capsules, but the player-fit graph needs a
    // deterministic support plane to walk between shafts. Keep that plane on the first
    // seal-safe ledge shared by the whole strate, so every tree edge has the same walk level.
    // This is shaft-network support, not an origin landing connector.
    FORCEINLINE float VerticalShaftConnectorFloorThickness(float Radius)
    {
        return FMath::Min(
            LandingFloorThicknessVoxels,
            FMath::Max(0.5f, FMath::Abs(Radius) * 0.5f));
    }

    FORCEINLINE float VerticalShaftConnectorFloorZ(
        float StrateBottomZ,
        float InteriorBottomZ,
        float InteriorTopZ,
        float LedgeSpacing,
        float LedgeDepth,
        float ConnectorRadius)
    {
        const float FloorThickness = VerticalShaftConnectorFloorThickness(ConnectorRadius);
        const float MinimumFloor = InteriorBottomZ
            + FloorThickness + SealSafetyMarginVoxels;
        const float MaximumFloor = InteriorTopZ
            - PlayerHeightVoxels - SealSafetyMarginVoxels;
        if (!FMath::IsFinite(StrateBottomZ)
            || !FMath::IsFinite(InteriorBottomZ)
            || !FMath::IsFinite(InteriorTopZ)
            || !FMath::IsFinite(LedgeSpacing)
            || !FMath::IsFinite(LedgeDepth)
            || !FMath::IsFinite(MinimumFloor)
            || !FMath::IsFinite(MaximumFloor)
            || !(MaximumFloor > MinimumFloor))
        {
            return 0.5f * (InteriorBottomZ + InteriorTopZ);
        }

        if (LedgeSpacing > 0.0f && LedgeDepth > 0.0f)
        {
            const float RelativeBottom = InteriorBottomZ - StrateBottomZ;
            const int32 FirstPeriod = FMath::FloorToInt(RelativeBottom / LedgeSpacing);
            // The small fixed bound keeps malformed authored values from turning generation into
            // an unbounded search. Normal shaft strates find the first or second period.
            constexpr int32 MaxPeriods = 64;
            for (int32 PeriodOffset = 0; PeriodOffset < MaxPeriods; ++PeriodOffset)
            {
                const float PeriodStart = static_cast<float>(FirstPeriod + PeriodOffset)
                    * LedgeSpacing;
                const float Candidate = StrateBottomZ + PeriodStart + LedgeDepth;
                if (Candidate > MinimumFloor && Candidate < MaximumFloor)
                {
                    return Candidate;
                }
            }
        }

        return FMath::Clamp(
            0.5f * (InteriorBottomZ + InteriorTopZ), MinimumFloor, MaximumFloor);
    }

    FORCEINLINE float VerticalShaftConnectorCenterZ(
        float StrateBottomZ,
        float InteriorBottomZ,
        float InteriorTopZ,
        float LedgeSpacing,
        float LedgeDepth,
        float ConnectorRadius)
    {
        const float SafeRadius = FMath::Abs(ConnectorRadius);
        const float FloorThickness = VerticalShaftConnectorFloorThickness(SafeRadius);
        return VerticalShaftConnectorFloorZ(
            StrateBottomZ, InteriorBottomZ, InteriorTopZ,
            LedgeSpacing, LedgeDepth, SafeRadius)
            + SafeRadius - FloorThickness;
    }

    /** Add a flat, inset support band below one horizontal shaft-tree capsule. */
    FORCEINLINE void VF_ApplyVerticalShaftConnectorFloor(
        float& Density,
        float WorldX,
        float WorldY,
        float WorldZ,
        const FVector& Start,
        const FVector& End,
        float Radius,
        float FloorZ,
        float BaseDensity)
    {
        const float SafeRadius = FMath::Abs(Radius);
        const float FloorThickness = VerticalShaftConnectorFloorThickness(SafeRadius);
        const float FloorRadius = FMath::Max(0.5f, SafeRadius - 0.5f);
        if (!(SafeRadius > 0.0f)
            || !(BaseDensity > 0.0f)
            || !FMath::IsFinite(WorldX)
            || !FMath::IsFinite(WorldY)
            || !FMath::IsFinite(WorldZ)
            || !FMath::IsFinite(FloorZ)
            || !FMath::IsFinite(Start.X) || !FMath::IsFinite(Start.Y)
            || !FMath::IsFinite(End.X) || !FMath::IsFinite(End.Y))
        {
            return;
        }

        const FVector2D A(Start.X, Start.Y);
        const FVector2D Delta(End.X - Start.X, End.Y - Start.Y);
        const float LengthSquared = Delta.SizeSquared();
        const float T = LengthSquared > KINDA_SMALL_NUMBER
            ? FMath::Clamp(
                FVector2D::DotProduct(FVector2D(WorldX, WorldY) - A, Delta)
                    / LengthSquared,
                0.0f, 1.0f)
            : 0.0f;
        const FVector2D Closest = A + Delta * T;
        if ((FVector2D(WorldX, WorldY) - Closest).SizeSquared()
                > FMath::Square(FloorRadius))
        {
            return;
        }

        if (WorldZ <= FloorZ + KINDA_SMALL_NUMBER
            && WorldZ > FloorZ - FloorThickness)
        {
            Density = FMath::Max(Density, BaseDensity);
        }
    }

    // ApplyVerticalShaftConnectorAir runs before the generator's shared disturbance pass.  Keep
    // this one-bit, call-local hand-off so a bridge/ridge cannot refill the core it just opened.
    // It is thread-local state, never a per-voxel cache, and is reset around every density query.
    FORCEINLINE bool& VerticalShaftConnectorAirMarker()
    {
        static thread_local bool bMarked = false;
        return bMarked;
    }

    FORCEINLINE void ResetVerticalShaftConnectorAirMarker()
    {
        VerticalShaftConnectorAirMarker() = false;
    }

    /** Reassert the walkable air core above one horizontal shaft-tree floor band. */
    FORCEINLINE void VF_ApplyVerticalShaftConnectorAir(
        float& Density,
        float WorldX,
        float WorldY,
        float WorldZ,
        const FVector& Start,
        const FVector& End,
        float Radius,
        float FloorZ,
        float BaseDensity)
    {
        const float SafeRadius = FMath::Abs(Radius);
        // Leave a one-voxel wall band for the roughness/solid shell, while retaining more than
        // enough width for the 1.36-voxel player capsule and its support stencil.
        const float CoreRadius = FMath::Max(0.5f, SafeRadius - 1.0f);
        if (!(SafeRadius > 0.0f)
            || !(BaseDensity > 0.0f)
            || !FMath::IsFinite(WorldX)
            || !FMath::IsFinite(WorldY)
            || !FMath::IsFinite(WorldZ)
            || !FMath::IsFinite(FloorZ)
            || !FMath::IsFinite(Start.X) || !FMath::IsFinite(Start.Y)
            || !FMath::IsFinite(Start.Z)
            || !FMath::IsFinite(End.X) || !FMath::IsFinite(End.Y)
            || !FMath::IsFinite(End.Z))
        {
            return;
        }

        // The exact floor sample belongs to the air core's lower ownership edge. The floor
        // writer only owns the three voxels below FloorZ, so clearing from the half-voxel band
        // above it cannot remove support.
        if (WorldZ + KINDA_SMALL_NUMBER
            < FloorZ + WalkableTunnelFloorAirClearanceVoxels)
        {
            return;
        }

        const FVector Delta = End - Start;
        const float LengthSquared = Delta.SizeSquared();
        const float T = LengthSquared > KINDA_SMALL_NUMBER
            ? FMath::Clamp(FVector::DotProduct(FVector(WorldX, WorldY, WorldZ) - Start, Delta)
                / LengthSquared, 0.0f, 1.0f)
            : 0.0f;
        const FVector Closest = Start + Delta * T;
        if ((FVector(WorldX, WorldY, WorldZ) - Closest).SizeSquared()
            > FMath::Square(CoreRadius))
        {
            return;
        }

        // Internal convention: negative is air, positive is solid. This is intentionally a
        // post-ledge write; a partial shaft shelf must not cap the tree route's player-height
        // envelope after the capsule has already carved it.
        VerticalShaftConnectorAirMarker() = true;
        Density = FMath::Min(Density, -BaseDensity);
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

        // Keep a solid walking ledge around the future shaft, not merely a room whose wall is
        // tangent to it. The shaft diameter is 2 * OriginRadius; the extra four voxels leave a
        // three-voxel / 0.75 m floor margin after the support patch's one-voxel inset at the
        // authored 25 cm scale.
        Result.HalfWidth = FMath::Max(
            LandingHalfWidthForRadius(OriginRadius),
            FMath::Max(FMath::Abs(OriginRadius), 1.0f)
                + OriginShaftFloorClearanceVoxels);
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

    /** True when the exact MC lattice has at least one sample in a closed interval. */
    FORCEINLINE bool LatticeAxisHasSampleInInterval(
        float Min, float Max, float Origin, int32 Step)
    {
        if (Step <= 0 || !FMath::IsFinite(Min) || !FMath::IsFinite(Max)
            || !FMath::IsFinite(Origin) || Min > Max)
        {
            return false;
        }
        const float InvStep = 1.0f / static_cast<float>(Step);
        const int32 First = FMath::CeilToInt(
            (Min - Origin) * InvStep - 1.0e-4f);
        const int32 Last = FMath::FloorToInt(
            (Max - Origin) * InvStep + 1.0e-4f);
        return First <= Last;
    }

    FORCEINLINE bool OriginLandingRoomTouchesLattice(
        const FBox& VoxelBox, const FIntVector& LatticeOrigin, int32 Step,
        float StrateTopZ, float StrateBottomZ, float SealThickness, float OriginRadius)
    {
        if (!VoxelBox.IsValid) return false;
        const FOriginLandingGeometry Geometry = BuildOriginLandingGeometry(
            StrateTopZ, StrateBottomZ, SealThickness, OriginRadius);
        if (!Geometry.bValid) return false;
        const float Pad = LandingCarveBlendVoxels;
        return LatticeAxisHasSampleInInterval(
                   FMath::Max((float)VoxelBox.Min.X, -Geometry.HalfWidth - Pad),
                   FMath::Min((float)VoxelBox.Max.X, Geometry.HalfWidth + Pad),
                   (float)LatticeOrigin.X, Step)
            && LatticeAxisHasSampleInInterval(
                   FMath::Max((float)VoxelBox.Min.Y, -Geometry.HalfWidth - Pad),
                   FMath::Min((float)VoxelBox.Max.Y, Geometry.HalfWidth + Pad),
                   (float)LatticeOrigin.Y, Step)
            && LatticeAxisHasSampleInInterval(
                   FMath::Max((float)VoxelBox.Min.Z, Geometry.FloorZ - Pad),
                   FMath::Min((float)VoxelBox.Max.Z, Geometry.CeilingZ + Pad),
                   (float)LatticeOrigin.Z, Step);
    }

    FORCEINLINE bool OriginLandingFloorTouchesLattice(
        const FBox& VoxelBox, const FIntVector& LatticeOrigin, int32 Step,
        float StrateTopZ, float StrateBottomZ, float SealThickness, float OriginRadius)
    {
        if (!VoxelBox.IsValid) return false;
        const FOriginLandingGeometry Geometry = BuildOriginLandingGeometry(
            StrateTopZ, StrateBottomZ, SealThickness, OriginRadius);
        if (!Geometry.bValid) return false;

        constexpr float ProofPad = 1.0f;
        const float FloorHalfWidth = FMath::Max(
            Geometry.HalfWidth - 1.0f, 0.0f);
        return LatticeAxisHasSampleInInterval(
                   FMath::Max((float)VoxelBox.Min.X, -FloorHalfWidth - ProofPad),
                   FMath::Min((float)VoxelBox.Max.X, FloorHalfWidth + ProofPad),
                   (float)LatticeOrigin.X, Step)
            && LatticeAxisHasSampleInInterval(
                   FMath::Max((float)VoxelBox.Min.Y, -FloorHalfWidth - ProofPad),
                   FMath::Min((float)VoxelBox.Max.Y, FloorHalfWidth + ProofPad),
                   (float)LatticeOrigin.Y, Step)
            && LatticeAxisHasSampleInInterval(
                   FMath::Max((float)VoxelBox.Min.Z,
                              Geometry.FloorZ - Geometry.FloorThickness - ProofPad),
                   FMath::Min((float)VoxelBox.Max.Z, Geometry.FloorZ + ProofPad),
                   (float)LatticeOrigin.Z, Step);
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
