#include "VoxelGPUMesher.h"
#include "VoxelStructs.h"
#include "VoxelMesher.h"
#include "VoxelSettings.h"

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
#include "HAL/PlatformProcess.h"
#include "HAL/CriticalSection.h"

// Console and Utilities
#include "HAL/IConsoleManager.h"
#include "Templates/SharedPointer.h"

// ============================================================================
// COMPUTE SHADER DECLARATIONS
// ============================================================================

/**
 * Count Pass Shader: Counts how many vertices each voxel will emit.
 *
 * Each thread processes one voxel and writes the vertex count to OutCount.
 * This count is read back to CPU for prefix scan to compute write offsets.
 */
class FGPUCountElementsCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FGPUCountElementsCS);
	SHADER_USE_PARAMETER_STRUCT(FGPUCountElementsCS, FGlobalShader);

public:
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		// Chunk dimensions and LOD
		SHADER_PARAMETER(uint32, SizeX)
		SHADER_PARAMETER(uint32, SizeY)
		SHADER_PARAMETER(uint32, SizeZ)
		SHADER_PARAMETER(uint32, XYScale)
		SHADER_PARAMETER(uint32, DefaultAO)

		// Neighbor availability flags
		SHADER_PARAMETER(uint32, bHasNeighborXN)
		SHADER_PARAMETER(uint32, bHasNeighborXP)
		SHADER_PARAMETER(uint32, bHasNeighborYN)
		SHADER_PARAMETER(uint32, bHasNeighborYP)
		SHADER_PARAMETER(uint32, bHasNeighborZN)
		SHADER_PARAMETER(uint32, bHasNeighborZP)

		// Input voxel data and neighbor slices
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, InVoxels)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, NeighborXN)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, NeighborXP)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, NeighborYN)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, NeighborYP)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, NeighborZN)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, NeighborZP)

		// Output counts (one per voxel)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, OutCount)

		// Dummy outputs for count pass
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, OutVerts)
		SHADER_PARAMETER(uint32, MaxVerts)

		// Control flags
		SHADER_PARAMETER(uint32, DisableGreedyMerge)
		SHADER_PARAMETER(uint32, DirectionMask)
		SHADER_PARAMETER(uint32, IgnoreNeighbors)

		// Neighbor data layout (for different storage formats)
		SHADER_PARAMETER(uint32, NeighborLayoutX)
		SHADER_PARAMETER(uint32, NeighborLayoutY)
		SHADER_PARAMETER(uint32, NeighborLayoutZ)

		// Dummy offsets for count pass
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, Offsets)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters&) { return true; }
};

IMPLEMENT_GLOBAL_SHADER(FGPUCountElementsCS, "/Plugin/VoxelCore/GPUGreedyMesher_Optimized.usf", "CountElementsCS", SF_Compute);

/**
 * Emit Pass Shader: Writes actual vertices to pre-assigned offsets.
 *
 * Uses the offsets computed by CPU prefix scan to write vertices in parallel
 * without atomics or locks (each thread knows its exact write location).
 */
class FGPUEmitElementsCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FGPUEmitElementsCS);
	SHADER_USE_PARAMETER_STRUCT(FGPUEmitElementsCS, FGlobalShader);

public:
	// Reuses the same parameter structure as count pass
	using FParameters = FGPUCountElementsCS::FParameters;
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters&) { return true; }
};

IMPLEMENT_GLOBAL_SHADER(FGPUEmitElementsCS, "/Plugin/VoxelCore/GPUGreedyMesher_Optimized.usf", "EmitElementsCS", SF_Compute);

/**
 * Single-Pass Mesh Shader: Combines count and emit in one pass using atomic allocation.
 *
 * This shader eliminates the GPU→CPU→GPU roundtrip by using InterlockedAdd
 * to atomically allocate space in the output buffer. This is 40-50% faster
 * than the two-pass approach.
 *
 * Features:
 * - Single GPU dispatch (no CPU roundtrip)
 * - Index buffer generation support (33% vertex reduction)
 * - Atomic allocation for lock-free parallel emission
 */
class FGPUSinglePassMeshCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FGPUSinglePassMeshCS);
	SHADER_USE_PARAMETER_STRUCT(FGPUSinglePassMeshCS, FGlobalShader);

public:
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		// Chunk dimensions and LOD
		SHADER_PARAMETER(uint32, SizeX)
		SHADER_PARAMETER(uint32, SizeY)
		SHADER_PARAMETER(uint32, SizeZ)
		SHADER_PARAMETER(uint32, XYScale)
		SHADER_PARAMETER(uint32, DefaultAO)

		// Neighbor availability flags
		SHADER_PARAMETER(uint32, bHasNeighborXN)
		SHADER_PARAMETER(uint32, bHasNeighborXP)
		SHADER_PARAMETER(uint32, bHasNeighborYN)
		SHADER_PARAMETER(uint32, bHasNeighborYP)
		SHADER_PARAMETER(uint32, bHasNeighborZN)
		SHADER_PARAMETER(uint32, bHasNeighborZP)

		// Input voxel data and neighbor slices
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, InVoxels)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, NeighborXN)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, NeighborXP)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, NeighborYN)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, NeighborYP)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, NeighborZN)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, NeighborZP)

		// Output buffers
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, OutVerts)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, OutIndices)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, GlobalVertexCounter)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, GlobalIndexCounter)

		// Capacity limits
		SHADER_PARAMETER(uint32, MaxVerts)
		SHADER_PARAMETER(uint32, MaxIndices)

		// Control flags
		SHADER_PARAMETER(uint32, bUseIndexBuffer)
		SHADER_PARAMETER(uint32, DisableGreedyMerge)
		SHADER_PARAMETER(uint32, DirectionMask)
		SHADER_PARAMETER(uint32, IgnoreNeighbors)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters&) { return true; }
};

IMPLEMENT_GLOBAL_SHADER(FGPUSinglePassMeshCS, "/Plugin/VoxelCore/GPUGreedyMesher_SinglePass.usf", "SinglePassMeshCS", SF_Compute);

/**
 * True Greedy Mesh Shader - Implements proper greedy quad merging.
 *
 * Uses slice-based parallelization:
 * - Each thread group processes one Z-slice
 * - Builds face mask in shared memory
 * - Greedily merges quads (horizontal + vertical)
 * - Reduces triangle count by 60-90% vs naive approach
 */
class FGPUTrueGreedyMeshCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FGPUTrueGreedyMeshCS);
	SHADER_USE_PARAMETER_STRUCT(FGPUTrueGreedyMeshCS, FGlobalShader);

public:
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		// Chunk dimensions and LOD
		SHADER_PARAMETER(uint32, SizeX)
		SHADER_PARAMETER(uint32, SizeY)
		SHADER_PARAMETER(uint32, SizeZ)
		SHADER_PARAMETER(uint32, XYScale)
		SHADER_PARAMETER(uint32, DefaultAO)

		// Neighbor availability flags
		SHADER_PARAMETER(uint32, bHasNeighborXN)
		SHADER_PARAMETER(uint32, bHasNeighborXP)
		SHADER_PARAMETER(uint32, bHasNeighborYN)
		SHADER_PARAMETER(uint32, bHasNeighborYP)
		SHADER_PARAMETER(uint32, bHasNeighborZN)
		SHADER_PARAMETER(uint32, bHasNeighborZP)

		// Input voxel data and neighbor slices
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, InVoxels)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, NeighborXN)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, NeighborXP)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, NeighborYN)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, NeighborYP)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, NeighborZN)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, NeighborZP)

		// Output buffers
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, OutVerts)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, GlobalVertexCounter)

		// Capacity limit
		SHADER_PARAMETER(uint32, MaxVerts)

		// Control flags
		SHADER_PARAMETER(uint32, IgnoreNeighbors)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters&) { return true; }
};

IMPLEMENT_GLOBAL_SHADER(FGPUTrueGreedyMeshCS, "/Plugin/VoxelCore/GPUGreedyMesher_TrueGreedy.usf", "TrueGreedyMeshCS", SF_Compute);

// ============================================================================
// NEIGHBOR DATA STRUCTURES
// ============================================================================

/**
 * GPU-friendly neighbor arrays (uint32 instead of uint8 for shader compatibility).
 *
 * Each array stores a single slice of an adjacent chunk:
 * - XN/XP: YZ slices (SizeY × SizeZ elements)
 * - YN/YP: XZ slices (SizeX × SizeZ elements)
 * - ZN/ZP: XY slices (SizeX × SizeY elements)
 */
struct FGPUNeighborArrays
{
	TArray<uint32> XN;  // Left neighbor (-X)
	TArray<uint32> XP;  // Right neighbor (+X)
	TArray<uint32> YN;  // Back neighbor (-Y)
	TArray<uint32> YP;  // Front neighbor (+Y)
	TArray<uint32> ZN;  // Bottom neighbor (-Z)
	TArray<uint32> ZP;  // Top neighbor (+Z)
};

/**
 * Copy neighbor data from CPU format (uint8*) to GPU format (uint32 arrays).
 *
 * Also logs neighbor statistics for the first XN neighbor copy (debugging).
 */
