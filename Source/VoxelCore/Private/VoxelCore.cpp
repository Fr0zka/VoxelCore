// Copyright Epic Games, Inc. All Rights Reserved.
#include "VoxelCore.h"
#include "Modules/ModuleManager.h"
#include "Interfaces/IPluginManager.h"
#include "ShaderCore.h"        // AddShaderSourceDirectoryMapping
#include "Misc/Paths.h"


#define LOCTEXT_NAMESPACE "FVoxelWorldModule"

void FVoxelCoreModule::StartupModule()
{

        // Map /Plugin/VoxelCore -> <PluginRoot>/Shaders
        TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("VoxelCore"));
        if (Plugin.IsValid())
        {
            const FString ShaderDir = FPaths::Combine(Plugin->GetBaseDir(), TEXT("Shaders"));
            AddShaderSourceDirectoryMapping(TEXT("/Plugin/VoxelCore"), ShaderDir);

            // Optional visibility log:
            UE_LOG(LogTemp, Display, TEXT("[VoxelCore] ShaderDir: %s (GPUGreedyMesher.usf %s)"),
                *ShaderDir,
                FPaths::FileExists(FPaths::Combine(ShaderDir, TEXT("GPUGreedyMesher.usf"))) ? TEXT("FOUND") : TEXT("MISSING"));
        }
        else
        {
            UE_LOG(LogTemp, Warning, TEXT("[VoxelCore] Plugin descriptor not found; shader dir mapping skipped."));
        }
}

void FVoxelCoreModule::ShutdownModule()
{
	// This function may be called during shutdown to clean up your module.  For modules that support dynamic reloading,
	// we call this function before unloading the module.
}

#undef LOCTEXT_NAMESPACE
	
IMPLEMENT_MODULE(FVoxelCoreModule, VoxelCore)