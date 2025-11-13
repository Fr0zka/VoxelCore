#pragma once
#include "Engine/Texture2DArray.h"
#include "Engine/DataAsset.h"
#include "VoxelMaterialSet.generated.h"

UCLASS(BlueprintType)
class VOXELCORE_API UVoxelMaterialSet : public UDataAsset {
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere) TObjectPtr<UTexture2DArray> Albedo = nullptr;
	UPROPERTY(EditAnywhere) TObjectPtr<UTexture2DArray> Normal = nullptr;
	UPROPERTY(EditAnywhere) TObjectPtr<UTexture2DArray> ORM = nullptr;
	UPROPERTY(EditAnywhere) float UVScale = 1.0f;
	// Optional: set to (NumSlices-1). If unset we’ll use 255.
	UPROPERTY(EditAnywhere) int32 LayersMinusOne = 255;
};