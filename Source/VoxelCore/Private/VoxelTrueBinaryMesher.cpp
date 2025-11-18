// ============================================================================
// TRUE BINARY GREEDY MESHER - BITWISE VOXEL PROCESSING
// ============================================================================
//
// This implementation uses bitwise operations to process 64 voxels at once,
// achieving significant speedup over traditional per-voxel processing.
//
// Algorithm:
// 1. Convert category arrays to bitmasks (1 bit per voxel, 64 voxels per uint64)
// 2. Use bitwise AND to find visible faces (current=solid AND neighbor=air)
// 3. Use CTZ (count trailing zeros) to find first face in a row
// 4. Use bit shifting to count consecutive runs
// 5. Merge runs greedily in both U and V directions using block ID matching
//
// Performance Optimizations:
// - Single array allocation per slice (not per tile)
// - Direct bit manipulation instead of separate processed masks
// - Simplified visibility logic (single check instead of multiple masks)
// - CTZ64 intrinsics for fast bit scanning
// - Block ID verification only when extending quads
//
// ============================================================================

#include "VoxelMesher.h"
#include "VoxelStats.h"
#include "VoxelOptimizationMacros.h"
#include "VoxelBlockTable.h"
#include <intrin.h>  // For _BitScanForward64 (CTZ)

// Platform-specific CTZ (count trailing zeros) intrinsic
FORCEINLINE static int32 CountTrailingZeros64(uint64 Value)
{
    if (Value == 0) return 64;

#if PLATFORM_WINDOWS || PLATFORM_XBOXONE || PLATFORM_HOLOLENS
    unsigned long Index = 0;
    _BitScanForward64(&Index, Value);
    return (int32)Index;
#elif defined(__GNUC__) || defined(__clang__)
    return __builtin_ctzll(Value);
#else
    // Fallback for other platforms
    int32 Count = 0;
    while ((Value & 1) == 0 && Count < 64)
    {
        Value >>= 1;
        Count++;
    }
    return Count;
#endif
}

// Platform-specific popcount (count set bits) intrinsic
FORCEINLINE static int32 PopCount64(uint64 Value)
{
#if PLATFORM_WINDOWS || PLATFORM_XBOXONE || PLATFORM_HOLOLENS
    return (int32)__popcnt64(Value);
#elif defined(__GNUC__) || defined(__clang__)
    return __builtin_popcountll(Value);
#else
    // Fallback
    int32 Count = 0;
    while (Value)
    {
        Count += Value & 1;
        Value >>= 1;
    }
    return Count;
#endif
}

// ============================================================================
// BINARY FACE FINDER - PROCESSES 64 VOXELS AT ONCE
// ============================================================================

/**
 * Finds visible faces in a row of voxels using bitwise operations.
 *
 * @param CurrentRow - Bitmask of current row (1 = solid, 0 = air)
 * @param NeighborRow - Bitmask of neighbor row
 * @param RowWidth - Number of valid voxels in the row (up to 64)
 * @return Bitmask of visible faces (1 = face should be rendered)
 */
FORCEINLINE static uint64 FindVisibleFaces(uint64 CurrentRow, uint64 NeighborRow, int32 RowWidth)
{
    // Create mask for valid voxels in the row
    const uint64 ValidMask = (RowWidth >= 64) ? 0xFFFFFFFFFFFFFFFFull : ((1ull << RowWidth) - 1);

    // A face is visible if current voxel is solid (1) AND neighbor is air (0)
    // This is: current & ~neighbor
    uint64 VisibleFaces = CurrentRow & ~NeighborRow;

    // Mask to valid range
    return VisibleFaces & ValidMask;
}

/**
 * Converts category array to bitmask for a specific category.
 *
 * @param Cats - Category array (0=air, 1=semi, 2=solid)
 * @param Offset - Starting offset in array
 * @param Count - Number of elements to convert (up to 64)
 * @param Category - Category to match (1 or 2)
 * @return 64-bit mask where 1 = voxel matches category
 */
