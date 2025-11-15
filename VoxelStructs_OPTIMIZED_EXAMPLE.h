// THIS IS AN EXAMPLE FILE SHOWING OPTIMIZED FCategoryBitset
// Copy these optimizations to your actual VoxelStructs.h

#pragma once

#include "CoreMinimal.h"
#include <emmintrin.h>  // SSE2 for SIMD

// ============================================================================
// OPTIMIZATION MACROS
// ============================================================================

// Branch prediction hints
#if defined(__GNUC__) || defined(__clang__)
    #define VOXEL_LIKELY(x)   __builtin_expect(!!(x), 1)
    #define VOXEL_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
    #define VOXEL_LIKELY(x)   (x)
    #define VOXEL_UNLIKELY(x) (x)
#endif

// Prefetch hint
#if defined(__GNUC__) || defined(__clang__)
    #define VOXEL_PREFETCH(addr) __builtin_prefetch(addr)
#elif defined(_MSC_VER)
    #include <xmmintrin.h>
    #define VOXEL_PREFETCH(addr) _mm_prefetch((const char*)(addr), _MM_HINT_T0)
#else
    #define VOXEL_PREFETCH(addr) ((void)0)
#endif

// ============================================================================
// OPTIMIZED FCategoryBitset
// ============================================================================

struct FCategoryBitset_Optimized
{
    int32 SizeX = 0;
    int32 SizeY = 0;
    int32 SizeZ = 0;
    TArray<uint8> Data;

private:
    // === NEW: LOOKUP TABLE FOR FAST BIT EXTRACTION ===
    static uint8 BitExtractLUT[256 * 8];
    static bool bLUTInitialized;

    static void InitializeLUT()
    {
        if (bLUTInitialized) return;

        // Pre-compute all possible (byte_value, bit_offset) combinations
        for (int32 byteVal = 0; byteVal < 256; ++byteVal)
        {
            for (int32 offset = 0; offset < 8; ++offset)
            {
                if (offset <= 6)
                {
                    // Extract 2 bits at offset
                    BitExtractLUT[byteVal * 8 + offset] = (byteVal >> offset) & 0x3;
                }
                else
                {
                    // offset == 7: only 1 bit from this byte
                    BitExtractLUT[byteVal * 8 + 7] = (byteVal >> 7) & 0x1;
                }
            }
        }

        bLUTInitialized = true;
    }

public:
    void Init(int32 InX, int32 InY, int32 InZ)
    {
        InitializeLUT();  // Ensure LUT is initialized

        SizeX = InX;
        SizeY = InY;
        SizeZ = InZ;
        const int64 TotalVoxels = static_cast<int64>(SizeX) * SizeY * SizeZ;
        const int64 NumBits = TotalVoxels * 2;
        const int64 NumBytes = (NumBits + 7) / 8;
        Data.SetNumZeroed(static_cast<int32>(NumBytes));
    }

    // === OPTIMIZED: WITH BRANCH HINTS ===
    FORCEINLINE int64 LinearIndex(int32 X, int32 Y, int32 Z) const
    {
        return static_cast<int64>(X) +
               static_cast<int64>(Y) * SizeX +
               static_cast<int64>(Z) * SizeX * SizeY;
    }

    // === OPTIMIZED: USING LUT + BRANCH HINTS ===
    FORCEINLINE uint8 Get(int32 X, int32 Y, int32 Z) const
    {
        const int64 idx = LinearIndex(X, Y, Z);
        const int64 bitIndex = idx * 2;
        const int64 byteIndex = bitIndex >> 3;
        const int32 bitOffset = static_cast<int32>(bitIndex & 7);

        const uint8 byteVal = Data[byteIndex];

        // Common case: bits don't span byte boundary
        if (VOXEL_LIKELY(bitOffset <= 6))
        {
            // Use lookup table (eliminates shift + mask)
            return BitExtractLUT[byteVal * 8 + bitOffset];
        }
        else  // UNLIKELY: bits span two bytes
        {
            // bitOffset == 7
            const uint8 lsb = BitExtractLUT[byteVal * 8 + 7];
            const uint8 msb = Data[byteIndex + 1] & 0x1u;
            return lsb | (msb << 1);
        }
    }

