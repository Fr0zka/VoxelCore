// VoxelCaveMorphology.cpp
// Hash-based room/tunnel generation and SDF evaluation.
//
// TWO-PHASE EVALUATION:
// Phase 1 — BuildChunkCache: collects rooms, builds backbone, resolves tunnels.
//           Called ONCE per chunk (~1 time per 32³ = 32,768 voxels).
// Phase 2 — EvaluateSDFCached: evaluates room/tunnel SDFs with distance culling.
//           Called PER VOXEL using the cached data.
//
// FEATURES:
// - Origin room: guaranteed finite landing room at (0,0) per strate — reserved for the spine
// - Hash-based rooms: ellipsoid, rounded box, and capsule shapes
// - Tunnels: tapered capsules with deterministic wandering control-point chains
// - Horizontal bias: tunnels prefer horizontal connections
// - Endpoint Z offset: tunnels enter rooms at different heights
// - Per-room/tunnel distance culling: skip SDFs that can't affect this voxel
//
// CROSS-CHUNK DETERMINISM (the invariant that prevents seams):
// The room/tunnel GRAPH must reconstruct IDENTICALLY in every chunk whose voxels a
// tunnel touches. Room geometry is a pure hash of its cell, so that half is trivially
// identical. The fragile half is the EXISTENCE decision (the nearest-neighbor
// backbone), which historically depended on the per-chunk window of collected rooms.
// We now separate two regions (see BuildChunkCache):
//   - COLLECT region (wide): every room whose NN-candidate set could influence a
//     tunnel touching this chunk. Connectivity is decided over THIS set, so the
//     decision is window-invariant.
//   - STORE region (tight): only rooms/tunnels that can actually reach a voxel in
//     this chunk are kept for the per-voxel loop, so hot-path cost stays low.

#include "VoxelCaveMorphology.h"
#include "VoxelDensityProfile.h"
#include "VoxelDensityPrimitives.h"
#include "VoxelStrateMeasure.h"
#include "VoxelTypes.h"          // Pour VOXEL_NOISE_SCALE, SmoothStep01
#include "VoxelStrateTypes.h"
#include "VoxelNoise.h"           // Pure FBM used by the slab landing query
#include "VoxelPassageGeometry.h"
#include "VoxelTerrainOpDefinition.h"

namespace
{
    // Density coordinates and cached cull radii are float-valued.  FVector is double-valued in
    // UE5, so using FVector::DistSquared here needlessly widens every candidate reject.  Keep the
    // cull in the same build-fixed float domain as the density query; the exact field output is
    // intentionally no longer required to match the pre-optimization double round-trip.
    FORCEINLINE float VF_FloatDistSquared(
        float X, float Y, float Z, const FVector& Center)
    {
        const float DX = X - static_cast<float>(Center.X);
        const float DY = Y - static_cast<float>(Center.Y);
        const float DZ = Z - static_cast<float>(Center.Z);
        return DX * DX + DY * DY + DZ * DZ;
    }

    FORCEINLINE float VF_DistanceSquaredToAabb(
        float X, float Y, float Z,
        const FVector& Min,
        const FVector& Max)
    {
        const float DX = X < static_cast<float>(Min.X)
            ? static_cast<float>(Min.X) - X
            : (X > static_cast<float>(Max.X) ? X - static_cast<float>(Max.X) : 0.0f);
        const float DY = Y < static_cast<float>(Min.Y)
            ? static_cast<float>(Min.Y) - Y
            : (Y > static_cast<float>(Max.Y) ? Y - static_cast<float>(Max.Y) : 0.0f);
        const float DZ = Z < static_cast<float>(Min.Z)
            ? static_cast<float>(Min.Z) - Z
            : (Z > static_cast<float>(Max.Z) ? Z - static_cast<float>(Max.Z) : 0.0f);
        return DX * DX + DY * DY + DZ * DZ;
    }

}

SIZE_T FChunkSDFCache::GetAllocatedSize() const
{
    return static_cast<SIZE_T>(GetAllocatedSizeBreakdown().TotalBytes());
}

VoxelDensityProfile::FCacheMemoryBreakdown FChunkSDFCache::GetAllocatedSizeBreakdown() const
{
    VoxelDensityProfile::FCacheMemoryBreakdown Breakdown;
    auto ArrayBytes = [](const auto& Array) -> uint64
    {
        return static_cast<uint64>(Array.GetAllocatedSize());
    };

    Breakdown.RoomsBytes = ArrayBytes(Rooms);
    Breakdown.RoomFloorJoinsBytes = ArrayBytes(RoomFloorJoins);
    Breakdown.TunnelsBytes = ArrayBytes(Tunnels);
    Breakdown.PitsBytes = ArrayBytes(Pits);
    Breakdown.ChimneysBytes = ArrayBytes(Chimneys);
    Breakdown.ColumnsBytes = ArrayBytes(Columns);
    Breakdown.SupportColumnEntriesBytes = ArrayBytes(SupportColumnEntries);
    Breakdown.SupportColumnsBytes = ArrayBytes(SupportColumns);
    for (const FTunnelSupportFloorColumn& Column : SupportColumns)
    {
        Breakdown.SupportColumnIntervalsBytes += ArrayBytes(Column.Intervals);
    }

    for (const FCachedTunnel& Tunnel : Tunnels)
    {
        Breakdown.TunnelsBytes += ArrayBytes(Tunnel.ControlPoints);
        Breakdown.TunnelsBytes += ArrayBytes(Tunnel.ControlRadii);
        Breakdown.TunnelsBytes += ArrayBytes(Tunnel.WorldControlPoints);
        Breakdown.TunnelsBytes += ArrayBytes(Tunnel.WorldControlRadii);
    }

    return Breakdown;
}

//=============================================================================
// INTERNAL: Full room data used during cache building only.
// The CellX/CellY fields are needed for tunnel pair hashing but NOT for
// per-voxel SDF evaluation, so they don't go into the cached FCachedRoom.
//=============================================================================
struct FBuildRoom
{
    FVector Center;         // World position of the room center
    float RadiusXY;         // Horizontal radius
    float RadiusZ;          // Vertical radius
    int32 CellX, CellY;    // Grid cell (needed for pair hash during tunnel decisions)
    uint32 Hash;            // Cell hash (for shape selection + tunnel property derivation)
    bool bIsOrigin;         // True for the origin room at (0,0)
    bool bStore;            // True if this room can affect a voxel in THIS chunk
                            // (collected for connectivity decisions either way).
    FVector PlayerFitPoint = FVector::ZeroVector;
    bool bHasPlayerFitPoint = false;
    bool bPlayerFitAttempted = false;
};

//=============================================================================
// INTERNAL: Shared hash-placement skeleton for the per-room baked features
// (pits / chimneys / columns). Squelette commun de placement par hash — les
// trois boucles de bake étaient des copies quasi identiques de ce motif.
//
// Rolls EXACTLY the hash chain the hand-written loops used (bit-identical):
//   H  = Mix(RoomHash ^ (SaltBase + i * SaltStep))  → density gate
//   H2 = Mix(H ^ Salt2)                             → XY offset (X: H2, Y: Mix(H2))
//   H3 = Mix(H2 ^ Salt3)                            → radius lerp [MinRadius, MaxRadius]
// Type-specific work (Z anchor, flare, bounds, struct fill) lives in the Emit
// lambda; it receives H3 so pits/chimneys can chain their 4th hash from it.
//=============================================================================
template <typename FEmit>
static void BakeRoomFeature(
    const FCachedRoom& CR,
    int32 MaxCount, float Density,
    uint32 SaltBase, uint32 SaltStep, uint32 Salt2, uint32 Salt3,
    float XYScale, float MinRadius, float MaxRadius,
    FEmit&& Emit)   // Emit(X, Y, Radius, H3)
{
    if (Density <= 0.0f) return;

    for (int32 i = 0; i < MaxCount; i++)
    {
        const uint32 H = VoxelHash::Mix(CR.Hash ^ (SaltBase + (uint32)i * SaltStep));
        if (VoxelHash::ToFloat01(H) > Density) continue;

        const uint32 H2 = VoxelHash::Mix(H ^ Salt2);
        const uint32 H3 = VoxelHash::Mix(H2 ^ Salt3);

        const float X = CR.Center.X + VoxelHash::ToFloatSigned(H2) * CR.RadiusXY * XYScale;
        const float Y = CR.Center.Y + VoxelHash::ToFloatSigned(VoxelHash::Mix(H2)) * CR.RadiusXY * XYScale;
        const float R = FMath::Lerp(MinRadius, MaxRadius, VoxelHash::ToFloat01(H3));

        Emit(X, Y, R, H3);
    }
}

    static void VF_FinalizeTunnelBroadPhaseBounds(FCachedTunnel& Tunnel, float BlendK)
    {
        const float BlendPad = FMath::Max(BlendK, 0.0f);
        // Detail modifiers run while CaveSDF is below 3*K, so the source
        // broad phase must cover that full downstream reach.
        const float SDFDetailPad = BlendPad * 3.0f;
        const float WorldFloorPad = FMath::Max(
            VoxelPassageGeometry::LandingFloorThicknessVoxels, BlendPad);

        if (Tunnel.ControlPoints.Num() >= 2
            && Tunnel.ControlRadii.Num() == Tunnel.ControlPoints.Num())
        {
            Tunnel.SDFCenterlineMin = Tunnel.ControlPoints[0];
            Tunnel.SDFCenterlineMax = Tunnel.ControlPoints[0];
            float MaxRadius = 0.0f;
            for (int32 Index = 0; Index < Tunnel.ControlPoints.Num(); ++Index)
            {
                const FVector& Point = Tunnel.ControlPoints[Index];
                Tunnel.SDFCenterlineMin.X = FMath::Min(Tunnel.SDFCenterlineMin.X, Point.X);
                Tunnel.SDFCenterlineMin.Y = FMath::Min(Tunnel.SDFCenterlineMin.Y, Point.Y);
                Tunnel.SDFCenterlineMin.Z = FMath::Min(Tunnel.SDFCenterlineMin.Z, Point.Z);
                Tunnel.SDFCenterlineMax.X = FMath::Max(Tunnel.SDFCenterlineMax.X, Point.X);
                Tunnel.SDFCenterlineMax.Y = FMath::Max(Tunnel.SDFCenterlineMax.Y, Point.Y);
                Tunnel.SDFCenterlineMax.Z = FMath::Max(Tunnel.SDFCenterlineMax.Z, Point.Z);
                MaxRadius = FMath::Max(MaxRadius, FMath::Abs(Tunnel.ControlRadii[Index]));
            }
            Tunnel.SDFInfluenceRadius = MaxRadius + SDFDetailPad;
        }
        else
        {
            Tunnel.SDFCenterlineMin = Tunnel.EndpointA;
            Tunnel.SDFCenterlineMax = Tunnel.EndpointA;
            Tunnel.SDFCenterlineMin.X = FMath::Min(Tunnel.SDFCenterlineMin.X, Tunnel.EndpointB.X);
            Tunnel.SDFCenterlineMin.Y = FMath::Min(Tunnel.SDFCenterlineMin.Y, Tunnel.EndpointB.Y);
            Tunnel.SDFCenterlineMin.Z = FMath::Min(Tunnel.SDFCenterlineMin.Z, Tunnel.EndpointB.Z);
            Tunnel.SDFCenterlineMax.X = FMath::Max(Tunnel.SDFCenterlineMax.X, Tunnel.EndpointB.X);
            Tunnel.SDFCenterlineMax.Y = FMath::Max(Tunnel.SDFCenterlineMax.Y, Tunnel.EndpointB.Y);
            Tunnel.SDFCenterlineMax.Z = FMath::Max(Tunnel.SDFCenterlineMax.Z, Tunnel.EndpointB.Z);
            float MaxRadius = FMath::Max(
                FMath::Abs(Tunnel.RadiusA), FMath::Abs(Tunnel.RadiusB));
            if (Tunnel.bHasMidpoint)
            {
                Tunnel.SDFCenterlineMin.X = FMath::Min(Tunnel.SDFCenterlineMin.X, Tunnel.Midpoint.X);
                Tunnel.SDFCenterlineMin.Y = FMath::Min(Tunnel.SDFCenterlineMin.Y, Tunnel.Midpoint.Y);
                Tunnel.SDFCenterlineMin.Z = FMath::Min(Tunnel.SDFCenterlineMin.Z, Tunnel.Midpoint.Z);
                Tunnel.SDFCenterlineMax.X = FMath::Max(Tunnel.SDFCenterlineMax.X, Tunnel.Midpoint.X);
                Tunnel.SDFCenterlineMax.Y = FMath::Max(Tunnel.SDFCenterlineMax.Y, Tunnel.Midpoint.Y);
                Tunnel.SDFCenterlineMax.Z = FMath::Max(Tunnel.SDFCenterlineMax.Z, Tunnel.Midpoint.Z);
                MaxRadius = FMath::Max(MaxRadius, FMath::Abs(Tunnel.RadiusMid));
            }
            Tunnel.SDFInfluenceRadius = MaxRadius + SDFDetailPad;
        }

        if (Tunnel.WorldControlPoints.Num() >= 2
            && Tunnel.WorldControlRadii.Num() == Tunnel.WorldControlPoints.Num())
        {
            Tunnel.WorldCenterlineMin = Tunnel.WorldControlPoints[0];
            Tunnel.WorldCenterlineMax = Tunnel.WorldControlPoints[0];
            float MaxRadius = 0.0f;
            for (int32 Index = 0; Index < Tunnel.WorldControlPoints.Num(); ++Index)
            {
                const FVector& Point = Tunnel.WorldControlPoints[Index];
                Tunnel.WorldCenterlineMin.X = FMath::Min(Tunnel.WorldCenterlineMin.X, Point.X);
                Tunnel.WorldCenterlineMin.Y = FMath::Min(Tunnel.WorldCenterlineMin.Y, Point.Y);
                Tunnel.WorldCenterlineMin.Z = FMath::Min(Tunnel.WorldCenterlineMin.Z, Point.Z);
                Tunnel.WorldCenterlineMax.X = FMath::Max(Tunnel.WorldCenterlineMax.X, Point.X);
                Tunnel.WorldCenterlineMax.Y = FMath::Max(Tunnel.WorldCenterlineMax.Y, Point.Y);
                Tunnel.WorldCenterlineMax.Z = FMath::Max(Tunnel.WorldCenterlineMax.Z, Point.Z);
                MaxRadius = FMath::Max(MaxRadius, FMath::Abs(Tunnel.WorldControlRadii[Index]));
            }
            Tunnel.WorldInfluenceRadius = MaxRadius + WorldFloorPad;
        }
        else
        {
            Tunnel.WorldCenterlineMin = Tunnel.SDFCenterlineMin;
            Tunnel.WorldCenterlineMax = Tunnel.SDFCenterlineMax;
            Tunnel.WorldInfluenceRadius = Tunnel.SDFInfluenceRadius
                - SDFDetailPad + WorldFloorPad;
        }
    }

    static void VF_FinalizeCacheBroadPhaseBounds(
        FChunkSDFCache& Cache,
        float BlendK)
    {
        for (int32 Index = 0; Index < Cache.Tunnels.Num(); ++Index)
        {
            FCachedTunnel& Tunnel = Cache.Tunnels[Index];
            VF_FinalizeTunnelBroadPhaseBounds(Tunnel, BlendK);
        }
    }
namespace
{
    // Runtime landing queries use the same pinned capsule dimensions and support rules as the
    // player-fit measurement, but they cannot call the editor-only flood fill.  Keep this local
    // evaluator deliberately source-only: the callback is pure density math supplied by the
    // archetype query, never a generator, manager, cache, or layout lookup.
    struct FVFPlayerCapsuleStencilRow
    {
        int32 RelativeZ = 0;
        TArray<FIntPoint> HorizontalOffsets;
    };

    bool VF_IsPointInsidePlayerCapsule(
        float RelativeZ,
        int32 OffsetX,
        int32 OffsetY,
        float Radius,
        float HalfHeight)
    {
        const float AxisHalfLength = FMath::Max(0.0f, HalfHeight - Radius);
        const float DistanceToAxis = FMath::Max(
            FMath::Abs(RelativeZ) - AxisHalfLength, 0.0f);
        const float DistanceSquared = static_cast<float>(OffsetX * OffsetX + OffsetY * OffsetY)
            + DistanceToAxis * DistanceToAxis;
        return DistanceSquared <= Radius * Radius + KINDA_SMALL_NUMBER;
    }

    bool VF_BuildPlayerFitStencil(
        const FVoxelStrateMeasureSettings& Settings,
        TArray<FVFPlayerCapsuleStencilRow>& OutRows,
        TArray<FIntPoint>& OutSupportOffsets,
        int32& OutCentreSupportIndex,
        int32& OutRequiredSupportCount)
    {
        OutRows.Reset();
        OutSupportOffsets.Reset();
        OutCentreSupportIndex = INDEX_NONE;
        OutRequiredSupportCount = 0;

        if (!FMath::IsFinite(Settings.PlayerCapsuleRadiusVoxels)
            || Settings.PlayerCapsuleRadiusVoxels <= 0.0f
            || !FMath::IsFinite(Settings.PlayerCapsuleHalfHeightVoxels)
            || Settings.PlayerCapsuleHalfHeightVoxels <= 0.0f
            || !FMath::IsFinite(Settings.PlayerMaxStepHeightMeters)
            || Settings.PlayerMaxStepHeightMeters < 0.0f
            || !FMath::IsFinite(Settings.PlayerWalkableFloorAngleDegrees)
            || Settings.PlayerWalkableFloorAngleDegrees < 0.0f
            || Settings.PlayerWalkableFloorAngleDegrees > 90.0f
            || !FMath::IsFinite(Settings.PlayerSupportPatchMinCoverageFraction)
            || Settings.PlayerSupportPatchMinCoverageFraction <= 0.0f
            || Settings.PlayerSupportPatchMinCoverageFraction > 1.0f)
        {
            return false;
        }

        const int32 HeightCells = FMath::CeilToInt(
            2.0f * Settings.PlayerCapsuleHalfHeightVoxels);
        const int32 MaxHorizontalOffset = FMath::CeilToInt(
            Settings.PlayerCapsuleRadiusVoxels);
        if (HeightCells <= 0 || HeightCells > 4096
            || MaxHorizontalOffset < 0 || MaxHorizontalOffset > 4096)
        {
            return false;
        }

        int64 NumStencilOffsets = 0;
        for (int32 RelativeZ = 0; RelativeZ < HeightCells; ++RelativeZ)
        {
            FVFPlayerCapsuleStencilRow& Row = OutRows.AddDefaulted_GetRef();
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
                    if (!VF_IsPointInsidePlayerCapsule(
                            CapsuleRelativeZ, OffsetX, OffsetY,
                            Settings.PlayerCapsuleRadiusVoxels,
                            Settings.PlayerCapsuleHalfHeightVoxels))
                    {
                        continue;
                    }
                    if (NumStencilOffsets >= (1ll << 20))
                    {
                        return false;
                    }
                    Row.HorizontalOffsets.Add(FIntPoint(OffsetX, OffsetY));
                    ++NumStencilOffsets;
                }
            }

