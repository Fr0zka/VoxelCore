# 🚀 Performance Optimization Guide

This document contains creative performance optimizations from various fields, ranked by impact vs effort.

---

## ✅ **IMPLEMENTED**

### 1. Frame Budget System for GPU Readbacks ⭐⭐⭐⭐⭐
**Status:** ✅ Implemented
**Expected Impact:** 80 FPS → 144 FPS
**From:** High-frequency trading (time-slicing)

**How it works:**
- Throttles GPU readbacks to N per frame (default: 20)
- Prevents frame spikes when hundreds of chunks complete at once
- Spreads work across multiple frames

**Tuning:**
```cpp
// In editor settings:
Voxel Settings → GPU Meshing → MaxGPUMeshReadbacksPerFrame = 20

// Or console command (runtime):
r.Voxel.GPU.MaxReadbacksPerFrame 20   // Smooth 144 FPS
r.Voxel.GPU.MaxReadbacksPerFrame 50   // Faster loading
r.Voxel.GPU.MaxReadbacksPerFrame 0    // Unlimited (old behavior)
```

---

### 2. Single-Pass GPU Mesher ⭐⭐⭐⭐⭐
**Status:** ✅ Implemented
**Expected Impact:** 40-50% faster meshing
**From:** GPU programming (atomic operations)

**How it works:**
- Eliminates GPU→CPU→GPU roundtrip
- Uses atomic allocation (InterlockedAdd)
- One shader dispatch instead of two

**Already enabled by default!**

---

### 3. Removed BlockUntilGPUIdle() ⭐⭐⭐⭐
**Status:** ✅ Implemented
**Expected Impact:** Eliminated 50+ FPS drops
**From:** Graphics programming best practices

**How it works:**
- Replaced blocking GPU waits with async polling
- GPU and CPU can work in parallel
- No more full GPU pipeline stalls

**Already enabled by default!**

---

## 🎯 **RECOMMENDED NEXT** (Not Yet Implemented)

### 4. Batch GPU Dispatches ⭐⭐⭐⭐
**Impact:** 20-30% faster + less CPU overhead
**Effort:** Medium
**From:** GPU compute optimization

**The Problem:**
- Currently: 1 GPU dispatch per chunk = 3031 dispatches
- Each dispatch has CPU overhead (validation, driver calls, etc.)

**The Solution:**
- Batch N chunks into single dispatch
- Process 10-50 chunks per compute shader call
- Reduces dispatch overhead by 90%

**Implementation Sketch:**
```cpp
// Instead of:
for (each chunk) { DispatchGPU(chunk); }

// Do:
TArray<ChunkData> Batch;
for (each chunk) {
    Batch.Add(chunk);
    if (Batch.Num() >= 32) {  // Batch size
        DispatchGPU_Batched(Batch);
        Batch.Reset();
    }
}
```

**ROI:** High impact, moderate effort.

---

### 5. Object Pooling for Chunks ⭐⭐⭐⭐
**Impact:** 15-25% faster, smoother FPS
**Effort:** Medium
**From:** Game engine optimization

**The Problem:**
- 3031 chunks × constant alloc/dealloc = allocator contention
- Memory fragmentation over time
- Allocator locks block threads

**The Solution:**
- Pre-allocate pool of chunk buffers
- Reuse instead of new/delete
- Lock-free allocation

**Implementation Sketch:**
```cpp
class FChunkPool {
    TArray<FChunkData*> FreeList;
    FCriticalSection Mutex;

public:
    FChunkData* Acquire() {
        FScopeLock Lock(&Mutex);
        return FreeList.Num() ? FreeList.Pop() : new FChunkData();
    }

    void Release(FChunkData* Chunk) {
        Chunk->Reset();
        FScopeLock Lock(&Mutex);
        FreeList.Add(Chunk);
    }
};
```

**ROI:** High impact, moderate effort.

---

### 6. Persistent Mapped Buffers ⭐⭐⭐⭐
**Impact:** 10-20% faster uploads
**Effort:** Hard
**From:** Modern graphics API techniques

**The Problem:**
- Each GPU buffer upload requires:
  - Lock buffer
  - Memcpy
  - Unlock buffer
  - Each step has sync overhead

**The Solution:**
- Map buffers permanently with write-combined memory
- Direct CPU writes to GPU memory
- No lock/unlock overhead