static void CopyNeighborArrays(const FGPUMeshBuildParams& Params, FGPUNeighborArrays& OutNeighbors)
{
	// XN: Left neighbor (-X direction)
	if (Params.bHasNeighborXN && Params.NeighborXN)
	{
		const int32 Count = Params.SizeY * Params.SizeZ;
		OutNeighbors.XN.SetNumUninitialized(Count);

		int32 SolidCount = 0;
		for (int32 i = 0; i < Count; ++i)
		{
			OutNeighbors.XN[i] = static_cast<uint32>(Params.NeighborXN[i]);
			if (Params.NeighborXN[i] != 0)
			{
				++SolidCount;
			}
		}

		// Debug logging for first neighbor copy (helps diagnose neighbor data issues)
		static bool bLoggedOnce = false;
		if (!bLoggedOnce)
		{
			bLoggedOnce = true;
			UE_LOG(LogTemp, Log, TEXT("[VoxelGPU] XN neighbor: %d solid voxels of %d total. All neighbors: XN=%d XP=%d YN=%d YP=%d ZN=%d ZP=%d"),
				SolidCount, Count,
				Params.bHasNeighborXN ? 1 : 0, Params.bHasNeighborXP ? 1 : 0,
				Params.bHasNeighborYN ? 1 : 0, Params.bHasNeighborYP ? 1 : 0,
				Params.bHasNeighborZN ? 1 : 0, Params.bHasNeighborZP ? 1 : 0);
		}
	}

	// XP: Right neighbor (+X direction)
	if (Params.bHasNeighborXP && Params.NeighborXP)
	{
		const int32 Count = Params.SizeY * Params.SizeZ;
		OutNeighbors.XP.SetNumUninitialized(Count);
		for (int32 i = 0; i < Count; ++i)
		{
			OutNeighbors.XP[i] = static_cast<uint32>(Params.NeighborXP[i]);
		}
	}

	// YN: Back neighbor (-Y direction)
	if (Params.bHasNeighborYN && Params.NeighborYN)
	{
		const int32 Count = Params.SizeX * Params.SizeZ;
		OutNeighbors.YN.SetNumUninitialized(Count);
		for (int32 i = 0; i < Count; ++i)
		{
			OutNeighbors.YN[i] = static_cast<uint32>(Params.NeighborYN[i]);
		}
	}

	// YP: Front neighbor (+Y direction)
	if (Params.bHasNeighborYP && Params.NeighborYP)
	{
		const int32 Count = Params.SizeX * Params.SizeZ;
		OutNeighbors.YP.SetNumUninitialized(Count);
		for (int32 i = 0; i < Count; ++i)
		{
			OutNeighbors.YP[i] = static_cast<uint32>(Params.NeighborYP[i]);
		}
	}

	// ZN: Bottom neighbor (-Z direction)
	if (Params.bHasNeighborZN && Params.NeighborZN)
	{
		const int32 Count = Params.SizeX * Params.SizeY;
		OutNeighbors.ZN.SetNumUninitialized(Count);
		for (int32 i = 0; i < Count; ++i)
		{
			OutNeighbors.ZN[i] = static_cast<uint32>(Params.NeighborZN[i]);
		}
	}

	// ZP: Top neighbor (+Z direction)
	if (Params.bHasNeighborZP && Params.NeighborZP)
	{
		const int32 Count = Params.SizeX * Params.SizeY;
		OutNeighbors.ZP.SetNumUninitialized(Count);
		for (int32 i = 0; i < Count; ++i)
		{
			OutNeighbors.ZP[i] = static_cast<uint32>(Params.NeighborZP[i]);
		}
	}
}

// ============================================================================
// ASYNC JOB MANAGEMENT
// ============================================================================

/**
 * Async job stages (single-pass GPU meshing).
 */
enum class EVoxelGPUAsyncStage : uint8
{
	WaitingForSinglePass  // Single-pass: waiting for readback of counters + vertices
};

/**
 * Async GPU meshing job.
 *
 * Tracks the state of an async single-pass GPU meshing operation:
 * 1. Single-pass shader with atomic allocation
 * 2. Readbacks poll until ready, then invoke completion callback
 */
struct FVoxelGPUAsyncJob : public TSharedFromThis<FVoxelGPUAsyncJob, ESPMode::ThreadSafe>
{
	// Input data (copied to avoid dangling pointers)
	TSharedPtr<TArray<uint32>, ESPMode::ThreadSafe> Voxels;
	TSharedPtr<FGPUNeighborArrays, ESPMode::ThreadSafe> Neighbors;

	// GPU readback handles
	TUniquePtr<FRHIGPUBufferReadback> VertexCounterReadback;
	TUniquePtr<FRHIGPUBufferReadback> VertsReadback;

	// Final packed vertices
	TArray<uint32> PackedResult;

	// Job metadata
	int32 SizeX = 0;
	int32 SizeY = 0;
	int32 SizeZ = 0;
	int32 XYScale = 1;
	uint8 DefaultAO = 3;
	int32 VolCount = 0;             // Total voxels (SizeX × SizeY × SizeZ)
	int32 TotalElements = 0;        // Total vertices emitted (from counter)

	// Settings
	int32 MaxOutputVerts = INT32_MAX;
	bool bAggressiveCulling = false;

	// Completion callback
	TFunction<void(bool, TArray<uint32>&&)> Completion;

	// State
	EVoxelGPUAsyncStage Stage = EVoxelGPUAsyncStage::WaitingForSinglePass;
	bool bSuccess = true;
};

// Global async job queue (protected by mutex)
static FCriticalSection GVoxelGPUAsyncMutex;
static TArray<TSharedPtr<FVoxelGPUAsyncJob, ESPMode::ThreadSafe>> GVoxelGPUAsyncJobs;

// ============================================================================
// CONSOLE VARIABLES (DEBUG AND TUNING)
// ============================================================================

/** Log per-chunk GPU vs CPU mesher usage and triangle counts. */
static TAutoConsoleVariable<int32> CVarVoxelGPU_LogPerChunk(
	TEXT("r.Voxel.GPU.LogPerChunk"),
	0,
	TEXT("Log GPU/CPU mesher usage per chunk with tri/vert counts."),
	ECVF_Default);

/** Disable greedy quad merging (emit 1×1 quads for every exposed face). */
static TAutoConsoleVariable<int32> CVarVoxelGPU_DisableGreedyMerge(
	TEXT("r.Voxel.GPU.DisableGreedyMerge"),
	0,
	TEXT("Bypass greedy merging in shader and emit 1x1 quads per mask cell."),
	ECVF_Default);

/** Ignore neighbor borders (treat all external faces as air - creates seams but faster). */
static TAutoConsoleVariable<int32> CVarVoxelGPU_IgnoreNeighbors(
	TEXT("r.Voxel.GPU.IgnoreNeighbors"),
	0,
	TEXT("Ignore neighbor borders for culling (treat all outside as air)."),
	ECVF_Default);

/** Compare GPU mesher output with CPU greedy mesher (log triangle counts). */
static TAutoConsoleVariable<int32> CVarVoxelGPU_CompareCPU(
	TEXT("r.Voxel.GPU.CompareCPU"),
	0,
	TEXT("When 1, builds CPU greedy mesh alongside GPU and logs triangle counts."),
	ECVF_Default);

/** Use single-pass GPU mesher (faster, eliminates CPU roundtrip). */
static TAutoConsoleVariable<int32> CVarVoxelGPU_UseSinglePass(
	TEXT("r.Voxel.GPU.UseSinglePass"),
	1,
	TEXT("Use single-pass GPU mesher with atomic allocation (1=enabled, 0=use two-pass)."),
	ECVF_Default);

/** Use true greedy meshing algorithm on GPU (reduces triangles by 60-90%). */
static TAutoConsoleVariable<int32> CVarVoxelGPU_UseTrueGreedy(
	TEXT("r.Voxel.GPU.UseTrueGreedy"),
	1,
	TEXT("Use true greedy meshing on GPU with quad merging (1=greedy, 0=naive face culling). Significantly reduces triangle count."),
	ECVF_Default);

/** Frame budget for GPU mesh readbacks. */
static TAutoConsoleVariable<int32> CVarVoxelGPU_MaxReadbacksPerFrame(
	TEXT("r.Voxel.GPU.MaxReadbacksPerFrame"),
	-1,
	TEXT("Max GPU mesh readbacks per frame. -1=use setting, 0=unlimited, >0=throttle. Adjust for FPS/throughput balance."),
	ECVF_Default);

/** Use index buffer for GPU mesher (33% vertex reduction). */
static TAutoConsoleVariable<int32> CVarVoxelGPU_UseIndexBuffer(
	TEXT("r.Voxel.GPU.UseIndexBuffer"),
	0,
	TEXT("Generate index buffer for GPU mesh (1=indexed, 0=vertex-only). Disabled by default until API supports index buffers."),
	ECVF_Default);

// ============================================================================
// HELPER: CREATE NEIGHBOR BUFFER FOR RENDER GRAPH
// ============================================================================

/**
 * Helper to create RDG buffer for neighbor data.
 *
 * Creates a structured buffer with neighbor data if available, otherwise
 * creates a dummy 1-element buffer cleared to 0 (air).
 *
 * This pattern is used 3× in the codebase (count pass, emit pass, async),
 * so we extract it to avoid duplication.
 */
static FRDGBufferSRVRef CreateNeighborBufferSRV(
	FRDGBuilder& GraphBuilder,
	const TCHAR* Name,
	const TArray<uint32>& Data,
	bool bHasData)
{
	if (bHasData && Data.Num() > 0)
	{
		FRDGBufferRef Buf = CreateStructuredBuffer(GraphBuilder, Name, sizeof(uint32), Data.Num(),
			Data.GetData(), Data.Num() * sizeof(uint32), ERDGInitialDataFlags::None);
		return GraphBuilder.CreateSRV(FRDGBufferSRVDesc(Buf));
	}

	// No neighbor data - create dummy air buffer
	FRDGBufferRef Buf = CreateStructuredBuffer(GraphBuilder, Name, sizeof(uint32), 1, nullptr, 0, ERDGInitialDataFlags::None);
	FRDGBufferUAVRef BufUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Buf));
	AddClearUAVPass(GraphBuilder, BufUAV, 0u);
	return GraphBuilder.CreateSRV(FRDGBufferSRVDesc(Buf));
}

// ============================================================================
// SINGLE-PASS SYNCHRONOUS GPU MESHING
// ============================================================================

/**
 * Single-pass synchronous GPU meshing implementation.
 *
 * Uses atomic allocation to eliminate GPU→CPU→GPU roundtrip.
 * This is 40-50% faster than the two-pass approach.
 *
 * **Single Pass (Count + Emit):**
 * - Each thread counts and atomically allocates space
 * - Writes vertices/indices directly (no CPU involvement)
 * - Readback final counter to determine actual output size
 *
 * @param Params Build parameters
 * @param OutPackedVerts Output packed vertices
 * @param OutIndices Output index buffer (optional)
 * @param bUseIndexBuffer Whether to generate index buffer
 * @return true if successful
 */