            // Preserve the top partial row exactly as the measurement does.  A row with no
            // rounded-cap offsets still needs a centre sample for full-height clearance.
            if (Row.HorizontalOffsets.IsEmpty())
            {
                Row.HorizontalOffsets.Add(FIntPoint::ZeroValue);
            }
        }

        const int32 MaxSupportOffset = FMath::CeilToInt(
            Settings.PlayerCapsuleRadiusVoxels);
        for (int32 OffsetY = -MaxSupportOffset;
             OffsetY <= MaxSupportOffset;
             ++OffsetY)
        {
            for (int32 OffsetX = -MaxSupportOffset;
                 OffsetX <= MaxSupportOffset;
                 ++OffsetX)
            {
                if (static_cast<float>(OffsetX * OffsetX + OffsetY * OffsetY)
                    > FMath::Square(Settings.PlayerCapsuleRadiusVoxels)
                        + KINDA_SMALL_NUMBER)
                {
                    continue;
                }
                if (OffsetX == 0 && OffsetY == 0)
                {
                    OutCentreSupportIndex = OutSupportOffsets.Num();
                }
                OutSupportOffsets.Add(FIntPoint(OffsetX, OffsetY));
            }
        }

        if (OutRows.IsEmpty() || OutSupportOffsets.IsEmpty()
            || OutCentreSupportIndex == INDEX_NONE)
        {
            return false;
        }
        OutRequiredSupportCount = FMath::Clamp(
            FMath::CeilToInt(
                Settings.PlayerSupportPatchMinCoverageFraction
                    * static_cast<float>(OutSupportOffsets.Num())),
            1,
            OutSupportOffsets.Num());
        return true;
    }

    using FVFPlayerDensitySampler = TFunctionRef<float(float, float, float)>;

    bool VF_GetPlayerFitInteriorBounds(
        const FVoxelStrateMeasureSettings& Settings,
        float StrateTopZ,
        float StrateBottomZ,
        float BoundarySealThickness,
        float& OutInnerTop,
        float& OutInnerBottom)
    {
        if (!FMath::IsFinite(StrateTopZ)
            || !FMath::IsFinite(StrateBottomZ)
            || !FMath::IsFinite(BoundarySealThickness)
            || BoundarySealThickness < 0.0f
            || StrateTopZ <= StrateBottomZ)
        {
            return false;
        }

        int32 InteriorMarginVoxels = Settings.InteriorMarginVoxels;
        if (InteriorMarginVoxels < 0)
        {
            const double DerivedMargin = 2.0
                * static_cast<double>(BoundarySealThickness);
            if (!FMath::IsFinite(DerivedMargin)
                || DerivedMargin > static_cast<double>(INT32_MAX))
            {
                return false;
            }
            InteriorMarginVoxels = FMath::CeilToInt(static_cast<float>(DerivedMargin));
        }
        if (InteriorMarginVoxels < 0 || InteriorMarginVoxels > 4096)
        {
            return false;
        }

        OutInnerBottom = StrateBottomZ + static_cast<float>(InteriorMarginVoxels);
        OutInnerTop = StrateTopZ - static_cast<float>(InteriorMarginVoxels);
        return FMath::IsFinite(OutInnerTop)
            && FMath::IsFinite(OutInnerBottom)
            && OutInnerTop > OutInnerBottom;
    }

    bool VF_ValidatePlayerFitPose(
        const FVoxelStrateMeasureSettings& Settings,
        const FVector& CandidateFeetPoint,
        float StrateTopZ,
        float StrateBottomZ,
        float BoundarySealThickness,
        FVFPlayerDensitySampler SampleDensity,
        FVector& OutPoint)
    {
        OutPoint = FVector::ZeroVector;
        if (!FMath::IsFinite(CandidateFeetPoint.X)
            || !FMath::IsFinite(CandidateFeetPoint.Y)
            || !FMath::IsFinite(CandidateFeetPoint.Z)
            || !FMath::IsFinite(StrateTopZ)
            || !FMath::IsFinite(StrateBottomZ)
            || !FMath::IsFinite(BoundarySealThickness)
            || BoundarySealThickness < 0.0f
            || StrateTopZ <= StrateBottomZ)
        {
            return false;
        }

        TArray<FVFPlayerCapsuleStencilRow> StencilRows;
        TArray<FIntPoint> SupportOffsets;
        int32 CentreSupportIndex = INDEX_NONE;
        int32 RequiredSupportCount = 0;
        if (!VF_BuildPlayerFitStencil(
                Settings, StencilRows, SupportOffsets,
                CentreSupportIndex, RequiredSupportCount))
        {
            return false;
        }

        const double MaxStepHeightVoxelsReal =
            static_cast<double>(Settings.PlayerMaxStepHeightMeters)
            / static_cast<double>(FVoxelPlayerCapsuleConstants::VoxelSizeMeters);
        if (!FMath::IsFinite(MaxStepHeightVoxelsReal)
            || MaxStepHeightVoxelsReal < 0.0
            || MaxStepHeightVoxelsReal > 4096.0)
        {
            return false;
        }
        const int32 MaxDownwardSearchCells = FMath::CeilToInt(
            static_cast<float>(MaxStepHeightVoxelsReal));
        const float MaxStepHeightVoxels = static_cast<float>(MaxStepHeightVoxelsReal);
        const float MinimumWalkableNormalZ = FMath::Cos(FMath::DegreesToRadians(
            Settings.PlayerWalkableFloorAngleDegrees));

        TArray<float> SupportHeights;
        SupportHeights.SetNumUninitialized(SupportOffsets.Num());
        TArray<uint8> bSupported;
        bSupported.Init(0u, SupportOffsets.Num());
        int32 NumSupported = 0;
        for (int32 SupportIndex = 0; SupportIndex < SupportOffsets.Num(); ++SupportIndex)
        {
            const FIntPoint& Offset = SupportOffsets[SupportIndex];
            for (int32 DownwardCell = 0;
                 DownwardCell <= MaxDownwardSearchCells;
                 ++DownwardCell)
            {
                const float UpperZ = CandidateFeetPoint.Z
                    - static_cast<float>(DownwardCell);
                const float LowerZ = UpperZ - 1.0f;
                const float LowerDensity = SampleDensity(
                    CandidateFeetPoint.X + static_cast<float>(Offset.X),
                    CandidateFeetPoint.Y + static_cast<float>(Offset.Y),
                    LowerZ);
                const float UpperDensity = SampleDensity(
                    CandidateFeetPoint.X + static_cast<float>(Offset.X),
                    CandidateFeetPoint.Y + static_cast<float>(Offset.Y),
                    UpperZ);
                if (!FMath::IsFinite(LowerDensity)
                    || !FMath::IsFinite(UpperDensity)
                    || !(LowerDensity <= 0.0f)
                    || !(UpperDensity > 0.0f))
                {
                    continue;
                }

                const float Denominator = UpperDensity - LowerDensity;
                if (!FMath::IsFinite(Denominator) || Denominator <= 0.0f)
                {
                    continue;
                }
                const float Fraction = FMath::Clamp(
                    -LowerDensity / Denominator, 0.0f, 1.0f);
                const float SurfaceHeight = LowerZ + Fraction;
                const float CandidateBottom = CandidateFeetPoint.Z - 0.5f;
                if (!FMath::IsFinite(SurfaceHeight)
                    || SurfaceHeight > CandidateBottom + KINDA_SMALL_NUMBER
                    || SurfaceHeight < CandidateBottom - MaxStepHeightVoxels
                        - KINDA_SMALL_NUMBER)
                {
                    continue;
                }

                bSupported[SupportIndex] = 1u;
                SupportHeights[SupportIndex] = SurfaceHeight;
                ++NumSupported;
                break;
            }
        }

        if (NumSupported < RequiredSupportCount
            || bSupported[CentreSupportIndex] == 0u)
        {
            return false;
        }

        float SupportHeight = -FLT_MAX;
        int32 SupportCount = 0;
        for (int32 Index = 0; Index < SupportOffsets.Num(); ++Index)
        {
            if (bSupported[Index] == 0u) continue;
            ++SupportCount;
            SupportHeight = FMath::Max(SupportHeight, SupportHeights[Index]);
        }
        if (SupportCount < RequiredSupportCount || !FMath::IsFinite(SupportHeight))
        {
            return false;
        }

        float MaximumGradient = 0.0f;
        for (int32 First = 0; First < SupportOffsets.Num(); ++First)
        {
            if (bSupported[First] == 0u) continue;
            for (int32 Second = First + 1; Second < SupportOffsets.Num(); ++Second)
            {
                if (bSupported[Second] == 0u) continue;
                const float DX = static_cast<float>(
                    SupportOffsets[Second].X - SupportOffsets[First].X);
                const float DY = static_cast<float>(
                    SupportOffsets[Second].Y - SupportOffsets[First].Y);
                const float HorizontalDistance = FMath::Sqrt(DX * DX + DY * DY);
                if (HorizontalDistance <= KINDA_SMALL_NUMBER) continue;
                MaximumGradient = FMath::Max(
                    MaximumGradient,
                    FMath::Abs(SupportHeights[Second] - SupportHeights[First])
                        / HorizontalDistance);
            }
        }
        const float SupportNormalZ = 1.0f / FMath::Sqrt(
            1.0f + MaximumGradient * MaximumGradient);
        if (!FMath::IsFinite(SupportNormalZ)
            || SupportNormalZ + KINDA_SMALL_NUMBER < MinimumWalkableNormalZ)
        {
            return false;
        }

        // Re-evaluate every occupied stencil sample at the chosen support, so an overhang cannot
        // masquerade as a floor. The returned point is the free centre immediately above that
        // resolved support, which is the stable standing/footing point used by passage mouths.
        for (const FVFPlayerCapsuleStencilRow& Row : StencilRows)
        {
            const float SampleZ = SupportHeight
                + static_cast<float>(Row.RelativeZ) + 0.5f;
            for (const FIntPoint& Offset : Row.HorizontalOffsets)
            {
                const float Density = SampleDensity(
                    CandidateFeetPoint.X + static_cast<float>(Offset.X),
                    CandidateFeetPoint.Y + static_cast<float>(Offset.Y),
                    SampleZ);
                if (!FMath::IsFinite(Density) || !(Density > 0.0f))
                {
                    return false;
                }
            }
        }

        float InnerTop = 0.0f;
        float InnerBottom = 0.0f;
        if (!VF_GetPlayerFitInteriorBounds(
                Settings, StrateTopZ, StrateBottomZ, BoundarySealThickness,
                InnerTop, InnerBottom))
        {
            return false;
        }
        const float OutputZ = SupportHeight + 0.5f;
        const float OccupiedTop = SupportHeight
            + 2.0f * Settings.PlayerCapsuleHalfHeightVoxels;
        if (!FMath::IsFinite(InnerBottom) || !FMath::IsFinite(InnerTop)
            || !FMath::IsFinite(OutputZ) || !FMath::IsFinite(OccupiedTop)
            || OutputZ <= InnerBottom
            || OccupiedTop >= InnerTop)
        {
            return false;
        }

        OutPoint = FVector(CandidateFeetPoint.X, CandidateFeetPoint.Y, OutputZ);
        return !OutPoint.ContainsNaN()
            && FMath::IsFinite(OutPoint.X)
            && FMath::IsFinite(OutPoint.Y)
            && FMath::IsFinite(OutPoint.Z);
    }

    struct FVFRoomLandingSite
    {
        FVector Center = FVector::ZeroVector;
        float RadiusXY = 0.0f;
        float RadiusZ = 0.0f;
        uint32 Hash = 0;
        bool bOrigin = false;
    };

    FVector VF_ApplyCaveWarp(
        const FVector& WorldPoint,
        const FStrateGenerationParams& Params,
        uint32 Seed)
    {
        float EffectiveZ = WorldPoint.Z;
        if (Params.VerticalScale > 0.0f && Params.VerticalScale != 1.0f)
        {
            EffectiveZ = WorldPoint.Z / Params.VerticalScale;
        }

        FVector Warped(WorldPoint.X, WorldPoint.Y, EffectiveZ);
        if (Params.CaveWarpStrength <= 0.0f)
        {
            return Warped;
        }

        const float Frequency = Params.CaveWarpFrequency;
        const float Strength = Params.CaveWarpStrength;
        Warped.X += VoxelNoise::Perlin3D(
            WorldPoint.X * Frequency + VoxelHash::SeedOffset(Seed, 0.37f),
            WorldPoint.Y * Frequency + 1.3f,
            EffectiveZ * Frequency + 5.7f)
            * VOXEL_NOISE_SCALE * Strength;
        Warped.Y += VoxelNoise::Perlin3D(
            WorldPoint.X * Frequency + 7.1f,
            WorldPoint.Y * Frequency + VoxelHash::SeedOffset(Seed, 0.59f),
            EffectiveZ * Frequency + 2.3f)
            * VOXEL_NOISE_SCALE * Strength;
        Warped.Z += VoxelNoise::Perlin3D(
            WorldPoint.X * Frequency + 11.3f,
            WorldPoint.Y * Frequency + 9.7f,
            EffectiveZ * Frequency + VoxelHash::SeedOffset(Seed, 0.41f))
            * VOXEL_NOISE_SCALE * Strength;
        return Warped;
    }

    FVector VF_UnwarpCavePoint(
        const FVector& TargetSDFPoint,
        const FStrateGenerationParams& Params,
        uint32 Seed)
    {
        FVector WorldPoint = TargetSDFPoint;
        const float ZScale = Params.VerticalScale > 0.0f
            ? Params.VerticalScale : 1.0f;
        if (ZScale != 1.0f)
        {
            WorldPoint.Z *= ZScale;
        }
        if (Params.CaveWarpStrength <= 0.0f)
        {
            return WorldPoint;
        }

        // The default warp is a contraction at its authored frequency. A fixed eight-step
        // Newton/Picard correction is enough to put a world landing back on the exact cached SDF
        // point, while keeping the query allocation-free and deterministic.
        for (int32 Iteration = 0; Iteration < 8; ++Iteration)
        {
            const FVector Mapped = VF_ApplyCaveWarp(WorldPoint, Params, Seed);
            const FVector Error = TargetSDFPoint - Mapped;
            if (!Error.ContainsNaN()
                && FMath::IsFinite(Error.X)
                && FMath::IsFinite(Error.Y)
                && FMath::IsFinite(Error.Z))
            {
                WorldPoint.X += Error.X;
                WorldPoint.Y += Error.Y;
                WorldPoint.Z += Error.Z * ZScale;
            }
        }
        return WorldPoint;
    }

    float VF_RoomFloorZFromParts(
        const FVector& RoomCenter,
        float RoomRadiusXY,
        float RoomRadiusZ,
        uint32 RoomHash,
        uint32 WarpSeed,
        const FStrateGenerationParams& Params)
    {
        const float Roll = VoxelHash::ToFloat01(VoxelHash::Mix(RoomHash ^ 0xF100F2u));
        const float FloorCut = FMath::Lerp(
            FMath::Min(Params.RoomFloorCutMin, Params.RoomFloorCutMax),
            FMath::Max(Params.RoomFloorCutMin, Params.RoomFloorCutMax),
            Roll);
        float FloorZ = FloorCut < 1.0f
            ? RoomCenter.Z - RoomRadiusZ * FloorCut
            : RoomCenter.Z - RoomRadiusZ;
        if (FloorCut < 1.0f && Params.FloorReliefStrength > 0.0f)
        {
            // Match the evaluator's domain-warped XY input at the room centre. The local landing
            // query still searches a bounded vertical band for the support patch around this
            // estimate because the capsule covers neighbouring XY samples too.
            const float EffectiveZ = Params.VerticalScale > 0.0f
                ? RoomCenter.Z / Params.VerticalScale : RoomCenter.Z;
            float SampleX = RoomCenter.X;
            float SampleY = RoomCenter.Y;
            if (Params.CaveWarpStrength > 0.0f)
            {
                const float Frequency = Params.CaveWarpFrequency;
                const float Strength = Params.CaveWarpStrength;
                SampleX += VoxelNoise::Perlin3D(
                    RoomCenter.X * Frequency + VoxelHash::SeedOffset(WarpSeed, 0.37f),
                    RoomCenter.Y * Frequency + 1.3f,
                    EffectiveZ * Frequency + 5.7f)
                    * VOXEL_NOISE_SCALE * Strength;
                SampleY += VoxelNoise::Perlin3D(
                    RoomCenter.X * Frequency + 7.1f,
                    RoomCenter.Y * Frequency + VoxelHash::SeedOffset(WarpSeed, 0.59f),
                    EffectiveZ * Frequency + 2.3f)
                    * VOXEL_NOISE_SCALE * Strength;
            }

            const float Frequency = Params.FloorReliefFrequency;
            const float FloorSeed = static_cast<float>(
                VoxelHash::Mix(RoomHash ^ 0xF100F1u)) * 0.00001f;
            float Noise = FMath::PerlinNoise2D(FVector2D(
                SampleX * Frequency + FloorSeed,
                SampleY * Frequency + FloorSeed * 1.7f)) * 0.65f;
            Noise += FMath::PerlinNoise2D(FVector2D(
                SampleX * Frequency * 2.3f + FloorSeed * 3.1f,
                SampleY * Frequency * 2.3f + FloorSeed * 5.3f)) * 0.35f;
            FloorZ += Noise * VOXEL_NOISE_SCALE * Params.FloorReliefStrength;
        }
        // Keep this in the same (unwarped SDF) coordinate space as FCachedRoom::FloorCutZ. The
        // production caller warps the query position before EvaluateSDFCached, and the exact fit
        // predicate below samples that same warped field. This value is only a bounded search
        // anchor; it must not pre-apply the Z warp or the room would be shifted twice.
        return FloorZ;
    }

    float VF_RoomFloorZ(
        const FVFRoomLandingSite& Site,
        uint32 WarpSeed,
        const FStrateGenerationParams& Params)
    {
        return VF_RoomFloorZFromParts(
            Site.Center, Site.RadiusXY, Site.RadiusZ, Site.Hash, WarpSeed, Params);
    }

    float VF_EvaluateRoomLandingDensity(
        const FVFRoomLandingSite& Site,
        const FStrateGenerationParams& Params,
        uint32 Seed,
        float StrateTopZ,
        float StrateBottomZ,
        float WorldX,
        float WorldY,
        float WorldZ)
    {
        const FVector Position = VF_ApplyCaveWarp(
            FVector(WorldX, WorldY, WorldZ), Params, static_cast<uint32>(Seed));

        const uint32 ShapeHash = VoxelHash::Mix(Site.Hash ^ 0xDEADBEEFu);
        const float ShapeRoll = Site.bOrigin ? 0.0f : VoxelHash::ToFloat01(ShapeHash);
        const float BoxThreshold = 1.0f - Params.RoomShapeVariety * 0.5f;
        const float CapsuleThreshold = 1.0f - Params.RoomShapeVariety * 0.2f;
        float RoomSDF = 0.0f;
        if (ShapeRoll >= BoxThreshold && ShapeRoll < CapsuleThreshold)
        {
            RoomSDF = VoxelSDF::RoundedBox(
                Position,
                Site.Center,
                FVector(Site.RadiusXY * 0.8f, Site.RadiusXY * 0.8f, Site.RadiusZ * 0.8f),
                Site.RadiusXY * 0.25f);
        }
        else if (ShapeRoll >= CapsuleThreshold)
        {
            const float DirectionAngle = VoxelHash::ToFloat01(
                VoxelHash::Mix(Site.Hash ^ 0xCAFEBABEu)) * 2.0f * PI;
            const float StretchDistance = Site.RadiusXY * 0.7f;
            const FVector Direction(FMath::Cos(DirectionAngle), FMath::Sin(DirectionAngle), 0.0f);
            RoomSDF = VoxelSDF::Capsule(
                Position,
                Site.Center + Direction * StretchDistance,
                Site.Center - Direction * StretchDistance,
                FMath::Min(Site.RadiusXY * 0.6f, Site.RadiusZ));
        }
        else
        {
            RoomSDF = VoxelSDF::Ellipsoid(
                Position, Site.Center,
                FVector(Site.RadiusXY, Site.RadiusXY, Site.RadiusZ));
        }

        const float FloorCut = FMath::Lerp(
            FMath::Min(Params.RoomFloorCutMin, Params.RoomFloorCutMax),
            FMath::Max(Params.RoomFloorCutMin, Params.RoomFloorCutMax),
            VoxelHash::ToFloat01(VoxelHash::Mix(Site.Hash ^ 0xF100F2u)));
        if (FloorCut < 1.0f)
        {
            float FloorZ = Site.Center.Z - Site.RadiusZ * FloorCut;
            if (Params.FloorReliefStrength > 0.0f)
            {
                const float Frequency = Params.FloorReliefFrequency;
                const float FloorSeed = static_cast<float>(
                    VoxelHash::Mix(Site.Hash ^ 0xF100F1u)) * 0.00001f;
                float Noise = FMath::PerlinNoise2D(FVector2D(
                    Position.X * Frequency + FloorSeed,
                    Position.Y * Frequency + FloorSeed * 1.7f)) * 0.65f;
                Noise += FMath::PerlinNoise2D(FVector2D(
                    Position.X * Frequency * 2.3f + FloorSeed * 3.1f,
                    Position.Y * Frequency * 2.3f + FloorSeed * 5.3f)) * 0.35f;
                FloorZ += Noise * VOXEL_NOISE_SCALE * Params.FloorReliefStrength;
            }
            RoomSDF = VoxelSDF::SmoothMax(
                RoomSDF, FloorZ - Position.Z, Params.SDFBlendRadius * 0.35f);
        }

        float InternalDensity = RoomSDF < 0.0f ? -1.0f : 1.0f;
        VF_ApplyBoundarySeal(
            InternalDensity, WorldZ, StrateTopZ, StrateBottomZ,
            Params.BoundarySealThickness, 1.0f);
        // Query-facing density uses the same MC polarity as the measurement: positive is air.
        return -InternalDensity;
    }

    bool VF_FindPlayerFitPointForRoom(
        const FStrateGenerationParams& Params,
        const FVFRoomLandingSite& Site,
        float StrateTopZ,
        float StrateBottomZ,
        uint32 WarpSeed,
        FVector& OutPoint)
    {
        OutPoint = FVector::ZeroVector;
        const float FloorZ = VF_RoomFloorZ(Site, WarpSeed, Params);
        if (!FMath::IsFinite(FloorZ))
        {
            return false;
        }

        const float MaxStepHeightVoxels =
            FVoxelPlayerCapsuleConstants::MaxStepHeightMeters
                / FVoxelPlayerCapsuleConstants::VoxelSizeMeters;
        const float FloorReliefBound = FMath::Abs(Params.FloorReliefStrength)
            * VOXEL_NOISE_SCALE;
        const float CaveWarpBound = FMath::Abs(Params.CaveWarpStrength)
            * VOXEL_NOISE_SCALE;
        const int32 ReliefSearch = FMath::Clamp(
            FMath::CeilToInt(FloorReliefBound + CaveWarpBound) + 8,
            8,
            128);
        FVoxelStrateMeasureSettings FitSettings;
        const auto SampleRoom = [&](float X, float Y, float Z)
        {
            return VF_EvaluateRoomLandingDensity(
                Site, Params, WarpSeed,
                StrateTopZ, StrateBottomZ, X, Y, Z);
        };

        const int32 LocalExtent = 0;
        TArray<FIntPoint, TInlineAllocator<128>> LocalProbes;
        for (int32 LocalY = -LocalExtent; LocalY <= LocalExtent; LocalY += 2)
        {
            for (int32 LocalX = -LocalExtent; LocalX <= LocalExtent; LocalX += 2)
            {
                if (static_cast<float>(LocalX * LocalX + LocalY * LocalY)
                    > FMath::Square(Site.RadiusXY * 0.75f))
                {
                    continue;
                }
                LocalProbes.Add(FIntPoint(LocalX, LocalY));
            }
        }
        if ((LocalExtent & 1) != 0)
        {
            LocalProbes.Add(FIntPoint::ZeroValue);
        }
        LocalProbes.Sort([](const FIntPoint& A, const FIntPoint& B)
        {
            const int32 ADistanceSq = A.X * A.X + A.Y * A.Y;
            const int32 BDistanceSq = B.X * B.X + B.Y * B.Y;
            if (ADistanceSq != BDistanceSq)
            {
                return ADistanceSq < BDistanceSq;
            }
            return A.X != B.X ? A.X < B.X : A.Y < B.Y;
        });

        for (const FIntPoint& LocalProbe : LocalProbes)
        {
            const FVector CandidateFeetBase = VF_UnwarpCavePoint(
                FVector(
                    Site.Center.X + static_cast<float>(LocalProbe.X),
                    Site.Center.Y + static_cast<float>(LocalProbe.Y),
                    FloorZ + MaxStepHeightVoxels + 0.5f),
                Params,
                WarpSeed);
            for (int32 OffsetIndex = 0;
                 OffsetIndex <= ReliefSearch * 2;
                 ++OffsetIndex)
            {
                const int32 SignedOffset = OffsetIndex == 0
                    ? 0
                    : ((OffsetIndex & 1) != 0
                        ? -((OffsetIndex + 1) / 2)
                        : OffsetIndex / 2);
                if (FMath::Abs(SignedOffset) > ReliefSearch)
                {
                    continue;
                }
                const FVector CandidateFeet = CandidateFeetBase
                    + FVector(0.0f, 0.0f, static_cast<float>(SignedOffset));
                if (VF_ValidatePlayerFitPose(
                        FitSettings,
                        CandidateFeet,
                        StrateTopZ,
                        StrateBottomZ,
                        Params.BoundarySealThickness,
                        SampleRoom,
                        OutPoint))
                {
                    return true;
                }
            }
        }
        return false;
    }

    float VF_EvaluateSlabLandingDensity(
        const FSlabGenerationParams& Params,
        uint32 Seed,
        float WorldX,
        float WorldY,
        float WorldZ)
    {
        const float StrateHeight = Params.StrateTopWorldZ - Params.StrateBottomWorldZ;
        if (!(StrateHeight > 0.0f)) return -1.0f;

        const uint32 SeedU = Seed;
        const float FloorZ = Params.StrateBottomWorldZ
            + StrateHeight * Params.FloorRelativeHeight;
        float FloorNoise = 0.0f;
        if (Params.FloorRoughness > 0.0f)
        {
            FloorNoise = VoxelNoise::FBM(
                WorldX * Params.FloorRoughnessFrequency
                    + VoxelHash::SeedOffset(SeedU, 7.3f),
                WorldY * Params.FloorRoughnessFrequency
                    + VoxelHash::SeedOffset(SeedU, 11.1f),
                0.0f, 3) * VOXEL_NOISE_SCALE * Params.FloorRoughness;
        }
        const float FloorSurface = FloorZ + FloorNoise;

        const float CeilZ = Params.StrateBottomWorldZ
            + StrateHeight * Params.CeilingRelativeHeight;
        float CeilNoise = 0.0f;
        if (Params.CeilingRoughness > 0.0f)
        {
            const float RawNoise = VoxelNoise::FBM(
                WorldX * Params.CeilingRoughnessFrequency
                    + VoxelHash::SeedOffset(SeedU, 17.3f) + 1000.0f,
                WorldY * Params.CeilingRoughnessFrequency
                    + VoxelHash::SeedOffset(SeedU, 19.7f) + 2000.0f,
                3000.0f, 3) * VOXEL_NOISE_SCALE;
            CeilNoise = FMath::Abs(RawNoise) * Params.CeilingRoughness;
        }
        const float CeilSurface = FMath::Max(
            CeilZ - CeilNoise, FloorSurface + 2.0f);
        float InternalDensity = -FMath::Min(
            WorldZ - FloorSurface, CeilSurface - WorldZ);

        if (Params.ColumnDensity > 0.0f && Params.ColumnSpacing > 0.0f)
        {
            float ColumnSDF = FLT_MAX;
            const float Spacing = Params.ColumnSpacing;
            const int32 CellX = FMath::FloorToInt(WorldX / Spacing);
            const int32 CellY = FMath::FloorToInt(WorldY / Spacing);
            for (int32 DY = -1; DY <= 1; ++DY)
            {
                for (int32 DX = -1; DX <= 1; ++DX)
                {
                    const int32 ColumnX = CellX + DX;
                    const int32 ColumnY = CellY + DY;
                    const uint32 H = VoxelHash::Cell(
                        ColumnX, ColumnY, SeedU ^ 0xC01C01u);
                    if (VoxelHash::ToFloat01(H) > Params.ColumnDensity) continue;
                    const float JX = VoxelHash::ToFloat01(
                        VoxelHash::Mix(H ^ 0x12345678u));
                    const float JY = VoxelHash::ToFloat01(
                        VoxelHash::Mix(H ^ 0x9ABCDEF0u));
                    const float CentreX = (ColumnX + 0.15f + JX * 0.7f) * Spacing;
                    const float CentreY = (ColumnY + 0.15f + JY * 0.7f) * Spacing;
                    const float Radius = FMath::Lerp(
                        Params.ColumnMinRadius, Params.ColumnMaxRadius,
                        VoxelHash::ToFloat01(VoxelHash::Mix(H ^ 0xBEEFu)));
                    ColumnSDF = FMath::Min(
                        ColumnSDF,
                        FMath::Sqrt(FMath::Square(WorldX - CentreX)
                            + FMath::Square(WorldY - CentreY)) - Radius);
                }
            }
            if (ColumnSDF < 2.0f && ColumnSDF < FLT_MAX)
            {
                float Fill = FMath::Clamp(
                    (2.0f - ColumnSDF) / 4.0f, 0.0f, 1.0f);
                InternalDensity += SmoothStep01(Fill)
                    * Params.BaseDensity * 1.5f;
            }
        }

        VF_ApplyBoundarySeal(
            InternalDensity, WorldZ, Params.StrateTopWorldZ,
            Params.StrateBottomWorldZ, Params.BoundarySealThickness,
            Params.BaseDensity);
        return -InternalDensity;
    }

    float VF_EvaluateMazeLandingDensity(
        const FMazeGenerationParams& Params,
        uint32 Seed,
        float WorldX,
        float WorldY,
        float WorldZ)
    {
        const float CellSize = FMath::Max(Params.CellSize, 1.0f);
        const FVector Position(WorldX, WorldY, WorldZ);
        const uint32 SeedU = Seed ^ 0x4D617A65u;
        const int32 CellX = FMath::FloorToInt(WorldX / CellSize);
        const int32 CellY = FMath::FloorToInt(WorldY / CellSize);
        const int32 CellZ = FMath::FloorToInt(WorldZ / CellSize);
        auto NodeCenter = [CellSize](int32 X, int32 Y, int32 Z)
        {
            return FVector(
                (X + 0.5f) * CellSize,
                (Y + 0.5f) * CellSize,
                (Z + 0.5f) * CellSize);
        };
        const float Radius = FMath::Max(Params.CorridorRadius, 0.5f);
        float MazeSDF = FLT_MAX;
        for (int32 DZ = -1; DZ <= 0; ++DZ)
        {
            for (int32 DY = -1; DY <= 0; ++DY)
            {
                for (int32 DX = -1; DX <= 0; ++DX)
                {
                    const int32 X = CellX + DX;
                    const int32 Y = CellY + DY;
                    const int32 Z = CellZ + DZ;
                    const FVector A = NodeCenter(X, Y, Z);
                    if (VoxelMazeTopology::IsOpenEdge(
                            X, Y, Z, VoxelMazeTopology::EAxis::X,
                            SeedU, Params.BranchProbability, Params.Verticality))
                    {
                        MazeSDF = FMath::Min(
                            MazeSDF,
                            VoxelSDF::Capsule(
                                Position, A, NodeCenter(X + 1, Y, Z), Radius));
                    }
                    if (VoxelMazeTopology::IsOpenEdge(
                            X, Y, Z, VoxelMazeTopology::EAxis::Y,
                            SeedU, Params.BranchProbability, Params.Verticality))
                    {
                        MazeSDF = FMath::Min(
                            MazeSDF,
                            VoxelSDF::Capsule(
                                Position, A, NodeCenter(X, Y + 1, Z), Radius));
                    }
                    if (VoxelMazeTopology::IsOpenEdge(
                            X, Y, Z, VoxelMazeTopology::EAxis::Z,
                            SeedU, Params.BranchProbability, Params.Verticality))
                    {
                        MazeSDF = FMath::Min(
                            MazeSDF,
                            VoxelSDF::Capsule(
                                Position, A, NodeCenter(X, Y, Z + 1), Radius));
                    }
                }
            }
        }

        if (Params.SurfaceRoughness > 0.0f
            && MazeSDF < Radius + Params.SurfaceRoughness + 2.0f)
        {
            MazeSDF += VoxelNoise::FBM(
                WorldX * 0.12f, WorldY * 0.12f, WorldZ * 0.12f, 3)
                * VOXEL_NOISE_SCALE * Params.SurfaceRoughness;
        }

        float InternalDensity = Params.BaseDensity;
        if (MazeSDF < 2.0f)
        {
            float Carve = FMath::Clamp(
                (2.0f - MazeSDF) / 4.0f, 0.0f, 1.0f);
            InternalDensity -= SmoothStep01(Carve)
                * Params.BaseDensity * 2.0f;
        }
        VF_ApplyBoundarySeal(
            InternalDensity, WorldZ, Params.StrateTopWorldZ,
            Params.StrateBottomWorldZ, Params.BoundarySealThickness,
            Params.BaseDensity);
        return -InternalDensity;
    }

    float VF_EvaluateShaftLandingDensity(
        const FVerticalShaftParams& Params,
        uint32 Seed,
        float WorldX,
        float WorldY,
        float WorldZ)
    {
        const float Spacing = FMath::Max(Params.ShaftSpacing, 1.0f);
        const int32 BaseCellX = FMath::FloorToInt(WorldX / Spacing);
        const int32 BaseCellY = FMath::FloorToInt(WorldY / Spacing);
        const uint32 SeedU = Seed ^ 0x53686674u;
        float CaveSDF = FLT_MAX;
        FVector NearestCentre = FVector::ZeroVector;
        float NearestDistanceSq = FLT_MAX;
        float NearestRadius = 0.0f;
        for (int32 DY = -1; DY <= 1; ++DY)
        {
            for (int32 DX = -1; DX <= 1; ++DX)
            {
                const int32 CellX = BaseCellX + DX;
                const int32 CellY = BaseCellY + DY;
                const uint32 H = VoxelHash::Cell(CellX, CellY, SeedU);
                if (VoxelHash::ToFloat01(H) > Params.ShaftDensity) continue;
                const float JX = VoxelHash::ToFloat01(
                    VoxelHash::Mix(H ^ 0x12345678u));
                const float JY = VoxelHash::ToFloat01(
                    VoxelHash::Mix(H ^ 0x9ABCDEF0u));
                const float CentreX = (CellX + 0.15f + JX * 0.7f) * Spacing;
                const float CentreY = (CellY + 0.15f + JY * 0.7f) * Spacing;
                const float Radius = FMath::Lerp(
                    Params.ShaftMinRadius, Params.ShaftMaxRadius,
                    VoxelHash::ToFloat01(VoxelHash::Mix(H ^ 0xBEEFu)));
                const float DistanceSq = FMath::Square(WorldX - CentreX)
                    + FMath::Square(WorldY - CentreY);
                if (DistanceSq < NearestDistanceSq)
                {
                    NearestDistanceSq = DistanceSq;
                    NearestCentre = FVector(CentreX, CentreY, 0.0f);
                    NearestRadius = Radius;
                }
                CaveSDF = FMath::Min(
                    CaveSDF,
                    FMath::Sqrt(DistanceSq) - Radius);
            }
        }

        float InternalDensity = Params.BaseDensity;
        if (Params.SurfaceRoughness > 0.0f
            && CaveSDF < Params.SurfaceRoughness + 4.0f)
        {
            CaveSDF += VoxelNoise::FBM(
                WorldX * 0.1f, WorldY * 0.1f, WorldZ * 0.1f, 3)
                * VOXEL_NOISE_SCALE * Params.SurfaceRoughness;
        }
        if (CaveSDF < 2.0f)
        {
            float Carve = FMath::Clamp(
                (2.0f - CaveSDF) / 4.0f, 0.0f, 1.0f);
            InternalDensity -= SmoothStep01(Carve)
                * Params.BaseDensity * 2.0f;
        }

        if (Params.LedgeSpacing > 0.0f && Params.LedgeDepth > 0.0f
            && CaveSDF < 0.0f && NearestDistanceSq < FLT_MAX)
        {
            const float Phase = FMath::Frac(
                (WorldZ - Params.StrateBottomWorldZ) / Params.LedgeSpacing);
            const float BandT = FMath::Min(Phase, 1.0f - Phase)
                * Params.LedgeSpacing;
            if (BandT < Params.LedgeDepth
                && (WorldX - NearestCentre.X) + (WorldY - NearestCentre.Y) > 1.0e-3f)
            {
                const float Shelf = 1.0f - SmoothStep01(
                    BandT / Params.LedgeDepth);
                InternalDensity = FMath::Max(
                    InternalDensity, Shelf * Params.BaseDensity);
            }
        }
        VF_ApplyBoundarySeal(
            InternalDensity, WorldZ, Params.StrateTopWorldZ,
            Params.StrateBottomWorldZ, Params.BoundarySealThickness,
            Params.BaseDensity);
        return -InternalDensity;
    }

    // Find the nearest hash room's feature core without constructing a cache. The search is
    // deliberately bounded: an empty neighbourhood is an honest "no answer", not a guessed point.
    // The returned XY is the selected room centre; only a room core is a trustworthy sparse target.
    // Cherche le centre vertical de la salle hachée la plus proche sans construire de cache. La
    // recherche est bornée : un voisinage vide signifie "pas de réponse", jamais un point inventé.
    bool VF_FindNearestHashRoomLandingPoint(
        const FStrateGenerationParams& Params,
        int32 RoomSeed,
        int32 WarpSeed,
        float StrateTopZ,
        float StrateBottomZ,
        float WorldX,
        float WorldY,
        float MaxLateralSnap,
        FVector& OutPoint)
    {
        if (!FMath::IsFinite(StrateTopZ) || !FMath::IsFinite(StrateBottomZ)
            || !FMath::IsFinite(WorldX) || !FMath::IsFinite(WorldY)
            || !FMath::IsFinite(MaxLateralSnap) || MaxLateralSnap < 0.0f
            || StrateTopZ <= StrateBottomZ)
        {
            return false;
        }

        const float CellSize = Params.RoomSpacing;
        const float RadiusEnvelope = FMath::Max(Params.MinRoomRadius, Params.MaxRoomRadius);
        if (!FMath::IsFinite(CellSize) || !FMath::IsFinite(Params.RoomDensity)
            || !FMath::IsFinite(Params.MinRoomRadius) || !FMath::IsFinite(Params.MaxRoomRadius)
            || !FMath::IsFinite(Params.RoomHeightRatio)
            || !FMath::IsFinite(Params.BoundarySealThickness)
            || !FMath::IsFinite(Params.MaxTunnelLength)
            || CellSize <= 0.0f || Params.RoomDensity <= 0.0f
            || Params.MaxTunnelLength <= 0.0f
            || RadiusEnvelope <= 0.0f || Params.RoomHeightRatio <= 0.0f
            || Params.BoundarySealThickness < 0.0f)
        {
            return false;
        }

        // PLACEMENT CONTRACT: this envelope MUST remain identical to the room-center placement
        // code in VoxelCaveMorphology::BuildChunkCache below. The pure query deliberately does
        // not call that per-chunk cache builder during Initialize, so this is intentionally a
        // second copy.
        // VoxelForge.Determinism.PassageLandsInOpenSpace is the fixed ring test that catches drift.
        // The extra room-height buffer keeps the generated room body away from both seal bands.
        const float RoomZBuffer = RadiusEnvelope * Params.RoomHeightRatio;
        const float StrateMinZ = StrateBottomZ + Params.BoundarySealThickness + RoomZBuffer;
        const float StrateMaxZ = StrateTopZ - Params.BoundarySealThickness - RoomZBuffer;
        const float StrateRangeZ = StrateMaxZ - StrateMinZ;
        if (!FMath::IsFinite(StrateRangeZ) || StrateRangeZ <= 0.0f)
        {
            return false;
        }

        // A passage is placed within the configured reach from the spine. Start the search a little
        // farther than that reach, then expand deterministic rings until the nearest candidate is
        // proven; cap pathological asset values so Initialize cannot become an unbounded grid scan.
        const float SearchDistance = FMath::Max(CellSize * 2.0f,
            FMath::Max(Params.MaxTunnelLength, 0.0f));
        const float SearchCells = SearchDistance / CellSize;
        const int32 InitialSearchRadius = FMath::Min(
            32,
            FMath::Max(2, FMath::CeilToInt(SearchCells) + 1));
        constexpr int32 MaxSearchRadius = 32;

        const int32 BaseCellX = FMath::FloorToInt(WorldX / CellSize);
        const int32 BaseCellY = FMath::FloorToInt(WorldY / CellSize);

        float BestDistSq = FLT_MAX;
        float BestZ = 0.0f;
        float BestRoomX = 0.0f;
        float BestRoomY = 0.0f;
        float BestRadiusXY = 0.0f;
        float BestRadiusZ = 0.0f;
        uint32 BestHash = 0u;
        int32 BestCellX = 0;
        int32 BestCellY = 0;
        bool bFound = false;

        struct FRoomCandidate
        {
            FVFRoomLandingSite Site;
            float DistSq = FLT_MAX;
            int32 CellX = 0;
            int32 CellY = 0;
        };
        TArray<FRoomCandidate, TInlineAllocator<64>> Candidates;

        auto ConsiderCell = [&](int32 CellX, int32 CellY)
        {
            // Keep this roll byte-for-byte aligned with BuildChunkCache's room placement.
            const uint32 CellHash = VoxelHash::Cell(CellX, CellY, (uint32)RoomSeed);
            if (VoxelHash::ToFloat01(CellHash) >= Params.RoomDensity) return;

            const float JitterX = VoxelHash::ToFloat01(VoxelHash::Mix(CellHash ^ 0x12345678u));
            const float JitterY = VoxelHash::ToFloat01(VoxelHash::Mix(CellHash ^ 0x9ABCDEF0u));
            const float JitterZ = VoxelHash::ToFloat01(VoxelHash::Mix(CellHash ^ 0x55AA55AAu));

            const float RoomX = (CellX + 0.15f + JitterX * 0.7f) * CellSize;
            const float RoomY = (CellY + 0.15f + JitterY * 0.7f) * CellSize;
            const float RoomZ = StrateMinZ + JitterZ * FMath::Max(StrateRangeZ, 1.0f);
            const float DX = RoomX - WorldX;
            const float DY = RoomY - WorldY;
            const float DistSq = DX * DX + DY * DY;

            FRoomCandidate Candidate;
            Candidate.Site = FVFRoomLandingSite{
                FVector(RoomX, RoomY, RoomZ),
                0.0f,
                0.0f,
                CellHash,
                false };
            const float CandidateSizeFactor = VoxelHash::ToFloat01(
                VoxelHash::Mix(CellHash ^ 0xFEDCBA98u));
            Candidate.Site.RadiusXY = FMath::Lerp(
                FMath::Min(Params.MinRoomRadius, Params.MaxRoomRadius),
                FMath::Max(Params.MinRoomRadius, Params.MaxRoomRadius),
                CandidateSizeFactor);
            Candidate.Site.RadiusZ = Candidate.Site.RadiusXY * Params.RoomHeightRatio;
            Candidate.DistSq = DistSq;
            Candidate.CellX = CellX;
            Candidate.CellY = CellY;
            Candidates.Add(Candidate);

            const bool bCloser = DistSq < BestDistSq;
            const bool bTie = DistSq == BestDistSq
                && (CellX < BestCellX || (CellX == BestCellX && CellY < BestCellY));
            if (!bCloser && !bTie) return;

            BestDistSq = DistSq;
            BestZ = RoomZ;
            BestRoomX = RoomX;
            BestRoomY = RoomY;
            BestRadiusXY = RadiusEnvelope;
            const float SizeFactor = VoxelHash::ToFloat01(
                VoxelHash::Mix(CellHash ^ 0xFEDCBA98u));
            BestRadiusXY = FMath::Lerp(
                FMath::Min(Params.MinRoomRadius, Params.MaxRoomRadius),
                FMath::Max(Params.MinRoomRadius, Params.MaxRoomRadius),
                SizeFactor);
            BestRadiusZ = BestRadiusXY * Params.RoomHeightRatio;
            BestHash = CellHash;
            BestCellX = CellX;
            BestCellY = CellY;
            bFound = true;
        };

        auto ScanRing = [&](int32 Radius)
        {
            for (int32 CellY = BaseCellY - Radius; CellY <= BaseCellY + Radius; ++CellY)
            {
                for (int32 CellX = BaseCellX - Radius; CellX <= BaseCellX + Radius; ++CellX)
                {
                    if (FMath::Max(FMath::Abs(CellX - BaseCellX), FMath::Abs(CellY - BaseCellY)) != Radius)
                    {
                        continue;
                    }
                    ConsiderCell(CellX, CellY);
                }
            }
        };

        // The nearest possible room centre in any cell outside this square is at least this far
        // from the query point (the placement jitter is bounded to [0.15, 0.85] cell).
        auto IsNearestProven = [&](int32 Radius)
        {
            if (!bFound) return false;
            const float OutsideDistance = ((float)Radius + 0.15f) * CellSize;
            return BestDistSq < OutsideDistance * OutsideDistance;
        };

        for (int32 CellY = BaseCellY - InitialSearchRadius;
             CellY <= BaseCellY + InitialSearchRadius;
             ++CellY)
        {
            for (int32 CellX = BaseCellX - InitialSearchRadius;
                 CellX <= BaseCellX + InitialSearchRadius;
                 ++CellX)
            {
                ConsiderCell(CellX, CellY);
            }
        }

        bool bNearestProven = IsNearestProven(InitialSearchRadius);
        for (int32 Radius = InitialSearchRadius + 1;
             Radius <= MaxSearchRadius && !bNearestProven;
             ++Radius)
        {
            ScanRing(Radius);
            bNearestProven = IsNearestProven(Radius);
        }

        if (!bFound || !bNearestProven) return false;

        // The placement envelope above normally makes this automatic. Keep the explicit check so
        // malformed but finite parameters never turn a boundary value into an open-point claim.
        const float InnerTop = StrateTopZ - Params.BoundarySealThickness;
        const float InnerBottom = StrateBottomZ + Params.BoundarySealThickness;
        if (!FMath::IsFinite(BestZ) || BestZ <= InnerBottom || BestZ >= InnerTop)
        {
            return false;
        }

        // ⭐ Renvoyer le CENTRE de la salle, pas le XY demandé.
        //
        // La version précédente renvoyait FVector(WorldX, WorldY, BestZ) : le Z de la salle la plus
        // proche, au XY de l'APPELANT. Un balayage d'un million de seeds a montré que ce point tombe
        // HORS de la salle sélectionnée 92,8 % du temps — la requête trouvait une salle puis visait
        // à côté. Le centre est intérieur PAR CONSTRUCTION, et c'est aussi le point qui garde la
        // plus grande marge face au warp de production (qui déplace le champ d'environ 1,5 voxel :
        // décisif au bord d'une salle, négligeable en son centre).
        //
        // Return the ROOM CENTRE, not the requested XY. The previous version returned the nearest
        // room's Z at the CALLER's XY; a million-seed sweep put that point OUTSIDE the selected room
        // 92.8% of the time — it found a room and then aimed beside it. The centre is inside by
        // construction and holds the largest margin against the production cave warp (~1.5 voxels of
        // displacement: decisive at a room's edge, negligible at its centre).
        //
        // ⚠️ Le budget est borné volontairement. Les passages sont placés à une distance délibérée
        // de la spine (0,0) pour qu'un joueur perdu puisse les retrouver ; un snap non borné
        // dissoudrait cette propriété en silence. Hors budget ⇒ on décline, et l'appelant garde son
        // ancienne portée aléatoire.
        // The budget is bounded on purpose: passages sit at a deliberate distance from the (0,0)
        // spine so a lost player can find them, and an unbounded snap would dissolve that silently.
        // Over budget => decline, and the caller keeps its old random reach.
        const float MaxLateralSnapSq = MaxLateralSnap * MaxLateralSnap;
        const float MaxStepHeightVoxels =
            FVoxelPlayerCapsuleConstants::MaxStepHeightMeters
                / FVoxelPlayerCapsuleConstants::VoxelSizeMeters;
        // The room-centre estimate is analytic, but production evaluates the room after the
        // bounded XYZ cave warp.  A support stencil must be allowed to descend through that
        // displacement as well as the authored floor relief; otherwise a valid room can be
        // incorrectly downgraded to a random mouth whenever its shape bottom is more than one
        // player step below the nominal cut.
        const float FloorReliefBound = FMath::Abs(Params.FloorReliefStrength)
            * VOXEL_NOISE_SCALE;
        const float CaveWarpBound = FMath::Abs(Params.CaveWarpStrength)
            * VOXEL_NOISE_SCALE;
        const int32 ReliefSearch = FMath::Clamp(
            FMath::CeilToInt(FloorReliefBound + CaveWarpBound) + 8,
            8,
            128);
        FVoxelStrateMeasureSettings FitSettings;

        // The nearest room is preferred, but it is not allowed to turn a local relief/shape
        // quirk into a random landing. Test every deterministic candidate inside the lateral
        // budget, nearest first, and return only after the full source fit stencil succeeds.
        Candidates.Sort([](const FRoomCandidate& A, const FRoomCandidate& B)
        {
            if (A.DistSq != B.DistSq)
            {
                return A.DistSq < B.DistSq;
            }
            return A.CellX != B.CellX ? A.CellX < B.CellX : A.CellY < B.CellY;
        });

        for (const FRoomCandidate& Candidate : Candidates)
        {
            if (Candidate.DistSq > MaxLateralSnapSq)
            {
                continue;
            }

            const FVFRoomLandingSite& Site = Candidate.Site;
            const float FloorZ = VF_RoomFloorZ(Site, static_cast<uint32>(WarpSeed), Params);
            const auto SampleRoom = [&](float X, float Y, float Z)
            {
                return VF_EvaluateRoomLandingDensity(
                    Site, Params, static_cast<uint32>(WarpSeed),
                    StrateTopZ, StrateBottomZ, X, Y, Z);
            };
            // Cave warp can move the unwarped room centre close to a wall. Probe a deterministic
            // coarse disk around that centre, nearest first, while leaving the final decision to
            // the exact capsule/stencil test. The disk stays well inside the authored room so it
            // cannot silently turn an adjacent room into this room's landing.
            const int32 LocalExtent = FMath::Max(
                2,
                FMath::FloorToInt(FMath::Min(
                    Site.RadiusXY * 0.75f,
                    FMath::Max(CaveWarpBound + 4.0f, 8.0f))));
            TArray<FIntPoint, TInlineAllocator<128>> LocalProbes;
            for (int32 LocalY = -LocalExtent; LocalY <= LocalExtent; LocalY += 2)
            {
                for (int32 LocalX = -LocalExtent; LocalX <= LocalExtent; LocalX += 2)
                {
                    if (static_cast<float>(LocalX * LocalX + LocalY * LocalY)
                        > FMath::Square(Site.RadiusXY * 0.75f))
                    {
                        continue;
                    }
                    LocalProbes.Add(FIntPoint(LocalX, LocalY));
                }
            }
            if ((LocalExtent & 1) != 0)
            {
                LocalProbes.Add(FIntPoint::ZeroValue);
            }
            LocalProbes.Sort([](const FIntPoint& A, const FIntPoint& B)
            {
                const int32 ADistanceSq = A.X * A.X + A.Y * A.Y;
                const int32 BDistanceSq = B.X * B.X + B.Y * B.Y;
                if (ADistanceSq != BDistanceSq)
                {
                    return ADistanceSq < BDistanceSq;
                }
                return A.X != B.X ? A.X < B.X : A.Y < B.Y;
            });

            for (const FIntPoint& LocalProbe : LocalProbes)
            {
                const FVector CandidateFeetBase = VF_UnwarpCavePoint(
                    FVector(
                    Site.Center.X + static_cast<float>(LocalProbe.X),
                        Site.Center.Y + static_cast<float>(LocalProbe.Y),
                        FloorZ + MaxStepHeightVoxels + 0.5f),
                    Params,
                    static_cast<uint32>(WarpSeed));

                // The production room floor includes bounded hash relief and the remaining local
                // warp variation. Search a deterministic vertical band around the analytic anchor
                // so valid rooms are never downgraded merely because the centre estimate is off by
                // a few voxels.
                for (int32 OffsetIndex = 0;
                     OffsetIndex <= ReliefSearch * 2;
                     ++OffsetIndex)
                {
                    const int32 SignedOffset = OffsetIndex == 0
                        ? 0
                        : ((OffsetIndex & 1) != 0
                            ? -((OffsetIndex + 1) / 2)
                            : OffsetIndex / 2);
                    if (FMath::Abs(SignedOffset) > ReliefSearch)
                    {
                        continue;
                    }
                    const FVector CandidateFeet = CandidateFeetBase
                        + FVector(0.0f, 0.0f, static_cast<float>(SignedOffset));
                    if (VF_ValidatePlayerFitPose(
                            FitSettings,
                            CandidateFeet,
                            StrateTopZ,
                            StrateBottomZ,
                            Params.BoundarySealThickness,
                            SampleRoom,
                            OutPoint))
                    {
                        return true;
                    }
                }
            }
        }
        return false;
    }

    // The slab source is an XY height band. Evaluate the same two pure height fields as
    // GetSlabDensity, then choose their midpoint after intersecting the seal-free interior.
    // La source slab est une bande de hauteurs XY : on recalcule les deux champs purs comme
    // GetSlabDensity, puis on prend leur milieu après intersection avec l'intérieur sans seal.
    bool VF_SuggestSlabLandingPoint(
        const FSlabGenerationParams& Params,
        int32 Seed,
        float StrateTopZ,
        float StrateBottomZ,
        float WorldX,
        float WorldY,
        float MaxLateralSnap,
        FVector& OutPoint)
    {
        if (!FMath::IsFinite(StrateTopZ) || !FMath::IsFinite(StrateBottomZ)
            || !FMath::IsFinite(WorldX) || !FMath::IsFinite(WorldY)
            || !FMath::IsFinite(MaxLateralSnap) || MaxLateralSnap < 0.0f
            || StrateTopZ <= StrateBottomZ)
        {
            return false;
        }

        if (!FMath::IsFinite(Params.FloorRelativeHeight)
            || !FMath::IsFinite(Params.CeilingRelativeHeight)
            || !FMath::IsFinite(Params.FloorRoughness)
            || !FMath::IsFinite(Params.FloorRoughnessFrequency)
            || !FMath::IsFinite(Params.CeilingRoughness)
            || !FMath::IsFinite(Params.CeilingRoughnessFrequency)
            || !FMath::IsFinite(Params.ColumnDensity)
            || !FMath::IsFinite(Params.ColumnMinRadius)
            || !FMath::IsFinite(Params.ColumnMaxRadius)
            || !FMath::IsFinite(Params.ColumnSpacing)
            || !FMath::IsFinite(Params.BoundarySealThickness)
            || !FMath::IsFinite(Params.BaseDensity)
            || Params.BoundarySealThickness < 0.0f)
        {
            return false;
        }

        const float StrateHeight = StrateTopZ - StrateBottomZ;
        const uint32 SeedU = (uint32)Seed;

        const float FloorZ = StrateBottomZ + StrateHeight * Params.FloorRelativeHeight;
        float FloorNoise = 0.0f;
        if (Params.FloorRoughness > 0.0f)
        {
            const float FF = Params.FloorRoughnessFrequency;
            FloorNoise = VoxelNoise::FBM(
                WorldX * FF + VoxelHash::SeedOffset(SeedU, 7.3f),
                WorldY * FF + VoxelHash::SeedOffset(SeedU, 11.1f),
                0.0f, 3
            ) * VOXEL_NOISE_SCALE * Params.FloorRoughness;
        }
        const float FloorSurface = FloorZ + FloorNoise;

        const float CeilZ = StrateBottomZ + StrateHeight * Params.CeilingRelativeHeight;
        float CeilNoise = 0.0f;
        if (Params.CeilingRoughness > 0.0f)
        {
            const float CF = Params.CeilingRoughnessFrequency;
            const float RawNoise = VoxelNoise::FBM(
                WorldX * CF + VoxelHash::SeedOffset(SeedU, 17.3f) + 1000.0f,
                WorldY * CF + VoxelHash::SeedOffset(SeedU, 19.7f) + 2000.0f,
                3000.0f, 3
            ) * VOXEL_NOISE_SCALE;
            CeilNoise = FMath::Abs(RawNoise) * Params.CeilingRoughness;
        }
        const float CeilSurface = FMath::Max(CeilZ - CeilNoise, FloorSurface + 2.0f);

        // VF_ApplyBoundarySeal's bands are [Top-Thickness, Top) and
        // (Bottom, Bottom+Thickness]. The midpoint below stays strictly inside both limits.
        const float InnerTop = StrateTopZ - Params.BoundarySealThickness;
        const float InnerBottom = StrateBottomZ + Params.BoundarySealThickness;
        const float OpenBottom = FMath::Max(FloorSurface, InnerBottom);
        const float OpenTop = FMath::Min(CeilSurface, InnerTop);
        if (!FMath::IsFinite(OpenBottom) || !FMath::IsFinite(OpenTop) || OpenTop <= OpenBottom)
        {
            return false;
        }

        // Slab columns are infinite-height. If one reaches this XY, no Z in the slab void is a
        // confident pre-passage landing point, so report "unanswerable" instead of guessing.
        if (Params.ColumnDensity > 0.0f && Params.ColumnSpacing > 0.0f)
        {
            const float Spacing = Params.ColumnSpacing;
            const int32 ColumnCellX = FMath::FloorToInt(WorldX / Spacing);
            const int32 ColumnCellY = FMath::FloorToInt(WorldY / Spacing);

            for (int32 DY = -1; DY <= 1; ++DY)
            {
                for (int32 DX = -1; DX <= 1; ++DX)
                {
                    const int32 CellX = ColumnCellX + DX;
                    const int32 CellY = ColumnCellY + DY;
                    const uint32 H = VoxelHash::Cell(CellX, CellY, SeedU ^ 0xC01C01u);
                    if (VoxelHash::ToFloat01(H) > Params.ColumnDensity) continue;

                    const float JX = VoxelHash::ToFloat01(VoxelHash::Mix(H ^ 0x12345678u));
                    const float JY = VoxelHash::ToFloat01(VoxelHash::Mix(H ^ 0x9ABCDEF0u));
                    const float ColumnX = (CellX + 0.15f + JX * 0.7f) * Spacing;
                    const float ColumnY = (CellY + 0.15f + JY * 0.7f) * Spacing;
                    const float Radius = FMath::Lerp(Params.ColumnMinRadius, Params.ColumnMaxRadius,
                        VoxelHash::ToFloat01(VoxelHash::Mix(H ^ 0xBEEFu)));
                    const float ColumnDX = WorldX - ColumnX;
                    const float ColumnDY = WorldY - ColumnY;
                    if (FMath::Sqrt(ColumnDX * ColumnDX + ColumnDY * ColumnDY) - Radius < 2.0f)
                    {
                        return false;
                    }
                }
            }
        }

        // The old query returned the centre of the void band.  That is open air, but it is not a
        // player pose: the body must stand on the floor.  Re-run the pure slab source through the
        // same support/capsule stencil used by the measurement and return the resolved feet row.
        FSlabGenerationParams SourceParams = Params;
        SourceParams.StrateTopWorldZ = StrateTopZ;
        SourceParams.StrateBottomWorldZ = StrateBottomZ;
        FVoxelStrateMeasureSettings FitSettings;
        const float MaxStepHeightVoxels =
            FVoxelPlayerCapsuleConstants::MaxStepHeightMeters
                / FVoxelPlayerCapsuleConstants::VoxelSizeMeters;
        const FVector CandidateFeet(
            WorldX, WorldY, OpenBottom + MaxStepHeightVoxels + 0.5f);
        const auto SampleSlab = [&](float X, float Y, float Z)
        {
            return VF_EvaluateSlabLandingDensity(
                SourceParams, static_cast<uint32>(Seed), X, Y, Z);
        };
        return VF_ValidatePlayerFitPose(
            FitSettings,
            CandidateFeet,
            StrateTopZ,
            StrateBottomZ,
            SourceParams.BoundarySealThickness,
            SampleSlab,
            OutPoint);
    }

    // Maze corridors are lattice edges. Only a horizontal edge is a useful landing source: a
    // vertical edge can prove air, but cannot prove a place to stand. The edge set and Z levels
    // below use the same origin-directed tree + loop contract as GetMazeDensity, with no cache.
    bool VF_SuggestMazeLandingPoint(
        const FMazeGenerationParams& Params,
        int32 Seed,
        float StrateTopZ,
        float StrateBottomZ,
        float WorldX,
        float WorldY,
        float MaxLateralSnap,
        FVector& OutPoint)
    {
        if (!FMath::IsFinite(StrateTopZ) || !FMath::IsFinite(StrateBottomZ)
            || !FMath::IsFinite(WorldX) || !FMath::IsFinite(WorldY)
            || !FMath::IsFinite(MaxLateralSnap) || MaxLateralSnap < 0.0f
            || StrateTopZ <= StrateBottomZ)
        {
            return false;
        }

        if (!FMath::IsFinite(Params.CellSize)
            || !FMath::IsFinite(Params.CorridorRadius)
            || !FMath::IsFinite(Params.BranchProbability)
            || !FMath::IsFinite(Params.Verticality)
            || !FMath::IsFinite(Params.SurfaceRoughness)
            || !FMath::IsFinite(Params.BoundarySealThickness)
            || !FMath::IsFinite(Params.BaseDensity)
            || Params.CellSize <= 0.0f
            || Params.CorridorRadius <= 0.0f
            || Params.BranchProbability < 0.0f || Params.BranchProbability > 1.0f
            || Params.Verticality < 0.0f || Params.Verticality > 1.0f
            || Params.SurfaceRoughness < 0.0f
            || Params.BoundarySealThickness < 0.0f
            || Params.BaseDensity <= 0.0f)
        {
            return false;
        }

        const float CellSize = FMath::Max(Params.CellSize, 1.0f);
        const float Radius = FMath::Max(Params.CorridorRadius, 0.5f);
        // VoxelNoise::FBM has the proven absolute bound 1.5. This is the displacement that a
        // corridor radius must survive; using the nominal [-1,1] label here would certify a
        // landing inside a wall at the extremum.
        const float RoughnessBound = Params.SurfaceRoughness * VOXEL_NOISE_SCALE * 1.5f;

        // A strict interior of the tube is needed. At the MC zero surface the source's smooth
        // carve is only half applied, so stopping at the nominal radius would be an air guess.
        const float SafeRadius = Radius - RoughnessBound - 0.25f;
        if (!FMath::IsFinite(SafeRadius) || SafeRadius <= 0.0f)
        {
            return false;
        }

        const float InnerBottom = StrateBottomZ + Params.BoundarySealThickness;
        const float InnerTop = StrateTopZ - Params.BoundarySealThickness;
        if (!FMath::IsFinite(InnerBottom) || !FMath::IsFinite(InnerTop)
            || InnerBottom >= InnerTop)
        {
            return false;
        }

        // A node centre must leave the corridor radius above the lower interior boundary. The
        // explicit checks in the loop keep ceil/floor edge cases strict after float rounding.
        const int32 MinNodeZ = FMath::CeilToInt(
            (InnerBottom + Radius) / CellSize - 0.5f);
        const int32 MaxNodeZ = FMath::FloorToInt(
            InnerTop / CellSize - 0.5f);
        constexpr int32 MaxNodeLevels = 256;
        if (MaxNodeZ < MinNodeZ || MaxNodeZ - MinNodeZ >= MaxNodeLevels)
        {
            return false;
        }

        const int32 BaseCellX = FMath::FloorToInt(WorldX / CellSize);
        const int32 BaseCellY = FMath::FloorToInt(WorldY / CellSize);
        const uint32 SeedU = (uint32)Seed ^ 0x4D617A65u;  // 'Maze'
        const float MaxSnapSq = MaxLateralSnap * MaxLateralSnap;

        float BestDistSq = FLT_MAX;
        float BestX = 0.0f;
        float BestY = 0.0f;
        float BestZ = 0.0f;
        int32 BestCellX = INT32_MAX;
        int32 BestCellY = INT32_MAX;
        int32 BestNodeZ = INT32_MAX;
        int32 BestAxis = INT32_MAX;
        bool bFound = false;

        auto ConsiderEdge = [&](int32 CellX, int32 CellY, int32 CellZ, int32 Axis)
        {
            const VoxelMazeTopology::EAxis EdgeAxis =
                static_cast<VoxelMazeTopology::EAxis>(Axis);
            if (!VoxelMazeTopology::IsOpenEdge(
                    CellX, CellY, CellZ, EdgeAxis, SeedU,
                    Params.BranchProbability, Params.Verticality))
            {
                return;
            }

            const float NodeX = (CellX + 0.5f) * CellSize;
            const float NodeY = (CellY + 0.5f) * CellSize;
            const float NodeZWorld = (CellZ + 0.5f) * CellSize;
            if (NodeZWorld - Radius <= InnerBottom || NodeZWorld >= InnerTop)
            {
                return;
            }

            float ClosestX = NodeX;
            float ClosestY = NodeY;
            if (Axis == 0)
            {
                ClosestX = FMath::Clamp(WorldX, NodeX, NodeX + CellSize);
            }
            else
            {
                ClosestY = FMath::Clamp(WorldY, NodeY, NodeY + CellSize);
            }

            const float DX = WorldX - ClosestX;
            const float DY = WorldY - ClosestY;
            const float DistSq = DX * DX + DY * DY;
            const bool bCloser = DistSq < BestDistSq;
            const bool bTie = DistSq == BestDistSq
                && (CellZ < BestNodeZ
                    || (CellZ == BestNodeZ
                        && (CellX < BestCellX
                            || (CellX == BestCellX
                                && (CellY < BestCellY
                                    || (CellY == BestCellY && Axis < BestAxis))))));
            if (!bCloser && !bTie) return;

            BestDistSq = DistSq;
            BestX = ClosestX;
            BestY = ClosestY;
            BestZ = NodeZWorld;
            BestCellX = CellX;
            BestCellY = CellY;
            BestNodeZ = CellZ;
            BestAxis = Axis;
            bFound = true;
        };

        // A candidate edge can be one cell beyond the requested point when the point is near a
        // cell boundary. The two-cell guard band covers the segment endpoint and the one-cell
        // lateral budget without turning this into an unbounded lattice scan.
        const int32 SearchRadius = FMath::CeilToInt(MaxLateralSnap / CellSize) + 2;
        for (int32 CellZ = MinNodeZ; CellZ <= MaxNodeZ; ++CellZ)
        {
            for (int32 CellY = BaseCellY - SearchRadius; CellY <= BaseCellY + SearchRadius; ++CellY)
            {
                for (int32 CellX = BaseCellX - SearchRadius; CellX <= BaseCellX + SearchRadius; ++CellX)
                {
                    ConsiderEdge(CellX, CellY, CellZ, 0);
                    ConsiderEdge(CellX, CellY, CellZ, 1);
                }
            }
        }

        if (!bFound || BestDistSq > MaxSnapSq || !FMath::IsFinite(BestX)
            || !FMath::IsFinite(BestY) || !FMath::IsFinite(BestZ))
        {
            return false;
        }

        FMazeGenerationParams SourceParams = Params;
        SourceParams.StrateTopWorldZ = StrateTopZ;
        SourceParams.StrateBottomWorldZ = StrateBottomZ;
        FVoxelStrateMeasureSettings FitSettings;
        const float MaxStepHeightVoxels =
            FVoxelPlayerCapsuleConstants::MaxStepHeightMeters
                / FVoxelPlayerCapsuleConstants::VoxelSizeMeters;
        const auto SampleMaze = [&](float X, float Y, float Z)
        {
            return VF_EvaluateMazeLandingDensity(
                SourceParams, static_cast<uint32>(Seed), X, Y, Z);
        };

        // The nearest point on a lattice edge can be its spherical node cap, where the support
        // patch is narrower than the same edge's middle. Try a few deterministic points along
        // the selected horizontal edge, then a small vertical anchor band. Every trial remains
        // inside the original lateral budget and is checked by the exact player stencil.
        const float NodeX = (BestCellX + 0.5f) * CellSize;
        const float NodeY = (BestCellY + 0.5f) * CellSize;
        constexpr int32 EdgeSamples = 9;
        const int32 VerticalOffsets[] = { 0, 1, -1, 2, -2, 3, -3, 4, 5, 6 };
        for (int32 EdgeSample = 0; EdgeSample < EdgeSamples; ++EdgeSample)
        {
            const float T = static_cast<float>(EdgeSample) / (EdgeSamples - 1);
            const float CandidateX = BestAxis == 0 ? NodeX + T * CellSize : NodeX;
            const float CandidateY = BestAxis == 1 ? NodeY + T * CellSize : NodeY;
            const float LateralDX = CandidateX - WorldX;
            const float LateralDY = CandidateY - WorldY;
            if (LateralDX * LateralDX + LateralDY * LateralDY > MaxSnapSq)
            {
                continue;
            }

            for (const int32 VerticalOffset : VerticalOffsets)
            {
                const FVector CandidateFeet(
                    CandidateX,
                    CandidateY,
                    BestZ - Radius + RoughnessBound
                        + MaxStepHeightVoxels + 0.5f
                        + static_cast<float>(VerticalOffset));
                if (VF_ValidatePlayerFitPose(
                        FitSettings,
                        CandidateFeet,
                        StrateTopZ,
                        StrateBottomZ,
                        SourceParams.BoundarySealThickness,
                        SampleMaze,
                        OutPoint))
                {
                    return true;
                }
            }
        }
        return false;
    }

    // VerticalShafts has a guaranteed floor only when its lower boundary seal exists. Select a
    // point above that seal and outside every ledge band; an infinite shaft by itself is not a
    // standing point. The 3x3 roll is deliberately the same local source neighbourhood as the
    // production density function.
    bool VF_SuggestVerticalShaftLandingPoint(
        const FVerticalShaftParams& Params,
        int32 Seed,
        float StrateTopZ,
        float StrateBottomZ,
        float WorldX,
        float WorldY,
        float MaxLateralSnap,
        FVector& OutPoint)
    {
        if (!FMath::IsFinite(StrateTopZ) || !FMath::IsFinite(StrateBottomZ)
            || !FMath::IsFinite(WorldX) || !FMath::IsFinite(WorldY)
            || !FMath::IsFinite(MaxLateralSnap) || MaxLateralSnap < 0.0f
            || StrateTopZ <= StrateBottomZ)
        {
            return false;
        }

        if (!FMath::IsFinite(Params.ShaftSpacing)
            || !FMath::IsFinite(Params.ShaftDensity)
            || !FMath::IsFinite(Params.ShaftMinRadius)
            || !FMath::IsFinite(Params.ShaftMaxRadius)
            || !FMath::IsFinite(Params.CrossConnectChance)
            || !FMath::IsFinite(Params.ConnectorRadius)
            || !FMath::IsFinite(Params.LedgeSpacing)
            || !FMath::IsFinite(Params.LedgeDepth)
            || !FMath::IsFinite(Params.SurfaceRoughness)
            || !FMath::IsFinite(Params.BoundarySealThickness)
            || !FMath::IsFinite(Params.BaseDensity)
            || Params.ShaftSpacing <= 0.0f
            || Params.ShaftDensity <= 0.0f || Params.ShaftDensity > 1.0f
            || Params.ShaftMinRadius <= 0.0f
            || Params.ShaftMaxRadius < Params.ShaftMinRadius
            || Params.CrossConnectChance < 0.0f || Params.CrossConnectChance > 1.0f
            || Params.ConnectorRadius <= 0.0f
            || Params.LedgeSpacing < 0.0f || Params.LedgeDepth < 0.0f
            || Params.SurfaceRoughness < 0.0f
            || Params.BoundarySealThickness <= 0.0f
            || Params.BaseDensity <= 0.0f)
        {
            return false;
        }

        const float RoughnessBound = Params.SurfaceRoughness * VOXEL_NOISE_SCALE;
        const float ShaftInteriorMargin = RoughnessBound + 0.25f;
        const float Spacing = FMath::Max(Params.ShaftSpacing, 1.0f);
        const int32 BaseCellX = FMath::FloorToInt(WorldX / Spacing);
        const int32 BaseCellY = FMath::FloorToInt(WorldY / Spacing);
        const uint32 SeedU = (uint32)Seed ^ 0x53686674u;  // 'Shft'

        const float MaxSnapSq = MaxLateralSnap * MaxLateralSnap;
        const int32 SearchRadius = FMath::CeilToInt(
            (MaxLateralSnap + FMath::Max(Params.ShaftMaxRadius, 1.0f)) / Spacing) + 2;

        float BestDistSq = FLT_MAX;
        float BestAxisX = 0.0f;
        float BestAxisY = 0.0f;
        float BestSafeRadius = 0.0f;
        bool bFound = false;
        int32 BestCellX = INT32_MAX;
        int32 BestCellY = INT32_MAX;
        for (int32 DY = -SearchRadius; DY <= SearchRadius; ++DY)
        {
            for (int32 DX = -SearchRadius; DX <= SearchRadius; ++DX)
            {
                const int32 CellX = BaseCellX + DX;
                const int32 CellY = BaseCellY + DY;
                const uint32 H = VoxelHash::Cell(CellX, CellY, SeedU);
                if (VoxelHash::ToFloat01(H) > Params.ShaftDensity) continue;

                const float JX = VoxelHash::ToFloat01(VoxelHash::Mix(H ^ 0x12345678u));
                const float JY = VoxelHash::ToFloat01(VoxelHash::Mix(H ^ 0x9ABCDEF0u));
                const float ShaftX = (CellX + 0.15f + JX * 0.7f) * Spacing;
                const float ShaftY = (CellY + 0.15f + JY * 0.7f) * Spacing;
                const float Radius = FMath::Lerp(Params.ShaftMinRadius, Params.ShaftMaxRadius,
                    VoxelHash::ToFloat01(VoxelHash::Mix(H ^ 0xBEEFu)));
                const float SafeRadius = Radius - ShaftInteriorMargin;
                if (!FMath::IsFinite(SafeRadius) || SafeRadius <= 0.0f)
                {
                    continue;
                }

                const float DXWorld = WorldX - ShaftX;
                const float DYWorld = WorldY - ShaftY;
                const float Distance = FMath::Sqrt(DXWorld * DXWorld + DYWorld * DYWorld);
                const float SnapDistance = FMath::Max(Distance - SafeRadius, 0.0f);
                const float DistSq = SnapDistance * SnapDistance;
                const bool bCloser = DistSq < BestDistSq;
                const bool bTie = DistSq == BestDistSq
                    && (CellX < BestCellX || (CellX == BestCellX && CellY < BestCellY));
                if (!bCloser && !bTie)
                {
                    continue;
                }

                // A VerticalShafts landing must identify the topology, not merely an open point
                // inside its radius. The safe-radius distance still chooses the nearest confident
                // site under the existing snap budget. The candidate offsets below stay inside
                // this same feature core while allowing the generated ledge to provide a real
                // walkable support patch; the exact shaft axis is an open cylinder and has no
                // support surface by itself.
                BestAxisX = ShaftX;
                BestAxisY = ShaftY;
                BestSafeRadius = SafeRadius;
                BestDistSq = DistSq;
                BestCellX = CellX;
                BestCellY = CellY;
                bFound = true;
            }
        }

        if (!bFound || BestDistSq > MaxSnapSq || !FMath::IsFinite(BestAxisX)
            || !FMath::IsFinite(BestAxisY))
        {
            return false;
        }

        FVerticalShaftParams SourceParams = Params;
        SourceParams.StrateTopWorldZ = StrateTopZ;
        SourceParams.StrateBottomWorldZ = StrateBottomZ;

        FVoxelStrateMeasureSettings FitSettings;
        float InnerTop = 0.0f;
        float InnerBottom = 0.0f;
        if (!VF_GetPlayerFitInteriorBounds(
                FitSettings, StrateTopZ, StrateBottomZ,
                Params.BoundarySealThickness, InnerTop, InnerBottom))
        {
            return false;
        }
        const auto SampleShaft = [&](float X, float Y, float Z)
        {
            return VF_EvaluateShaftLandingDensity(
                SourceParams, static_cast<uint32>(Seed), X, Y, Z);
        };

        // The query's default interior margin is the same 2x-seal margin used by the exact
        // player-fit measurement. This prevents a lower boundary seal from being reported as a
        // standing floor that the measured window intentionally excludes.
        const float FirstCandidateZ = InnerBottom + 0.5f;
        const float LastCandidateZ = InnerTop
            - 2.0f * FitSettings.PlayerCapsuleHalfHeightVoxels - 0.5f;
        if (!FMath::IsFinite(FirstCandidateZ) || !FMath::IsFinite(LastCandidateZ)
            || LastCandidateZ < FirstCandidateZ)
        {
            return false;
        }

        const int32 NumCandidateSteps = FMath::Min(
            4096,
            FMath::Max(0, FMath::CeilToInt(LastCandidateZ - FirstCandidateZ)));

        const FVector2D CandidateOffsets[] = {
            FVector2D(0.0f, 0.0f),
            FVector2D(0.20f, 0.20f),
            FVector2D(0.35f, 0.35f),
            FVector2D(0.50f, 0.15f),
            FVector2D(0.15f, 0.50f),
            FVector2D(0.55f, 0.35f),
            FVector2D(0.35f, 0.55f),
        };
        for (const FVector2D& NormalizedOffset : CandidateOffsets)
        {
            const float CandidateX = BestAxisX
                + NormalizedOffset.X * BestSafeRadius;
            const float CandidateY = BestAxisY
                + NormalizedOffset.Y * BestSafeRadius;
            const float OffsetSq = FMath::Square(CandidateX - BestAxisX)
                + FMath::Square(CandidateY - BestAxisY);
            const float LateralDX = CandidateX - WorldX;
            const float LateralDY = CandidateY - WorldY;
            const float LateralDistanceSq = FMath::Square(LateralDX)
                + FMath::Square(LateralDY);
            if (!FMath::IsFinite(CandidateX) || !FMath::IsFinite(CandidateY)
                || !FMath::IsFinite(LateralDistanceSq)
                || OffsetSq > FMath::Square(BestSafeRadius)
                || LateralDistanceSq > MaxSnapSq + KINDA_SMALL_NUMBER)
            {
                continue;
            }

            for (int32 CandidateStep = 0;
                 CandidateStep <= NumCandidateSteps;
                 ++CandidateStep)
            {
                const FVector CandidateFeet(
                    CandidateX, CandidateY,
                    FirstCandidateZ + static_cast<float>(CandidateStep));
                if (VF_ValidatePlayerFitPose(
                        FitSettings,
                        CandidateFeet,
                        StrateTopZ,
                        StrateBottomZ,
                        Params.BoundarySealThickness,
                        SampleShaft,
                        OutPoint))
                {
                    if (Seed == 14 && FMath::IsNearlyEqual(StrateBottomZ, -736.0f))
                    {
                        UE_LOG(LogTemp, Display,
                            TEXT("[VoxelForgeExplore][TemporaryVerticalLanding] axis=(%.3f,%.3f) "
                                 "safeRadius=%.3f candidate=(%.3f,%.3f,%.3f)"),
                            BestAxisX, BestAxisY, BestSafeRadius,
                            CandidateFeet.X, CandidateFeet.Y, CandidateFeet.Z);
                    }
                    return true;
                }
            }
        }
        return false;
    }

    struct FVFIslandSite
    {
        int32 CellX, CellY;
        float X, Y, Rxy, TopHalf, TopZ, BotZ, TaperEnd;
    };

    // Evaluate only the island source, in the same internal-density convention as
    // GetFloatingIslandDensity (positive = solid, negative = void). This is intentionally a
    // local pure evaluator: it lets the query bracket the actual blob top instead of treating a
    // nominal island centre as footing. Full-octave noise is used because this query has no LOD
    // state by design and GeneratePassages runs before a tile's LOD is selected.
    float VF_EvaluateIslandInteriorDensity(
        const FFloatingIslandParams& Params,
        uint32 Seed,
        const FVFIslandSite* Islands,
        int32 NumIslands,
        float WorldX,
        float WorldY,
        float WorldZ)
    {
        const float WarpAmp = (Params.IslandMinRadius + Params.IslandMaxRadius) * 0.5f * 0.35f;
        const FVector WarpXPosition(
            WorldX * 0.04f + VoxelHash::SeedOffset(Seed, 0.0007f),
            WorldY * 0.04f,
            WorldZ * 0.012f);
        const FVector WarpYPosition(
            WorldX * 0.04f + 31.0f,
            WorldY * 0.04f + 7.0f,
            WorldZ * 0.012f);
        const float WX = WorldX + VoxelNoise::FBM(
            (float)WarpXPosition.X, (float)WarpXPosition.Y, (float)WarpXPosition.Z, 3)
            * VOXEL_NOISE_SCALE * WarpAmp;
        const float WY = WorldY + VoxelNoise::FBM(
            (float)WarpYPosition.X, (float)WarpYPosition.Y, (float)WarpYPosition.Z, 3)
            * VOXEL_NOISE_SCALE * WarpAmp;

        const float BlendK = FMath::Max(Params.SDFBlendRadius, 0.01f);
        const float Spacing = FMath::Max(Params.IslandSpacing, 1.0f);
        const int32 BaseCellX = FMath::FloorToInt(WorldX / Spacing);
        const int32 BaseCellY = FMath::FloorToInt(WorldY / Spacing);
        float IslandSDF = FLT_MAX;
        for (int32 IslandIndex = 0; IslandIndex < NumIslands; ++IslandIndex)
        {
            const FVFIslandSite& Island = Islands[IslandIndex];
            // Production GetFloatingIslandDensity evaluates exactly this 3x3 cell neighbourhood
            // for the queried XY. The outer search may collect more candidates so a lateral snap
            // can be found, but those distant blobs must not influence the returned footing.
            if (FMath::Abs(Island.CellX - BaseCellX) > 1
                || FMath::Abs(Island.CellY - BaseCellY) > 1)
            {
                continue;
            }
            const float DX = WX - Island.X;
            const float DY = WY - Island.Y;
            const float DistXY = FMath::Sqrt(DX * DX + DY * DY);
            const float Height = FMath::Clamp(
                (WorldZ - Island.BotZ) / FMath::Max(Island.TopZ - Island.BotZ, 1.0f),
                0.0f, 1.0f);
            const float Taper = SmoothStep01(FMath::Clamp(
                Height / Island.TaperEnd, 0.0f, 1.0f));
            const float Envelope = Island.Rxy * Taper;

            float TopSurface = Island.TopZ;
            if (Params.TopFlatten < 1.0f)
            {
                const float Edge = FMath::Clamp(
                    DistXY / FMath::Max(Island.Rxy, 1.0f), 0.0f, 1.0f);
                TopSurface = Island.TopZ
                    - (1.0f - Params.TopFlatten) * Island.TopHalf * 2.0f * Edge * Edge;
            }

            const float SDF = FMath::Max(DistXY - Envelope, WorldZ - TopSurface);
            IslandSDF = VoxelSDF::SmoothMin(IslandSDF, SDF, BlendK);
        }

        if (Params.SurfaceRoughness > 0.0f
            && IslandSDF < Params.SurfaceRoughness + BlendK + 2.0f)
        {
            const FVector RoughnessPosition(WorldX * 0.08f, WorldY * 0.08f, WorldZ * 0.08f);
            IslandSDF += VoxelNoise::FBM(
                (float)RoughnessPosition.X, (float)RoughnessPosition.Y,
                (float)RoughnessPosition.Z, 4)
                * VOXEL_NOISE_SCALE * Params.SurfaceRoughness;
        }

        float Density = -Params.BaseDensity;
        if (IslandSDF < BlendK)
        {
            float Fill = FMath::Clamp(
                (BlendK - IslandSDF) / (BlendK * 2.0f), 0.0f, 1.0f);
            Fill = SmoothStep01(Fill);
            Density += Fill * Params.BaseDensity * 2.0f;
        }
        return Density;
    }

    bool VF_SuggestFloatingIslandLandingPoint(
        const FFloatingIslandParams& Params,
        int32 Seed,
        float StrateTopZ,
        float StrateBottomZ,
        float WorldX,
        float WorldY,
        float MaxLateralSnap,
        FVector& OutPoint)
    {
        if (!FMath::IsFinite(StrateTopZ) || !FMath::IsFinite(StrateBottomZ)
            || !FMath::IsFinite(WorldX) || !FMath::IsFinite(WorldY)
            || !FMath::IsFinite(MaxLateralSnap) || MaxLateralSnap < 0.0f
            || StrateTopZ <= StrateBottomZ)
        {
            return false;
        }

        if (!FMath::IsFinite(Params.IslandSpacing)
            || !FMath::IsFinite(Params.IslandDensity)
            || !FMath::IsFinite(Params.IslandMinRadius)
            || !FMath::IsFinite(Params.IslandMaxRadius)
            || !FMath::IsFinite(Params.ThicknessRatio)
            || !FMath::IsFinite(Params.VerticalJitter)
            || !FMath::IsFinite(Params.TopFlatten)
            || !FMath::IsFinite(Params.SurfaceRoughness)
            || !FMath::IsFinite(Params.SDFBlendRadius)
            || !FMath::IsFinite(Params.BoundarySealThickness)
            || !FMath::IsFinite(Params.BaseDensity)
            || Params.IslandSpacing <= 0.0f
            || Params.IslandDensity <= 0.0f || Params.IslandDensity > 1.0f
            || Params.IslandMinRadius <= 0.0f
            || Params.IslandMaxRadius < Params.IslandMinRadius
            || Params.ThicknessRatio < 0.0f
            || Params.VerticalJitter < 0.0f
            || Params.SurfaceRoughness < 0.0f
            || Params.BoundarySealThickness < 0.0f
            || Params.BaseDensity <= 0.0f)
        {
            return false;
        }

        const float H = StrateTopZ - StrateBottomZ;
        const float Spacing = FMath::Max(Params.IslandSpacing, 1.0f);
        const uint32 SeedU = (uint32)Seed ^ 0x49736C64u;  // 'Isld'
        const float MidZ = (StrateTopZ + StrateBottomZ) * 0.5f;

        const float WarpAmp = (Params.IslandMinRadius + Params.IslandMaxRadius) * 0.5f * 0.35f;
        const float WarpBound = WarpAmp * VOXEL_NOISE_SCALE;
        const float MaxIslandRadius = FMath::Max(Params.IslandMaxRadius, 1.0f);
        const float SearchReach = MaxLateralSnap + MaxIslandRadius + WarpBound + 2.0f;
        const int32 SearchRadius = FMath::CeilToInt(SearchReach / Spacing) + 2;
        const int32 BaseCellX = FMath::FloorToInt(WorldX / Spacing);
        const int32 BaseCellY = FMath::FloorToInt(WorldY / Spacing);

        TArray<FVFIslandSite, TInlineAllocator<64>> Islands;
        Islands.Reserve((2 * SearchRadius + 1) * (2 * SearchRadius + 1));
        float MinBotZ = FLT_MAX;
        float MaxTopZ = -FLT_MAX;
        for (int32 DY = -SearchRadius; DY <= SearchRadius; ++DY)
        {
            for (int32 DX = -SearchRadius; DX <= SearchRadius; ++DX)
            {
                const int32 CellX = BaseCellX + DX;
                const int32 CellY = BaseCellY + DY;
                const uint32 Hh = VoxelHash::Cell(CellX, CellY, SeedU);
                if (VoxelHash::ToFloat01(Hh) > Params.IslandDensity) continue;

                FVFIslandSite& Island = Islands.AddDefaulted_GetRef();
                Island.CellX = CellX;
                Island.CellY = CellY;
                const float JX = VoxelHash::ToFloat01(VoxelHash::Mix(Hh ^ 0x12345678u));
                const float JY = VoxelHash::ToFloat01(VoxelHash::Mix(Hh ^ 0x9ABCDEF0u));
                Island.X = (CellX + 0.15f + JX * 0.7f) * Spacing;
                Island.Y = (CellY + 0.15f + JY * 0.7f) * Spacing;
                Island.Rxy = FMath::Lerp(Params.IslandMinRadius, Params.IslandMaxRadius,
                    VoxelHash::ToFloat01(VoxelHash::Mix(Hh ^ 0x5A5Au)));
                Island.TopHalf = Island.Rxy * 0.20f;
                const float UnderDepth = Island.Rxy * FMath::Max(Params.ThicknessRatio, 0.25f);
                const float SpreadZ = FMath::Max(
                    H * 0.5f - FMath::Max(Island.TopHalf, UnderDepth)
                    - Params.BoundarySealThickness, 0.0f) * Params.VerticalJitter;
                const float CenterZ = MidZ
                    + VoxelHash::ToFloatSigned(VoxelHash::Mix(Hh ^ 0xB17Du)) * SpreadZ;
                Island.TopZ = CenterZ + Island.TopHalf;
                Island.BotZ = CenterZ - UnderDepth;
                Island.TaperEnd = FMath::Lerp(0.45f, 0.7f,
                    VoxelHash::ToFloat01(VoxelHash::Mix(Hh ^ 0x7A1Eu)));

                MinBotZ = FMath::Min(MinBotZ, Island.BotZ);
                MaxTopZ = FMath::Max(MaxTopZ, Island.TopZ);
            }
        }

        if (Islands.Num() == 0 || !FMath::IsFinite(MinBotZ) || !FMath::IsFinite(MaxTopZ))
        {
            return false;
        }

        const float RoughnessBound = Params.SurfaceRoughness * VOXEL_NOISE_SCALE;
        const float BlendK = FMath::Max(Params.SDFBlendRadius, 0.01f);
        const float SearchPad = RoughnessBound + BlendK + 3.0f;
        const float SearchTop = MaxTopZ + SearchPad;
        const float SearchBottom = MinBotZ - SearchPad;
        constexpr float ScanStep = 0.5f;
        constexpr int32 MaxVerticalSamples = 2048;
        if (!FMath::IsFinite(SearchTop) || !FMath::IsFinite(SearchBottom)
            || SearchTop <= SearchBottom
            || (SearchTop - SearchBottom) / ScanStep > (float)MaxVerticalSamples)
        {
            return false;
        }

        const float MaxSnapSq = MaxLateralSnap * MaxLateralSnap;
        float BestDistSq = FLT_MAX;
        FVector BestPoint = FVector::ZeroVector;
        int32 BestIslandIndex = INDEX_NONE;
        FVoxelStrateMeasureSettings FitSettings;

        auto FindLandingZ = [&](float CandidateX, float CandidateY, float& OutLandingZ) -> bool
        {
            float UpperZ = SearchTop;
            float UpperDensity = VF_EvaluateIslandInteriorDensity(
                Params, SeedU, Islands.GetData(), Islands.Num(), CandidateX, CandidateY, UpperZ);
            if (!FMath::IsFinite(UpperDensity) || UpperDensity > 0.0f)
            {
                return false;
            }

            for (int32 Sample = 0; Sample < MaxVerticalSamples && UpperZ > SearchBottom; ++Sample)
            {
                const float LowerZ = FMath::Max(SearchBottom, UpperZ - ScanStep);
                const float LowerDensity = VF_EvaluateIslandInteriorDensity(
                    Params, SeedU, Islands.GetData(), Islands.Num(), CandidateX, CandidateY, LowerZ);
                if (!FMath::IsFinite(LowerDensity)) return false;

                // Descending from guaranteed air, the first solid bracket is the highest actual
                // blob top at this XY. That is the footing surface; no island-centre guess is
                // involved.
                if (UpperDensity <= 0.0f && LowerDensity > 0.0f)
                {
                    float SolidZ = LowerZ;
                    float AirZ = UpperZ;
                    for (int32 Iteration = 0; Iteration < 12; ++Iteration)
                    {
                        const float Mid = (SolidZ + AirZ) * 0.5f;
                        const float MidDensity = VF_EvaluateIslandInteriorDensity(
                            Params, SeedU, Islands.GetData(), Islands.Num(), CandidateX, CandidateY, Mid);
                        if (!FMath::IsFinite(MidDensity)) return false;
                        if (MidDensity > 0.0f) SolidZ = Mid;
                        else                   AirZ = Mid;
                    }

                    const float InnerBottom = StrateBottomZ + Params.BoundarySealThickness;
                    const float InnerTop = StrateTopZ - Params.BoundarySealThickness;
                    const float LandingZ = AirZ + 0.5f;
                    if (!FMath::IsFinite(LandingZ)
                        || LandingZ <= InnerBottom || LandingZ >= InnerTop
                        || VF_EvaluateIslandInteriorDensity(
                            Params, SeedU, Islands.GetData(), Islands.Num(), CandidateX, CandidateY, LandingZ) > 0.0f
                        || VF_EvaluateIslandInteriorDensity(
                            Params, SeedU, Islands.GetData(), Islands.Num(), CandidateX, CandidateY, SolidZ) <= 0.0f
                        || VF_EvaluateIslandInteriorDensity(
                            Params, SeedU, Islands.GetData(), Islands.Num(), CandidateX, CandidateY, LandingZ - 1.0f) <= 0.0f)
                    {
                        return false;
                    }

                    OutLandingZ = LandingZ;
                    return FMath::IsFinite(OutLandingZ);
                }

                UpperZ = LowerZ;
                UpperDensity = LowerDensity;
            }

            return false;
        };

        const auto SampleIsland = [&](float X, float Y, float Z)
        {
            // Island density uses the opposite convention internally (positive = solid).
            return -VF_EvaluateIslandInteriorDensity(
                Params, SeedU, Islands.GetData(), Islands.Num(), X, Y, Z);
        };
        const float MaxStepHeightVoxels =
            FVoxelPlayerCapsuleConstants::MaxStepHeightMeters
                / FVoxelPlayerCapsuleConstants::VoxelSizeMeters;
        const FIntPoint CoreOffsets[] = {
            FIntPoint(0, 0),
            FIntPoint(1, 0), FIntPoint(-1, 0),
            FIntPoint(0, 1), FIntPoint(0, -1),
            FIntPoint(2, 0), FIntPoint(-2, 0),
            FIntPoint(0, 2), FIntPoint(0, -2),
            FIntPoint(1, 1), FIntPoint(1, -1),
            FIntPoint(-1, 1), FIntPoint(-1, -1),
            FIntPoint(3, 0), FIntPoint(-3, 0),
            FIntPoint(0, 3), FIntPoint(0, -3)
        };
        const float FeetOffsets[] = { 0.0f, -1.0f, 1.0f, -2.0f, 2.0f, 3.0f };

        for (int32 IslandIndex = 0; IslandIndex < Islands.Num(); ++IslandIndex)
        {
            const FVFIslandSite& Island = Islands[IslandIndex];
            // Reserve a conservative interior disk. WarpBound protects the source's lobed frame;
            // the one-voxel margin keeps a returned top away from the nominal radial edge. If no
            // such disk is within MaxLateralSnap, declining is preferable to an arbitrary jump.
            const float SafeRadius = Island.Rxy - WarpBound - 1.0f;
            if (!FMath::IsFinite(SafeRadius) || SafeRadius <= 0.0f) continue;

            const float DX = WorldX - Island.X;
            const float DY = WorldY - Island.Y;
            const float Distance = FMath::Sqrt(DX * DX + DY * DY);
            const float SnapDistance = FMath::Max(Distance - SafeRadius, 0.0f);
            const float DistSq = SnapDistance * SnapDistance;
            if (DistSq > MaxSnapSq) continue;

            float CandidateX = WorldX;
            float CandidateY = WorldY;
            // Prefer the feature core whenever it fits the lateral budget. A point near the
            // nominal blob rim can be open air yet fail the support patch; the deterministic
            // centre is the island's large, stable landing feature.
            if (Distance <= MaxLateralSnap)
            {
                CandidateX = Island.X;
                CandidateY = Island.Y;
            }
            else if (Distance > SafeRadius && Distance > KINDA_SMALL_NUMBER)
            {
                const float Scale = SafeRadius / Distance;
                CandidateX = Island.X + DX * Scale;
                CandidateY = Island.Y + DY * Scale;
            }

            const float CandidateDX = CandidateX - WorldX;
            const float CandidateDY = CandidateY - WorldY;
            const float CandidateDistSq =
                CandidateDX * CandidateDX + CandidateDY * CandidateDY;
            if (CandidateDistSq > MaxSnapSq)
            {
                continue;
            }

            // A top surface is only a landing site if the complete player stencil can occupy the
            // air above it and the support patch is walkable.  The core offsets are a bounded,
            // deterministic local search for a broad patch; they are not a component query.
            for (const FIntPoint& CoreOffset : CoreOffsets)
            {
                const float TrialX = CandidateX + static_cast<float>(CoreOffset.X);
                const float TrialY = CandidateY + static_cast<float>(CoreOffset.Y);
                const float TrialDX = TrialX - Island.X;
                const float TrialDY = TrialY - Island.Y;
                if (TrialDX * TrialDX + TrialDY * TrialDY
                        > FMath::Square(SafeRadius) + KINDA_SMALL_NUMBER)
                {
                    continue;
                }
                const float LateralDX = TrialX - WorldX;
                const float LateralDY = TrialY - WorldY;
                const float TrialDistSq = LateralDX * LateralDX + LateralDY * LateralDY;
                if (TrialDistSq > MaxSnapSq) continue;

                float CandidateZ = 0.0f;
                if (!FindLandingZ(TrialX, TrialY, CandidateZ)) continue;
                for (const float FeetOffset : FeetOffsets)
                {
                    FVector FitPoint = FVector::ZeroVector;
                    if (!VF_ValidatePlayerFitPose(
                            FitSettings,
                            FVector(TrialX, TrialY,
                                CandidateZ + FeetOffset + MaxStepHeightVoxels),
                            StrateTopZ,
                            StrateBottomZ,
                            Params.BoundarySealThickness,
                            SampleIsland,
                            FitPoint))
                    {
                        continue;
                    }

                    const bool bCloser = TrialDistSq < BestDistSq;
                    const bool bTie = TrialDistSq == BestDistSq
                        && IslandIndex < BestIslandIndex;
                    if (!bCloser && !bTie) continue;

                    BestDistSq = TrialDistSq;
                    BestPoint = FitPoint;
                    BestIslandIndex = IslandIndex;
                }
            }
        }

        if (BestIslandIndex == INDEX_NONE || BestDistSq > MaxSnapSq || BestPoint.ContainsNaN())
        {
            return false;
        }

        OutPoint = BestPoint;
        return FMath::IsFinite(OutPoint.X) && FMath::IsFinite(OutPoint.Y)
            && FMath::IsFinite(OutPoint.Z);
    }
}

