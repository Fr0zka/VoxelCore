// Shared measurement-window policy for player-fit diagnostics.
//
// This is deliberately test/editor-side policy. The density generators do not know which two
// points a measurement is asking about, so the caller that knows the archetype must declare
// whether its topology can require the origin spine.

#pragma once

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "VoxelStrateMeasure.h"
#include "VoxelStrateTypes.h"

namespace VoxelForgePlayerFitWindow
{
    /**
     * The four families below have an origin-rooted connectivity implication:
     *
     *   - Maze: every lattice parent chain terminates at (0,0,0).
     *   - VerticalShafts: every drainage-tree parent chain terminates at the origin spine.
     *   - TunnelNetwork/Underwater: their origin-flowing room graph terminates at the origin hub.
     *
     * Slabs, SurfaceWorld, and FloatingIslands have local/independent interior topology for
     * this law. The global structural spine is still generated for them, but it is not the
     * topology that their mouth-to-mouth fit law promises to follow.
     */
    inline bool NeedsOriginInWindow(ECaveGeneratorType Archetype)
    {
        switch (Archetype)
        {
        case ECaveGeneratorType::Maze:
        case ECaveGeneratorType::VerticalShafts:
        case ECaveGeneratorType::TunnelNetwork:
        case ECaveGeneratorType::Underwater:
            return true;
        case ECaveGeneratorType::FlatPlain:
        case ECaveGeneratorType::CrystalChamber:
        case ECaveGeneratorType::SurfaceWorld:
        case ECaveGeneratorType::FloatingIslands:
        default:
            return false;
        }
    }

    inline const TCHAR* RouteWindowName(ECaveGeneratorType Archetype)
    {
        return NeedsOriginInWindow(Archetype)
            ? TEXT("origin-inclusive mouth-AABB + margin")
            : TEXT("mouth-AABB + margin");
    }

    inline FString DescribeResolvedWindow(
        const FVoxelStrateMetrics& Metrics,
        int32 SampleStep,
        ECaveGeneratorType Archetype)
    {
        if (!Metrics.bValid)
        {
            return FString::Printf(
                TEXT("%s step=%d REFUSED: %s"),
                RouteWindowName(Archetype),
                SampleStep,
                Metrics.RefusalReason.IsEmpty() ? TEXT("unknown") : *Metrics.RefusalReason);
        }
        return FString::Printf(
            TEXT("%s step=%d X[%.1f,%.1f) Y[%.1f,%.1f) Z[%d,%d) "
                 "grid=%dx%dx%d cells=%lld"),
            RouteWindowName(Archetype),
            SampleStep,
            Metrics.SampledMinX,
            Metrics.SampledMaxX,
            Metrics.SampledMinY,
            Metrics.SampledMaxY,
            Metrics.SampledMinZ,
            Metrics.SampledMaxZ,
            Metrics.SampledNumX,
            Metrics.SampledNumY,
            Metrics.SampledNumZ,
            static_cast<long long>(Metrics.NumSampled));
    }

    /** Configure the exact fitted XY window used by the player-fit law. */
    inline void ConfigureMouthWindow(
        FVoxelStrateMeasureSettings& InOutSettings,
        const FVector& ArrivalPoint,
        const FVector& DeparturePoint,
        ECaveGeneratorType Archetype)
    {
        InOutSettings.CenterXY = FVector2D(
            0.5f * (ArrivalPoint.X + DeparturePoint.X),
            0.5f * (ArrivalPoint.Y + DeparturePoint.Y));
        InOutSettings.CoverPointA = FVector2D(ArrivalPoint.X, ArrivalPoint.Y);
        InOutSettings.CoverPointB = FVector2D(DeparturePoint.X, DeparturePoint.Y);
        InOutSettings.CoverMarginVoxels = FMath::CeilToFloat(
            InOutSettings.PlayerCapsuleRadiusVoxels) + 2.0f;
        InOutSettings.bIncludeOriginInCoverWindow = NeedsOriginInWindow(Archetype);
    }
}

#endif // WITH_DEV_AUTOMATION_TESTS