FORCEINLINE static uint64 CategoriesToBitmask(const uint8* VOXEL_RESTRICT Cats, int32 Offset, int32 Count, uint8 Category)
{
    uint64 Mask = 0;
    const int32 End = FMath::Min(Count, 64);

    for (int32 i = 0; i < End; ++i)
    {
        if (VOXEL_LIKELY(Cats[Offset + i] == Category))
        {
            Mask |= (1ull << i);
        }
    }

    return Mask;
}

/**
 * Returns the solid voxel that owns a face (depends on face direction and chunk borders).
 * The "owner" is the solid block on the back side of the face, or the front side if back is air.
 */
static FORCEINLINE EVoxelBlockID OwnerBlockForFace(
    const TArray<EVoxelBlockID>& V, const FIntVector& Size,
    const FChunkNeighbors* VOXEL_RESTRICT Nbh, int x, int y, int z, EVoxelFaceDir dir)
{
    const int SX = Size.X, SY = Size.Y, SZ = Size.Z;

    auto inside = [&](int X, int Y, int Z) {
        return (unsigned)X < (unsigned)SX && (unsigned)Y < (unsigned)SY && (unsigned)Z < (unsigned)SZ;
    };
    auto at = [&](int X, int Y, int Z)->EVoxelBlockID { return V[X + Y * SX + Z * SX * SY]; };

    auto sample = [&](int X, int Y, int Z)->EVoxelBlockID {
        if (VOXEL_LIKELY(inside(X, Y, Z))) return at(X, Y, Z);
        if (!Nbh) return EVoxelBlockID::Air;
        if (Z < 0)    return (Nbh->bHasZNeg && (unsigned)X < (unsigned)SX && (unsigned)Y < (unsigned)SY) ? Nbh->ZNeg[X + Y * SX] : EVoxelBlockID::Air;
        if (Z >= SZ)  return (Nbh->bHasZPos && (unsigned)X < (unsigned)SX && (unsigned)Y < (unsigned)SY) ? Nbh->ZPos[X + Y * SX] : EVoxelBlockID::Air;
        if (X < 0)    return (Nbh->bHasXNeg && (unsigned)Y < (unsigned)SY && (unsigned)Z < (unsigned)SZ) ? Nbh->XNeg[Y + Z * SY] : EVoxelBlockID::Air;
        if (X >= SX)  return (Nbh->bHasXPos && (unsigned)Y < (unsigned)SY && (unsigned)Z < (unsigned)SZ) ? Nbh->XPos[Y + Z * SY] : EVoxelBlockID::Air;
        if (Y < 0)    return (Nbh->bHasYNeg && (unsigned)X < (unsigned)SX && (unsigned)Z < (unsigned)SZ) ? Nbh->YNeg[X + Z * SX] : EVoxelBlockID::Air;
        return (Nbh->bHasYPos && (unsigned)X < (unsigned)SX && (unsigned)Z < (unsigned)SZ) ? Nbh->YPos[X + Z * SX] : EVoxelBlockID::Air;
    };

    // "Back" = solid side of the face. If that is empty, fall back to the other side.
    switch (dir)
    {
    case EVoxelFaceDir::XPos: { EVoxelBlockID back = sample(x - 1, y, z); return VoxelBlockCategory(back) ? back : sample(x, y, z); }
    case EVoxelFaceDir::XNeg: { EVoxelBlockID back = sample(x, y, z);   return VoxelBlockCategory(back) ? back : sample(x - 1, y, z); }
    case EVoxelFaceDir::YPos: { EVoxelBlockID back = sample(x, y - 1, z); return VoxelBlockCategory(back) ? back : sample(x, y, z); }
    case EVoxelFaceDir::YNeg: { EVoxelBlockID back = sample(x, y, z);   return VoxelBlockCategory(back) ? back : sample(x, y - 1, z); }
    case EVoxelFaceDir::ZPos: { EVoxelBlockID back = sample(x, y, z - 1); return VoxelBlockCategory(back) ? back : sample(x, y, z); }
    case EVoxelFaceDir::ZNeg: { EVoxelBlockID back = sample(x, y, z);   return VoxelBlockCategory(back) ? back : sample(x, y, z - 1); }
    }
    return EVoxelBlockID::Air;
}

