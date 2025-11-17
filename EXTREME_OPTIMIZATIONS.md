# 🔥 EXTREME Performance Optimizations
## *For Running on Modest PCs - No Trick Too Small*

This document contains **every possible optimization** from research papers, game engines, HPC, databases, and obscure fields.

---

## 🎯 **TIER S: MASSIVE IMPACT**

### 1. Binary Greedy Meshing ⭐⭐⭐⭐⭐
**Impact:** 10-50x faster meshing (50us vs 2500us)
**From:** Recent voxel research (2024)
**Reference:** https://github.com/cgerikj/binary-greedy-meshing

**The Technique:**
Use bitwise operations to process **64 faces at once** instead of one-by-one.

**Implementation:**
```cpp
// Current: Check faces individually (slow)
for (int i = 0; i < 64; i++) {
    if (IsVisible(voxels[i])) faces++;
}

// Binary: Check 64 at once (FAST!)
uint64_t VisibilityMask[64];  // 64x64 chunk = 64 uint64_t
// Each bit = 1 voxel, 64 bits = 64 voxels

// Cull 64 faces at once with bit operations
uint64_t visible = VisibilityMask[layer] & (~VisibilityMask[layer+1]);
int faceCount = __builtin_popcountll(visible);  // Hardware popcount

// Merge 64 faces at once
uint64_t canMerge = (visible & (visible << 1)) & MergeMask;
```

**Key Tricks:**
- `__builtin_popcountll()` - Hardware popcount (counts 1s in 64 bits)
- `__builtin_ctzll()` - Count trailing zeros (find first set bit)
- Bit shifting for neighbor checks
- 64-bit masks for entire rows

**ROI:** HUGE - Should be your #1 priority!

---

### 2. Morton Codes (Z-Order Curve) ⭐⭐⭐⭐⭐
**Impact:** 30-50% cache hit rate improvement
**From:** Database systems / spatial indexing
**Reference:** https://www.forceflow.be/2013/10/07/morton-encodingdecoding-through-bit-interleaving-implementations/

**The Problem:**
Linear memory layout doesn't match 3D access patterns → cache misses.

**The Solution:**
Store voxels in Z-order (space-filling curve) for spatial locality.

**Implementation:**
```cpp
// Fast Morton encoding (bit interleaving)
inline uint32_t Morton3D(uint32_t x, uint32_t y, uint32_t z) {
    // Magic bit-spreading (lookup table variant)
    static const uint32_t MORTON_TABLE[256] = { /* pre-computed */ };

    return MORTON_TABLE[x & 0xFF] |
           (MORTON_TABLE[y & 0xFF] << 1) |
           (MORTON_TABLE[z & 0xFF] << 2);
}

// Use Morton code as array index
uint32_t morton = Morton3D(x, y, z);
Voxels[morton] = blockID;  // Better cache locality!
```

**Pre-compute Morton table:**
```cpp
// Build at startup
for (uint32_t i = 0; i < 256; i++) {
    uint32_t v = i;
    v = (v | (v << 8)) & 0x00FF00FF;
    v = (v | (v << 4)) & 0x0F0F0F0F;
    v = (v | (v << 2)) & 0x33333333;
    v = (v | (v << 1)) & 0x55555555;
    MORTON_TABLE[i] = v;
}
```

**Benefits:**
- Nearby voxels in 3D → nearby in memory
- Cache-line friendly (64 bytes = ~16 voxels)
- Better prefetcher performance

**ROI:** High - Works for ANY memory access pattern!

---

### 3. Mimalloc Allocator ⭐⭐⭐⭐⭐
**Impact:** Up to 60% faster, used by Street Fighter 6
**From:** Microsoft Research, adopted by RE ENGINE (Capcom)
**Reference:** https://github.com/microsoft/mimalloc

**The Problem:**
Default allocators (malloc, new) are slow and cause contention with 3031 chunks.

**The Solution:**
Use mimalloc - lockless, cache-friendly allocator.