namespace
{
    constexpr float VF_LandingCarveSafetyMargin =
        VoxelPassageGeometry::SealSafetyMarginVoxels;
}

FVector VoxelCaveMorphology::ApplyCaveWarp(
    const FVector& WorldPoint,
    const FStrateGenerationParams& Params,
    uint32 Seed)
{
    return VF_ApplyCaveWarp(WorldPoint, Params, Seed);
}

FVoxelPassageLanding VF_BuildPassageLanding(
    const FVector& InStandingPoint,
    float MouthRadius,
    const FVector& InDoorDirection,
    float StrateTopZ,
    float StrateBottomZ,
    float BoundarySealThickness,
    bool bSourcePlayerFit)
{
    FVoxelPassageLanding Landing;

    // Body-derived dimensions at the authored 25 cm voxel scale are shared with the origin
    // landing.  In particular, the 0.68 m player diameter, 3 m turning patch, 1.76 m capsule
    // height and 1 m headroom become the common §6.5 formulas in VoxelPassageGeometry.
    const float SafeMouthRadius = FMath::Max(FMath::Abs(MouthRadius), 1.0f);
    const float DesiredHalfWidth = VoxelPassageGeometry::LandingHalfWidthForRadius(MouthRadius);
    const float DesiredHeight = VoxelPassageGeometry::LandingHeightForRadius(MouthRadius);
    const float SafeSeal = FMath::Max(BoundarySealThickness, 0.0f);

    FVector Direction(InDoorDirection.X, InDoorDirection.Y, 0.0f);
    if (!Direction.Normalize())
    {
        Direction = FVector(1.0f, 0.0f, 0.0f);
    }

    Landing.StandingPoint = InStandingPoint;
    Landing.FloorZ = InStandingPoint.Z - 0.5f;
    Landing.HalfWidth = DesiredHalfWidth;
    Landing.CeilingZ = Landing.FloorZ + DesiredHeight;
    Landing.FloorThickness = VoxelPassageGeometry::LandingFloorThicknessVoxels;
    Landing.DoorDirection = Direction;
    Landing.bSourcePlayerFit = bSourcePlayerFit;

    // Keep the complete room and its floor strictly inside the vertical seal. Normal authored
    // strates have far more room than this bound; the clamp is a fail-safe for a source pose near
    // a seal and never moves a normal interior pose.
    const float InnerBottom = StrateBottomZ + SafeSeal;
    const float InnerTop = StrateTopZ - SafeSeal;
    if (FMath::IsFinite(InnerBottom) && FMath::IsFinite(InnerTop)
        && InnerTop > InnerBottom)
    {
        // Passage carving is intentionally ordered after the vertical seal and fades over its
        // four-voxel blend radius. Keep both the underside of the support slab and the top of the
        // room beyond that radius from the seal, so the landing cannot use the passage exception
        // to open a strate boundary.
        const float MinFloor = InnerBottom + Landing.FloorThickness
            + VF_LandingCarveSafetyMargin;
        const float MaxFloor = InnerTop - DesiredHeight - VF_LandingCarveSafetyMargin;
        if (MaxFloor >= MinFloor)
        {
            Landing.FloorZ = FMath::Clamp(Landing.FloorZ, MinFloor, MaxFloor);
            Landing.StandingPoint.Z = Landing.FloorZ + 0.5f;
            Landing.CeilingZ = Landing.FloorZ + DesiredHeight;
        }
        else
        {
            // A degenerate strate cannot satisfy the full body/headroom contract. Fail closed
            // instead of emitting a partial chamber whose blend would approach a seal.
            Landing.HalfWidth = 0.0f;
            Landing.CeilingZ = Landing.FloorZ;
            Landing.bSourcePlayerFit = false;
            return Landing;
        }
    }

    // The tube centreline is tangent to the floor: the tube bottom is exactly FloorZ.  The door
    // sits one voxel inside the room so the room/tube smooth union cannot leave a lip.
    Landing.DoorPoint = Landing.StandingPoint
        + Direction * FMath::Max(0.0f, Landing.HalfWidth - 1.0f);
    Landing.DoorPoint.Z = Landing.FloorZ + SafeMouthRadius;

    return Landing;
}

