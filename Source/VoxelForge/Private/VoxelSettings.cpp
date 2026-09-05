#include "VoxelSettings.h"

#include "VoxelSeasonAsset.h"

int32 UVoxelSettings::GetEffectiveWorldSeed() const
{
    if (!Season.IsNull())
    {
        if (const UVoxelSeasonAsset* Asset = Season.LoadSynchronous())
        {
            return Asset->GetSeasonSeed();
        }
    }
    return Seed;
}

float UVoxelSettings::GetEffectiveOriginSpineRadius() const
{
    if (!Season.IsNull())
    {
        if (const UVoxelSeasonAsset* Asset = Season.LoadSynchronous())
        {
            return Asset->GetOriginSpineRadius();
        }
    }
    return OriginSpineRadius;
}

float UVoxelSettings::GetEffectiveWorldRadiusVoxels() const
{
    if (!Season.IsNull())
    {
        if (const UVoxelSeasonAsset* Asset = Season.LoadSynchronous())
        {
            return Asset->GetWorldRadiusVoxels();
        }
    }
    return WorldRadiusVoxels;
}

int32 UVoxelSettings::GetEffectiveSeasonNumber() const
{
    if (!Season.IsNull())
    {
        if (const UVoxelSeasonAsset* Asset = Season.LoadSynchronous())
        {
            return Asset->GetSeasonNumber();
        }
    }
    return CurrentSeason;
}
