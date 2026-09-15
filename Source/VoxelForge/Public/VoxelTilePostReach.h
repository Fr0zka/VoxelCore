// Per-tile reach state for density posts.
//
// The marching-cubes worker installs this state once for the tile's lattice plus its one-point
// halo.  Density helpers only read the bit that owns them.  The inline TLS is intentional: the
// runtime hot path must not cross a DLL boundary for a single boolean load.

#pragma once

#include "CoreMinimal.h"

#include <atomic>

namespace VoxelGenLOD
{
    enum ETilePostReach : uint8
    {
        OriginLandingReachable    = 1u << 0,
        PassageLandingReachable   = 1u << 1,
        PassageStructuralReachable= 1u << 2,
        AllTilePostReach          = OriginLandingReachable
                                  | PassageLandingReachable
                                  | PassageStructuralReachable,
    };

    enum class ETilePostComparisonKind : uint8
    {
        Other = 0,
        OriginAir,
        OriginFloor,
        PassageLandingSDF,
        PassageLandingAir,
        PassageTunnelAir,
        PassageLandingFloor,
        PassageStructuralPosts,
        PassageNativeFloor,
        PassageLandingRoomFloor,
        Count,
    };

    // A tile that is not being meshed, or a point query outside the mesher, must retain the
    // canonical behaviour.  GenerateMesh scopes this to its computed mask.
    inline thread_local uint8 TilePostReachFlags = AllTilePostReach;
    inline thread_local bool bTilePostReachDebug = false;
    inline thread_local bool bTilePostReachBypass = false;

    // These are diagnostics only.  They never participate in a field decision.
    inline std::atomic<bool> GTilePostReachDebugEnabled { false };
    inline std::atomic<uint64> GTilePostReachTileCount { 0 };
    inline std::atomic<uint64> GOriginLandingReachableTiles { 0 };
    inline std::atomic<uint64> GPassageLandingReachableTiles { 0 };
    inline std::atomic<uint64> GPassageStructuralReachableTiles { 0 };
    inline std::atomic<uint64> GOriginLandingSkippedTiles { 0 };
    inline std::atomic<uint64> GPassageLandingSkippedTiles { 0 };
    inline std::atomic<uint64> GPassageStructuralSkippedTiles { 0 };
    inline std::atomic<uint64> GSkippedPostComparisons { 0 };
    inline std::atomic<uint64> GSkippedPostDifferences { 0 };
    inline std::atomic<uint64> GSkippedPostComparisonsByKind[
        static_cast<uint8>(ETilePostComparisonKind::Count)]{};
    inline std::atomic<uint64> GSkippedPostDifferencesByKind[
        static_cast<uint8>(ETilePostComparisonKind::Count)]{};

    FORCEINLINE bool IsTilePostReachable(uint8 Bit)
    {
        return (TilePostReachFlags & Bit) != 0;
    }

    FORCEINLINE bool IsOriginLandingReachable()
    {
        return IsTilePostReachable(OriginLandingReachable);
    }

    FORCEINLINE bool IsPassageLandingReachable()
    {
        return IsTilePostReachable(PassageLandingReachable);
    }

    FORCEINLINE bool IsPassageStructuralReachable()
    {
        return IsTilePostReachable(PassageStructuralReachable);
    }

    FORCEINLINE bool IsTilePostReachBypassActive()
    {
        return bTilePostReachBypass;
    }

    FORCEINLINE void RecordTileReachMask(uint8 Flags)
    {
        GTilePostReachTileCount.fetch_add(1, std::memory_order_relaxed);
        if ((Flags & OriginLandingReachable) != 0)
        {
            GOriginLandingReachableTiles.fetch_add(1, std::memory_order_relaxed);
        }
        else
        {
            GOriginLandingSkippedTiles.fetch_add(1, std::memory_order_relaxed);
        }
        if ((Flags & PassageLandingReachable) != 0)
        {
            GPassageLandingReachableTiles.fetch_add(1, std::memory_order_relaxed);
        }
        else
        {
            GPassageLandingSkippedTiles.fetch_add(1, std::memory_order_relaxed);
        }
        if ((Flags & PassageStructuralReachable) != 0)
        {
            GPassageStructuralReachableTiles.fetch_add(1, std::memory_order_relaxed);
        }
        else
        {
            GPassageStructuralSkippedTiles.fetch_add(1, std::memory_order_relaxed);
        }
    }

    FORCEINLINE bool SameFloatBits(float A, float B)
    {
        uint32 ABits = 0;
        uint32 BBits = 0;
        FMemory::Memcpy(&ABits, &A, sizeof(float));
        FMemory::Memcpy(&BBits, &B, sizeof(float));
        return ABits == BBits;
    }

    FORCEINLINE void RecordSkippedPostComparison(
        ETilePostComparisonKind Kind, bool bDifferent)
    {
        GSkippedPostComparisons.fetch_add(1, std::memory_order_relaxed);
        const uint8 KindIndex = static_cast<uint8>(Kind);
        if (KindIndex < static_cast<uint8>(ETilePostComparisonKind::Count))
        {
            GSkippedPostComparisonsByKind[KindIndex].fetch_add(
                1, std::memory_order_relaxed);
        }
        if (bDifferent)
        {
            GSkippedPostDifferences.fetch_add(1, std::memory_order_relaxed);
            if (KindIndex < static_cast<uint8>(ETilePostComparisonKind::Count))
            {
                GSkippedPostDifferencesByKind[KindIndex].fetch_add(
                    1, std::memory_order_relaxed);
            }
        }
    }

    FORCEINLINE void RecordSkippedPostComparison(bool bDifferent)
    {
        RecordSkippedPostComparison(ETilePostComparisonKind::Other, bDifferent);
    }

    /** Run one skipped float post through its canonical body, then restore the fast-path value. */
    template <typename CallableType>
    FORCEINLINE void CompareSkippedPost(
        float& Density, CallableType Callable,
        ETilePostComparisonKind Kind = ETilePostComparisonKind::Other)
    {
        if (!bTilePostReachDebug || bTilePostReachBypass)
        {
            return;
        }

        const float Before = Density;
        {
            const bool PreviousBypass = bTilePostReachBypass;
            bTilePostReachBypass = true;
            Callable();
            bTilePostReachBypass = PreviousBypass;
        }
        RecordSkippedPostComparison(Kind, !SameFloatBits(Before, Density));
        Density = Before;
    }

    /** Compare a scalar proof input such as EvaluateModifierSDF without changing the field. */
    FORCEINLINE void CompareSkippedPostValue(
        float Candidate, float Canonical,
        ETilePostComparisonKind Kind = ETilePostComparisonKind::Other)
    {
        if (bTilePostReachDebug && !bTilePostReachBypass)
        {
            RecordSkippedPostComparison(Kind, !SameFloatBits(Candidate, Canonical));
        }
    }
}