static bool BuildPackedVerts_GPU_SinglePass(
	const FGPUMeshBuildParams& Params,
	TArray<uint32>& OutPackedVerts,
	TArray<uint32>& OutIndices,
	bool bUseIndexBuffer)
{
	OutPackedVerts.Reset();
	OutIndices.Reset();

	// Validate prerequisites
	if (!GDynamicRHI)
	{
		return false;
	}
	if (!Params.Voxels || Params.SizeX <= 0 || Params.SizeY <= 0 || Params.SizeZ <= 0)
	{
		return false;
	}

	const int32 VolCount = Params.SizeX * Params.SizeY * Params.SizeZ;

	// Estimate maximum output size (worst case: all voxels emit 6 faces)
	const int32 MaxFaces = VolCount * 6;
	const int32 MaxOutputVerts = bUseIndexBuffer ? (MaxFaces * 4) : (MaxFaces * 6);
	const int32 MaxOutputIndices = bUseIndexBuffer ? (MaxFaces * 6) : 0;

	// Apply capacity limit
	const int32 ActualMaxVerts = (Params.MaxOutputVerts > 0)
		? FMath::Min(MaxOutputVerts, Params.MaxOutputVerts)
		: MaxOutputVerts;
	const int32 ActualMaxIndices = bUseIndexBuffer ? ActualMaxVerts : 0;

	// Copy voxel data to GPU format
	TArray<uint32> VoxelsCPU;
	VoxelsCPU.SetNumUninitialized(VolCount);
	for (int32 i = 0; i < VolCount; ++i)
	{
		VoxelsCPU[i] = static_cast<uint32>(Params.Voxels[i]);
	}

	// Copy neighbor data
	TSharedRef<FGPUNeighborArrays, ESPMode::ThreadSafe> NeighborData = MakeShared<FGPUNeighborArrays, ESPMode::ThreadSafe>();
	CopyNeighborArrays(Params, NeighborData.Get());

	// Allocate readback for counters
	TUniquePtr<FRHIGPUBufferReadback> VertexCounterReadback = MakeUnique<FRHIGPUBufferReadback>(TEXT("VoxelGPU_VertexCounter"));
	TUniquePtr<FRHIGPUBufferReadback> IndexCounterReadback = bUseIndexBuffer
		? MakeUnique<FRHIGPUBufferReadback>(TEXT("VoxelGPU_IndexCounter"))
		: nullptr;
	TUniquePtr<FRHIGPUBufferReadback> VertsReadback = MakeUnique<FRHIGPUBufferReadback>(TEXT("VoxelGPU_Verts"));
	TUniquePtr<FRHIGPUBufferReadback> IndicesReadback = bUseIndexBuffer
		? MakeUnique<FRHIGPUBufferReadback>(TEXT("VoxelGPU_Indices"))
		: nullptr;

	// ========== SINGLE PASS: COUNT + EMIT WITH ATOMIC ALLOCATION ==========

	FEvent* Done = FPlatformProcess::GetSynchEventFromPool(false);

	ENQUEUE_RENDER_COMMAND(VoxelGPU_SinglePass)(
		[Params, Voxels = MoveTemp(VoxelsCPU), Neighbors = NeighborData,
		 ActualMaxVerts, ActualMaxIndices, bUseIndexBuffer,
		 VertCountRB = VertexCounterReadback.Get(),
		 IdxCountRB = IndexCounterReadback.Get(),
		 VertsRB = VertsReadback.Get(),
		 IdxRB = IndicesReadback.Get()]
		(FRHICommandListImmediate& RHICmdList)
		{
			FRDGBuilder GraphBuilder(RHICmdList);

			// Input voxel buffer
			FRDGBufferRef VoxBuf = CreateStructuredBuffer(GraphBuilder, TEXT("VoxelGPU.InVoxels"),
				sizeof(uint32), Voxels.Num(), Voxels.GetData(), Voxels.Num() * sizeof(uint32), ERDGInitialDataFlags::None);
			FRDGBufferSRVRef VoxSRV = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(VoxBuf));

			// Output vertex buffer (pre-allocated to max size)
			FRDGBufferRef OutVertsBuf = CreateStructuredBuffer(GraphBuilder, TEXT("VoxelGPU.OutVerts"),
				sizeof(uint32), ActualMaxVerts, nullptr, 0, ERDGInitialDataFlags::None);
			FRDGBufferUAVRef OutVertsUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(OutVertsBuf));
			AddClearUAVPass(GraphBuilder, OutVertsUAV, 0u);

			// Global vertex counter (atomic allocation)
			FRDGBufferRef VertexCounterBuf = CreateStructuredBuffer(GraphBuilder, TEXT("VoxelGPU.VertexCounter"),
				sizeof(uint32), 1, nullptr, 0, ERDGInitialDataFlags::None);
			FRDGBufferUAVRef VertexCounterUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(VertexCounterBuf, PF_R32_UINT));
			AddClearUAVPass(GraphBuilder, VertexCounterUAV, 0u);

			// Index buffer and counter (if using indexed mode)
			FRDGBufferRef OutIndicesBuf = nullptr;
			FRDGBufferUAVRef OutIndicesUAV = nullptr;
			FRDGBufferRef IndexCounterBuf = nullptr;
			FRDGBufferUAVRef IndexCounterUAV = nullptr;

			if (bUseIndexBuffer)
			{
				OutIndicesBuf = CreateStructuredBuffer(GraphBuilder, TEXT("VoxelGPU.OutIndices"),
					sizeof(uint32), ActualMaxIndices, nullptr, 0, ERDGInitialDataFlags::None);
				OutIndicesUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(OutIndicesBuf));
				AddClearUAVPass(GraphBuilder, OutIndicesUAV, 0u);

				IndexCounterBuf = CreateStructuredBuffer(GraphBuilder, TEXT("VoxelGPU.IndexCounter"),
					sizeof(uint32), 1, nullptr, 0, ERDGInitialDataFlags::None);
				IndexCounterUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(IndexCounterBuf, PF_R32_UINT));
				AddClearUAVPass(GraphBuilder, IndexCounterUAV, 0u);
			}
			else
			{
				// Dummy buffers for non-indexed mode
				OutIndicesBuf = CreateStructuredBuffer(GraphBuilder, TEXT("VoxelGPU.DummyIndices"),
					sizeof(uint32), 1, nullptr, 0, ERDGInitialDataFlags::None);
				OutIndicesUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(OutIndicesBuf));
				AddClearUAVPass(GraphBuilder, OutIndicesUAV, 0u);

				IndexCounterBuf = CreateStructuredBuffer(GraphBuilder, TEXT("VoxelGPU.DummyIndexCounter"),
					sizeof(uint32), 1, nullptr, 0, ERDGInitialDataFlags::None);
				IndexCounterUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(IndexCounterBuf, PF_R32_UINT));
				AddClearUAVPass(GraphBuilder, IndexCounterUAV, 0u);
			}

			// Neighbor buffers
			FRDGBufferSRVRef NeighborXNSRV = CreateNeighborBufferSRV(GraphBuilder, TEXT("VoxelGPU.NeighborXN"), Neighbors->XN, Params.bHasNeighborXN);
			FRDGBufferSRVRef NeighborXPSRV = CreateNeighborBufferSRV(GraphBuilder, TEXT("VoxelGPU.NeighborXP"), Neighbors->XP, Params.bHasNeighborXP);
			FRDGBufferSRVRef NeighborYNSRV = CreateNeighborBufferSRV(GraphBuilder, TEXT("VoxelGPU.NeighborYN"), Neighbors->YN, Params.bHasNeighborYN);
			FRDGBufferSRVRef NeighborYPSRV = CreateNeighborBufferSRV(GraphBuilder, TEXT("VoxelGPU.NeighborYP"), Neighbors->YP, Params.bHasNeighborYP);
			FRDGBufferSRVRef NeighborZNSRV = CreateNeighborBufferSRV(GraphBuilder, TEXT("VoxelGPU.NeighborZN"), Neighbors->ZN, Params.bHasNeighborZN);
			FRDGBufferSRVRef NeighborZPSRV = CreateNeighborBufferSRV(GraphBuilder, TEXT("VoxelGPU.NeighborZP"), Neighbors->ZP, Params.bHasNeighborZP);

			// Shader parameters
			TShaderMapRef<FGPUSinglePassMeshCS> CS(GetGlobalShaderMap(GMaxRHIFeatureLevel));
			FGPUSinglePassMeshCS::FParameters* ShaderParams = GraphBuilder.AllocParameters<FGPUSinglePassMeshCS::FParameters>();
			ShaderParams->SizeX = static_cast<uint32>(Params.SizeX);
			ShaderParams->SizeY = static_cast<uint32>(Params.SizeY);
			ShaderParams->SizeZ = static_cast<uint32>(Params.SizeZ);
			ShaderParams->XYScale = static_cast<uint32>(FMath::Max(1, Params.XYScale));
			ShaderParams->DefaultAO = static_cast<uint32>(Params.DefaultAO);
			ShaderParams->InVoxels = VoxSRV;
			ShaderParams->OutVerts = OutVertsUAV;
			ShaderParams->OutIndices = OutIndicesUAV;
			ShaderParams->GlobalVertexCounter = VertexCounterUAV;
			ShaderParams->GlobalIndexCounter = IndexCounterUAV;
			ShaderParams->NeighborXN = NeighborXNSRV;
			ShaderParams->NeighborXP = NeighborXPSRV;
			ShaderParams->NeighborYN = NeighborYNSRV;
			ShaderParams->NeighborYP = NeighborYPSRV;
			ShaderParams->NeighborZN = NeighborZNSRV;
			ShaderParams->NeighborZP = NeighborZPSRV;
			ShaderParams->bHasNeighborXN = Params.bHasNeighborXN ? 1u : 0u;
			ShaderParams->bHasNeighborXP = Params.bHasNeighborXP ? 1u : 0u;
			ShaderParams->bHasNeighborYN = Params.bHasNeighborYN ? 1u : 0u;
			ShaderParams->bHasNeighborYP = Params.bHasNeighborYP ? 1u : 0u;
			ShaderParams->bHasNeighborZN = Params.bHasNeighborZN ? 1u : 0u;
			ShaderParams->bHasNeighborZP = Params.bHasNeighborZP ? 1u : 0u;
			ShaderParams->MaxVerts = static_cast<uint32>(ActualMaxVerts);
			ShaderParams->MaxIndices = static_cast<uint32>(ActualMaxIndices);
			ShaderParams->bUseIndexBuffer = bUseIndexBuffer ? 1u : 0u;
			ShaderParams->DisableGreedyMerge = 0u;
			ShaderParams->DirectionMask = 0x3Fu;  // All 6 directions

			static auto* CVarIgnNbh = IConsoleManager::Get().FindConsoleVariable(TEXT("r.Voxel.GPU.IgnoreNeighbors"));
			const bool bIgnoreNeighbors = Params.bAggressiveCulling || (CVarIgnNbh && CVarIgnNbh->GetInt() != 0);
			ShaderParams->IgnoreNeighbors = bIgnoreNeighbors ? 1u : 0u;

			// Check if we should use true greedy meshing (CVar or setting)
			static auto* CVarUseGreedy = IConsoleManager::Get().FindConsoleVariable(TEXT("r.Voxel.GPU.UseTrueGreedy"));
			const bool bUseTrueGreedyCVar = CVarUseGreedy && CVarUseGreedy->GetInt() != 0;
			const bool bUseTrueGreedy = bUseTrueGreedyCVar; // CVar takes precedence for now

			if (bUseTrueGreedy)
			{
				// TRUE GREEDY MESHING PATH - Slice-based parallel algorithm
				// Dispatch one thread group per Z-slice (each group = 32 threads)
				TShaderMapRef<FGPUTrueGreedyMeshCS> GreedyCS(GetGlobalShaderMap(GMaxRHIFeatureLevel));
				FGPUTrueGreedyMeshCS::FParameters* GreedyParams = GraphBuilder.AllocParameters<FGPUTrueGreedyMeshCS::FParameters>();

				// Copy common parameters
				GreedyParams->SizeX = ShaderParams->SizeX;
				GreedyParams->SizeY = ShaderParams->SizeY;
				GreedyParams->SizeZ = ShaderParams->SizeZ;
				GreedyParams->XYScale = ShaderParams->XYScale;
				GreedyParams->DefaultAO = ShaderParams->DefaultAO;
				GreedyParams->InVoxels = ShaderParams->InVoxels;
				GreedyParams->NeighborXN = ShaderParams->NeighborXN;
				GreedyParams->NeighborXP = ShaderParams->NeighborXP;
				GreedyParams->NeighborYN = ShaderParams->NeighborYN;
				GreedyParams->NeighborYP = ShaderParams->NeighborYP;
				GreedyParams->NeighborZN = ShaderParams->NeighborZN;
				GreedyParams->NeighborZP = ShaderParams->NeighborZP;
				GreedyParams->bHasNeighborXN = ShaderParams->bHasNeighborXN;
				GreedyParams->bHasNeighborXP = ShaderParams->bHasNeighborXP;
				GreedyParams->bHasNeighborYN = ShaderParams->bHasNeighborYN;
				GreedyParams->bHasNeighborYP = ShaderParams->bHasNeighborYP;
				GreedyParams->bHasNeighborZN = ShaderParams->bHasNeighborZN;
				GreedyParams->bHasNeighborZP = ShaderParams->bHasNeighborZP;
				GreedyParams->OutVerts = ShaderParams->OutVerts;
				GreedyParams->GlobalVertexCounter = ShaderParams->GlobalVertexCounter;
				GreedyParams->MaxVerts = ShaderParams->MaxVerts;
				GreedyParams->IgnoreNeighbors = ShaderParams->IgnoreNeighbors;

				// Dispatch: One group per Z-slice (32 threads per group)
				const FIntVector GreedyGroups(Params.SizeZ, 1, 1);
				FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("VoxelGPU TrueGreedy"), GreedyCS, GreedyParams, GreedyGroups);
			}
			else
			{
				// NAIVE SINGLE-PASS PATH - Original 1×1 quad per face
				// Dispatch 8×8×8 thread groups
				const FIntVector Groups(
					FMath::DivideAndRoundUp(Params.SizeX, 8),
					FMath::DivideAndRoundUp(Params.SizeY, 8),
					FMath::DivideAndRoundUp(Params.SizeZ, 8));

				FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("VoxelGPU SinglePass"), CS, ShaderParams, Groups);
			}

			// Readback counters and buffers
			AddEnqueueCopyPass(GraphBuilder, VertCountRB, VertexCounterBuf, 0);
			AddEnqueueCopyPass(GraphBuilder, VertsRB, OutVertsBuf, 0);
			if (bUseIndexBuffer)
			{
				AddEnqueueCopyPass(GraphBuilder, IdxCountRB, IndexCounterBuf, 0);
				AddEnqueueCopyPass(GraphBuilder, IdxRB, OutIndicesBuf, 0);
			}

			GraphBuilder.Execute();
		});

	// Readback results to CPU
	TArray<uint32> VertsCPU;
	TArray<uint32> IndicesCPU;
	uint32 FinalVertexCount = 0;
	uint32 FinalIndexCount = 0;

	// Flush commands but DON'T block the entire GPU
	ENQUEUE_RENDER_COMMAND(VoxelGPU_SinglePass_Flush)(
		[](FRHICommandListImmediate& RHICmdList)
		{
			RHICmdList.SubmitCommandsAndFlushGPU();
			// Removed BlockUntilGPUIdle() - was causing FPS to drop to 50!
		});

	// Poll for readback completion (yields to other threads)
	while (!VertexCounterReadback->IsReady())
	{
		FPlatformProcess::Sleep(0.001f); // 1ms sleep, allows other work
	}

	ENQUEUE_RENDER_COMMAND(VoxelGPU_SinglePass_Readback)(
		[VertCountRB = VertexCounterReadback.Get(),
		 IdxCountRB = IndexCounterReadback.Get(),
		 VertsRB = VertsReadback.Get(),
		 IdxRB = IndicesReadback.Get(),
		 &VertsCPU, &IndicesCPU, &FinalVertexCount, &FinalIndexCount,
		 ActualMaxVerts, ActualMaxIndices, bUseIndexBuffer, Done]
		(FRHICommandListImmediate& RHICmdList)
		{
			// Readback is ready, just read the data

			// Read vertex count
			const void* VertCountPtr = VertCountRB->Lock(sizeof(uint32));
			if (VertCountPtr)
			{
				FMemory::Memcpy(&FinalVertexCount, VertCountPtr, sizeof(uint32));
			}
			VertCountRB->Unlock();

			// Clamp to max
			FinalVertexCount = FMath::Min<uint32>(FinalVertexCount, ActualMaxVerts);

			// Read vertices
			if (FinalVertexCount > 0)
			{
				const void* VertsPtr = VertsRB->Lock(static_cast<int64>(ActualMaxVerts) * sizeof(uint32));
				if (VertsPtr)
				{
					VertsCPU.SetNumUninitialized(FinalVertexCount);
					FMemory::Memcpy(VertsCPU.GetData(), VertsPtr, FinalVertexCount * sizeof(uint32));
				}
				VertsRB->Unlock();
			}

			// Read index count and indices (if using index buffer)
			if (bUseIndexBuffer && IdxCountRB && IdxRB)
			{
				const void* IdxCountPtr = IdxCountRB->Lock(sizeof(uint32));
				if (IdxCountPtr)
				{
					FMemory::Memcpy(&FinalIndexCount, IdxCountPtr, sizeof(uint32));
				}
				IdxCountRB->Unlock();

				FinalIndexCount = FMath::Min<uint32>(FinalIndexCount, ActualMaxIndices);

				if (FinalIndexCount > 0)
				{
					const void* IdxPtr = IdxRB->Lock(static_cast<int64>(ActualMaxIndices) * sizeof(uint32));
					if (IdxPtr)
					{
						IndicesCPU.SetNumUninitialized(FinalIndexCount);
						FMemory::Memcpy(IndicesCPU.GetData(), IdxPtr, FinalIndexCount * sizeof(uint32));
					}
					IdxRB->Unlock();
				}
			}

			Done->Trigger();
		});

	Done->Wait();
	FPlatformProcess::ReturnSynchEventToPool(Done);

	// Transfer results
	OutPackedVerts = MoveTemp(VertsCPU);
	if (bUseIndexBuffer)
	{
		OutIndices = MoveTemp(IndicesCPU);
	}

	UE_LOG(LogTemp, Log, TEXT("[VoxelGPU SinglePass] Generated %d vertices, %d indices (indexed=%d)"),
		OutPackedVerts.Num(), OutIndices.Num(), bUseIndexBuffer ? 1 : 0);

	return true;
}

