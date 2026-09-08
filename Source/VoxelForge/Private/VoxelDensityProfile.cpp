#include "VoxelDensityProfile.h"

namespace VoxelDensityProfile
{
    namespace
    {
        std::atomic<bool> GEnabled(false);
        std::atomic<uint64> GCalls[BucketCount];
        std::atomic<uint64> GCycles[BucketCount];
        std::atomic<uint64> GCounters[CounterCount];

        int32 ToIndex(EBucket Bucket)
        {
            const int32 Index = static_cast<int32>(Bucket);
            return (Index >= 0 && Index < BucketCount)
                ? Index
                : static_cast<int32>(EBucket::OtherOp);
        }
    }

    void SetEnabled(bool bEnabled)
    {
        GEnabled.store(bEnabled, std::memory_order_relaxed);
    }

    bool IsEnabled()
    {
        return GEnabled.load(std::memory_order_relaxed);
    }

    void Reset()
    {
        for (int32 Index = 0; Index < BucketCount; ++Index)
        {
            GCalls[Index].store(0, std::memory_order_relaxed);
            GCycles[Index].store(0, std::memory_order_relaxed);
        }
        for (int32 Index = 0; Index < CounterCount; ++Index)
        {
            GCounters[Index].store(0, std::memory_order_relaxed);
        }
    }

    FSnapshot Snapshot()
    {
        FSnapshot Result;
        for (int32 Index = 0; Index < BucketCount; ++Index)
        {
            Result.Calls[Index] = GCalls[Index].load(std::memory_order_relaxed);
            Result.Cycles[Index] = GCycles[Index].load(std::memory_order_relaxed);
        }
        for (int32 Index = 0; Index < CounterCount; ++Index)
        {
            Result.Counters[Index] = GCounters[Index].load(std::memory_order_relaxed);
        }
        return Result;
    }

    void AddCounter(ECounter Counter, uint64 Amount)
    {
        const int32 Index = static_cast<int32>(Counter);
        if (Index >= 0 && Index < CounterCount)
        {
            GCounters[Index].fetch_add(Amount, std::memory_order_relaxed);
        }
    }

    const TCHAR* CounterName(ECounter Counter)
    {
        switch (Counter)
        {
        case ECounter::TunnelCacheLookup: return TEXT("TunnelCacheLookup");
        case ECounter::TunnelCacheHit:    return TEXT("TunnelCacheHit");
        case ECounter::TunnelCacheMiss:   return TEXT("TunnelCacheMiss");
        case ECounter::SdfCacheBuild:     return TEXT("SdfCacheBuild");
        case ECounter::CaveRoomCandidates:return TEXT("CaveRoomCandidates");
        case ECounter::CaveRoomEvaluated: return TEXT("CaveRoomEvaluated");
        case ECounter::CaveTunnelCandidates:return TEXT("CaveTunnelCandidates");
        case ECounter::CaveTunnelEvaluated:return TEXT("CaveTunnelEvaluated");
        case ECounter::TunnelCoreCandidates:return TEXT("TunnelCoreCandidates");
        case ECounter::TunnelCoreEvaluated:return TEXT("TunnelCoreEvaluated");
        case ECounter::PassageCandidates: return TEXT("PassageCandidates");
        case ECounter::PassageEvaluated:  return TEXT("PassageEvaluated");
        case ECounter::Count:              break;
        }
        return TEXT("Unknown");
    }

