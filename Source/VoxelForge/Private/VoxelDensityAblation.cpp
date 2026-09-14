// VoxelDensityAblation.cpp
// Development-only TunnelNetwork ablation controls.

#include "VoxelDensityAblation.h"

#include "HAL/IConsoleManager.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

#include <atomic>

namespace
{
    constexpr uint32 UnresolvedMask = 0xFFFFFFFFu;
    std::atomic<uint32> GResolvedMask(UnresolvedMask);

#if !UE_BUILD_SHIPPING
    int32 GCaveWarp = 0;
    int32 GDetailOps = 0;
    int32 GRoomSDF = 0;
    int32 GTunnelSDF = 0;
    int32 GTunnelCore = 0;
    int32 GPassageCarving = 0;
    int32 GPassageStructuralPosts = 0;
    int32 GNativeFloor = 0;
    int32 GDisturbances = 0;
    int32 GOriginSpine = 0;
    int32 GBoundarySeal = 0;
    int32 GLandingPosts = 0;
    int32 GXYEdgeSeal = 0;
    int32 GPitChimneySDF = 0;

#define VF_ABLATION_CVAR(Name, Storage) \
    FAutoConsoleVariableRef CVar##Storage( \
        Name, Storage, \
        TEXT("WORLD-CHANGING DEVELOPMENT-ONLY measurement override. Forces this " \
             "TunnelNetwork density stage off; never use as a gameplay switch. " \
             "All peers and regenerated worlds must use the same value. Resolved once " \
             "per process. 0=on, 1=off."), ECVF_Default)

    VF_ABLATION_CVAR(TEXT("voxel.TunnelAblateCaveWarp"), GCaveWarp);
    VF_ABLATION_CVAR(TEXT("voxel.TunnelAblateDetailOps"), GDetailOps);
    VF_ABLATION_CVAR(TEXT("voxel.TunnelAblateRoomSDF"), GRoomSDF);
    VF_ABLATION_CVAR(TEXT("voxel.TunnelAblateTunnelSDF"), GTunnelSDF);
    VF_ABLATION_CVAR(TEXT("voxel.TunnelAblateTunnelCore"), GTunnelCore);
    VF_ABLATION_CVAR(TEXT("voxel.TunnelAblatePassageCarving"), GPassageCarving);
    VF_ABLATION_CVAR(TEXT("voxel.TunnelAblatePassageStructuralPosts"), GPassageStructuralPosts);
    VF_ABLATION_CVAR(TEXT("voxel.TunnelAblateNativeFloor"), GNativeFloor);
    VF_ABLATION_CVAR(TEXT("voxel.TunnelAblateDisturbances"), GDisturbances);
    VF_ABLATION_CVAR(TEXT("voxel.TunnelAblateOriginSpine"), GOriginSpine);
    VF_ABLATION_CVAR(TEXT("voxel.TunnelAblateBoundarySeal"), GBoundarySeal);
    VF_ABLATION_CVAR(TEXT("voxel.TunnelAblateLandingPosts"), GLandingPosts);
    VF_ABLATION_CVAR(TEXT("voxel.TunnelAblateXYEdgeSeal"), GXYEdgeSeal);
    VF_ABLATION_CVAR(TEXT("voxel.TunnelAblatePitChimneySDF"), GPitChimneySDF);

#undef VF_ABLATION_CVAR

    uint32 ResolveValue(const TCHAR* Name, int32& Storage)
    {
        int32 Value = Storage;
        FParse::Value(FCommandLine::Get(), Name, Value);
        Storage = Value != 0 ? 1 : 0;
        return Storage != 0 ? 1u : 0u;
    }

    uint32 ResolveMask()
    {
        uint32 Mask = 0;
        Mask |= ResolveValue(TEXT("voxel.TunnelAblateCaveWarp="), GCaveWarp)
            * static_cast<uint32>(VoxelDensityAblation::EStage::CaveWarp);
        Mask |= ResolveValue(TEXT("voxel.TunnelAblateDetailOps="), GDetailOps)
            * static_cast<uint32>(VoxelDensityAblation::EStage::DetailOps);
        Mask |= ResolveValue(TEXT("voxel.TunnelAblateRoomSDF="), GRoomSDF)
            * static_cast<uint32>(VoxelDensityAblation::EStage::RoomSDF);
        Mask |= ResolveValue(TEXT("voxel.TunnelAblateTunnelSDF="), GTunnelSDF)
            * static_cast<uint32>(VoxelDensityAblation::EStage::TunnelSDF);
        Mask |= ResolveValue(TEXT("voxel.TunnelAblateTunnelCore="), GTunnelCore)
            * static_cast<uint32>(VoxelDensityAblation::EStage::TunnelCore);
        Mask |= ResolveValue(TEXT("voxel.TunnelAblatePassageCarving="), GPassageCarving)
            * static_cast<uint32>(VoxelDensityAblation::EStage::PassageCarving);
        Mask |= ResolveValue(
            TEXT("voxel.TunnelAblatePassageStructuralPosts="), GPassageStructuralPosts)
            * static_cast<uint32>(VoxelDensityAblation::EStage::PassageStructuralPosts);
        Mask |= ResolveValue(TEXT("voxel.TunnelAblateNativeFloor="), GNativeFloor)
            * static_cast<uint32>(VoxelDensityAblation::EStage::NativeFloor);
        Mask |= ResolveValue(TEXT("voxel.TunnelAblateDisturbances="), GDisturbances)
            * static_cast<uint32>(VoxelDensityAblation::EStage::Disturbances);
        Mask |= ResolveValue(TEXT("voxel.TunnelAblateOriginSpine="), GOriginSpine)
            * static_cast<uint32>(VoxelDensityAblation::EStage::OriginSpine);
        Mask |= ResolveValue(TEXT("voxel.TunnelAblateBoundarySeal="), GBoundarySeal)
            * static_cast<uint32>(VoxelDensityAblation::EStage::BoundarySeal);
        Mask |= ResolveValue(TEXT("voxel.TunnelAblateLandingPosts="), GLandingPosts)
            * static_cast<uint32>(VoxelDensityAblation::EStage::LandingPosts);
        Mask |= ResolveValue(TEXT("voxel.TunnelAblateXYEdgeSeal="), GXYEdgeSeal)
            * static_cast<uint32>(VoxelDensityAblation::EStage::XYEdgeSeal);
        Mask |= ResolveValue(TEXT("voxel.TunnelAblatePitChimneySDF="), GPitChimneySDF)
            * static_cast<uint32>(VoxelDensityAblation::EStage::PitChimneySDF);
        return Mask;
    }
#endif
}

namespace VoxelDensityAblation
{
    uint32 GetResolvedMask()
    {
        uint32 Existing = GResolvedMask.load(std::memory_order_acquire);
        if (Existing != UnresolvedMask)
        {
            return Existing;
        }

#if UE_BUILD_SHIPPING
        constexpr uint32 Resolved = 0;
#else
        const uint32 Resolved = ResolveMask();
#endif
        uint32 Expected = UnresolvedMask;
        if (!GResolvedMask.compare_exchange_strong(
                Expected, Resolved, std::memory_order_release, std::memory_order_acquire))
        {
            return Expected;
        }

        UE_LOG(LogTemp, Display,
            TEXT("[VoxelForgeTunnelAblation] resolved mask=0x%08x "
                 "(world-changing development measurement overrides; 0=all on)"),
            Resolved);
        return Resolved;
    }
}
