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
		// ⚠️ TEMPORARY EXPERIMENT — 2026-07-27. REMOVE THIS LINE WHEN THE ANSWER IS IN.
		// ============================================================================
		// Testing whether the ~1 ULP residue between GetMazeDensity and its operator-stack
		// port (VoxelForge.OpStack.MazeEquivalence: 454 of 20000 samples, 0 crossing the
		// isosurface) is caused by the compiler being allowed to reassociate identical
		// source differently per translation unit.
		//
		// FPSemantics is a PER-MODULE property. Setting it on the VoxelM game module does
		// NOT affect this one — every line of density code lives in VoxelForge, so the
		// switch has to be here to mean anything. (That mistake already cost one build and
		// one wrong conclusion.)
		//
		// UnrealBuildTool's Windows default is /fp:fast ("Default is imprecise FP
		// semantics", VCToolChain.cs); every Clang target defaults to precise instead.
		//
		// READ THE RESULT LIKE THIS:
		//   454 -> 0    : the FP model WAS the cause. Then decide separately whether to keep
		//                 precise (it costs vectorisation on the density hot path — the thing
		//                 T2.a's SIMD noise work was buying — for an unmeasured amount).
		//   454 -> 454  : the FP model is NOT the cause and the difference is real logic.
		//                 Read the WORST-POINT DUMP the test now prints.
		//
		// EITHER WAY THIS LINE COMES BACK OUT once measured. Keeping /fp:precise on the hot
		// path is a decision that needs a profile, not a leftover from a diagnostic.
		FPSemantics = FPSemanticsMode.Precise;

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