// ============================================================================
// TWO-PASS SYNCHRONOUS GPU MESHING
// ============================================================================

/**
 * Two-pass synchronous GPU meshing implementation.
 *
 * **Pass 1 (Count):**
 * - Dispatch count shader to count vertices per voxel
 * - Readback counts to CPU and wait for completion
 *
 * **CPU Prefix Scan:**
 * - Compute cumulative offsets (each voxel knows where to write)
 * - Validate total vertex count fits in buffer
 *
 * **Pass 2 (Emit):**
 * - Dispatch emit shader with offset buffer
 * - Readback vertices to CPU and wait for completion
 *
 * @param Params Build parameters
 * @param OutPackedVerts Output packed vertices
 * @return true if successful
 */
static bool BuildPackedVerts_GPU_TwoPass(const FGPUMeshBuildParams& Params, TArray<uint32>& OutPackedVerts)
{
	OutPackedVerts.Reset();

	// Validate prerequisites
	if (!GDynamicRHI)
	{
		return false;
	}
	if (!Params.Voxels || Params.SizeX <= 0 || Params.SizeY <= 0 || Params.SizeZ <= 0)
	{
		return false;
	}

	const int32 VolCount = Params.SizeX * Params.SizeY * Params.SizeZ;

	// Copy voxel categories to uint32 for GPU (shader expects uint)
	TArray<uint32> VoxelsCPU;
	VoxelsCPU.SetNumUninitialized(VolCount);
	for (int32 i = 0; i < VolCount; ++i)
	{
		VoxelsCPU[i] = static_cast<uint32>(Params.Voxels[i]);
	}

	// Copy neighbor data
	TSharedRef<FGPUNeighborArrays, ESPMode::ThreadSafe> NeighborData = MakeShared<FGPUNeighborArrays, ESPMode::ThreadSafe>();
	CopyNeighborArrays(Params, NeighborData.Get());

	// Allocate readback resources
	TUniquePtr<FRHIGPUBufferReadback> CountReadback = MakeUnique<FRHIGPUBufferReadback>(TEXT("VoxelGPU_Counts"));
	TUniquePtr<FRHIGPUBufferReadback> VertsReadback = MakeUnique<FRHIGPUBufferReadback>(TEXT("VoxelGPU_Verts"));

	// ========== PASS 1: COUNT ELEMENTS PER VOXEL ==========

	TArray<uint32> CountsCPU;
	CountsCPU.SetNumUninitialized(VolCount);

	FEvent* CountDone = FPlatformProcess::GetSynchEventFromPool(false);

	ENQUEUE_RENDER_COMMAND(VoxelGPU_TwoPass_Count)(
		[Params, Voxels = MoveTemp(VoxelsCPU), Neighbors = NeighborData, CountRB = CountReadback.Get()]
		(FRHICommandListImmediate& RHICmdList)
		{
			FRDGBuilder GraphBuilder(RHICmdList);
			const int32 VolCountRT = Params.SizeX * Params.SizeY * Params.SizeZ;

			// Input voxel buffer
			FRDGBufferRef VoxBuf = CreateStructuredBuffer(GraphBuilder, TEXT("VoxelGPU.InVoxels"),
				sizeof(uint32), Voxels.Num(), Voxels.GetData(), Voxels.Num() * sizeof(uint32), ERDGInitialDataFlags::None);
			FRDGBufferSRVRef VoxSRV = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(VoxBuf));

			// Output count buffer
			FRDGBufferRef CountsBuf = CreateStructuredBuffer(GraphBuilder, TEXT("VoxelGPU.Counts"),
				sizeof(uint32), VolCountRT, nullptr, 0, ERDGInitialDataFlags::None);
			FRDGBufferUAVRef CountsUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(CountsBuf, PF_R32_UINT));
			AddClearUAVPass(GraphBuilder, CountsUAV, 0u);

			// Neighbor buffers
			FRDGBufferSRVRef NeighborXNSRV = CreateNeighborBufferSRV(GraphBuilder, TEXT("VoxelGPU.NeighborXN"), Neighbors->XN, Params.bHasNeighborXN);
			FRDGBufferSRVRef NeighborXPSRV = CreateNeighborBufferSRV(GraphBuilder, TEXT("VoxelGPU.NeighborXP"), Neighbors->XP, Params.bHasNeighborXP);
			FRDGBufferSRVRef NeighborYNSRV = CreateNeighborBufferSRV(GraphBuilder, TEXT("VoxelGPU.NeighborYN"), Neighbors->YN, Params.bHasNeighborYN);
			FRDGBufferSRVRef NeighborYPSRV = CreateNeighborBufferSRV(GraphBuilder, TEXT("VoxelGPU.NeighborYP"), Neighbors->YP, Params.bHasNeighborYP);
			FRDGBufferSRVRef NeighborZNSRV = CreateNeighborBufferSRV(GraphBuilder, TEXT("VoxelGPU.NeighborZN"), Neighbors->ZN, Params.bHasNeighborZN);
			FRDGBufferSRVRef NeighborZPSRV = CreateNeighborBufferSRV(GraphBuilder, TEXT("VoxelGPU.NeighborZP"), Neighbors->ZP, Params.bHasNeighborZP);

			// Dummy outputs for count pass
			FRDGBufferRef DummyOutBuf = CreateStructuredBuffer(GraphBuilder, TEXT("VoxelGPU.CountDummyOut"), sizeof(uint32), 1, nullptr, 0, ERDGInitialDataFlags::None);
			FRDGBufferUAVRef DummyOutUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(DummyOutBuf));
			AddClearUAVPass(GraphBuilder, DummyOutUAV, 0u);

			FRDGBufferRef DummyOffsetsBuf = CreateStructuredBuffer(GraphBuilder, TEXT("VoxelGPU.CountDummyOffsets"), sizeof(uint32), 1, nullptr, 0, ERDGInitialDataFlags::None);
			FRDGBufferSRVRef DummyOffsetsSRV = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(DummyOffsetsBuf));

			// Shader parameters
			TShaderMapRef<FGPUCountElementsCS> CS(GetGlobalShaderMap(GMaxRHIFeatureLevel));
			FGPUCountElementsCS::FParameters* ShaderParams = GraphBuilder.AllocParameters<FGPUCountElementsCS::FParameters>();
			ShaderParams->SizeX = static_cast<uint32>(Params.SizeX);
			ShaderParams->SizeY = static_cast<uint32>(Params.SizeY);
			ShaderParams->SizeZ = static_cast<uint32>(Params.SizeZ);
			ShaderParams->XYScale = static_cast<uint32>(FMath::Max(1, Params.XYScale));
			ShaderParams->DefaultAO = static_cast<uint32>(Params.DefaultAO);
			ShaderParams->InVoxels = VoxSRV;
			ShaderParams->OutCount = CountsUAV;
			ShaderParams->NeighborXN = NeighborXNSRV;
			ShaderParams->NeighborXP = NeighborXPSRV;
			ShaderParams->NeighborYN = NeighborYNSRV;
			ShaderParams->NeighborYP = NeighborYPSRV;
			ShaderParams->NeighborZN = NeighborZNSRV;
			ShaderParams->NeighborZP = NeighborZPSRV;
			ShaderParams->bHasNeighborXN = Params.bHasNeighborXN ? 1u : 0u;
			ShaderParams->bHasNeighborXP = Params.bHasNeighborXP ? 1u : 0u;
			ShaderParams->bHasNeighborYN = Params.bHasNeighborYN ? 1u : 0u;
			ShaderParams->bHasNeighborYP = Params.bHasNeighborYP ? 1u : 0u;
			ShaderParams->bHasNeighborZN = Params.bHasNeighborZN ? 1u : 0u;
			ShaderParams->bHasNeighborZP = Params.bHasNeighborZP ? 1u : 0u;
			ShaderParams->OutVerts = DummyOutUAV;
			ShaderParams->MaxVerts = 0u;
			ShaderParams->DisableGreedyMerge = 0u;

			// Check console variables
			static auto* CVarIgnNbh = IConsoleManager::Get().FindConsoleVariable(TEXT("r.Voxel.GPU.IgnoreNeighbors"));
			const bool bIgnoreNeighbors = Params.bAggressiveCulling || (CVarIgnNbh && CVarIgnNbh->GetInt() != 0);
			ShaderParams->IgnoreNeighbors = bIgnoreNeighbors ? 1u : 0u;

			// Debug logging (once per session)
			static bool bLoggedOnce = false;
			if (!bLoggedOnce)
			{
				bLoggedOnce = true;
				UE_LOG(LogTemp, Log, TEXT("[VoxelGPU Count] IgnoreNeighbors=%d, bAggressiveCulling=%d"),
					ShaderParams->IgnoreNeighbors, Params.bAggressiveCulling ? 1 : 0);
			}

			ShaderParams->NeighborLayoutX = 0u;
			ShaderParams->NeighborLayoutY = 0u;
			ShaderParams->NeighborLayoutZ = 0u;
			ShaderParams->DirectionMask = 0u;
			ShaderParams->Offsets = DummyOffsetsSRV;

			// Dispatch count shader (8×8×8 thread groups)
			const FIntVector Groups(
				FMath::DivideAndRoundUp(Params.SizeX, 8),
				FMath::DivideAndRoundUp(Params.SizeY, 8),
				FMath::DivideAndRoundUp(Params.SizeZ, 8));

			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("VoxelGPU Count"), CS, ShaderParams, Groups);
			AddEnqueueCopyPass(GraphBuilder, CountRB, CountsBuf, 0);
			GraphBuilder.Execute();
		});

	// Flush GPU commands
	ENQUEUE_RENDER_COMMAND(VoxelGPU_TwoPass_CountFlush)(
		[](FRHICommandListImmediate& RHICmdList)
		{
			RHICmdList.SubmitCommandsAndFlushGPU();
		});

	// Poll for count readback (non-blocking wait)
	while (!CountReadback->IsReady())
	{
		FPlatformProcess::Sleep(0.001f);
	}

	// Readback count results to CPU
	ENQUEUE_RENDER_COMMAND(VoxelGPU_TwoPass_CountReadback)(
		[CountRB = CountReadback.Get(), CountsPtr = CountsCPU.GetData(), VolCount, CountDone]
		(FRHICommandListImmediate& RHICmdList)
		{
			// Readback is ready
			const void* Ptr = CountRB->Lock(static_cast<int64>(VolCount) * sizeof(uint32));
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

	// ========== CPU PREFIX SCAN ==========

	const uint64 MaxSupportedElems = (Params.MaxOutputVerts > 0) ? static_cast<uint64>(Params.MaxOutputVerts) : static_cast<uint64>(INT32_MAX);
	uint64 TotalElems64 = 0;
	bool bOverflow = false;

	TArray<uint32> OffsetsCPU;
	OffsetsCPU.SetNumUninitialized(VolCount);

	for (int32 i = 0; i < VolCount; ++i)
	{
		OffsetsCPU[i] = static_cast<uint32>(TotalElems64);
		TotalElems64 += CountsCPU[i];

		if (TotalElems64 > MaxSupportedElems)
		{
			bOverflow = true;
			break;
		}
	}

	if (bOverflow)
	{
		UE_LOG(LogTemp, Warning, TEXT("[VoxelGPU] GPU mesher exceeded vertex cap (%llu >= %llu); falling back to CPU."),
			TotalElems64, MaxSupportedElems);
		return false;
	}

	const int32 TotalElems = static_cast<int32>(TotalElems64);
	if (TotalElems <= 0)
	{
		// Empty chunk - success with no output
		return true;
	}

	// ========== PASS 2: EMIT VERTICES USING OFFSETS ==========

	TArray<uint32> PackedCPU;
	PackedCPU.SetNumUninitialized(TotalElems);

	FEvent* EmitDone = FPlatformProcess::GetSynchEventFromPool(false);

	ENQUEUE_RENDER_COMMAND(VoxelGPU_TwoPass_Emit)(
		[Params, Offsets = MoveTemp(OffsetsCPU), Neighbors = NeighborData, VertsRB = VertsReadback.Get(), TotalElems]
		(FRHICommandListImmediate& RHICmdList) mutable
		{
			const int32 VolCountRT = Params.SizeX * Params.SizeY * Params.SizeZ;

			// Re-stage voxel data (can't reuse from count pass - different render command)
			TArray<uint32> VoxelsCPU;
			VoxelsCPU.SetNumUninitialized(VolCountRT);
			for (int32 i = 0; i < VolCountRT; ++i)
			{
				VoxelsCPU[i] = static_cast<uint32>(Params.Voxels[i]);
			}

			FRDGBuilder GraphBuilder(RHICmdList);

			// Input voxel buffer
			FRDGBufferRef VoxBuf = CreateStructuredBuffer(GraphBuilder, TEXT("VoxelGPU.InVoxels"),
				sizeof(uint32), VoxelsCPU.Num(), VoxelsCPU.GetData(), VoxelsCPU.Num() * sizeof(uint32), ERDGInitialDataFlags::None);
			FRDGBufferSRVRef VoxSRV = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(VoxBuf));

			// Offset buffer (computed by CPU prefix scan)
			FRDGBufferRef OffsBuf = CreateStructuredBuffer(GraphBuilder, TEXT("VoxelGPU.Offsets"),
				sizeof(uint32), Offsets.Num(), Offsets.GetData(), Offsets.Num() * sizeof(uint32), ERDGInitialDataFlags::None);
			FRDGBufferSRVRef OffsSRV = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(OffsBuf));

			// Output vertex buffer
			FRDGBufferRef OutBuf = CreateStructuredBuffer(GraphBuilder, TEXT("VoxelGPU.OutVerts"),
				sizeof(uint32), TotalElems, nullptr, 0, ERDGInitialDataFlags::None);
			FRDGBufferUAVRef OutUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(OutBuf));
			AddClearUAVPass(GraphBuilder, OutUAV, 0u);

			// Neighbor buffers
			FRDGBufferSRVRef NeighborXNSRV = CreateNeighborBufferSRV(GraphBuilder, TEXT("VoxelGPU.NeighborXN"), Neighbors->XN, Params.bHasNeighborXN);
			FRDGBufferSRVRef NeighborXPSRV = CreateNeighborBufferSRV(GraphBuilder, TEXT("VoxelGPU.NeighborXP"), Neighbors->XP, Params.bHasNeighborXP);
			FRDGBufferSRVRef NeighborYNSRV = CreateNeighborBufferSRV(GraphBuilder, TEXT("VoxelGPU.NeighborYN"), Neighbors->YN, Params.bHasNeighborYN);
			FRDGBufferSRVRef NeighborYPSRV = CreateNeighborBufferSRV(GraphBuilder, TEXT("VoxelGPU.NeighborYP"), Neighbors->YP, Params.bHasNeighborYP);
			FRDGBufferSRVRef NeighborZNSRV = CreateNeighborBufferSRV(GraphBuilder, TEXT("VoxelGPU.NeighborZN"), Neighbors->ZN, Params.bHasNeighborZN);
			FRDGBufferSRVRef NeighborZPSRV = CreateNeighborBufferSRV(GraphBuilder, TEXT("VoxelGPU.NeighborZP"), Neighbors->ZP, Params.bHasNeighborZP);

			// Dummy count buffer for emit pass
			FRDGBufferRef DummyCountBuf = CreateStructuredBuffer(GraphBuilder, TEXT("VoxelGPU.EmitDummyCount"), sizeof(uint32), 1, nullptr, 0, ERDGInitialDataFlags::None);
			FRDGBufferUAVRef DummyCountUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(DummyCountBuf, PF_R32_UINT));
			AddClearUAVPass(GraphBuilder, DummyCountUAV, 0u);

			// Shader parameters
			TShaderMapRef<FGPUEmitElementsCS> CS(GetGlobalShaderMap(GMaxRHIFeatureLevel));
			FGPUEmitElementsCS::FParameters* ShaderParams = GraphBuilder.AllocParameters<FGPUEmitElementsCS::FParameters>();
			ShaderParams->SizeX = static_cast<uint32>(Params.SizeX);
			ShaderParams->SizeY = static_cast<uint32>(Params.SizeY);
			ShaderParams->SizeZ = static_cast<uint32>(Params.SizeZ);
			ShaderParams->XYScale = static_cast<uint32>(FMath::Max(1, Params.XYScale));
			ShaderParams->DefaultAO = static_cast<uint32>(Params.DefaultAO);
			ShaderParams->InVoxels = VoxSRV;
			ShaderParams->Offsets = OffsSRV;
			ShaderParams->OutVerts = OutUAV;
			ShaderParams->NeighborXN = NeighborXNSRV;
			ShaderParams->NeighborXP = NeighborXPSRV;
			ShaderParams->NeighborYN = NeighborYNSRV;
			ShaderParams->NeighborYP = NeighborYPSRV;
			ShaderParams->NeighborZN = NeighborZNSRV;
			ShaderParams->NeighborZP = NeighborZPSRV;
			ShaderParams->bHasNeighborXN = Params.bHasNeighborXN ? 1u : 0u;
			ShaderParams->bHasNeighborXP = Params.bHasNeighborXP ? 1u : 0u;
			ShaderParams->bHasNeighborYN = Params.bHasNeighborYN ? 1u : 0u;
			ShaderParams->bHasNeighborYP = Params.bHasNeighborYP ? 1u : 0u;
			ShaderParams->bHasNeighborZN = Params.bHasNeighborZN ? 1u : 0u;
			ShaderParams->bHasNeighborZP = Params.bHasNeighborZP ? 1u : 0u;
			ShaderParams->OutCount = DummyCountUAV;
			ShaderParams->MaxVerts = static_cast<uint32>(TotalElems);

			// Console variables
			static auto* CVarDisableMerge = IConsoleManager::Get().FindConsoleVariable(TEXT("r.Voxel.GPU.DisableGreedyMerge"));
			ShaderParams->DisableGreedyMerge = CVarDisableMerge ? static_cast<uint32>(CVarDisableMerge->GetInt()) : 0u;

			static auto* CVarIgnNbh = IConsoleManager::Get().FindConsoleVariable(TEXT("r.Voxel.GPU.IgnoreNeighbors"));
			const bool bIgnoreNeighbors = Params.bAggressiveCulling || (CVarIgnNbh && CVarIgnNbh->GetInt() != 0);
			ShaderParams->IgnoreNeighbors = bIgnoreNeighbors ? 1u : 0u;

			ShaderParams->NeighborLayoutX = 0u;
			ShaderParams->NeighborLayoutY = 0u;
			ShaderParams->NeighborLayoutZ = 0u;
			ShaderParams->DirectionMask = 0x3Fu;  // All 6 directions enabled

			// Dispatch emit shader
			const FIntVector Groups(
				FMath::DivideAndRoundUp(Params.SizeX, 8),
				FMath::DivideAndRoundUp(Params.SizeY, 8),
				FMath::DivideAndRoundUp(Params.SizeZ, 8));

			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("VoxelGPU Emit"), CS, ShaderParams, Groups);
			AddEnqueueCopyPass(GraphBuilder, VertsRB, OutBuf, 0);
			GraphBuilder.Execute();
		});

	// Flush GPU commands
	ENQUEUE_RENDER_COMMAND(VoxelGPU_TwoPass_EmitFlush)(
		[](FRHICommandListImmediate& RHICmdList)
		{
			RHICmdList.SubmitCommandsAndFlushGPU();
		});

	// Poll for vertex readback (non-blocking wait)
	while (!VertsReadback->IsReady())
	{
		FPlatformProcess::Sleep(0.001f);
	}

	// Readback vertex results to CPU
	ENQUEUE_RENDER_COMMAND(VoxelGPU_TwoPass_EmitReadback)(
		[VertsRB = VertsReadback.Get(), PackedPtr = PackedCPU.GetData(), TotalElems, EmitDone]
		(FRHICommandListImmediate& RHICmdList)
		{
			// Readback is ready
			const void* Ptr = VertsRB->Lock(static_cast<int64>(TotalElems) * sizeof(uint32));
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
// PUBLIC API IMPLEMENTATION
// ============================================================================

bool FVoxelGPUMesher::BuildPackedVerts_GPU(const FGPUMeshBuildParams& Params, TArray<uint32>& OutPackedVerts)
{
	// Check which implementation to use
	static auto* CVarUseSinglePass = IConsoleManager::Get().FindConsoleVariable(TEXT("r.Voxel.GPU.UseSinglePass"));
	static auto* CVarUseIndexBuffer = IConsoleManager::Get().FindConsoleVariable(TEXT("r.Voxel.GPU.UseIndexBuffer"));

	const bool bUseSinglePass = CVarUseSinglePass ? (CVarUseSinglePass->GetInt() != 0) : true;
	const bool bUseIndexBuffer = CVarUseIndexBuffer ? (CVarUseIndexBuffer->GetInt() != 0) : false;

	// Lambda to select and call the appropriate implementation
	auto ExecuteImpl = [&Params, &OutPackedVerts, bUseSinglePass, bUseIndexBuffer]() -> bool
	{
		if (bUseSinglePass)
		{
			// Single-pass implementation
			TArray<uint32> Indices;
			bool bSuccess = BuildPackedVerts_GPU_SinglePass(Params, OutPackedVerts, Indices, bUseIndexBuffer);

			if (bSuccess && bUseIndexBuffer && Indices.Num() > 0)
			{
				// Convert indexed format back to non-indexed for API compatibility
				// TODO: In the future, expose index buffer through API
				TArray<uint32> ExpandedVerts;
				ExpandedVerts.Reserve(Indices.Num());
				for (uint32 Idx : Indices)
				{
					if (Idx < static_cast<uint32>(OutPackedVerts.Num()))
					{
						ExpandedVerts.Add(OutPackedVerts[Idx]);
					}
				}
				OutPackedVerts = MoveTemp(ExpandedVerts);
			}

			return bSuccess;
		}
		else
		{
			// Two-pass implementation (legacy fallback)
			return BuildPackedVerts_GPU_TwoPass(Params, OutPackedVerts);
		}
	};

	// If already on game thread, call directly
	if (IsInGameThread())
	{
		return ExecuteImpl();
	}

	// Otherwise, marshal to game thread and wait
	TArray<uint32> LocalOut;
	bool bSuccess = false;
	FEvent* Done = FPlatformProcess::GetSynchEventFromPool(false);

	// Copy params by value to avoid lifetime issues
	FGPUMeshBuildParams ParamsCopy = Params;

	AsyncTask(ENamedThreads::GameThread, [ParamsCopy, Done, &LocalOut, &bSuccess, bUseSinglePass, bUseIndexBuffer]()
		{
			if (bUseSinglePass)
			{
				TArray<uint32> Indices;
				bSuccess = BuildPackedVerts_GPU_SinglePass(ParamsCopy, LocalOut, Indices, bUseIndexBuffer);

				if (bSuccess && bUseIndexBuffer && Indices.Num() > 0)
				{
					// Convert indexed format back to non-indexed
					TArray<uint32> ExpandedVerts;
					ExpandedVerts.Reserve(Indices.Num());
					for (uint32 Idx : Indices)
					{
						if (Idx < static_cast<uint32>(LocalOut.Num()))
						{
							ExpandedVerts.Add(LocalOut[Idx]);
						}
					}
					LocalOut = MoveTemp(ExpandedVerts);
				}
			}
			else
			{
				bSuccess = BuildPackedVerts_GPU_TwoPass(ParamsCopy, LocalOut);
			}
			Done->Trigger();
		});

	Done->Wait();
	FPlatformProcess::ReturnSynchEventToPool(Done);

	if (bSuccess)
	{
		OutPackedVerts = MoveTemp(LocalOut);
	}
	return bSuccess;
}

bool FVoxelGPUMesher::BuildPackedVerts_GPU_Async(const FGPUMeshBuildParams& Params, TFunction<void(bool, TArray<uint32>&&)> Completion)
{
	// Validate prerequisites
	if (!GDynamicRHI)
	{
		return false;
	}
	if (!Completion)
	{
		return false;
	}
	if (!Params.Voxels || Params.SizeX <= 0 || Params.SizeY <= 0 || Params.SizeZ <= 0)
	{
		return false;
	}

	const int32 VolCount = Params.SizeX * Params.SizeY * Params.SizeZ;

	// Estimate maximum output size (worst case: all voxels emit 6 faces)
	const int32 MaxFaces = VolCount * 6;
	const int32 MaxOutputVerts = MaxFaces * 6;  // 6 vertices per face (2 triangles)

	// Apply capacity limit
	const int32 ActualMaxVerts = (Params.MaxOutputVerts > 0)
		? FMath::Min(MaxOutputVerts, Params.MaxOutputVerts)
		: MaxOutputVerts;

	// Create async job
	TSharedPtr<FVoxelGPUAsyncJob, ESPMode::ThreadSafe> Job = MakeShared<FVoxelGPUAsyncJob, ESPMode::ThreadSafe>();
	Job->SizeX = Params.SizeX;
	Job->SizeY = Params.SizeY;
	Job->SizeZ = Params.SizeZ;
	Job->XYScale = Params.XYScale;
	Job->DefaultAO = Params.DefaultAO;
	Job->VolCount = VolCount;
	Job->MaxOutputVerts = Params.MaxOutputVerts;
	Job->bAggressiveCulling = Params.bAggressiveCulling;
	Job->Completion = MoveTemp(Completion);

	// Copy voxel data to shared ptr (avoid dangling pointers)
	Job->Voxels = MakeShared<TArray<uint32>, ESPMode::ThreadSafe>();
	Job->Voxels->SetNumUninitialized(VolCount);
	for (int32 i = 0; i < VolCount; ++i)
	{
		(*Job->Voxels)[i] = static_cast<uint32>(Params.Voxels[i]);
	}

	// Copy neighbor data
	Job->Neighbors = MakeShared<FGPUNeighborArrays, ESPMode::ThreadSafe>();
	CopyNeighborArrays(Params, *Job->Neighbors);

	// Allocate readbacks for single-pass
	Job->VertexCounterReadback = MakeUnique<FRHIGPUBufferReadback>(TEXT("VoxelGPU_VertexCounterAsync"));
	Job->VertsReadback = MakeUnique<FRHIGPUBufferReadback>(TEXT("VoxelGPU_VertsAsync"));

	// Enqueue single-pass shader on render thread
	TSharedPtr<FVoxelGPUAsyncJob, ESPMode::ThreadSafe> JobPtr = Job;
	TSharedPtr<TArray<uint32>, ESPMode::ThreadSafe> VoxelsShared = Job->Voxels;
	TSharedPtr<FGPUNeighborArrays, ESPMode::ThreadSafe> NeighborsShared = Job->Neighbors;
	FRHIGPUBufferReadback* VertCountRBPtr = Job->VertexCounterReadback.Get();
	FRHIGPUBufferReadback* VertsRBPtr = Job->VertsReadback.Get();
	const uint32 SizeX = static_cast<uint32>(Job->SizeX);
	const uint32 SizeY = static_cast<uint32>(Job->SizeY);
	const uint32 SizeZ = static_cast<uint32>(Job->SizeZ);
	const uint32 XYScale = static_cast<uint32>(Params.XYScale);
	const uint32 DefaultAO = static_cast<uint32>(Params.DefaultAO);

	ENQUEUE_RENDER_COMMAND(VoxelGPU_AsyncSinglePass)(
		[JobPtr, VoxelsShared, NeighborsShared, VertCountRBPtr, VertsRBPtr, SizeX, SizeY, SizeZ, XYScale, DefaultAO, ActualMaxVerts]
		(FRHICommandListImmediate& RHICmdList)
		{
			FRDGBuilder GraphBuilder(RHICmdList);

			// Input voxel buffer
			FRDGBufferRef VoxBuf = CreateStructuredBuffer(GraphBuilder, TEXT("VoxelGPU.InVoxels"),
				sizeof(uint32), VoxelsShared->Num(), VoxelsShared->GetData(), VoxelsShared->Num() * sizeof(uint32), ERDGInitialDataFlags::None);
			FRDGBufferSRVRef VoxSRV = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(VoxBuf));

			// Output vertex buffer (pre-allocated to max size)
			FRDGBufferRef OutVertsBuf = CreateStructuredBuffer(GraphBuilder, TEXT("VoxelGPU.OutVerts"),
				sizeof(uint32), ActualMaxVerts, nullptr, 0, ERDGInitialDataFlags::None);
			FRDGBufferUAVRef OutVertsUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(OutVertsBuf));
			AddClearUAVPass(GraphBuilder, OutVertsUAV, 0u);

			// Global vertex counter (atomic allocation)
			FRDGBufferRef VertexCounterBuf = CreateStructuredBuffer(GraphBuilder, TEXT("VoxelGPU.VertexCounter"),
				sizeof(uint32), 1, nullptr, 0, ERDGInitialDataFlags::None);
			FRDGBufferUAVRef VertexCounterUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(VertexCounterBuf, PF_R32_UINT));
			AddClearUAVPass(GraphBuilder, VertexCounterUAV, 0u);

			// Dummy index buffer (not used in async mode)
			FRDGBufferRef DummyIndicesBuf = CreateStructuredBuffer(GraphBuilder, TEXT("VoxelGPU.DummyIndices"),
				sizeof(uint32), 1, nullptr, 0, ERDGInitialDataFlags::None);
			FRDGBufferUAVRef DummyIndicesUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(DummyIndicesBuf));
			AddClearUAVPass(GraphBuilder, DummyIndicesUAV, 0u);

			FRDGBufferRef DummyIndexCounterBuf = CreateStructuredBuffer(GraphBuilder, TEXT("VoxelGPU.DummyIndexCounter"),
				sizeof(uint32), 1, nullptr, 0, ERDGInitialDataFlags::None);
			FRDGBufferUAVRef DummyIndexCounterUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(DummyIndexCounterBuf, PF_R32_UINT));
			AddClearUAVPass(GraphBuilder, DummyIndexCounterUAV, 0u);

			// Neighbor buffers
			FRDGBufferSRVRef NeighborXNSRV = CreateNeighborBufferSRV(GraphBuilder, TEXT("VoxelGPU.NeighborXN"), NeighborsShared->XN, NeighborsShared->XN.Num() > 0);
			FRDGBufferSRVRef NeighborXPSRV = CreateNeighborBufferSRV(GraphBuilder, TEXT("VoxelGPU.NeighborXP"), NeighborsShared->XP, NeighborsShared->XP.Num() > 0);
			FRDGBufferSRVRef NeighborYNSRV = CreateNeighborBufferSRV(GraphBuilder, TEXT("VoxelGPU.NeighborYN"), NeighborsShared->YN, NeighborsShared->YN.Num() > 0);
			FRDGBufferSRVRef NeighborYPSRV = CreateNeighborBufferSRV(GraphBuilder, TEXT("VoxelGPU.NeighborYP"), NeighborsShared->YP, NeighborsShared->YP.Num() > 0);
			FRDGBufferSRVRef NeighborZNSRV = CreateNeighborBufferSRV(GraphBuilder, TEXT("VoxelGPU.NeighborZN"), NeighborsShared->ZN, NeighborsShared->ZN.Num() > 0);
			FRDGBufferSRVRef NeighborZPSRV = CreateNeighborBufferSRV(GraphBuilder, TEXT("VoxelGPU.NeighborZP"), NeighborsShared->ZP, NeighborsShared->ZP.Num() > 0);

			// Shader parameters
			TShaderMapRef<FGPUSinglePassMeshCS> CS(GetGlobalShaderMap(GMaxRHIFeatureLevel));
			FGPUSinglePassMeshCS::FParameters* ShaderParams = GraphBuilder.AllocParameters<FGPUSinglePassMeshCS::FParameters>();
			ShaderParams->SizeX = SizeX;
			ShaderParams->SizeY = SizeY;
			ShaderParams->SizeZ = SizeZ;
			ShaderParams->XYScale = XYScale;
			ShaderParams->DefaultAO = DefaultAO;
			ShaderParams->InVoxels = VoxSRV;
			ShaderParams->OutVerts = OutVertsUAV;
			ShaderParams->OutIndices = DummyIndicesUAV;
			ShaderParams->GlobalVertexCounter = VertexCounterUAV;
			ShaderParams->GlobalIndexCounter = DummyIndexCounterUAV;
			ShaderParams->NeighborXN = NeighborXNSRV;
			ShaderParams->NeighborXP = NeighborXPSRV;
			ShaderParams->NeighborYN = NeighborYNSRV;
			ShaderParams->NeighborYP = NeighborYPSRV;
			ShaderParams->NeighborZN = NeighborZNSRV;
			ShaderParams->NeighborZP = NeighborZPSRV;
			ShaderParams->bHasNeighborXN = NeighborsShared->XN.Num() > 0 ? 1u : 0u;
			ShaderParams->bHasNeighborXP = NeighborsShared->XP.Num() > 0 ? 1u : 0u;
			ShaderParams->bHasNeighborYN = NeighborsShared->YN.Num() > 0 ? 1u : 0u;
			ShaderParams->bHasNeighborYP = NeighborsShared->YP.Num() > 0 ? 1u : 0u;
			ShaderParams->bHasNeighborZN = NeighborsShared->ZN.Num() > 0 ? 1u : 0u;
			ShaderParams->bHasNeighborZP = NeighborsShared->ZP.Num() > 0 ? 1u : 0u;
			ShaderParams->MaxVerts = static_cast<uint32>(ActualMaxVerts);
			ShaderParams->MaxIndices = 0u;  // Not using index buffer in async mode
			ShaderParams->bUseIndexBuffer = 0u;
			ShaderParams->DisableGreedyMerge = 0u;
			ShaderParams->DirectionMask = 0x3Fu;  // All 6 directions

			static auto* CVarIgnNbh = IConsoleManager::Get().FindConsoleVariable(TEXT("r.Voxel.GPU.IgnoreNeighbors"));
			const bool bIgnoreNeighbors = JobPtr->bAggressiveCulling || (CVarIgnNbh && CVarIgnNbh->GetInt() != 0);
			ShaderParams->IgnoreNeighbors = bIgnoreNeighbors ? 1u : 0u;

			// Dispatch single-pass shader
			const FIntVector Groups(
				FMath::DivideAndRoundUp(static_cast<int32>(SizeX), 8),
				FMath::DivideAndRoundUp(static_cast<int32>(SizeY), 8),
				FMath::DivideAndRoundUp(static_cast<int32>(SizeZ), 8));

			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("VoxelGPU SinglePass Async"), CS, ShaderParams, Groups);

			// Readback counters and vertices
			AddEnqueueCopyPass(GraphBuilder, VertCountRBPtr, VertexCounterBuf, 0);
			AddEnqueueCopyPass(GraphBuilder, VertsRBPtr, OutVertsBuf, 0);
			GraphBuilder.Execute();
		});

	// Add job to global queue
	{
		FScopeLock Lock(&GVoxelGPUAsyncMutex);
		GVoxelGPUAsyncJobs.Add(Job);
	}

	return true;
}