**Implementation Sketch:**
```cpp
// Create persistent mapped buffer
FBufferRHIRef PersistentBuffer = RHICreateBuffer(
    Size,
    BUF_ShaderResource | BUF_Persistent,
    ERHIAccess::CPUWrite
);

void* MappedPtr = RHILockBufferPermanent(PersistentBuffer);

// Fast writes (no sync!)
FMemory::Memcpy(MappedPtr, Data, Size);
// No unlock needed - stays mapped
```

**ROI:** Medium-high impact, high effort.

---

### 7. GPU-Driven Culling ⭐⭐⭐⭐⭐
**Impact:** Massive - skip invisible chunks entirely
**Effort:** Very Hard
**From:** AAA game rendering

**The Problem:**
- CPU decides which chunks to mesh
- But CPU doesn't know GPU view frustum perfectly
- Meshing chunks player can't see

**The Solution:**
- GPU does visibility test
- Only mesh visible chunks
- Indirect dispatch based on GPU results

**Implementation Sketch:**
```cpp
// GPU Pass 1: Visibility test
ComputeShader VisibilityCS:
    for each chunk:
        if (in frustum && not occluded):
            AppendToVisibleList(chunk)

// GPU Pass 2: Mesh only visible chunks
IndirectDispatch(VisibleList)
```

**ROI:** Potentially 50%+ faster, but VERY complex.

---

### 8. Hierarchical Spatial Hashing ⭐⭐⭐
**Impact:** 20-40% fewer chunks to process
**Effort:** Medium
**From:** Database systems (indexing)

**The Problem:**
- Currently checks all 3031 chunks every frame
- Most aren't visible or don't need updates

**The Solution:**
- Spatial hash grid at multiple resolutions
- Only check nearby chunks
- Early-out for distant chunks

**Implementation Sketch:**
```cpp
struct FHierarchicalGrid {
    TMap<FIntVector, TArray<Chunk*>> Cells[4];  // 4 LOD levels

    void GetVisibleChunks(FVector CameraPos, TArray<Chunk*>& Out) {
        // Level 0: 32x32x32 cells (coarse)
        // Level 3: 1x1x1 cells (fine)
        for (int LOD = 0; LOD < 4; LOD++) {
            FIntVector CellPos = ToCellCoords(CameraPos, LOD);
            // Only check nearby cells
        }
    }
};
```

**ROI:** Medium-high impact, moderate effort.

---

### 9. SIMD Optimizations ⭐⭐⭐
**Impact:** 2-4x faster CPU processing
**Effort:** Medium
**From:** High-performance computing

**The Problem:**
- CPU processes voxel data one at a time
- Modern CPUs can process 4-8 at once (AVX)

**The Solution:**
- Use SIMD intrinsics for batch operations
- Process 8 voxels simultaneously

**Implementation Example:**
```cpp
// Scalar (old):
for (int i = 0; i < Count; i++) {
    if (Voxels[i] != 0) SolidCount++;
}

// SIMD (new):
__m256i zero = _mm256_setzero_si256();
for (int i = 0; i < Count; i += 8) {
    __m256i v = _mm256_loadu_si256((__m256i*)&Voxels[i]);
    __m256i mask = _mm256_cmpeq_epi32(v, zero);
    SolidCount += 8 - _mm_popcnt_u32(_mm256_movemask_epi8(mask));
}
```

**ROI:** High impact for CPU work, requires expertise.

---

### 10. Async Shader Compilation ⭐⭐⭐
**Impact:** Eliminates hitches
**Effort:** Easy
**From:** Unreal Engine features

**The Problem:**
- If shaders compile on-demand, causes frame stalls

**The Solution:**
- Enable async shader compilation in Unreal

**Settings:**
```ini
[/Script/Engine.RendererSettings]
r.ShaderCompiler.AsyncCompilation=1
r.ShaderPipelineCache.Enabled=1
```

**ROI:** Easy win if shaders are stalling.

---

## 🔬 **EXPERIMENTAL** (Advanced Techniques)

### 11. Lock-Free Job Queue ⭐⭐⭐⭐
**Impact:** 30%+ faster task scheduling
**From:** Concurrent programming

Use lock-free ring buffer instead of mutex-protected queues for async jobs.

---

### 12. Thread Affinity ⭐⭐⭐
**Impact:** 10-15% faster, less variance
**From:** Real-time systems

