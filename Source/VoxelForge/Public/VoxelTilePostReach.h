// Per-tile reach state for density posts.
//
// The marching-cubes worker installs this state once for the tile's lattice plus its one-point
// halo.  Density helpers only read the bit that owns them.  The inline TLS is intentional: the
// runtime hot path must not cross a DLL boundary for a single boolean load.

#pragma once

#include "CoreMinimal.h"
#include "HAL/PlatformTime.h"

#include <atomic>

namespace VoxelGenLOD
{
    enum ETilePostReach : uint8
    {
        OriginLandingReachable    = 1u << 0,
        PassageLandingReachable   = 1u << 1,
        PassageStructuralReachable= 1u << 2,
        TunnelCoreReachable       = 1u << 3,
        PassageCarvingReachable   = 1u << 4,
        AllTilePostReach          = OriginLandingReachable
                                  | PassageLandingReachable
                                  | PassageStructuralReachable
                                  | TunnelCoreReachable
                                  | PassageCarvingReachable,
        AllTileReach              = AllTilePostReach,
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
        TunnelCore,
        PassageCarving,
        FinalField,
        Count,
    };

    enum class ETileReachCostKind : uint8
    {
        TunnelCoreWorld = 0,
        TunnelCoreTail,
        PassageCarving,
        Count,
    };

    constexpr int32 TileReachBlockCells = 8;
    constexpr int32 TileReachMaxBlocksPerAxis = 4;
    constexpr int32 TileReachMaxBlockCount =
        TileReachMaxBlocksPerAxis * TileReachMaxBlocksPerAxis * TileReachMaxBlocksPerAxis;

    FORCEINLINE FBox MakeTileReachBlockBox(
        const FIntVector& TileOrigin, int32 Step, int32 CellsPerAxis,
        int32 BlockX, int32 BlockY, int32 BlockZ)
    {
        const int32 SafeStep = FMath::Max(Step, 1);
        const int32 MinX = BlockX * TileReachBlockCells;
        const int32 MinY = BlockY * TileReachBlockCells;
        const int32 MinZ = BlockZ * TileReachBlockCells;
        const int32 MaxX = FMath::Min(
            CellsPerAxis, MinX + TileReachBlockCells);
        const int32 MaxY = FMath::Min(
            CellsPerAxis, MinY + TileReachBlockCells);
        const int32 MaxZ = FMath::Min(
            CellsPerAxis, MinZ + TileReachBlockCells);
        return FBox(
            FVector(
                static_cast<float>(static_cast<int64>(TileOrigin.X) + MinX * SafeStep - SafeStep),
                static_cast<float>(static_cast<int64>(TileOrigin.Y) + MinY * SafeStep - SafeStep),
                static_cast<float>(static_cast<int64>(TileOrigin.Z) + MinZ * SafeStep - SafeStep)),
            FVector(
                static_cast<float>(static_cast<int64>(TileOrigin.X) + MaxX * SafeStep),
                static_cast<float>(static_cast<int64>(TileOrigin.Y) + MaxY * SafeStep),
                static_cast<float>(static_cast<int64>(TileOrigin.Z) + MaxZ * SafeStep)));
    }

    // A tile that is not being meshed, or a point query outside the mesher, must retain the
    // canonical behaviour.  GenerateMesh scopes this to its computed mask.
    inline thread_local uint8 TilePostReachFlags = AllTileReach;
    inline thread_local bool bTilePostReachDebug = false;
    inline thread_local bool bTilePostReachBypass = false;
    inline thread_local bool bTileReachDiagnosticTile = false;
    inline thread_local bool bTileCoreReachDecisionRecorded = false;
    inline thread_local float TileReachScale = 1.0f;
    inline thread_local int32 TileReachBlocksPerAxis = 0;
    inline thread_local int32 TileReachBlockIndex = INDEX_NONE;
    inline thread_local bool bTileBlockReachValid = false;
    inline thread_local uint8 TileBlockReachFlags[TileReachMaxBlockCount]{};
    // A false result is installed only after a cache was built with the whole mesher tile as its
    // graph window.  Chunk-local caches leave this true (the conservative no-skip fallback).
    inline thread_local bool bTileCoreReachProofEnabled = false;

