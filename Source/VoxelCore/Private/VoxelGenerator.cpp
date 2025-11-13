// VoxelGenerator.cpp — unified 3D density pipeline (macro-surface + narrow-band density)
// Requires FVoxelNoiseContext::MacroSurfaceZ, ::Density3D_FromMacro, ::IsCave.

#include "VoxelGenerator.h"
#include "VoxelSettings.h"
#include "VoxelStats.h"
#include "VoxelMaterialSet.h"
#include "VoxelBlockTable.h"
#include "VoxelBiome.h"
#include "VoxelNoise.h"

#define LOCTEXT_NAMESPACE "VoxelGenerator"

//------------------------------ utils

template <typename T>
static T* LoadSoft(const TSoftObjectPtr<T>& Ptr, const TCHAR* Name)
{
    if (Ptr.IsNull())
    {
        UE_LOG(LogTemp, Warning, TEXT("Voxel: missing asset reference: %s"), Name);
        return nullptr;
    }
    T* R = Ptr.LoadSynchronous();
    if (!R)
    {
        UE_LOG(LogTemp, Error, TEXT("Voxel: failed to load asset: %s"), Name);
    }
    return R;
}

// Legacy height for non-3D worlds
static FORCEINLINE float GetTerrainHeight_Legacy(const FChunkGenParams& P, int32 WX, int32 WY)
{
    if (P.NoiseProfile)
    {
        FVoxelNoiseContext N(P.Seed, P.NoiseProfile);
        return N.HeightAbs((float)WX, (float)WY, P.BaseHeight, P.NoiseAmplitude);
    }
    // Fallback: simple Perlin
    const float Frequency = 1.0f / 64.0f;
    const float n = FMath::PerlinNoise2D(FVector2D(WX * Frequency, WY * Frequency)); // [-1,1]
    return P.BaseHeight + (n * 0.5f + 0.5f) * P.NoiseAmplitude;
}

static FORCEINLINE void SampleClimate(const FChunkGenParams& P, float WX, float WY, float& OutT, float& OutM)
{
    if (P.NoiseProfile)
    {
        FVoxelNoiseContext N(P.Seed, P.NoiseProfile);
        OutT = N.Temp(WX, WY);
        OutM = N.Moist(WX, WY);
    }
    else
    {
        const float fT = 1.0f / 256.0f;
        const float fM = 1.0f / 192.0f;
        OutT = FMath::Clamp(FMath::PerlinNoise2D(FVector2D(WX * fT, WY * fT)) * 0.5f + 0.5f, 0.f, 1.f);
        OutM = FMath::Clamp(FMath::PerlinNoise2D(FVector2D(WX * fM, WY * fM)) * 0.5f + 0.5f, 0.f, 1.f);
    }
}

// Pick the correct block type based on depth below surface
static FORCEINLINE EVoxelBlockID PickSubsurfaceBlock(const UVoxelBiomeDef* Biome, int32 DepthBelowSurface)
{
    if (!Biome || Biome->Subsurface.Num() == 0)
        return EVoxelBlockID::Stone;

    int32 AccumulatedDepth = 0;
    for (const FBiomeLayer& Layer : Biome->Subsurface)
    {
        AccumulatedDepth += FMath::Max(0, Layer.Thickness);
        if (DepthBelowSurface <= AccumulatedDepth)
            return Layer.Block;
    }

    // Beyond all layers - return the last layer's block
    return Biome->Subsurface.Last().Block;
}

//------------------------------ params

