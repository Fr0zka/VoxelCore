#pragma once
#include "Engine/DataAsset.h"
#include "VoxelStructs.h"
#include "VoxelBiome.generated.h"

USTRUCT(BlueprintType)
struct FBiomeLayer {
    GENERATED_BODY()
    UPROPERTY(EditAnywhere) EVoxelBlockID Block = EVoxelBlockID::Dirt;
    UPROPERTY(EditAnywhere) int32 Thickness = 1; // depth below surface
};

// Per-biome terrain generation parameters
// These control the shape and features of terrain in each biome
USTRUCT(BlueprintType)
struct VOXELCORE_API FBiomeTerrainParams {
    GENERATED_BODY()

    // ==================== BASE TERRAIN HEIGHT ====================

    // Height variation amplitude - controls how tall/flat the terrain is
    // Examples: 50=flat desert, 150=rolling hills, 500=dramatic mountains
    UPROPERTY(EditAnywhere, Category="Terrain|Height", meta=(ClampMin="0", ClampMax="10000", UIMin="10", UIMax="1000"))
    float HeightAmplitude = 500.0f;

    // Base frequency for height noise - controls terrain feature size
    // Lower values = larger features, higher values = more detailed/noisy
    UPROPERTY(EditAnywhere, Category="Terrain|Height", meta=(ClampMin="0.0001", ClampMax="0.01"))
    float HeightFrequency = 0.002f;

    // Number of noise octaves - controls terrain detail layers
    // More octaves = more detail but slower generation
    UPROPERTY(EditAnywhere, Category="Terrain|Height", meta=(ClampMin="1", ClampMax="8"))
    int32 HeightOctaves = 3;

    // ==================== MOUNTAINS ====================

    // Mountain peak height addition (on top of base terrain)
    // Set to 0 to disable mountains in this biome
    UPROPERTY(EditAnywhere, Category="Terrain|Mountains", meta=(ClampMin="0", ClampMax="500", UIMin="0", UIMax="300"))
    float MountainAmplitude = 120.0f;

    // Mountain feature frequency - controls mountain range size
    // Lower = larger mountain ranges, higher = smaller peaks
    UPROPERTY(EditAnywhere, Category="Terrain|Mountains", meta=(ClampMin="0.0001", ClampMax="0.1"))
    float MountainFrequency = 1.f / 128.f;

    // Mountain threshold - controls mountain rarity
    // Higher = fewer mountains (0.7 = rare peaks, 0.3 = many mountains)
    UPROPERTY(EditAnywhere, Category="Terrain|Mountains", meta=(ClampMin="0.0", ClampMax="1.0"))
    float MountainThreshold = 0.55f;

    // Mountain sharpness - controls peak shape
    // Higher = sharper peaks (1.0 = gentle, 2.0 = spiky)
    UPROPERTY(EditAnywhere, Category="Terrain|Mountains", meta=(ClampMin="0.5", ClampMax="3.0"))
    float MountainSharpness = 1.5f;

    // ==================== ADVANCED HEIGHT NOISE ====================

    // Height noise lacunarity - frequency multiplier per octave (2.0 = doubling)
    UPROPERTY(EditAnywhere, Category="Terrain|Height|Advanced", meta=(ClampMin="1.0", ClampMax="4.0"))
    float HeightLacunarity = 2.0f;

    // Height noise gain - amplitude multiplier per octave (0.5 = halving)
    UPROPERTY(EditAnywhere, Category="Terrain|Height|Advanced", meta=(ClampMin="0.0", ClampMax="1.0"))
    float HeightGain = 0.5f;

    // Domain warp strength for height noise (adds organic variation)
    UPROPERTY(EditAnywhere, Category="Terrain|Height|Advanced", meta=(ClampMin="0.0", ClampMax="50.0"))
    float HeightWarpStrength = 0.0f;

