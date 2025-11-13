#include "VoxelNoise.h"
#include "VoxelBiome.h"

static FORCEINLINE float fBm2D(float x, float y, int oct, float lac, float gain) {
    float a = 1, f = 1, s = 0, n = 0;
    for (int i = 0; i < oct; ++i) { n += a * FMath::PerlinNoise2D(FVector2D(x * f, y * f)); s += a; a *= gain; f *= lac; }
    return (n / s) * 0.5f + 0.5f; // 0..1
}
static FORCEINLINE float fBm3D(float x, float y, float z, int oct, float lac, float gain) {
    float a = 1, f = 1, s = 0, n = 0;
    for (int i = 0; i < oct; ++i) { n += a * FMath::PerlinNoise3D(FVector(x * f, y * f, z * f)); s += a; a *= gain; f *= lac; }
    return n / s; // -1..1
}
// ---- 3D density helpers ----
static FORCEINLINE float Saturate(float v) { return FMath::Clamp(v, 0.f, 1.f); }

static FORCEINLINE float SmoothStep(float a, float b, float x) {
    const float t = Saturate((x - a) / (b - a)); return t * t * (3.f - 2.f * t);
}

static FORCEINLINE float Ridged3D(float x, float y, float z) { return 1.f - FMath::Abs(FMath::PerlinNoise3D({ x,y,z })); }

static FORCEINLINE float Ridged2D(float x, float y) {
    return 1.f - FMath::Abs(FMath::PerlinNoise2D(FVector2D(x, y))); // [0,1]
}
static FORCEINLINE FVector3f DomainWarp3D(float amp, float freq, int32 seed, float x, float y, float z)
{
    if (amp <= 0.f) return FVector3f(x, y, z);
    const float f = freq, s = amp;
    const float ox = seed * 0.17f, oy = seed * 0.29f, oz = seed * 0.41f;
    const float wx = FMath::PerlinNoise3D(FVector((x + ox) * f, y * f, z * f));
    const float wy = FMath::PerlinNoise3D(FVector(x * f, (y + oy) * f, z * f));
    const float wz = FMath::PerlinNoise3D(FVector(x * f, y * f, (z + oz) * f));
    return FVector3f(x + wx * s, y + wy * s, z + wz * s);
}
static FORCEINLINE void DomainWarp3(float x, float y, float z, float amp, float freq,
    float& dx, float& dy, float& dz, float seed)
{
    const float ox = seed * 0.13f, oy = seed * 0.17f, oz = seed * 0.23f;
    dx = FMath::PerlinNoise3D({ (x + ox) * freq,  y * freq,        z * freq }) * amp;
    dy = FMath::PerlinNoise3D({ x * freq,      (y + oy) * freq,   z * freq }) * amp;
    dz = FMath::PerlinNoise3D({ x * freq,       y * freq,        (z + oz) * freq }) * amp;
}

float FVoxelNoiseContext::Sample01_2D(const FNoiseChannel& C, float x, float y) const {
    const float sx = (x + (WorldSeed + C.SeedOffset) * 11) * C.BaseFreq;
    const float sy = (y + (WorldSeed + C.SeedOffset) * 13) * C.BaseFreq;
    float nx = sx, ny = sy;
    if (C.WarpStrength > 0) {
        const float wx = fBm2D(sx + 17, sy + 23, C.Octaves, C.Lacunarity, C.Gain) - 0.5f;
        const float wy = fBm2D(sx + 31, sy + 37, C.Octaves, C.Lacunarity, C.Gain) - 0.5f;
        nx += C.WarpStrength * wx; ny += C.WarpStrength * wy;
    }
    return fBm2D(nx, ny, C.Octaves, C.Lacunarity, C.Gain);
}

float FVoxelNoiseContext::HeightAbs(float x, float y, int32 baseH, float amp) const {
    const float n01 = Sample01_2D(Profile->Height, x, y);
    return baseH + (n01 - 0.5f) * 2.0f * amp;
}

float FVoxelNoiseContext::SampleSS_3D(const FNoiseChannel& C, float x, float y, float z) const {
    const float sx = (x + (WorldSeed + C.SeedOffset) * 19) * C.BaseFreq;
    const float sy = (y + (WorldSeed + C.SeedOffset) * 23) * C.BaseFreq;
    const float sz = (z + (WorldSeed + C.SeedOffset) * 29) * C.BaseFreq;
    return fBm3D(sx, sy, sz, C.Octaves, C.Lacunarity, C.Gain); // -1..1
}

