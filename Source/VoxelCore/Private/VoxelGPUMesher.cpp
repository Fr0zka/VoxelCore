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
#include "Async/Async.h"
#include "HAL/PlatformProcess.h"

// ============================================================
// Compute shader wrapper
// ============================================================
class FGPUGreedyMesherCS : public FGlobalShader
{
    DECLARE_GLOBAL_SHADER(FGPUGreedyMesherCS);
    SHADER_USE_PARAMETER_STRUCT(FGPUGreedyMesherCS, FGlobalShader);

public:
    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        SHADER_PARAMETER(uint32, SizeX)
        SHADER_PARAMETER(uint32, SizeY)
        SHADER_PARAMETER(uint32, SizeZ)
        SHADER_PARAMETER(uint32, XYScale)
        SHADER_PARAMETER(uint32, DefaultAO)

        // RDG resources (types must match HLSL usage)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, InVoxels)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, OutVerts)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, OutCount)
    END_SHADER_PARAMETER_STRUCT()

    static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters&) { return true; }
};
IMPLEMENT_GLOBAL_SHADER(FGPUGreedyMesherCS, "/Plugin/VoxelCore/GPUGreedyMesher.usf", "MainCS", SF_Compute);

// ============================================================
// Helpers
// ============================================================
static FVector FaceNormalFromCode(uint32 N)
{
    switch (N & 7u)
    {
    case 0: return FVector(1, 0, 0);
    case 1: return FVector(-1, 0, 0);
    case 2: return FVector(0, 1, 0);
    case 3: return FVector(0, -1, 0);
    case 4: return FVector(0, 0, 1);
    default:return FVector(0, 0, -1);
    }
}

static FVector2D DeriveUV(uint8 /*Block*/, uint32 N, const FVector& LocalPos)
{
    switch (N & 7u)
    {
    case 0: case 1: return FVector2D(LocalPos.Y, LocalPos.Z);
    case 2: case 3: return FVector2D(LocalPos.X, LocalPos.Z);
    case 4: case 5: return FVector2D(LocalPos.X, LocalPos.Y);
    default:        return FVector2D(0, 0);
    }
}

