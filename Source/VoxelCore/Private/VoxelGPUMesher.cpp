#include "VoxelGPUMesher.h"
#include "VoxelStructs.h"
#include "VoxelMesher.h"
#include "RenderCore.h"
#include "RHI.h"
#include "RHIResources.h"
#include "RHIGPUReadback.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "ShaderParameterStruct.h"
#include "GlobalShader.h"
#include "RenderingThread.h"
#include "Async/Async.h"
#include "HAL/PlatformProcess.h"
#include "HAL/IConsoleManager.h"
#include "Templates/SharedPointer.h"
#include "HAL/CriticalSection.h"

// ============================================================================
// COMPUTE SHADER WRAPPERS
// ============================================================================
class FGPUCountElementsCS : public FGlobalShader
{
    DECLARE_GLOBAL_SHADER(FGPUCountElementsCS);
    SHADER_USE_PARAMETER_STRUCT(FGPUCountElementsCS, FGlobalShader);

public:
    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        SHADER_PARAMETER(uint32, SizeX)
        SHADER_PARAMETER(uint32, SizeY)
        SHADER_PARAMETER(uint32, SizeZ)
        SHADER_PARAMETER(uint32, XYScale)
        SHADER_PARAMETER(uint32, DefaultAO)
        SHADER_PARAMETER(uint32, bHasNeighborXN)
        SHADER_PARAMETER(uint32, bHasNeighborXP)
        SHADER_PARAMETER(uint32, bHasNeighborYN)
        SHADER_PARAMETER(uint32, bHasNeighborYP)
        SHADER_PARAMETER(uint32, bHasNeighborZN)
        SHADER_PARAMETER(uint32, bHasNeighborZP)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, InVoxels)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, NeighborXN)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, NeighborXP)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, NeighborYN)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, NeighborYP)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, NeighborZN)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, NeighborZP)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, OutCount)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, OutVerts)
        SHADER_PARAMETER(uint32, MaxVerts)
        SHADER_PARAMETER(uint32, DisableGreedyMerge)
        SHADER_PARAMETER(uint32, DirectionMask)
        SHADER_PARAMETER(uint32, IgnoreNeighbors)
        SHADER_PARAMETER(uint32, NeighborLayoutX)
        SHADER_PARAMETER(uint32, NeighborLayoutY)
        SHADER_PARAMETER(uint32, NeighborLayoutZ)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, Offsets)
    END_SHADER_PARAMETER_STRUCT()

    static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters&) { return true; }
};

IMPLEMENT_GLOBAL_SHADER(FGPUCountElementsCS, "/Plugin/VoxelCore/GPUGreedyMesher_Optimized.usf", "CountElementsCS", SF_Compute);

class FGPUEmitElementsCS : public FGlobalShader
{
    DECLARE_GLOBAL_SHADER(FGPUEmitElementsCS);
    SHADER_USE_PARAMETER_STRUCT(FGPUEmitElementsCS, FGlobalShader);
public:
    using FParameters = FGPUCountElementsCS::FParameters;
    static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters&) { return true; }
};

IMPLEMENT_GLOBAL_SHADER(FGPUEmitElementsCS, "/Plugin/VoxelCore/GPUGreedyMesher_Optimized.usf", "EmitElementsCS", SF_Compute);

// ============================================================================
// NEIGHBOR DATA STRUCTURES
// ============================================================================
struct FGPUNeighborArrays
{
    TArray<uint32> XN;
    TArray<uint32> XP;
    TArray<uint32> YN;
    TArray<uint32> YP;
    TArray<uint32> ZN;
    TArray<uint32> ZP;
};

static void CopyNeighborArrays(const FGPUMeshBuildParams& P, FGPUNeighborArrays& Out)
{
    //ehehehe
    if (P.bHasNeighborXN && P.NeighborXN)
    {
        const int32 Count = P.SizeY * P.SizeZ;
        Out.XN.SetNumUninitialized(Count);
        int32 solidCount = 0;
        for (int32 i = 0; i < Count; ++i)
        {
            Out.XN[i] = (uint32)P.NeighborXN[i];
            if ((uint8)P.NeighborXN[i] != 0) solidCount++;
        }
        UE_LOG(LogTemp, Warning, TEXT("[VoxelGPU Copy] XN: Count=%d, Solid=%d/%d, HasNeighbors: XN=%d XP=%d YN=%d YP=%d ZN=%d ZP=%d"),
            Count, solidCount, Count,
            P.bHasNeighborXN ? 1 : 0, P.bHasNeighborXP ? 1 : 0,
            P.bHasNeighborYN ? 1 : 0, P.bHasNeighborYP ? 1 : 0,
            P.bHasNeighborZN ? 1 : 0, P.bHasNeighborZP ? 1 : 0);
    }
    if (P.bHasNeighborXP && P.NeighborXP)
    {
        const int32 Count = P.SizeY * P.SizeZ;
        Out.XP.SetNumUninitialized(Count);
        for (int32 i = 0; i < Count; ++i)
        {
            Out.XP[i] = (uint32)P.NeighborXP[i];
        }
    }
    if (P.bHasNeighborYN && P.NeighborYN)
    {
        const int32 Count = P.SizeX * P.SizeZ;
        Out.YN.SetNumUninitialized(Count);
        for (int32 i = 0; i < Count; ++i)
        {
            Out.YN[i] = (uint32)P.NeighborYN[i];
        }
    }
    if (P.bHasNeighborYP && P.NeighborYP)
    {
        const int32 Count = P.SizeX * P.SizeZ;
        Out.YP.SetNumUninitialized(Count);
        for (int32 i = 0; i < Count; ++i)
        {
            Out.YP[i] = (uint32)P.NeighborYP[i];
        }
    }
    if (P.bHasNeighborZN && P.NeighborZN)
    {
        const int32 Count = P.SizeX * P.SizeY;
        Out.ZN.SetNumUninitialized(Count);
        for (int32 i = 0; i < Count; ++i)
        {
            Out.ZN[i] = (uint32)P.NeighborZN[i];
        }
    }
    if (P.bHasNeighborZP && P.NeighborZP)
    {
        const int32 Count = P.SizeX * P.SizeY;
        Out.ZP.SetNumUninitialized(Count);
        for (int32 i = 0; i < Count; ++i)
        {
            Out.ZP[i] = (uint32)P.NeighborZP[i];
        }
    }
}

// ============================================================================
// ASYNC JOB STRUCTURES
// ============================================================================
enum class EVoxelGPUAsyncStage : uint8
{
    WaitingForCounts,
    WaitingForVertices
};

struct FVoxelGPUAsyncJob : public TSharedFromThis<FVoxelGPUAsyncJob, ESPMode::ThreadSafe>
{
    TSharedPtr<TArray<uint32>, ESPMode::ThreadSafe> Voxels;
    TSharedPtr<FGPUNeighborArrays, ESPMode::ThreadSafe> Neighbors;
    TUniquePtr<FRHIGPUBufferReadback> CountReadback;
    TUniquePtr<FRHIGPUBufferReadback> VertsReadback;
    TArray<uint32> CountsCPU;
    TArray<uint32> OffsetsCPU;
    TArray<uint32> PackedResult;
    int32 SizeX = 0;
    int32 SizeY = 0;
    int32 SizeZ = 0;
    int32 XYScale = 1;
    uint8 DefaultAO = 3;
    int32 VolCount = 0;
    int32 TotalElements = 0;
    int32 MaxOutputVerts = INT32_MAX;
    bool bAggressiveCulling = false;
    TFunction<void(bool, TArray<uint32>&&)> Completion;
    EVoxelGPUAsyncStage Stage = EVoxelGPUAsyncStage::WaitingForCounts;
    bool bSuccess = true;
};