bool FVoxelNoiseContext::IsCave(float x, float y, float zWorld, float terrainH) const
{
    if (!Profile) return false;

    const float depth = terrainH - zWorld;   // >0 sous la surface
    if (depth < 2.f) return false;

    // 0..1, 1 près de la surface
    const float surf = FMath::Clamp(1.f - depth / 24.f, 0.f, 1.f);

    // Masque 2D, un peu plus permissif près de la surface
    const float t2d = Sample01_2D(Profile->Cave2D, x, y);
    const float th2d = 0.68f - 0.10f * surf;
    const bool  mask = (t2d > th2d);

    // Tube 3D, rayon augmente avec la profondeur
    const float n3d = FMath::Abs(SampleSS_3D(Profile->Cave3D, x, y, zWorld));
    const float k = FMath::Clamp(depth / 96.f, 0.f, 1.f);   // remplace Saturate
    const float rad = FMath::Lerp(0.12f, 0.20f, k);
    const bool  tube = (n3d < rad);

    return mask && tube;
}

float FVoxelNoiseContext::Density3D(float x, float y, float zWorld,
    int32 BaseHeight, float HeightAmp, int32 /*WaterLevel*/) const
{
    check(Profile);
    const auto& TP = Profile->Terrain3D;

    const float Macro = MacroSurfaceZ(x, y, BaseHeight, HeightAmp);

    // Overhangs / arches via domain-warped 3D noise
    const FVector3f pw = DomainWarp3D(TP.WarpAmplitude, TP.WarpFrequency, WorldSeed, x, y, zWorld);
    const float Overhang =
        TP.OverhangAmplitude *
        FMath::PerlinNoise3D(FVector(pw.X * TP.OverhangFrequency,
            pw.Y * TP.OverhangFrequency,
            pw.Z * TP.OverhangFrequency));

    // Floating islands (gated so they don’t create a second sheet)
    const float iband = 1.f - FMath::Clamp(FMath::Abs(zWorld - TP.IslandBandCenterZ) / FMath::Max(1.f, TP.IslandBandHalfThickness), 0.f, 1.f);
    const float islandField =
        1.f - FMath::Abs(FMath::PerlinNoise3D(FVector(pw.X * TP.IslandFrequency + 11.3f,
            pw.Y * TP.IslandFrequency + 27.1f,
            pw.Z * TP.IslandFrequency - 5.7f)));
    const float Islands = iband * ((islandField - TP.IslandThreshold) * TP.IslandAmplitude);

    float D = Macro - zWorld;          // base heightfield
    D += Overhang;

    // Only union islands if they float above macro surface by a margin.
    if (zWorld >= Macro + 6.f)         // fixed 6-voxel margin, no new params needed
        D = FMath::Max(D, Islands);

    // Note: Cave carving is now handled separately in the generator
    // for better control and performance

    return D;
}

float FVoxelNoiseContext::MacroSurfaceZ(float x, float y, int32 BaseHeight, float HeightAmp) const
{
    const float H2D = HeightAbs(x, y, BaseHeight, HeightAmp);

    if (!Profile || !Profile->bUse3DTerrain) return H2D;

    const auto& TP = Profile->Terrain3D;

    // Thresholded mountains so we don’t lift everything.
    // r in [0,1], keep only the top band then sharpen.
    const float r = Ridged2D(x * TP.MountainFrequency + WorldSeed * 0.13f,
        y * TP.MountainFrequency + WorldSeed * 0.19f);
    const float band = FMath::Max(0.f, r - 0.55f);     // keep peaks only
    const float shaped = FMath::Pow(band, 1.5f);       // sharpen
    const float M = TP.MountainAmplitude * shaped;     // ≥0, sparse

    return H2D + M;
}

// ============================================================================
// BIOME-AWARE TERRAIN GENERATION
// ============================================================================

// Custom 2D noise sampler with configurable octave count
float FVoxelNoiseContext::Sample01_2D_Custom(float x, float y, int32 octaves) const
{
    if (octaves <= 0) return 0.5f;

    float sum = 0.f;
    float maxAmp = 0.f;
    float amp = 1.f;
    float freq = 1.f;

    const float lacunarity = 2.0f;
    const float gain = 0.5f;

    for (int32 i = 0; i < octaves; ++i)
    {
        sum += FMath::PerlinNoise2D(FVector2D(x * freq, y * freq)) * amp;
        maxAmp += amp;
        freq *= lacunarity;
        amp *= gain;
    }

    // Normalize to [0,1]
    return (sum / maxAmp) * 0.5f + 0.5f;
}