    // These are diagnostics only.  They never participate in a field decision.
    inline std::atomic<bool> GTilePostReachDebugEnabled { false };
    inline std::atomic<uint64> GTilePostReachTileCount { 0 };
    inline std::atomic<uint64> GOriginLandingReachableTiles { 0 };
    inline std::atomic<uint64> GPassageLandingReachableTiles { 0 };
    inline std::atomic<uint64> GPassageStructuralReachableTiles { 0 };
    inline std::atomic<uint64> GOriginLandingSkippedTiles { 0 };
    inline std::atomic<uint64> GPassageLandingSkippedTiles { 0 };
    inline std::atomic<uint64> GPassageStructuralSkippedTiles { 0 };
    inline std::atomic<uint64> GTunnelCoreReachableTiles { 0 };
    inline std::atomic<uint64> GTunnelCoreSkippedTiles { 0 };
    inline std::atomic<uint64> GPassageCarvingReachableTiles { 0 };
    inline std::atomic<uint64> GPassageCarvingSkippedTiles { 0 };
    inline std::atomic<uint64> GSkippedPostComparisons { 0 };
    inline std::atomic<uint64> GSkippedPostDifferences { 0 };
    inline std::atomic<uint64> GSkippedPostComparisonsByKind[
        static_cast<uint8>(ETilePostComparisonKind::Count)]{};
    inline std::atomic<uint64> GSkippedPostDifferencesByKind[
        static_cast<uint8>(ETilePostComparisonKind::Count)]{};
    inline std::atomic<bool> GTileReachCostDiagnosticsEnabled { false };
    inline std::atomic<uint64> GTileReachCostCalls[
        static_cast<uint8>(ETileReachCostKind::Count)][2]{};
    inline std::atomic<uint64> GTileReachCostCycles[
        static_cast<uint8>(ETileReachCostKind::Count)][2]{};
    inline std::atomic<uint64> GTileReachBlockCostCalls[
        static_cast<uint8>(ETileReachCostKind::Count)][2]{};
    inline std::atomic<uint64> GTileReachBlockCostCycles[
        static_cast<uint8>(ETileReachCostKind::Count)][2]{};

    // Automation tests deliberately run the real mesher twice (full reach and a shrunken reach)
    // in one process. Keep the reset in the reach owner so a test cannot accidentally omit one
    // of the per-kind counters and accept a vacuous proof.
    FORCEINLINE void ResetTilePostReachDiagnostics()
    {
        GTilePostReachDebugEnabled.store(false, std::memory_order_relaxed);
        GTilePostReachTileCount.store(0, std::memory_order_relaxed);
        GOriginLandingReachableTiles.store(0, std::memory_order_relaxed);
        GPassageLandingReachableTiles.store(0, std::memory_order_relaxed);
        GPassageStructuralReachableTiles.store(0, std::memory_order_relaxed);
        GOriginLandingSkippedTiles.store(0, std::memory_order_relaxed);
        GPassageLandingSkippedTiles.store(0, std::memory_order_relaxed);
        GPassageStructuralSkippedTiles.store(0, std::memory_order_relaxed);
        GTunnelCoreReachableTiles.store(0, std::memory_order_relaxed);
        GTunnelCoreSkippedTiles.store(0, std::memory_order_relaxed);
        GPassageCarvingReachableTiles.store(0, std::memory_order_relaxed);
        GPassageCarvingSkippedTiles.store(0, std::memory_order_relaxed);
        GSkippedPostComparisons.store(0, std::memory_order_relaxed);
        GSkippedPostDifferences.store(0, std::memory_order_relaxed);
        for (uint8 Index = 0;
             Index < static_cast<uint8>(ETilePostComparisonKind::Count); ++Index)
        {
            GSkippedPostComparisonsByKind[Index].store(0, std::memory_order_relaxed);
            GSkippedPostDifferencesByKind[Index].store(0, std::memory_order_relaxed);
        }
        GTileReachCostDiagnosticsEnabled.store(false, std::memory_order_relaxed);
        for (uint8 Kind = 0; Kind < static_cast<uint8>(ETileReachCostKind::Count); ++Kind)
        {
            for (uint8 Reach = 0; Reach < 2; ++Reach)
            {
                GTileReachCostCalls[Kind][Reach].store(0, std::memory_order_relaxed);
                GTileReachCostCycles[Kind][Reach].store(0, std::memory_order_relaxed);
                GTileReachBlockCostCalls[Kind][Reach].store(0, std::memory_order_relaxed);
                GTileReachBlockCostCycles[Kind][Reach].store(0, std::memory_order_relaxed);
            }
        }
    }

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

    FORCEINLINE bool IsTunnelCoreReachable()
    {
        // Core's measured far share is negligible; keep its exact tile decision and avoid a
        // second, weaker block geometry approximation. Passage carving is the only block-gated
        // work because its tight chain produced a material far share in the pre-gate measure.
        return IsTilePostReachable(TunnelCoreReachable);
    }

