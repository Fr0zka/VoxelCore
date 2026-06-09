// VoxelForgeModule.h
// Module interface - this is boilerplate that every Unreal plugin needs

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

/**
 * FVoxelForgeModule
 *
 * This is the module class for VoxelForge. Every Unreal plugin needs one.
 * It handles startup and shutdown of the plugin.
 *
 * For our purposes, this is just boilerplate - the real code is elsewhere.
 */
class FVoxelForgeModule : public IModuleInterface
{
public:
	// Called when the plugin loads (game/editor starts)
	virtual void StartupModule() override;

	// Called when the plugin unloads (game/editor closes)
	virtual void ShutdownModule() override;
};