static FCriticalSection GVoxelGPUAsyncMutex;
static TArray<TSharedPtr<FVoxelGPUAsyncJob, ESPMode::ThreadSafe>> GVoxelGPUAsyncJobs;

// ============================================================================
// CONSOLE VARIABLES
// ============================================================================
static TAutoConsoleVariable<int32> CVarVoxelGPU_LogPerChunk(
    TEXT("r.Voxel.GPU.LogPerChunk"),
    0,
    TEXT("Log GPU/CPU mesher usage per chunk with tri/vert counts."),
    ECVF_Default);

static TAutoConsoleVariable<int32> CVarVoxelGPU_DisableGreedyMerge(
    TEXT("r.Voxel.GPU.DisableGreedyMerge"),
    0,
    TEXT("Bypass greedy merging in shader and emit 1x1 quads per mask cell."),
    ECVF_Default);

static TAutoConsoleVariable<int32> CVarVoxelGPU_IgnoreNeighbors(
    TEXT("r.Voxel.GPU.IgnoreNeighbors"),
    0,
    TEXT("Ignore neighbor borders for culling (treat all outside as air)."),
    ECVF_Default);

static TAutoConsoleVariable<int32> CVarVoxelGPU_CompareCPU(
    TEXT("r.Voxel.GPU.CompareCPU"),
    0,
    TEXT("When 1, builds CPU greedy mesh alongside GPU and logs triangle counts."),
    ECVF_Default);

