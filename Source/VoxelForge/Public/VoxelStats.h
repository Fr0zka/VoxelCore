// VoxelStats.h
// Per-frame runtime counters for tile classification and meshing.
// Compteurs runtime par frame pour la classification et le meshing des tuiles.

#pragma once

#include "Stats/Stats.h"

DECLARE_STATS_GROUP(TEXT("VoxelForge"), STATGROUP_VoxelForge, STATCAT_Advanced);

DECLARE_DWORD_COUNTER_STAT_EXTERN(TEXT("Tiles Classified"), STAT_VoxelForgeTilesClassified, STATGROUP_VoxelForge, VOXELFORGE_API);
DECLARE_DWORD_COUNTER_STAT_EXTERN(TEXT("Tiles Skipped All Solid"), STAT_VoxelForgeTilesSkippedAllSolid, STATGROUP_VoxelForge, VOXELFORGE_API);
DECLARE_DWORD_COUNTER_STAT_EXTERN(TEXT("Tiles Skipped All Air"), STAT_VoxelForgeTilesSkippedAllAir, STATGROUP_VoxelForge, VOXELFORGE_API);
DECLARE_DWORD_COUNTER_STAT_EXTERN(TEXT("Tiles Meshed"), STAT_VoxelForgeTilesMeshed, STATGROUP_VoxelForge, VOXELFORGE_API);
DECLARE_DWORD_COUNTER_STAT_EXTERN(TEXT("Tiles Operator Stack Solid"), STAT_VoxelForgeTilesOpStackSolid, STATGROUP_VoxelForge, VOXELFORGE_API);
DECLARE_DWORD_COUNTER_STAT_EXTERN(TEXT("Tiles Operator Stack Air"), STAT_VoxelForgeTilesOpStackAir, STATGROUP_VoxelForge, VOXELFORGE_API);
DECLARE_DWORD_COUNTER_STAT_EXTERN(TEXT("Column Memo Hits"), STAT_VoxelForgeColumnMemoHit, STATGROUP_VoxelForge, VOXELFORGE_API);
DECLARE_DWORD_COUNTER_STAT_EXTERN(TEXT("Column Memo Misses"), STAT_VoxelForgeColumnMemoMiss, STATGROUP_VoxelForge, VOXELFORGE_API);
