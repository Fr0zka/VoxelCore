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
#include "VoxelNoise.h"
// Async and Threading
#include "Async/Async.h"

// ============================================================================
// BIOME DATA STRUCTURE (must match shader BiomeData struct layout exactly)
// ============================================================================



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

        // Climate noise parameters (Temperature)
        SHADER_PARAMETER(float, TempBaseFreq)
        SHADER_PARAMETER(int32, TempOctaves)
        SHADER_PARAMETER(float, TempLacunarity)
        SHADER_PARAMETER(float, TempGain)
        SHADER_PARAMETER(float, TempWarpStrength)
        SHADER_PARAMETER(int32, TempSeedOffset)

        // Climate noise parameters (Moisture)
        SHADER_PARAMETER(float, MoistBaseFreq)
        SHADER_PARAMETER(int32, MoistOctaves)
        SHADER_PARAMETER(float, MoistLacunarity)
        SHADER_PARAMETER(float, MoistGain)
        SHADER_PARAMETER(float, MoistWarpStrength)
        SHADER_PARAMETER(int32, MoistSeedOffset)

        // Biome array (structured buffer for per-column biome selection)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGPUBiomeData>, Biomes)
        SHADER_PARAMETER(uint32, BiomeCount)

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

int32 FVoxelGPUGenerator::GetPendingJobCount()
{
    FScopeLock Lock(&JobsMutex);
    return PendingJobs.Num();
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
    // IMPORTANT: Jobs in CompletedJobs have already passed IsReady() check,
    // so they are safe to lock on the render thread
    for (TSharedPtr<FGPUGenerationJob, ESPMode::ThreadSafe>& Job : CompletedJobs)
    {
        if (!Job.IsValid())
            continue;

        // Capture job data for async processing
        TFunction<void(TArray<uint8>&&)> OnComplete = Job->OnComplete;
        int32 BufferSizeBytes = Job->BufferSizeBytes;

        // CRITICAL: Move ownership of readback to render thread command
        // This keeps the readback alive until the command executes
        FRHIGPUBufferReadback* ReadbackPtr = Job->Readback.Release();

        // Enqueue render command to lock and copy data (Lock MUST be on render thread)
        ENQUEUE_RENDER_COMMAND(VoxelGeneration_ReadbackData)(
            [ReadbackPtr, BufferSizeBytes, OnComplete](FRHICommandListImmediate& RHICmdList)
            {
                // Allocate output buffer on render thread
                TArray<uint8> CategoryData;
                CategoryData.SetNumUninitialized(BufferSizeBytes);

                // Lock and copy GPU data (safe because IsReady() already passed)
                const uint32* BufferPtr = (const uint32*)ReadbackPtr->Lock(BufferSizeBytes);
                if (BufferPtr)
                {
                    FMemory::Memcpy(CategoryData.GetData(), BufferPtr, BufferSizeBytes);
                    ReadbackPtr->Unlock();

                    // Call completion callback on game thread (async task)
                    if (OnComplete)
                    {
                        AsyncTask(ENamedThreads::GameThread, [OnComplete, CategoryData = MoveTemp(CategoryData)]() mutable
                        {
                            OnComplete(MoveTemp(CategoryData));
                        });
                    }
                }
                else
                {
                    // Lock failed - log error
                    UE_LOG(LogTemp, Error, TEXT("VoxelGPU: Failed to lock readback buffer!"));
                }

                // Clean up readback buffer (we own it now)
                delete ReadbackPtr;
            });
    }
}

