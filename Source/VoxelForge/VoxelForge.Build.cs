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

		// ============================================================================
		// ⚠️ DO NOT SET `FPSemantics` HERE — tried 2026-07-27, it does not build.
		// ============================================================================
		// Setting FPSemantics (or any other property that alters this module's compile
		// environment) makes VoxelForge ineligible for the ENGINE'S SHARED PCH: UBT can only
		// share a precompiled header between modules whose compile environments match. The
		// build then fails with ~30 "undefined type" errors — UMaterialInterface, USoundBase,
		// TSubclassOf<AActor>, APawn, ENABLE_DRAW_DEBUG — none of which are FP-related. They
		// are includes this plugin has always relied on the shared PCH to provide for free.
		//
		// So the plugin has a latent IWYU (include-what-you-use) debt: several public headers
		// use engine types they never include. That is worth fixing on its own terms one day
		// (UE has been moving away from implicit shared-PCH includes for years), but it is a
		// real chunk of work and must not be attempted inside an unrelated diagnostic.
		//
		// The FP question it was meant to settle — whether identical source reassociates
		// differently per translation unit under /fp:fast — is now answered inside
		// VoxelForge.OpStack.MazeEquivalence instead, by compiling a verbatim copy of the
		// Maze core into the TEST's translation unit and comparing all three. No build
		// settings involved, and it cannot break anything.

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