    // Seed offset for height noise (for biome variation)
    UPROPERTY(EditAnywhere, Category="Terrain|Height|Advanced")
    int32 HeightSeedOffset = 0;

    // ==================== 3D TERRAIN FEATURES ====================

    // Overhang/arch amplitude - height of overhanging features
    UPROPERTY(EditAnywhere, Category="Terrain|3D Features|Overhangs", meta=(ClampMin="0.0", ClampMax="50.0"))
    float OverhangAmplitude = 6.0f;

    // Overhang frequency - size of overhang features
    UPROPERTY(EditAnywhere, Category="Terrain|3D Features|Overhangs", meta=(ClampMin="0.001", ClampMax="0.1"))
    float OverhangFrequency = 1.f / 24.f;

    // 3D domain warp amplitude - adds organic warping to 3D terrain
    UPROPERTY(EditAnywhere, Category="Terrain|3D Features|Warping", meta=(ClampMin="0.0", ClampMax="50.0"))
    float WarpAmplitude = 8.0f;

    // 3D domain warp frequency
    UPROPERTY(EditAnywhere, Category="Terrain|3D Features|Warping", meta=(ClampMin="0.001", ClampMax="0.1"))
    float WarpFrequency = 1.f / 64.f;

    // Floating island strength
    UPROPERTY(EditAnywhere, Category="Terrain|3D Features|Islands", meta=(ClampMin="0.0", ClampMax="50.0"))
    float IslandAmplitude = 12.0f;

    // Floating island feature size
    UPROPERTY(EditAnywhere, Category="Terrain|3D Features|Islands", meta=(ClampMin="0.001", ClampMax="0.1"))
    float IslandFrequency = 1.f / 48.f;

    // Floating island spawn threshold (higher = rarer islands)
    UPROPERTY(EditAnywhere, Category="Terrain|3D Features|Islands", meta=(ClampMin="0.0", ClampMax="1.0"))
    float IslandThreshold = 0.55f;

    // ==================== CAVES ====================

    // Cave density multiplier
    // 0.0 = no caves, 1.0 = normal caves, 2.0+ = swiss cheese terrain
    UPROPERTY(EditAnywhere, Category="Terrain|Caves", meta=(ClampMin="0.0", ClampMax="5.0", UIMin="0.0", UIMax="3.0"))
    float CaveDensity = 1.0f;

    // 2D cave mask frequency
    UPROPERTY(EditAnywhere, Category="Terrain|Caves|Advanced", meta=(ClampMin="0.0001", ClampMax="0.01"))
    float CaveFrequency2D = 0.002f;

    // 2D cave mask octaves
    UPROPERTY(EditAnywhere, Category="Terrain|Caves|Advanced", meta=(ClampMin="1", ClampMax="6"))
    int32 CaveOctaves2D = 3;

    // 2D cave lacunarity
    UPROPERTY(EditAnywhere, Category="Terrain|Caves|Advanced", meta=(ClampMin="1.0", ClampMax="4.0"))
    float CaveLacunarity2D = 2.0f;

    // 2D cave gain
    UPROPERTY(EditAnywhere, Category="Terrain|Caves|Advanced", meta=(ClampMin="0.0", ClampMax="1.0"))
    float CaveGain2D = 0.5f;

    // 3D cave tube frequency
    UPROPERTY(EditAnywhere, Category="Terrain|Caves|Advanced", meta=(ClampMin="0.0001", ClampMax="0.01"))
    float CaveFrequency3D = 0.002f;

    // 3D cave tube octaves
    UPROPERTY(EditAnywhere, Category="Terrain|Caves|Advanced", meta=(ClampMin="1", ClampMax="6"))
    int32 CaveOctaves3D = 3;

