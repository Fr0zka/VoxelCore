#include "VoxelStats.h"

// ============================================================================
// UNREAL ENGINE STAT SYSTEM DEFINITIONS
// ============================================================================
// Defines performance counters for profiling and debugging voxel terrain.
// View stats in-game with: stat VoxelWorld, stat VoxelTiming, stat VoxelMemory
// ============================================================================

// === WORLD STATE COUNTERS ===
// Track active and pending chunk counts for debugging load balancing
DEFINE_STAT(STAT_VoxelActiveChunks);
DEFINE_STAT(STAT_VoxelPendingChunks);
DEFINE_STAT(STAT_VoxelActiveMacroTiles);

// === TASK SYSTEM COUNTERS ===
// Monitor concurrent task scheduling (running tasks and queue sizes)
DEFINE_STAT(STAT_VoxelGenTasksRunning);
DEFINE_STAT(STAT_VoxelMeshTasksRunning);
DEFINE_STAT(STAT_VoxelGenQueueSize);
DEFINE_STAT(STAT_VoxelMeshQueueSize);
DEFINE_STAT(STAT_VoxelApplyQueueSize);
DEFINE_STAT(STAT_VoxelGPUGenJobsPending);

// === TIMING STATS ===
// Performance profiling for each pipeline stage
DEFINE_STAT(STAT_VoxelWorldTick);        // Overall world tick time
DEFINE_STAT(STAT_VoxelUpdateChunks);     // Chunk management (spawn/unload)
DEFINE_STAT(STAT_VoxelDrainApplyQueue);  // Mesh apply queue processing
DEFINE_STAT(STAT_VoxelPromotePendings);  // Pending → Active promotion
DEFINE_STAT(STAT_VoxelGeneration);       // Terrain generation
DEFINE_STAT(STAT_VoxelMeshing);          // Overall meshing time
DEFINE_STAT(STAT_VoxelGreedyMesh);       // CPU greedy mesher
DEFINE_STAT(STAT_VoxelGreedyBinary);     // CPU binary greedy mesher
DEFINE_STAT(STAT_VoxelHeightfieldMesh);  // Heightfield mesher (LOD2)
DEFINE_STAT(STAT_VoxelGPUMeshing);       // GPU mesher (if enabled)
DEFINE_STAT(STAT_VoxelApplyToPMC);       // Mesh buffer → ProceduralMeshComponent
DEFINE_STAT(STAT_VoxelSnapshotNeighbors);// Neighbor data extraction

// === MEMORY STATS ===
// Track memory usage for voxel data and mesh buffers
DEFINE_STAT(STAT_VoxelDataMemory);       // Voxel data arrays (categories, block IDs)
DEFINE_STAT(STAT_VoxelMeshBufferMemory); // Mesh vertex/index buffers
