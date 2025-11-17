using UnrealBuildTool;
using UnrealBuildTool.Rules;


public class VoxelCore : ModuleRules
{
    public VoxelCore(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        // ============================================================================
        // AGGRESSIVE PERFORMANCE OPTIMIZATIONS
        // ============================================================================

        // Enable aggressive code optimization
        OptimizeCode = CodeOptimization.Always;  // Always optimize, even in dev builds

        // Note: bUseAVX causes type conflicts with Unreal's VectorRegister types
        // Our SIMD optimizations use SSE2 (always available on x64) instead

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
