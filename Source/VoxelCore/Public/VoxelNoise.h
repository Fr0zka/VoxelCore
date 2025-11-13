#pragma once

#include "Engine/DataAsset.h"
// Include after struct declaration so inline functions can access FBiomeTerrainParams
#include "VoxelBiome.h"
#include "VoxelNoise.generated.h"

// Forward declaration - full definition in VoxelBiome.h
struct FBiomeTerrainParams;

// Data-only, editable in editor
USTRUCT(BlueprintType)
struct FNoiseChannel {
    GENERATED_BODY()
    UPROPERTY(EditAnywhere) float BaseFreq = 0.002f;
    UPROPERTY(EditAnywhere) int32 Octaves = 3;
    UPROPERTY(EditAnywhere) float Lacunarity = 2.0f;
    UPROPERTY(EditAnywhere) float Gain = 0.5f;
    UPROPERTY(EditAnywhere) float Amplitude = 1.0f;   // used for height
    UPROPERTY(EditAnywhere) float WarpStrength = 0.0f;
    UPROPERTY(EditAnywhere) int32 SeedOffset = 0;
};
USTRUCT(BlueprintType)
struct F3DTerrainParams
{
    GENERATED_BODY()

    // Macro landforms
    UPROPERTY(EditAnywhere, Category = "3D Terrain") float MountainAmplitude = 24.0f;
    UPROPERTY(EditAnywhere, Category = "3D Terrain") float MountainFrequency = 1.f / 128.f;

    // Overhangs / arches
    UPROPERTY(EditAnywhere, Category = "3D Terrain") float OverhangAmplitude = 6.0f;
    UPROPERTY(EditAnywhere, Category = "3D Terrain") float OverhangFrequency = 1.f / 24.f;

    // Domain warp
    UPROPERTY(EditAnywhere, Category = "3D Terrain") float WarpAmplitude = 8.0f;
    UPROPERTY(EditAnywhere, Category = "3D Terrain") float WarpFrequency = 1.f / 64.f;

    // Floating islands band
    UPROPERTY(EditAnywhere, Category = "3D Terrain") float IslandBandCenterZ = 72.0f;
    UPROPERTY(EditAnywhere, Category = "3D Terrain") float IslandBandHalfThickness = 18.0f;
    UPROPERTY(EditAnywhere, Category = "3D Terrain") float IslandAmplitude = 12.0f;
    UPROPERTY(EditAnywhere, Category = "3D Terrain") float IslandFrequency = 1.f / 48.f;
    UPROPERTY(EditAnywhere, Category = "3D Terrain", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float IslandThreshold = 0.55f;
};
UCLASS(BlueprintType)
class UVoxelNoiseProfile : public UDataAsset {
    GENERATED_BODY()
public:
    // ==================== ACTIVE SETTINGS ====================
    // Used for biome selection based on climate noise
    UPROPERTY(EditAnywhere, Category = "Climate (Global)") FNoiseChannel Temperature;
    UPROPERTY(EditAnywhere, Category = "Climate (Global)") FNoiseChannel Moisture;

    // Enable 3D terrain features (overhangs, caves, islands)
    UPROPERTY(EditAnywhere, Category = "3D Terrain (Global)") bool bUse3DTerrain = true;

    // Global island band settings (world-level configuration)
    // Note: Per-biome settings control amplitude, frequency, threshold
    UPROPERTY(EditAnywhere, Category = "3D Terrain (Global)|Islands", meta = (EditCondition = "bUse3DTerrain"))
    float IslandBandCenterZ = 72.0f;

    UPROPERTY(EditAnywhere, Category = "3D Terrain (Global)|Islands", meta = (EditCondition = "bUse3DTerrain"))
    float IslandBandHalfThickness = 18.0f;

    // ==================== DEPRECATED (Legacy - Unused) ====================
    // These settings are now controlled per-biome via FBiomeTerrainParams
    // Kept for backwards compatibility only - values are ignored
    UPROPERTY(EditAnywhere, Category = "Legacy (Unused)", AdvancedDisplay, meta = (DeprecatedProperty, DeprecationMessage = "Height control moved to per-biome TerrainParams"))
    FNoiseChannel Height;

    UPROPERTY(EditAnywhere, Category = "Legacy (Unused)", AdvancedDisplay, meta = (DeprecatedProperty, DeprecationMessage = "Cave control moved to per-biome TerrainParams"))
    FNoiseChannel Cave2D;

    UPROPERTY(EditAnywhere, Category = "Legacy (Unused)", AdvancedDisplay, meta = (DeprecatedProperty, DeprecationMessage = "Cave control moved to per-biome TerrainParams"))
    FNoiseChannel Cave3D;

    UPROPERTY(EditAnywhere, Category = "Legacy (Unused)", AdvancedDisplay, meta = (DeprecatedProperty, DeprecationMessage = "3D terrain control moved to per-biome TerrainParams (except global island band settings above)"))
    F3DTerrainParams Terrain3D;
};

struct FVoxelNoiseContext
{
    int32 WorldSeed = 0;
    const UVoxelNoiseProfile* Profile = nullptr;

    explicit FVoxelNoiseContext(int32 Seed, const UVoxelNoiseProfile* P) : WorldSeed(Seed), Profile(P) {}

    // 0..1
    float Sample01_2D(const FNoiseChannel& C, float x, float y) const;
    // -1..1
    float SampleSS_3D(const FNoiseChannel& C, float x, float y, float z) const;

    // Conveniences
    float Temp(float x, float y)  const { return Sample01_2D(Profile->Temperature, x, y); }
    float Moist(float x, float y) const { return Sample01_2D(Profile->Moisture, x, y); }

