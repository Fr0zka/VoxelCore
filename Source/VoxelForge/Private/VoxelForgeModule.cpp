// VoxelForgeModule.cpp
// Module implementation - boilerplate

#include "VoxelForgeModule.h"

// This macro registers our module with Unreal
// "VoxelForge" must match the module name in VoxelForge.Build.cs and .uplugin
IMPLEMENT_MODULE(FVoxelForgeModule, VoxelForge)

void FVoxelForgeModule::StartupModule()
{
	// Called when plugin loads
	// We don't need to do anything here for now
	UE_LOG(LogTemp, Log, TEXT("VoxelForge module started!"));
}

void FVoxelForgeModule::ShutdownModule()
{
	// Called when plugin unloads
	// Cleanup would go here if we had any global resources
	UE_LOG(LogTemp, Log, TEXT("VoxelForge module shutdown."));
}