// ============================================================
// Internal: runs entirely through the render thread; safe from any caller.
// ============================================================
static bool BuildPackedVerts_GPU_OnGameThread(const FGPUMeshBuildParams& P, TArray<uint32>& OutPackedVerts)
{
    OutPackedVerts.Reset();

    if (!GDynamicRHI)
    {
        UE_LOG(LogTemp, Warning, TEXT("[VoxelGPU] GDynamicRHI is null; skipping GPU meshing."));
        return false;
    }
    if (!P.Voxels || P.SizeX <= 0 || P.SizeY <= 0 || P.SizeZ <= 0)
    {
        UE_LOG(LogTemp, Warning, TEXT("[VoxelGPU] Bad input: Vox=%p Size=%dx%dx%d"), P.Voxels, P.SizeX, P.SizeY, P.SizeZ);
        return false;
    }

    const int32 VolCount = P.SizeX * P.SizeY * P.SizeZ;

    // Stage voxels to uint32 (low 8 bits used by shader)
    TArray<uint32> VoxCPU;
    VoxCPU.SetNumUninitialized(VolCount);
    for (int32 i = 0; i < VolCount; ++i)
    {
        VoxCPU[i] = (uint32)P.Voxels[i];
    }

    // Readbacks created on GT, used/locked on RT
    TUniquePtr<FRHIGPUBufferReadback> CountReadback = MakeUnique<FRHIGPUBufferReadback>(TEXT("VoxelGPU_Count"));
    TUniquePtr<FRHIGPUBufferReadback> VertsReadback = MakeUnique<FRHIGPUBufferReadback>(TEXT("VoxelGPU_Verts"));

    const uint32 WorstVerts = (uint32)VoxCPU.Num() * 36u;
    UE_LOG(LogTemp, Verbose, TEXT("[VoxelGPU] Dispatch start | Size=%dx%dx%d XYScale=%d WorstVerts=%u"),
        P.SizeX, P.SizeY, P.SizeZ, P.XYScale, WorstVerts);

    // 1) Enqueue the RDG compute + copy passes.
    ENQUEUE_RENDER_COMMAND(VoxelGPU_ComputeAndCopy)(
        [P,
        Voxels = MoveTemp(VoxCPU),
        WorstVerts,
        CountRB = CountReadback.Get(),
        VertsRB = VertsReadback.Get()](FRHICommandListImmediate& RHICmdList)
        {
            FRDGBuilder GraphBuilder(RHICmdList);

            // STRUCTURED buffer for voxels (matches HLSL StructuredBuffer<uint> InVoxels)
            FRDGBufferRef VoxBuf = CreateStructuredBuffer(
                GraphBuilder,
                TEXT("VoxelGPU.InVoxels"),
                sizeof(uint32),                        // stride
                Voxels.Num(),                          // elements
                Voxels.GetData(),                      // init data
                Voxels.Num() * sizeof(uint32),         // init size
                ERDGInitialDataFlags::None);

            FRDGBufferSRVRef VoxSRV = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(VoxBuf));

            // Output typed UAVs
            FRDGBufferRef OutBuf = GraphBuilder.CreateBuffer(
                FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), WorstVerts),
                TEXT("VoxelGPU.OutVerts"));
            FRDGBufferUAVRef OutBufUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(OutBuf, PF_R32_UINT));

            FRDGBufferRef CountBuf = GraphBuilder.CreateBuffer(
                FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), 1),
                TEXT("VoxelGPU.OutCount"));
            FRDGBufferUAVRef CountUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(CountBuf, PF_R32_UINT));

            AddClearUAVPass(GraphBuilder, CountUAV, 0u);

            // Params
            FGPUGreedyMesherCS::FParameters* Params = GraphBuilder.AllocParameters<FGPUGreedyMesherCS::FParameters>();
            Params->SizeX = (uint32)P.SizeX;
            Params->SizeY = (uint32)P.SizeY;
            Params->SizeZ = (uint32)P.SizeZ;
            Params->XYScale = (uint32)FMath::Max(1, P.XYScale);
            Params->DefaultAO = (uint32)P.DefaultAO;
            Params->InVoxels = VoxSRV;
            Params->OutVerts = OutBufUAV;
            Params->OutCount = CountUAV;

            // Dispatch
            TShaderMapRef<FGPUGreedyMesherCS> CS(GetGlobalShaderMap(GMaxRHIFeatureLevel));
            const FIntVector GroupCount(
                FMath::DivideAndRoundUp(P.SizeX, 8),
                FMath::DivideAndRoundUp(P.SizeY, 8),
                P.SizeZ);

            FComputeShaderUtils::AddPass(
                GraphBuilder,
                RDG_EVENT_NAME("VoxelGPU Greedy Face Emit"),
                CS,
                Params,
                GroupCount);

            // Readbacks
            AddEnqueueCopyPass(GraphBuilder, CountRB, CountBuf, 0);
            AddEnqueueCopyPass(GraphBuilder, VertsRB, OutBuf, 0);

            GraphBuilder.Execute();
        });

    // 2) Enqueue a follow-up RT command that blocks on GPU if needed,
    //    locks readbacks on RT, copies into a heap buffer, and signals an event.
    struct FReadbackResult
    {
        uint32 Count = 0;
        uint32* Data = nullptr;
    };
    FReadbackResult Result;
    FEvent* Done = FPlatformProcess::GetSynchEventFromPool(false);

    ENQUEUE_RENDER_COMMAND(VoxelGPU_Readback)(
        [CountRB = CountReadback.Get(),
        VertsRB = VertsReadback.Get(),
        WorstVerts,               // clamp upper bound
        &Result,
        Done](FRHICommandListImmediate& RHICmdList)
        {
            // Lock count (may block until GPU finishes copy into readback)
            const void* CountPtr = CountRB->Lock(sizeof(uint32));
            uint32 Count = CountPtr ? *((const uint32*)CountPtr) : 0u;
            CountRB->Unlock();

            // Clamp to the number of uints we actually staged into the readback
                if (Count > WorstVerts)
                 {
                UE_LOG(LogTemp, Warning, TEXT("[VoxelGPU] Clamping bad vertex count %u -> %u"), Count, WorstVerts);
                Count = WorstVerts;
                }
             Result.Count = Count;
            
              if (Count > 0)
              {
                  const void* VertsPtr = VertsRB->Lock(Count * sizeof(uint32));
              if (VertsPtr)
                {
                  Result.Data = (uint32*)FMemory::Malloc(Count * sizeof(uint32));
                  FMemory::Memcpy(Result.Data, VertsPtr, Count * sizeof(uint32));
              }
                VertsRB->Unlock();
            }

            Done->Trigger();
        });

    // Wait (on GT or worker) for the RT readback copy to finish
    Done->Wait();
    FPlatformProcess::ReturnSynchEventToPool(Done);

    // Move results to OutPackedVerts on the calling thread
    if (Result.Count > 0 && Result.Data)
    {
        OutPackedVerts.SetNumUninitialized(Result.Count);
        FMemory::Memcpy(OutPackedVerts.GetData(), Result.Data, Result.Count * sizeof(uint32));
        FMemory::Free(Result.Data);
    }
    else
    {
        UE_LOG(LogTemp, Verbose, TEXT("[VoxelGPU] Readback count = 0"));
    }

    return true;
}

