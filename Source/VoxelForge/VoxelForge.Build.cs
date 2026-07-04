// VoxelForge.Build.cs
// This file tells Unreal how to compile our plugin

using UnrealBuildTool;

public class VoxelForge : ModuleRules
{
	public VoxelForge(ReadOnlyTargetRules Target) : base(Target)
	{
		// PCH = Precompiled Headers - speeds up compilation
		// UseExplicitOrSharedPCHs is the modern recommended setting
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// Modules we depend on:
		// - Core: Basic types (TArray, FString, etc.)
		// - CoreUObject: UObject system (UCLASS, UPROPERTY, etc.)
		// - Engine: Game engine features (AActor, UWorld, etc.)
		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"GameplayTags",  // For FGameplayTagContainer on strate definitions
		});

		// RealtimeMeshComponent - the rendering library we'll use
		// This is a third-party plugin you should have installed
		PublicDependencyModuleNames.Add("RealtimeMeshComponent");

		// Private dependencies - only used in our .cpp files, not exposed in headers
		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"ImageWrapper",  // PNG encode for the biome-map preview bake (BakeBiomePreview)
			"RHI",           // Texture3D create + RHIUpdateTexture3D for the density volume (mini-sun shadows)
			"RenderCore",    // ENQUEUE_RENDER_COMMAND for the volume upload
		});
	}
}
