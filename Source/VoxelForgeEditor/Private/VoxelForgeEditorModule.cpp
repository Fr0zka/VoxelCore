#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

class FVoxelForgeEditorModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override {}
	virtual void ShutdownModule() override {}
};

IMPLEMENT_MODULE(FVoxelForgeEditorModule, VoxelForgeEditor)
