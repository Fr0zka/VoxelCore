// VoxelForgeModule.cpp
// Module implementation - boilerplate

#include "VoxelForgeModule.h"
#include "VoxelStackSampler.h"
#include "VoxelGenerator.h"
#include "VoxelMarchingCubesMesher.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

#if PLATFORM_WINDOWS
#include <math.h>
#endif

// This macro registers our module with Unreal
// "VoxelForge" must match the module name in VoxelForge.Build.cs and .uplugin
IMPLEMENT_MODULE(FVoxelForgeModule, VoxelForge)

void FVoxelForgeModule::StartupModule()
{
	// Called when plugin loads
	// Keep this diagnostic before any plugin-owned generation can begin.  The UCRT may select
	// its FMA3 math implementations during process startup; this switch lets the canonical export
	// compare that path with the non-FMA3 implementation on the same machine.
#if PLATFORM_WINDOWS
	const int32 Fma3Before = _get_FMA3_enable();
	int32 RequestedFma3 = 0;
	const bool bHasFma3Override = FParse::Value(
		FCommandLine::Get(), TEXT("voxel.CrtFma3="), RequestedFma3);
	if (bHasFma3Override)
	{
		_set_FMA3_enable(RequestedFma3 != 0 ? 1 : 0);
	}
	const int32 Fma3After = _get_FMA3_enable();
	UE_LOG(LogTemp, Log,
		TEXT("VoxelForge CRT FMA3 diagnostic: before=%d override=%s requested=%d after=%d"),
		Fma3Before,
		bHasFma3Override ? TEXT("yes") : TEXT("no"),
		RequestedFma3,
		Fma3After);
#endif
	// These parsers used to be entered lazily by worker-side density/mesher calls.  Resolve the
	// command line once while the module is starting, before any generation task is submitted.
	VoxelGenLOD::InitializeConsoleSwitches();
	UVoxelMarchingCubesMesher::InitializeConsoleSwitches();
	UE_LOG(LogTemp, Log, TEXT("VoxelForge module started!"));
}

void FVoxelForgeModule::ShutdownModule()
{
	// Called when plugin unloads
	FVoxelStackSampler::Get().Shutdown();
	UE_LOG(LogTemp, Log, TEXT("VoxelForge module shutdown."));
}