    EBucket BucketFromName(const TCHAR* Name)
    {
        if (Name == nullptr) { return EBucket::OtherOp; }

        struct FNameBucket
        {
            const TCHAR* Name;
            EBucket Bucket;
        };
        static const FNameBucket Names[] =
        {
            { TEXT("ApplyDisturbances"), EBucket::ApplyDisturbances },
            { TEXT("PassageModifier"), EBucket::PassageModifier },
            { TEXT("PassageLandingAir"), EBucket::PassageLandingAir },
            { TEXT("PassageLandingFloor"), EBucket::PassageLandingFloor },
            { TEXT("PassageLandingRoomFloor"), EBucket::PassageLandingRoomFloor },
            { TEXT("PassageTunnelAir"), EBucket::PassageTunnelAir },
            { TEXT("PassageStructuralPosts"), EBucket::PassageStructuralPosts },
            { TEXT("TunnelCoreSupport"), EBucket::TunnelCoreSupport },
            { TEXT("TunnelCoreSdf"), EBucket::TunnelCoreSdf },
            { TEXT("TunnelCorePosts"), EBucket::TunnelCorePosts },
            { TEXT("StructuralTail"), EBucket::StructuralTail },
            { TEXT("RoomGraphSource"),  EBucket::RoomGraphSource },
            { TEXT("SdfCarve"),         EBucket::SdfCarve },
            { TEXT("SdfConvertOp"),     EBucket::SdfCarve },
            { TEXT("CaveRoughnessMod"), EBucket::CaveRoughnessMod },
            { TEXT("CaveTerraceMod"),   EBucket::CaveTerraceMod },
            { TEXT("LayerLineMod"),     EBucket::LayerLineMod },
            { TEXT("RibbingMod"),       EBucket::RibbingMod },
            { TEXT("CaveOverhangMod"),  EBucket::CaveOverhangMod },
            { TEXT("CaveCliffMod"),     EBucket::CaveCliffMod },
            { TEXT("ScallopMod"),        EBucket::ScallopMod },
            { TEXT("CaveArchMod"),       EBucket::CaveArchMod },
            { TEXT("RoomColumnMod"),     EBucket::RoomColumnMod },
            { TEXT("DomeMod"),           EBucket::DomeMod },
            { TEXT("PinchMod"),          EBucket::PinchMod },
            { TEXT("FloorBiasMod"),      EBucket::FloorBiasMod },
            { TEXT("WormFieldSource"),   EBucket::WormFieldSource },
            { TEXT("OriginSpineOp"),     EBucket::OriginSpineOp },
            { TEXT("BoundarySealOp"),    EBucket::BoundarySealOp },
            { TEXT("PassageCarveOp"),    EBucket::PassageCarveOp },
            { TEXT("CaveTunnelFloorOp"), EBucket::CaveTunnelFloorOp },
            { TEXT("CaveTunnelAirOp"),   EBucket::CaveTunnelAirOp },
            { TEXT("XYEdgeSealOp"),      EBucket::XYEdgeSealOp },
        };
        for (const FNameBucket& Entry : Names)
        {
            if (FCString::Strcmp(Name, Entry.Name) == 0)
            {
                return Entry.Bucket;
            }
        }
        return EBucket::OtherOp;
    }

    const TCHAR* BucketName(EBucket Bucket)
    {
        switch (Bucket)
        {
        case EBucket::GetDensityAt:      return TEXT("GetDensityAt");
        case EBucket::ApplyDisturbances:return TEXT("ApplyDisturbances");
        case EBucket::PassageModifier:  return TEXT("PassageModifier");
        case EBucket::PassageLandingAir:return TEXT("PassageLandingAir");
        case EBucket::PassageLandingFloor:return TEXT("PassageLandingFloor");
        case EBucket::PassageLandingRoomFloor:return TEXT("PassageLandingRoomFloor");
        case EBucket::PassageTunnelAir:return TEXT("PassageTunnelAir");
        case EBucket::PassageStructuralPosts:return TEXT("PassageStructuralPosts");
        case EBucket::TunnelCoreSupport:return TEXT("TunnelCoreSupport");
        case EBucket::TunnelCoreSdf:    return TEXT("TunnelCoreSdf");
        case EBucket::TunnelCorePosts:  return TEXT("TunnelCorePosts");
        case EBucket::StructuralTail:   return TEXT("StructuralTail");
        case EBucket::RoomGraphSource:  return TEXT("RoomGraphSource");
        case EBucket::SdfCarve:         return TEXT("SdfCarve");
        case EBucket::CaveRoughnessMod: return TEXT("CaveRoughnessMod");
        case EBucket::CaveTerraceMod:   return TEXT("CaveTerraceMod");
        case EBucket::LayerLineMod:     return TEXT("LayerLineMod");
        case EBucket::RibbingMod:       return TEXT("RibbingMod");
        case EBucket::CaveOverhangMod:  return TEXT("CaveOverhangMod");
        case EBucket::CaveCliffMod:     return TEXT("CaveCliffMod");
        case EBucket::ScallopMod:       return TEXT("ScallopMod");
        case EBucket::CaveArchMod:      return TEXT("CaveArchMod");
        case EBucket::RoomColumnMod:    return TEXT("RoomColumnMod");
        case EBucket::DomeMod:          return TEXT("DomeMod");
        case EBucket::PinchMod:         return TEXT("PinchMod");
        case EBucket::FloorBiasMod:     return TEXT("FloorBiasMod");
        case EBucket::WormFieldSource:  return TEXT("WormFieldSource");
        case EBucket::OriginSpineOp:    return TEXT("OriginSpineOp");
        case EBucket::BoundarySealOp:   return TEXT("BoundarySealOp");
        case EBucket::PassageCarveOp:   return TEXT("PassageCarveOp");
        case EBucket::CaveTunnelFloorOp:return TEXT("CaveTunnelFloorOp");
        case EBucket::CaveTunnelAirOp:  return TEXT("CaveTunnelAirOp");
        case EBucket::XYEdgeSealOp:     return TEXT("XYEdgeSealOp");
        case EBucket::OtherOp:           return TEXT("OtherOp");
        case EBucket::Count:             break;
        }
        return TEXT("Unknown");
    }

    FScopedTimer::~FScopedTimer()
    {
        if (!bActive) { return; }
        const int32 Index = ToIndex(Bucket);
        GCalls[Index].fetch_add(1, std::memory_order_relaxed);
        GCycles[Index].fetch_add(FPlatformTime::Cycles64() - StartCycles,
                                 std::memory_order_relaxed);
    }
}