    // Legacy functions (will be deprecated - use biome-aware versions)
    float HeightAbs(float x, float y, int32 baseH, float amp) const;
    bool  IsCave(float x, float y, float zWorld, float terrainH) const;
    float MacroSurfaceZ(float x, float y, int32 BaseHeight, float HeightAmp) const; // H2D + mountains

    // NEW: Biome-aware terrain generation functions
    float Sample01_2D_Custom(float x, float y, int32 octaves) const;
    float HeightAbs_Biome(float x, float y, int32 baseH, const struct FBiomeTerrainParams& Params) const;
    float MacroSurfaceZ_Biome(float x, float y, int32 BaseHeight, const struct FBiomeTerrainParams& Params) const;
    bool  IsCave_Biome(float x, float y, float zWorld, float terrainH, const struct FBiomeTerrainParams& Params) const;
    void  SampleClimate(float x, float y, float& outTemp, float& outMoist) const;

    // New: full 3D density field. Positive => solid
    float Density3D(float x, float y, float zWorld,
        int32 BaseHeight, float HeightAmplitude, int32 WaterLevel) const;

    // Legacy inline function (uses deprecated Terrain3D)
    FORCEINLINE float Density3D_FromMacro(float x, float y, float zWorld, float Macro) const
    {
        check(Profile);
        const auto& TP = Profile->Terrain3D;

        // Domain warp inline
        FVector3f pw(x, y, zWorld);
        if (TP.WarpAmplitude > 0.f)
        {
            const float f = TP.WarpFrequency, s = TP.WarpAmplitude;
            const float ox = WorldSeed * 0.17f, oy = WorldSeed * 0.29f, oz = WorldSeed * 0.41f;
            const float wx = FMath::PerlinNoise3D({ (x + ox) * f,  y * f,        zWorld * f });
            const float wy = FMath::PerlinNoise3D({ x * f,     (y + oy) * f,    zWorld * f });
            const float wz = FMath::PerlinNoise3D({ x * f,       y * f,      (zWorld + oz) * f });
            pw = FVector3f(x + wx * s, y + wy * s, zWorld + wz * s);
        }

        float D = Macro - zWorld;

        if (TP.OverhangAmplitude > 0.f)
            D += TP.OverhangAmplitude *
            FMath::PerlinNoise3D({ pw.X * TP.OverhangFrequency,
                                  pw.Y * TP.OverhangFrequency,
                                  pw.Z * TP.OverhangFrequency });

        if (TP.IslandAmplitude > 0.f)
        {
            const float iband = 1.f - FMath::Clamp(FMath::Abs(zWorld - TP.IslandBandCenterZ) /
                FMath::Max(1.f, TP.IslandBandHalfThickness), 0.f, 1.f);
            const float islandField = 1.f - FMath::Abs(FMath::PerlinNoise3D(
                { pw.X * TP.IslandFrequency + 11.3f,
                 pw.Y * TP.IslandFrequency + 27.1f,
                 pw.Z * TP.IslandFrequency - 5.7f }));
            const float Islands = iband * ((islandField - TP.IslandThreshold) * TP.IslandAmplitude);
            if (zWorld >= Macro + 6.f)
                D = FMath::Max(D, Islands);
        }

        // Note: Cave carving is now handled separately in the generator
        // for better control and performance

        return D;
    }

    // NEW: Biome-aware 3D density using biome-specific parameters
    FORCEINLINE float Density3D_FromMacro_Biome(float x, float y, float zWorld, float Macro,
        const struct FBiomeTerrainParams& Params) const
    {
        // Domain warp using biome-specific parameters
        FVector3f pw(x, y, zWorld);
        if (Params.WarpAmplitude > 0.f)
        {
            const float f = Params.WarpFrequency, s = Params.WarpAmplitude;
            const float ox = WorldSeed * 0.17f, oy = WorldSeed * 0.29f, oz = WorldSeed * 0.41f;
            const float wx = FMath::PerlinNoise3D({ (x + ox) * f,  y * f,        zWorld * f });
            const float wy = FMath::PerlinNoise3D({ x * f,     (y + oy) * f,    zWorld * f });
            const float wz = FMath::PerlinNoise3D({ x * f,       y * f,      (zWorld + oz) * f });
            pw = FVector3f(x + wx * s, y + wy * s, zWorld + wz * s);
        }

        float D = Macro - zWorld;

        // Overhangs and arches using biome-specific parameters
        if (Params.OverhangAmplitude > 0.f)
            D += Params.OverhangAmplitude *
                FMath::PerlinNoise3D({ pw.X * Params.OverhangFrequency,
                                      pw.Y * Params.OverhangFrequency,
                                      pw.Z * Params.OverhangFrequency });

        // Floating islands using biome-specific parameters
        // Note: Island band altitude is global (IslandBandCenterZ from Profile)
        if (Params.IslandAmplitude > 0.f && Profile)
        {
            const float iband = 1.f - FMath::Clamp(FMath::Abs(zWorld - Profile->IslandBandCenterZ) /
                FMath::Max(1.f, Profile->IslandBandHalfThickness), 0.f, 1.f);
            const float islandField = 1.f - FMath::Abs(FMath::PerlinNoise3D(
                { pw.X * Params.IslandFrequency + 11.3f,
                 pw.Y * Params.IslandFrequency + 27.1f,
                 pw.Z * Params.IslandFrequency - 5.7f }));
            const float Islands = iband * ((islandField - Params.IslandThreshold) * Params.IslandAmplitude);
            if (zWorld >= Macro + 6.f)
                D = FMath::Max(D, Islands);
        }

        return D;
    }
};

