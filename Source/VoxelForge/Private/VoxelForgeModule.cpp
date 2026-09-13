// VoxelForgeModule.cpp
// Module implementation - boilerplate

#include "VoxelForgeModule.h"
#include "VoxelStackSampler.h"

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
	FVoxelStackSampler::Get().Shutdown();
	UE_LOG(LogTemp, Log, TEXT("VoxelForge module shutdown."));
}
