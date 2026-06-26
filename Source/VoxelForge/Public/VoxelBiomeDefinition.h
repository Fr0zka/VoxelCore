// VoxelBiomeDefinition.h
// Data asset defining one biome: where it lives (climate), how it reshapes the base
// terrain (modulation), and what content/atmosphere it brings.
//
// HOW TO USE:
// -----------
// 1. Content Browser → Miscellaneous → Data Asset → "VoxelBiomeDefinition".
// 2. Set its climate box (ReliefMin/Max, MoistureMin/Max), its terrain override, and its
//    Decorations / atmosphere / water.
// 3. Add it to a UVoxelStrateDefinition's Biomes[] list.
//
// A biome is generator-agnostic: the same asset works inside any archetype strate
// (surface biomes today, cave biomes later). The strate's base archetype params are
// the baseline; this asset modulates on top.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "GameplayTagContainer.h"
#include "VoxelBiomeTypes.h"
#include "VoxelStrateTypes.h"   // FStrateDecoration / FStrateAmbientActor
#include "VoxelBiomeDefinition.generated.h"

/**
 * UVoxelBiomeDefinition — one biome's identity, placement, terrain modulation and content.
 */
UCLASS(BlueprintType)
class VOXELFORGE_API UVoxelBiomeDefinition : public UPrimaryDataAsset
{
    GENERATED_BODY()

public:
    //=========================================================================
    // IDENTITY
    //=========================================================================

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Biome")
    FText BiomeName;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Biome", meta = (MultiLine = true))
    FText BiomeDescription;

    // Colour used by the 2D biome-preview bake to identify this biome's regions.
    // Pick something distinct per biome so the baked map is readable.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Biome")
    FLinearColor DebugColor = FLinearColor(0.5f, 0.5f, 0.5f, 1.0f);

    //=========================================================================
    // CLIMATE PLACEMENT
    //=========================================================================
    // A biome wins the Voronoi cells whose site climate (relief, moisture) falls inside
    // — or, failing any match, nearest to — this box. Both axes are [0,1].
    //   Mountains / snow → high ReliefMin.   Lush forest → high Moisture.
    //   Desert → low Moisture.               Plains → mid relief, any moisture.
    // Boxes may overlap; ties are broken deterministically per cell.

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Biome|Climate", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float ReliefMin = 0.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Biome|Climate", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float ReliefMax = 1.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Biome|Climate", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float MoistureMin = 0.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Biome|Climate", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float MoistureMax = 1.0f;

    //=========================================================================
    // TERRAIN OVERRIDE — a biome is a "mini-strate-variant"
    //=========================================================================
    // When enabled, this biome supplies its OWN archetype params for its regions,
    // replacing the strate's. Heightfield archetypes (SurfaceWorld) blend the output
    // across biome borders, so even frequency/shape differences stay seamless.
    // Global/structural fields (strate Z bounds, boundary seal, base density, water
    // level) are always forced from the strate — a biome can't break seals/connectivity.
    //
    // GeneratorType must match the strate this biome is used in; if it doesn't, the
    // override is ignored and the strate's params are used. (Currently wired: SurfaceWorld.
    // Other archetypes' param overrides arrive as each is hooked up — see CODEMAP §8.14.)

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Biome|Terrain")
    bool bOverrideTerrain = false;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Biome|Terrain",
        meta = (EditCondition = "bOverrideTerrain"))
    ECaveGeneratorType GeneratorType = ECaveGeneratorType::SurfaceWorld;

    // Open-sky terrain override (used when bOverrideTerrain && GeneratorType == SurfaceWorld).
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Biome|Terrain",
        meta = (EditCondition = "bOverrideTerrain && GeneratorType == ECaveGeneratorType::SurfaceWorld"))
    FSurfaceGenerationParams SurfaceParams;

    //=========================================================================
    // CONTENT PROFILE (consumed by the content / atmosphere managers)
    //=========================================================================

    // Decorations placed in this biome's regions. When set, these REPLACE the strate's
    // decorations for chunks whose dominant biome is this one (falls back to the strate
    // list when empty).
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Biome|Content")
    TArray<FStrateDecoration> Decorations;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Biome|Content")
    TArray<FStrateAmbientActor> AmbientActors;

    //=========================================================================
    // ATMOSPHERE OVERRIDE (optional — beats the strate's atmosphere for this biome)
    //=========================================================================

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Biome|Atmosphere")
    bool bOverrideAtmosphere = false;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Biome|Atmosphere", meta = (EditCondition = "bOverrideAtmosphere"))
    FLinearColor FogColor = FLinearColor(0.05f, 0.05f, 0.1f, 1.0f);

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Biome|Atmosphere", meta = (EditCondition = "bOverrideAtmosphere", ClampMin = "0.0", ClampMax = "1.0"))
    float FogDensity = 0.3f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Biome|Atmosphere", meta = (EditCondition = "bOverrideAtmosphere"))
    FLinearColor AmbientLightColor = FLinearColor(0.1f, 0.1f, 0.15f, 1.0f);

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Biome|Atmosphere", meta = (EditCondition = "bOverrideAtmosphere", ClampMin = "0.0"))
    float AmbientLightIntensity = 0.5f;

    //=========================================================================
    // WATER
    //=========================================================================

    // Water material override for this biome (null = use the strate's WaterMaterial).
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Biome|Water")
    UMaterialInterface* WaterMaterial = nullptr;

    //=========================================================================
    // MATERIAL — F6 vertex-colour triplanar palette
    //=========================================================================
    // Which palette layer this biome's terrain uses in the master triplanar material.
    // It is baked into the mesh vertex colour (R channel) at mesh time, so ONE material
    // re-skins the terrain per biome and cross-fades across biome borders (the neighbour
    // biome's index + a blend weight ride in the A/B channels). The terrain material is
    // still the strate's OverrideMaterial / Settings->VoxelMaterial — author it as the
    // master palette material and switch its layers on this index. 0 = default layer.

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Biome|Material", meta = (ClampMin = "0", ClampMax = "255"))
    int32 MaterialPaletteIndex = 0;

    //=========================================================================
    // GAMEPLAY TAGS
    //=========================================================================

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Biome|Gameplay")
    FGameplayTagContainer GameplayTags;
};
