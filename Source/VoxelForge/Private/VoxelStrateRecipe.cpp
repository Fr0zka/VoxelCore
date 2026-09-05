#include "VoxelStrateComposer.h"

bool FVoxelStrateMeasuredMetrics::IsUsable() const
{
    return bValid
        && NumSampled > 0
        && NumAir > 0
        && NumSolid > 0
        && NumSampled == NumAir + NumSolid
        && NumAirComponents > 0
        && LargestComponentCells > 0
        && AirComponentCells.Num() == NumAirComponents
        && FMath::IsFinite(AirFraction)
        && FMath::IsFinite(LargestComponentShare)
        && FMath::IsFinite(WalkableFraction)
        && FMath::IsFinite(WalkableFloorAreaFraction)
        && FMath::IsFinite(LargestWalkableSurfaceShare)
        && FMath::IsFinite(MedianFeatureScale)
        && FMath::IsFinite(LargestComponentPoint.X)
        && FMath::IsFinite(LargestComponentPoint.Y)
        && FMath::IsFinite(LargestComponentPoint.Z);
}

const TCHAR* VF_GetStrateArchetypeName(ECaveGeneratorType Archetype)
{
    switch (Archetype)
    {
    case ECaveGeneratorType::TunnelNetwork:  return TEXT("TunnelNetwork");
    case ECaveGeneratorType::FlatPlain:      return TEXT("FlatPlain");
    case ECaveGeneratorType::CrystalChamber: return TEXT("CrystalChamber");
    case ECaveGeneratorType::Maze:            return TEXT("Maze");
    case ECaveGeneratorType::SurfaceWorld:    return TEXT("SurfaceWorld");
    case ECaveGeneratorType::VerticalShafts:  return TEXT("VerticalShafts");
    case ECaveGeneratorType::FloatingIslands: return TEXT("FloatingIslands");
    case ECaveGeneratorType::Underwater:      return TEXT("Underwater");
    }
    return TEXT("Unknown");
}