FChunkGenParams UVoxelGenerator::MakeParamsFromSettings(const UVoxelSettings* S)
{
    FChunkGenParams P;
    P.SizeX = S ? S->ChunkSizeX : 16;
    P.SizeY = S ? S->ChunkSizeY : 16;
    P.SizeZ = S ? S->ChunkSizeZ : 64;

    P.Seed = S ? S->Seed : 1337;
    P.NoiseScale = S ? FMath::Max(1.f, S->NoiseScale) : 32.f;
    P.NoiseAmplitude = S ? S->NoiseAmplitude : 10.f;
    P.BaseHeight = S ? S->BaseHeight : 20;
    P.WaterLevel = S ? S->WaterLevel : 18;

    P.MaterialSet = LoadSoft(S ? S->MaterialSet : TSoftObjectPtr<UVoxelMaterialSet>{}, TEXT("MaterialSet"));
    P.BlockTable = LoadSoft(S ? S->BlockTable : TSoftObjectPtr<UVoxelBlockTable>{}, TEXT("BlockTable"));
    P.BiomeTable = LoadSoft(S ? S->BiomeTable : TSoftObjectPtr<UVoxelBiomeTable>{}, TEXT("BiomeTable"));
    P.NoiseProfile = LoadSoft(S ? S->NoiseProfile : TSoftObjectPtr<UVoxelNoiseProfile>{}, TEXT("NoiseProfile"));
    return P;
}

int32 UVoxelGenerator::SampleHeightWorld(float WX, float WY)
{
    // Simple, profile-agnostic macro estimate for external callers.
    const float baseH = 32.0f + FMath::PerlinNoise2D(FVector2D(WX / 64.f, WY / 64.f)) * 20.0f;
    return FMath::FloorToInt(baseH);
}

//------------------------------ generation