float VF_EvaluatePassageLandingSDF(
    const FVector& Position,
    const FVoxelPassageLanding& Landing)
{
    if (!FMath::IsFinite(Position.X) || !FMath::IsFinite(Position.Y)
        || !FMath::IsFinite(Position.Z)
        || !FMath::IsFinite(Landing.StandingPoint.X)
        || !FMath::IsFinite(Landing.StandingPoint.Y)
        || !FMath::IsFinite(Landing.FloorZ)
        || !FMath::IsFinite(Landing.CeilingZ)
        || !FMath::IsFinite(Landing.HalfWidth)
        || Landing.HalfWidth <= 0.0f
        || Landing.CeilingZ <= Landing.FloorZ)
    {
        return FLT_MAX;
    }

    const FVector RoomCenter(
        Landing.StandingPoint.X,
        Landing.StandingPoint.Y,
        (Landing.FloorZ + Landing.CeilingZ) * 0.5f);
    constexpr float RoomRounding = 1.5f;
    const FVector RoomHalfExtent(
        FMath::Max(Landing.HalfWidth - RoomRounding, 0.25f),
        FMath::Max(Landing.HalfWidth - RoomRounding, 0.25f),
        FMath::Max((Landing.CeilingZ - Landing.FloorZ) * 0.5f - RoomRounding, 0.25f));

    // max(Room, FloorZ-Z) is the important distinction from a bore: below FloorZ is solid,
    // while every horizontal section above it has the same flat floor plane.
    float LandingSDF = FMath::Max(
        VoxelSDF::RoundedBox(Position, RoomCenter, RoomHalfExtent, RoomRounding),
        Landing.FloorZ - Position.Z);

    return LandingSDF;
}