// ============================================================================
// TWO-PASS GPU MESHING (Count -> CPU scan -> Emit)
// ============================================================================
static bool BuildPackedVerts_GPU_TwoPass(const FGPUMeshBuildParams& P, TArray<uint32>& OutPackedVerts)
{
    OutPackedVerts.Reset();
    if (!GDynamicRHI) return false;
    if (!P.Voxels || P.SizeX <= 0 || P.SizeY <= 0 || P.SizeZ <= 0) return false;

    const int32 VolCount = P.SizeX * P.SizeY * P.SizeZ;

    // Stage voxel categories to uint32
    TArray<uint32> VoxCPU;
    VoxCPU.SetNumUninitialized(VolCount);
    for (int32 i = 0; i < VolCount; ++i)
    {
        VoxCPU[i] = (uint32)P.Voxels[i];
    }

    TSharedRef<FGPUNeighborArrays, ESPMode::ThreadSafe> NeighborData = MakeShared<FGPUNeighborArrays, ESPMode::ThreadSafe>();
    CopyNeighborArrays(P, NeighborData.Get());

    // Readbacks
    TUniquePtr<FRHIGPUBufferReadback> CountReadback = MakeUnique<FRHIGPUBufferReadback>(TEXT("VoxelGPU_Counts"));
    TUniquePtr<FRHIGPUBufferReadback> VertsReadback = MakeUnique<FRHIGPUBufferReadback>(TEXT("VoxelGPU_Verts"));

    // Pass 1: Count elements per voxel
    TArray<uint32> CountsCPU;
    CountsCPU.SetNumUninitialized(VolCount);

    FEvent* CountDone = FPlatformProcess::GetSynchEventFromPool(false);

    ENQUEUE_RENDER_COMMAND(VoxelGPU_TwoPass_Count)([P, Vox = MoveTemp(VoxCPU), Neighbors = NeighborData, CountRB = CountReadback.Get()](FRHICommandListImmediate& RHICmdList)
        {
            FRDGBuilder GraphBuilder(RHICmdList);
            const int32 VolCountRT = P.SizeX * P.SizeY * P.SizeZ;

            FRDGBufferRef VoxBuf = CreateStructuredBuffer(GraphBuilder, TEXT("VoxelGPU.InVoxels"),
                sizeof(uint32), Vox.Num(), Vox.GetData(), Vox.Num() * sizeof(uint32), ERDGInitialDataFlags::None);
            FRDGBufferSRVRef VoxSRV = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(VoxBuf));

            FRDGBufferRef CountsBuf = CreateStructuredBuffer(GraphBuilder, TEXT("VoxelGPU.Counts"),
                sizeof(uint32), VolCountRT, nullptr, 0, ERDGInitialDataFlags::None);
            FRDGBufferUAVRef CountsUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(CountsBuf, PF_R32_UINT));
            AddClearUAVPass(GraphBuilder, CountsUAV, 0u);

            auto CreateNeighborBuffer = [&](const TCHAR* Name, const TArray<uint32>& Data, bool bHasData) -> FRDGBufferSRVRef
                {
                    if (bHasData && Data.Num() > 0)
                    {
                        FRDGBufferRef Buf = CreateStructuredBuffer(GraphBuilder, Name, sizeof(uint32), Data.Num(),
                            Data.GetData(), Data.Num() * sizeof(uint32), ERDGInitialDataFlags::None);
                        return GraphBuilder.CreateSRV(FRDGBufferSRVDesc(Buf));
                    }
                    FRDGBufferRef Buf = CreateStructuredBuffer(GraphBuilder, Name, sizeof(uint32), 1, nullptr, 0, ERDGInitialDataFlags::None);
                    FRDGBufferUAVRef BufUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Buf));
                    AddClearUAVPass(GraphBuilder, BufUAV, 0u);
                    return GraphBuilder.CreateSRV(FRDGBufferSRVDesc(Buf));
                };

            FRDGBufferSRVRef NeighborXNSRV = CreateNeighborBuffer(TEXT("VoxelGPU.NeighborXN"), Neighbors->XN, P.bHasNeighborXN);
            FRDGBufferSRVRef NeighborXPSRV = CreateNeighborBuffer(TEXT("VoxelGPU.NeighborXP"), Neighbors->XP, P.bHasNeighborXP);
            FRDGBufferSRVRef NeighborYNSRV = CreateNeighborBuffer(TEXT("VoxelGPU.NeighborYN"), Neighbors->YN, P.bHasNeighborYN);
            FRDGBufferSRVRef NeighborYPSRV = CreateNeighborBuffer(TEXT("VoxelGPU.NeighborYP"), Neighbors->YP, P.bHasNeighborYP);
            FRDGBufferSRVRef NeighborZNSRV = CreateNeighborBuffer(TEXT("VoxelGPU.NeighborZN"), Neighbors->ZN, P.bHasNeighborZN);
            FRDGBufferSRVRef NeighborZPSRV = CreateNeighborBuffer(TEXT("VoxelGPU.NeighborZP"), Neighbors->ZP, P.bHasNeighborZP);

            FRDGBufferRef DummyOutBuf = CreateStructuredBuffer(GraphBuilder, TEXT("VoxelGPU.CountDummyOut"), sizeof(uint32), 1, nullptr, 0, ERDGInitialDataFlags::None);
            FRDGBufferUAVRef DummyOutUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(DummyOutBuf));
            AddClearUAVPass(GraphBuilder, DummyOutUAV, 0u);

            FRDGBufferRef DummyOffsetsBuf = CreateStructuredBuffer(GraphBuilder, TEXT("VoxelGPU.CountDummyOffsets"), sizeof(uint32), 1, nullptr, 0, ERDGInitialDataFlags::None);
            FRDGBufferSRVRef DummyOffsetsSRV = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(DummyOffsetsBuf));

            TShaderMapRef<FGPUCountElementsCS> CS(GetGlobalShaderMap(GMaxRHIFeatureLevel));
            FGPUCountElementsCS::FParameters* Params = GraphBuilder.AllocParameters<FGPUCountElementsCS::FParameters>();
            Params->SizeX = (uint32)P.SizeX;
            Params->SizeY = (uint32)P.SizeY;
            Params->SizeZ = (uint32)P.SizeZ;
            Params->XYScale = (uint32)FMath::Max(1, P.XYScale);
            Params->DefaultAO = (uint32)P.DefaultAO;
            Params->InVoxels = VoxSRV;
            Params->OutCount = CountsUAV;
            Params->NeighborXN = NeighborXNSRV;
            Params->NeighborXP = NeighborXPSRV;
            Params->NeighborYN = NeighborYNSRV;
            Params->NeighborYP = NeighborYPSRV;
            Params->NeighborZN = NeighborZNSRV;
            Params->NeighborZP = NeighborZPSRV;
            Params->bHasNeighborXN = P.bHasNeighborXN ? 1u : 0u;
            Params->bHasNeighborXP = P.bHasNeighborXP ? 1u : 0u;
            Params->bHasNeighborYN = P.bHasNeighborYN ? 1u : 0u;
            Params->bHasNeighborYP = P.bHasNeighborYP ? 1u : 0u;
            Params->bHasNeighborZN = P.bHasNeighborZN ? 1u : 0u;
            Params->bHasNeighborZP = P.bHasNeighborZP ? 1u : 0u;
            Params->OutVerts = DummyOutUAV;
            Params->MaxVerts = 0u;
            Params->DisableGreedyMerge = 0u;

            static auto* CVarIgnNbh = IConsoleManager::Get().FindConsoleVariable(TEXT("r.Voxel.GPU.IgnoreNeighbors"));
            const bool bIgnoreNeighbors = P.bAggressiveCulling || (CVarIgnNbh && CVarIgnNbh->GetInt() != 0);
            Params->IgnoreNeighbors = bIgnoreNeighbors ? 1u : 0u;

            // Debug: Log neighbor usage
            static bool bLoggedOnce = false;
            if (!bLoggedOnce)
            {
                bLoggedOnce = true;
                UE_LOG(LogTemp, Warning, TEXT("[VoxelGPU Count] IgnoreNeighbors=%d, bAggressiveCulling=%d, Neighbors: XN=%d XP=%d YN=%d YP=%d ZN=%d ZP=%d"),
                    Params->IgnoreNeighbors, P.bAggressiveCulling ? 1 : 0,
                    P.bHasNeighborXN ? 1 : 0, P.bHasNeighborXP ? 1 : 0,
                    P.bHasNeighborYN ? 1 : 0, P.bHasNeighborYP ? 1 : 0,
                    P.bHasNeighborZN ? 1 : 0, P.bHasNeighborZP ? 1 : 0);
            }

            Params->NeighborLayoutX = 0u;
            Params->NeighborLayoutY = 0u;
            Params->NeighborLayoutZ = 0u;
            Params->DirectionMask = 0u;
            Params->Offsets = DummyOffsetsSRV;

            const FIntVector Groups(
                FMath::DivideAndRoundUp(P.SizeX, 8),
                FMath::DivideAndRoundUp(P.SizeY, 8),
                FMath::DivideAndRoundUp(P.SizeZ, 8));

            FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("VoxelGPU Count"), CS, Params, Groups);
            AddEnqueueCopyPass(GraphBuilder, CountRB, CountsBuf, 0);
            GraphBuilder.Execute();
        });

    ENQUEUE_RENDER_COMMAND(VoxelGPU_TwoPass_CountReadback)([CountRB = CountReadback.Get(), CountsPtr = CountsCPU.GetData(), VolCount, CountDone](FRHICommandListImmediate& RHICmdList)
        {
            RHICmdList.SubmitCommandsAndFlushGPU();
            RHICmdList.BlockUntilGPUIdle();
            const void* Ptr = CountRB->Lock((int64)VolCount * sizeof(uint32));
            if (Ptr)
            {
                FMemory::Memcpy(CountsPtr, Ptr, VolCount * sizeof(uint32));
            }
            CountRB->Unlock();
            CountDone->Trigger();
        });

    CountDone->Wait();
    FPlatformProcess::ReturnSynchEventToPool(CountDone);
    CountReadback.Reset();

    if (CountsCPU.Num() != VolCount)
    {
        return false;
    }

    // CPU prefix scan
    const uint64 MaxSupportedElems = (P.MaxOutputVerts > 0) ? (uint64)P.MaxOutputVerts : (uint64)INT32_MAX;
    uint64 TotalElems64 = 0;
    bool bOverflow = false;
    TArray<uint32> OffsetsCPU;
    OffsetsCPU.SetNumUninitialized(VolCount);

    for (int32 i = 0; i < VolCount; ++i)
    {
        OffsetsCPU[i] = (uint32)TotalElems64;
        TotalElems64 += CountsCPU[i];
        if (TotalElems64 > MaxSupportedElems)
        {
            bOverflow = true;
            break;
        }
    }

    if (bOverflow)
    {
        UE_LOG(LogTemp, Warning, TEXT("[VoxelGPU] GPU mesher exceeded vertex cap (%llu >= %llu); falling back to CPU."), TotalElems64, MaxSupportedElems);
        return false;
    }

    const int32 TotalElems = (int32)TotalElems64;
    if (TotalElems <= 0)
    {
        return true;
    }

    // Pass 2: Emit elements using offsets
    TArray<uint32> PackedCPU;
    PackedCPU.SetNumUninitialized(TotalElems);

    FEvent* EmitDone = FPlatformProcess::GetSynchEventFromPool(false);

    ENQUEUE_RENDER_COMMAND(VoxelGPU_TwoPass_Emit)([P, Offs = MoveTemp(OffsetsCPU), Neighbors = NeighborData, VertsRB = VertsReadback.Get(), TotalElems](FRHICommandListImmediate& RHICmdList) mutable
        {
            const int32 VolCountRT = P.SizeX * P.SizeY * P.SizeZ;
            TArray<uint32> VoxelsCPU;
            VoxelsCPU.SetNumUninitialized(VolCountRT);
            for (int32 i = 0; i < VolCountRT; ++i)
            {
                VoxelsCPU[i] = (uint32)P.Voxels[i];
            }

            FRDGBuilder GraphBuilder(RHICmdList);

            FRDGBufferRef VoxBuf = CreateStructuredBuffer(GraphBuilder, TEXT("VoxelGPU.InVoxels"),
                sizeof(uint32), VoxelsCPU.Num(), VoxelsCPU.GetData(), VoxelsCPU.Num() * sizeof(uint32), ERDGInitialDataFlags::None);
            FRDGBufferSRVRef VoxSRV = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(VoxBuf));

            FRDGBufferRef OffsBuf = CreateStructuredBuffer(GraphBuilder, TEXT("VoxelGPU.Offsets"),
                sizeof(uint32), Offs.Num(), Offs.GetData(), Offs.Num() * sizeof(uint32), ERDGInitialDataFlags::None);
            FRDGBufferSRVRef OffsSRV = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(OffsBuf));

            FRDGBufferRef OutBuf = CreateStructuredBuffer(GraphBuilder, TEXT("VoxelGPU.OutVerts"),
                sizeof(uint32), TotalElems, nullptr, 0, ERDGInitialDataFlags::None);
            FRDGBufferUAVRef OutUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(OutBuf));
            AddClearUAVPass(GraphBuilder, OutUAV, 0u);

            auto CreateNeighborBuffer = [&](const TCHAR* Name, const TArray<uint32>& Data, bool bHasData) -> FRDGBufferSRVRef
                {
                    if (bHasData && Data.Num() > 0)
                    {
                        FRDGBufferRef Buf = CreateStructuredBuffer(GraphBuilder, Name, sizeof(uint32), Data.Num(),
                            Data.GetData(), Data.Num() * sizeof(uint32), ERDGInitialDataFlags::None);
                        return GraphBuilder.CreateSRV(FRDGBufferSRVDesc(Buf));
                    }
                    FRDGBufferRef Buf = CreateStructuredBuffer(GraphBuilder, Name, sizeof(uint32), 1, nullptr, 0, ERDGInitialDataFlags::None);
                    FRDGBufferUAVRef BufUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Buf));
                    AddClearUAVPass(GraphBuilder, BufUAV, 0u);
                    return GraphBuilder.CreateSRV(FRDGBufferSRVDesc(Buf));
                };

            FRDGBufferSRVRef NeighborXNSRV = CreateNeighborBuffer(TEXT("VoxelGPU.NeighborXN"), Neighbors->XN, P.bHasNeighborXN);
            FRDGBufferSRVRef NeighborXPSRV = CreateNeighborBuffer(TEXT("VoxelGPU.NeighborXP"), Neighbors->XP, P.bHasNeighborXP);
            FRDGBufferSRVRef NeighborYNSRV = CreateNeighborBuffer(TEXT("VoxelGPU.NeighborYN"), Neighbors->YN, P.bHasNeighborYN);
            FRDGBufferSRVRef NeighborYPSRV = CreateNeighborBuffer(TEXT("VoxelGPU.NeighborYP"), Neighbors->YP, P.bHasNeighborYP);
            FRDGBufferSRVRef NeighborZNSRV = CreateNeighborBuffer(TEXT("VoxelGPU.NeighborZN"), Neighbors->ZN, P.bHasNeighborZN);
            FRDGBufferSRVRef NeighborZPSRV = CreateNeighborBuffer(TEXT("VoxelGPU.NeighborZP"), Neighbors->ZP, P.bHasNeighborZP);

            FRDGBufferRef DummyCountBuf = CreateStructuredBuffer(GraphBuilder, TEXT("VoxelGPU.EmitDummyCount"), sizeof(uint32), 1, nullptr, 0, ERDGInitialDataFlags::None);
            FRDGBufferUAVRef DummyCountUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(DummyCountBuf, PF_R32_UINT));
            AddClearUAVPass(GraphBuilder, DummyCountUAV, 0u);

            TShaderMapRef<FGPUEmitElementsCS> CS(GetGlobalShaderMap(GMaxRHIFeatureLevel));
            FGPUEmitElementsCS::FParameters* Params = GraphBuilder.AllocParameters<FGPUEmitElementsCS::FParameters>();
            Params->SizeX = (uint32)P.SizeX;
            Params->SizeY = (uint32)P.SizeY;
            Params->SizeZ = (uint32)P.SizeZ;
            Params->XYScale = (uint32)FMath::Max(1, P.XYScale);
            Params->DefaultAO = (uint32)P.DefaultAO;
            Params->InVoxels = VoxSRV;
            Params->Offsets = OffsSRV;
            Params->OutVerts = OutUAV;
            Params->NeighborXN = NeighborXNSRV;
            Params->NeighborXP = NeighborXPSRV;
            Params->NeighborYN = NeighborYNSRV;
            Params->NeighborYP = NeighborYPSRV;
            Params->NeighborZN = NeighborZNSRV;
            Params->NeighborZP = NeighborZPSRV;
            Params->bHasNeighborXN = P.bHasNeighborXN ? 1u : 0u;
            Params->bHasNeighborXP = P.bHasNeighborXP ? 1u : 0u;
            Params->bHasNeighborYN = P.bHasNeighborYN ? 1u : 0u;
            Params->bHasNeighborYP = P.bHasNeighborYP ? 1u : 0u;
            Params->bHasNeighborZN = P.bHasNeighborZN ? 1u : 0u;
            Params->bHasNeighborZP = P.bHasNeighborZP ? 1u : 0u;
            Params->OutCount = DummyCountUAV;
            Params->MaxVerts = (uint32)TotalElems;

            static auto* CVarDisableMerge = IConsoleManager::Get().FindConsoleVariable(TEXT("r.Voxel.GPU.DisableGreedyMerge"));
            Params->DisableGreedyMerge = CVarDisableMerge ? (uint32)CVarDisableMerge->GetInt() : 0u;

            static auto* CVarIgnNbh = IConsoleManager::Get().FindConsoleVariable(TEXT("r.Voxel.GPU.IgnoreNeighbors"));
            const bool bIgnoreNeighbors = P.bAggressiveCulling || (CVarIgnNbh && CVarIgnNbh->GetInt() != 0);
            Params->IgnoreNeighbors = bIgnoreNeighbors ? 1u : 0u;
            Params->NeighborLayoutX = 0u;
            Params->NeighborLayoutY = 0u;
            Params->NeighborLayoutZ = 0u;
            Params->DirectionMask = 0x3Fu;

            const FIntVector Groups(
                FMath::DivideAndRoundUp(P.SizeX, 8),
                FMath::DivideAndRoundUp(P.SizeY, 8),
                FMath::DivideAndRoundUp(P.SizeZ, 8));

            FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("VoxelGPU Emit"), CS, Params, Groups);
            AddEnqueueCopyPass(GraphBuilder, VertsRB, OutBuf, 0);
            GraphBuilder.Execute();
        });

    ENQUEUE_RENDER_COMMAND(VoxelGPU_TwoPass_EmitReadback)([VertsRB = VertsReadback.Get(), PackedPtr = PackedCPU.GetData(), TotalElems, EmitDone](FRHICommandListImmediate& RHICmdList)
        {
            RHICmdList.SubmitCommandsAndFlushGPU();
            RHICmdList.BlockUntilGPUIdle();
            const void* Ptr = VertsRB->Lock((int64)TotalElems * sizeof(uint32));
            if (Ptr)
            {
                FMemory::Memcpy(PackedPtr, Ptr, TotalElems * sizeof(uint32));
            }
            VertsRB->Unlock();
            EmitDone->Trigger();
        });

    EmitDone->Wait();
    FPlatformProcess::ReturnSynchEventToPool(EmitDone);
    VertsReadback.Reset();

    OutPackedVerts = MoveTemp(PackedCPU);
    return true;
}