**Integration (5 minutes):**
```cpp
// 1. Download mimalloc.dll
// 2. In .Build.cs:
PublicAdditionalLibraries.Add("path/to/mimalloc.lib");
RuntimeDependencies.Add("path/to/mimalloc.dll");

// 3. Override global allocator (optional):
#include "mimalloc-override.h"  // Done!

// Or use explicitly:
void* ptr = mi_malloc(size);
mi_free(ptr);
```

**Benefits:**
- Lockless (scales to many cores)
- 25% less memory overhead
- Free-list per thread (no contention)
- Better cache alignment

**ROI:** EASY WIN - 5 minute integration, massive gains!

---

### 4. Async Compute ⭐⭐⭐⭐⭐
**Impact:** 20-40% GPU utilization increase
**From:** Modern GPU architecture (DX12/Vulkan)
**Reference:** https://developer.nvidia.com/blog/advanced-api-performance-async-compute-and-overlap/

**The Technique:**
Run meshing compute shaders WHILE rendering previous frame.

**Current:**
```
Frame 1: [Render] → [GPU Idle] → [Mesh Compute]
Frame 2: [Render] → [GPU Idle] → [Mesh Compute]
                    ↑ wasted cycles!
```

**With Async Compute:**
```
Frame 1: [Render]
         [Mesh Compute] ← Running simultaneously!
Frame 2: [Render]
         [Mesh Compute]
```

**Implementation Sketch (Unreal):**
```cpp
// Create async compute queue
FRHICommandListImmediate& AsyncCmdList = FRHICommandListExecutor::GetImmediateAsyncComputeCommandList();

// Dispatch meshing on async queue
ENQUEUE_RENDER_COMMAND(AsyncMesh)([](FRHICommandListImmediate& RHICmdList)
{
    // Submit to ASYNC compute queue instead of graphics
    FRHIAsyncComputeCommandListImmediate& AsyncCompute =
        FRHICommandListExecutor::GetImmediateAsyncComputeCommandList();

    // Dispatch shader on async queue
    AsyncCompute.DispatchComputeShader(...);
    AsyncCompute.SubmitCommandsHint();  // Fire and forget!
});
```

**ROI:** High if GPU has free compute units during rendering.

---

## 🎯 **TIER A: VERY HIGH IMPACT**

### 5. SIMD for Voxel Processing ⭐⭐⭐⭐
**Impact:** 4-8x faster CPU processing
**From:** High-performance computing

**Process 8 voxels at once with AVX2:**
```cpp
#include <immintrin.h>

// Count solid voxels (scalar - slow)
int CountSolid(EVoxelBlockID* voxels, int count) {
    int solid = 0;
    for (int i = 0; i < count; i++) {
        if (voxels[i] != EVoxelBlockID::Air) solid++;
    }
    return solid;
}

// Count solid voxels (SIMD - 8x faster!)
int CountSolid_SIMD(EVoxelBlockID* voxels, int count) {
    __m256i zero = _mm256_setzero_si256();
    int solid = 0;

    for (int i = 0; i < count; i += 8) {
        // Load 8 voxels at once (8-bit IDs)
        __m256i v = _mm256_loadu_si256((__m256i*)&voxels[i]);

        // Compare all 8 to zero
        __m256i mask = _mm256_cmpeq_epi8(v, zero);

        // Count non-zero (solid voxels)
        uint32_t bits = _mm256_movemask_epi8(mask);
        solid += __builtin_popcount(~bits);
    }

    return solid;
}
```

**More SIMD uses:**
- Flood fill (8 voxels at once)
- Neighbor checks (compare 8 in parallel)
- Block type matching
- Biome ID checks

**ROI:** High for CPU-heavy code!

---

### 6. GPU Shared Memory Optimization ⭐⭐⭐⭐
**Impact:** 10-20x faster than global memory
**From:** CUDA/Compute shader best practices

**Avoid Bank Conflicts:**
```hlsl
// BAD: Bank conflict (all threads access same bank)
groupshared uint Data[32][32];
uint value = Data[threadIdx.x][0];  // All read bank 0!

// GOOD: Pad to avoid conflicts
groupshared uint Data[32][33];  // +1 padding
uint value = Data[threadIdx.x][threadIdx.y];  // No conflict!
```

