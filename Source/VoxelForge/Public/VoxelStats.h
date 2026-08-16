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
DECLARE_DWORD_COUNTER_STAT_EXTERN(TEXT("Cave Bail Not Op Stack Sole Slot"), STAT_VoxelForgeCaveBailNotOpStackSoleSlot, STATGROUP_VoxelForge, VOXELFORGE_API);
DECLARE_DWORD_COUNTER_STAT_EXTERN(TEXT("Cave Bail Not Op Stack Boundary Tile"), STAT_VoxelForgeCaveBailNotOpStackBoundaryTile, STATGROUP_VoxelForge, VOXELFORGE_API);
DECLARE_DWORD_COUNTER_STAT_EXTERN(TEXT("Cave Bail Not Op Stack No Layout"), STAT_VoxelForgeCaveBailNotOpStackNoLayout, STATGROUP_VoxelForge, VOXELFORGE_API);
DECLARE_DWORD_COUNTER_STAT_EXTERN(TEXT("Cave Bail Not Op Stack Recheck"), STAT_VoxelForgeCaveBailNotOpStackRecheck, STATGROUP_VoxelForge, VOXELFORGE_API);
DECLARE_DWORD_COUNTER_STAT_EXTERN(TEXT("Cave Mix Out Of Layout"), STAT_VoxelForgeCaveMixOutOfLayout, STATGROUP_VoxelForge, VOXELFORGE_API);
DECLARE_DWORD_COUNTER_STAT_EXTERN(TEXT("Cave Mix Gap"), STAT_VoxelForgeCaveMixGap, STATGROUP_VoxelForge, VOXELFORGE_API);
DECLARE_DWORD_COUNTER_STAT_EXTERN(TEXT("Cave Mix Surface World"), STAT_VoxelForgeCaveMixSurfaceWorld, STATGROUP_VoxelForge, VOXELFORGE_API);
DECLARE_DWORD_COUNTER_STAT_EXTERN(TEXT("Cave Bail Two Cave Slots"), STAT_VoxelForgeCaveBailTwoCaveSlots, STATGROUP_VoxelForge, VOXELFORGE_API);
DECLARE_DWORD_COUNTER_STAT_EXTERN(TEXT("Cave Bail Params"), STAT_VoxelForgeCaveBailParams, STATGROUP_VoxelForge, VOXELFORGE_API);
DECLARE_DWORD_COUNTER_STAT_EXTERN(TEXT("Cave Bail Stack Verdict"), STAT_VoxelForgeCaveBailStackVerdict, STATGROUP_VoxelForge, VOXELFORGE_API);
DECLARE_DWORD_COUNTER_STAT_EXTERN(TEXT("Cave Bail Fold Conflict"), STAT_VoxelForgeCaveBailFoldConflict, STATGROUP_VoxelForge, VOXELFORGE_API);
DECLARE_DWORD_COUNTER_STAT_EXTERN(TEXT("Cave Bail Disturbance"), STAT_VoxelForgeCaveBailDisturbance, STATGROUP_VoxelForge, VOXELFORGE_API);
DECLARE_DWORD_COUNTER_STAT_EXTERN(TEXT("Cave Bail No Stack"), STAT_VoxelForgeCaveBailNoStack, STATGROUP_VoxelForge, VOXELFORGE_API);
DECLARE_DWORD_COUNTER_STAT_EXTERN(TEXT("Column Memo Hits"), STAT_VoxelForgeColumnMemoHit, STATGROUP_VoxelForge, VOXELFORGE_API);
DECLARE_DWORD_COUNTER_STAT_EXTERN(TEXT("Column Memo Misses"), STAT_VoxelForgeColumnMemoMiss, STATGROUP_VoxelForge, VOXELFORGE_API);