// ============================================================================
// TRUE BINARY GREEDY MESHER IMPLEMENTATION
// ============================================================================

void UVoxelMesher::BuildTrueBinaryGreedyMesh(
    const TArray<uint8>& Cats,
    const TArray<EVoxelBlockID>& Voxels,
    const FIntVector& Size,
    const FChunkNeighbors* VOXEL_RESTRICT Nbh,
    float VoxelUU,
    int32 XYScale,
    bool bUseAO,
    const class UVoxelBlockTable* VOXEL_RESTRICT BlockTable,
    FMeshBuffers& Out)
{
    SCOPE_CYCLE_COUNTER(STAT_VoxelGreedyMesh);

    const int32 SX = Size.X, SY = Size.Y, SZ = Size.Z;

    // Validate inputs
    if (SX <= 0 || SY <= 0 || SZ <= 0)
    {
        UE_LOG(LogTemp, Error, TEXT("[TrueBinaryMesher] Invalid chunk dimensions: %dx%dx%d"), SX, SY, SZ);
        return;
    }

    if (Cats.Num() != SX * SY * SZ)
    {
        UE_LOG(LogTemp, Error, TEXT("[TrueBinaryMesher] Category array size mismatch: got %d, expected %d"),
            Cats.Num(), SX * SY * SZ);
        return;
    }

    // Reset output buffers
    Out.Vertices.Reset();
    Out.Triangles.Reset();
    Out.UVs.Reset();
    Out.Colors.Reset();
    Out.Normals.Reset();

    // Pre-allocate output buffers
    const int32 approxQuads = FMath::Max(1, (SX * SY + SY * SZ + SX * SZ) / 2);
    Out.Vertices.Reserve(approxQuads * 4);
    Out.Triangles.Reserve(approxQuads * 6);
    Out.UVs.Reserve(approxQuads * 4);
    Out.Colors.Reserve(approxQuads * 4);
    Out.Normals.Reserve(approxQuads * 4);

    const double Sx = (double)VoxelUU * (double)XYScale;
    const double Sy = (double)VoxelUU * (double)XYScale;
    const double Sz = (double)VoxelUU;

    // Lambda helpers for indexing
    auto LinearIndex = [&](int32 x, int32 y, int32 z) -> int32
    {
        return x + y * SX + z * SX * SY;
    };

    auto CatAt = [&](int32 x, int32 y, int32 z) -> uint8
    {
        if (VOXEL_LIKELY((unsigned)x < (unsigned)SX && (unsigned)y < (unsigned)SY && (unsigned)z < (unsigned)SZ))
            return Cats[LinearIndex(x, y, z)];

        // Neighbor lookup
        if (!Nbh) return 0;

        if (z < 0 && Nbh->bHasZNeg && (unsigned)x < (unsigned)SX && (unsigned)y < (unsigned)SY)
            return VoxelBlockCategory(Nbh->ZNeg[x + y * SX]);
        if (z >= SZ && Nbh->bHasZPos && (unsigned)x < (unsigned)SX && (unsigned)y < (unsigned)SY)
            return VoxelBlockCategory(Nbh->ZPos[x + y * SX]);
        if (x < 0 && Nbh->bHasXNeg && (unsigned)y < (unsigned)SY && (unsigned)z < (unsigned)SZ)
            return VoxelBlockCategory(Nbh->XNeg[y + z * SY]);
        if (x >= SX && Nbh->bHasXPos && (unsigned)y < (unsigned)SY && (unsigned)z < (unsigned)SZ)
            return VoxelBlockCategory(Nbh->XPos[y + z * SY]);
        if (y < 0 && Nbh->bHasYNeg && (unsigned)x < (unsigned)SX && (unsigned)z < (unsigned)SZ)
            return VoxelBlockCategory(Nbh->YNeg[x + z * SX]);
        if (y >= SY && Nbh->bHasYPos && (unsigned)x < (unsigned)SX && (unsigned)z < (unsigned)SZ)
            return VoxelBlockCategory(Nbh->YPos[x + z * SX]);

        return 0;
    };

    auto BlockAt = [&](int x, int y, int z) -> EVoxelBlockID
    {
        if (VOXEL_LIKELY((unsigned)x < (unsigned)SX && (unsigned)y < (unsigned)SY && (unsigned)z < (unsigned)SZ))
            return Voxels[LinearIndex(x, y, z)];

        if (!Nbh) return EVoxelBlockID::Air;

        if (z < 0)       return (Nbh->bHasZNeg && (unsigned)x < (unsigned)SX && (unsigned)y < (unsigned)SY) ? Nbh->ZNeg[x + y * SX] : EVoxelBlockID::Air;
        if (z >= SZ)     return (Nbh->bHasZPos && (unsigned)x < (unsigned)SX && (unsigned)y < (unsigned)SY) ? Nbh->ZPos[x + y * SX] : EVoxelBlockID::Air;
        if (x < 0)       return (Nbh->bHasXNeg && (unsigned)y < (unsigned)SY && (unsigned)z < (unsigned)SZ) ? Nbh->XNeg[y + z * SY] : EVoxelBlockID::Air;
        if (x >= SX)     return (Nbh->bHasXPos && (unsigned)y < (unsigned)SY && (unsigned)z < (unsigned)SZ) ? Nbh->XPos[y + z * SY] : EVoxelBlockID::Air;
        if (y < 0)       return (Nbh->bHasYNeg && (unsigned)x < (unsigned)SX && (unsigned)z < (unsigned)SZ) ? Nbh->YNeg[x + z * SX] : EVoxelBlockID::Air;
        return (Nbh->bHasYPos && (unsigned)x < (unsigned)SX && (unsigned)z < (unsigned)SZ) ? Nbh->YPos[x + z * SX] : EVoxelBlockID::Air;
    };

    auto Neg = [](const FIntVector& v) -> FIntVector
    {
        return FIntVector(-v.X, -v.Y, -v.Z);
    };

    auto SampleAO = [&](const FIntVector& P, const FIntVector& U, const FIntVector& V) -> float
    {
        if (!bUseAO) return 1.f;

        auto solid = [&](const FIntVector& Q) { return CatAt(Q.X, Q.Y, Q.Z) != 0; };
        const bool SA = solid(P + U);
        const bool SB = solid(P + V);
        const bool SC = solid(P + U + V);
        if (SA && SB) return 0.f;
        return 1.f - (int(SA) + int(SB) + int(SC)) / 3.f;
    };

    auto ToByte = [](float v)->uint8 { return (uint8)FMath::Clamp((int32)(v * 255.f + 0.5f), 0, 255); };

    // Axis configuration
    struct Axis
    {
        FIntVector N, U, V;
        int32 Slice, DimU, DimV;
        EVoxelFaceDir FaceDir;
    };

    auto GetAxis = [](int dir) -> Axis
    {
        switch (dir)
        {
        case 0: return { { 1,0,0},{0,1,0},{0,0,1}, 0, 0, 0, EVoxelFaceDir::XPos };
        case 1: return { {-1,0,0},{0,1,0},{0,0,1}, 0, 0, 0, EVoxelFaceDir::XNeg };
        case 2: return { {0, 1,0},{1,0,0},{0,0,1}, 0, 0, 0, EVoxelFaceDir::YPos };
        case 3: return { {0,-1,0},{1,0,0},{0,0,1}, 0, 0, 0, EVoxelFaceDir::YNeg };
        case 4: return { {0,0, 1},{1,0,0},{0,1,0}, 0, 0, 0, EVoxelFaceDir::ZPos };
        default:return { {0,0,-1},{1,0,0},{0,1,0}, 0, 0, 0, EVoxelFaceDir::ZNeg };
        }
    };

    auto MakePos = [&](const FIntVector& Nrm, int s, int u, int v) -> FIntVector
    {
        if (Nrm.X) return { s, u, v };
        if (Nrm.Y) return { u, s, v };
        return { u, v, s };
    };

    auto GetDimensions = [&](int dir) -> FIntVector
    {
        switch (dir)
        {
        case 0: case 1: return { SX, SY, SZ };  // X-axis
        case 2: case 3: return { SY, SX, SZ };  // Y-axis
        default:        return { SZ, SX, SY };  // Z-axis
        }
    };

    // Emit a quad
    auto EmitQuad = [&](const FIntVector& Origin, const FIntVector& SpanU, const FIntVector& SpanV,
                       const FIntVector& Normal, EVoxelBlockID BlockID, EVoxelFaceDir FaceDir)
    {
        const FVector BasePos = FVector(Origin.X * Sx, Origin.Y * Sy, Origin.Z * Sz);
        const FVector U = FVector(SpanU.X * Sx, SpanU.Y * Sy, SpanU.Z * Sz);
        const FVector V = FVector(SpanV.X * Sx, SpanV.Y * Sy, SpanV.Z * Sz);
        const FVector Norm = FVector(Normal.X, Normal.Y, Normal.Z);

        // Get texture layer for this block + face
        const uint8 Layer = BlockTable ? (uint8)FMath::Clamp(BlockTable->GetLayer(FaceDir, BlockID), 0, 255) : 0;

        // AO samples (using Neg() helper to negate vectors)
        const float AO0 = SampleAO(Origin, Neg(SpanU), Neg(SpanV));
        const float AO1 = SampleAO(Origin + SpanU, SpanU, Neg(SpanV));
        const float AO2 = SampleAO(Origin + SpanU + SpanV, SpanU, SpanV);
        const float AO3 = SampleAO(Origin + SpanV, Neg(SpanU), SpanV);

        // Vertex positions
        const FVector V0 = BasePos;
        const FVector V1 = BasePos + U;
        const FVector V2 = BasePos + U + V;
        const FVector V3 = BasePos + V;

        // Add vertices
        const int32 BaseIdx = Out.Vertices.Num();
        Out.Vertices.Add(V0);
        Out.Vertices.Add(V1);
        Out.Vertices.Add(V2);
        Out.Vertices.Add(V3);

        // UVs
        Out.UVs.Add(FVector2D(0, 0));
        Out.UVs.Add(FVector2D(1, 0));
        Out.UVs.Add(FVector2D(1, 1));
        Out.UVs.Add(FVector2D(0, 1));

        // Colors (AO in RGB, Layer in Alpha) - matches BuildBinaryGreedyMesh_Cats
        const uint8 AO0_Byte = ToByte(AO0);
        const uint8 AO1_Byte = ToByte(AO1);
        const uint8 AO2_Byte = ToByte(AO2);
        const uint8 AO3_Byte = ToByte(AO3);
        Out.Colors.Add(FColor(AO0_Byte, AO0_Byte, AO0_Byte, Layer));
        Out.Colors.Add(FColor(AO1_Byte, AO1_Byte, AO1_Byte, Layer));
        Out.Colors.Add(FColor(AO2_Byte, AO2_Byte, AO2_Byte, Layer));
        Out.Colors.Add(FColor(AO3_Byte, AO3_Byte, AO3_Byte, Layer));

        // Normals
        Out.Normals.Add(Norm);
        Out.Normals.Add(Norm);
        Out.Normals.Add(Norm);
        Out.Normals.Add(Norm);

        // Indices (winding depends on normal direction)
        // Match the working greedy mesher: useStandardWinding = (normal.X < 0) || (normal.Y > 0) || (normal.Z < 0)
        const bool useStandardWinding = (Normal.X < 0 || Normal.Y > 0 || Normal.Z < 0);

        if (useStandardWinding)
        {
            // Standard winding: 0->1->2, 0->2->3
            Out.Triangles.Add(BaseIdx + 0);
            Out.Triangles.Add(BaseIdx + 1);
            Out.Triangles.Add(BaseIdx + 2);
            Out.Triangles.Add(BaseIdx + 0);
            Out.Triangles.Add(BaseIdx + 2);
            Out.Triangles.Add(BaseIdx + 3);
        }
        else
        {
            // Flipped winding: 0->2->1, 0->3->2
            Out.Triangles.Add(BaseIdx + 0);
            Out.Triangles.Add(BaseIdx + 2);
            Out.Triangles.Add(BaseIdx + 1);
            Out.Triangles.Add(BaseIdx + 0);
            Out.Triangles.Add(BaseIdx + 3);
            Out.Triangles.Add(BaseIdx + 2);
        }
    };

    // ========================================================================
    // MAIN MESHING LOOP - PROCESS EACH CATEGORY (SOLID, THEN SEMI)
    // ========================================================================

    for (uint8 CatType : { uint8(2), uint8(1) })  // Solid first, then semi-transparent
    {
        for (int dir = 0; dir < 6; ++dir)
        {
            Axis A = GetAxis(dir);
            const FIntVector Dims = GetDimensions(dir);
            A.Slice = Dims.X;
            A.DimU = Dims.Y;
            A.DimV = Dims.Z;

            // Process each slice along the normal direction
            for (int s = 0; s < A.Slice; ++s)
            {
                // ============================================================
                // BINARY OPTIMIZATION: Process rows of up to 64 voxels at once
                // ============================================================

                // OPTIMIZATION: Allocate ONCE per slice instead of per tile
                const int32 MaxTileWidth = FMath::Min(64, A.DimU);

                // Single array allocation per slice (not per tile!)
                TArray<uint64> Rows;
                Rows.SetNumZeroed(A.DimV);

                // Process rows in chunks of 64
                for (int uTile = 0; uTile < A.DimU; uTile += MaxTileWidth)
                {
                    const int32 uCount = FMath::Min(MaxTileWidth, A.DimU - uTile);

                    // Reset rows for this tile
                    for (int v = 0; v < A.DimV; ++v)
                    {
                        Rows[v] = 0;
                    }

                    // OPTIMIZATION: Build face-visibility masks with simplified logic
                    bool bTileHasAnyFaces = false;
                    for (int v = 0; v < A.DimV; ++v)
                    {
                        uint64 bits = 0ull;
                        for (int du = 0; du < uCount; ++du)
                        {
                            const int u = uTile + du;
                            const FIntVector P = MakePos(A.N, s, u, v);
                            const FIntVector Q = P + A.N;
                            const uint8 Ac = CatAt(P.X, P.Y, P.Z);
                            const uint8 Bc = CatAt(Q.X, Q.Y, Q.Z);

                            // CRITICAL: A face is visible ONLY if current=CatType AND neighbor=AIR
                            const bool visible = (Ac == CatType) && (Bc == 0);
                            bits |= (uint64)visible << du;
                        }

                        Rows[v] = bits;
                        bTileHasAnyFaces |= (bits != 0);
                    }

                    // Skip this tile if no faces
                    if (!bTileHasAnyFaces)
                        continue;

                    // ========================================================
                    // GREEDY MESHING: Merge adjacent faces using bit manipulation
                    // ========================================================

                    for (int v = 0; v < A.DimV; ++v)
                    {
                        uint64 row = Rows[v];

                        while (row)
                        {
                            // OPTIMIZATION: Use CTZ64 intrinsic to find first set bit
                            const int32 du0 = CountTrailingZeros64(row);
                            if (du0 >= 64) break;

                            // Get base block ID for the first visible face
                            const int32 u0 = uTile + du0;
                            const FIntVector P0 = MakePos(A.N, s, u0, v);
                            const EVoxelBlockID baseBlockID = BlockAt(P0.X, P0.Y, P0.Z);

                            // Count consecutive ones - but stop if block ID changes
                            const uint64 run = row >> du0;
                            int32 w = 0;
                            uint64 temp = run;

                            while ((temp & 1ull) != 0ull && w < uCount - du0)
                            {
                                // Check if this voxel has the same block ID
                                const int uCheck = u0 + w;
                                const FIntVector PCheck = MakePos(A.N, s, uCheck, v);
                                const EVoxelBlockID checkBlockID = BlockAt(PCheck.X, PCheck.Y, PCheck.Z);

                                if (checkBlockID != baseBlockID)
                                    break; // Don't merge different block types

                                temp >>= 1;
                                ++w;
                            }
                            if (w == 0) break;

                            // Clamp width
                            w = FMath::Min(w, uCount - du0);
                            const uint64 colMask = (w >= 64) ? ~0ull : ((1ull << w) - 1);

                            // Find height - stop if block ID changes
                            int32 h = 1;
                            while ((v + h) < A.DimV)
                            {
                                const uint64 checkMask = (Rows[v + h] >> du0) & colMask;
                                if (checkMask != colMask)
                                    break;

                                // CRITICAL: Check all voxels in the row have same block ID
                                bool blockIDMatches = true;
                                for (int du = 0; du < w; ++du)
                                {
                                    const int uCheck = u0 + du;
                                    const FIntVector PCheck = MakePos(A.N, s, uCheck, v + h);
                                    const EVoxelBlockID checkBlockID = BlockAt(PCheck.X, PCheck.Y, PCheck.Z);
                                    if (checkBlockID != baseBlockID)
                                    {
                                        blockIDMatches = false;
                                        break;
                                    }
                                }

                                if (!blockIDMatches)
                                    break;

                                ++h;
                            }

                            // Clear consumed bits directly in Rows array
                            const uint64 clearMask = ~(colMask << du0);
                            for (int dv = 0; dv < h; ++dv)
                            {
                                Rows[v + dv] &= clearMask;
                            }
                            row = Rows[v];

                            // Emit the merged quad
                            // CRITICAL: Add offset for positive normals (matches BuildBinaryGreedyMesh_Cats)
                            const FIntVector FaceBaseGrid = MakePos(A.N, s, u0, v)
                                + FIntVector(FMath::Max(0, A.N.X), FMath::Max(0, A.N.Y), FMath::Max(0, A.N.Z));

                            // CRITICAL: Use OwnerBlockForFace to get correct block owner (not direct BlockAt!)
                            const EVoxelBlockID Owner = OwnerBlockForFace(Voxels, Size, Nbh, FaceBaseGrid.X, FaceBaseGrid.Y, FaceBaseGrid.Z, A.FaceDir);

                            const FIntVector SpanU = A.U * w;
                            const FIntVector SpanV = A.V * h;

                            EmitQuad(FaceBaseGrid, SpanU, SpanV, A.N, Owner, A.FaceDir);
                        }
                    }
                }
            }
        }
    }

    // Log statistics
    const int32 BufferMemory =
        Out.Vertices.Num() * sizeof(FVector) +
        Out.Triangles.Num() * sizeof(int32) +
        Out.UVs.Num() * sizeof(FVector2D) +
        Out.Colors.Num() * sizeof(FColor) +
        Out.Normals.Num() * sizeof(FVector);
    INC_MEMORY_STAT_BY(STAT_VoxelMeshBufferMemory, BufferMemory);
}
