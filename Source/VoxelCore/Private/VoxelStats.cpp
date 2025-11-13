#include "VoxelStats.h"

// Define stats
DEFINE_STAT(STAT_VoxelActiveChunks);
DEFINE_STAT(STAT_VoxelPendingChunks);
DEFINE_STAT(STAT_VoxelActiveMacroTiles);
DEFINE_STAT(STAT_VoxelGenTasksRunning);
DEFINE_STAT(STAT_VoxelMeshTasksRunning);
DEFINE_STAT(STAT_VoxelGenQueueSize);
DEFINE_STAT(STAT_VoxelMeshQueueSize);
DEFINE_STAT(STAT_VoxelApplyQueueSize);

DEFINE_STAT(STAT_VoxelWorldTick);
DEFINE_STAT(STAT_VoxelUpdateChunks);
DEFINE_STAT(STAT_VoxelDrainApplyQueue);
DEFINE_STAT(STAT_VoxelPromotePendings);
DEFINE_STAT(STAT_VoxelGeneration);
DEFINE_STAT(STAT_VoxelMeshing);
DEFINE_STAT(STAT_VoxelGreedyMesh);
DEFINE_STAT(STAT_VoxelHeightfieldMesh);
DEFINE_STAT(STAT_VoxelApplyToPMC);
DEFINE_STAT(STAT_VoxelSnapshotNeighbors);
DEFINE_STAT(STAT_VoxelGPUMeshing);
DEFINE_STAT(STAT_VoxelGreedyBinary);

DEFINE_STAT(STAT_VoxelDataMemory);
DEFINE_STAT(STAT_VoxelMeshBufferMemory);