void FVoxelGPUMesher::PumpAsyncReadbacks()
{
	// Get frame budget for GPU readbacks (0 = unlimited)
	// Priority: console variable > settings > default
	static auto* CVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.Voxel.GPU.MaxReadbacksPerFrame"));
	int32 MaxReadbacksPerFrame = CVar ? CVar->GetInt() : -1;

	if (MaxReadbacksPerFrame < 0)
	{
		// Use settings value
		const UVoxelSettings* Settings = GetDefault<UVoxelSettings>();
		MaxReadbacksPerFrame = Settings ? Settings->MaxGPUMeshReadbacksPerFrame : 20;
	}

	int32 ProcessedThisFrame = 0;

	TArray<TSharedPtr<FVoxelGPUAsyncJob, ESPMode::ThreadSafe>> CompletedJobs;

	{
		FScopeLock Lock(&GVoxelGPUAsyncMutex);

		// Iterate backwards to allow safe removal
		for (int32 Index = GVoxelGPUAsyncJobs.Num() - 1; Index >= 0; --Index)
		{
			// Frame budget check: stop processing if we hit the limit
			if (MaxReadbacksPerFrame > 0 && ProcessedThisFrame >= MaxReadbacksPerFrame)
			{
				break;  // Defer remaining readbacks to next frame
			}

			TSharedPtr<FVoxelGPUAsyncJob, ESPMode::ThreadSafe> Job = GVoxelGPUAsyncJobs[Index];
			if (!Job.IsValid())
			{
				GVoxelGPUAsyncJobs.RemoveAtSwap(Index);
				continue;
			}

			// ========== SINGLE-PASS: WAITING FOR READBACK ==========
			if (Job->Stage == EVoxelGPUAsyncStage::WaitingForSinglePass)
			{
				// Check if both readbacks are ready
				if (!Job->VertexCounterReadback || !Job->VertsReadback)
				{
					continue;  // Not ready yet
				}
				if (!Job->VertexCounterReadback->IsReady() || !Job->VertsReadback->IsReady())
				{
					continue;  // Not ready yet
				}

				// Count this as processed
				ProcessedThisFrame++;

				// Readback counter to get actual vertex count
				uint32 ActualVertexCount = 0;
				FEvent* CounterDone = FPlatformProcess::GetSynchEventFromPool(false);
				ENQUEUE_RENDER_COMMAND(VoxelGPU_AsyncCounterReadback)(
					[Job, &ActualVertexCount, CounterDone](FRHICommandListImmediate& RHICmdList)
					{
						if (Job->VertexCounterReadback)
						{
							const void* Ptr = Job->VertexCounterReadback->Lock(sizeof(uint32));
							if (Ptr)
							{
								FMemory::Memcpy(&ActualVertexCount, Ptr, sizeof(uint32));
							}
							Job->VertexCounterReadback->Unlock();
						}
						CounterDone->Trigger();
					});

				CounterDone->Wait();
				FPlatformProcess::ReturnSynchEventToPool(CounterDone);
				Job->VertexCounterReadback.Reset();

				// Validate vertex count
				const uint32 MaxAllowed = (Job->MaxOutputVerts > 0)
					? static_cast<uint32>(FMath::Min(Job->MaxOutputVerts, INT32_MAX))
					: static_cast<uint32>(INT32_MAX);

				// Empty chunk - complete successfully
				if (ActualVertexCount == 0)
				{
					Job->TotalElements = 0;
					Job->PackedResult.Reset();
					Job->bSuccess = true;
					Job->VertsReadback.Reset();
					GVoxelGPUAsyncJobs.RemoveAtSwap(Index);
					CompletedJobs.Add(Job);
					UE_LOG(LogTemp, Log, TEXT("[VoxelGPU SinglePass Async] Empty chunk, 0 vertices"));
					continue;
				}

				// Overflow - fail and fallback to CPU
				if (ActualVertexCount > MaxAllowed)
				{
					UE_LOG(LogTemp, Warning, TEXT("[VoxelGPU SinglePass Async] Exceeded vertex cap (%u > %u); falling back to CPU."),
						ActualVertexCount, MaxAllowed);
					Job->PackedResult.Reset();
					Job->bSuccess = false;
					Job->VertsReadback.Reset();
					GVoxelGPUAsyncJobs.RemoveAtSwap(Index);
					CompletedJobs.Add(Job);
					continue;
				}

				Job->TotalElements = static_cast<int32>(ActualVertexCount);

				// Readback vertices
				FEvent* VertsDone = FPlatformProcess::GetSynchEventFromPool(false);
				ENQUEUE_RENDER_COMMAND(VoxelGPU_AsyncVertsReadback)(
					[Job, VertsDone](FRHICommandListImmediate& RHICmdList)
					{
						if (Job->VertsReadback && Job->TotalElements > 0)
						{
							const void* Ptr = Job->VertsReadback->Lock(static_cast<int64>(Job->TotalElements) * sizeof(uint32));
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
						VertsDone->Trigger();
					});

				VertsDone->Wait();
				FPlatformProcess::ReturnSynchEventToPool(VertsDone);
				Job->VertsReadback.Reset();

				// Release shared data
				Job->Voxels.Reset();
				Job->Neighbors.Reset();

				UE_LOG(LogTemp, Log, TEXT("[VoxelGPU SinglePass Async] Generated %d vertices"), Job->TotalElements);

				GVoxelGPUAsyncJobs.RemoveAtSwap(Index);
				CompletedJobs.Add(Job);
			}
		}
	}

	// Invoke completion callbacks outside of mutex lock
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

	// Diagnostic logging (throttled to avoid spam)
	if (ProcessedThisFrame > 0)
	{
		static int32 LogCounter = 0;
		if (++LogCounter % 60 == 0)  // Log every 60 frames (~1 second at 60 FPS)
		{
			int32 PendingJobs = 0;
			{
				FScopeLock Lock(&GVoxelGPUAsyncMutex);
				PendingJobs = GVoxelGPUAsyncJobs.Num();
			}

			if (PendingJobs > 0)
			{
				UE_LOG(LogTemp, Log, TEXT("[VoxelGPU Frame Budget] Processed %d/%d readbacks, %d pending"),
					ProcessedThisFrame, MaxReadbacksPerFrame, PendingJobs);
			}
		}
	}
}

bool FVoxelGPUMesher::HasPendingAsyncReadbacks()
{
	FScopeLock Lock(&GVoxelGPUAsyncMutex);
	return GVoxelGPUAsyncJobs.Num() > 0;
}

// ============================================================================
// DECODE PACKED VERTICES TO MESH BUFFERS
// ============================================================================

/**
 * Convert face normal code (0..5) to direction vector.
 */
static FVector FaceNormalFromCode3(uint32 Code)
{
	switch (Code & 7u)
	{
	case 0: return FVector(1, 0, 0);   // +X
	case 1: return FVector(-1, 0, 0);  // -X
	case 2: return FVector(0, 1, 0);   // +Y
	case 3: return FVector(0, -1, 0);  // -Y
	case 4: return FVector(0, 0, 1);   // +Z
	case 5: return FVector(0, 0, -1);  // -Z
	default: return FVector(0, 0, 1);  // Fallback
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
	UE_LOG(LogTemp, Log, TEXT("[VoxelGPU Decode] Input: %d packed verts, Size=(%d,%d,%d), XYScale=%d"),
		Packed.Num(), SizeX, SizeY, SizeZ, XYScale);

	Out.Vertices.Reset();
	Out.Normals.Reset();
	Out.UVs.Reset();
	Out.Colors.Reset();
	Out.Triangles.Reset();

	// Voxel scale factors
	const double ScaleX = static_cast<double>(XYScale) * static_cast<double>(VoxelUU);
	const double ScaleY = static_cast<double>(XYScale) * static_cast<double>(VoxelUU);
	const double ScaleZ = static_cast<double>(VoxelUU);

	// Helper to unpack position (7 bits per component)
	auto UnpackPos = [](uint32 P) -> FIntVector
		{
			return FIntVector(
				static_cast<int32>((P) & 0x7Fu),
				static_cast<int32>((P >> 7) & 0x7Fu),
				static_cast<int32>((P >> 14) & 0x7Fu));
		};

	// Round to multiple of 3 (triangles)
	const int32 Count = Packed.Num() - (Packed.Num() % 3);
	if (Count != Packed.Num())
	{
		UE_LOG(LogTemp, Warning, TEXT("[VoxelGPU] Packed count %d not multiple of 3. Dropping %d vertices."),
			Packed.Num(), Packed.Num() - Count);
	}

	// Pre-allocate for performance
	Out.Vertices.Reserve(Count);
	Out.Normals.Reserve(Count);
	Out.UVs.Reserve(Count);
	Out.Colors.Reserve(Count);
	Out.Triangles.Reserve(Count);

	// Decode triangles
	for (int32 i = 0; i < Count; i += 3)
	{
		const uint32 p0 = Packed[i + 0];
		const uint32 p1 = Packed[i + 1];
		const uint32 p2 = Packed[i + 2];

		// Skip empty/invalid triangles (capacity clamping can produce zeros)
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

		// Bounds check (inclusive for face coordinates)
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

		// Convert to world space
		FVector A(L0.X * ScaleX, L0.Y * ScaleY, L0.Z * ScaleZ);
		FVector B(L1.X * ScaleX, L1.Y * ScaleY, L1.Z * ScaleZ);
		FVector C(L2.X * ScaleX, L2.Y * ScaleY, L2.Z * ScaleZ);

		// Decode normal (same for all 3 verts of a quad triangle)
		const uint32 NormalCode = (p0 >> 21) & 0x7u;
		const FVector N = FaceNormalFromCode3(NormalCode);

		const int32 vStart = Out.Vertices.Num();

		// Add vertices and normals
		Out.Vertices.Add(A); Out.Normals.Add(N);
		Out.Vertices.Add(B); Out.Normals.Add(N);
		Out.Vertices.Add(C); Out.Normals.Add(N);

		// Generate UVs (simple local-space mapping)
		Out.UVs.Add(FVector2D(static_cast<float>(L0.X) / 16.f, static_cast<float>(L0.Y) / 16.f));
		Out.UVs.Add(FVector2D(static_cast<float>(L1.X) / 16.f, static_cast<float>(L1.Y) / 16.f));
		Out.UVs.Add(FVector2D(static_cast<float>(L2.X) / 16.f, static_cast<float>(L2.Y) / 16.f));

		// Add triangle indices
		Out.Triangles.Add(vStart + 0);
		Out.Triangles.Add(vStart + 1);
		Out.Triangles.Add(vStart + 2);
	}

	UE_LOG(LogTemp, Log, TEXT("[VoxelGPU Decode] Output: %d vertices, %d triangles"),
		Out.Vertices.Num(), Out.Triangles.Num() / 3);
}