void FVoxelGPUGenerator::GenerateChunkGPU(
    const FVoxelCoord& Coord,
    int32 SizeX, int32 SizeY, int32 SizeZ,
    int32 BaseSizeX, int32 BaseSizeY, int32 BaseSizeZ,
    int32 LODScaleXY,
    int32 Seed,
    int32 BaseHeight,
    int32 WaterLevel,
    int32 MaxCaveDepth,
    const UVoxelBiomeTable* BiomeTable,
    const UVoxelNoiseProfile* NoiseProfile,
    TFunction<void(TArray<uint8>&&)> OnComplete)
{
    // Validate GPU availability
    if (!IsGPUGenerationAvailable())
    {
        UE_LOG(LogTemp, Warning, TEXT("GPU generation not available - falling back to CPU"));
        // Caller should handle fallback to CPU generation
        return;
    }

    // Convert UVoxelBiomeTable to TArray<FGPUBiomeData> on game thread
    TArray<FGPUBiomeData> BiomeDataArray;
    if (BiomeTable && BiomeTable->Biomes.Num() > 0)
    {
        for (const UVoxelBiomeDef* Biome : BiomeTable->Biomes)
        {
            if (!Biome)
                continue;

            FGPUBiomeData Data;
            Data.TempMin = Biome->TempMin;
            Data.TempMax = Biome->TempMax;
            Data.MoistMin = Biome->MoistMin;
            Data.MoistMax = Biome->MoistMax;

            // Copy all terrain parameters
            const FBiomeTerrainParams& P = Biome->TerrainParams;
            Data.HeightAmplitude = P.HeightAmplitude;
            Data.HeightFrequency = P.HeightFrequency;
            Data.HeightOctaves = P.HeightOctaves;
            Data.HeightLacunarity = P.HeightLacunarity;
            Data.HeightGain = P.HeightGain;

            Data.MountainAmplitude = P.MountainAmplitude;
            Data.MountainFrequency = P.MountainFrequency;
            Data.MountainThreshold = P.MountainThreshold;
            Data.MountainSharpness = P.MountainSharpness;

            Data.OverhangAmplitude = P.OverhangAmplitude;
            Data.OverhangFrequency = P.OverhangFrequency;
            Data.WarpAmplitude = P.WarpAmplitude;
            Data.WarpFrequency = P.WarpFrequency;
            Data.IslandAmplitude = P.IslandAmplitude;
            Data.IslandFrequency = P.IslandFrequency;
            Data.IslandThreshold = P.IslandThreshold;
            Data.IslandBandCenterZ = P.IslandBandCenterZ;
            Data.IslandBandHalfThickness = P.IslandBandHalfThickness;

            Data.CaveDensity = P.CaveDensity;
            Data.CaveFrequency2D = P.CaveFrequency2D;
            Data.CaveOctaves2D = P.CaveOctaves2D;
            Data.CaveLacunarity2D = P.CaveLacunarity2D;
            Data.CaveGain2D = P.CaveGain2D;
            Data.CaveFrequency3D = P.CaveFrequency3D;
            Data.CaveOctaves3D = P.CaveOctaves3D;

            BiomeDataArray.Add(Data);
        }
    }

    // Fallback: Create default biome if table is empty
    if (BiomeDataArray.Num() == 0)
    {
        FGPUBiomeData DefaultData;
        FMemory::Memzero(DefaultData);

        // Default climate range (accepts all)
        DefaultData.TempMin = 0.0f;
        DefaultData.TempMax = 1.0f;
        DefaultData.MoistMin = 0.0f;
        DefaultData.MoistMax = 1.0f;

        // Default terrain parameters
        FBiomeTerrainParams DefaultParams;
        DefaultData.HeightAmplitude = DefaultParams.HeightAmplitude;
        DefaultData.HeightFrequency = DefaultParams.HeightFrequency;
        DefaultData.HeightOctaves = DefaultParams.HeightOctaves;
        DefaultData.HeightLacunarity = DefaultParams.HeightLacunarity;
        DefaultData.HeightGain = DefaultParams.HeightGain;

        BiomeDataArray.Add(DefaultData);
    }

    // Extract climate noise parameters from NoiseProfile (if available)
    float TempBaseFreq = 1.0f / 256.0f;      // Default fallback
    int32 TempOctaves = 3;
    float TempLacunarity = 2.0f;
    float TempGain = 0.5f;
    float TempWarpStrength = 0.0f;
    int32 TempSeedOffset = 0;

    float MoistBaseFreq = 1.0f / 192.0f;     // Default fallback
    int32 MoistOctaves = 3;
    float MoistLacunarity = 2.0f;
    float MoistGain = 0.5f;
    float MoistWarpStrength = 0.0f;
    int32 MoistSeedOffset = 0;

    if (NoiseProfile)
    {
        TempBaseFreq = NoiseProfile->Temperature.BaseFreq;
        TempOctaves = NoiseProfile->Temperature.Octaves;
        TempLacunarity = NoiseProfile->Temperature.Lacunarity;
        TempGain = NoiseProfile->Temperature.Gain;
        TempWarpStrength = NoiseProfile->Temperature.WarpStrength;
        TempSeedOffset = NoiseProfile->Temperature.SeedOffset;

        MoistBaseFreq = NoiseProfile->Moisture.BaseFreq;
        MoistOctaves = NoiseProfile->Moisture.Octaves;
        MoistLacunarity = NoiseProfile->Moisture.Lacunarity;
        MoistGain = NoiseProfile->Moisture.Gain;
        MoistWarpStrength = NoiseProfile->Moisture.WarpStrength;
        MoistSeedOffset = NoiseProfile->Moisture.SeedOffset;
    }

    // Enqueue work on render thread
    ENQUEUE_RENDER_COMMAND(VoxelGPUGeneration)(
        [Coord, SizeX, SizeY, SizeZ, BaseSizeX, BaseSizeY, BaseSizeZ, LODScaleXY, Seed, BaseHeight, WaterLevel, MaxCaveDepth,
         TempBaseFreq, TempOctaves, TempLacunarity, TempGain, TempWarpStrength, TempSeedOffset,
         MoistBaseFreq, MoistOctaves, MoistLacunarity, MoistGain, MoistWarpStrength, MoistSeedOffset,
         BiomeDataArray, OnComplete](FRHICommandListImmediate& RHICmdList)
        {
            DispatchGenerationShader_RenderThread(Coord, SizeX, SizeY, SizeZ, BaseSizeX, BaseSizeY, BaseSizeZ, LODScaleXY, Seed, BaseHeight, WaterLevel, MaxCaveDepth,
                TempBaseFreq, TempOctaves, TempLacunarity, TempGain, TempWarpStrength, TempSeedOffset,
                MoistBaseFreq, MoistOctaves, MoistLacunarity, MoistGain, MoistWarpStrength, MoistSeedOffset,
                BiomeDataArray, OnComplete);
        });
}