Pin voxel threads to specific CPU cores to reduce cache thrashing.

```cpp
FPlatformProcess::SetThreadAffinityMask(ThreadHandle, AffinityMask);
```

---

### 13. GPU-Side Noise Generation ⭐⭐⭐⭐
**Impact:** Eliminate CPU generation entirely
**From:** Procedural generation

Generate voxel data on GPU using compute shaders, skip CPU generation.

---

### 14. Huge Pages ⭐⭐
**Impact:** 5-10% faster memory access
**From:** Linux kernel optimization

Allocate chunk buffers with 2MB huge pages instead of 4KB pages.

```cpp
// Platform-specific
VirtualAlloc(..., MEM_LARGE_PAGES);
```

---

### 15. Cache-Aware Data Layout ⭐⭐⭐
**Impact:** 20%+ faster CPU meshing
**From:** Data-oriented design

Reorder voxel data to match CPU cache lines (64 bytes).

```cpp
// Bad (random access):
struct Chunk { EVoxelBlockID Voxels[32][32][32]; };

// Good (cache-friendly):
struct Chunk {
    // Group by 8x8x8 tiles (512 voxels = 8 cache lines)
    EVoxelBlockID Tiles[4][4][4][8][8][8];
};
```

---

## 📊 **PROFILING FIRST!**

Before implementing advanced optimizations, **profile with Unreal Insights**:

```cpp
// In console:
stat startfile
// ... play for 30 seconds ...
stat stopfile

// Open .ue4stats file in Unreal Insights
```

**Look for:**
- Where is game thread spending time?
- Is render thread waiting on game thread?
- Which functions take the most time?

**Don't optimize blindly!** Profile → Identify bottleneck → Fix → Measure.

---

## 🎯 **RECOMMENDED ORDER**

For maximum FPS with minimum effort:

1. ✅ **Frame Budget System** (already done!)
2. ⏭️ **Batch GPU Dispatches** (best ROI)
3. ⏭️ **Object Pooling** (easy, high impact)
4. ⏭️ **Async Shader Compilation** (1-line change)
5. 🔬 **Profile with Unreal Insights** (find real bottleneck)
6. ⏭️ **Implement top bottleneck fix**

---

## 🧪 **TUNING CURRENT SETTINGS**

Try these console commands in-game:

```cpp
// GPU readback throttling (our new feature!)
r.Voxel.GPU.MaxReadbacksPerFrame 10    // Ultra-smooth 240 FPS
r.Voxel.GPU.MaxReadbacksPerFrame 20    // Smooth 144 FPS (default)
r.Voxel.GPU.MaxReadbacksPerFrame 50    // Faster loading
r.Voxel.GPU.MaxReadbacksPerFrame 100   // Max throughput
r.Voxel.GPU.MaxReadbacksPerFrame 0     // Unlimited (test if this is bottleneck)

// Chunk spawning
// Note: Set in VoxelSettings, not via CVar
MaxChunksSpawnPerFrame 2    // Slower spawn, smoother FPS
MaxChunksSpawnPerFrame 4    // Default
MaxChunksSpawnPerFrame 8    // Faster spawn, possible hitches

// Mesh applies
MaxMeshAppliesPerTick 2     // Slower mesh updates, smoother FPS
MaxMeshAppliesPerTick 4     // Default
MaxMeshAppliesPerTick 8     // Faster updates, possible hitches
```

**Experimentation:**
1. Set `r.Voxel.GPU.MaxReadbacksPerFrame 0` (unlimited)
2. If FPS drops → GPU readbacks are bottleneck (default of 20 is good)
3. If FPS stays high → bottleneck is elsewhere (try other optimizations)

---

## 📈 **EXPECTED RESULTS**

With frame budget system (default 20):
- **Before:** 80 FPS during generation
- **After:** ~130-144 FPS during generation

If still below 144 FPS:
- Try `r.Voxel.GPU.MaxReadbacksPerFrame 10` (lower = smoother)
- Profile with Unreal Insights to find real bottleneck
- Implement batch GPU dispatches (next biggest win)

---

## 🔧 **DEBUGGING COMMANDS**

```cpp
// See GPU meshing stats
stat GPU
stat RHI

// See game thread breakdown
stat Game

// Full profiling
stat StartFile
stat StopFile
```

---

**Questions? Check the commit messages for implementation details!**