// ============================================================================
// PUBLIC API
// ============================================================================
bool FVoxelGPUMesher::BuildPackedVerts_GPU(const FGPUMeshBuildParams& P, TArray<uint32>& OutPackedVerts)
{
    if (IsInGameThread())
    {
        return BuildPackedVerts_GPU_TwoPass(P, OutPackedVerts);
    }

    // Capture by value or move, not by reference!
    TArray<uint32> LocalOut;
    bool bOk = false;
    FEvent* Done = FPlatformProcess::GetSynchEventFromPool(false);
    FGPUMeshBuildParams PCopy = P; // Copy P by value

    AsyncTask(ENamedThreads::GameThread, [PCopy, Done, &LocalOut, &bOk]()
        {
            bOk = BuildPackedVerts_GPU_TwoPass(PCopy, LocalOut);
            Done->Trigger();
        });

    Done->Wait();
    FPlatformProcess::ReturnSynchEventToPool(Done);

    if (bOk)
    {
        OutPackedVerts = MoveTemp(LocalOut);
    }
    return bOk;
}

bool FVoxelGPUMesher::BuildPackedVerts_GPU_Async(const FGPUMeshBuildParams& P, TFunction<void(bool, TArray<uint32>&&)> Completion)
{
    if (!GDynamicRHI)
    {
        return false;
    }
    if (!Completion)
    {
        return false;
    }
    if (!P.Voxels || P.SizeX <= 0 || P.SizeY <= 0 || P.SizeZ <= 0)
    {
        return false;
    }

    const int32 VolCount = P.SizeX * P.SizeY * P.SizeZ;
    TSharedPtr<FVoxelGPUAsyncJob, ESPMode::ThreadSafe> Job = MakeShared<FVoxelGPUAsyncJob, ESPMode::ThreadSafe>();
    Job->SizeX = P.SizeX;
    Job->SizeY = P.SizeY;
    Job->SizeZ = P.SizeZ;
    Job->XYScale = P.XYScale;
    Job->DefaultAO = P.DefaultAO;
    Job->VolCount = VolCount;
    Job->MaxOutputVerts = P.MaxOutputVerts;
    Job->bAggressiveCulling = P.bAggressiveCulling;
    Job->Completion = MoveTemp(Completion);
    Job->CountsCPU.SetNumUninitialized(VolCount);
    Job->OffsetsCPU.SetNumUninitialized(VolCount);
    Job->Voxels = MakeShared<TArray<uint32>, ESPMode::ThreadSafe>();
    Job->Voxels->SetNumUninitialized(VolCount);
    for (int32 i = 0; i < VolCount; ++i)
    {
        (*Job->Voxels)[i] = (uint32)P.Voxels[i];
    }
    Job->Neighbors = MakeShared<FGPUNeighborArrays, ESPMode::ThreadSafe>();
    CopyNeighborArrays(P, *Job->Neighbors);
    Job->CountReadback = MakeUnique<FRHIGPUBufferReadback>(TEXT("VoxelGPU_CountsAsync"));

    TSharedPtr<FVoxelGPUAsyncJob, ESPMode::ThreadSafe> JobPtr = Job;
    TSharedPtr<TArray<uint32>, ESPMode::ThreadSafe> VoxelsShared = Job->Voxels;
    TSharedPtr<FGPUNeighborArrays, ESPMode::ThreadSafe> NeighborsShared = Job->Neighbors;
    FRHIGPUBufferReadback* CountReadbackPtr = Job->CountReadback.Get();
    const uint32 SizeX = (uint32)Job->SizeX;
    const uint32 SizeY = (uint32)Job->SizeY;
    const uint32 SizeZ = (uint32)Job->SizeZ;
    const uint32 XYScale = (uint32)P.XYScale;
    const uint32 DefaultAO = (uint32)P.DefaultAO;

    ENQUEUE_RENDER_COMMAND(VoxelGPU_AsyncCount)([JobPtr, VoxelsShared, NeighborsShared, CountReadbackPtr, SizeX, SizeY, SizeZ, XYScale, DefaultAO](FRHICommandListImmediate& RHICmdList)
        {
            const uint32 VolCountRT = SizeX * SizeY * SizeZ;

            FRDGBuilder GraphBuilder(RHICmdList);
            FRDGBufferRef VoxBuf = CreateStructuredBuffer(GraphBuilder, TEXT("VoxelGPU.InVoxels"),
                sizeof(uint32), VoxelsShared->Num(), VoxelsShared->GetData(), VoxelsShared->Num() * sizeof(uint32), ERDGInitialDataFlags::None);
            FRDGBufferSRVRef VoxSRV = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(VoxBuf));

            auto CreateNeighborSRV = [&](const TCHAR* Name, const TArray<uint32>& Data) -> FRDGBufferSRVRef
                {
                    if (Data.Num() == 0)
                    {
                        FRDGBufferRef EmptyBuf = CreateStructuredBuffer(GraphBuilder, Name, sizeof(uint32), 1, nullptr, 0, ERDGInitialDataFlags::None);
                        FRDGBufferUAVRef EmptyUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(EmptyBuf));
                        AddClearUAVPass(GraphBuilder, EmptyUAV, 0u);
                        return GraphBuilder.CreateSRV(FRDGBufferSRVDesc(EmptyBuf));
                    }
                    FRDGBufferRef Buf = CreateStructuredBuffer(GraphBuilder, Name, sizeof(uint32), Data.Num(),
                        Data.GetData(), Data.Num() * sizeof(uint32), ERDGInitialDataFlags::None);
                    return GraphBuilder.CreateSRV(FRDGBufferSRVDesc(Buf));
                };

            FRDGBufferSRVRef NeighborXNSRV = CreateNeighborSRV(TEXT("VoxelGPU.NeighborXN"), NeighborsShared->XN);
            FRDGBufferSRVRef NeighborXPSRV = CreateNeighborSRV(TEXT("VoxelGPU.NeighborXP"), NeighborsShared->XP);
            FRDGBufferSRVRef NeighborYNSRV = CreateNeighborSRV(TEXT("VoxelGPU.NeighborYN"), NeighborsShared->YN);
            FRDGBufferSRVRef NeighborYPSRV = CreateNeighborSRV(TEXT("VoxelGPU.NeighborYP"), NeighborsShared->YP);
            FRDGBufferSRVRef NeighborZNSRV = CreateNeighborSRV(TEXT("VoxelGPU.NeighborZN"), NeighborsShared->ZN);
            FRDGBufferSRVRef NeighborZPSRV = CreateNeighborSRV(TEXT("VoxelGPU.NeighborZP"), NeighborsShared->ZP);

            FRDGBufferRef CountsBuf = CreateStructuredBuffer(GraphBuilder, TEXT("VoxelGPU.Counts"),
                sizeof(uint32), VolCountRT, nullptr, 0, ERDGInitialDataFlags::None);
            FRDGBufferUAVRef CountsUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(CountsBuf, PF_R32_UINT));
            AddClearUAVPass(GraphBuilder, CountsUAV, 0u);

            FRDGBufferRef DummyOutBuf = CreateStructuredBuffer(GraphBuilder, TEXT("VoxelGPU.AsyncDummyOut"),
                sizeof(uint32), 1, nullptr, 0, ERDGInitialDataFlags::None);
            FRDGBufferUAVRef DummyOutUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(DummyOutBuf));
            AddClearUAVPass(GraphBuilder, DummyOutUAV, 0u);

            FRDGBufferRef DummyOffsetsBuf = CreateStructuredBuffer(GraphBuilder, TEXT("VoxelGPU.AsyncDummyOffsets"),
                sizeof(uint32), 1, nullptr, 0, ERDGInitialDataFlags::None);
            FRDGBufferSRVRef DummyOffsetsSRV = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(DummyOffsetsBuf));

            TShaderMapRef<FGPUCountElementsCS> CS(GetGlobalShaderMap(GMaxRHIFeatureLevel));
            FGPUCountElementsCS::FParameters* Params = GraphBuilder.AllocParameters<FGPUCountElementsCS::FParameters>();
            Params->SizeX = SizeX;
            Params->SizeY = SizeY;
            Params->SizeZ = SizeZ;
            Params->XYScale = XYScale;
            Params->DefaultAO = DefaultAO;
            Params->InVoxels = VoxSRV;
            Params->OutCount = CountsUAV;
            Params->NeighborXN = NeighborXNSRV;
            Params->NeighborXP = NeighborXPSRV;
            Params->NeighborYN = NeighborYNSRV;
            Params->NeighborYP = NeighborYPSRV;
            Params->NeighborZN = NeighborZNSRV;
            Params->NeighborZP = NeighborZPSRV;
            Params->bHasNeighborXN = NeighborsShared->XN.Num() > 0 ? 1u : 0u;
            Params->bHasNeighborXP = NeighborsShared->XP.Num() > 0 ? 1u : 0u;
            Params->bHasNeighborYN = NeighborsShared->YN.Num() > 0 ? 1u : 0u;
            Params->bHasNeighborYP = NeighborsShared->YP.Num() > 0 ? 1u : 0u;
            Params->bHasNeighborZN = NeighborsShared->ZN.Num() > 0 ? 1u : 0u;
            Params->bHasNeighborZP = NeighborsShared->ZP.Num() > 0 ? 1u : 0u;
            Params->OutVerts = DummyOutUAV;
            Params->MaxVerts = 0u;
            Params->DisableGreedyMerge = 0u;

            static auto* CVarIgnNbh = IConsoleManager::Get().FindConsoleVariable(TEXT("r.Voxel.GPU.IgnoreNeighbors"));
            const bool bIgnoreNeighbors = JobPtr->bAggressiveCulling || (CVarIgnNbh && CVarIgnNbh->GetInt() != 0);
            Params->IgnoreNeighbors = bIgnoreNeighbors ? 1u : 0u;
            Params->NeighborLayoutX = 0u;
            Params->NeighborLayoutY = 0u;
            Params->NeighborLayoutZ = 0u;
            Params->DirectionMask = 0u;
            Params->Offsets = DummyOffsetsSRV;

            const FIntVector Groups(
                FMath::DivideAndRoundUp((int32)SizeX, 8),
                FMath::DivideAndRoundUp((int32)SizeY, 8),
                FMath::DivideAndRoundUp((int32)SizeZ, 8));

            FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("VoxelGPU Count Async"), CS, Params, Groups);
            AddEnqueueCopyPass(GraphBuilder, CountReadbackPtr, CountsBuf, 0);
            GraphBuilder.Execute();
        });

    {
        FScopeLock Lock(&GVoxelGPUAsyncMutex);
        GVoxelGPUAsyncJobs.Add(Job);
    }
    return true;
}