void UVoxelGenerator::GenerateChunkLOD_Categories(
    const FVoxelCoord& Coord, const FChunkGenParams& P, int32 LODScaleXY, FCategoryBitset& OutCats)
{
    SCOPE_CYCLE_COUNTER(STAT_VoxelGeneration);

    const int32 SX = (P.SizeX + LODScaleXY - 1) / LODScaleXY;
    const int32 SY = (P.SizeY + LODScaleXY - 1) / LODScaleXY;
    const int32 SZ = P.SizeZ;

    OutCats.Init(SX + 2, SY + 2, SZ + 2);

    const int32 BaseWX = Coord.Cx * P.SizeX;
    const int32 BaseWY = Coord.Cy * P.SizeY;
    const int32 BaseWZ = Coord.Cz * P.SizeZ;

    const bool b3D = (P.NoiseProfile != nullptr) && P.NoiseProfile->bUse3DTerrain;
    if (!b3D)
    {
        // Legacy height-only path
        for (int32 z = -1; z <= SZ; ++z)
        {
            const int32 GlobalZInt = BaseWZ + z;
            for (int32 y = -1; y <= SY; ++y)
            {
                const int32 WY = BaseWY + y * LODScaleXY;
                for (int32 x = -1; x <= SX; ++x)
                {
                    const int32 WX = BaseWX + x * LODScaleXY;
                    const int32 hWorld = FMath::FloorToInt(GetTerrainHeight_Legacy(P, WX, WY));
                    uint8 Cat;
                    if (GlobalZInt > hWorld)  Cat = (GlobalZInt <= P.WaterLevel) ? 1 : 0;
                    else                      Cat = 2;
                    OutCats.Set(x + 1, y + 1, z + 1, Cat);
                }
            }
        }
        return;
    }

    // 3D density path with narrow-band evaluation
    FVoxelNoiseContext N(P.Seed, P.NoiseProfile);

    // BIOME-AWARE GENERATION: Cache biome parameters and macro surface per XY column
    const int32 MX = SX + 2, MY = SY + 2;
    TArray<float> Macro;
    TArray<FBiomeTerrainParams> BiomeParamsCache;
    Macro.SetNumUninitialized(MX * MY);
    BiomeParamsCache.SetNum(MX * MY);

    // Precompute biome params and macro surface with halo - cache-friendly single pass
    for (int32 y = -1; y <= SY; ++y)
    {
        const int32 WY = BaseWY + y * LODScaleXY;
        const int32 rowOffset = (y + 1) * MX;

        for (int32 x = -1; x <= SX; ++x)
        {
            const int32 WX = BaseWX + x * LODScaleXY;
            const int32 idx = rowOffset + (x + 1);

            // Sample climate and pick biome for this XY column
            float T, Mo;
            N.SampleClimate((float)WX, (float)WY, T, Mo);
            const UVoxelBiomeDef* Biome = P.BiomeTable.IsValid() ? P.BiomeTable->Pick(T, Mo) : nullptr;

            // Cache biome terrain parameters (or use defaults if no biome)
            BiomeParamsCache[idx] = Biome ? Biome->TerrainParams : FBiomeTerrainParams();

            // Compute macro surface using biome-specific parameters
            Macro[idx] = N.MacroSurfaceZ_Biome((float)WX, (float)WY, P.BaseHeight, BiomeParamsCache[idx]);
        }
    }

    // Process in XY-major order for better cache locality
    for (int32 y = -1; y <= SY; ++y)
    {
        const int32 WY = BaseWY + y * LODScaleXY;
        const int32 rowOffset = (y + 1) * MX;

        for (int32 x = -1; x <= SX; ++x)
        {
            const int32 WX = BaseWX + x * LODScaleXY;
            const int32 idx = rowOffset + (x + 1);
            const float M = Macro[idx];
            const FBiomeTerrainParams& BiomeParams = BiomeParamsCache[idx];

            // Calculate margin using biome-specific parameters
            const float Margin = BiomeParams.OverhangAmplitude + BiomeParams.IslandAmplitude + 3.f;

            // Process entire Z column at once
            for (int32 z = -1; z <= SZ; ++z)
            {
                const float Z = float(BaseWZ + z);
                const float D0 = M - Z; // cheap bound relative to macro surface

                uint8 Cat;

                // Far above surface - definitely air/water, no terrain to carve
                if (D0 < -Margin)
                {
                    Cat = (Z <= float(P.WaterLevel)) ? 1 : 0;
                }
                // Deep below surface - could be solid or cave
                else if (D0 > Margin)
                {
                    // CRITICAL FIX: Verify terrain is actually solid before carving caves.
                    // The macro surface is a 2D approximation and doesn't account for
                    // overhangs, floating islands, or other 3D features. We must evaluate
                    // the full 3D density to confirm solid terrain exists before carving.
                    const float D = N.Density3D_FromMacro_Biome((float)WX, (float)WY, Z, M, BiomeParams);
                    if (D > 0.f)
                    {
                        // Confirmed solid terrain - now check if cave should carve through it
                        // OPTIMIZATION: Skip cave checks if too deep (major perf gain)
                        const float DepthFromSurface = M - Z;
                        const bool bCheckCaves = (BiomeParams.CaveDensity > 0.f) &&
                                                 (DepthFromSurface <= float(P.MaxCaveDepth));

                        if (bCheckCaves && N.IsCave_Biome((float)WX, (float)WY, Z, M, BiomeParams))
                            Cat = 0; // Cave carved through verified solid terrain
                        else
                            Cat = 2; // Solid terrain (or too deep for caves)
                    }
                    else
                    {
                        // Actually empty space (e.g., under floating island or between overhangs)
                        Cat = (Z <= float(P.WaterLevel)) ? 1 : 0;
                    }
                }
                // Narrow band around surface - needs full density check
                else
                {
                    const float D = N.Density3D_FromMacro_Biome((float)WX, (float)WY, Z, M, BiomeParams);
                    if (D > 0.f)
                    {
                        // Solid in narrow band - check for caves using biome params
                        // OPTIMIZATION: Narrow band cave checks (within MaxCaveDepth)
                        const float DepthFromSurface = M - Z;
                        const bool bCheckCaves = (BiomeParams.CaveDensity > 0.f) &&
                                                 (DepthFromSurface <= float(P.MaxCaveDepth));

                        if (bCheckCaves && N.IsCave_Biome((float)WX, (float)WY, Z, M, BiomeParams))
                            Cat = 0; // Cave carved through narrow band
                        else
                            Cat = 2; // Solid
                    }
                    else
                        Cat = (Z <= float(P.WaterLevel)) ? 1 : 0; // Water or air
                }
                OutCats.Set(x + 1, y + 1, z + 1, Cat);
            }
        }
    }
}

