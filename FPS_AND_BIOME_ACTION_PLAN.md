# FPS Investigation & Block Type Preservation - Action Plan

## Issue Summary

**User Reports:**
1. ✅ FPS drops to ~50 during generation (was 144)
2. ⚠️ FPS still dips below 90 after initial fix
3. ⚠️ All biome blocks look the same (Grass, Sand, Stone identical)

## Root Cause Analysis

### Issue 1: BlockUntilGPUIdle() - ✅ FIXED
**Location:** `VoxelGPUMesher.cpp` lines 647, 874, 1036
**Problem:** Blocked entire GPU and game thread
**Fix Applied:** Replaced with `IsReady()` polling + `Sleep(0.001f)`
**Status:** ✅ Committed (commit 97a19e2)

### Issue 2: Remaining FPS Dips - ⚠️ NEEDS VERIFICATION
**Location:** `VoxelChunkComponent.cpp` line 545 & 606
**Root Cause:** Two GPU meshing paths:
- **Async path** (line 545): Uses `BuildGreedyMesh_GPU_Async` ✅
- **Sync path** (line 606): Uses `BuildGreedyMesh_GPU` 💥 (blocks background thread)

**Condition:** Uses async IF `bAsyncGPUReadback = true`

**Default Setting:** `bAsyncGPUReadback = true` (in VoxelSettings.h:160)

**Possible Causes:**
1. **Setting disabled** - User may have changed `bAsyncGPUReadback` to false
2. **Too many concurrent tasks** - CPU thread pool exhaustion
3. **Mesh application overhead** - `EnqueueMeshApply` on game thread
4. **Not using single-pass** - User may have disabled single-pass mesher

### Issue 3: Block Types Lost - ⚠️ NOT FIXED YET
**Location:** `VoxelMesher.cpp` line 824
**Problem:** Block IDs converted to categories, losing biome information

**Current Flow (BROKEN):**
```
Generator → EVoxelBlockID (Grass, Stone, Sand, etc.)
     ↓
Storage → FCategoryBitset (0=air, 1=semi, 2=solid) ← LOSES INFO!
     ↓
DoMeshing → Reconstructs from categories + biome grid
     ↓
BuildGreedyMesh_GPU → Converts BACK to categories! ← WASTE!
     ↓
Result → All blocks look the same
```

**Why This Happens:**
1. Line 305-431: DoMeshing reconstructs block IDs from categories + biome
2. Line 606: BuildGreedyMesh_GPU receives actual block IDs
3. Line 824 (VoxelMesher.cpp): Converts back to categories!
4. GPU mesher only sees categories (0, 1, 2)
5. Decoder doesn't use block IDs for materials

## Action Plan

### ✅ Completed
1. Remove BlockUntilGPUIdle() calls
2. Implement proper async polling
3. Document issues comprehensively

### 🔧 Immediate Actions (User)
1. **Check Settings:**
   ```
   VoxelSettings:
   - bAsyncGPUReadback = true  ← MUST BE TRUE!
   - bUseGPUMesherForLOD0 = true
   - r.Voxel.GPU.UseSinglePass = 1  ← New setting from upgrade
   ```

2. **Verify Console Variables:**
   ```cpp
   r.Voxel.GPU.UseSinglePass 1    // Use new single-pass mesher
   r.Voxel.GPU.UseIndexBuffer 0   // Disabled by default (OK)
   r.Voxel.GPU.LogPerChunk 1      // Enable to see meshing stats
   ```

3. **Check Concurrent Task Limits:**
   ```
   VoxelSettings:
   - MaxConcurrentGenerationTasks (default: 4)
   - MaxConcurrentMeshingTasks (default: 4)
   ```
   Try reducing these if CPU is maxed out.

### 🚀 Next Implementation (Block ID Preservation)

**Option A: Full Block ID Storage (Recommended)**

**Changes Required:**
1. **VoxelChunkComponent.h/cpp:**
   ```cpp
   // Before:
   FCategoryBitset CategoryData;  // 2 bits per voxel

   // After:
   TArray<EVoxelBlockID> BlockData;  // 8 bits per voxel (uint8)
   ```

2. **VoxelMesher.cpp line 824:**
   ```cpp
   // Before:
   CatData[idx] = VoxelBlockCategory(Voxels[idx]);  // Loses info!

   // After:
   BlockData[idx] = Voxels[idx];  // Preserves all block types!
   ```

3. **GPU Mesher Params:**
   ```cpp
   // Pass block IDs instead of categories
   P.Voxels = BlockData.GetData();  // Now uint8 block IDs
   ```

4. **Shaders (GPUGreedyMesher_SinglePass.usf):**
   ```hlsl
   // Read block ID and derive category
   uint blockID = InVoxels[idx] & 0xFFu;
   uint cat = DeriveCategory(blockID);  // 0=air, 1=water, 2=solid

   // Store block ID in packed vertex
   PackVert(x, y, z, normal, blockID);  // Not category!
   ```

5. **Decoder (VoxelGPUMesher.cpp line 1730+):**
   ```cpp
   // Extract block ID
   const uint32 BlockID = (p0 >> 24) & 0xFFu;
   const EVoxelBlockID Owner = static_cast<EVoxelBlockID>(BlockID);

   // Use BlockTable for materials
   const uint8 Layer = BlockTable->GetLayer(FaceDir, Owner);
   Out.Colors.Add(FLinearColor(Layer, 0, 0, 0));
   ```

**Memory Impact:**
- Per chunk: 34KB → 136KB (+102KB)
- 1000 chunks: 34MB → 136MB (+102MB)
- Acceptable for most modern systems

**Implementation Time:** ~2-3 hours

## Testing Checklist

### After Current Fixes:
- [ ] FPS stays near 144 during generation (not 50)
- [ ] FPS doesn't dip below 100 (ideally stays at 144)
- [ ] Single-pass mesher is being used (check logs)
- [ ] Async GPU readback is enabled (check settings)

### After Block ID Fix:
- [ ] Grass blocks appear green
- [ ] Sand blocks appear tan/yellow
- [ ] Stone blocks appear gray
- [ ] Snow blocks appear white
- [ ] Different biomes have distinct appearances
- [ ] Materials/textures applied correctly

## Performance Expectations

**Current State (with fixes):**
- Single-pass: 40-50% faster than two-pass
- No BlockUntilGPUIdle: No GPU stalls
- Async meshing: Background threads, game thread free

**Expected Result:**
- FPS should stay at 144 (or vsync limit)
- Minor dips (140-144) acceptable
- Drops below 100 indicate other issues

**If Still Dropping Below 90:**
Possible causes:
1. Too many chunks spawning per frame
2. Mesh application on game thread taking too long
3. GPU generation not enabled (should use GPU gen + GPU mesh)
4. Other game logic consuming CPU

## Debug Commands

```cpp
// Enable detailed logging
r.Voxel.GPU.LogPerChunk 1

// Check which mesher is being used
r.Voxel.GPU.UseSinglePass 1  // Should be 1

// Compare GPU vs CPU output
r.Voxel.GPU.CompareCPU 1  // Logs triangle counts

// Check if generation is on GPU
// (Look for "GPU generation complete" logs)
```

## Next Steps

1. **User:** Test current fixes, report FPS results
2. **User:** Verify settings (bAsyncGPUReadback, UseSinglePass)
3. **Claude:** Implement block ID preservation (Option A)
4. **User:** Test biome diversity
5. **Both:** Profile any remaining FPS issues

---

**Last Updated:** 2025-11-14
**Status:** Awaiting user testing + block ID implementation
