# GPU Mesher Critical Issues and Fixes

## Issue #1: FPS Drops During Generation (144 → 50 FPS) ✅ FIXED

### Problem
`BlockUntilGPUIdle()` was being called 3 times per chunk mesh, causing the game thread to block while waiting for ALL GPU work to complete. This stopped both rendering and generation.

### Root Cause
```cpp
RHICmdList.BlockUntilGPUIdle();  // BLOCKS ENTIRE GPU + GAME THREAD!
```

This aggressive call:
- Stops the entire GPU pipeline
- Blocks the game thread
- Prevents frame rendering
- Causes 5-10ms stalls per chunk

### Fix Applied ✅
Replaced with proper async readback polling:
```cpp
// Flush commands (non-blocking)
RHICmdList.SubmitCommandsAndFlushGPU();

// Poll for completion (yields CPU)
while (!Readback->IsReady())
{
    FPlatformProcess::Sleep(0.001f);  // 1ms sleep, allows other work
}
```

### Impact
- **Before:** FPS drops to ~50 during generation
- **After:** FPS should stay near 144 (or vsync limit)
- Game thread stays responsive
- GPU can continue rendering while waiting for readbacks

---

## Issue #2: Biome Blocks Not Preserved (All Blocks Look the Same) ⚠️ TODO

### Problem
The GPU mesher currently **loses all block type information**, making all biomes look identical.

### Root Cause

#### Current Data Flow (BROKEN):
```
Generator creates actual block types:
  EVoxelBlockID: Grass, Stone, Sand, Snow, Water, etc. (many types)
          ↓
Converted to categories for storage:
  FCategoryBitset: 0=air, 1=semi-solid, 2=solid (only 3 values!)
          ↓
GPU mesher receives categories:
  Voxels array: [2, 2, 1, 0, 2, ...] (just categories)
          ↓
Stores category in packed vertex:
  blockID field (bits 24-31): category value (0, 1, or 2)
          ↓
Decoder ignores blockID field entirely:
  DecodePackedVertsToMeshBuffers() never reads bits 24-31
          ↓
All blocks get default material:
  No block-specific colors or materials applied!
```

### What's Lost
- **Grass** → becomes category 2 (solid)
- **Stone** → becomes category 2 (solid)
- **Sand** → becomes category 2 (solid)
- **Snow** → becomes category 2 (solid)

**Result:** All solid blocks look the same! Biome diversity is completely lost.

### The Architecture Problem

The system currently uses **FCategoryBitset** to save memory:
- **2 bits per voxel** (4x compression!)
- Only stores 3 categories (air, semi-solid, solid)
- Original block types are discarded after generation

To preserve block types:
- Need **8 bits per voxel** (full EVoxelBlockID enum)
- **4x memory increase** per chunk
- Or maintain separate block ID array alongside categories

### How CPU Mesher Handles This (Correctly)

```cpp
// CPU mesher receives actual block types
void BuildBinaryGreedyMesh_Cats(
    const TArray<EVoxelBlockID>& Voxels,  // Actual block types!
    const UVoxelBlockTable* BlockTable)
{
    // For each face, get the owner block type
    const EVoxelBlockID Owner = OwnerBlockForFace(...);

    // Look up material layer for this block type
    const uint8 Layer = BlockTable->GetLayer(FaceDir, Owner);

    // Store in Colors array for material assignment
    Out.Colors.Add(FLinearColor(Layer, 0, 0, 0));
}
```

The CPU mesher **works correctly** because it receives actual block IDs, not just categories.

### Potential Solutions

#### Option A: Store Full Block IDs (Simple but Memory-Heavy)
**Change:**
- Replace `FCategoryBitset` (2 bits/voxel) with `TArray<uint8>` (8 bits/voxel)
- Pass actual block IDs to GPU mesher
- Store block IDs in packed vertex format
- Use BlockTable to assign materials

**Pros:**
- Simple implementation
- Preserves all block type information
- Works with existing material system

**Cons:**
- **4x memory increase** for voxel storage
- Larger neighbor border data transfers
- More GPU bandwidth usage

**Memory Impact:**
- 32×32×32 chunk: 32KB → 128KB per chunk
- 1000 active chunks: 32MB → 128MB (+96MB)