void UVoxelGenerator::GenerateChunkLOD(
    const FVoxelCoord& Coord, const FChunkGenParams& P, int32 LODScaleXY,
    TArray<EVoxelBlockID>& OutData, FIntVector& OutSize)
{
    SCOPE_CYCLE_COUNTER(STAT_VoxelGeneration);

    const int32 SX = (P.SizeX + LODScaleXY - 1) / LODScaleXY;
    const int32 SY = (P.SizeY + LODScaleXY - 1) / LODScaleXY;
    const int32 SZ = P.SizeZ;

    OutSize = FIntVector(SX, SY, SZ);
    OutData.SetNumUninitialized(SX * SY * SZ);

    const int32 BaseWX = Coord.Cx * P.SizeX;
    const int32 BaseWY = Coord.Cy * P.SizeY;
    const int32 BaseWZ = Coord.Cz * P.SizeZ;

    const bool b3D = (P.NoiseProfile != nullptr) && P.NoiseProfile->bUse3DTerrain;

    // Biome per XY
    TArray<const UVoxelBiomeDef*> BiomeAtXY;
    BiomeAtXY.SetNumUninitialized(SX * SY);
    for (int32 y = 0; y < SY; ++y)
    {
        for (int32 x = 0; x < SX; ++x)
        {
            const float wx = float(BaseWX + x * LODScaleXY);
            const float wy = float(BaseWY + y * LODScaleXY);
            float T, M; SampleClimate(P, wx, wy, T, M);
            BiomeAtXY[y * SX + x] = P.BiomeTable.IsValid() ? P.BiomeTable->Pick(T, M) : nullptr;
        }
    }

    if (!b3D)
    {
        // Legacy block fill
        int32 Index = 0;
        for (int32 z = 0; z < SZ; ++z)
        {
            const int32 GlobalZInt = BaseWZ + z;
            for (int32 y = 0; y < SY; ++y)
            {
                const int32 WY = BaseWY + y * LODScaleXY;
                for (int32 x = 0; x < SX; ++x, ++Index)
                {
                    const int32 WX = BaseWX + x * LODScaleXY;
                    const int32 hWorld = FMath::FloorToInt(GetTerrainHeight_Legacy(P, WX, WY));
                    if (GlobalZInt > hWorld)
                    {
                        OutData[Index] = (GlobalZInt <= P.WaterLevel) ? EVoxelBlockID::Water : EVoxelBlockID::Air;
                        continue;
                    }

                    // Surface if empty above
                    const bool bSurface = (GlobalZInt + 1 > hWorld);
                    const UVoxelBiomeDef* B = BiomeAtXY[y * SX + x];
                    if (bSurface)
                    {
                        OutData[Index] = (B && B->Surface != EVoxelBlockID::Air) ? B->Surface : EVoxelBlockID::Grass;
                    }
                    else
                    {
                        // Calculate depth below surface for subsurface layer selection
                        const int32 Depth = hWorld - GlobalZInt;
                        OutData[Index] = PickSubsurfaceBlock(B, Depth);
                    }
                }
            }
        }
        return;
    }

    // 3D density path with narrow-band evaluation
    FVoxelNoiseContext N(P.Seed, P.NoiseProfile);

    // Cache biome parameters and macro surface (no halo needed for block fill)
    TArray<float> Macro;
    TArray<FBiomeTerrainParams> BiomeParamsCache;
    Macro.SetNumUninitialized(SX * SY);
    BiomeParamsCache.SetNum(SX * SY);

    for (int32 y = 0; y < SY; ++y)
    {
        const int32 WY = BaseWY + y * LODScaleXY;
        const int32 rowOffset = y * SX;
        for (int32 x = 0; x < SX; ++x)
        {
            const int32 WX = BaseWX + x * LODScaleXY;
            const int32 idx = rowOffset + x;

            // Get biome for this XY column (already cached in BiomeAtXY)
            const UVoxelBiomeDef* B = BiomeAtXY[idx];
            BiomeParamsCache[idx] = B ? B->TerrainParams : FBiomeTerrainParams();

            // Compute macro surface using biome-specific parameters
            Macro[idx] = N.MacroSurfaceZ_Biome((float)WX, (float)WY, P.BaseHeight, BiomeParamsCache[idx]);
        }
    }

    // Process in Z-major order for better cache locality
    int32 Index = 0;
    for (int32 z = 0; z < SZ; ++z)
    {
        const float Z = float(BaseWZ + z);
        const float ZUp = Z + 1.f;

        for (int32 y = 0; y < SY; ++y)
        {
            const int32 WY = BaseWY + y * LODScaleXY;
            const int32 rowOffset = y * SX;

            for (int32 x = 0; x < SX; ++x, ++Index)
            {
                const int32 WX = BaseWX + x * LODScaleXY;
                const int32 xyIdx = rowOffset + x;
                const float M = Macro[xyIdx];
                const FBiomeTerrainParams& BiomeParams = BiomeParamsCache[xyIdx];
                const float Margin = BiomeParams.OverhangAmplitude + BiomeParams.IslandAmplitude + 3.f;
                const float D0 = M - Z;

                // Far above surface - definitely air/water, no terrain to carve
                if (D0 < -Margin)
                {
                    OutData[Index] = (Z <= float(P.WaterLevel)) ? EVoxelBlockID::Water : EVoxelBlockID::Air;
                    continue;
                }

                // Deep solid - could be solid or cave
                if (D0 > Margin)
                {
                    const float D = N.Density3D_FromMacro_Biome((float)WX, (float)WY, Z, M, BiomeParams);
                    if (D > 0.f)
                    {
                        // OPTIMIZATION: Skip cave checks if too deep below surface (major perf gain)
                        const float DepthFromSurface = M - Z;
                        const bool bCheckCaves = (BiomeParams.CaveDensity > 0.f) &&
                                                 (DepthFromSurface <= float(P.MaxCaveDepth));

                        // Confirmed solid terrain - check for caves
                        if (bCheckCaves && N.IsCave_Biome((float)WX, (float)WY, Z, M, BiomeParams))
                        {
                            OutData[Index] = EVoxelBlockID::Air;
                        }
                        else
                        {
                            // Deep solid (or too deep for caves) - use deepest subsurface layer (typically stone)
                            const UVoxelBiomeDef* B = BiomeAtXY[xyIdx];
                            OutData[Index] = PickSubsurfaceBlock(B, 999);
                        }
                    }
                    else
                    {
                        OutData[Index] = (Z <= float(P.WaterLevel)) ? EVoxelBlockID::Water : EVoxelBlockID::Air;
                    }
                    continue;
                }

                // Narrow band: evaluate full density
                const float D = N.Density3D_FromMacro_Biome((float)WX, (float)WY, Z, M, BiomeParams);
                if (D <= 0.f)
                {
                    OutData[Index] = (Z <= float(P.WaterLevel)) ? EVoxelBlockID::Water : EVoxelBlockID::Air;
                    continue;
                }

                // OPTIMIZATION: Narrow band cave checks (within MaxCaveDepth)
                const float DepthFromSurface = M - Z;
                const bool bCheckCaves = (BiomeParams.CaveDensity > 0.f) &&
                                         (DepthFromSurface <= float(P.MaxCaveDepth));

                // Solid in narrow band - check for caves
                if (bCheckCaves && N.IsCave_Biome((float)WX, (float)WY, Z, M, BiomeParams))
                {
                    OutData[Index] = EVoxelBlockID::Air;
                    continue;
                }

                // Check if this is a surface
                const float DUp = N.Density3D_FromMacro_Biome((float)WX, (float)WY, ZUp, M, BiomeParams);
                const bool bSurface = (DUp <= 0.f);

                const UVoxelBiomeDef* B = BiomeAtXY[rowOffset + x];
                if (bSurface)
                {
                    OutData[Index] = (B && B->Surface != EVoxelBlockID::Air) ? B->Surface : EVoxelBlockID::Grass;
                }
                else
                {
                    // Not a surface - use deepest subsurface layer (typically stone)
                    OutData[Index] = PickSubsurfaceBlock(B, 999);
                }
            }
        }
    }
}

