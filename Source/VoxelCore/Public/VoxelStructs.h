#pragma once

#include "CoreMinimal.h"
#include "VoxelStructs.generated.h"

USTRUCT(BlueprintType)
struct FVoxelCoord
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite) int32 Cx;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) int32 Cy;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) int32 Cz;

    FVoxelCoord() : Cx(0), Cy(0), Cz(0) {}
    FVoxelCoord(int32 InX, int32 InY, int32 InZ) : Cx(InX), Cy(InY), Cz(InZ) {}

    bool operator==(const FVoxelCoord& Other) const
    {
        return Cx == Other.Cx && Cy == Other.Cy && Cz == Other.Cz;
    }
};
FORCEINLINE uint32 GetTypeHash(const FVoxelCoord& Coord)
{
    return HashCombine(HashCombine(::GetTypeHash(Coord.Cx), ::GetTypeHash(Coord.Cy)), ::GetTypeHash(Coord.Cz));
}

// Up to 256 blocks
UENUM(BlueprintType)
enum class EVoxelBlockID : uint8
{
    Air = 0,
    Stone = 1,
    Dirt = 2,
    Grass = 3,
    Sand = 4,
    Snow = 5,
    Bedrock = 6,
    Water = 7,
    // 8..255 available
};

FORCEINLINE bool VoxelIsAir(EVoxelBlockID Id) { return Id == EVoxelBlockID::Air; }
FORCEINLINE bool VoxelIsSolid(EVoxelBlockID Id) { return Id != EVoxelBlockID::Air; }

// Determine a coarse category for a block.  Category 0 means the voxel is
// completely empty/transparent (air) and will not generate any faces.  Category
// 1 indicates a semi‑solid block (such as water, leaves or other non‑colliding
// foliage) which is rendered but does not occlude other geometry; faces are
// emitted between semi‑solid and any different category.  Any other value
// returns 2, marking the block as fully solid and opaque.  Additional block
// types can be mapped to category 1 here to customise their behaviour.
FORCEINLINE uint8 VoxelBlockCategory(EVoxelBlockID Id)
{
    switch (Id)
    {
    case EVoxelBlockID::Air:
        return 0; // empty
    case EVoxelBlockID::Water:
        return 1; // semi‑solid (e.g. liquids, leaves)
    default:
        return 2; // fully solid
    }
}

UENUM()
enum class EVoxelLODLevel : uint8
{
    LOD0 = 0, // full voxels
    LOD1 = 1, // coarsened voxels (XY)
    LOD2 = 2  // heightfield impostor
};

// A compact representation of voxel categories for a chunk.  Each voxel
// category is encoded using two bits: 0 = air/empty, 1 = semi‑solid (e.g. liquids
// or foliage) and 2 = fully solid.  The bitfield is stored in a byte array
// with little‑endian ordering.  For example, voxel i's category occupies bits
// [2*i .. 2*i+1] in Data.  This structure dramatically reduces the memory
// footprint compared to storing a full byte per voxel while still allowing
// random access to voxel categories.  Categories beyond 2 are currently
// unsupported and will be clamped.
struct FCategoryBitset
{
    int32 SizeX = 0;
    int32 SizeY = 0;
    int32 SizeZ = 0;
    TArray<uint8> Data;

    /**
     * Initialise the bitset with the given dimensions.  Existing data is
     * discarded.  All categories are initialised to zero (air).
     */
    void Init(int32 InX, int32 InY, int32 InZ)
    {
        SizeX = InX;
        SizeY = InY;
        SizeZ = InZ;
        const int64 TotalVoxels = static_cast<int64>(SizeX) * SizeY * SizeZ;
        // two bits per voxel
        const int64 NumBits = TotalVoxels * 2;
        const int64 NumBytes = (NumBits + 7) / 8;
        Data.SetNumZeroed(static_cast<int32>(NumBytes));
    }

    /**
     * Compute the linear index for voxel (x,y,z) within the chunk.  No bounds
     * checks are performed; callers must ensure x, y and z are within [0..SizeX-1],
     * [0..SizeY-1] and [0..SizeZ-1] respectively.
     */
    FORCEINLINE int64 LinearIndex(int32 X, int32 Y, int32 Z) const
    {
        return static_cast<int64>(X) + static_cast<int64>(Y) * SizeX + static_cast<int64>(Z) * SizeX * SizeY;
    }

    /**
     * Set the category of voxel (x,y,z).  Cat must be 0, 1 or 2; higher values
     * will be clamped.  No bounds checks are performed.
     */
    FORCEINLINE void Set(int32 X, int32 Y, int32 Z, uint8 Cat)
    {
        Cat &= 0x3; // clamp to 2 bits
        const int64 idx = LinearIndex(X, Y, Z);
        const int64 bitIndex = idx * 2;
        const int64 byteIndex = bitIndex >> 3;
        const int32 bitOffset = static_cast<int32>(bitIndex & 7);
        // We may need to set bits across a byte boundary when bitOffset == 7
        if (bitOffset <= 6)
        {
            // Clear the two bits
            uint8 mask = static_cast<uint8>(0x3u << bitOffset);
            Data[byteIndex] = (Data[byteIndex] & ~mask) | (Cat << bitOffset);
        }
        else
        {
            // bitOffset == 7: first bit goes to position 7 of current byte,
            // second bit goes to position 0 of next byte
            // Clear current bit 7
            Data[byteIndex] &= static_cast<uint8>(~0x80u);
            // Set bit 7 to LSB of Cat
            Data[byteIndex] |= static_cast<uint8>((Cat & 0x1u) << 7);
            // Clear bit 0 of next byte and set to second bit of Cat
            Data[byteIndex + 1] &= static_cast<uint8>(~0x1u);
            Data[byteIndex + 1] |= static_cast<uint8>((Cat >> 1) & 0x1u);
        }
    }