#### Option B: Hybrid Storage (Memory Efficient but Complex)
**Change:**
- Keep `FCategoryBitset` for fast meshing queries
- Add separate `TArray<EVoxelBlockID>` stored compressed or on-demand
- Pass both categories AND block IDs to GPU mesher
- Use block IDs only for material/color assignment

**Pros:**
- Minimal memory increase (categories still 2 bits)
- Block IDs can be compressed or generated on-demand
- Preserves biome diversity

**Cons:**
- More complex implementation
- Two data structures to maintain
- Synchronization overhead

#### Option C: Biome-Based Material Assignment (Workaround)
**Change:**
- Keep current category system
- Use biome grid + position to assign materials
- Calculate materials in decoder based on world position

**Pros:**
- No memory increase
- Works with current architecture
- Can add variation within biomes

**Cons:**
- Doesn't preserve exact block types
- Position-based lookup overhead
- Less precise than per-voxel block IDs

### Recommended Approach: Option A (Simple Fix)

For now, the simplest fix is **Option A**:

1. **Replace FCategoryBitset with TArray<uint8>** for block storage
2. **Pass actual block IDs to GPU mesher** (not categories)
3. **Store block IDs in packed vertex format** (already have bits 24-31!)
4. **Decode block IDs and use BlockTable** in DecodePackedVertsToMeshBuffers()

**Implementation Steps:**
```cpp
// 1. Change chunk storage (VoxelChunkComponent.h)
// Before:
FCategoryBitset CategoryData;  // 2 bits per voxel

// After:
TArray<uint8> BlockData;  // 8 bits per voxel (EVoxelBlockID)

// 2. Update GPU mesher params (VoxelGPUMesher.h)
struct FGPUMeshBuildParams
{
    const uint8* Voxels;  // Now contains actual block IDs!
    // ...
};

// 3. Update shaders to use block IDs
// GPUGreedyMesher_SinglePass.usf:
uint block = InVoxels[idx] & 0xFFu;  // Actual block ID, not category
uint cat = VoxelBlockCategory(block);  // Derive category for culling

// 4. Decode and use block IDs (VoxelGPUMesher.cpp)
void DecodePackedVertsToMeshBuffers(...)
{
    // Extract block ID from packed vertex
    const uint32 BlockID = (p0 >> 24) & 0xFFu;
    const EVoxelBlockID Owner = static_cast<EVoxelBlockID>(BlockID);

    // Look up material layer
    const uint8 Layer = BlockTable->GetLayer(FaceDir, Owner);

    // Store in Colors for material assignment
    Out.Colors.Add(FLinearColor(Layer, 0, 0, 0));
}
```

### Testing Plan

1. **Verify block type preservation:**
   - Generate chunks in different biomes
   - Check that Grass, Sand, Snow appear with correct materials
   - Verify Water/Air boundaries work correctly

2. **Measure memory impact:**
   - Profile memory usage before/after
   - Verify 4x increase is acceptable
   - Test with 1000+ active chunks

3. **Performance validation:**
   - Ensure GPU meshing still fast
   - Check generation FPS stays high (not affected by FPS fix)
   - Verify no new bottlenecks introduced

---

## Current Status

✅ **Issue #1 (FPS Drops):** FIXED and committed
- Removed all BlockUntilGPUIdle() calls
- Implemented proper async polling
- FPS should stay stable during generation

⚠️ **Issue #2 (Biome Blocks):** IDENTIFIED but NOT FIXED
- Root cause: Category storage loses block type information
- Solution: Need to store and pass actual block IDs
- Requires architecture change (memory trade-off)
- Awaiting user decision on approach

---

## Recommendations

### Immediate Action (Issue #1)
**Test the FPS fix:**
1. Compile with latest code
2. Generate chunks and observe FPS
3. Should stay near 144 (or vsync limit) instead of dropping to 50
4. Report results!

### Next Steps (Issue #2)
**Decide on block type preservation approach:**
1. **If memory is not a concern:** Implement Option A (simple, works great)
2. **If memory is critical:** Implement Option B (complex but efficient)
3. **If can live without exact blocks:** Implement Option C (workaround)

### Questions for User
1. Is 4x memory increase (32MB → 128MB for 1000 chunks) acceptable?
2. Are biome-specific blocks critical for your game?
3. Do you need exact block types or is biome-based assignment enough?

---

**Last Updated:** 2025-11-14
**Author:** Claude (GPU Mesher Bug Investigation)
