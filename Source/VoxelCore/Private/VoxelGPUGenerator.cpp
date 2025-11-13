// VoxelGPUGenerator.cpp
// GPU-based voxel terrain generation implementation

#include "VoxelGPUGenerator.h"
#include "VoxelBiome.h"

// Unreal Rendering Headers
#include "RenderCore.h"
#include "RHI.h"
#include "RHIResources.h"
#include "RHIGPUReadback.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "ShaderParameterStruct.h"
#include "GlobalShader.h"
#include "RenderingThread.h"

// Async and Threading
#include "Async/Async.h"

// ============================================================================
// COMPUTE SHADER BINDING
// ============================================================================

/**
 * FVoxelGenerationCS - Compute shader for GPU voxel generation.
 *
 * This binds to VoxelGenerationCS.usf and provides the C++ interface
 * for dispatching the GPU generation compute shader.
 */
class FVoxelGenerationCS : public FGlobalShader
{
    DECLARE_GLOBAL_SHADER(FVoxelGenerationCS);
    SHADER_USE_PARAMETER_STRUCT(FVoxelGenerationCS, FGlobalShader);

public:
    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        // Chunk dimensions
        SHADER_PARAMETER(uint32, SizeX)
        SHADER_PARAMETER(uint32, SizeY)
        SHADER_PARAMETER(uint32, SizeZ)

        // World position offset
        SHADER_PARAMETER(int32, BaseWX)
        SHADER_PARAMETER(int32, BaseWY)
        SHADER_PARAMETER(int32, BaseWZ)

        // LOD scale
        SHADER_PARAMETER(uint32, LODScaleXY)

        // Generation settings
        SHADER_PARAMETER(int32, WorldSeed)
        SHADER_PARAMETER(int32, BaseHeight)
        SHADER_PARAMETER(int32, WaterLevel)
        SHADER_PARAMETER(int32, MaxCaveDepth)

        // Biome terrain parameters - Height
        SHADER_PARAMETER(float, HeightAmplitude)
        SHADER_PARAMETER(float, HeightFrequency)
        SHADER_PARAMETER(int32, HeightOctaves)
        SHADER_PARAMETER(float, HeightLacunarity)
        SHADER_PARAMETER(float, HeightGain)

        // Biome terrain parameters - Mountains
        SHADER_PARAMETER(float, MountainAmplitude)
        SHADER_PARAMETER(float, MountainFrequency)
        SHADER_PARAMETER(float, MountainThreshold)
        SHADER_PARAMETER(float, MountainSharpness)

        // Biome terrain parameters - 3D Features
        SHADER_PARAMETER(float, OverhangAmplitude)
        SHADER_PARAMETER(float, OverhangFrequency)
        SHADER_PARAMETER(float, WarpAmplitude)
        SHADER_PARAMETER(float, WarpFrequency)
        SHADER_PARAMETER(float, IslandAmplitude)
        SHADER_PARAMETER(float, IslandFrequency)
        SHADER_PARAMETER(float, IslandThreshold)

        // Biome terrain parameters - Caves
        SHADER_PARAMETER(float, CaveDensity)
        SHADER_PARAMETER(float, CaveFrequency2D)
        SHADER_PARAMETER(int32, CaveOctaves2D)
        SHADER_PARAMETER(float, CaveLacunarity2D)
        SHADER_PARAMETER(float, CaveGain2D)
        SHADER_PARAMETER(float, CaveFrequency3D)
        SHADER_PARAMETER(int32, CaveOctaves3D)

        // Output buffer
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, OutCategories)
    END_SHADER_PARAMETER_STRUCT()

    static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters&)
    {
        return true;
    }

    static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
    {
        FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
        OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE"), 4);
    }
};

// Register shader with Unreal's shader system
IMPLEMENT_GLOBAL_SHADER(FVoxelGenerationCS, "/Plugin/VoxelCore/VoxelGenerationCS.usf", "GenerateVoxelChunk", SF_Compute);

// ============================================================================
// PUBLIC API
// ============================================================================

bool FVoxelGPUGenerator::IsGPUGenerationAvailable()
{
    // Check if compute shaders are supported on this platform
    return GSupportsComputeShaders && FApp::CanEverRender();
}

