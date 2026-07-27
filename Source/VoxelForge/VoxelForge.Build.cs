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
		// FLOAT MODEL — pinned to Precise so Windows and Linux compute the SAME WORLD.
		// ============================================================================
		// Jahni, 2026-07-27: the game must be playable on both Linux and Windows, either side
		// hosting. The MP design replicates the SEED and has every peer regenerate the terrain, so
		// a Windows host and a Linux client must agree on the density field.
		//
		// They did not, by construction. UBT resolves FPSemanticsMode.Default differently per
		// toolchain (verified in UE 5.7 source, not assumed):
		//     VCToolChain.cs:1264   Default/Imprecise -> "/fp:fast"       (Windows/MSVC)
		//     ClangToolChain.cs:712 Default/Precise   -> "-ffp-contract=off"  (Linux/Mac/Clang)
		// So the same source was compiled under OPPOSITE float rules depending on who built it.
		//
		// Precise resolves to "/fp:precise" on MSVC and "-ffp-contract=off" on Clang — both
		// IEEE-754 compliant with no FMA contraction, so the two toolchains agree BY CONSTRUCTION
		// rather than by luck. That is the fix for AUDIT-2026-07.md C9.
		//
		// COST: /fp:precise forbids the reassociation and contraction /fp:fast allowed, on a
		// noise-heavy hot path. Expect a measurable perf regression and check it against
		// ARCHITECTURE 8.10 — determinism across platforms is worth paying for, but the price
		// should be known, not assumed.
		//
		// VERIFY: run VoxelForge.Determinism.CrossPlatformDigest on both platforms and compare the
		// SHAPE digest (sign of density = the world) and the FIELD digest (bit-for-bit). Pin the
		// values in that test once they agree, and it guards this forever after.
		FPSemantics = FPSemanticsMode.Precise;

		// ============================================================================
		// ⚠️ HISTORY — why this took a second attempt (kept: it explains the includes below)
		// ============================================================================
		// Setting FPSemantics (or any property that alters this module's compile environment)
		// makes VoxelForge ineligible for the ENGINE'S SHARED PCH — UBT can only share a
		// precompiled header between modules whose compile environments match. The first attempt
		// (2026-07-27) was therefore reverted: it failed with ~30 "undefined type" errors that
		// were not FP-related at all — UMaterialInterface, USoundBase, TSubclassOf<AActor>,
		// ENABLE_DRAW_DEBUG — i.e. includes this plugin had always taken from the shared PCH for
		// free. That is a latent IWYU debt, not an FP problem.
		//
		// ⚠️ SO IF YOU SEE "undefined type" ERRORS HERE, THEY ARE IWYU, NOT FLOAT SETTINGS.
		// The fix is to add the missing include or forward declaration to the header that needs
		// it — never to revert FPSemantics, which is now load-bearing for cross-platform play.
		// Headers fixed on 2026-07-27: VoxelBiomeDefinition, VoxelSettings, VoxelStrateDefinition,
		// VoxelStrateTypes, VoxelContentManager, VoxelAtmosphereManager, VoxelDensityVolume.
		// Plus VoxelWorld.cpp (GameFramework/Pawn.h) — the .cpp files needed auditing too, not just
		// the public headers. That was the whole residual tail: one site, found in one build.
		// Expect a residual tail: the shared PCH hid these for years and only a build enumerates
		// them all. VoxelDensityVolume's was the nasty one — ENABLE_DRAW_DEBUG is used in an #if,
		// and an undefined macro there is silently 0 rather than an error.

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