bool VF_IsPassageLandingFloor(
    const FVector& Position,
    const FVoxelPassageLanding& Landing)
{
    if (!FMath::IsFinite(Position.X) || !FMath::IsFinite(Position.Y)
        || !FMath::IsFinite(Position.Z) || !FMath::IsFinite(Landing.FloorZ)
        || !FMath::IsFinite(Landing.FloorThickness)
        || Landing.FloorThickness <= 0.0f || Landing.HalfWidth <= 0.0f)
    {
        return false;
    }

    const auto IsInFloorBand = [](float Z, float FloorZ, float Thickness)
    {
        // The analytic ramp and the sampled density use the same float interpolation, but a
        // caller can arrive at the mathematically identical plane through a different fused
        // multiply/add sequence. Include one machine epsilon on the upper side so the support
        // backstop cannot disappear at an exact floor sample.
        return Z <= FloorZ + KINDA_SMALL_NUMBER && Z > FloorZ - Thickness;
    };
    // The room floor is a true horizontal landing: it is the only floor that owns the full
    // three-metre turning patch.  The origin room owns its centre, including its support floor.
    if (IsInFloorBand(Position.Z, Landing.FloorZ, Landing.FloorThickness))
    {
        const float FloorHalfWidth = FMath::Max(Landing.HalfWidth - 1.0f, 0.0f);
        if (FMath::Abs(Position.X - Landing.StandingPoint.X) <= FloorHalfWidth
            && FMath::Abs(Position.Y - Landing.StandingPoint.Y) <= FloorHalfWidth)
        {
            return true;
        }
    }

    return false;
}

bool VF_SuggestLandingPoint(
    ECaveGeneratorType Archetype,
    const FStrateGenerationParams& CaveParams,
    const FSlabGenerationParams& SlabParams,
    int32 Seed,
    float StrateTopZ,
    float StrateBottomZ,
    float DesiredX,
    float DesiredY,
    float MaxLateralSnap,
    FVector& OutPoint,
    int32 WorldSeed)
{
    switch (Archetype)
    {
    case ECaveGeneratorType::TunnelNetwork:
    case ECaveGeneratorType::Underwater:
        return VF_FindNearestHashRoomLandingPoint(
            CaveParams, Seed,
            WorldSeed == MIN_int32 ? Seed : WorldSeed,
            StrateTopZ, StrateBottomZ,
                                                  DesiredX, DesiredY, MaxLateralSnap, OutPoint);

    case ECaveGeneratorType::FlatPlain:
    case ECaveGeneratorType::CrystalChamber:
        return VF_SuggestSlabLandingPoint(SlabParams, Seed, StrateTopZ, StrateBottomZ,
                                           DesiredX, DesiredY, MaxLateralSnap, OutPoint);

    default:
        // This overload intentionally covers only archetypes whose placement parameters are
        // already present in the two legacy structs. The complete overload below carries the
        // lattice/grid/blob parameters and answers those sources without guessing.
        return false;
    }
}

bool VF_SuggestLandingPoint(
    ECaveGeneratorType Archetype,
    const FStrateGenerationParams& CaveParams,
    const FSlabGenerationParams& SlabParams,
    const FMazeGenerationParams& MazeParams,
    const FVerticalShaftParams& VerticalShaftParams,
    const FFloatingIslandParams& FloatingIslandParams,
    int32 Seed,
    float StrateTopZ,
    float StrateBottomZ,
    float DesiredX,
    float DesiredY,
    float MaxLateralSnap,
    FVector& OutPoint,
    int32 WorldSeed)
{
    switch (Archetype)
    {
    case ECaveGeneratorType::TunnelNetwork:
    case ECaveGeneratorType::Underwater:
        return VF_FindNearestHashRoomLandingPoint(
            CaveParams, Seed,
            WorldSeed == MIN_int32 ? Seed : WorldSeed,
            StrateTopZ, StrateBottomZ,
                                                  DesiredX, DesiredY, MaxLateralSnap, OutPoint);

    case ECaveGeneratorType::FlatPlain:
    case ECaveGeneratorType::CrystalChamber:
        return VF_SuggestSlabLandingPoint(SlabParams, Seed, StrateTopZ, StrateBottomZ,
                                           DesiredX, DesiredY, MaxLateralSnap, OutPoint);

    case ECaveGeneratorType::Maze:
        return VF_SuggestMazeLandingPoint(MazeParams, Seed, StrateTopZ, StrateBottomZ,
                                          DesiredX, DesiredY, MaxLateralSnap, OutPoint);

    case ECaveGeneratorType::VerticalShafts:
        return VF_SuggestVerticalShaftLandingPoint(VerticalShaftParams, Seed,
                                                   StrateTopZ, StrateBottomZ,
                                                   DesiredX, DesiredY, MaxLateralSnap, OutPoint);

    case ECaveGeneratorType::FloatingIslands:
        return VF_SuggestFloatingIslandLandingPoint(FloatingIslandParams, Seed,
                                                    StrateTopZ, StrateBottomZ,
                                                    DesiredX, DesiredY, MaxLateralSnap, OutPoint);

    case ECaveGeneratorType::SurfaceWorld:
        // The structural heightfield is analytically seed-based, but production SurfaceWorld
        // resolves biome-selected surface params and per-column overhang state through the
        // manager/cache path. This pure API has no biome context and must not query it.
        return false;

    default:
        return false;
    }
}

//=============================================================================
// PHASE 1: BUILD CHUNK CACHE
//=============================================================================
// Collects all rooms in the COLLECT region, computes a window-invariant
// nearest-neighbor backbone for connectivity, decides tunnel connections, and
// pre-computes all tunnel geometry (radii, floor-aligned wandering chains, bounding
// spheres). Only rooms/tunnels relevant to the chunk (STORE region) are kept.
//
// The result is stored in OutCache and reused for every voxel in the chunk.