void FVoxelGPUGenerator::GenerateChunkGPU(
    const FVoxelCoord& Coord,
    int32 SizeX, int32 SizeY, int32 SizeZ,
    int32 LODScaleXY,
    int32 Seed,
    int32 BaseHeight,
    int32 WaterLevel,
    int32 MaxCaveDepth,
    const FBiomeTerrainParams& BiomeParams,
    TFunction<void(TArray<uint8>&&)> OnComplete)
{
    // Validate GPU availability
    if (!IsGPUGenerationAvailable())
    {
        UE_LOG(LogTemp, Warning, TEXT("GPU generation not available - falling back to CPU"));
        // Caller should handle fallback to CPU generation
        return;
    }

    // Enqueue work on render thread
    ENQUEUE_RENDER_COMMAND(VoxelGPUGeneration)(
        [Coord, SizeX, SizeY, SizeZ, LODScaleXY, Seed, BaseHeight, WaterLevel, MaxCaveDepth, BiomeParams, OnComplete](FRHICommandListImmediate& RHICmdList)
        {
            DispatchGenerationShader_RenderThread(Coord, SizeX, SizeY, SizeZ, LODScaleXY, Seed, BaseHeight, WaterLevel, MaxCaveDepth, BiomeParams, OnComplete);
        });
}

// ============================================================================
// INTERNAL IMPLEMENTATION
// ============================================================================