void UVoxelGenerator::GenerateHeightmap(
    const FVoxelCoord& Coord, const FChunkGenParams& P, int32 LODScaleXY,
    TArray<int32>& OutHeights, FIntPoint& OutSizeXY)
{
    SCOPE_CYCLE_COUNTER(STAT_VoxelGeneration);

    const int32 CellsX = (P.SizeX + LODScaleXY - 1) / LODScaleXY;
    const int32 CellsY = (P.SizeY + LODScaleXY - 1) / LODScaleXY;

    const int32 SamplesX = CellsX + 1;
    const int32 SamplesY = CellsY + 1;

    OutSizeXY = FIntPoint(SamplesX, SamplesY);
    OutHeights.SetNumUninitialized(SamplesX * SamplesY);

    const int32 BaseWX = Coord.Cx * P.SizeX;
    const int32 BaseWY = Coord.Cy * P.SizeY;
    const int32 BaseWZ = Coord.Cz * P.SizeZ;

    const bool b3D = (P.NoiseProfile != nullptr) && P.NoiseProfile->bUse3DTerrain;

    int32 Index = 0;
    if (!b3D)
    {
        for (int32 y = 0; y < SamplesY; ++y)
        {
            const int32 WY = BaseWY + ((y == SamplesY - 1) ? P.SizeY : FMath::Min(y * LODScaleXY, P.SizeY));
            for (int32 x = 0; x < SamplesX; ++x, ++Index)
            {
                const int32 WX = BaseWX + ((x == SamplesX - 1) ? P.SizeX : FMath::Min(x * LODScaleXY, P.SizeX));
                const int32 hWorld = FMath::FloorToInt(GetTerrainHeight_Legacy(P, WX, WY));
                OutHeights[Index] = FMath::Clamp(hWorld - BaseWZ, 0, P.SizeZ - 1);
            }
        }
        return;
    }

    FVoxelNoiseContext N(P.Seed, P.NoiseProfile);

    for (int32 y = 0; y < SamplesY; ++y)
    {
        const int32 WY = BaseWY + ((y == SamplesY - 1) ? P.SizeY : FMath::Min(y * LODScaleXY, P.SizeY));
        for (int32 x = 0; x < SamplesX; ++x, ++Index)
        {
            const int32 WX = BaseWX + ((x == SamplesX - 1) ? P.SizeX : FMath::Min(x * LODScaleXY, P.SizeX));

            // Get biome for this XY position
            float T, M;
            SampleClimate(P, (float)WX, (float)WY, T, M);
            const UVoxelBiomeDef* Biome = P.BiomeTable.IsValid() ? P.BiomeTable->Pick(T, M) : nullptr;
            const FBiomeTerrainParams BiomeParams = Biome ? Biome->TerrainParams : FBiomeTerrainParams();

            // Use biome-aware macro surface
            const float Macro = N.MacroSurfaceZ_Biome((float)WX, (float)WY, P.BaseHeight, BiomeParams);

            // Start near macro and scan downward a few voxels for the highest solid
            int32 zStart = FMath::Clamp(FMath::FloorToInt(Macro) - BaseWZ + 8, 0, P.SizeZ - 1);
            int32 bestLocalZ = 0;
            for (int32 z = zStart; z >= 0; --z)
            {
                const float Z = float(BaseWZ + z);
                const float D = N.Density3D_FromMacro_Biome((float)WX, (float)WY, Z, Macro, BiomeParams);
                if (D > 0.f) { bestLocalZ = z; break; }
            }
            OutHeights[Index] = bestLocalZ;
        }
    }
}