    // Constructor with default values
    FBiomeTerrainParams()
        : HeightAmplitude(500.0f)
        , HeightFrequency(0.002f)
        , HeightOctaves(3)
        , MountainAmplitude(120.0f)
        , MountainFrequency(1.f / 128.f)
        , MountainThreshold(0.55f)
        , MountainSharpness(1.5f)
        , HeightLacunarity(2.0f)
        , HeightGain(0.5f)
        , HeightWarpStrength(0.0f)
        , HeightSeedOffset(0)
        , OverhangAmplitude(6.0f)
        , OverhangFrequency(1.f / 24.f)
        , WarpAmplitude(8.0f)
        , WarpFrequency(1.f / 64.f)
        , IslandAmplitude(12.0f)
        , IslandFrequency(1.f / 48.f)
        , IslandThreshold(0.55f)
        , CaveDensity(1.0f)
        , CaveFrequency2D(0.002f)
        , CaveOctaves2D(3)
        , CaveLacunarity2D(2.0f)
        , CaveGain2D(0.5f)
        , CaveFrequency3D(0.002f)
        , CaveOctaves3D(3)
    {}
};

UCLASS(BlueprintType)
class VOXELCORE_API UVoxelBiomeDef : public UDataAsset
{
    GENERATED_BODY()
public:
    // ==================== BIOME SELECTION ====================

    UPROPERTY(EditAnywhere, Category="Biome")
    FName BiomeName;

    UPROPERTY(EditAnywhere, Category="Biome|Climate")
    float TempMin = 0;

    UPROPERTY(EditAnywhere, Category="Biome|Climate")
    float TempMax = 1;

    UPROPERTY(EditAnywhere, Category="Biome|Climate")
    float MoistMin = 0;

    UPROPERTY(EditAnywhere, Category="Biome|Climate")
    float MoistMax = 1;

    // ==================== TERRAIN GENERATION ====================

    UPROPERTY(EditAnywhere, Category="Terrain Generation")
    FBiomeTerrainParams TerrainParams;

    // ==================== BLOCK TYPES ====================

    UPROPERTY(EditAnywhere, Category="Blocks")
    EVoxelBlockID Surface = EVoxelBlockID::Grass;

    UPROPERTY(EditAnywhere, Category="Blocks")
    TArray<FBiomeLayer> Subsurface; // applied top-down
};

UCLASS(BlueprintType)
class VOXELCORE_API UVoxelBiomeTable : public UDataAsset
{
    GENERATED_BODY()
public:
    UPROPERTY(EditAnywhere) TArray<TObjectPtr<UVoxelBiomeDef>> Biomes;

    const UVoxelBiomeDef* Pick(float Temp, float Moist) const
    {
        const UVoxelBiomeDef* best = nullptr;
        float bestD2 = FLT_MAX;

        for (const UVoxelBiomeDef* B : Biomes)
        {
            if (!B) continue;

            const bool inside =
                (Temp >= B->TempMin && Temp <= B->TempMax) &&
                (Moist >= B->MoistMin && Moist <= B->MoistMax);
            if (inside) return B;

            // distance from point to rectangle (0 if inside)
            const float dx =
                (Temp < B->TempMin) ? (B->TempMin - Temp) :
                (Temp > B->TempMax) ? (Temp - B->TempMax) : 0.0f;
            const float dy =
                (Moist < B->MoistMin) ? (B->MoistMin - Moist) :
                (Moist > B->MoistMax) ? (Moist - B->MoistMax) : 0.0f;

            const float d2 = dx * dx + dy * dy;
            if (d2 < bestD2) { bestD2 = d2; best = B; }
        }

        // If table empty, still nothing to pick.
        return best; // never nullptr when Biomes has at least one entry
    }

    // Helpers for index-based storage
    int32 IndexOf(const UVoxelBiomeDef* B) const
    {
        return Biomes.IndexOfByKey(const_cast<UVoxelBiomeDef*>(B));
    }

    const UVoxelBiomeDef* GetByIndex(int32 Idx) const
    {
        return Biomes.IsValidIndex(Idx) ? Biomes[Idx].Get() : nullptr;
    }
};