**Coalesced Global Memory:**
```hlsl
// BAD: Strided access (slow)
for (uint i = threadIdx.x; i < 1024; i += 256)
    sum += GlobalData[i];

// GOOD: Coalesced access (fast)
uint idx = threadIdx.x;
sum = GlobalData[idx] + GlobalData[idx + 256] +
      GlobalData[idx + 512] + GlobalData[idx + 768];
```

**ROI:** Essential for GPU compute performance!

---

### 7. Wave Intrinsics (GPU) ⭐⭐⭐⭐
**Impact:** Eliminate sync points, faster reductions
**From:** Modern GPU programming (HLSL 6.0+)

**Use wave-level operations instead of shared memory:**
```hlsl
// OLD: Shared memory reduction (slow, needs sync)
groupshared uint LocalSum[64];
LocalSum[threadIdx.x] = value;
GroupMemoryBarrierWithGroupSync();  // Expensive!
// ... tree reduction ...

// NEW: Wave intrinsics (fast, no sync!)
uint waveSum = WaveActiveSum(value);  // No sync needed!
if (WaveIsFirstLane()) {
    FinalSum = waveSum;
}
```

**More wave intrinsics:**
- `WaveActiveCountBits()` - Count bits across wave
- `WaveActiveMin/Max()` - Min/max across wave
- `WavePrefixSum()` - Parallel prefix scan
- `WaveReadLaneAt()` - Read from specific lane

**ROI:** Free performance if targeting modern GPUs!

---

### 8. Bit Packing ⭐⭐⭐⭐
**Impact:** 4-16x memory reduction
**From:** Compression / embedded systems

**Pack multiple values into single uint32:**
```cpp
// Current: 4 bytes per voxel (wasteful!)
struct Voxel {
    EVoxelBlockID BlockID;  // 1 byte used, 3 wasted
};

// Packed: Multiple voxels per uint32
union PackedVoxel {
    uint32_t Packed;
    struct {
        uint32_t BlockID : 8;     // 256 block types
        uint32_t BiomeID : 6;     // 64 biomes
        uint32_t AO : 2;          // 4 AO levels
        uint32_t Flags : 4;       // Custom flags
        uint32_t Reserved : 12;   // Future use
    };
};

// Access with bit operations
inline uint8_t GetBlockID(uint32_t packed) {
    return packed & 0xFF;
}

inline void SetBlockID(uint32_t& packed, uint8_t id) {
    packed = (packed & ~0xFF) | id;
}
```

**Even more extreme - palette indices:**
```cpp
// If chunk has <= 256 unique blocks, use 8-bit indices
struct ChunkPalette {
    EVoxelBlockID Palette[256];  // Unique blocks in chunk
    uint8_t Indices[32*32*32];   // Just indices!
};
// Saves 75% memory if palette is small!
```

**ROI:** Massive memory savings = faster cache/RAM access!

---

### 9. Fast Noise Library (SIMD) ⭐⭐⭐⭐
**Impact:** 5-10x faster terrain generation
**From:** FastNoise2 by Auburn
**Reference:** https://github.com/Auburn/FastNoise2

**Replace your noise with SIMD-accelerated version:**
```cpp
// OLD: Scalar noise (slow)
float GetNoise(float x, float y, float z) {
    return PerlinNoise(x, y, z);  // 1 at a time
}

// NEW: FastNoise2 (processes 8+ at once)
#include "FastNoise/FastNoise.h"

auto noise = FastNoise::New<FastNoise::Simplex>();
noise->GenUniformGrid3D(
    noiseOut,           // Output array
    xStart, yStart, zStart,
    xSize, ySize, zSize,
    frequency, seed
);
// Uses AVX2/AVX-512 automatically!
```

**ROI:** If generation is bottleneck, this is EASY win!

---

### 10. Fixed-Point Math ⭐⭐⭐
**Impact:** 2-4x faster on some platforms
**From:** Embedded systems / retro game dev

