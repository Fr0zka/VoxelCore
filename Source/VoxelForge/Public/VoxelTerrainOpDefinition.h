// VoxelTerrainOpDefinition.h
// Individual terrain operation data asset — one per operation variant.
//
// DESIGN:
// ========
// Each terrain operation (pit, arch, terrace, etc.) is its own asset.
// The strate definition holds a weighted array of references to these.
// This allows:
//   - Reuse: "DA_Op_DeepPit" can be referenced by multiple strates
//   - Composition: mix different ops per strate by drag-and-drop
//   - Tuning: tweak one asset, all strates using it update
//
// HOW PARAMS FLOW:
// At generation time, the strate manager builds FStrateGenerationParams by:
//   1. Starting from the strate definition's base params (terrain op fields = 0)
//   2. For each terrain op reference: calling ApplyTo() which copies the op's
//      fields into the params, scaled by the entry's Weight.
//   3. The generator reads the final params as usual — no code change needed.
//
// EDITOR UX:
// The Type enum controls which param group is visible via EditCondition.
// When you select "Pit", only pit-related fields appear in the Details panel.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "VoxelStrateTypes.h"
#include "VoxelTerrainOpDefinition.generated.h"

/**
 * EVoxelTerrainOpType — Which terrain operation this asset configures.
 * Each type maps to a specific density modification in the generation pipeline.
 */
UENUM(BlueprintType)
enum class EVoxelTerrainOpType : uint8
{
    // Surface treatments (modify cave wall character)
    Terrace       UMETA(DisplayName = "Terrace / Ledges"),
    LayerLines    UMETA(DisplayName = "Layer Lines"),
    Ribbing       UMETA(DisplayName = "Ribbing"),
    Cliff         UMETA(DisplayName = "Cliff Sharpening"),
    Scallop       UMETA(DisplayName = "Scallop / Erosion"),
    Overhang      UMETA(DisplayName = "Overhang / Shelf"),

    // Structural features (place geometry in caves)
    Arch          UMETA(DisplayName = "Arch / Bridge"),
    Column        UMETA(DisplayName = "Column / Pillar"),
    Pit           UMETA(DisplayName = "Pit / Shaft (downward)"),
    Chimney       UMETA(DisplayName = "Chimney / Shaft (upward)"),
    Dome          UMETA(DisplayName = "Dome (cathedral ceiling)"),
    Pinch         UMETA(DisplayName = "Pinch / Bottleneck"),
};

/**
 * UVoxelTerrainOpDefinition — A single terrain operation, configured as a data asset.
 *
 * Create these in your Content folder (e.g., Content/TerrainOps/DA_Op_DeepPit).
 * Then reference them from your strate definitions with a weight.
 *
 * USAGE EXAMPLE:
 *   1. Create: Right-click Content → Miscellaneous → Data Asset → VoxelTerrainOpDefinition
 *   2. Set Type to "Pit", configure depth/radius/taper
 *   3. In your strate definition, add this asset to TerrainOperations with Weight=1.0
 */
UCLASS(BlueprintType)
class VOXELFORGE_API UVoxelTerrainOpDefinition : public UPrimaryDataAsset
{
    GENERATED_BODY()

public:

    //=========================================================================
    // OPERATION TYPE
    //=========================================================================

    // Which terrain operation this asset configures.
    // Changing this shows/hides the relevant parameter group below.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terrain Operation")
    EVoxelTerrainOpType Type = EVoxelTerrainOpType::Terrace;

    //=========================================================================
    // TERRACE PARAMS (visible when Type == Terrace)
    //=========================================================================

