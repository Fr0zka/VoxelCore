// VoxelTerrainOpDefinition.cpp
// Applies a terrain operation's parameters to the generation params struct.

#include "VoxelTerrainOpDefinition.h"

void UVoxelTerrainOpDefinition::ApplyTo(FStrateGenerationParams& OutParams, float Weight) const
{
    // Weight scales the "primary" field of each op type — the one that controls
    // whether the op is active and how strong it is. Other fields (radii, spacing,
    // ratios) are copied directly since they define shape, not intensity.

    switch (Type)
    {
    case EVoxelTerrainOpType::Terrace:
        // TerraceStepHeight is the activation field (0 = disabled)
        OutParams.TerraceStepHeight         = TerraceStepHeight * Weight;
        OutParams.TerraceHardness           = TerraceHardness;
        OutParams.TerraceNoiseDisplacement  = TerraceNoiseDisplacement;
        break;

    case EVoxelTerrainOpType::LayerLines:
        // LayerLineSpacing is the activation field (0 = disabled)
        OutParams.LayerLineSpacing          = LayerLineSpacing * Weight;
        OutParams.LayerLineDepth            = LayerLineDepth;
        break;

    case EVoxelTerrainOpType::Ribbing:
        // RibbingSpacing is the activation field (0 = disabled)
        OutParams.RibbingSpacing            = RibbingSpacing * Weight;
        OutParams.RibbingDepth              = RibbingDepth;
        break;

    case EVoxelTerrainOpType::Cliff:
        OutParams.CliffStrength             = CliffStrength * Weight;
        break;

    case EVoxelTerrainOpType::Scallop:
        OutParams.ScallopStrength           = ScallopStrength * Weight;
        OutParams.ScallopFrequency          = ScallopFrequency;
        break;

    case EVoxelTerrainOpType::Overhang:
        OutParams.OverhangStrength          = OverhangStrength * Weight;
        OutParams.OverhangDepth             = OverhangDepth;
        OutParams.OverhangFrequency         = OverhangFrequency;
        break;

    case EVoxelTerrainOpType::Arch:
        OutParams.ArchDensity               = ArchDensity * Weight;
        OutParams.ArchMinRadius             = ArchMinRadius;
        OutParams.ArchMaxRadius             = ArchMaxRadius;
        break;

    case EVoxelTerrainOpType::Column:
        OutParams.ColumnDensity             = ColumnDensity * Weight;
        OutParams.ColumnMinRadius           = ColumnMinRadius;
        OutParams.ColumnMaxRadius           = ColumnMaxRadius;
        break;

    case EVoxelTerrainOpType::Pit:
        OutParams.PitDensity                = PitDensity * Weight;
        OutParams.PitMinRadius              = PitMinRadius;
        OutParams.PitMaxRadius              = PitMaxRadius;
        OutParams.PitDepth                  = PitDepth;
        break;

    case EVoxelTerrainOpType::Chimney:
        OutParams.ChimneyDensity            = ChimneyDensity * Weight;
        OutParams.ChimneyMinRadius          = ChimneyMinRadius;
        OutParams.ChimneyMaxRadius          = ChimneyMaxRadius;
        OutParams.ChimneyHeight             = ChimneyHeight;
        break;

    case EVoxelTerrainOpType::Dome:
        OutParams.DomeDensity               = DomeDensity * Weight;
        OutParams.DomeMinRadius             = DomeMinRadius;
        OutParams.DomeMaxRadius             = DomeMaxRadius;
        OutParams.DomeHeightRatio           = DomeHeightRatio;
        break;

    case EVoxelTerrainOpType::Pinch:
        OutParams.PinchDensity              = PinchDensity * Weight;
        OutParams.PinchStrength             = PinchStrength;
        OutParams.PinchLength               = PinchLength;
        break;
    }
}