**Use integers instead of floats for voxel coords:**
```cpp
// Floats (slow, precision issues)
struct VoxelPos {
    float X, Y, Z;
};

// Fixed-point (fast, exact)
struct VoxelPosFixed {
    int32_t X, Y, Z;  // Units = 1/256th of a block
};

// Math is just integers!
VoxelPosFixed a, b;
a.X = b.X + (128 << 8);  // Add 128 voxels (exact!)

// Convert to float only when rendering
float worldX = a.X / 256.0f;
```

**ROI:** Medium - depends on amount of float math.

---

## 🎯 **TIER B: HIGH IMPACT**

### 11. Lookup Tables (LUTs) ⭐⭐⭐⭐
**Impact:** 10-100x faster than computing
**From:** Classic optimization

**Pre-compute expensive functions:**
```cpp
// Expensive: sqrt, sin, cos, etc.
float distance = sqrtf(dx*dx + dy*dy + dz*dz);

// Fast: Lookup table
static float SQRT_TABLE[65536];  // Pre-computed at startup

void InitSqrtTable() {
    for (int i = 0; i < 65536; i++) {
        SQRT_TABLE[i] = sqrtf((float)i);
    }
}

// Use table (100x faster!)
uint32_t distSq = dx*dx + dy*dy + dz*dz;
if (distSq < 65536) {
    float distance = SQRT_TABLE[distSq];
}
```

**Other LUT candidates:**
- AO values based on neighbor config
- Face normals (6 directions only)
- UV coordinates for block faces
- Light attenuation curves

**ROI:** Huge if you have repeated calculations!

---

### 12. Restrict Keyword ⭐⭐⭐
**Impact:** 5-15% faster loops
**From:** C99 compiler optimization

**Tell compiler pointers don't alias:**
```cpp
// Without restrict (compiler assumes overlap)
void Process(uint8_t* in, uint32_t* out) {
    for (int i = 0; i < 1000; i++) {
        out[i] = in[i] * 2;  // Compiler can't optimize (might overlap)
    }
}

// With restrict (compiler knows no overlap)
void Process(uint8_t* __restrict in, uint32_t* __restrict out) {
    for (int i = 0; i < 1000; i++) {
        out[i] = in[i] * 2;  // Can be vectorized!
    }
}
```

**ROI:** Free performance in hot loops!

---

### 13. Branch Prediction Hints ⭐⭐⭐
**Impact:** 5-10% in branchy code
**From:** Compiler intrinsics

**Help CPU predict branches:**
```cpp
// Modern C++20
if (voxel != Air) [[likely]] {
    ProcessSolidVoxel(voxel);
} else [[unlikely]] {
    // Rare case
}

// GCC/Clang
#define LIKELY(x) __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)

if (LIKELY(voxel != Air)) {
    ProcessSolidVoxel(voxel);
}
```

**ROI:** Small but free in critical paths.

---

### 14. Prefetch Hints ⭐⭐⭐
**Impact:** 10-20% cache miss reduction
**From:** CPU optimization

**Prefetch data before you need it:**
```cpp
#include <xmmintrin.h>  // _mm_prefetch

void ProcessChunks(Chunk** chunks, int count) {
    for (int i = 0; i < count; i++) {
        // Prefetch next chunk while processing current
        if (i + 1 < count) {
            _mm_prefetch((char*)chunks[i+1], _MM_HINT_T0);
        }

        ProcessChunk(chunks[i]);
    }
}
```

**ROI:** Good for sequential processing.

---

### 15. SoA (Structure of Arrays) ⭐⭐⭐⭐
**Impact:** 2-4x better cache usage
**From:** Data-oriented design

**Reorganize data for cache:**
```cpp
// AoS (Array of Structures) - BAD for cache
struct Voxel {
    EVoxelBlockID BlockID;  // 1 byte
    uint8_t BiomeID;        // 1 byte
    uint8_t AO;             // 1 byte
    uint8_t Flags;          // 1 byte
};
Voxel voxels[32768];

// When accessing BlockID, loads all 4 bytes (wastes 3!)

// SoA (Structure of Arrays) - GOOD for cache
struct VoxelsSoA {
    EVoxelBlockID BlockIDs[32768];  // Contiguous!
    uint8_t BiomeIDs[32768];
    uint8_t AO[32768];
    uint8_t Flags[32768];
};

// Accessing BlockIDs = perfect cache lines!
```