void VoxelCaveMorphology::BuildChunkCache(
    FChunkSDFCache& OutCache,
    float SearchMinX, float SearchMinY,
    float SearchMaxX, float SearchMaxY,
    const FStrateGenerationParams& Params,
    uint32 Seed, int32 StrateIndex,
    const TArray<FStrateTerrainOpEntry>* TerrainOps)
{
    // Clear previous data (arrays keep their allocation for reuse)
    OutCache.Rooms.Reset();
    OutCache.RoomFloorJoins.Reset();
    OutCache.Tunnels.Reset();
    OutCache.Pits.Reset();
    OutCache.Chimneys.Reset();
    OutCache.Columns.Reset();
    OutCache.SupportColumnMinX = 0;
    OutCache.SupportColumnMinY = 0;
    OutCache.SupportColumnCellsX = 0;
    OutCache.SupportColumnCellsY = 0;
    OutCache.SupportColumnEntries.Reset();
    OutCache.SupportColumns.Reset();

    // Combine world seed with strate index so each strate gets unique caves
    const uint32 StrateSeed = VoxelCaveMorphology::MakeStrateSeed(Seed, StrateIndex);

    const float CellSize = Params.RoomSpacing;
    if (CellSize <= 0.0f) return;

    //=========================================================================
    // INFLUENCE RADII
    //=========================================================================
    // MaxInfluence = how far a room body / tunnel TUBE reaches PERPENDICULAR to its
    // anchor — NOT its length. A room or tunnel whose anchor lies within MaxInfluence
    // of a box can touch a voxel inside that box.
    // Envelope conservatif / conservative bound: Lerp accepts inverted endpoints,
    // so max(Min, Max) covers either radius without changing the authored roll.
    const float RoomRadiusEnvelope = FMath::Max(
        FMath::Abs(Params.MinRoomRadius), FMath::Abs(Params.MaxRoomRadius));
    // Interior control points can carry the authored radius variation (+/-18%). Include that
    // reach in collection; the per-tunnel index below uses each baked radius exactly.
    const float TunnelRadiusEnvelope = FMath::Max(
        0.5f,
        FMath::Max(FMath::Abs(Params.TunnelMinRadius), FMath::Abs(Params.TunnelMaxRadius))
            * 1.18f);
    const float FloorReliefEnvelope = FMath::Abs(Params.FloorReliefStrength)
        * VOXEL_NOISE_SCALE * 1.5f;
    const float BlendEnvelope = FMath::Max(Params.SDFBlendRadius, 0.0f);
    const float MaxInfluence = FMath::Max(
        RoomRadiusEnvelope + FloorReliefEnvelope,
        FMath::Abs(Params.TunnelWarpStrength) + TunnelRadiusEnvelope
    ) + BlendEnvelope * 3.0f;

    const float MaxTunnelLen = FMath::Max(Params.MaxTunnelLength, 0.0f);

    //=========================================================================
    // STORE decision — what we keep for the per-voxel loop.
    //=========================================================================
    // A room/tunnel is stored iff its OWN influence sphere can overlap the search box
    // (RoomReachesSearchBox / tunnel bounding spheres below). Per-voxel culling refines.

    //=========================================================================
    // COLLECT region — what we hash into existence for the connectivity decision.
    //=========================================================================
    // To DECIDE the graph identically in neighboring chunks, we must see, for every
    // room that could emit a tunnel touching this chunk, that room's ENTIRE
    // nearest-neighbor candidate set (all rooms within MaxTunnelLength of it):
    //   - A tunnel touching the chunk has BOTH endpoints within
    //     (MaxTunnelLength + MaxInfluence) of the search box (capsule len <= MaxTunnelLength).
    //   - Each endpoint's NN candidates lie within MaxTunnelLength of that endpoint.
    //   => collect within (2 * MaxTunnelLength + MaxInfluence) of the search box.
    // Combined with NN candidates being filtered to <= MaxTunnelLength below, this
    // makes the backbone decision for any STORED tunnel window-invariant.
    const float CollectMargin = 2.0f * MaxTunnelLen + MaxInfluence;
    const float CollectMinX = SearchMinX - CollectMargin;
    const float CollectMinY = SearchMinY - CollectMargin;
    const float CollectMaxX = SearchMaxX + CollectMargin;
    const float CollectMaxY = SearchMaxY + CollectMargin;

    // Convert COLLECT bounds to cell range
    const int32 CellMinX = FMath::FloorToInt(CollectMinX / CellSize);
    const int32 CellMaxX = FMath::FloorToInt(CollectMaxX / CellSize);
    const int32 CellMinY = FMath::FloorToInt(CollectMinY / CellSize);
    const int32 CellMaxY = FMath::FloorToInt(CollectMaxY / CellSize);

    //=========================================================================
    // Vertical range for room CENTER placement.
    // PLACEMENT CONTRACT: this formula MUST remain identical to
    // VF_FindNearestHashRoomLandingPoint above. The pure landing query deliberately does not call this
    // per-chunk cache builder during Initialize, so the fixed
    // VoxelForge.Determinism.PassageLandsInOpenSpace ring test is the guard that catches drift
    // between the two sites.
    //=========================================================================
    // Buffer = seal thickness + max room half-height.
    // This guarantees the tallest possible room (RoomRadiusEnvelope * RoomHeightRatio)
    // fits entirely within the seal boundary — no room gets its ceiling or floor
    // cut flat by the seal. Smaller rooms have proportionally more margin.
    const float RoomZBuffer = RoomRadiusEnvelope * Params.RoomHeightRatio;
    const float StrateMinZ  = Params.StrateBottomWorldZ + Params.BoundarySealThickness + RoomZBuffer;
    const float StrateMaxZ  = Params.StrateTopWorldZ   - Params.BoundarySealThickness - RoomZBuffer;
    const float StrateRangeZ = StrateMaxZ - StrateMinZ;
    const float StrateCenterZ = (StrateMinZ + StrateMaxZ) * 0.5f;

    const float BlendK = Params.SDFBlendRadius;

    // "This room can reach the search box" test, using the room's OWN reach. The old test
    // compared the center against a box inflated
    // by the SHARED MaxInfluence, which silently assumed every room's reach <= MaxInfluence.
    // That's FALSE for the origin room (OriginRoomRadius >> MaxRoomRadius) — chunks inside
    // the big room but > MaxInfluence from (0,0) didn't store it, so its carve clipped at an
    // arbitrary chunk-aligned radius — and slightly false even for hash rooms (1.5x stretch).
    const auto RoomShapeReachUpperBound = [](float RadiusXY, float RadiusZ) -> float
    {
        const float RXY = FMath::Abs(RadiusXY);
        const float RZ = FMath::Abs(RadiusZ);
        const float Ellipsoid = FMath::Max(RXY, RZ);
        const FVector BoxExtent(
            RXY * 0.8f + RXY * 0.25f,
            RXY * 0.8f + RXY * 0.25f,
            RZ * 0.8f + RXY * 0.25f);
        const float RoundedBox = BoxExtent.Size();
        const float Capsule = RXY * 0.7f + FMath::Min(RXY * 0.6f, RZ);
        return FMath::Max3(Ellipsoid, RoundedBox, Capsule);
    };

    auto RoomReachesSearchBox = [&](const FVector& C, float RadiusXY, float RadiusZ) -> bool
    {
        const float ShapeReach = RoomShapeReachUpperBound(RadiusXY, RadiusZ);
        const float Reach = ShapeReach + FloorReliefEnvelope + BlendEnvelope * 3.0f;
        const float dx = FMath::Max3((float)(SearchMinX - C.X), 0.0f, (float)(C.X - SearchMaxX));
        const float dy = FMath::Max3((float)(SearchMinY - C.Y), 0.0f, (float)(C.Y - SearchMaxY));
        return (dx * dx + dy * dy) <= Reach * Reach;
    };

    // Sphere-vs-search-box test in XY (treated as infinite in Z; per-voxel culling
    // resolves Z). Used to decide whether a tunnel is worth storing for this chunk.
    auto SphereTouchesSearchXY = [&](const FVector& C, float RSq) -> bool
    {
        const float dx = FMath::Max3((float)(SearchMinX - C.X), 0.0f, (float)(C.X - SearchMaxX));
        const float dy = FMath::Max3((float)(SearchMinY - C.Y), 0.0f, (float)(C.Y - SearchMaxY));
        return (dx * dx + dy * dy) <= RSq;
    };

    // Temporary array with cell coordinates for tunnel connection decisions
    TArray<FBuildRoom, TInlineAllocator<64>> BuildRooms;

    // --- ORIGIN ROOM ---
    // Guaranteed large origin landing at (0, 0) in each strate — the future shaft landing and
    // graph root room. Inter-strate passage mouths are placed elsewhere and have no radial road.
    // Collected whenever (0,0) is inside the COLLECT region so it participates in the
    // connectivity decision; only stored if it can reach this chunk.
    int32 OriginIdx = -1;
    if (Params.OriginRoomRadius > 0.0f)
    {
        // Collected when (0,0) is in the COLLECT region (connectivity) OR when the room's
        // own body can reach this chunk (store) — its radius may exceed the collect margin.
        const bool bOriginInCollect =
            0.0f >= CollectMinX && 0.0f <= CollectMaxX &&
            0.0f >= CollectMinY && 0.0f <= CollectMaxY;
        const float OriginRZ = Params.OriginRoomRadius * Params.RoomHeightRatio;
        if (bOriginInCollect ||
            RoomReachesSearchBox(FVector(0.0f, 0.0f, StrateCenterZ), Params.OriginRoomRadius, OriginRZ))
        {
            FBuildRoom OriginRoom;
            OriginRoom.CellX = INT32_MAX;  // Sentinel — never matches a real grid cell
            OriginRoom.CellY = INT32_MAX;
            OriginRoom.Hash = VoxelHash::Cell(0, 0, StrateSeed ^ 0x0A161Cu);
            OriginRoom.Center = FVector(0.0f, 0.0f, StrateCenterZ);
            OriginRoom.RadiusXY = Params.OriginRoomRadius;
            OriginRoom.RadiusZ = Params.OriginRoomRadius * Params.RoomHeightRatio;
            OriginRoom.bIsOrigin = true;
            OriginRoom.bStore = RoomReachesSearchBox(OriginRoom.Center, OriginRoom.RadiusXY, OriginRoom.RadiusZ);
            OriginIdx = BuildRooms.Add(OriginRoom);
        }
    }

    // --- HASH-BASED ROOMS ---
    for (int32 CY = CellMinY; CY <= CellMaxY; CY++)
    {
        for (int32 CX = CellMinX; CX <= CellMaxX; CX++)
        {
            // Hash this cell to decide if it has a room
            const uint32 CellHash = VoxelHash::Cell(CX, CY, StrateSeed);
            const float RoomChance = VoxelHash::ToFloat01(CellHash);

            // Skip empty cells (no room here)
            if (RoomChance >= Params.RoomDensity) continue;

            // Room position: jittered within the cell
            const float JitterX = VoxelHash::ToFloat01(VoxelHash::Mix(CellHash ^ 0x12345678u));
            const float JitterY = VoxelHash::ToFloat01(VoxelHash::Mix(CellHash ^ 0x9ABCDEF0u));
            const float JitterZ = VoxelHash::ToFloat01(VoxelHash::Mix(CellHash ^ 0x55AA55AAu));

            FBuildRoom Room;
            Room.CellX = CX;
            Room.CellY = CY;
            Room.Hash = CellHash;
            Room.Center.X = (CX + 0.15f + JitterX * 0.7f) * CellSize;
            Room.Center.Y = (CY + 0.15f + JitterY * 0.7f) * CellSize;
            Room.Center.Z = StrateMinZ + JitterZ * FMath::Max(StrateRangeZ, 1.0f);

            // Room size: lerp between min and max
            const float SizeFactor = VoxelHash::ToFloat01(VoxelHash::Mix(CellHash ^ 0xFEDCBA98u));
            Room.RadiusXY = FMath::Lerp(Params.MinRoomRadius, Params.MaxRoomRadius, SizeFactor);
            Room.RadiusZ = Room.RadiusXY * Params.RoomHeightRatio;
            Room.bIsOrigin = false;
            Room.bStore = RoomReachesSearchBox(Room.Center, Room.RadiusXY, Room.RadiusZ);

            BuildRooms.Add(Room);
        }
    }

    const int32 NumRooms = BuildRooms.Num();
    if (NumRooms == 0)
    {
        VF_FinalizeCacheBroadPhaseBounds(OutCache, BlendK);
        return;
    }

    // Keep the room floor calculation in one place so tunnel mouths and overlap joins use the
    // same deterministic support plane as the emitted room. It includes the room-centre relief;
    // the evaluator still applies the spatial relief field per voxel around that anchor.
    const auto RoomFloorZFor = [&Params, Seed](const FBuildRoom& Room) -> float
    {
        return VF_RoomFloorZFromParts(
            Room.Center, Room.RadiusXY, Room.RadiusZ, Room.Hash, Seed, Params);
    };

    //=========================================================================
    // Window-invariant guaranteed backbone
    //=========================================================================
    // Each room gets ONE guaranteed link, chosen among candidates within MaxTunnelLength
    // (that reach filter is what keeps the decision identical across chunk windows).
    //
    // bTunnelsFlowTowardOrigin = true (default): the link target is the best candidate
    // among rooms STRICTLY CLOSER to (0,0) in XY. Every chain of links then descends in
    // origin-distance and terminates at the origin room → the network is a TREE ROOTED AT
    // THE SPINE HUB: every room is reachable, tunnels flow inward like tributaries.
    // (Frontier rooms with no closer candidate in reach fall back to plain NN — a far
    // cluster stays internally chained even when it can't bridge to the origin side.)
    //
    // bTunnelsFlowTowardOrigin = false (legacy): plain nearest-neighbor pairing. NOTE:
    // despite what this comment used to claim, an NN-graph is a FOREST of small clusters,
    // not a connected tree — isolated cave pockets are expected in this mode.
    //
    // Selection metric (not the reach filter) penalizes vertical separation via
    // TunnelHorizontalBias, so the GUARANTEED links also prefer walkable slopes —
    // previously only the random TunnelDensity extras were biased, which is why
    // backbone tunnels could come out absurdly steep.
    TArray<int32, TInlineAllocator<64>> NearestNeighbor;
    NearestNeighbor.SetNumUninitialized(NumRooms);

    const float MaxTunnelLenSq = MaxTunnelLen * MaxTunnelLen;
    const bool bFlowToOrigin = Params.bTunnelsFlowTowardOrigin;

    auto LinkMetric = [&](int32 I, int32 J) -> float
    {
        const float D = FVector::Dist(BuildRooms[I].Center, BuildRooms[J].Center);
        const float VertSep = FMath::Abs(BuildRooms[I].Center.Z - BuildRooms[J].Center.Z);
        return D + VertSep * Params.TunnelHorizontalBias * 5.0f;
    };
    // Squared XY distance to the (0,0) spine — the "inward" ordering. Purely positional,
    // so it is window-invariant by construction.
    auto OriginKeySq = [&](int32 I) -> float
    {
        const FVector& C = BuildRooms[I].Center;
        return C.X * C.X + C.Y * C.Y;
    };

    // ExcludeJ: used by the origin-cap redirect below (re-pick ignoring the origin room).
    auto PickNeighbor = [&](int32 I, int32 ExcludeJ) -> int32
    {
        const float MyKeySq = OriginKeySq(I);
        float BestInward = FLT_MAX;  int32 BestInwardJ = -1;
        float BestAny    = FLT_MAX;  int32 BestAnyJ    = -1;
        for (int32 J = 0; J < NumRooms; J++)
        {
            if (J == I || J == ExcludeJ) continue;
            const float DSq = FVector::DistSquared(BuildRooms[I].Center, BuildRooms[J].Center);
            if (DSq > MaxTunnelLenSq) continue;       // out of reach — never a tunnel
            const float M = LinkMetric(I, J);
            if (M < BestAny) { BestAny = M; BestAnyJ = J; }
            if (bFlowToOrigin && OriginKeySq(J) < MyKeySq && M < BestInward)
            {
                BestInward = M; BestInwardJ = J;
            }
        }
        return (bFlowToOrigin && BestInwardJ != -1) ? BestInwardJ : BestAnyJ;
    };

    for (int32 I = 0; I < NumRooms; I++)
    {
        NearestNeighbor[I] = PickNeighbor(I, /*ExcludeJ=*/INDEX_NONE);
    }

    //=========================================================================
    // ORIGIN CONNECTION CAP (deterministic, order-independent)
    //=========================================================================
    // OriginRoomMaxConnections caps how many rooms backbone-force into the origin.
    // The OLD approach counted connections in pair-loop order, which depended on the
    // per-chunk room ordering → non-deterministic across chunks. Instead we gather
    // ALL rooms backbone-linked to origin (window-invariant given the COLLECT region),
    // rank them by a deterministic pair hash, and keep only the top N as forced.
    // The rest are DOWNGRADED to the random TunnelDensity path (connectivity not
    // broken, only the "guaranteed" aspect is limited).
    TSet<int32> OriginDowngraded;
    const int32 MaxOriginConn = Params.OriginRoomMaxConnections;
    if (OriginIdx >= 0 && MaxOriginConn > 0)
    {
        // Collect origin backbone candidates with a deterministic ranking key.
        TArray<TPair<uint32, int32>, TInlineAllocator<32>> OriginLinks;
        for (int32 I = 0; I < NumRooms; I++)
        {
            if (I == OriginIdx) continue;
            const bool bLinked = (NearestNeighbor[I] == OriginIdx) || (NearestNeighbor[OriginIdx] == I);
            if (!bLinked) continue;
            const uint32 Key = VoxelHash::Pair(
                BuildRooms[OriginIdx].CellX, BuildRooms[OriginIdx].CellY,
                BuildRooms[I].CellX, BuildRooms[I].CellY,
                StrateSeed ^ 0x031A1Eu);
            OriginLinks.Add(TPair<uint32, int32>(Key, I));
        }
        // Stable deterministic order by (hash, then index for tie-break).
        OriginLinks.Sort([](const TPair<uint32, int32>& A, const TPair<uint32, int32>& B)
        {
            return A.Key != B.Key ? A.Key < B.Key : A.Value < B.Value;
        });
        for (int32 R = MaxOriginConn; R < OriginLinks.Num(); ++R)
        {
            OriginDowngraded.Add(OriginLinks[R].Value);
        }

        // REDIRECT, don't strand: a downgraded room whose guaranteed link pointed at the
        // origin re-picks its best target EXCLUDING origin. It keeps a guaranteed link
        // (chains to the hub through another room instead of directly), which matters
        // doubly now that rooms with zero connections are culled below. Deterministic:
        // same candidate set, same metric, one exclusion.
        for (int32 DowngradedI : OriginDowngraded)
        {
            if (NearestNeighbor[DowngradedI] == OriginIdx)
            {
                NearestNeighbor[DowngradedI] = PickNeighbor(DowngradedI, /*ExcludeJ=*/OriginIdx);
            }
        }
    }

    //=========================================================================
    // Resolve tunnel connections, pre-compute geometry, store chunk-relevant ones
    //=========================================================================
    // A tunnel exists if EITHER:
    //   1. One is the other's nearest neighbor (backbone — guarantees connectivity),
    //      and (for origin links) it survived the origin cap, OR
    //   2. The pair hash passes TunnelDensity (random extra loops).
    // Both must pass the distance check (MaxTunnelLength). Only tunnels whose bounding
    // sphere reaches the search box are stored for the per-voxel loop.
    // Tracks whether each room ends up with at least one tunnel — DECIDED connections,
    // independent of whether the tunnel itself is stored for this chunk (a room near the
    // window edge may have all its tunnels outside the box; it's still "connected").
    // Stored rooms with zero connections are culled at emission: they'd be sealed air
    // pockets no tunnel ever reaches. Window-invariant: a stored room's full candidate
    // set (and each candidate's own candidates) lies inside the COLLECT region.
    TArray<bool, TInlineAllocator<64>> RoomConnected;
    RoomConnected.Init(false, NumRooms);

    const auto ResolvePlayerFitPoint = [](FBuildRoom& Room, const FStrateGenerationParams& InParams,
                                          uint32 InWorldSeed) -> bool
    {
        if (Room.bIsOrigin)
        {
            return false;
        }
        if (Room.bPlayerFitAttempted)
        {
            return Room.bHasPlayerFitPoint;
        }
        Room.bPlayerFitAttempted = true;
        const FVFRoomLandingSite Site{
            Room.Center,
            Room.RadiusXY,
            Room.RadiusZ,
            Room.Hash,
            false};
        Room.bHasPlayerFitPoint = VF_FindPlayerFitPointForRoom(
            InParams,
            Site,
            InParams.StrateTopWorldZ,
            InParams.StrateBottomWorldZ,
            InWorldSeed,
            Room.PlayerFitPoint);
        return Room.bHasPlayerFitPoint;
    };

    for (int32 I = 0; I < NumRooms; I++)
    {
        for (int32 J = I + 1; J < NumRooms; J++)
        {
            const FBuildRoom& RoomA = BuildRooms[I];
            const FBuildRoom& RoomB = BuildRooms[J];
            bool bBackbone = (NearestNeighbor[I] == J) || (NearestNeighbor[J] == I);

            // Origin cap: downgrade backbone links beyond the deterministic top-N.
            if (bBackbone && (RoomA.bIsOrigin || RoomB.bIsOrigin))
            {
                const int32 Other = RoomA.bIsOrigin ? J : I;
                if (OriginDowngraded.Contains(Other))
                {
                    bBackbone = false;  // Let TunnelDensity decide instead
                }
            }

            // --- DISTANCE CHECK ---
            const float EuclidDist = FVector::Dist(RoomA.Center, RoomB.Center);
            float CheckDist = EuclidDist;

            // Horizontal bias: penalize vertical separation for non-backbone tunnels
            if (!bBackbone && Params.TunnelHorizontalBias > 0.0f)
            {
                const float VertSep = FMath::Abs(RoomA.Center.Z - RoomB.Center.Z);
                CheckDist += VertSep * Params.TunnelHorizontalBias * 5.0f;
            }

            if (CheckDist > Params.MaxTunnelLength) continue;

            // --- CONNECTION DECISION ---
            if (!bBackbone)
            {
                const uint32 PairHash = VoxelHash::Pair(
                    RoomA.CellX, RoomA.CellY,
                    RoomB.CellX, RoomB.CellY,
                    StrateSeed
                );
                const float ConnectChance = VoxelHash::ToFloat01(PairHash);
                if (ConnectChance >= Params.TunnelDensity) continue;
            }

            // Connection DECIDED (backbone or density roll) — both rooms are reachable.
            RoomConnected[I] = true;
            RoomConnected[J] = true;

            // --- TUNNEL HASH (for deriving all tunnel properties) ---
            const uint32 TunnelHash = VoxelHash::Pair(
                RoomA.CellX, RoomA.CellY,
                RoomB.CellX, RoomB.CellY,
                StrateSeed ^ 0xDECAF001u
            );

            // --- RADIUS ---
            const float FactorA = VoxelHash::ToFloat01(VoxelHash::Mix(TunnelHash ^ 0xBAADF00Du));
            const float FactorB = VoxelHash::ToFloat01(VoxelHash::Mix(TunnelHash ^ 0x8BADF00Du));
            const float RadA = FMath::Lerp(Params.TunnelMinRadius, Params.TunnelMaxRadius, FactorA);
            const float RadB = FMath::Lerp(Params.TunnelMinRadius, Params.TunnelMaxRadius, FactorB);

            // --- ENDPOINT FLOOR ALIGNMENT ---
            // The old endpoint Z roll entered each room at an arbitrary height. When the two
            // rooms had different floor cuts, the tube then met one floor several metres above or
            // below the other and the player-fit graph saw a vertical severance. Anchor each tube
            // tangent to the two rooms' deterministic floor planes instead. Interior control
            // points still receive bounded vertical wander, so this removes the lip without
            // turning the whole network into a level grid.
            const float RoomFloorA = RoomFloorZFor(RoomA);
            const float RoomFloorB = RoomFloorZFor(RoomB);
            const float SafeFloorA = FMath::IsFinite(RoomFloorA)
                ? RoomFloorA : RoomA.Center.Z - RoomA.RadiusZ;
            const float SafeFloorB = FMath::IsFinite(RoomFloorB)
                ? RoomFloorB : RoomB.Center.Z - RoomB.RadiusZ;
            FVector EndA = RoomA.Center + FVector(0.0f, 0.0f, SafeFloorA
                + RadA - RoomA.Center.Z);
            FVector EndB = RoomB.Center + FVector(0.0f, 0.0f, SafeFloorB
                + RadB - RoomB.Center.Z);

            // The passage landing query is allowed to move inside a warped room until the
            // capsule actually fits.  Use that same deterministic fit anchor for graph mouths;
            // otherwise a room whose centre needs a local correction gets a tunnel mouth at one
            // point and the inter-strate/player-fit landing at another.  Querying with zero
            // lateral budget pins the source site to this exact room centre (the helper still
            // performs its bounded local fit search).  If a malformed asset cannot produce a
            // source fit, retain the analytic room-floor fallback above.  PlayerFitPoint is a
            // feet point; the tunnel floor's authored support band ends half a voxel below it,
            // and the route probe stands one voxel above the tunnel-floor plane.  Subtracting a
            // full voxel here makes that probe exactly the same feet point as the room landing.
            FVector WorldEndA = VF_UnwarpCavePoint(EndA, Params, Seed);
            FVector WorldEndB = VF_UnwarpCavePoint(EndB, Params, Seed);
            if (!RoomA.bIsOrigin)
            {
                if (ResolvePlayerFitPoint(
                        BuildRooms[I], Params, Seed))
                {
                    WorldEndA = FVector(
                        RoomA.PlayerFitPoint.X,
                        RoomA.PlayerFitPoint.Y,
                        RoomA.PlayerFitPoint.Z - 1.0f + RadA);
                    EndA = VF_ApplyCaveWarp(WorldEndA, Params, Seed);
                }
            }
            if (!RoomB.bIsOrigin)
            {
                if (ResolvePlayerFitPoint(
                        BuildRooms[J], Params, Seed))
                {
                    WorldEndB = FVector(
                        RoomB.PlayerFitPoint.X,
                        RoomB.PlayerFitPoint.Y,
                        RoomB.PlayerFitPoint.Z - 1.0f + RadB);
                    EndB = VF_ApplyCaveWarp(WorldEndB, Params, Seed);
                }
            }

            // --- BUILD CACHED TUNNEL ---
            FCachedTunnel CT;
            CT.EndpointA = EndA;
            CT.EndpointB = EndB;
            CT.RadiusA = RadA;
            CT.RadiusB = RadB;

            // A room edge is not a straight capsule between cell centres. Build a deterministic
            // chain of hash-jittered control points instead. The chain is keyed only by the pair
            // hash, so every chunk that collects this edge reconstructs the same path; the wide
            // collect region above keeps that topology seam-safe. The envelope is zero at both
            // mouths so the room/tunnel join remains anchored, while the interior is allowed to
            // wander in the horizontal plane perpendicular to the tunnel axis.
            //
            // The cache stores the graph in SDF coordinates, but the player walks in world
            // coordinates. Building the chain directly in SDF space made the non-linear cave warp
            // bend the floor and could turn a modest authored slope into a vertical severance.
            // Pick the exact world-space mouth anchors by inversion, lay out the wandering chain
            // there, then map each control point back into SDF space. This keeps the generated
            // route deterministic and seam-safe while making its walkable floor the authored
            // world-space interpolation between the two room floors.
            const float TunnelLength = FVector::Dist(WorldEndA, WorldEndB);
            const int32 WanderSegments = TunnelLength > 1.0f
                ? FMath::Clamp(FMath::CeilToInt(TunnelLength / 48.0f), 3, 12)
                : 1;
            CT.ControlPoints.Reserve(WanderSegments + 1);
            CT.ControlRadii.Reserve(WanderSegments + 1);
            CT.WorldControlPoints.Reserve(WanderSegments + 1);
            CT.WorldControlRadii.Reserve(WanderSegments + 1);

            FVector HorizontalAxis(
                WorldEndB.X - WorldEndA.X, WorldEndB.Y - WorldEndA.Y, 0.0f);
            FVector PerpA;
            if (HorizontalAxis.Normalize())
            {
                PerpA = FVector(-HorizontalAxis.Y, HorizontalAxis.X, 0.0f);
            }
            else
            {
                PerpA = FVector::RightVector;
            }

            const float MaxWander = FMath::Min(
                FMath::Max(Params.TunnelWarpStrength, 0.0f), TunnelLength * 0.25f);
            const float MaxRadiusVariation = 0.18f;
            float PreviousSide = 0.0f;
            for (int32 ControlIndex = 0;
                 ControlIndex <= WanderSegments;
                 ++ControlIndex)
            {
                const float T = static_cast<float>(ControlIndex)
                    / static_cast<float>(WanderSegments);
                const float Envelope = FMath::Sin(T * PI);
                FVector WorldControlPoint = FMath::Lerp(WorldEndA, WorldEndB, T);
                const float RadiusBase = FMath::Lerp(RadA, RadB, T);
                float ControlRadius = RadiusBase;

                if (ControlIndex > 0 && ControlIndex < WanderSegments)
                {
                    const uint32 PointHash = VoxelHash::Mix(
                        TunnelHash ^ (0xBADC0DEu
                            + static_cast<uint32>(ControlIndex) * 0x9E3779B9u));
                    const float RawSide = VoxelHash::ToFloatSigned(
                        VoxelHash::Mix(PointHash ^ 0x13579BDFu));
                    // A short deterministic low-pass keeps the chain organic instead of making
                    // every control point a sharp alternating zig-zag.
                    const float Side = RawSide * 0.65f + PreviousSide * 0.35f;
                    WorldControlPoint += PerpA * (Side * MaxWander * Envelope);
                    PreviousSide = Side;

                    const float RadiusNoise = VoxelHash::ToFloatSigned(
                        VoxelHash::Mix(PointHash ^ 0x5A17EADu));
                    ControlRadius = FMath::Max(
                        0.5f,
                        RadiusBase * (1.0f + RadiusNoise * MaxRadiusVariation * Envelope));
                }
                else if (ControlIndex == 0)
                {
                    PreviousSide = 0.0f;
                }

                // Pin the endpoints after all arithmetic. This is the seam and room-mouth
                // contract: no residual sin(PI) or interpolation rounding moves a mouth.
                if (ControlIndex == 0)
                {
                    WorldControlPoint = WorldEndA;
                    ControlRadius = RadA;
                }
                else if (ControlIndex == WanderSegments)
                {
                    WorldControlPoint = WorldEndB;
                    ControlRadius = RadB;
                }
                CT.WorldControlPoints.Add(WorldControlPoint);
                CT.WorldControlRadii.Add(ControlRadius);
                const FVector ControlPoint = (ControlIndex == 0)
                    ? EndA
                    : ((ControlIndex == WanderSegments)
                        ? EndB
                        : VF_ApplyCaveWarp(WorldControlPoint, Params, Seed));
                CT.ControlPoints.Add(ControlPoint);
                CT.ControlRadii.Add(ControlRadius);
            }

            CT.Midpoint = CT.ControlPoints.Num() > 2
                ? CT.ControlPoints[CT.ControlPoints.Num() / 2]
                : FVector::ZeroVector;
            CT.RadiusMid = CT.ControlRadii.Num() > 2
                ? CT.ControlRadii[CT.ControlRadii.Num() / 2]
                : 0.0f;
            CT.bHasMidpoint = CT.ControlPoints.Num() > 2;

            // Bounding sphere: enclose every control point and the widest local tube. The bound
            // is built from the complete chain, not a midpoint approximation, so culling cannot
            // clip a bend at a chunk edge.
            CT.BoundCenter = FVector::ZeroVector;
            for (const FVector& Point : CT.ControlPoints)
            {
                CT.BoundCenter += Point;
            }
            CT.BoundCenter /= static_cast<float>(FMath::Max(CT.ControlPoints.Num(), 1));
            float MaxTunnelRadius = 0.0f;
            float BoundR = 0.0f;
            for (int32 PointIndex = 0; PointIndex < CT.ControlPoints.Num(); ++PointIndex)
            {
                MaxTunnelRadius = FMath::Max(
                    MaxTunnelRadius, FMath::Abs(CT.ControlRadii[PointIndex]));
                BoundR = FMath::Max(
                    BoundR, FVector::Dist(CT.BoundCenter, CT.ControlPoints[PointIndex]));
            }
            BoundR += MaxTunnelRadius + FMath::Max(BlendK, 0.0f) * 3.0f;
            CT.BoundRadiusSq = BoundR * BoundR;

            // Keep a separate bound for the world-space structural backstop. The SDF and world
            // chains have the same topology but their cave-warped coordinates do not share a
            // useful rejection sphere.
            CT.WorldBoundCenter = FVector::ZeroVector;
            for (const FVector& Point : CT.WorldControlPoints)
            {
                CT.WorldBoundCenter += Point;
            }
            CT.WorldBoundCenter /= static_cast<float>(
                FMath::Max(CT.WorldControlPoints.Num(), 1));
            float WorldBoundR = 0.0f;
            for (int32 PointIndex = 0;
                 PointIndex < CT.WorldControlPoints.Num();
                 ++PointIndex)
            {
                WorldBoundR = FMath::Max(
                    WorldBoundR,
                    FVector::Dist(CT.WorldBoundCenter, CT.WorldControlPoints[PointIndex]));
            }
            // The support predicate reaches LandingFloorThickness below the centreline floor;
            // the world sphere must cover that slab, not only the blended SDF tube.
            WorldBoundR += MaxTunnelRadius + FMath::Max(
                VoxelPassageGeometry::LandingFloorThicknessVoxels,
                FMath::Max(BlendK, 0.0f));
            CT.WorldBoundRadiusSq = WorldBoundR * WorldBoundR;

            // Store if either representation can reach a voxel in this chunk. The warped SDF
            // chain is needed by the room morphology; the world chain is needed by the final
            // walkable-air backstop. Their bounds differ by design, so testing only the former
            // could make a world-space route disappear at a chunk edge.
            if (SphereTouchesSearchXY(CT.BoundCenter, CT.BoundRadiusSq)
                || SphereTouchesSearchXY(CT.WorldBoundCenter, CT.WorldBoundRadiusSq))
            {
                OutCache.Tunnels.Add(CT);
            }
        }
    }

    //==========================================================================
    // FLATTEN INTERSECTING ROOMS
    //==========================================================================
    // A spherical room union is not automatically walkable when the two rooms' floor cuts land
    // at different Z values. For every overlapping, connected room pair, add a short flat bridge
    // only inside their actual horizontal overlap. This is a floor repair, not a graph edge: it
    // does not connect a landing to the origin and it never creates a corridor through unrelated
    // cells. The common plane is the higher of the two existing floors, so it does not cut below a
    // room's authored support; the bridge is admitted only when the shared player/headroom volume
    // still fits below both room ceilings.
    OutCache.RoomFloorJoins.Reserve(NumRooms / 2);
    const float JoinRequiredHeight =
        VoxelPassageGeometry::PlayerHeightVoxels
        + VoxelPassageGeometry::HeadroomVoxels;
    const float JoinMinimumRadius =
        VoxelPassageGeometry::PlayerRadiusVoxels + 0.5f;
    const float JoinCentreClearance =
        VoxelPassageGeometry::PlayerRadiusVoxels + 2.0f;
    for (int32 I = 0; I < NumRooms; ++I)
    {
        const FBuildRoom& RoomA = BuildRooms[I];
        if (!RoomA.bStore || !RoomConnected[I]) continue;

        for (int32 J = I + 1; J < NumRooms; ++J)
        {
            const FBuildRoom& RoomB = BuildRooms[J];
            if (!RoomB.bStore || !RoomConnected[J]) continue;

            const FVector2D DeltaXY(
                RoomB.Center.X - RoomA.Center.X,
                RoomB.Center.Y - RoomA.Center.Y);
            const float DistanceXY = DeltaXY.Size();
            if (DistanceXY <= KINDA_SMALL_NUMBER
                || DistanceXY >= RoomA.RadiusXY + RoomB.RadiusXY)
            {
                continue;
            }

            const float VerticalReach = RoomA.RadiusZ + RoomB.RadiusZ;
            if (FMath::Abs(RoomB.Center.Z - RoomA.Center.Z) >= VerticalReach)
            {
                continue;
            }

            const float FloorA = RoomFloorZFor(RoomA);
            const float FloorB = RoomFloorZFor(RoomB);
            const float CommonFloor = FMath::Max(FloorA, FloorB);
            const float SharedCeiling = FMath::Min(
                RoomA.Center.Z + RoomA.RadiusZ,
                RoomB.Center.Z + RoomB.RadiusZ);
            if (!FMath::IsFinite(CommonFloor)
                || !FMath::IsFinite(SharedCeiling)
                || SharedCeiling - CommonFloor < JoinRequiredHeight)
            {
                continue;
            }

            // Along the centre-centre line, the two projected disks overlap over this interval.
            // Trim both ends so the bridge remains a local room-edge join and leaves each room's
            // player-fit centre untouched for the pure landing query.
            const float OverlapStart = FMath::Max(
                0.0f, DistanceXY - RoomB.RadiusXY);
            const float OverlapEnd = FMath::Min(
                DistanceXY, RoomA.RadiusXY);
            const float OverlapLength = OverlapEnd - OverlapStart;
            if (OverlapLength < 2.0f * JoinCentreClearance)
            {
                continue;
            }

            const float EdgeInset = FMath::Min(2.0f, OverlapLength * 0.2f);
            const float StartDistance = FMath::Max(
                OverlapStart + EdgeInset, JoinCentreClearance);
            const float EndDistance = FMath::Min(
                OverlapEnd - EdgeInset, DistanceXY - JoinCentreClearance);
            if (EndDistance <= StartDistance)
            {
                continue;
            }

            const float JoinRadius = FMath::Min(
                FMath::Min(RoomA.RadiusXY, RoomB.RadiusXY) * 0.35f,
                (EndDistance - StartDistance) * 0.5f);
            if (!FMath::IsFinite(JoinRadius) || JoinRadius < JoinMinimumRadius)
            {
                continue;
            }

            const FVector2D AxisXY = DeltaXY / DistanceXY;
            const float JoinCentreZ = CommonFloor + 0.5f * JoinRequiredHeight;
            FCachedRoomFloorJoin Join;
            Join.Start = FVector(
                RoomA.Center.X + AxisXY.X * StartDistance,
                RoomA.Center.Y + AxisXY.Y * StartDistance,
                JoinCentreZ);
            Join.End = FVector(
                RoomA.Center.X + AxisXY.X * EndDistance,
                RoomA.Center.Y + AxisXY.Y * EndDistance,
                JoinCentreZ);
            Join.Radius = JoinRadius;
            Join.FloorZ = CommonFloor;
            Join.CeilingZ = CommonFloor + JoinRequiredHeight;
            Join.BoundCenter = (Join.Start + Join.End) * 0.5f;
            const float BoundRadius = 0.5f * FVector::Dist(Join.Start, Join.End)
                + FMath::Abs(Join.Radius)
                + FMath::Max(BlendK, 0.0f) * 3.0f;
            Join.BoundRadiusSq = BoundRadius * BoundRadius;

            if (SphereTouchesSearchXY(Join.BoundCenter, Join.BoundRadiusSq))
            {
                OutCache.RoomFloorJoins.Add(MoveTemp(Join));
            }
        }
    }

    //=========================================================================
    // Copy STORE-relevant rooms to cache (cull radii + per-room terrain ops),
    // then pre-bake their pits / chimneys / columns.
    //=========================================================================
    // Build terrain op pool stats once (outside the per-room loop).
    // TerrainOps is the strate's probability pool; each entry has a Probability
    // in [0,1]. We do a weighted random draw per room using the room's hash.
    //
    // PROBABILITY MODEL:
    //   - Entries are checked cumulatively (like drawing from a bucket).
    //   - The "no op" slot takes the remaining probability space (if sum < 1.0).
    //   - If sum >= 1.0, every room gets an op (normalized selection).
    float TotalOpProb = 0.0f;
    if (TerrainOps)
    {
        for (const FStrateTerrainOpEntry& E : *TerrainOps)
            TotalOpProb += FMath::Max(E.Probability, 0.0f);
    }
    const float NormFactor = (TotalOpProb > 1.0f) ? (1.0f / TotalOpProb) : 1.0f;

    // Temporary params struct for reading op fields (PitDensity, PitMinRadius, etc.)
    FStrateGenerationParams OpParams;

    OutCache.Rooms.Reserve(NumRooms);

    for (int32 RoomIdx = 0; RoomIdx < NumRooms; RoomIdx++)
    {
        const FBuildRoom& BR = BuildRooms[RoomIdx];
        if (!BR.bStore) continue;  // Far room — collected for connectivity only

        // Sealed-bubble cull: a room no tunnel ever reaches would be an isolated air
        // pocket — don't carve it at all. The origin landing is always kept (future shaft/root).
        if (!RoomConnected[RoomIdx] && !BR.bIsOrigin) continue;

        FCachedRoom CR;
        CR.Center = BR.Center;
        CR.RadiusXY = BR.RadiusXY;
        CR.RadiusZ = BR.RadiusZ;
        CR.Hash = BR.Hash;
        CR.bIsOrigin = BR.bIsOrigin;

        // Cull radius: max extent the room can reach + blend margin.
        // 1.5x accounts for capsule shapes extending beyond nominal radius.
        float MaxExtent = FMath::Max(
            FMath::Abs(BR.RadiusXY) * 1.5f,
            FMath::Abs(BR.RadiusZ));
        MaxExtent += FloorReliefEnvelope + BlendEnvelope * 3.0f;

        // --- PRE-BAKED SHAPE ---
        // Same hash roll + thresholds + capsule trig the evaluator used to redo PER VOXEL;
        // done once here → EvaluateSDFCached just switches on ShapeType. Bit-identical output.
        {
            const uint32 ShapeHash = VoxelHash::Mix(CR.Hash ^ 0xDEADBEEFu);
            const float ShapeRoll = CR.bIsOrigin ? 0.0f : VoxelHash::ToFloat01(ShapeHash);
            const float BoxThreshold     = 1.0f - Params.RoomShapeVariety * 0.5f;
            const float CapsuleThreshold = 1.0f - Params.RoomShapeVariety * 0.2f;

            if (ShapeRoll >= BoxThreshold && ShapeRoll < CapsuleThreshold)
            {
                // ROUNDED BOX: angular chamber with smooth corners
                CR.ShapeType = 1;
                CR.ShapeA = FVector(CR.RadiusXY * 0.8f, CR.RadiusXY * 0.8f, CR.RadiusZ * 0.8f);
                CR.ShapeR = CR.RadiusXY * 0.25f;
            }
            else if (ShapeRoll >= CapsuleThreshold)
            {
                // ELONGATED CAPSULE: stretched hall/corridor-room
                CR.ShapeType = 2;
                const float DirAngle = VoxelHash::ToFloat01(VoxelHash::Mix(CR.Hash ^ 0xCAFEBABEu)) * 2.0f * PI;
                const float StretchDist = CR.RadiusXY * 0.7f;
                const FVector Dir(FMath::Cos(DirAngle), FMath::Sin(DirAngle), 0.0f);
                CR.ShapeA = CR.Center + Dir * StretchDist;
                CR.ShapeB = CR.Center - Dir * StretchDist;
                CR.ShapeR = FMath::Min(CR.RadiusXY * 0.6f, CR.RadiusZ);
            }
            else
            {
                // ELLIPSOID (default): smooth oval chamber
                CR.ShapeType = 0;
                CR.ShapeA = FVector(CR.RadiusXY, CR.RadiusXY, CR.RadiusZ);
            }

            // Use the actual pre-baked shape's farthest possible distance, not a guessed 1.5x
            // sphere. The old guess underbounded a rounded-box corner when RadiusZ was large.
            float ShapeReach = 0.0f;
            if (CR.ShapeType == 1)
            {
                const float Round = FMath::Abs(CR.ShapeR);
                const FVector Extent(
                    FMath::Abs(CR.ShapeA.X) + Round,
                    FMath::Abs(CR.ShapeA.Y) + Round,
                    FMath::Abs(CR.ShapeA.Z) + Round);
                ShapeReach = Extent.Size();
            }
            else if (CR.ShapeType == 2)
            {
                ShapeReach = 0.5f * FVector::Dist(CR.ShapeA, CR.ShapeB)
                    + FMath::Abs(CR.ShapeR);
            }
            else
            {
                ShapeReach = FMath::Max3(
                    FMath::Abs(CR.ShapeA.X),
                    FMath::Abs(CR.ShapeA.Y),
                    FMath::Abs(CR.ShapeA.Z));
            }
            MaxExtent = ShapeReach + FloorReliefEnvelope + BlendEnvelope * 3.0f;
            CR.CullRadiusSq = MaxExtent * MaxExtent;
        }

        // Flat floor cut: soft floor plane per room, hash-rolled from [Min, Max].
        // SmoothMax applied in EvaluateSDFCached so tunnels/pits don't create hard seams.
        // Sentinel -FLT_MAX means "no cut" so the per-voxel check is a single compare.
        {
            const float FloorRoll = VoxelHash::ToFloat01(VoxelHash::Mix(BR.Hash ^ 0xF100F2u));
            const float FloorCut  = FMath::Lerp(
                FMath::Min(Params.RoomFloorCutMin, Params.RoomFloorCutMax),
                FMath::Max(Params.RoomFloorCutMin, Params.RoomFloorCutMax),
                FloorRoll
            );

            if (FloorCut < 1.0f)
            {
                CR.FloorCutZ            = CR.Center.Z - CR.RadiusZ * FloorCut;
                CR.FloorReliefStrength  = Params.FloorReliefStrength;
                CR.FloorReliefFrequency = Params.FloorReliefFrequency;
                CR.FloorSeed            = VoxelHash::Mix(BR.Hash ^ 0xF100F1u);
            }
            else
            {
                CR.FloorCutZ            = -FLT_MAX;
                CR.FloorReliefStrength  = 0.0f;
                CR.FloorReliefFrequency = 0.015f;
                CR.FloorSeed            = 0;
            }
        }

        // --- PER-ROOM TERRAIN OP SELECTION ---
        if (TerrainOps && TerrainOps->Num() > 0 && TotalOpProb > 0.0f)
        {
            const uint32 OpHash = VoxelHash::Mix(BR.Hash ^ 0x0FEED00u);
            const float Roll = VoxelHash::ToFloat01(OpHash);

            float Cursor = 0.0f;
            for (const FStrateTerrainOpEntry& E : *TerrainOps)
            {
                Cursor += FMath::Max(E.Probability, 0.0f) * NormFactor;
                if (Roll < Cursor)
                {
                    const UVoxelTerrainOpDefinition* Op = E.Operation.Get();
                    if (Op)
                    {
                        CR.RoomOp = Op;
                        CR.RoomOpWeight = E.Weight;
                    }
                    break;
                }
            }
        }

        // ARCHES/PINCHES — all geometry below is a pure function of the room hash and the
        // room-local terrain parameters.  Bake the hash chains and the two trigonometric pairs
        // once per room instead of once per near-surface voxel.  The active decision uses the
        // same `ToFloat01(H) > Density` predicate as the old loop; only its location changes.
        FStrateGenerationParams FeatureParams = Params;
        if (CR.RoomOp)
        {
            CR.RoomOp->ApplyTo(FeatureParams, CR.RoomOpWeight);
        }
        for (int32 i = 0; i < 3; ++i)
        {
            const uint32 AH = VoxelHash::Mix(CR.Hash ^ (0xA4C400u + (uint32)i * 7369u));
            if (VoxelHash::ToFloat01(AH) <= FeatureParams.ArchDensity)
            {
                const uint32 AH2 = VoxelHash::Mix(AH ^ 0xA4C4u);
                const float ArcCX = CR.Center.X
                    + VoxelHash::ToFloatSigned(AH2) * CR.RadiusXY * 0.3f;
                const float ArcCY = CR.Center.Y
                    + VoxelHash::ToFloatSigned(VoxelHash::Mix(AH2)) * CR.RadiusXY * 0.3f;
                const uint32 AH3 = VoxelHash::Mix(AH2 ^ 0xB41Du);
                const float ArcCZ = CR.Center.Z
                    + VoxelHash::ToFloatSigned(AH3) * CR.RadiusZ * 0.4f;
                const uint32 AH4 = VoxelHash::Mix(AH3 ^ 0xCAFEu);
                const float Angle = VoxelHash::ToFloat01(AH4) * PI;
                const float HalfSpan = CR.RadiusXY
                    * (0.5f + VoxelHash::ToFloat01(VoxelHash::Mix(AH4)) * 0.35f);
                const float CosA = FMath::Cos(Angle);
                const float SinA = FMath::Sin(Angle);
                FCachedArch& Arch = CR.Arches[i];
                Arch.bActive = true;
                Arch.EndpointA = FVector(ArcCX - CosA * HalfSpan,
                                         ArcCY - SinA * HalfSpan, ArcCZ);
                Arch.EndpointB = FVector(ArcCX + CosA * HalfSpan,
                                         ArcCY + SinA * HalfSpan, ArcCZ);
                const uint32 AH5 = VoxelHash::Mix(AH4 ^ 0xF00Du);
                Arch.Radius = FMath::Lerp(
                    FeatureParams.ArchMinRadius, FeatureParams.ArchMaxRadius,
                    VoxelHash::ToFloat01(AH5));
                Arch.BaseDensity = FeatureParams.BaseDensity;
            }

            const uint32 PnH = VoxelHash::Mix(CR.Hash ^ (0xF1C400u + (uint32)i * 5417u));
            if (VoxelHash::ToFloat01(PnH) <= FeatureParams.PinchDensity)
            {
                const uint32 PnH2 = VoxelHash::Mix(PnH ^ 0xF1C4u);
                const float PnX = CR.Center.X
                    + VoxelHash::ToFloatSigned(PnH2) * CR.RadiusXY * 0.85f;
                const float PnY = CR.Center.Y
                    + VoxelHash::ToFloatSigned(VoxelHash::Mix(PnH2)) * CR.RadiusXY * 0.85f;
                const uint32 PnH3 = VoxelHash::Mix(PnH2 ^ 0x5432u);
                const float PnZ = CR.Center.Z
                    + VoxelHash::ToFloatSigned(PnH3) * CR.RadiusZ * 0.5f;
                const uint32 PnH4 = VoxelHash::Mix(PnH3 ^ 0x9A3Bu);
                const float PnAngle = VoxelHash::ToFloat01(PnH4) * PI;
                FCachedPinch& Pinch = CR.Pinches[i];
                Pinch.bActive = true;
                Pinch.CenterX = PnX;
                Pinch.CenterY = PnY;
                Pinch.CenterZ = PnZ;
                Pinch.CosAngle = FMath::Cos(PnAngle);
                Pinch.SinAngle = FMath::Sin(PnAngle);
                Pinch.MaxExtent = FMath::Max(
                    FeatureParams.PinchLength, FeatureParams.PinchStrength) + 5.0f;
                Pinch.HalfLength = FeatureParams.PinchLength * 0.5f;
                Pinch.HalfNarrow = FeatureParams.PinchStrength;
                Pinch.HalfVertical = FeatureParams.PinchStrength * 1.5f;
                Pinch.BaseDensity = FeatureParams.BaseDensity;
            }
        }

        OutCache.Rooms.Add(CR);

        //---------------------------------------------------------------------
        // PRE-BAKE: PITS, CHIMNEYS, COLUMNS for this room
        //---------------------------------------------------------------------
        // Pre-baking makes features independent of NearestRoomIdx (no thin "lid"
        // when the owning room flips mid-shaft). Only stored rooms are baked —
        // a far room's features can't reach this chunk anyway.
        if (!CR.RoomOp) continue;

        OpParams = FStrateGenerationParams{};
        CR.RoomOp->ApplyTo(OpParams, CR.RoomOpWeight);

        // PITS — downward shafts anchored in the room's lower half.
        BakeRoomFeature(CR, /*Max*/2, OpParams.PitDensity,
            0xDE1A7Eu, 6271u, 0xABCDu, 0x5EEDu,
            /*XYScale*/0.6f, OpParams.PitMinRadius, OpParams.PitMaxRadius,
            [&](float PX, float PY, float PitRadius, uint32 PH3)
            {
                const uint32 PH4 = VoxelHash::Mix(PH3 ^ 0xF00Du);
                FCachedPit Pit;
                Pit.CenterX       = PX;
                Pit.CenterY       = PY;
                Pit.TopZ          = CR.Center.Z - CR.RadiusZ * 0.5f
                                    + VoxelHash::ToFloat01(PH4) * CR.RadiusZ * 0.2f;
                Pit.Radius        = PitRadius;
                Pit.Depth         = OpParams.PitDepth;
                Pit.FlareDist     = PitRadius * 2.0f;
                Pit.FlareExtra    = PitRadius * 1.0f;
                Pit.BaseDensity   = Params.BaseDensity;
                Pit.BlendK        = Params.SDFBlendRadius;
                const float MaxXYR = PitRadius + PitRadius + Params.SDFBlendRadius + 4.0f;
                Pit.BoundXYRadiusSq = MaxXYR * MaxXYR;
                OutCache.Pits.Add(Pit);
            });

        // CHIMNEYS — mirror of pits: upward tubes anchored in the room's upper half.
        BakeRoomFeature(CR, /*Max*/2, OpParams.ChimneyDensity,
            0xC4F007u, 7919u, 0x1337u, 0xCAFEu,
            /*XYScale*/0.6f, OpParams.ChimneyMinRadius, OpParams.ChimneyMaxRadius,
            [&](float CX, float CY, float ChmRadius, uint32 CH3)
            {
                const uint32 CH4 = VoxelHash::Mix(CH3 ^ 0xD00Du);
                FCachedChimney Chim;
                Chim.CenterX       = CX;
                Chim.CenterY       = CY;
                Chim.BottomZ       = CR.Center.Z + CR.RadiusZ * 0.5f
                                     - VoxelHash::ToFloat01(CH4) * CR.RadiusZ * 0.2f;
                Chim.Radius        = ChmRadius;
                Chim.Height        = OpParams.ChimneyHeight;
                Chim.FlareDist     = ChmRadius * 2.0f;
                Chim.FlareExtra    = ChmRadius * 1.0f;
                Chim.BaseDensity   = Params.BaseDensity;
                Chim.BlendK        = Params.SDFBlendRadius;
                const float MaxXYR = ChmRadius + ChmRadius + Params.SDFBlendRadius + 4.0f;
                Chim.BoundXYRadiusSq = MaxXYR * MaxXYR;
                OutCache.Chimneys.Add(Chim);
            });

        // COLUMNS — full-height solid cylinders (no Z anchor, no flare).
        BakeRoomFeature(CR, /*Max*/4, OpParams.ColumnDensity,
            0xC01C01u, 3571u, 0x1A2B3Cu, 0xBEEFu,
            /*XYScale*/0.75f, OpParams.ColumnMinRadius, OpParams.ColumnMaxRadius,
            [&](float ColX, float ColY, float ColR, uint32 /*H3*/)
            {
                FCachedColumn Col;
                Col.CenterX       = ColX;
                Col.CenterY       = ColY;
                Col.Radius        = ColR;
                Col.BaseDensity   = Params.BaseDensity;
                const float MaxXYR = ColR + 6.0f;
                Col.BoundXYRadiusSq = MaxXYR * MaxXYR;
                OutCache.Columns.Add(Col);
        });
    }

    // All candidate arrays are complete now. Build the immutable broad phase only after every
    // deterministic room/tunnel decision and feature bake has finished.
    VF_FinalizeCacheBroadPhaseBounds(OutCache, BlendK);
}