    FORCEINLINE bool IsPassageCarvingReachable()
    {
        if (bTileBlockReachValid
            && TileReachBlockIndex >= 0
            && TileReachBlockIndex < TileReachMaxBlockCount)
        {
            return (TileBlockReachFlags[TileReachBlockIndex] & PassageCarvingReachable) != 0;
        }
        return IsTilePostReachable(PassageCarvingReachable);
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
        if ((Flags & PassageCarvingReachable) != 0)
        {
            GPassageCarvingReachableTiles.fetch_add(1, std::memory_order_relaxed);
        }
        else
        {
            GPassageCarvingSkippedTiles.fetch_add(1, std::memory_order_relaxed);
        }
    }

    FORCEINLINE void RecordTunnelCoreReachDecision(bool bReachable)
    {
        if (bTileCoreReachDecisionRecorded)
        {
            return;
        }
        bTileCoreReachDecisionRecorded = true;
        if (bReachable)
        {
            GTunnelCoreReachableTiles.fetch_add(1, std::memory_order_relaxed);
        }
        else
        {
            GTunnelCoreSkippedTiles.fetch_add(1, std::memory_order_relaxed);
        }
    }

    FORCEINLINE uint8 TileReachCostIndex(ETileReachCostKind Kind)
    {
        switch (Kind)
        {
            case ETileReachCostKind::TunnelCoreWorld:
            case ETileReachCostKind::TunnelCoreTail:
                return IsTilePostReachable(TunnelCoreReachable) ? 1u : 0u;
            case ETileReachCostKind::PassageCarving:
                return IsTilePostReachable(PassageCarvingReachable) ? 1u : 0u;
            default:
                return 1u;
        }
    }

    FORCEINLINE uint8 BlockReachCostIndex(ETileReachCostKind Kind)
    {
        switch (Kind)
        {
            case ETileReachCostKind::TunnelCoreWorld:
            case ETileReachCostKind::TunnelCoreTail:
                return IsTunnelCoreReachable() ? 1u : 0u;
            case ETileReachCostKind::PassageCarving:
                return IsPassageCarvingReachable() ? 1u : 0u;
            default:
                return 1u;
        }
    }

    FORCEINLINE void RecordTileReachBlockCost(
        ETileReachCostKind Kind, uint8 ReachIndex, uint64 Cycles)
    {
        const uint8 KindIndex = static_cast<uint8>(Kind);
        if (KindIndex < static_cast<uint8>(ETileReachCostKind::Count))
        {
            GTileReachBlockCostCalls[KindIndex][ReachIndex].fetch_add(
                1, std::memory_order_relaxed);
            GTileReachBlockCostCycles[KindIndex][ReachIndex].fetch_add(
                Cycles, std::memory_order_relaxed);
        }
    }

    /** Inclusive timing split used for the pre-gate near/far measurement only. */
    struct FScopedReachCost
    {
        ETileReachCostKind Kind;
        uint8 TileReachIndex = 1;
        uint8 BlockReachIndex = 1;
        uint64 StartCycles = 0;
        bool bActive = false;

        explicit FScopedReachCost(ETileReachCostKind InKind)
            : Kind(InKind)
        {
            if (!bTileReachDiagnosticTile
                || !GTileReachCostDiagnosticsEnabled.load(std::memory_order_relaxed))
            {
                return;
            }
            TileReachIndex = TileReachCostIndex(Kind);
            BlockReachIndex = BlockReachCostIndex(Kind);
            StartCycles = FPlatformTime::Cycles64();
            bActive = true;
        }

        ~FScopedReachCost()
        {
            if (!bActive)
            {
                return;
            }
            const uint8 KindIndex = static_cast<uint8>(Kind);
            if (KindIndex < static_cast<uint8>(ETileReachCostKind::Count))
            {
                const uint64 Cycles = FPlatformTime::Cycles64() - StartCycles;
                GTileReachCostCalls[KindIndex][TileReachIndex].fetch_add(
                    1, std::memory_order_relaxed);
                GTileReachCostCycles[KindIndex][TileReachIndex].fetch_add(
                    Cycles,
                    std::memory_order_relaxed);
                if (bTileBlockReachValid
                    && TileReachBlockIndex >= 0
                    && TileReachBlockIndex < TileReachMaxBlockCount)
                {
                    RecordTileReachBlockCost(Kind, BlockReachIndex, Cycles);
                }
            }
        }
    };

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
