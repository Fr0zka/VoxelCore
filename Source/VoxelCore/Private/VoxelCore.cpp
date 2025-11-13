// Copyright Epic Games, Inc. All Rights Reserved.
#include "VoxelCore.h"
#include "Modules/ModuleManager.h"
#include "Interfaces/IPluginManager.h"
#include "ShaderCore.h"
#include "Misc/Paths.h"

#define LOCTEXT_NAMESPACE "FVoxelWorldModule"

// ============================================================================
// MODULE LIFECYCLE
// ============================================================================

/**
 * Module startup: Initialize VoxelCore plugin.
 * Maps shader directory for GPU mesher compute shaders.
 */
void FVoxelCoreModule::StartupModule()
{
	// Map virtual shader path /Plugin/VoxelCore to physical plugin Shaders directory
	// This allows GPU mesher shaders (GPUGreedyMesher.usf) to be found at runtime
	TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("VoxelCore"));
	if (Plugin.IsValid())
	{
		const FString ShaderDir = FPaths::Combine(Plugin->GetBaseDir(), TEXT("Shaders"));
		AddShaderSourceDirectoryMapping(TEXT("/Plugin/VoxelCore"), ShaderDir);

		// Log shader directory mapping for debugging
		UE_LOG(LogTemp, Display, TEXT("[VoxelCore] Shader directory mapped: %s"), *ShaderDir);
		UE_LOG(LogTemp, Display, TEXT("[VoxelCore] GPUGreedyMesher.usf: %s"),
			FPaths::FileExists(FPaths::Combine(ShaderDir, TEXT("GPUGreedyMesher.usf"))) ? TEXT("FOUND") : TEXT("MISSING"));
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("[VoxelCore] Plugin descriptor not found; shader directory mapping skipped."));
	}
}

/**
 * Module shutdown: Cleanup VoxelCore plugin.
 * Called during engine shutdown or module unload.
 */
void FVoxelCoreModule::ShutdownModule()
{
	// Cleanup resources if needed
	// (Shader mappings are automatically cleaned up by engine)
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FVoxelCoreModule, VoxelCore)