// Biome-specific height generation
float FVoxelNoiseContext::HeightAbs_Biome(float x, float y, int32 baseH, const FBiomeTerrainParams& Params) const
{
    // Use biome-specific frequency, octaves, and amplitude
    const float n01 = Sample01_2D_Custom(x * Params.HeightFrequency, y * Params.HeightFrequency, Params.HeightOctaves);
    return baseH + (n01 - 0.5f) * 2.0f * Params.HeightAmplitude;
}

// Biome-specific macro surface with mountains
float FVoxelNoiseContext::MacroSurfaceZ_Biome(float x, float y, int32 BaseHeight, const FBiomeTerrainParams& Params) const
{
    // Base terrain height using biome params
    const float H2D = HeightAbs_Biome(x, y, BaseHeight, Params);

    // Skip mountain calculation if disabled for this biome
    if (!Profile || !Profile->bUse3DTerrain || Params.MountainAmplitude <= 0.f)
        return H2D;

    // Generate mountains using biome-specific parameters
    const float r = Ridged2D(x * Params.MountainFrequency + WorldSeed * 0.13f,
                             y * Params.MountainFrequency + WorldSeed * 0.19f);

    // Apply biome-specific threshold and sharpness
    const float band = FMath::Max(0.f, r - Params.MountainThreshold);
    const float shaped = FMath::Pow(band, Params.MountainSharpness);
    const float M = Params.MountainAmplitude * shaped;

    return H2D + M;
}

// Biome-specific cave generation using per-biome cave parameters
bool FVoxelNoiseContext::IsCave_Biome(float x, float y, float zWorld, float terrainH, const FBiomeTerrainParams& Params) const
{
    // Skip caves entirely if density is 0 in this biome
    if (Params.CaveDensity <= 0.f) return false;

    const float depth = terrainH - zWorld;
    if (depth < 2.f) return false;  // No caves too close to surface

    // Surface modifier: fewer caves near surface
    const float surf = FMath::Clamp(1.f - depth / 24.f, 0.f, 1.f);

    // === STAGE 1: 2D cave mask (horizontal tunnels) using biome-specific parameters ===
    // Custom 2D noise sampling with biome cave parameters
    const float sx = x * Params.CaveFrequency2D;
    const float sy = y * Params.CaveFrequency2D;
    float sum = 0.f, maxAmp = 0.f, amp = 1.f, freq = 1.f;

    for (int32 i = 0; i < Params.CaveOctaves2D; ++i)
    {
        sum += FMath::PerlinNoise2D(FVector2D(sx * freq, sy * freq)) * amp;
        maxAmp += amp;
        freq *= Params.CaveLacunarity2D;
        amp *= Params.CaveGain2D;
    }

    const float t2d = (sum / maxAmp) * 0.5f + 0.5f; // Normalize to [0,1]
    const float th2d = 0.68f - 0.10f * surf;

    // Adjust threshold by cave density (higher density = easier to pass threshold)
    const bool mask = (t2d > th2d / Params.CaveDensity);
    if (!mask) return false;

    // === STAGE 2: 3D cave tube (vertical variation) using biome-specific parameters ===
    // Custom 3D noise sampling with biome cave parameters
    const float sx3d = x * Params.CaveFrequency3D;
    const float sy3d = y * Params.CaveFrequency3D;
    const float sz3d = zWorld * Params.CaveFrequency3D;
    float sum3d = 0.f, maxAmp3d = 0.f, amp3d = 1.f, freq3d = 1.f;

    for (int32 i = 0; i < Params.CaveOctaves3D; ++i)
    {
        sum3d += FMath::PerlinNoise3D(FVector(sx3d * freq3d, sy3d * freq3d, sz3d * freq3d)) * amp3d;
        maxAmp3d += amp3d;
        freq3d *= Params.CaveLacunarity2D; // Reuse 2D lacunarity for consistency
        amp3d *= Params.CaveGain2D;
    }

    const float n3d = FMath::Abs(sum3d / maxAmp3d);

    // Cave radius increases with depth
    const float k = FMath::Clamp(depth / 96.f, 0.f, 1.f);
    const float baseRad = FMath::Lerp(0.12f, 0.20f, k);

    // Scale radius by square root of density (more caves = larger radius)
    const float rad = baseRad * FMath::Sqrt(Params.CaveDensity);
    const bool tube = (n3d < rad);

    return tube;
}

// Helper to sample climate values
void FVoxelNoiseContext::SampleClimate(float x, float y, float& outTemp, float& outMoist) const
{
    if (Profile)
    {
        outTemp = Sample01_2D(Profile->Temperature, x, y);
        outMoist = Sample01_2D(Profile->Moisture, x, y);
    }
    else
    {
        outTemp = 0.5f;
        outMoist = 0.5f;
    }
}