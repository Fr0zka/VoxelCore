// Editor-only commandlets and offline explorers. This module is not linked by Game/Shipping.

using UnrealBuildTool;

public class VoxelForgeEditor : ModuleRules
{
	public VoxelForgeEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		// The commandlet's PNG shading, metre conversions, and JSON fields are part of its
		// deterministic artifact contract, so keep the editor-side arithmetic on the same precise
		// floating-point model as the authoritative Runtime module.
		FPSemantics = FPSemanticsMode.Precise;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Json",
			"ImageWrapper",
			"VoxelForge",
		});
	}
}
