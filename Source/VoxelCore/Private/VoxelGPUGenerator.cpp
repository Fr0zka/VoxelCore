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
// STATIC MEMBERS
// ============================================================================

TArray<TSharedPtr<FVoxelGPUGenerator::FGPUGenerationJob, ESPMode::ThreadSafe>> FVoxelGPUGenerator::PendingJobs;
FCriticalSection FVoxelGPUGenerator::JobsMutex;

// ============================================================================
// PUBLIC API
// ============================================================================

bool FVoxelGPUGenerator::IsGPUGenerationAvailable()
{
    // Check if compute shaders are supported on this platform
    // GMaxRHIFeatureLevel must be at least SM5 (ERHIFeatureLevel::SM5) for compute shaders
    return FApp::CanEverRender() && (GMaxRHIFeatureLevel >= ERHIFeatureLevel::SM5);
}

void FVoxelGPUGenerator::TickGPUGenerationJobs()
{
    QUICK_SCOPE_CYCLE_COUNTER(STAT_VoxelGPU_TickGeneration);

    TArray<TSharedPtr<FGPUGenerationJob, ESPMode::ThreadSafe>> CompletedJobs;

    {
        FScopeLock Lock(&JobsMutex);

        // Check each pending job
        for (int32 Index = PendingJobs.Num() - 1; Index >= 0; --Index)
        {
            TSharedPtr<FGPUGenerationJob, ESPMode::ThreadSafe> Job = PendingJobs[Index];
            if (!Job.IsValid() || !Job->Readback)
            {
                PendingJobs.RemoveAtSwap(Index);
                continue;
            }

            // Check if readback is ready (non-blocking)
            if (!Job->Readback->IsReady())
            {
                continue; // Not ready yet - check next frame
            }

            // Readback is ready - move to completed list
            PendingJobs.RemoveAtSwap(Index);
            CompletedJobs.Add(Job);
        }
    }

    // Process completed jobs (lock and copy data on render thread)
    for (TSharedPtr<FGPUGenerationJob, ESPMode::ThreadSafe>& Job : CompletedJobs)
    {
        if (!Job.IsValid())
            continue;

        TArray<uint8> CategoryData;
        CategoryData.SetNumUninitialized(Job->BufferSizeBytes);

        FEvent* ReadbackDone = FPlatformProcess::GetSynchEventFromPool(false);

        // Enqueue render command to lock and copy data (Lock MUST be on render thread)
        FRHIGPUBufferReadback* ReadbackPtr = Job->Readback.Get();
        int32 BufferSize = Job->BufferSizeBytes;

        ENQUEUE_RENDER_COMMAND(VoxelGeneration_ReadbackData)(
            [ReadbackPtr, BufferSize, &CategoryData, ReadbackDone](FRHICommandListImmediate& RHICmdList)
            {
                const uint32* BufferPtr = (const uint32*)ReadbackPtr->Lock(BufferSize);
                if (BufferPtr)
                {
                    FMemory::Memcpy(CategoryData.GetData(), BufferPtr, BufferSize);
                }
                ReadbackPtr->Unlock();
                ReadbackDone->Trigger();
            });

        // Wait for render thread to finish copying
        ReadbackDone->Wait();
        FPlatformProcess::ReturnSynchEventToPool(ReadbackDone);

        // Call completion callback with generated data
        if (Job->OnComplete)
        {
            Job->OnComplete(MoveTemp(CategoryData));
        }

        // Readback will be cleaned up when Job is destroyed
    }
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

    // CRITICAL: Clear output buffer to zero (prevents garbage data)
    AddClearUAVPass(GraphBuilder, OutputUAV, 0u);

    // Get shader from global shader map
    TShaderMapRef<FVoxelGenerationCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

    // Setup shader parameters
    FVoxelGenerationCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FVoxelGenerationCS::FParameters>();

    // Chunk dimensions
    PassParameters->SizeX = SizeX;
    PassParameters->SizeY = SizeY;
    PassParameters->SizeZ = SizeZ;

    // World position calculation
    // SizeX/Y/Z includes the +2 halo, so (SizeX-2) = actual voxel grid size
    // Chunk world size = voxel grid size * LOD scale
    // Example: LOD0 has 16 voxels covering 16 world units (16*1=16)
    //          LOD1 has 8 voxels covering 16 world units (8*2=16)
    const int32 VoxelGridSizeX = SizeX - 2;
    const int32 VoxelGridSizeY = SizeY - 2;
    const int32 VoxelGridSizeZ = SizeZ - 2;

    const int32 ChunkWorldSizeX = VoxelGridSizeX * LODScaleXY;
    const int32 ChunkWorldSizeY = VoxelGridSizeY * LODScaleXY;
    const int32 ChunkWorldSizeZ = VoxelGridSizeZ; // Z never scales

    // BaseWX/Y/Z = world coordinate of the chunk origin (matches CPU calculation)
    PassParameters->BaseWX = Coord.Cx * ChunkWorldSizeX;
    PassParameters->BaseWY = Coord.Cy * ChunkWorldSizeY;
    PassParameters->BaseWZ = Coord.Cz * ChunkWorldSizeZ;

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

    // Create job to track async readback (polled each frame, non-blocking)
    TSharedPtr<FGPUGenerationJob, ESPMode::ThreadSafe> Job = MakeShared<FGPUGenerationJob, ESPMode::ThreadSafe>();
    Job->Readback = TUniquePtr<FRHIGPUBufferReadback>(Readback);
    Job->BufferSizeBytes = BufferSizeBytes;
    Job->OnComplete = OnComplete;

    // Add to pending jobs list (will be polled by TickGPUGenerationJobs)
    {
        FScopeLock Lock(&JobsMutex);
        PendingJobs.Add(Job);
    }
}