//=============================================================================
// PHASE 2: EVALUATE SDF WITH CACHED DATA
//=============================================================================
// Pure SDF math — no hashing, no array building, no backbone computation.
// Just loops through the pre-built rooms and tunnels, evaluates distance,
// and smooth-mins everything together. Distance culling skips primitives
// that are clearly too far to contribute.

float VoxelCaveMorphology::EvaluateSDFCached(
    float WorldX, float WorldY, float WorldZ,
    const FChunkSDFCache& Cache,
    float SDFBlendRadius,
    int32* OutNearestRoomIdx)
{
    float MinSDF = FLT_MAX;
    const float BlendK = SDFBlendRadius;
    const FVector Pos(WorldX, WorldY, WorldZ);

    // Track which room contributes the smallest (most-inside) raw SDF.
    // This is used by the terrain ops system to find the "owning" room for
    // this voxel and apply that room's per-room terrain operation.
    // We track raw room SDF (before SmoothMin) so tunnel SDFs don't interfere.
    float NearestRoomRawSDF = FLT_MAX;
    int32 NearestIdx = -1;

    if (VoxelDensityProfile::AreCountersEnabled())
    {
        VoxelDensityProfile::AddCounter(
            VoxelDensityProfile::ECounter::CaveRoomCandidates,
            static_cast<uint64>(Cache.Rooms.Num()));
    }

    //=========================================================================
    // Room SDFs
    //=========================================================================
    auto EvaluateRoom = [&](int32 RoomIdx)
    {
        const FCachedRoom& Room = Cache.Rooms[RoomIdx];

        // --- DISTANCE CULL ---
        const float DistSq = VF_FloatDistSquared(WorldX, WorldY, WorldZ, Room.Center);
        if (DistSq > Room.CullRadiusSq) return;
        if (VoxelDensityProfile::AreCountersEnabled())
        {
            VoxelDensityProfile::AddCounter(
                VoxelDensityProfile::ECounter::CaveRoomEvaluated);
        }

        // --- SHAPE (pre-baked in BuildChunkCache — no per-voxel hash roll / trig) ---
        float RoomSDF;
        switch (Room.ShapeType)
        {
        case 1:  RoomSDF = VoxelSDF::RoundedBox(Pos, Room.Center, Room.ShapeA, Room.ShapeR); break;
        case 2:  RoomSDF = VoxelSDF::Capsule(Pos, Room.ShapeA, Room.ShapeB, Room.ShapeR);    break;
        default: RoomSDF = VoxelSDF::Ellipsoid(Pos, Room.Center, Room.ShapeA);               break;
        }

        // Soft floor: SmoothMax of the room SDF and the floor half-space.
        if (Room.FloorCutZ > -FLT_MAX)
        {
            float FloorZ = Room.FloorCutZ;

            if (Room.FloorReliefStrength > 0.0f)
            {
                const float RF = Room.FloorReliefFrequency;
                const float SF = (float)Room.FloorSeed * 0.00001f;

                float N = FMath::PerlinNoise2D(FVector2D(Pos.X * RF + SF,       Pos.Y * RF + SF * 1.7f)) * 0.65f
                        + FMath::PerlinNoise2D(FVector2D(Pos.X * RF * 2.3f + SF * 3.1f, Pos.Y * RF * 2.3f + SF * 5.3f)) * 0.35f;
                N *= VOXEL_NOISE_SCALE;
                FloorZ += N * Room.FloorReliefStrength;
            }

            RoomSDF = VoxelSDF::SmoothMax(RoomSDF, FloorZ - Pos.Z, BlendK * 0.35f);
        }

        // Track the room whose SDF is smallest (most inside).
        if (OutNearestRoomIdx && RoomSDF < NearestRoomRawSDF)
        {
            NearestRoomRawSDF = RoomSDF;
            NearestIdx = RoomIdx;
        }

        MinSDF = VoxelSDF::SmoothMin(MinSDF, RoomSDF, BlendK);
    };

    for (int32 RoomIdx = 0; RoomIdx < Cache.Rooms.Num(); ++RoomIdx)
    {
        EvaluateRoom(RoomIdx);
    }

    //==========================================================================
    // Flattened room joins
    //==========================================================================
    // These short bridges exist only where two room bodies already overlap. Their floor is a
    // single horizontal plane, so a player crossing the shared volume cannot meet the tall lip
    // produced by two independent floor cuts. They are deliberately not reported as rooms and do
    // not participate in nearest-room terrain-op ownership.
    auto EvaluateJoin = [&](int32 JoinIdx)
    {
        const FCachedRoomFloorJoin& Join = Cache.RoomFloorJoins[JoinIdx];
        const float DistSq = VF_FloatDistSquared(WorldX, WorldY, WorldZ, Join.BoundCenter);
        if (DistSq > Join.BoundRadiusSq) return;

        const float HorizontalSDF = VoxelSDF::Capsule(
            Pos, Join.Start, Join.End, Join.Radius);
        const float VerticalSDF = FMath::Max(
            Join.FloorZ - Pos.Z,
            Pos.Z - Join.CeilingZ);
        const float JoinSDF = FMath::Max(HorizontalSDF, VerticalSDF);
        MinSDF = VoxelSDF::SmoothMin(MinSDF, JoinSDF, BlendK);
    };
    for (int32 JoinIdx = 0; JoinIdx < Cache.RoomFloorJoins.Num(); ++JoinIdx)
    {
        EvaluateJoin(JoinIdx);
    }

    //=========================================================================
    // Tunnel SDFs
    //=========================================================================
    if (VoxelDensityProfile::AreCountersEnabled())
    {
        VoxelDensityProfile::AddCounter(
            VoxelDensityProfile::ECounter::CaveTunnelCandidates,
            static_cast<uint64>(Cache.Tunnels.Num()));
    }

    auto EvaluateTunnel = [&](int32 TunnelIdx)
    {
        const FCachedTunnel& Tunnel = Cache.Tunnels[TunnelIdx];
        if (Tunnel.SDFInfluenceRadius > 0.0f
            && VF_DistanceSquaredToAabb(
                WorldX, WorldY, WorldZ,
                Tunnel.SDFCenterlineMin, Tunnel.SDFCenterlineMax)
                > FMath::Square(Tunnel.SDFInfluenceRadius))
        {
            return;
        }
        // --- BOUNDING SPHERE CULL ---
        const float DistSq = VF_FloatDistSquared(WorldX, WorldY, WorldZ, Tunnel.BoundCenter);
        if (DistSq > Tunnel.BoundRadiusSq) return;
        if (VoxelDensityProfile::AreCountersEnabled())
        {
            VoxelDensityProfile::AddCounter(
                VoxelDensityProfile::ECounter::CaveTunnelEvaluated);
        }

        float TunnelSDF;

        if (Tunnel.ControlPoints.Num() >= 2
            && Tunnel.ControlRadii.Num() == Tunnel.ControlPoints.Num())
        {
            // A wandering chain is evaluated segment-by-segment. Min keeps the shared control
            // point watertight and preserves the old capsule union semantics; the outer
            // SmoothMin is still the room/tunnel blend.
            TunnelSDF = FLT_MAX;
            for (int32 SegmentIndex = 0;
                 SegmentIndex + 1 < Tunnel.ControlPoints.Num();
                 ++SegmentIndex)
            {
                TunnelSDF = FMath::Min(
                    TunnelSDF,
                    VoxelSDF::TaperedCapsule(
                        Pos,
                        Tunnel.ControlPoints[SegmentIndex],
                        Tunnel.ControlPoints[SegmentIndex + 1],
                        Tunnel.ControlRadii[SegmentIndex],
                        Tunnel.ControlRadii[SegmentIndex + 1]));
            }
        }
        else if (Tunnel.bHasMidpoint)
        {
            // Backward-compatible descriptor path for older cooked caches.
            float SegA = VoxelSDF::TaperedCapsule(Pos, Tunnel.EndpointA, Tunnel.Midpoint,
                                                   Tunnel.RadiusA, Tunnel.RadiusMid);
            float SegB = VoxelSDF::TaperedCapsule(Pos, Tunnel.Midpoint, Tunnel.EndpointB,
                                                   Tunnel.RadiusMid, Tunnel.RadiusB);
            TunnelSDF = FMath::Min(SegA, SegB);
        }
        else
        {
            // Backward-compatible straight descriptor path.
            TunnelSDF = VoxelSDF::TaperedCapsule(Pos, Tunnel.EndpointA, Tunnel.EndpointB,
                                                  Tunnel.RadiusA, Tunnel.RadiusB);
        }

        MinSDF = VoxelSDF::SmoothMin(MinSDF, TunnelSDF, BlendK);
    };

    for (int32 TunnelIdx = 0; TunnelIdx < Cache.Tunnels.Num(); ++TunnelIdx)
    {
        EvaluateTunnel(TunnelIdx);
    }

    // Write nearest room index for the caller (terrain ops system)
    if (OutNearestRoomIdx)
    {
        *OutNearestRoomIdx = NearestIdx;
    }

    return MinSDF;
}