// ============================================================================
// INTERNAL IMPLEMENTATION
// ============================================================================

void FVoxelGPUGenerator::DispatchGenerationShader_RenderThread(
    const FVoxelCoord& Coord,
    int32 SizeX, int32 SizeY, int32 SizeZ,
    int32 BaseSizeX, int32 BaseSizeY, int32 BaseSizeZ,
    int32 LODScaleXY,
    int32 Seed,
    int32 BaseHeight,
    int32 WaterLevel,
    int32 MaxCaveDepth,
    float TempBaseFreq, int32 TempOctaves, float TempLacunarity, float TempGain, float TempWarpStrength, int32 TempSeedOffset,
    float MoistBaseFreq, int32 MoistOctaves, float MoistLacunarity, float MoistGain, float MoistWarpStrength, int32 MoistSeedOffset,
    const TArray<FGPUBiomeData>& BiomeDataArray,
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

    // Create biome data buffer
    const int32 BiomeCount = BiomeDataArray.Num();
    FRDGBufferRef BiomeBuffer = GraphBuilder.CreateBuffer(
        FRDGBufferDesc::CreateStructuredDesc(sizeof(FGPUBiomeData), BiomeCount),
        TEXT("BiomeDataBuffer"));

    // Upload biome data to GPU
    GraphBuilder.QueueBufferUpload(BiomeBuffer, BiomeDataArray.GetData(),
        BiomeCount * sizeof(FGPUBiomeData));

    FRDGBufferSRVRef BiomeSRV = GraphBuilder.CreateSRV(BiomeBuffer);

    // Get shader from global shader map
    TShaderMapRef<FVoxelGenerationCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

    // Setup shader parameters
    FVoxelGenerationCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FVoxelGenerationCS::FParameters>();

    // Chunk dimensions
    PassParameters->SizeX = SizeX;
    PassParameters->SizeY = SizeY;
    PassParameters->SizeZ = SizeZ;

    // World position calculation (CRITICAL: must match CPU exactly!)
    // CPU uses: BaseWX = Coord.Cx * P.SizeX (base unscaled chunk size)
    // GPU must do the same - use BaseSizeX/Y/Z (NOT VoxelGridSize * LODScale!)
    PassParameters->BaseWX = Coord.Cx * BaseSizeX;
    PassParameters->BaseWY = Coord.Cy * BaseSizeY;
    PassParameters->BaseWZ = Coord.Cz * BaseSizeZ;

    // LOD scale
    PassParameters->LODScaleXY = LODScaleXY;

    // Generation settings
    PassParameters->WorldSeed = Seed;
    PassParameters->BaseHeight = BaseHeight;
    PassParameters->WaterLevel = WaterLevel;
    PassParameters->MaxCaveDepth = MaxCaveDepth;

    // Climate noise parameters
    PassParameters->TempBaseFreq = TempBaseFreq;
    PassParameters->TempOctaves = TempOctaves;
    PassParameters->TempLacunarity = TempLacunarity;
    PassParameters->TempGain = TempGain;
    PassParameters->TempWarpStrength = TempWarpStrength;
    PassParameters->TempSeedOffset = TempSeedOffset;

    PassParameters->MoistBaseFreq = MoistBaseFreq;
    PassParameters->MoistOctaves = MoistOctaves;
    PassParameters->MoistLacunarity = MoistLacunarity;
    PassParameters->MoistGain = MoistGain;
    PassParameters->MoistWarpStrength = MoistWarpStrength;
    PassParameters->MoistSeedOffset = MoistSeedOffset;

    // Biome array for per-column selection
    PassParameters->Biomes = BiomeSRV;
    PassParameters->BiomeCount = BiomeCount;

    // DIAGNOSTIC: Log biome count
    UE_LOG(LogTemp, Warning, TEXT("GPU Generation: Using %d biomes for per-column selection"), BiomeCount);

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
