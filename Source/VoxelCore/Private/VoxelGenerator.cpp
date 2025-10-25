#include "VoxelGenerator.h"
#include "VoxelSettings.h"

FChunkGenParams UVoxelGenerator::MakeParamsFromSettings(const UVoxelSettings* S)
{
    FChunkGenParams P;
    P.SizeX = S->ChunkSizeX;
    P.SizeY = S->ChunkSizeY;
    P.SizeZ = S->ChunkSizeZ;

    P.Seed = S->Seed;
    P.NoiseScale = FMath::Max(1.f, S->NoiseScale);
    P.NoiseAmplitude = S->NoiseAmplitude;
    P.BaseHeight = FMath::Clamp(S->BaseHeight, 0, S->ChunkSizeZ - 1);
    P.WaterLevel = FMath::Clamp(S->WaterLevel, 0, S->ChunkSizeZ - 1);
    return P;
}

static FORCEINLINE int32 HeightAt(const FChunkGenParams& P, int32 WX, int32 WY)
{
    const float nx = (WX + P.Seed * 13) / P.NoiseScale;
    const float ny = (WY + P.Seed * 17) / P.NoiseScale;
    const float n = FMath::PerlinNoise2D(FVector2D(nx, ny));
    int32 Height = P.BaseHeight + FMath::FloorToInt(n * P.NoiseAmplitude);
    return FMath::Clamp(Height, 0, P.SizeZ - 1);
}

void UVoxelGenerator::GenerateChunkLOD(
    const FVoxelCoord& Coord,
    const FChunkGenParams& P,
    int32 LODScaleXY,
    TArray<EVoxelBlockID>& OutData,
    FIntVector& OutSize)
{
    const int32 SX = (P.SizeX + LODScaleXY - 1) / LODScaleXY;
    const int32 SY = (P.SizeY + LODScaleXY - 1) / LODScaleXY;
    const int32 SZ = P.SizeZ;

    OutSize = FIntVector(SX, SY, SZ);
    OutData.SetNumUninitialized(SX * SY * SZ);

    const int32 BaseWX = Coord.Cx * P.SizeX;
    const int32 BaseWY = Coord.Cy * P.SizeY;

    int32 Index = 0;
    for (int32 z = 0; z < SZ; ++z)
    {
        for (int32 y = 0; y < SY; ++y)
        {
            const int32 WY = BaseWY + y * LODScaleXY;
            for (int32 x = 0; x < SX; ++x, ++Index)
            {
                const int32 WX = BaseWX + x * LODScaleXY;
                const int32 Height = HeightAt(P, WX, WY);

                EVoxelBlockID Id;
                if (z < Height - 3)      Id = EVoxelBlockID::Stone;
                else if (z < Height)     Id = EVoxelBlockID::Dirt;
                else if (z == Height)    Id = EVoxelBlockID::Grass;
                else if (z <= P.WaterLevel) Id = EVoxelBlockID::Water;
                else                     Id = EVoxelBlockID::Air;

                OutData[Index] = Id;
            }
        }
    }

    // Bedrock
    for (int32 y = 0; y < SY; ++y)
        for (int32 x = 0; x < SX; ++x)
            OutData[Idx(x, y, 0, SX, SY)] = EVoxelBlockID::Bedrock;
}

void UVoxelGenerator::GenerateHeightmap(
    const FVoxelCoord& Coord,
    const FChunkGenParams& P,
    int32 LODScaleXY,
    TArray<int32>& OutHeights,
    FIntPoint& OutSizeXY)
{
    // Number of LOD cells per chunk edge
    const int32 CellsX = (P.SizeX + LODScaleXY - 1) / LODScaleXY;
    const int32 CellsY = (P.SizeY + LODScaleXY - 1) / LODScaleXY;

    // We need (Cells+1) samples so the last quad reaches the chunk edge exactly
    const int32 SamplesX = CellsX + 1;
    const int32 SamplesY = CellsY + 1;

    OutSizeXY = FIntPoint(SamplesX, SamplesY);
    OutHeights.SetNumUninitialized(SamplesX * SamplesY);

    const int32 BaseWX = Coord.Cx * P.SizeX;
    const int32 BaseWY = Coord.Cy * P.SizeY;

    int32 Index = 0;
    for (int32 y = 0; y < SamplesY; ++y)
    {
        const int32 LocalY = (y == SamplesY - 1) ? P.SizeY : FMath::Min(y * LODScaleXY, P.SizeY);
        const int32 WY = BaseWY + LocalY;

        for (int32 x = 0; x < SamplesX; ++x, ++Index)
        {
            const int32 LocalX = (x == SamplesX - 1) ? P.SizeX : FMath::Min(x * LODScaleXY, P.SizeX);
            const int32 WX = BaseWX + LocalX;

            OutHeights[Index] = HeightAt(P, WX, WY);
        }
    }
}
int32 UVoxelGenerator::SampleHeightWorld(float WX, float WY)
{
    // Very simple example — replace with your noise stack:
    const float Frequency = 1.0f / 64.0f;
    const float Amp = 20.0f;
    const float n = FMath::PerlinNoise2D(FVector2D(WX * Frequency, WY * Frequency));
    const float h = 32.0f + n * Amp; // base plane 32 + noise
    return FMath::Clamp(FMath::FloorToInt(h), 0, 1023); // clamp to your world Z range
}