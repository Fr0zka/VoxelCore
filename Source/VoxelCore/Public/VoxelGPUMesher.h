#pragma once

#include "CoreMinimal.h"
#include "Templates/Function.h"

struct FMeshBuffers;

/**
 * Packed vertex format for GPU mesher (32-bit per vertex).
 *
 * Bit layout:
 *  0..6:   posX (0..127) - Local voxel X coordinate
 *  7..13:  posY (0..127) - Local voxel Y coordinate
 * 14..20:  posZ (0..127) - Local voxel Z coordinate
 * 21..23:  normal (0..5) - Face direction code (0=+X, 1=-X, 2=+Y, 3=-Y, 4=+Z, 5=-Z)
 * 24..31:  blockID (0..255) - Voxel category (0=air, 1=semi-solid, 2=solid)
 *
 * This compact format minimizes GPU→CPU transfer bandwidth and allows
 * efficient storage in GPU buffers. Vertices are transmitted as triangles
 * (groups of 3 packed values).
 */
struct FPackedVoxelVert
{
	uint32 Packed = 0;
};

/**
 * Parameters for GPU mesh building.
 *
 * The GPU mesher operates on category data (0=air, 1=semi-solid, 2=solid)
 * and generates packed vertices that can be decoded on CPU or GPU.
 *
 * Neighbor borders are required for seamless chunk edges. Each neighbor
 * array stores a single slice of adjacent chunk data:
 * - XN/XP: YZ slices (SizeY × SizeZ)
 * - YN/YP: XZ slices (SizeX × SizeZ)
 * - ZN/ZP: XY slices (SizeX × SizeY)
 */
struct FGPUMeshBuildParams
{
	// Core voxel data (category values: 0=air, 1=semi-solid, 2=solid)
	const uint8* Voxels = nullptr;
	int32 SizeX = 0, SizeY = 0, SizeZ = 0;

	// LOD and rendering settings
	int32 XYScale = 1;          // XY coarsening factor (1=LOD0, 2+=LOD1+)
	uint8 DefaultAO = 3;        // Default ambient occlusion value (0..3)
	float VoxelUU = 100.f;      // Voxel size in Unreal Units

	// ==================== NEIGHBOR DATA FOR SEAMLESS EDGES ====================

	// XN: Left neighbor (-X direction, rightmost YZ slice of left chunk)
	const uint8* NeighborXN = nullptr;
	bool bHasNeighborXN = false;

	// XP: Right neighbor (+X direction, leftmost YZ slice of right chunk)
	const uint8* NeighborXP = nullptr;
	bool bHasNeighborXP = false;

	// YN: Back neighbor (-Y direction, frontmost XZ slice of back chunk)
	const uint8* NeighborYN = nullptr;
	bool bHasNeighborYN = false;

	// YP: Front neighbor (+Y direction, backmost XZ slice of front chunk)
	const uint8* NeighborYP = nullptr;
	bool bHasNeighborYP = false;

	// ZN: Bottom neighbor (-Z direction, topmost XY slice of bottom chunk)
	const uint8* NeighborZN = nullptr;
	bool bHasNeighborZN = false;

	// ZP: Top neighbor (+Z direction, bottommost XY slice of top chunk)
	const uint8* NeighborZP = nullptr;
	bool bHasNeighborZP = false;

	// ==================== PERFORMANCE TUNING ====================

	// Maximum output vertices (prevents GPU buffer overflow)
	int32 MaxOutputVerts = INT32_MAX;

	// Aggressive culling: ignore neighbor data and cull all external faces
	// (useful for LOD impostors where seams are acceptable)
	bool bAggressiveCulling = false;
};

/**
 * GPU-accelerated voxel mesher using compute shaders.
 *
 * This mesher implements a two-pass greedy meshing algorithm on the GPU:
 *
 * **Pass 1 (Count):**
 * - Each voxel thread counts how many vertices it will emit
 * - Results are read back to CPU
 *
 * **CPU Prefix Scan:**
 * - Computes output offsets for each voxel
 * - Validates total vertex count fits in buffer
 *
 * **Pass 2 (Emit):**
 * - Each voxel thread writes vertices to its pre-assigned offset
 * - No atomics required (lock-free parallel emission)
 *
 * The shader performs greedy quad merging similar to the CPU mesher,
 * but the exact topology may differ (which is acceptable per requirements).
 */
class VOXELCORE_API FVoxelGPUMesher
{
public:
	/**
	 * Synchronous GPU meshing.
	 * Blocks until both passes complete and results are available on CPU.
	 *
	 * @param Params Build parameters (voxel data, size, neighbors, etc.)
	 * @param OutPackedVerts Output packed vertices (groups of 3 = triangles)
	 * @return true if successful, false if GPU unavailable or overflow
	 */
	static bool BuildPackedVerts_GPU(
		const FGPUMeshBuildParams& Params,
		TArray<uint32>& OutPackedVerts);

	/**
	 * Asynchronous GPU meshing with callback.
	 * Returns immediately and invokes callback when complete.
	 *
	 * Use PumpAsyncReadbacks() to poll for completion.
	 *
	 * @param Params Build parameters (voxel data, size, neighbors, etc.)
	 * @param Completion Callback invoked on completion (bSuccess, PackedVerts)
	 * @return true if job was enqueued, false if invalid params
	 */
	static bool BuildPackedVerts_GPU_Async(
		const FGPUMeshBuildParams& Params,
		TFunction<void(bool bSuccess, TArray<uint32>&& PackedVerts)> Completion);

	/**
	 * Pump async readback queue.
	 * Call this regularly (e.g., per frame) to process completed GPU jobs.
	 * Invokes completion callbacks for finished jobs.
	 */
	static void PumpAsyncReadbacks();

	/**
	 * Check if any async jobs are pending.
	 * @return true if jobs are still in flight
	 */
	static bool HasPendingAsyncReadbacks();

	/**
	 * Decode packed vertices to standard mesh buffers.
	 * Converts compact GPU format to CPU-friendly FMeshBuffers.
	 *
	 * @param Packed Input packed vertices (groups of 3)
	 * @param Out Output mesh buffers (Vertices, Normals, UVs, Triangles, Colors)
	 * @param VoxelUU Voxel size in Unreal Units
	 * @param XYScale LOD scale factor
	 * @param SizeX Chunk X size (for bounds checking)
	 * @param SizeY Chunk Y size (for bounds checking)
	 * @param SizeZ Chunk Z size (for bounds checking)
	 */
	static void DecodePackedVertsToMeshBuffers(
		const TArray<uint32>& Packed,
		FMeshBuffers& Out,
		float VoxelUU,
		int32 XYScale,
		int32 SizeX,
		int32 SizeY,
		int32 SizeZ);
};