void FVoxelGPUMesher::PumpAsyncReadbacks()
{
    TArray<TSharedPtr<FVoxelGPUAsyncJob, ESPMode::ThreadSafe>> CompletedJobs;

    {
        FScopeLock Lock(&GVoxelGPUAsyncMutex);
        for (int32 Index = GVoxelGPUAsyncJobs.Num() - 1; Index >= 0; --Index)
        {
            TSharedPtr<FVoxelGPUAsyncJob, ESPMode::ThreadSafe> Job = GVoxelGPUAsyncJobs[Index];
            if (!Job.IsValid())
            {
                GVoxelGPUAsyncJobs.RemoveAtSwap(Index);
                continue;
            }

            if (Job->Stage == EVoxelGPUAsyncStage::WaitingForCounts)
            {
                if (!Job->CountReadback || !Job->CountReadback->IsReady())
                {
                    continue;
                }

                FEvent* ReadbackDone = FPlatformProcess::GetSynchEventFromPool(false);
                ENQUEUE_RENDER_COMMAND(VoxelGPU_AsyncCountReadback)(
                    [Job, ReadbackDone](FRHICommandListImmediate& RHICmdList)
                    {
                        if (Job->CountReadback && Job->VolCount > 0 && Job->CountsCPU.Num() == Job->VolCount)
                        {
                            const void* Ptr = Job->CountReadback->Lock((int64)Job->VolCount * sizeof(uint32));
                            if (Ptr)
                            {
                                FMemory::Memcpy(Job->CountsCPU.GetData(), Ptr, (int64)Job->VolCount * sizeof(uint32));
                            }
                            Job->CountReadback->Unlock();
                        }
                        ReadbackDone->Trigger();
                    });
                ReadbackDone->Wait();
                FPlatformProcess::ReturnSynchEventToPool(ReadbackDone);
                Job->CountReadback.Reset();

                uint64 Total = 0;
                for (int32 i = 0; i < Job->VolCount; ++i)
                {
                    Job->OffsetsCPU[i] = (uint32)Total;
                    Total += Job->CountsCPU[i];
                }
                Job->CountsCPU.Reset();

                const uint64 MaxAllowed = (Job->MaxOutputVerts > 0)
                    ? FMath::Min<uint64>((uint64)Job->MaxOutputVerts, (uint64)INT32_MAX)
                    : (uint64)INT32_MAX;

                if (Total == 0)
                {
                    Job->TotalElements = 0;
                    Job->PackedResult.Reset();
                    Job->bSuccess = true;
                    GVoxelGPUAsyncJobs.RemoveAtSwap(Index);
                    CompletedJobs.Add(Job);
                    continue;
                }

                if (Total > MaxAllowed)
                {
                    UE_LOG(LogTemp, Warning, TEXT("[VoxelGPU] Async mesher exceeded vertex cap (%llu >= %llu); falling back to CPU."), Total, MaxAllowed);
                    Job->PackedResult.Reset();
                    Job->bSuccess = false;
                    GVoxelGPUAsyncJobs.RemoveAtSwap(Index);
                    CompletedJobs.Add(Job);
                    continue;
                }

                if (Total > MAX_uint32)
                {
                    Job->PackedResult.Reset();
                    Job->bSuccess = false;
                    GVoxelGPUAsyncJobs.RemoveAtSwap(Index);
                    CompletedJobs.Add(Job);
                    continue;
                }

                Job->TotalElements = (int32)Total;

                Job->VertsReadback = MakeUnique<FRHIGPUBufferReadback>(TEXT("VoxelGPU_VertsAsync"));
                Job->Stage = EVoxelGPUAsyncStage::WaitingForVertices;

                TSharedPtr<FVoxelGPUAsyncJob, ESPMode::ThreadSafe> JobPtr = Job;
                ENQUEUE_RENDER_COMMAND(VoxelGPU_AsyncEmit)([JobPtr](FRHICommandListImmediate& RHICmdList)
                    {
                        FRDGBuilder GraphBuilder(RHICmdList);

                        FRDGBufferRef VoxBuf = CreateStructuredBuffer(GraphBuilder, TEXT("VoxelGPU.InVoxels"),
                            sizeof(uint32), JobPtr->Voxels->Num(), JobPtr->Voxels->GetData(),
                            JobPtr->Voxels->Num() * sizeof(uint32), ERDGInitialDataFlags::None);
                        FRDGBufferSRVRef VoxSRV = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(VoxBuf));

                        auto CreateNeighborSRV = [&](const TCHAR* Name, const TArray<uint32>& Data) -> FRDGBufferSRVRef
                            {
                                if (Data.Num() == 0)
                                {
                                    FRDGBufferRef EmptyBuf = CreateStructuredBuffer(GraphBuilder, Name, sizeof(uint32), 1, nullptr, 0, ERDGInitialDataFlags::None);
                                    FRDGBufferUAVRef EmptyUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(EmptyBuf));
                                    AddClearUAVPass(GraphBuilder, EmptyUAV, 0u);
                                    return GraphBuilder.CreateSRV(FRDGBufferSRVDesc(EmptyBuf));
                                }
                                FRDGBufferRef Buf = CreateStructuredBuffer(GraphBuilder, Name, sizeof(uint32), Data.Num(),
                                    Data.GetData(), Data.Num() * sizeof(uint32), ERDGInitialDataFlags::None);
                                return GraphBuilder.CreateSRV(FRDGBufferSRVDesc(Buf));
                            };

                        FRDGBufferSRVRef NeighborXNSRV = CreateNeighborSRV(TEXT("VoxelGPU.NeighborXN"), JobPtr->Neighbors->XN);
                        FRDGBufferSRVRef NeighborXPSRV = CreateNeighborSRV(TEXT("VoxelGPU.NeighborXP"), JobPtr->Neighbors->XP);
                        FRDGBufferSRVRef NeighborYNSRV = CreateNeighborSRV(TEXT("VoxelGPU.NeighborYN"), JobPtr->Neighbors->YN);
                        FRDGBufferSRVRef NeighborYPSRV = CreateNeighborSRV(TEXT("VoxelGPU.NeighborYP"), JobPtr->Neighbors->YP);
                        FRDGBufferSRVRef NeighborZNSRV = CreateNeighborSRV(TEXT("VoxelGPU.NeighborZN"), JobPtr->Neighbors->ZN);
                        FRDGBufferSRVRef NeighborZPSRV = CreateNeighborSRV(TEXT("VoxelGPU.NeighborZP"), JobPtr->Neighbors->ZP);

                        FRDGBufferRef OffsBuf = CreateStructuredBuffer(GraphBuilder, TEXT("VoxelGPU.Offsets"),
                            sizeof(uint32), JobPtr->OffsetsCPU.Num(), JobPtr->OffsetsCPU.GetData(),
                            JobPtr->OffsetsCPU.Num() * sizeof(uint32), ERDGInitialDataFlags::None);
                        FRDGBufferSRVRef OffsSRV = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(OffsBuf));

                        FRDGBufferRef OutBuf = CreateStructuredBuffer(GraphBuilder, TEXT("VoxelGPU.OutVerts"),
                            sizeof(uint32), JobPtr->TotalElements, nullptr, 0, ERDGInitialDataFlags::None);
                        FRDGBufferUAVRef OutUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(OutBuf));
                        AddClearUAVPass(GraphBuilder, OutUAV, 0u);

                        FRDGBufferRef DummyCountBuf = CreateStructuredBuffer(GraphBuilder, TEXT("VoxelGPU.AsyncDummyCount"),
                            sizeof(uint32), 1, nullptr, 0, ERDGInitialDataFlags::None);
                        FRDGBufferUAVRef DummyCountUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(DummyCountBuf, PF_R32_UINT));
                        AddClearUAVPass(GraphBuilder, DummyCountUAV, 0u);

                        TShaderMapRef<FGPUEmitElementsCS> CS(GetGlobalShaderMap(GMaxRHIFeatureLevel));
                        FGPUEmitElementsCS::FParameters* Params = GraphBuilder.AllocParameters<FGPUEmitElementsCS::FParameters>();
                        Params->SizeX = (uint32)JobPtr->SizeX;
                        Params->SizeY = (uint32)JobPtr->SizeY;
                        Params->SizeZ = (uint32)JobPtr->SizeZ;
                        Params->XYScale = (uint32)JobPtr->XYScale;
                        Params->DefaultAO = (uint32)JobPtr->DefaultAO;
                        Params->InVoxels = VoxSRV;
                        Params->Offsets = OffsSRV;
                        Params->OutVerts = OutUAV;
                        Params->NeighborXN = NeighborXNSRV;
                        Params->NeighborXP = NeighborXPSRV;
                        Params->NeighborYN = NeighborYNSRV;
                        Params->NeighborYP = NeighborYPSRV;
                        Params->NeighborZN = NeighborZNSRV;
                        Params->NeighborZP = NeighborZPSRV;
                        Params->bHasNeighborXN = JobPtr->Neighbors->XN.Num() > 0 ? 1u : 0u;
                        Params->bHasNeighborXP = JobPtr->Neighbors->XP.Num() > 0 ? 1u : 0u;
                        Params->bHasNeighborYN = JobPtr->Neighbors->YN.Num() > 0 ? 1u : 0u;
                        Params->bHasNeighborYP = JobPtr->Neighbors->YP.Num() > 0 ? 1u : 0u;
                        Params->bHasNeighborZN = JobPtr->Neighbors->ZN.Num() > 0 ? 1u : 0u;
                        Params->bHasNeighborZP = JobPtr->Neighbors->ZP.Num() > 0 ? 1u : 0u;
                        Params->OutCount = DummyCountUAV;
                        Params->MaxVerts = (uint32)JobPtr->TotalElements;
                        Params->DisableGreedyMerge = 0u;

                        static auto* CVarIgnNbh = IConsoleManager::Get().FindConsoleVariable(TEXT("r.Voxel.GPU.IgnoreNeighbors"));
                        const bool bIgnoreNeighbors = JobPtr->bAggressiveCulling || (CVarIgnNbh && CVarIgnNbh->GetInt() != 0);
                        Params->IgnoreNeighbors = bIgnoreNeighbors ? 1u : 0u;
                        Params->NeighborLayoutX = 0u;
                        Params->NeighborLayoutY = 0u;
                        Params->NeighborLayoutZ = 0u;
                        Params->DirectionMask = 0x3Fu;

                        const FIntVector Groups(
                            FMath::DivideAndRoundUp(JobPtr->SizeX, 8),
                            FMath::DivideAndRoundUp(JobPtr->SizeY, 8),
                            FMath::DivideAndRoundUp(JobPtr->SizeZ, 8));

                        FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("VoxelGPU Emit Async"), CS, Params, Groups);
                        AddEnqueueCopyPass(GraphBuilder, JobPtr->VertsReadback.Get(), OutBuf, 0);
                        GraphBuilder.Execute();

                        JobPtr->OffsetsCPU.Reset();
                        JobPtr->Voxels.Reset();
                        JobPtr->Neighbors.Reset();
                    });
            }
            else if (Job->Stage == EVoxelGPUAsyncStage::WaitingForVertices)
            {
                if (!Job->VertsReadback || !Job->VertsReadback->IsReady())
                {
                    continue;
                }

                FEvent* ReadbackDone = FPlatformProcess::GetSynchEventFromPool(false);
                ENQUEUE_RENDER_COMMAND(VoxelGPU_AsyncVertsReadback)(
                    [Job, ReadbackDone](FRHICommandListImmediate& RHICmdList)
                    {
                        if (Job->VertsReadback && Job->TotalElements > 0)
                        {
                            const void* Ptr = Job->VertsReadback->Lock((int64)Job->TotalElements * sizeof(uint32));
                            if (Ptr)
                            {
                                Job->PackedResult.SetNumUninitialized(Job->TotalElements);
                                FMemory::Memcpy(Job->PackedResult.GetData(), Ptr, Job->TotalElements * sizeof(uint32));
                            }
                            else
                            {
                                Job->PackedResult.Reset();
                            }
                            Job->VertsReadback->Unlock();
                        }
                        else
                        {
                            Job->PackedResult.Reset();
                        }
                        ReadbackDone->Trigger();
                    });
                ReadbackDone->Wait();
                FPlatformProcess::ReturnSynchEventToPool(ReadbackDone);
                Job->VertsReadback.Reset();

                GVoxelGPUAsyncJobs.RemoveAtSwap(Index);
                CompletedJobs.Add(Job);
            }
        }
    }

    for (TSharedPtr<FVoxelGPUAsyncJob, ESPMode::ThreadSafe>& Job : CompletedJobs)
    {
        if (!Job.IsValid())
        {
            continue;
        }
        if (Job->Completion)
        {
            Job->Completion(Job->bSuccess, MoveTemp(Job->PackedResult));
        }
    }
}

