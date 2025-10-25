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

UENUM()
enum class EVoxelLODLevel : uint8
{
    LOD0 = 0, // full voxels
    LOD1 = 1, // coarsened voxels (XY)
    LOD2 = 2  // heightfield impostor
};
