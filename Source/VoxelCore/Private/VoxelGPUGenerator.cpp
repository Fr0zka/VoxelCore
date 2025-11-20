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

        // Output buffers
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, OutCategories)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, OutBiomeGrid)  // OPTION B: BiomeGrid data
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
            if (!Job.IsValid() || !Job->CategoryReadback || !Job->BiomeGridReadback)
            {
                PendingJobs.RemoveAtSwap(Index);
                continue;
            }

            // Check if BOTH readbacks are ready (non-blocking)
            if (!Job->CategoryReadback->IsReady() || !Job->BiomeGridReadback->IsReady())
            {
                continue; // Not ready yet - check next frame
            }

            // Both readbacks ready - move to completed list
            PendingJobs.RemoveAtSwap(Index);
            CompletedJobs.Add(Job);
        }
    }

    // Process completed jobs (lock and copy data on render thread)
    // IMPORTANT: Jobs in CompletedJobs have already passed IsReady() check,
    // so they are safe to lock on the render thread
    // OPTION B: Now processes category and BiomeGrid buffers
    for (TSharedPtr<FGPUGenerationJob, ESPMode::ThreadSafe>& Job : CompletedJobs)
    {
        if (!Job.IsValid())
            continue;

        // Capture job data for async processing
        TFunction<void(TArray<uint8>&&, FBiomeGrid2D&&)> OnComplete = Job->OnComplete;
        int32 CategoryBufferSizeBytes = Job->CategoryBufferSizeBytes;
        int32 BiomeGridBufferSizeBytes = Job->BiomeGridBufferSizeBytes;
        int32 BiomeGridSizeX = Job->BiomeGridSizeX;
        int32 BiomeGridSizeY = Job->BiomeGridSizeY;
        const UVoxelBiomeTable* BiomeTable = Job->BiomeTable;

        // CRITICAL: Move ownership of readbacks to render thread command
        // This keeps the readbacks alive until the command executes
        FRHIGPUBufferReadback* CategoryReadbackPtr = Job->CategoryReadback.Release();
        FRHIGPUBufferReadback* BiomeGridReadbackPtr = Job->BiomeGridReadback.Release();

        // Enqueue render command to lock and copy data (Lock MUST be on render thread)
        ENQUEUE_RENDER_COMMAND(VoxelGeneration_ReadbackData)(
            [CategoryReadbackPtr, BiomeGridReadbackPtr, CategoryBufferSizeBytes, BiomeGridBufferSizeBytes,
             BiomeGridSizeX, BiomeGridSizeY, BiomeTable, OnComplete](FRHICommandListImmediate& RHICmdList)
            {
                // Allocate output buffers on render thread
                TArray<uint8> CategoryData;
                FBiomeGrid2D BiomeGridData;
                CategoryData.SetNumUninitialized(CategoryBufferSizeBytes);
                BiomeGridData.Init(BiomeGridSizeX, BiomeGridSizeY);

                // Lock and copy category data
                const uint32* CategoryPtr = (const uint32*)CategoryReadbackPtr->Lock(CategoryBufferSizeBytes);
                bool bCategorySuccess = false;
                if (CategoryPtr)
                {
                    FMemory::Memcpy(CategoryData.GetData(), CategoryPtr, CategoryBufferSizeBytes);
                    CategoryReadbackPtr->Unlock();
                    bCategorySuccess = true;
                }

                // Lock and unpack BiomeGrid data
                const uint32* BiomeGridPtr = (const uint32*)BiomeGridReadbackPtr->Lock(BiomeGridBufferSizeBytes);
                bool bBiomeGridSuccess = false;
                if (BiomeGridPtr)
                {
                    // Unpack BiomeGrid: packed format per column
                    // Lower 16 bits = SurfaceZWorld (int16), upper 16 bits = BiomeIndex (uint8)
                    const int32 NumColumns = BiomeGridSizeX * BiomeGridSizeY;

                    // DIAGNOSTIC: Track unique biome indices and sample corners
                    TSet<uint8> UniqueBiomes;
                    TMap<uint8, int32> BiomeCount; // Count how many columns per biome
                    int32 MinSurfaceZ = INT32_MAX;
                    int32 MaxSurfaceZ = INT32_MIN;

                    // Sample corner biome indices for detailed logging
                    TArray<uint8> CornerBiomes;

                    for (int32 i = 0; i < NumColumns; ++i)
                    {
                        const uint32 PackedValue = BiomeGridPtr[i];

                        // Unpack SurfaceZWorld (lower 16 bits, signed)
                        const int16 SurfaceZ = (int16)(PackedValue & 0xFFFF);
                        BiomeGridData.SurfaceZWorld[i] = SurfaceZ;

                        MinSurfaceZ = FMath::Min(MinSurfaceZ, (int32)SurfaceZ);
                        MaxSurfaceZ = FMath::Max(MaxSurfaceZ, (int32)SurfaceZ);

                        // Unpack BiomeIndex (upper 16 bits, actually only 8 bits used)
                        const uint8 BiomeIndex = (uint8)((PackedValue >> 16) & 0xFF);
                        UniqueBiomes.Add(BiomeIndex);
                        BiomeCount.FindOrAdd(BiomeIndex)++;

                        // Sample corners (0,0), (SizeX-1,0), (0,SizeY-1), (SizeX-1,SizeY-1), center
                        int32 x = i % BiomeGridSizeX;
                        int32 y = i / BiomeGridSizeX;
                        if ((x == 0 && y == 0) ||
                            (x == BiomeGridSizeX-1 && y == 0) ||
                            (x == 0 && y == BiomeGridSizeY-1) ||
                            (x == BiomeGridSizeX-1 && y == BiomeGridSizeY-1) ||
                            (x == BiomeGridSizeX/2 && y == BiomeGridSizeY/2))
                        {
                            CornerBiomes.Add(BiomeIndex);
                        }

                        // Map BiomeIndex back to UVoxelBiomeDef* pointer
                        if (BiomeTable && BiomeIndex < (uint8)BiomeTable->Biomes.Num())
                        {
                            BiomeGridData.BiomeAtXY[i] = BiomeTable->Biomes[BiomeIndex];
                        }
                        else
                        {
                            BiomeGridData.BiomeAtXY[i] = nullptr;
                        }
                    }
                    BiomeGridReadbackPtr->Unlock();
                    bBiomeGridSuccess = true;
                }

                // Call completion callback on game thread (async task)
                if (bCategorySuccess && bBiomeGridSuccess && OnComplete)
                {
                    AsyncTask(ENamedThreads::GameThread, [OnComplete, CategoryData = MoveTemp(CategoryData), BiomeGridData = MoveTemp(BiomeGridData)]() mutable
                    {
                        OnComplete(MoveTemp(CategoryData), MoveTemp(BiomeGridData));
                    });
                }
                else
                {
                    // Lock failed - log error
                    UE_LOG(LogTemp, Error, TEXT("VoxelGPU: Failed to lock readback buffers! Category=%d BiomeGrid=%d"), bCategorySuccess, bBiomeGridSuccess);
                }

                // Clean up readback buffers (we own them now)
                delete CategoryReadbackPtr;
                delete BiomeGridReadbackPtr;
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
    TFunction<void(TArray<uint8>&&, FBiomeGrid2D&&)> OnComplete)  // OPTION B: Updated signature
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
            Data.CaveThreshold = P.CaveThreshold;
            Data.CaveChamberFrequency = P.CaveChamberFrequency;
            Data.CaveChamberOctaves = P.CaveChamberOctaves;
            Data.CaveTunnelFrequency = P.CaveTunnelFrequency;
            Data.CaveTunnelOctaves = P.CaveTunnelOctaves;
            Data.CaveWarpFrequency = P.CaveWarpFrequency;
            Data.CaveWarpAmplitude = P.CaveWarpAmplitude;
            Data.CaveLacunarity = P.CaveLacunarity;
            Data.CaveGain = P.CaveGain;

            // OPTION B: No block data packing needed (blocks generated on CPU)
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

        // OPTION B: No block data needed (blocks generated on CPU)
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
         BiomeDataArray, BiomeTable, OnComplete](FRHICommandListImmediate& RHICmdList)
        {
            DispatchGenerationShader_RenderThread(Coord, SizeX, SizeY, SizeZ, BaseSizeX, BaseSizeY, BaseSizeZ, LODScaleXY, Seed, BaseHeight, WaterLevel, MaxCaveDepth,
                TempBaseFreq, TempOctaves, TempLacunarity, TempGain, TempWarpStrength, TempSeedOffset,
                MoistBaseFreq, MoistOctaves, MoistLacunarity, MoistGain, MoistWarpStrength, MoistSeedOffset,
                BiomeDataArray, BiomeTable, OnComplete);
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
    const UVoxelBiomeTable* BiomeTable,
    TFunction<void(TArray<uint8>&&, FBiomeGrid2D&&)> OnComplete)  // OPTION B: Updated
{
    check(IsInRenderingThread());

    FRDGBuilder GraphBuilder(FRHICommandListExecutor::GetImmediateCommandList());

    // Calculate buffer sizes
    const int32 TotalVoxels = SizeX * SizeY * SizeZ;

    // Categories are packed 2 bits per voxel, so we need (SizeX * SizeY * SizeZ * 2) bits
    // which equals (SizeX * SizeY * SizeZ + 3) / 4 bytes, rounded up to uint32s
    const int32 CategoryBufferSizeBytes = FMath::DivideAndRoundUp(TotalVoxels * 2, 32) * 4; // Round up to uint32s
    const int32 CategoryBufferSizeUints = CategoryBufferSizeBytes / 4;

    // OPTION B: BiomeGrid is one entry per XY column (not per voxel)
    // Each entry is one uint32: lower 16 bits = SurfaceZWorld, upper 16 bits = BiomeIndex
    const int32 BiomeGridSize = SizeX * SizeY;
    const int32 BiomeGridBufferSizeBytes = BiomeGridSize * sizeof(uint32);
    const int32 BiomeGridBufferSizeUints = BiomeGridSize;

    // Create category output buffer (RDG managed)
    FRDGBufferRef CategoryBuffer = GraphBuilder.CreateBuffer(
        FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), CategoryBufferSizeUints),
        TEXT("VoxelGenerationCategories"));

    FRDGBufferUAVRef CategoryUAV = GraphBuilder.CreateUAV(CategoryBuffer, PF_R32_UINT);

    // OPTION B: Create BiomeGrid output buffer (RDG managed)
    FRDGBufferRef BiomeGridBuffer = GraphBuilder.CreateBuffer(
        FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), BiomeGridBufferSizeUints),
        TEXT("VoxelGenerationBiomeGrid"));

    FRDGBufferUAVRef BiomeGridUAV = GraphBuilder.CreateUAV(BiomeGridBuffer, PF_R32_UINT);

    // CRITICAL: Clear output buffers to zero (prevents garbage data)
    AddClearUAVPass(GraphBuilder, CategoryUAV, 0u);
    AddClearUAVPass(GraphBuilder, BiomeGridUAV, 0u);

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

    // Output buffers (OPTION B: Categories and BiomeGrid)
    PassParameters->OutCategories = CategoryUAV;
    PassParameters->OutBiomeGrid = BiomeGridUAV;

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

    // OPTION B: Setup async GPU readbacks for BOTH buffers
    FRHIGPUBufferReadback* CategoryReadback = new FRHIGPUBufferReadback(TEXT("VoxelCategoryReadback"));
    FRHIGPUBufferReadback* BiomeGridReadback = new FRHIGPUBufferReadback(TEXT("VoxelBiomeGridReadback"));

    // Enqueue category readback
    AddReadbackBufferPass(GraphBuilder, RDG_EVENT_NAME("ReadbackVoxelCategories"), CategoryBuffer,
        [CategoryReadback, CategoryBuffer](FRHICommandList& RHICmdList)
        {
            CategoryReadback->EnqueueCopy(RHICmdList, CategoryBuffer->GetRHI(), 0u);
        });

    // Enqueue BiomeGrid readback
    AddReadbackBufferPass(GraphBuilder, RDG_EVENT_NAME("ReadbackVoxelBiomeGrid"), BiomeGridBuffer,
        [BiomeGridReadback, BiomeGridBuffer](FRHICommandList& RHICmdList)
        {
            BiomeGridReadback->EnqueueCopy(RHICmdList, BiomeGridBuffer->GetRHI(), 0u);
        });

    GraphBuilder.Execute();

    // OPTION B: Create job to track async readbacks (polled each frame, non-blocking)
    TSharedPtr<FGPUGenerationJob, ESPMode::ThreadSafe> Job = MakeShared<FGPUGenerationJob, ESPMode::ThreadSafe>();
    Job->CategoryReadback = TUniquePtr<FRHIGPUBufferReadback>(CategoryReadback);
    Job->BiomeGridReadback = TUniquePtr<FRHIGPUBufferReadback>(BiomeGridReadback);
    Job->CategoryBufferSizeBytes = CategoryBufferSizeBytes;
    Job->BiomeGridBufferSizeBytes = BiomeGridBufferSizeBytes;
    Job->BiomeGridSizeX = SizeX;
    Job->BiomeGridSizeY = SizeY;
    Job->BiomeTable = BiomeTable;
    Job->OnComplete = OnComplete;

    // Add to pending jobs list (will be polled by TickGPUGenerationJobs)
    {
        FScopeLock Lock(&JobsMutex);
        PendingJobs.Add(Job);
    }
}
