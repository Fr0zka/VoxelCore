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

    // Floating island vertical band center (altitude where islands are most common)
    UPROPERTY(EditAnywhere, Category="Terrain|3D Features|Islands", meta=(ClampMin="-500.0", ClampMax="500.0"))
    float IslandBandCenterZ = 72.0f;

    // Floating island vertical band half-thickness (vertical range where islands can spawn)
    UPROPERTY(EditAnywhere, Category="Terrain|3D Features|Islands", meta=(ClampMin="1.0", ClampMax="200.0"))
    float IslandBandHalfThickness = 18.0f;

    // ==================== CAVES ====================
    // New 3D cave system: realistic chambers and tunnels
    // Uses pure world-space noise to avoid surface correlation artifacts

    // Cave density multiplier
    // 0.0 = no caves, 1.0 = normal caves, 2.0+ = swiss cheese terrain
    UPROPERTY(EditAnywhere, Category="Terrain|Caves", meta=(ClampMin="0.0", ClampMax="5.0", UIMin="0.0", UIMax="3.0"))
    float CaveDensity = 1.0f;

    // Base threshold for cave carving (higher = fewer/smaller caves)
    // Perlin noise is in range [-1, 1], so threshold 0.0 = ~50% caves, 0.3 = ~35% caves
    // Range: -0.5 to 0.5, typical values -0.1 to 0.2
    UPROPERTY(EditAnywhere, Category="Terrain|Caves", meta=(ClampMin="-0.5", ClampMax="0.5", UIMin="-0.2", UIMax="0.3"))
    float CaveThreshold = 0.0f;

    // === CHAMBER PARAMETERS (Large open spaces) ===

    // Chamber frequency (lower = larger chambers)
    // Frequency of 0.004 = wavelength ~250 voxels = medium-large chambers
    UPROPERTY(EditAnywhere, Category="Terrain|Caves|Chambers", meta=(ClampMin="0.0001", ClampMax="0.01", UIMin="0.0005", UIMax="0.005"))
    float CaveChamberFrequency = 0.004f;

    // Chamber detail octaves
    UPROPERTY(EditAnywhere, Category="Terrain|Caves|Chambers", meta=(ClampMin="1", ClampMax="4"))
    int32 CaveChamberOctaves = 3;

    // === TUNNEL PARAMETERS (Connecting passages) ===

    // Tunnel frequency (higher = more intricate tunnels)
    // Frequency of 0.008 = wavelength ~125 voxels = medium tunnels
    UPROPERTY(EditAnywhere, Category="Terrain|Caves|Tunnels", meta=(ClampMin="0.0001", ClampMax="0.01", UIMin="0.001", UIMax="0.008"))
    float CaveTunnelFrequency = 0.008f;

    // Tunnel detail octaves
    UPROPERTY(EditAnywhere, Category="Terrain|Caves|Tunnels", meta=(ClampMin="1", ClampMax="4"))
    int32 CaveTunnelOctaves = 4;

    // === DOMAIN WARPING (Creates organic, curvy shapes) ===

    // Warp frequency (controls curvature scale)
    UPROPERTY(EditAnywhere, Category="Terrain|Caves|Warping", meta=(ClampMin="0.0001", ClampMax="0.01", UIMin="0.001", UIMax="0.005"))
    float CaveWarpFrequency = 0.002f;

    // Warp amplitude (controls how much caves curve)
    UPROPERTY(EditAnywhere, Category="Terrain|Caves|Warping", meta=(ClampMin="0.0", ClampMax="500.0", UIMin="10.0", UIMax="200.0"))
    float CaveWarpAmplitude = 80.0f;

    // === NOISE PARAMETERS ===

    // Lacunarity for cave noise (frequency multiplier per octave)
    UPROPERTY(EditAnywhere, Category="Terrain|Caves|Noise", meta=(ClampMin="1.0", ClampMax="4.0"))
    float CaveLacunarity = 2.0f;

    // Gain for cave noise (amplitude multiplier per octave)
    UPROPERTY(EditAnywhere, Category="Terrain|Caves|Noise", meta=(ClampMin="0.0", ClampMax="1.0"))
    float CaveGain = 0.5f;

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
        , CaveThreshold(0.0f)
        , CaveChamberFrequency(0.004f)
        , CaveChamberOctaves(3)
        , CaveTunnelFrequency(0.008f)
        , CaveTunnelOctaves(4)
        , CaveWarpFrequency(0.002f)
        , CaveWarpAmplitude(80.0f)
        , CaveLacunarity(2.0f)
        , CaveGain(0.5f)
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