**ROI:** Major win for large datasets!

---

## 🎯 **TIER C: MEDIUM IMPACT**

### 16. Custom Allocators (Arena, Pool) ⭐⭐⭐
**Impact:** 3-5x faster alloc/free
**From:** Game engine patterns

**Arena allocator for chunks:**
```cpp
class ArenaAllocator {
    uint8_t* Buffer;
    size_t Offset = 0;
    size_t Size;

public:
    void* Alloc(size_t bytes) {
        void* ptr = Buffer + Offset;
        Offset += bytes;
        return ptr;
    }

    void Reset() { Offset = 0; }  // Free everything!
};

// Use for temporary chunk processing
ArenaAllocator frameArena(1024*1024*10);  // 10MB
void* temp = frameArena.Alloc(sizeof(ChunkData));
// ... use temp ...
frameArena.Reset();  // Instant "free"!
```

**ROI:** Good for frame-temporary data.

---

### 17. Thread Affinity ⭐⭐⭐
**Impact:** 5-15% less variance, faster
**From:** Real-time systems

**Pin threads to cores:**
```cpp
#include <Windows.h>  // or pthread on Linux

void PinThreadToCore(int coreID) {
    HANDLE thread = GetCurrentThread();
    DWORD_PTR mask = 1ULL << coreID;
    SetThreadAffinityMask(thread, mask);
}

// Pin mesher threads to P-cores (fast cores)
PinThreadToCore(0);  // Core 0
PinThreadToCore(1);  // Core 1
// Reduces cache thrashing!
```

**ROI:** Reduces jitter, improves consistency.

---

### 18. Huge Pages ⭐⭐
**Impact:** 5-10% memory access speedup
**From:** Linux kernel optimization

**Use 2MB pages instead of 4KB:**
```cpp
#ifdef _WIN32
#include <Windows.h>

void* AllocHugePages(size_t size) {
    SIZE_T minSize = size;
    return VirtualAlloc(
        nullptr, size,
        MEM_COMMIT | MEM_RESERVE | MEM_LARGE_PAGES,
        PAGE_READWRITE
    );
}
#else
#include <sys/mman.h>

void* AllocHugePages(size_t size) {
    return mmap(
        nullptr, size,
        PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB,
        -1, 0
    );
}
#endif
```

**ROI:** Small but easy on Linux.

---

### 19. Profile-Guided Optimization (PGO) ⭐⭐⭐⭐
**Impact:** 10-20% across the board
**From:** Compiler optimization

**Let compiler optimize for real usage:**
```bash
# 1. Compile with instrumentation
clang++ -fprofile-generate main.cpp -o game.exe

# 2. Run game and play normally (generates .profdata)
./game.exe

# 3. Recompile with profile data
clang++ -fprofile-use=default.profdata main.cpp -o game_optimized.exe

# Compiler now knows:
# - Which branches are common
# - Which functions are hot
# - Which code paths are cold
```

**ROI:** Free 10-20% speedup!

---

### 20. Indirect Drawing ⭐⭐⭐
**Impact:** 100x less CPU→GPU traffic
**From:** Modern graphics APIs

**Let GPU decide what to render:**
```cpp
// OLD: CPU loops, issues draws
for (Chunk* chunk : visibleChunks) {
    RHI.DrawIndexed(chunk->indexCount, ...);  // 3031 calls!
}

// NEW: GPU loops, CPU issues ONE draw
DrawIndirectCommand commands[3031];
// ... fill on GPU or CPU once ...

RHI.DrawIndexedIndirect(
    commandBuffer,  // Buffer on GPU
    3031           // Draw count
);  // ONE call for 3031 chunks!
```

**ROI:** Massive CPU savings!

---