void UVoxelGenerator::GenerateBiomeGrid2D(
    const FVoxelCoord& Coord, const FChunkGenParams& P, int32 LODScaleXY, FBiomeGrid2D& OutGrid)
{
    SCOPE_CYCLE_COUNTER(STAT_VoxelGeneration);

    // MEMORY OPTIMIZATION NOTE: This grid is always allocated even if BiomeTable is empty
    // or contains only a single biome. Future optimization: Check if P.BiomeTable->Biomes.Num() > 1
    // before allocating, and use a nullptr check in meshing to skip biome logic entirely.
    // Current cost: ~10KB per chunk (18x18 padded grid * (2 bytes + 8 bytes pointer)).

    // LOD-reduced logical cells this chunk covers
    const int32 SX = (P.SizeX + LODScaleXY - 1) / LODScaleXY;
    const int32 SY = (P.SizeY + LODScaleXY - 1) / LODScaleXY;

    // Build a 1-voxel padded grid because meshing samples a halo
    const int32 PaddedSX = SX + 2;
    const int32 PaddedSY = SY + 2;
    OutGrid.Init(PaddedSX, PaddedSY);

    const int32 BaseWX = Coord.Cx * P.SizeX;
    const int32 BaseWY = Coord.Cy * P.SizeY;
    const int32 BaseWZ = Coord.Cz * P.SizeZ;

    const bool bUse3D = (P.NoiseProfile && P.NoiseProfile->bUse3DTerrain);
    FVoxelNoiseContext N(P.Seed, P.NoiseProfile);

    // For each haloed XY cell
    for (int32 y = 0; y < PaddedSY; ++y)
    {
        // Clamp from haloed coords [0..SY+1] to the interior [0..SY-1]
        const int32 vy = FMath::Clamp(y - 1, 0, SY - 1);
        const int32 WY = BaseWY + vy * LODScaleXY;

        for (int32 x = 0; x < PaddedSX; ++x)
        {
            const int32 vx = FMath::Clamp(x - 1, 0, SX - 1);
            const int32 WX = BaseWX + vx * LODScaleXY;

            int16 SurfaceZWorld = 0;

            if (!bUse3D)
            {
                // Legacy 2D surface - use biome-aware height calculation
                float Temp, Moist;
                N.SampleClimate((float)WX, (float)WY, Temp, Moist);
                const UVoxelBiomeDef* Biome = P.BiomeTable.IsValid() ? P.BiomeTable->Pick(Temp, Moist) : nullptr;
                const FBiomeTerrainParams BiomeParams = Biome ? Biome->TerrainParams : FBiomeTerrainParams();

                const int32 H = FMath::FloorToInt(N.HeightAbs_Biome((float)WX, (float)WY, P.BaseHeight, BiomeParams));
                SurfaceZWorld = (int16)FMath::Clamp(H, INT16_MIN, INT16_MAX);

                // Store surface and biome (already sampled)
                OutGrid.SurfaceZWorld[x + y * PaddedSX] = SurfaceZWorld;
                OutGrid.BiomeAtXY[x + y * PaddedSX] = Biome;
                continue; // Skip the biome sampling below
            }
            else
            {
                // Pick biome from climate first (needed for macro surface calculation)
                float Temp, Moist;
                N.SampleClimate((float)WX, (float)WY, Temp, Moist);
                const UVoxelBiomeDef* Biome = P.BiomeTable.IsValid() ? P.BiomeTable->Pick(Temp, Moist) : nullptr;
                const FBiomeTerrainParams BiomeParams = Biome ? Biome->TerrainParams : FBiomeTerrainParams();

                // 3D: find the highest solid voxel in a narrow band around the macro surface.
                // Search is local to this chunk's vertical span to keep costs bounded.
                // Use biome-aware macro surface
                const float Macro = N.MacroSurfaceZ_Biome((float)WX, (float)WY, P.BaseHeight, BiomeParams);

                // Scan a small window around Macro in local-Z of this chunk.
                // Start slightly above macro to catch overhangs, scan downward.
                const int32 zStartLocal = FMath::Clamp(FMath::FloorToInt(Macro) - BaseWZ + 8, 0, P.SizeZ - 1);
                const int32 zEndLocal = FMath::Clamp(FMath::FloorToInt(Macro) - BaseWZ - 16, 0, P.SizeZ - 1);

                int32 bestLocalZ = zEndLocal; // default if none found inside window
                for (int32 z = zStartLocal; z >= zEndLocal; --z)
                {
                    const float Zworld = float(BaseWZ + z);
                    const float D = N.Density3D_FromMacro_Biome((float)WX, (float)WY, Zworld, Macro, BiomeParams);
                    if (D > 0.f) { bestLocalZ = z; break; } // first solid from above
                }

                SurfaceZWorld = (int16)(BaseWZ + bestLocalZ);

                // Store biome (already sampled above)
                OutGrid.SurfaceZWorld[x + y * PaddedSX] = SurfaceZWorld;
                OutGrid.BiomeAtXY[x + y * PaddedSX] = Biome;
            }
        }
    }
}


#undef LOCTEXT_NAMESPACE