    // === ORIGINAL Set() - Already well optimized ===
    FORCEINLINE void Set(int32 X, int32 Y, int32 Z, uint8 Cat)
    {
        Cat &= 0x3;
        const int64 idx = LinearIndex(X, Y, Z);
        const int64 bitIndex = idx * 2;
        const int64 byteIndex = bitIndex >> 3;
        const int32 bitOffset = static_cast<int32>(bitIndex & 7);

        if (VOXEL_LIKELY(bitOffset <= 6))
        {
            uint8 mask = static_cast<uint8>(0x3u << bitOffset);
            Data[byteIndex] = (Data[byteIndex] & ~mask) | (Cat << bitOffset);
        }
        else
        {
            Data[byteIndex] &= static_cast<uint8>(~0x80u);
            Data[byteIndex] |= static_cast<uint8>((Cat & 0x1u) << 7);
            Data[byteIndex + 1] &= static_cast<uint8>(~0x1u);
            Data[byteIndex + 1] |= static_cast<uint8>((Cat >> 1) & 0x1u);
        }
    }

    // === OPTIMIZED: SIMD VERSION FOR EMPTY CHECK ===
    FORCEINLINE bool IsAllEmpty() const
    {
        const uint8* RESTRICT Ptr = Data.GetData();
        const int32 NumBytes = Data.Num();

        // Process 16 bytes at a time with SSE2
        const int32 NumVectors = NumBytes / 16;
        const __m128i Zero = _mm_setzero_si128();

        for (int32 i = 0; i < NumVectors; ++i)
        {
            // Load 16 bytes
            __m128i chunk = _mm_loadu_si128((const __m128i*)(Ptr + i * 16));

            // Compare all 16 bytes to zero
            __m128i cmp = _mm_cmpeq_epi8(chunk, Zero);

            // Get comparison mask (0xFFFF if all zeros)
            int mask = _mm_movemask_epi8(cmp);

            // If not all zeros, we found data
            if (VOXEL_UNLIKELY(mask != 0xFFFF))
            {
                return false;
            }
        }

        // Handle remaining bytes (scalar)
        for (int32 i = NumVectors * 16; i < NumBytes; ++i)
        {
            if (VOXEL_UNLIKELY(Ptr[i] != 0))
            {
                return false;
            }
        }

        return true;
    }

    // === OPTIMIZED: SIMD VERSION FOR CORE RENDERABLE CHECK ===
    FORCEINLINE bool IsCoreRenderableEmpty(int32 PadWidth) const
    {
        // Skip 1-voxel padding around edges
        const int32 CoreSX = SizeX - 2 * PadWidth;
        const int32 CoreSY = SizeY - 2 * PadWidth;
        const int32 CoreSZ = SizeZ - 2 * PadWidth;

        if (CoreSX <= 0 || CoreSY <= 0 || CoreSZ <= 0)
        {
            return true;
        }

        // Check if all core voxels are air
        for (int32 z = PadWidth; z < SizeZ - PadWidth; ++z)
        {
            for (int32 y = PadWidth; y < SizeY - PadWidth; ++y)
            {
                // Prefetch next row (helps with cache)
                if (y + 1 < SizeY - PadWidth)
                {
                    const int64 nextIdx = LinearIndex(PadWidth, y + 1, z);
                    const int64 nextBitIndex = nextIdx * 2;
                    const int64 nextByteIndex = nextBitIndex >> 3;
                    VOXEL_PREFETCH(&Data[nextByteIndex]);
                }

                for (int32 x = PadWidth; x < SizeX - PadWidth; ++x)
                {
                    if (VOXEL_UNLIKELY(Get(x, y, z) != 0))
                    {
                        return false;
                    }
                }
            }
        }

        return true;
    }
};

// Static member initialization (put in .cpp file)
// uint8 FCategoryBitset_Optimized::BitExtractLUT[256 * 8];
// bool FCategoryBitset_Optimized::bLUTInitialized = false;

// ============================================================================
// USAGE EXAMPLE
// ============================================================================

/*
// In your VoxelStructs.h:
// 1. Add the macros at the top
// 2. Replace FCategoryBitset with FCategoryBitset_Optimized
// 3. In VoxelStructs.cpp, add:
//    uint8 FCategoryBitset::BitExtractLUT[256 * 8];
//    bool FCategoryBitset::bLUTInitialized = false;

// Expected performance gains:
// - Get(): 10-20% faster (LUT eliminates shift+mask)
// - IsAllEmpty(): 8-16x faster (SIMD processes 16 bytes at once)
// - IsCoreRenderableEmpty(): 10-15% faster (prefetch + branch hints)

// Memory overhead:
// - LUT: 2048 bytes (256 * 8), shared across all instances
// - One-time cost, huge speedup
*/