    // Vertical spacing between terrace steps (in voxels). 0 = disabled.
    //   3-5 → tight steps (staircase feel)
    //   8-12 → wide ledges (platforming scale)
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terrace",
        meta = (EditCondition = "Type == EVoxelTerrainOpType::Terrace"))
    float TerraceStepHeight = 5.0f;

    // Edge sharpness: 0 = rounded steps, 1 = razor sharp edges.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terrace",
        meta = (EditCondition = "Type == EVoxelTerrainOpType::Terrace",
                ClampMin = "0.0", ClampMax = "1.0"))
    float TerraceHardness = 0.5f;

    // Noise displacement on step edges (organic variation).
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terrace",
        meta = (EditCondition = "Type == EVoxelTerrainOpType::Terrace",
                ClampMin = "0.0", ClampMax = "2.0"))
    float TerraceNoiseDisplacement = 0.5f;

    //=========================================================================
    // LAYER LINES PARAMS (visible when Type == LayerLines)
    //=========================================================================

    // Spacing between horizontal geological lines (in voxels). 0 = disabled.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Layer Lines",
        meta = (EditCondition = "Type == EVoxelTerrainOpType::LayerLines"))
    float LayerLineSpacing = 4.0f;

    // Depth of the grooves (how much density is removed).
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Layer Lines",
        meta = (EditCondition = "Type == EVoxelTerrainOpType::LayerLines",
                ClampMin = "0.0", ClampMax = "2.0"))
    float LayerLineDepth = 0.3f;

    //=========================================================================
    // RIBBING PARAMS (visible when Type == Ribbing)
    //=========================================================================

    // Spacing between parallel ribs along Z (in voxels). 0 = disabled.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ribbing",
        meta = (EditCondition = "Type == EVoxelTerrainOpType::Ribbing"))
    float RibbingSpacing = 3.0f;

    // Depth of the ridges.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ribbing",
        meta = (EditCondition = "Type == EVoxelTerrainOpType::Ribbing",
                ClampMin = "0.0", ClampMax = "3.0"))
    float RibbingDepth = 0.4f;

    //=========================================================================
    // CLIFF PARAMS (visible when Type == Cliff)
    //=========================================================================

    // How much to amplify vertical gradients near cave surfaces.
    // 0 = no effect, 1 = maximum sheer cliff amplification.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cliff",
        meta = (EditCondition = "Type == EVoxelTerrainOpType::Cliff",
                ClampMin = "0.0", ClampMax = "1.0"))
    float CliffStrength = 0.5f;

    //=========================================================================
    // SCALLOP PARAMS (visible when Type == Scallop)
    //=========================================================================

    // Strength of the erosion bowl patterns (cellular noise subtraction).
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Scallop",
        meta = (EditCondition = "Type == EVoxelTerrainOpType::Scallop",
                ClampMin = "0.0", ClampMax = "2.0"))
    float ScallopStrength = 0.5f;

    // Frequency of the cellular noise (smaller = larger bowls).
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Scallop",
        meta = (EditCondition = "Type == EVoxelTerrainOpType::Scallop"))
    float ScallopFrequency = 0.1f;

    //=========================================================================
    // OVERHANG PARAMS (visible when Type == Overhang)
    //=========================================================================

    // Probability/strength of overhang generation (0 = none, 1 = max).
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Overhang",
        meta = (EditCondition = "Type == EVoxelTerrainOpType::Overhang",
                ClampMin = "0.0", ClampMax = "1.0"))
    float OverhangStrength = 0.5f;

    // How far the overhang protrudes from the wall (in voxels).
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Overhang",
        meta = (EditCondition = "Type == EVoxelTerrainOpType::Overhang"))
    float OverhangDepth = 5.0f;

    // Frequency of the noise that creates overhangs (lower = broader shelves).
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Overhang",
        meta = (EditCondition = "Type == EVoxelTerrainOpType::Overhang"))
    float OverhangFrequency = 0.06f;

    //=========================================================================
    // ARCH PARAMS (visible when Type == Arch)
    //=========================================================================

    // Probability per grid cell (0 = none, 0.3 = max recommended).
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Arch",
        meta = (EditCondition = "Type == EVoxelTerrainOpType::Arch",
                ClampMin = "0.0", ClampMax = "0.3"))
    float ArchDensity = 0.1f;

    // Minimum arch thickness (radius of the capsule SDF).
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Arch",
        meta = (EditCondition = "Type == EVoxelTerrainOpType::Arch",
                ClampMin = "1.0"))
    float ArchMinRadius = 3.0f;

    // Maximum arch thickness.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Arch",
        meta = (EditCondition = "Type == EVoxelTerrainOpType::Arch",
                ClampMin = "1.0"))
    float ArchMaxRadius = 6.0f;


    //=========================================================================
    // COLUMN PARAMS (visible when Type == Column)
    //=========================================================================

    // Probability per grid cell (0 = none, 1 = every cell).
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Column",
        meta = (EditCondition = "Type == EVoxelTerrainOpType::Column",
                ClampMin = "0.0", ClampMax = "1.0"))
    float ColumnDensity = 0.15f;

    // Minimum column radius.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Column",
        meta = (EditCondition = "Type == EVoxelTerrainOpType::Column",
                ClampMin = "0.5"))
    float ColumnMinRadius = 2.0f;

    // Maximum column radius.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Column",
        meta = (EditCondition = "Type == EVoxelTerrainOpType::Column",
                ClampMin = "1.0"))
    float ColumnMaxRadius = 5.0f;


    //=========================================================================
    // PIT PARAMS (visible when Type == Pit)
    //=========================================================================

    // Probability per grid cell.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pit",
        meta = (EditCondition = "Type == EVoxelTerrainOpType::Pit",
                ClampMin = "0.0", ClampMax = "0.5"))
    float PitDensity = 0.1f;

    // Minimum pit radius at the top.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pit",
        meta = (EditCondition = "Type == EVoxelTerrainOpType::Pit",
                ClampMin = "1.0"))
    float PitMinRadius = 4.0f;

    // Maximum pit radius at the top.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pit",
        meta = (EditCondition = "Type == EVoxelTerrainOpType::Pit",
                ClampMin = "2.0"))
    float PitMaxRadius = 10.0f;

    // How deep the pit extends downward (in voxels).
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pit",
        meta = (EditCondition = "Type == EVoxelTerrainOpType::Pit",
                ClampMin = "5.0"))
    float PitDepth = 25.0f;


    //=========================================================================
    // CHIMNEY PARAMS (visible when Type == Chimney)
    //=========================================================================

    // Probability per grid cell.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Chimney",
        meta = (EditCondition = "Type == EVoxelTerrainOpType::Chimney",
                ClampMin = "0.0", ClampMax = "1.0"))
    float ChimneyDensity = 0.1f;

    // Minimum chimney radius.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Chimney",
        meta = (EditCondition = "Type == EVoxelTerrainOpType::Chimney",
                ClampMin = "0.5"))
    float ChimneyMinRadius = 2.0f;

    // Maximum chimney radius.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Chimney",
        meta = (EditCondition = "Type == EVoxelTerrainOpType::Chimney",
                ClampMin = "1.0"))
    float ChimneyMaxRadius = 5.0f;

    // How high the chimney extends upward (in voxels).
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Chimney",
        meta = (EditCondition = "Type == EVoxelTerrainOpType::Chimney",
                ClampMin = "5.0"))
    float ChimneyHeight = 20.0f;


    //=========================================================================
    // DOME PARAMS (visible when Type == Dome)
    //=========================================================================

    // Probability per grid cell.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dome",
        meta = (EditCondition = "Type == EVoxelTerrainOpType::Dome",
                ClampMin = "0.0", ClampMax = "1.0"))
    float DomeDensity = 0.15f;

    // Minimum dome radius.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dome",
        meta = (EditCondition = "Type == EVoxelTerrainOpType::Dome",
                ClampMin = "3.0"))
    float DomeMinRadius = 8.0f;

    // Maximum dome radius.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dome",
        meta = (EditCondition = "Type == EVoxelTerrainOpType::Dome",
                ClampMin = "4.0"))
    float DomeMaxRadius = 15.0f;

    // Height-to-radius ratio (0.5 = flat, 1.0 = hemisphere, 1.5 = tall/gothic).
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dome",
        meta = (EditCondition = "Type == EVoxelTerrainOpType::Dome",
                ClampMin = "0.3", ClampMax = "2.0"))
    float DomeHeightRatio = 0.8f;


    //=========================================================================
    // PINCH PARAMS (visible when Type == Pinch)
    //=========================================================================

    // Probability per grid cell.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pinch",
        meta = (EditCondition = "Type == EVoxelTerrainOpType::Pinch",
                ClampMin = "0.0", ClampMax = "1.0"))
    float PinchDensity = 0.15f;

    // How much the passage narrows (voxels of added density from each side).
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pinch",
        meta = (EditCondition = "Type == EVoxelTerrainOpType::Pinch",
                ClampMin = "1.0"))
    float PinchStrength = 5.0f;

    // Length of the narrowed section along the passage.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pinch",
        meta = (EditCondition = "Type == EVoxelTerrainOpType::Pinch",
                ClampMin = "3.0"))
    float PinchLength = 12.0f;


    //=========================================================================
    // APPLY TO GENERATION PARAMS
    //=========================================================================

    /**
     * Copy this operation's parameters into a FStrateGenerationParams struct.
     * Only the fields matching this operation's Type are written.
     * Weight scales the primary "strength" or "density" field for intensity control.
     *
     * @param OutParams - The generation params to write into
     * @param Weight - Multiplier for the op's intensity (1.0 = full strength)
     */
    void ApplyTo(FStrateGenerationParams& OutParams, float Weight = 1.0f) const;
};
