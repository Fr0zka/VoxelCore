// VoxelDensityAblation.h
// Development-only, world-changing measurement overrides for TunnelNetwork density stages.

#pragma once

#include "CoreMinimal.h"

#include <atomic>

namespace VoxelDensityAblation
{
    /**
     * A bit in this mask means that the named stage is forced off. These are measurement
     * overrides, not gameplay settings: they change the generated field, are resolved once per
     * process, and must match across peers and regenerated worlds.
     */
    enum class EStage : uint32
    {
        CaveWarp              = 1u << 0,
        DetailOps             = 1u << 1,
        RoomSDF               = 1u << 2,
        TunnelSDF             = 1u << 3,
        TunnelCore            = 1u << 4,
        PassageCarving        = 1u << 5,
        PassageStructuralPosts= 1u << 6,
        NativeFloor           = 1u << 7,
        Disturbances          = 1u << 8,
        OriginSpine           = 1u << 9,
        BoundarySeal          = 1u << 10,
        LandingPosts          = 1u << 11,
        XYEdgeSeal            = 1u << 12,
        PitChimneySDF         = 1u << 13,
    };

    /** Resolve command-line/CVar values once and return the process-wide mask. */
    VOXELFORGE_API uint32 GetResolvedMask();

    FORCEINLINE bool IsForcedOff(EStage Stage)
    {
        return (GetResolvedMask() & static_cast<uint32>(Stage)) != 0;
    }

    FORCEINLINE bool IsCaveWarpOff() { return IsForcedOff(EStage::CaveWarp); }
    FORCEINLINE bool IsDetailOpsOff() { return IsForcedOff(EStage::DetailOps); }
    FORCEINLINE bool IsRoomSDFOff() { return IsForcedOff(EStage::RoomSDF); }
    FORCEINLINE bool IsTunnelSDFOff() { return IsForcedOff(EStage::TunnelSDF); }
    FORCEINLINE bool IsTunnelCoreOff() { return IsForcedOff(EStage::TunnelCore); }
    FORCEINLINE bool IsPassageCarvingOff() { return IsForcedOff(EStage::PassageCarving); }
    FORCEINLINE bool IsPassageStructuralPostsOff()
    {
        return IsForcedOff(EStage::PassageStructuralPosts);
    }
    FORCEINLINE bool IsNativeFloorOff() { return IsForcedOff(EStage::NativeFloor); }
    FORCEINLINE bool IsDisturbancesOff() { return IsForcedOff(EStage::Disturbances); }
    FORCEINLINE bool IsOriginSpineOff() { return IsForcedOff(EStage::OriginSpine); }
    FORCEINLINE bool IsBoundarySealOff() { return IsForcedOff(EStage::BoundarySeal); }
    FORCEINLINE bool IsLandingPostsOff() { return IsForcedOff(EStage::LandingPosts); }
    FORCEINLINE bool IsXYEdgeSealOff() { return IsForcedOff(EStage::XYEdgeSeal); }
    FORCEINLINE bool IsPitChimneySDFOff() { return IsForcedOff(EStage::PitChimneySDF); }
}