## 🔬 **TIER D: EXPERIMENTAL**

### 21. Jump Flooding Algorithm ⭐⭐⭐
**Impact:** 100x faster distance fields
**From:** GPU algorithms research

For AO or distance-based effects:
```hlsl
// Compute distance field in log2(N) passes instead of N^2
// Pass 1: Jump by 16 pixels
// Pass 2: Jump by 8 pixels
// Pass 3: Jump by 4 pixels
// ... each pass halves the jump
```

---

### 22. Mesh Shaders ⭐⭐⭐⭐
**Impact:** Potentially 2x faster than vertex shaders
**From:** DirectX 12 / Vulkan (2018+)

Replace traditional vertex pipeline with compute-like mesh shading.

---

### 23. Conservative Rasterization ⭐⭐⭐
**Impact:** Perfect voxel coverage
**From:** Modern GPUs

Guarantee voxels are covered (no gaps).

---

### 24. RLE Compression ⭐⭐⭐
**Impact:** 10-50x memory reduction for sparse chunks
**From:** Compression

```cpp
// Empty chunks = just store "32768x Air"
struct RLEChunk {
    TArray<TPair<EVoxelBlockID, uint16_t>> Runs;
    // [(Air, 1000), (Stone, 500), (Air, 200), ...]
};
```

---

### 25. Neural Network LOD Prediction ⭐⭐⭐⭐
**Impact:** Pre-fetch chunks player will need
**From:** Machine learning

Train network to predict player movement, prefetch chunks.

---

## 📊 **BENCHMARKING**

Always measure! Use these tools:

```cpp
// Unreal profiling
SCOPE_CYCLE_COUNTER(STAT_MyFunction);

// Tracy Profiler (best!)
#include "tracy/Tracy.hpp"
ZoneScoped;  // Profile this scope

// Manual timing
auto start = std::chrono::high_resolution_clock::now();
// ... code ...
auto end = std::chrono::high_resolution_clock::now();
auto us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
UE_LOG(LogTemp, Log, TEXT("Took %lld us"), us);
```

**Profile before optimizing!**

---

## 🎯 **RECOMMENDED ORDER FOR MODEST PCs**

1. **Binary Greedy Meshing** (10-50x faster)
2. **Mimalloc** (5-min integration, huge win)
3. **Morton Codes** (better cache)
4. **Bit Packing** (less memory)
5. **SIMD** (4-8x CPU work)
6. **SoA** (better cache lines)
7. **Lookup Tables** (eliminate expensive math)
8. **GPU Shared Memory** (10-20x faster)
9. **Async Compute** (overlap work)
10. **Profile and iterate!**

---

## 💡 **QUICK WINS (< 1 Hour)**

- Enable PGO compilation
- Use mimalloc
- Add restrict keywords
- Add [[likely]]/[[unlikely]]
- Use FastNoise2
- Enable compiler optimizations (-O3, /Ox)

---

## 🧪 **TESTING METHODOLOGY**

1. **Baseline:** Capture FPS on target modest PC
2. **Apply one optimization at a time**
3. **Measure impact**
4. **Keep if > 5% improvement**
5. **Repeat**

**Target Hardware (Modest PC):**
- Intel i3 / Ryzen 3
- 8GB RAM
- GTX 1050 / RX 560
- Goal: 60 FPS stable

---

## 📚 **FURTHER READING**

- **Binary meshing:** https://github.com/cgerikj/binary-greedy-meshing
- **Morton codes:** https://www.forceflow.be/2013/10/07/morton-encodingdecoding-through-bit-interleaving-implementations/
- **Mimalloc:** https://github.com/microsoft/mimalloc
- **Async compute:** https://developer.nvidia.com/blog/advanced-api-performance-async-compute-and-overlap/
- **GPU optimization:** https://gpuopen.com/learn/
- **SIMD guide:** Intel Intrinsics Guide
- **Tracy profiler:** https://github.com/wolfpld/tracy

---

**Remember:** Measure, optimize, measure again. Don't optimize blindly!

🚀 **With these techniques, you can run on a toaster!**