float VoxelCaveMorphology::EvaluateTunnelCoreSDF(
    float WorldX, float WorldY, float WorldZ,
    const FChunkSDFCache& Cache)
{
    const FVector Pos(WorldX, WorldY, WorldZ);
    float MinSDF = FLT_MAX;

    auto EvaluateTunnel = [&](int32 TunnelIdx)
    {
        const FCachedTunnel& Tunnel = Cache.Tunnels[TunnelIdx];
        if (Tunnel.SDFInfluenceRadius > 0.0f
            && VF_DistanceSquaredToAabb(
                WorldX, WorldY, WorldZ,
                Tunnel.SDFCenterlineMin, Tunnel.SDFCenterlineMax)
                > FMath::Square(Tunnel.SDFInfluenceRadius))
        {
            return;
        }
        if (VF_FloatDistSquared(WorldX, WorldY, WorldZ, Tunnel.BoundCenter)
            > Tunnel.BoundRadiusSq)
        {
            return;
        }

        if (Tunnel.ControlPoints.Num() >= 2
            && Tunnel.ControlRadii.Num() == Tunnel.ControlPoints.Num())
        {
            for (int32 SegmentIndex = 0;
                 SegmentIndex + 1 < Tunnel.ControlPoints.Num();
                 ++SegmentIndex)
            {
                MinSDF = FMath::Min(
                    MinSDF,
                    VoxelSDF::TaperedCapsule(
                        Pos,
                        Tunnel.ControlPoints[SegmentIndex],
                        Tunnel.ControlPoints[SegmentIndex + 1],
                        Tunnel.ControlRadii[SegmentIndex],
                        Tunnel.ControlRadii[SegmentIndex + 1]));
            }
        }
        else if (Tunnel.bHasMidpoint)
        {
            MinSDF = FMath::Min(
                MinSDF,
                VoxelSDF::TaperedCapsule(
                    Pos, Tunnel.EndpointA, Tunnel.Midpoint,
                    Tunnel.RadiusA, Tunnel.RadiusMid));
            MinSDF = FMath::Min(
                MinSDF,
                VoxelSDF::TaperedCapsule(
                    Pos, Tunnel.Midpoint, Tunnel.EndpointB,
                    Tunnel.RadiusMid, Tunnel.RadiusB));
        }
        else
        {
            MinSDF = FMath::Min(
                MinSDF,
                VoxelSDF::TaperedCapsule(
                    Pos, Tunnel.EndpointA, Tunnel.EndpointB,
                    Tunnel.RadiusA, Tunnel.RadiusB));
        }
    };

    for (int32 TunnelIdx = 0; TunnelIdx < Cache.Tunnels.Num(); ++TunnelIdx)
    {
        EvaluateTunnel(TunnelIdx);
    }

    return MinSDF;
}

FTunnelCoreWorldEvaluation VoxelCaveMorphology::EvaluateTunnelCoreWorld(
    float WorldX, float WorldY, float WorldZ,
    const FChunkSDFCache& Cache,
    const FTunnelSupportFloorColumn* SupportColumn)
{
    const FVector Pos(WorldX, WorldY, WorldZ);
    FTunnelCoreWorldEvaluation Result;

    if (SupportColumn != nullptr && VoxelDensityProfile::AreCountersEnabled())
    {
        VoxelDensityProfile::AddCounter(
            VoxelDensityProfile::ECounter::TunnelSupportFloorQueries);
    }
    if (VoxelDensityProfile::AreCountersEnabled())
    {
        VoxelDensityProfile::AddCounter(
            VoxelDensityProfile::ECounter::TunnelCoreCandidates,
            static_cast<uint64>(Cache.Tunnels.Num()));
    }

    auto EvaluateTunnel = [&](int32 TunnelIdx)
    {
        const FCachedTunnel& Tunnel = Cache.Tunnels[TunnelIdx];
        const bool bHasWorldChain = Tunnel.WorldControlPoints.Num() >= 2
            && Tunnel.WorldControlRadii.Num() == Tunnel.WorldControlPoints.Num();
        const FVector& BoundCenter = bHasWorldChain
            ? Tunnel.WorldBoundCenter : Tunnel.BoundCenter;
        const float BoundRadiusSq = bHasWorldChain
            ? Tunnel.WorldBoundRadiusSq : Tunnel.BoundRadiusSq;
        const float InfluenceRadius = bHasWorldChain
            ? Tunnel.WorldInfluenceRadius : Tunnel.SDFInfluenceRadius;
        const FVector& CenterlineMin = bHasWorldChain
            ? Tunnel.WorldCenterlineMin : Tunnel.SDFCenterlineMin;
        const FVector& CenterlineMax = bHasWorldChain
            ? Tunnel.WorldCenterlineMax : Tunnel.SDFCenterlineMax;
        if (InfluenceRadius > 0.0f
            && VF_DistanceSquaredToAabb(
                WorldX, WorldY, WorldZ, CenterlineMin, CenterlineMax)
                > FMath::Square(InfluenceRadius))
        {
            return;
        }
        if (BoundRadiusSq > 0.0f
            && VF_FloatDistSquared(WorldX, WorldY, WorldZ, BoundCenter) > BoundRadiusSq)
        {
            return;
        }
        if (VoxelDensityProfile::AreCountersEnabled())
        {
            VoxelDensityProfile::AddCounter(
                VoxelDensityProfile::ECounter::TunnelCoreEvaluated);
        }

        const TArray<FVector>& ControlPoints = bHasWorldChain
            ? Tunnel.WorldControlPoints : Tunnel.ControlPoints;
        const TArray<float>& ControlRadii = bHasWorldChain
            ? Tunnel.WorldControlRadii : Tunnel.ControlRadii;
        if (bHasWorldChain)
        {
            if (SupportColumn != nullptr && VoxelDensityProfile::AreCountersEnabled())
            {
                VoxelDensityProfile::AddCounter(
                    VoxelDensityProfile::ECounter::TunnelSupportFloorChecks);
            }
            float FloorZ = 0.0f;
            float SupportRadius = 0.0f;
            float SupportMinZ = 0.0f;
            float SupportMaxZ = 0.0f;
            const bool bProjectedFloor = SupportColumn != nullptr
                ? VoxelCaveMorphology::GetTunnelSupportFloorColumnBand(
                    TunnelIdx, *SupportColumn, FloorZ, SupportMinZ, SupportMaxZ)
                : VoxelPassageGeometry::ProjectWalkableTunnelFloor(
                    ControlPoints, ControlRadii, Pos, FloorZ, SupportRadius);
            // The first cull above already used WorldBoundCenter/WorldBoundRadiusSq for a
            // world-chain tunnel. Repeating the same squared-distance test here only doubled
            // arithmetic on every accepted tunnel; keep the projection and Z checks, which are
            // the distinct support-floor predicates.
            if (bProjectedFloor
                && WorldZ <= FloorZ
                    + VoxelPassageGeometry::WalkableTunnelFloorAirClearanceVoxels
                && (SupportColumn == nullptr
                    ? WorldZ >= FloorZ
                        - VoxelPassageGeometry::LandingFloorThicknessVoxels
                    : WorldZ >= SupportMinZ && WorldZ <= SupportMaxZ))
            {
                Result.bSupportFloor = true;
            }
            if (bProjectedFloor && WorldZ <= FloorZ
                    + VoxelPassageGeometry::WalkableTunnelFloorAirClearanceVoxels)
            {
                // The finite floor slab owns the bottom of this tunnel. Another intersecting
                // tunnel may still win below its own floor; the loop continues so that air
                // ownership remains the union of all valid tunnel cores.
                return;
            }
        }
        if (ControlPoints.Num() >= 2
            && ControlRadii.Num() == ControlPoints.Num())
        {
            for (int32 SegmentIndex = 0;
                 SegmentIndex + 1 < ControlPoints.Num();
                 ++SegmentIndex)
            {
                Result.SDF = FMath::Min(
                    Result.SDF,
                    VoxelSDF::TaperedCapsule(
                        Pos,
                        ControlPoints[SegmentIndex],
                        ControlPoints[SegmentIndex + 1],
                        ControlRadii[SegmentIndex],
                        ControlRadii[SegmentIndex + 1]));
            }
        }
        else if (Tunnel.bHasMidpoint)
        {
            Result.SDF = FMath::Min(
                Result.SDF,
                VoxelSDF::TaperedCapsule(
                    Pos, Tunnel.EndpointA, Tunnel.Midpoint,
                    Tunnel.RadiusA, Tunnel.RadiusMid));
            Result.SDF = FMath::Min(
                Result.SDF,
                VoxelSDF::TaperedCapsule(
                    Pos, Tunnel.Midpoint, Tunnel.EndpointB,
                    Tunnel.RadiusMid, Tunnel.RadiusB));
        }
        else
        {
            Result.SDF = FMath::Min(
                Result.SDF,
                VoxelSDF::TaperedCapsule(
                    Pos, Tunnel.EndpointA, Tunnel.EndpointB,
                    Tunnel.RadiusA, Tunnel.RadiusB));
        }
    };

    for (int32 TunnelIdx = 0; TunnelIdx < Cache.Tunnels.Num(); ++TunnelIdx)
    {
        EvaluateTunnel(TunnelIdx);
    }

    return Result;
}

float VoxelCaveMorphology::EvaluateTunnelCoreWorldSDF(
    float WorldX, float WorldY, float WorldZ,
    const FChunkSDFCache& Cache)
{
    return EvaluateTunnelCoreWorld(WorldX, WorldY, WorldZ, Cache).SDF;
}

bool VoxelCaveMorphology::IsTunnelSupportFloorWorldPoint(
    float WorldX, float WorldY, float WorldZ,
    const FChunkSDFCache& Cache)
{
    const FVector Pos(WorldX, WorldY, WorldZ);

    if (VoxelDensityProfile::AreCountersEnabled())
    {
        VoxelDensityProfile::AddCounter(
            VoxelDensityProfile::ECounter::TunnelCoreCandidates,
            static_cast<uint64>(Cache.Tunnels.Num()));
    }

    auto TestTunnel = [&](int32 TunnelIdx) -> bool
    {
        const FCachedTunnel& Tunnel = Cache.Tunnels[TunnelIdx];
        if (Tunnel.WorldControlPoints.Num() < 2
            || Tunnel.WorldControlRadii.Num() != Tunnel.WorldControlPoints.Num())
        {
            return false;
        }

        if (Tunnel.WorldInfluenceRadius > 0.0f
            && VF_DistanceSquaredToAabb(
                WorldX, WorldY, WorldZ,
                Tunnel.WorldCenterlineMin, Tunnel.WorldCenterlineMax)
                > FMath::Square(Tunnel.WorldInfluenceRadius))
        {
            return false;
        }

        float FloorZ = 0.0f;
        float SupportRadius = 0.0f;
        if (!VoxelPassageGeometry::ProjectWalkableTunnelFloor(
                Tunnel.WorldControlPoints,
                Tunnel.WorldControlRadii,
                Pos,
                FloorZ,
                SupportRadius)
            || WorldZ > FloorZ + VoxelPassageGeometry::WalkableTunnelFloorAirClearanceVoxels
            || WorldZ < FloorZ - VoxelPassageGeometry::LandingFloorThicknessVoxels)
        {
            return false;
        }

        if (Tunnel.WorldBoundRadiusSq <= 0.0f
            || VF_FloatDistSquared(WorldX, WorldY, WorldZ, Tunnel.WorldBoundCenter)
                <= Tunnel.WorldBoundRadiusSq)
        {
            return true;
        }
        return false;
    };

    bool bSupport = false;
    for (int32 TunnelIdx = 0; TunnelIdx < Cache.Tunnels.Num(); ++TunnelIdx)
    {
        if (TestTunnel(TunnelIdx))
        {
            bSupport = true;
            if (VoxelDensityProfile::AreCountersEnabled())
            {
                VoxelDensityProfile::AddCounter(
                    VoxelDensityProfile::ECounter::TunnelCoreEvaluated);
            }
            break;
        }
    }
    return bSupport;
}

void VoxelCaveMorphology::BuildTunnelSupportFloorColumn(
    float WorldX, float WorldY,
    const FChunkSDFCache& Cache,
    FTunnelSupportFloorColumn& OutColumn)
{
    OutColumn.Reset();

    if (VoxelDensityProfile::AreCountersEnabled())
    {
        VoxelDensityProfile::AddCounter(
            VoxelDensityProfile::ECounter::TunnelSupportColumnBuilds);
    }

    if (VoxelDensityProfile::AreCountersEnabled())
    {
        VoxelDensityProfile::AddCounter(
            VoxelDensityProfile::ECounter::TunnelSupportColumnCandidates,
            static_cast<uint64>(Cache.Tunnels.Num()));
    }

    for (int32 TunnelIdx = 0; TunnelIdx < Cache.Tunnels.Num(); ++TunnelIdx)
    {
        if (!Cache.Tunnels.IsValidIndex(TunnelIdx))
        {
            continue;
        }
        const FCachedTunnel& Tunnel = Cache.Tunnels[TunnelIdx];
        if (Tunnel.WorldControlPoints.Num() < 2
            || Tunnel.WorldControlRadii.Num() != Tunnel.WorldControlPoints.Num())
        {
            continue;
        }

        const FVector ColumnPosition(WorldX, WorldY, 0.0f);
        float FloorZ = 0.0f;
        float SupportRadius = 0.0f;
        if (!VoxelPassageGeometry::ProjectWalkableTunnelFloor(
                Tunnel.WorldControlPoints,
                Tunnel.WorldControlRadii,
                ColumnPosition,
                FloorZ,
                SupportRadius))
        {
            continue;
        }

        float MinZ = FloorZ - VoxelPassageGeometry::LandingFloorThicknessVoxels;
        float MaxZ = FloorZ + VoxelPassageGeometry::WalkableTunnelFloorAirClearanceVoxels;

        // Preserve the public point predicate's world bounding sphere exactly, while resolving
        // its Z interval once. The XY projection already proves the support-radius condition.
        if (Tunnel.WorldBoundRadiusSq > 0.0f)
        {
            const float DX = WorldX - static_cast<float>(Tunnel.WorldBoundCenter.X);
            const float DY = WorldY - static_cast<float>(Tunnel.WorldBoundCenter.Y);
            const float Remaining = Tunnel.WorldBoundRadiusSq - (DX * DX + DY * DY);
            if (Remaining < 0.0f)
            {
                continue;
            }
            const float HalfZ = FMath::Sqrt(Remaining);
            MinZ = FMath::Max(MinZ, static_cast<float>(Tunnel.WorldBoundCenter.Z) - HalfZ);
            MaxZ = FMath::Min(MaxZ, static_cast<float>(Tunnel.WorldBoundCenter.Z) + HalfZ);
        }

        if (MinZ <= MaxZ)
        {
            FTunnelSupportFloorInterval& Interval =
                OutColumn.Intervals.Emplace_GetRef();
            Interval.TunnelIndex = TunnelIdx;
            Interval.FloorZ = FloorZ;
            Interval.MinZ = MinZ;
            Interval.MaxZ = MaxZ;
        }
    }

    // Candidate IDs are deterministic and allow the world evaluator to find a
    // tunnel's own band without re-running the XY projection. The public union
    // query below simply tests every band; graph columns normally contain only a
    // handful of candidates.
    OutColumn.Intervals.Sort(
        [](const FTunnelSupportFloorInterval& A,
           const FTunnelSupportFloorInterval& B)
        {
            return A.TunnelIndex < B.TunnelIndex;
        });
}

bool VoxelCaveMorphology::IsTunnelSupportFloorColumnZ(
    float WorldZ,
    const FTunnelSupportFloorColumn& Column)
{
    for (const FTunnelSupportFloorInterval& Interval : Column.Intervals)
    {
        if (WorldZ <= Interval.MaxZ)
        {
            if (WorldZ >= Interval.MinZ)
            {
                return true;
            }
        }
    }
    return false;
}

bool VoxelCaveMorphology::GetTunnelSupportFloorColumnBand(
    int32 TunnelIndex,
    const FTunnelSupportFloorColumn& Column,
    float& OutFloorZ,
    float& OutMinZ,
    float& OutMaxZ)
{
    for (const FTunnelSupportFloorInterval& Interval : Column.Intervals)
    {
        if (Interval.TunnelIndex == TunnelIndex)
        {
            OutFloorZ = Interval.FloorZ;
            OutMinZ = Interval.MinZ;
            OutMaxZ = Interval.MaxZ;
            return true;
        }
    }
    return false;
}

//=============================================================================
// CONVENIENCE WRAPPER (backward compatible)
//=============================================================================
// Builds a temporary cache for a single point, then evaluates.
// For chunk generation, use BuildChunkCache + EvaluateSDFCached directly.
// The search box around the point only needs a MaxInfluence margin — BuildChunkCache
// internally widens the COLLECT region to (2*MaxTunnelLength + MaxInfluence) so the
// graph it builds is the same one the chunk path would build at this point.

float VoxelCaveMorphology::EvaluateSDF(
    float WorldX, float WorldY, float WorldZ,
    const FStrateGenerationParams& Params,
    uint32 Seed, int32 StrateIndex)
{
    const float RoomRadiusEnvelope = FMath::Max(
        FMath::Abs(Params.MinRoomRadius), FMath::Abs(Params.MaxRoomRadius));
    const float TunnelRadiusEnvelope = FMath::Max(
        0.5f,
        FMath::Max(FMath::Abs(Params.TunnelMinRadius), FMath::Abs(Params.TunnelMaxRadius))
            * 1.18f);
    const float FloorReliefEnvelope = FMath::Abs(Params.FloorReliefStrength)
        * VOXEL_NOISE_SCALE * 1.5f;
    const float BlendEnvelope = FMath::Max(Params.SDFBlendRadius, 0.0f);
    const float Margin = FMath::Max(
        RoomRadiusEnvelope + FloorReliefEnvelope,
        FMath::Abs(Params.TunnelWarpStrength) + TunnelRadiusEnvelope
    ) + BlendEnvelope * 3.0f;

    FChunkSDFCache TempCache;
    BuildChunkCache(
        TempCache,
        WorldX - Margin, WorldY - Margin,
        WorldX + Margin, WorldY + Margin,
        Params, Seed, StrateIndex
    );

    return EvaluateSDFCached(
        WorldX, WorldY, WorldZ,
        TempCache, Params.SDFBlendRadius
    );
}
