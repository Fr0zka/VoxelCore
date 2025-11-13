#pragma once
#include "Engine/DataAsset.h"
#include "VoxelStructs.h"
#include "VoxelBlockTable.generated.h"

USTRUCT(BlueprintType)
struct FBlockFaceSet {
    GENERATED_BODY()
    UPROPERTY(EditAnywhere) int32 Top = 0;
    UPROPERTY(EditAnywhere) int32 Side = 0;
    UPROPERTY(EditAnywhere) int32 Bottom = 0;
};

UENUM(BlueprintType)
enum class EVoxelFaceDir : uint8 { XPos, XNeg, YPos, YNeg, ZPos, ZNeg };

UCLASS(BlueprintType)
class VOXELCORE_API UVoxelBlockTable : public UDataAsset
{
    GENERATED_BODY()
public:
    // Map block → per-face layer indices in the Texture2DArray
    UPROPERTY(EditAnywhere) TMap<EVoxelBlockID, FBlockFaceSet> Blocks;

    int32 GetLayer(EVoxelFaceDir Face, EVoxelBlockID Id) const
    {
        const FBlockFaceSet* S = Blocks.Find(Id);
        if (!S) return 0;
        switch (Face) {
        case EVoxelFaceDir::ZPos: return S->Top;
        case EVoxelFaceDir::ZNeg: return S->Bottom;
        default:                  return S->Side;
        }
    }
};