void FVoxelGPUGenerator::DispatchGenerationShader_RenderThread(
    const FVoxelCoord& Coord,
    int32 SizeX, int32 SizeY, int32 SizeZ,
    int32 LODScaleXY,
    int32 Seed,
    int32 BaseHeight,
    int32 WaterLevel,
    int32 MaxCaveDepth,
    const FBiomeTerrainParams& BiomeParams,
    TFunction<void(TArray<uint8>&&)> OnComplete)
{
    check(IsInRenderingThread());

    FRDGBuilder GraphBuilder(FRHICommandListExecutor::GetImmediateCommandList());

    // Calculate buffer size
    // Categories are packed 2 bits per voxel, so we need (SizeX * SizeY * SizeZ * 2) bits
    // which equals (SizeX * SizeY * SizeZ + 3) / 4 bytes, rounded up to uint32s
    const int32 TotalVoxels = SizeX * SizeY * SizeZ;
    const int32 BufferSizeBytes = FMath::DivideAndRoundUp(TotalVoxels * 2, 32) * 4; // Round up to uint32s
    const int32 BufferSizeUints = BufferSizeBytes / 4;

    // Create output buffer (RDG managed)
    FRDGBufferRef OutputBuffer = GraphBuilder.CreateBuffer(
        FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), BufferSizeUints),
        TEXT("VoxelGenerationOutput"));

    FRDGBufferUAVRef OutputUAV = GraphBuilder.CreateUAV(OutputBuffer, PF_R32_UINT);

    // Get shader from global shader map
    TShaderMapRef<FVoxelGenerationCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

    // Setup shader parameters
    FVoxelGenerationCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FVoxelGenerationCS::FParameters>();

    // Chunk dimensions
    PassParameters->SizeX = SizeX;
    PassParameters->SizeY = SizeY;
    PassParameters->SizeZ = SizeZ;

    // World position
    PassParameters->BaseWX = Coord.Cx * SizeX;
    PassParameters->BaseWY = Coord.Cy * SizeY;
    PassParameters->BaseWZ = Coord.Cz * SizeZ;

    // LOD scale
    PassParameters->LODScaleXY = LODScaleXY;

    // Generation settings
    PassParameters->WorldSeed = Seed;
    PassParameters->BaseHeight = BaseHeight;
    PassParameters->WaterLevel = WaterLevel;
    PassParameters->MaxCaveDepth = MaxCaveDepth;

    // Biome parameters - Height
    PassParameters->HeightAmplitude = BiomeParams.HeightAmplitude;
    PassParameters->HeightFrequency = BiomeParams.HeightFrequency;
    PassParameters->HeightOctaves = BiomeParams.HeightOctaves;
    PassParameters->HeightLacunarity = BiomeParams.HeightLacunarity;
    PassParameters->HeightGain = BiomeParams.HeightGain;

    // Biome parameters - Mountains
    PassParameters->MountainAmplitude = BiomeParams.MountainAmplitude;
    PassParameters->MountainFrequency = BiomeParams.MountainFrequency;
    PassParameters->MountainThreshold = BiomeParams.MountainThreshold;
    PassParameters->MountainSharpness = BiomeParams.MountainSharpness;

    // Biome parameters - 3D Features
    PassParameters->OverhangAmplitude = BiomeParams.OverhangAmplitude;
    PassParameters->OverhangFrequency = BiomeParams.OverhangFrequency;
    PassParameters->WarpAmplitude = BiomeParams.WarpAmplitude;
    PassParameters->WarpFrequency = BiomeParams.WarpFrequency;
    PassParameters->IslandAmplitude = BiomeParams.IslandAmplitude;
    PassParameters->IslandFrequency = BiomeParams.IslandFrequency;
    PassParameters->IslandThreshold = BiomeParams.IslandThreshold;

    // Biome parameters - Caves
    PassParameters->CaveDensity = BiomeParams.CaveDensity;
    PassParameters->CaveFrequency2D = BiomeParams.CaveFrequency2D;
    PassParameters->CaveOctaves2D = BiomeParams.CaveOctaves2D;
    PassParameters->CaveLacunarity2D = BiomeParams.CaveLacunarity2D;
    PassParameters->CaveGain2D = BiomeParams.CaveGain2D;
    PassParameters->CaveFrequency3D = BiomeParams.CaveFrequency3D;
    PassParameters->CaveOctaves3D = BiomeParams.CaveOctaves3D;

    // Output buffer
    PassParameters->OutCategories = OutputUAV;

    // Calculate dispatch dimensions (4x4x4 thread groups)
    const uint32 ThreadGroupSizeX = 4;
    const uint32 ThreadGroupSizeY = 4;
    const uint32 ThreadGroupSizeZ = 4;

    const uint32 NumGroupsX = FMath::DivideAndRoundUp((uint32)SizeX, ThreadGroupSizeX);
    const uint32 NumGroupsY = FMath::DivideAndRoundUp((uint32)SizeY, ThreadGroupSizeY);
    const uint32 NumGroupsZ = FMath::DivideAndRoundUp((uint32)SizeZ, ThreadGroupSizeZ);

    // Dispatch compute shader
    FComputeShaderUtils::AddPass(
        GraphBuilder,
        RDG_EVENT_NAME("VoxelGeneration"),
        ComputeShader,
        PassParameters,
        FIntVector(NumGroupsX, NumGroupsY, NumGroupsZ));

    // Setup async GPU readback
    FRHIGPUBufferReadback* Readback = new FRHIGPUBufferReadback(TEXT("VoxelGenerationReadback"));

    AddReadbackBufferPass(GraphBuilder, RDG_EVENT_NAME("ReadbackVoxelGeneration"), OutputBuffer,
        [Readback, OutputBuffer](FRHICommandList& RHICmdList)
        {
            Readback->EnqueueCopy(RHICmdList, OutputBuffer->GetRHI(), 0u);
        });

    GraphBuilder.Execute();

    // Poll for readback completion on game thread
    AsyncTask(ENamedThreads::GameThread, [Readback, BufferSizeBytes, OnComplete]()
        {
            // Poll until GPU readback is complete (non-blocking)
            while (!Readback->IsReady())
            {
                FPlatformProcess::Sleep(0.001f); // 1ms sleep
            }

            // Read data from GPU
            TArray<uint8> CategoryData;
            CategoryData.SetNumUninitialized(BufferSizeBytes);

            const uint32* BufferPtr = (const uint32*)Readback->Lock(BufferSizeBytes);
            FMemory::Memcpy(CategoryData.GetData(), BufferPtr, BufferSizeBytes);
            Readback->Unlock();

            // Cleanup readback
            delete Readback;

            // Call completion callback with generated data
            OnComplete(MoveTemp(CategoryData));
        });
}
