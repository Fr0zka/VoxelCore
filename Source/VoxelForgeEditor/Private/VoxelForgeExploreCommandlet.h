#pragma once

#include "Commandlets/Commandlet.h"
#include "VoxelForgeExploreCommandlet.generated.h"

/** Headless, editor-only explorer for authoritative density, traversal, and mesh output. */
UCLASS()
class VOXELFORGEEDITOR_API UVoxelForgeExploreCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UVoxelForgeExploreCommandlet();

	virtual int32 Main(const FString& Params) override;
};
