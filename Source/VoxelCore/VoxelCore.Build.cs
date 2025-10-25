using System.IO;
using UnrealBuildTool;
using UnrealBuildTool.Rules;

public class VoxelCore : ModuleRules
{
    public VoxelCore(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(new string[] {
            "Core", "CoreUObject", "Engine", "InputCore",
            "RenderCore", "RHI", "ProceduralMeshComponent", "UMG"
        });

        PrivateDependencyModuleNames.AddRange(new string[] { "Slate", "SlateCore", "Projects"});

        // Optional RuntimeMeshComponent (plugin)
        bool bHasRMC = false;
        string RMCPath = Path.Combine(EngineDirectory, "Plugins", "Marketplace", "RuntimeMeshComponent");
        if (Directory.Exists(RMCPath))
        {
            bHasRMC = true;
            PublicDependencyModuleNames.Add("RuntimeMeshComponent");
        }
        PublicDefinitions.Add("WITH_RUNTIME_MESHCOMPONENT=" + (bHasRMC ? "1" : "0"));

        if (Target.Platform == UnrealTargetPlatform.Win64)
        {
            bUseRTTI = true;
            bEnableExceptions = true;
        }
    }
}