bool FVoxelGPUMesher::HasPendingAsyncReadbacks()
{
    FScopeLock Lock(&GVoxelGPUAsyncMutex);
    return GVoxelGPUAsyncJobs.Num() > 0;
}

// ============================================================================
// DECODE PACKED VERTS TO MESH BUFFERS
// ============================================================================
static FVector FaceNormalFromCode3(uint32 Code)
{
    switch (Code & 7u)
    {
    case 0: return FVector(1, 0, 0);
    case 1: return FVector(-1, 0, 0);
    case 2: return FVector(0, 1, 0);
    case 3: return FVector(0, -1, 0);
    case 4: return FVector(0, 0, 1);
    case 5: return FVector(0, 0, -1);
    default: return FVector(0, 0, 1);
    }
}

void FVoxelGPUMesher::DecodePackedVertsToMeshBuffers(
    const TArray<uint32>& Packed,
    FMeshBuffers& Out,
    float VoxelUU,
    int32 XYScale,
    int32 SizeX,
    int32 SizeY,
    int32 SizeZ)
{
    UE_LOG(LogTemp, Warning, TEXT("[VoxelGPU Decode] Packed=%d, Size=(%d,%d,%d), XYScale=%d"),
        Packed.Num(), SizeX, SizeY, SizeZ, XYScale);

    Out.Vertices.Reset();
    Out.Normals.Reset();
    Out.UVs.Reset();
    Out.Colors.Reset();
    Out.Triangles.Reset();

    const double SX = (double)XYScale * (double)VoxelUU;
    const double SY = (double)XYScale * (double)VoxelUU;
    const double SZ = (double)VoxelUU;

    auto UnpackPos = [](uint32 P) -> FIntVector
        {
            return FIntVector(
                int32((P) & 0x7Fu),
                int32((P >> 7) & 0x7Fu),
                int32((P >> 14) & 0x7Fu));
        };

    auto AOToColor = [](uint32 ao) -> FLinearColor
        {
            const float f = float(ao & 3u) * (1.f / 3.f);
            return FLinearColor(f, f, f, 1.f);
        };

    const int32 Count = Packed.Num() - (Packed.Num() % 3);
    if (Count != Packed.Num())
    {
        UE_LOG(LogTemp, Warning, TEXT("[VoxelGPU] Packed count %d not multiple of 3. Dropping %d."),
            Packed.Num(), Packed.Num() - Count);
    }

    // Pre-allocate for better performance
    Out.Vertices.Reserve(Count);
    Out.Normals.Reserve(Count);
    Out.UVs.Reserve(Count);
    Out.Colors.Reserve(Count);
    Out.Triangles.Reserve(Count);

    for (int32 i = 0; i < Count; i += 3)
    {
        const uint32 p0 = Packed[i + 0];
        const uint32 p1 = Packed[i + 1];
        const uint32 p2 = Packed[i + 2];

        // Skip empty or invalid triangles produced by capacity clamping
        if ((p0 | p1 | p2) == 0u)
        {
            continue;
        }

        const FIntVector L0 = UnpackPos(p0);
        const FIntVector L1 = UnpackPos(p1);
        const FIntVector L2 = UnpackPos(p2);

        // Discard degenerate triangles
        if (L0 == L1 || L1 == L2 || L2 == L0)
        {
            continue;
        }

        // Bounds check against logical chunk size (inclusive coords for faces)
        auto InBounds = [&](const FIntVector& P)
            {
                return (unsigned)P.X <= (unsigned)SizeX &&
                    (unsigned)P.Y <= (unsigned)SizeY &&
                    (unsigned)P.Z <= (unsigned)SizeZ;
            };
        if (!InBounds(L0) || !InBounds(L1) || !InBounds(L2))
        {
            continue;
        }

        FVector A(L0.X * SX, L0.Y * SY, L0.Z * SZ);
        FVector B(L1.X * SX, L1.Y * SY, L1.Z * SZ);
        FVector C(L2.X * SX, L2.Y * SY, L2.Z * SZ);

        const uint32 nCode = (p0 >> 21) & 0x7u;
        const FVector N = FaceNormalFromCode3(nCode);

        const int32 vStart = Out.Vertices.Num();

        Out.Vertices.Add(A); Out.Normals.Add(N);
        Out.Vertices.Add(B); Out.Normals.Add(N);
        Out.Vertices.Add(C); Out.Normals.Add(N);

        Out.UVs.Add(FVector2D(float(L0.X) / 16.f, float(L0.Y) / 16.f));
        Out.UVs.Add(FVector2D(float(L1.X) / 16.f, float(L1.Y) / 16.f));
        Out.UVs.Add(FVector2D(float(L2.X) / 16.f, float(L2.Y) / 16.f));

        Out.Triangles.Add(vStart + 0);
        Out.Triangles.Add(vStart + 1);
        Out.Triangles.Add(vStart + 2);
    }

    UE_LOG(LogTemp, Warning, TEXT("[VoxelGPU Decode] Output: Verts=%d, Tris=%d, Normals=%d"),
        Out.Vertices.Num(), Out.Triangles.Num() / 3, Out.Normals.Num());
}