    /**
     * Get the category of voxel (x,y,z).  Returns a value in the range [0..3].
     * No bounds checks are performed.
     */
    FORCEINLINE uint8 Get(int32 X, int32 Y, int32 Z) const
    {
        const int64 idx = LinearIndex(X, Y, Z);
        const int64 bitIndex = idx * 2;
        const int64 byteIndex = bitIndex >> 3;
        const int32 bitOffset = static_cast<int32>(bitIndex & 7);
        if (bitOffset <= 6)
        {
            uint8 val = (Data[byteIndex] >> bitOffset) & 0x3u;
            return val;
        }
        else
        {
            // bitOffset == 7: combine bit 7 of current and bit 0 of next
            uint8 lsb = (Data[byteIndex] >> 7) & 0x1u;
            uint8 msb = Data[byteIndex + 1] & 0x1u;
            return static_cast<uint8>(lsb | (msb << 1));
        }
    }

    /**
     * Check if all voxels in this bitset are empty (category 0 = Air).
     * This is an extremely fast check that can skip meshing for empty chunks.
     * Returns true if all bytes are zero (all voxels are air).
     */
    FORCEINLINE bool IsAllEmpty() const
    {
        // Fast path: check if all bytes are zero
        // Since category 0 (air) is represented as 00 bits, an all-air chunk will have all zero bytes
        for (int32 i = 0; i < Data.Num(); ++i)
        {
            if (Data[i] != 0)
            {
                return false; // Found non-zero byte, so there's at least one non-air voxel
            }
        }
        return true;
    }

    /**
     * Check if the core region (excluding 1-voxel padding) contains only air.
     * This is used to skip meshing for chunks where the actual chunk content is empty,
     * even if the padding halo contains neighbor data.
     *
     * @param PaddingSize The number of voxels of padding on each side (typically 1)
     * @return true if all voxels in the core region are air (category 0)
     */
    FORCEINLINE bool IsCoreEmpty(int32 PaddingSize = 1) const
    {
        // Calculate core region bounds (skip padding on all sides)
        const int32 CoreSizeX = SizeX - 2 * PaddingSize;
        const int32 CoreSizeY = SizeY - 2 * PaddingSize;
        const int32 CoreSizeZ = SizeZ - 2 * PaddingSize;

        // If core is invalid, return false
        if (CoreSizeX <= 0 || CoreSizeY <= 0 || CoreSizeZ <= 0)
        {
            return false;
        }

        // Check only the core region
        for (int32 z = PaddingSize; z < SizeZ - PaddingSize; ++z)
        {
            for (int32 y = PaddingSize; y < SizeY - PaddingSize; ++y)
            {
                for (int32 x = PaddingSize; x < SizeX - PaddingSize; ++x)
                {
                    if (Get(x, y, z) != 0)
                    {
                        return false; // Found non-air voxel in core
                    }
                }
            }
        }
        return true;
    }

    /**
     * Check if the core region contains ONLY air or transparent blocks that won't produce geometry.
     * Includes category 0 (air) and category 1 (water/semi-transparent) as "renderable-empty".
     * This catches more empty chunks than IsCoreEmpty() because water-only chunks produce no geometry
     * when fully surrounded by water, but still cost 0.5ms to mesh.
     *
     * @param PaddingSize The number of voxels of padding on each side (typically 1)
     * @return true if all voxels in the core region are air or water (categories 0 or 1)
     */
    FORCEINLINE bool IsCoreRenderableEmpty(int32 PaddingSize = 1) const
    {
        // Calculate core region bounds (skip padding on all sides)
        const int32 CoreSizeX = SizeX - 2 * PaddingSize;
        const int32 CoreSizeY = SizeY - 2 * PaddingSize;
        const int32 CoreSizeZ = SizeZ - 2 * PaddingSize;

        // If core is invalid, return false
        if (CoreSizeX <= 0 || CoreSizeY <= 0 || CoreSizeZ <= 0)
        {
            return false;
        }

        // Check only the core region - allow category 0 (air) and 1 (water/transparent)
        for (int32 z = PaddingSize; z < SizeZ - PaddingSize; ++z)
        {
            for (int32 y = PaddingSize; y < SizeY - PaddingSize; ++y)
            {
                for (int32 x = PaddingSize; x < SizeX - PaddingSize; ++x)
                {
                    const uint8 Cat = Get(x, y, z);
                    if (Cat >= 2) // Category 2+ are solid blocks
                    {
                        return false; // Found solid voxel in core
                    }
                }
            }
        }
        return true; // Only air/water in core
    }
};
class UVoxelBiomeDef; // forward declare, pas d’include ici

// Grille 2D paddée (SX+2 x SY+2) : Z de surface en monde + pointeur sur le biome choisi
struct FBiomeGrid2D
{
    int32 SizeX = 0;
    int32 SizeY = 0;
    TArray<int16> SurfaceZWorld;                 // Z monde de la surface par (x,y)
    TArray<const UVoxelBiomeDef*> BiomeAtXY;     // pointeur sur le biome (peut être nullptr)

    void Init(int32 InSizeX, int32 InSizeY)
    {
        SizeX = InSizeX; SizeY = InSizeY;
        // FIXED: Initialize to zero to avoid garbage data
        SurfaceZWorld.SetNumZeroed(SizeX * SizeY);
        BiomeAtXY.SetNumZeroed(SizeX * SizeY);
    }
    FORCEINLINE int32 Index(int32 x, int32 y) const { return x + y * SizeX; }
    bool IsValid() const
    {
        return SizeX > 0 && SizeY > 0
            && SurfaceZWorld.Num() == SizeX * SizeY
            && BiomeAtXY.Num() == SizeX * SizeY;
    }
};