// ============================================================
// Public entry — safe to call from ANY thread. Marshals to GT.
// ============================================================
bool FVoxelGPUMesher::BuildPackedVerts_GPU(const FGPUMeshBuildParams& P, TArray<uint32>& OutPackedVerts)
{
    if (IsInGameThread())
    {
        return BuildPackedVerts_GPU_OnGameThread(P, OutPackedVerts);
    }

    bool bOk = false;
    TArray<uint32> LocalOut;
    FEvent* Done = FPlatformProcess::GetSynchEventFromPool(false);

    AsyncTask(ENamedThreads::GameThread, [&P, &LocalOut, &bOk, Done]()
        {
            bOk = BuildPackedVerts_GPU_OnGameThread(P, LocalOut);
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

// ============================================================
// TEMP: CPU decode into standard MeshBuffers (for PMC path)
// ============================================================
void FVoxelGPUMesher::DecodePackedVertsToMeshBuffers(
    const TArray<uint32>& Packed,
    FMeshBuffers& Out,
    float VoxelUU,
    int32 XYScale)
{
    Out.Vertices.Reset();
    Out.Normals.Reset();
    Out.UVs.Reset();
    Out.Colors.Reset();
    Out.Triangles.Reset();

    Out.Vertices.Reserve(Packed.Num());
    Out.Normals.Reserve(Packed.Num());
    Out.UVs.Reserve(Packed.Num());
    Out.Colors.Reserve(Packed.Num());
    Out.Triangles.Reserve(Packed.Num());

    for (int32 i = 0; i < Packed.Num(); ++i)
    {
        const uint32 Pk = Packed[i];

        const uint32 PosX = (Pk) & 0x3Fu;
        const uint32 PosY = ((Pk >> 6) & 0x3Fu);
        const uint32 PosZ = ((Pk >> 12) & 0x3Fu);
        const uint32 Nrm = ((Pk >> 18) & 0x7u);
        const uint32 AO = ((Pk >> 21) & 0x3u);
        const uint32 BID = ((Pk >> 23) & 0xFFu);

        const FVector Normal = FaceNormalFromCode(Nrm);

        const float WorldX = (float)PosX * VoxelUU * XYScale;
        const float WorldY = (float)PosY * VoxelUU * XYScale;
        const float WorldZ = (float)PosZ * VoxelUU;

        const FVector World(WorldX, WorldY, WorldZ);

        Out.Vertices.Add(World);
        Out.Normals.Add(Normal);
        Out.UVs.Add(DeriveUV((uint8)BID, Nrm, FVector((float)PosX, (float)PosY, (float)PosZ)));

        const float ao = (float)AO * (1.0f / 3.0f);
        Out.Colors.Add(FLinearColor(ao, ao, ao, 1.0f));
        Out.Triangles.Add(i);
    }
}
