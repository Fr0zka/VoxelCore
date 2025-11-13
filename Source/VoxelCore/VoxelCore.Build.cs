using UnrealBuildTool;
using UnrealBuildTool.Rules;


public class VoxelCore : ModuleRules
{
    public VoxelCore(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(new string[] {
            "Core", "CoreUObject", "Engine", "InputCore",
            "RenderCore", "RHI", "ProceduralMeshComponent", "UMG", "RHICore", "RealtimeMeshComponent"
        });

        PrivateDependencyModuleNames.AddRange(new string[] { "Slate", "SlateCore", "Projects" });

        PublicDefinitions.Add("WITH_REALTIME_MESHCOMPONENT=1");

        if (Target.Platform == UnrealTargetPlatform.Win64)
        {
            bUseRTTI = true;
            bEnableExceptions = true;
        }
    }
}
