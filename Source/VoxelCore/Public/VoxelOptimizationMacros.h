// Copyright Epic Games, Inc. All Rights Reserved.
// VoxelCore - Performance Optimization Macros

#pragma once

#include "CoreMinimal.h"

// ============================================================================
// BRANCH PREDICTION HINTS
// ============================================================================
// Help the CPU predict which branch is more likely to be taken
// Use [[likely]] and [[unlikely]] in C++20, or these macros for compatibility

#if defined(__GNUC__) || defined(__clang__)
    #define VOXEL_LIKELY(x)   __builtin_expect(!!(x), 1)
    #define VOXEL_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
    // MSVC and C++20 support
    #define VOXEL_LIKELY(x)   (x)
    #define VOXEL_UNLIKELY(x) (x)
#endif

// ============================================================================
// PREFETCH HINTS
// ============================================================================
// Tell the CPU to prefetch data before we need it (reduces cache misses)

#if defined(__GNUC__) || defined(__clang__)
    #define VOXEL_PREFETCH(addr)        __builtin_prefetch(addr)
    #define VOXEL_PREFETCH_WRITE(addr)  __builtin_prefetch(addr, 1)
    #define VOXEL_PREFETCH_LOCALITY(addr, level) __builtin_prefetch(addr, 0, level)
#elif defined(_MSC_VER)
    #include <xmmintrin.h>
    #define VOXEL_PREFETCH(addr)        _mm_prefetch((const char*)(addr), _MM_HINT_T0)
    #define VOXEL_PREFETCH_WRITE(addr)  _mm_prefetch((const char*)(addr), _MM_HINT_T0)
    #define VOXEL_PREFETCH_LOCALITY(addr, level) _mm_prefetch((const char*)(addr), _MM_HINT_T0)
#else
    #define VOXEL_PREFETCH(addr)        ((void)0)
    #define VOXEL_PREFETCH_WRITE(addr)  ((void)0)
    #define VOXEL_PREFETCH_LOCALITY(addr, level) ((void)0)
#endif

// ============================================================================
// RESTRICT KEYWORD
// ============================================================================
// Tell the compiler that pointers don't alias (enables better vectorization)

#if defined(__GNUC__) || defined(__clang__)
    #define VOXEL_RESTRICT __restrict__
#elif defined(_MSC_VER)
    #define VOXEL_RESTRICT __restrict
#else
    #define VOXEL_RESTRICT
#endif

// ============================================================================
// FORCED INLINE
// ============================================================================
// More aggressive than FORCEINLINE for critical hot paths

#if defined(_MSC_VER)
    #define VOXEL_FORCE_INLINE __forceinline
#elif defined(__GNUC__) || defined(__clang__)
    #define VOXEL_FORCE_INLINE inline __attribute__((always_inline))
#else
    #define VOXEL_FORCE_INLINE inline
#endif

// ============================================================================
// ASSUME HINTS
// ============================================================================
// Tell the compiler to assume certain conditions (enables optimizations)

#if defined(_MSC_VER)
    #define VOXEL_ASSUME(expr) __assume(expr)
#elif defined(__clang__)
    #define VOXEL_ASSUME(expr) __builtin_assume(expr)
#else
    #define VOXEL_ASSUME(expr) do { if (!(expr)) __builtin_unreachable(); } while(0)
#endif

// ============================================================================
// MEMORY ALIGNMENT HINTS
// ============================================================================
// Tell the compiler about memory alignment (enables SIMD)

#define VOXEL_ALIGN(n) alignas(n)
#define VOXEL_ASSUME_ALIGNED(ptr, alignment) __builtin_assume_aligned(ptr, alignment)

// ============================================================================
// CACHE LINE SIZE
// ============================================================================
// Modern CPUs use 64-byte cache lines

#define VOXEL_CACHE_LINE_SIZE 64

// ============================================================================
// HOT/COLD PATH HINTS
// ============================================================================
// Mark functions as hot (frequently called) or cold (rarely called)

#if defined(__GNUC__) || defined(__clang__)
    #define VOXEL_HOT   __attribute__((hot))
    #define VOXEL_COLD  __attribute__((cold))
#else
    #define VOXEL_HOT
    #define VOXEL_COLD
#endif

// ============================================================================
// USAGE EXAMPLES
// ============================================================================

/*
// BRANCH PREDICTION:
if (VOXEL_LIKELY(voxel != Air))  // Common case
{
    ProcessSolidVoxel(voxel);
}
else  // Rare case
{
    // Handle air
}

// RESTRICT:
void Process(const uint8* VOXEL_RESTRICT input, uint32* VOXEL_RESTRICT output)
{
    // Compiler knows input and output don't overlap
}

// PREFETCH:
for (int i = 0; i < count; i++)
{
    VOXEL_PREFETCH(&data[i + 8]);  // Prefetch 8 elements ahead
    ProcessData(&data[i]);
}

// ASSUME:
void Process(int value)
{
    VOXEL_ASSUME(value >= 0 && value < 256);  // Compiler can optimize based on this
    LookupTable[value] = ...;
}

// ALIGNMENT:
VOXEL_ALIGN(16) float data[4];  // SSE-friendly alignment
*/
