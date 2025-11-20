#include "VoxelMesher.h"
#include "VoxelStats.h"
#include "ProceduralMeshComponent.h"

#include "RealtimeMeshComponent.h"
#include "RealtimeMeshSimple.h"
#include "Core/RealtimeMeshBuilder.h"
#include "Core/RealtimeMeshDataStream.h"
#include "VoxelGPUMesher.h"
#include "VoxelSettings.h"
#include "HAL/IConsoleManager.h"
#include <VoxelBlockTable.h>
#include "VoxelOptimizationMacros.h"  // OPTIMIZATION: Branch hints, restrict, prefetch

static void WarnUnsupportedTileSize(const UVoxelSettings* Settings)
{
    if (!Settings) return;
    if (Settings->GPUMesherTileSize == 8) return;

    static bool bWarned = false;
    if (!bWarned)
    {
        bWarned = true;
        UE_LOG(LogTemp, Warning, TEXT("[VoxelGPU] GPUMesherTileSize is fixed to 8; requested value %d will be ignored."), Settings->GPUMesherTileSize);
    }
}

enum class EVoxelFaceDirection : uint8 { XPos, XNeg, YPos, YNeg, ZPos, ZNeg };

struct FFaceData
{
    FIntVector Normal;
    FIntVector TangentU;
    FIntVector TangentV;
    EVoxelFaceDirection Dir;
};

static const FFaceData GFaceDefs[6] =
{
    { { 1,  0,  0}, {0, 1, 0}, {0, 0, 1}, EVoxelFaceDirection::XPos },
    { {-1,  0,  0}, {0, 1, 0}, {0, 0, 1}, EVoxelFaceDirection::XNeg },
    { { 0,  1,  0}, {1, 0, 0}, {0, 0, 1}, EVoxelFaceDirection::YPos },
    { { 0, -1,  0}, {1, 0, 0}, {0, 0, 1}, EVoxelFaceDirection::YNeg },
    { { 0,  0,  1}, {1, 0, 0}, {0, 1, 0}, EVoxelFaceDirection::ZPos },
    { { 0,  0, -1}, {1, 0, 0}, {0, 1, 0}, EVoxelFaceDirection::ZNeg },
};

static FORCEINLINE int32 Idx(int32 x, int32 y, int32 z, int32 SX, int32 SY)
{
    return x + y * SX + z * SX * SY;
}
static FORCEINLINE bool IsInside(int32 x, int32 y, int32 z, int32 SX, int32 SY, int32 SZ)
{
    return (x >= 0 && y >= 0 && z >= 0 && x < SX && y < SY && z < SZ);
}

// --- bit ops ---------------------------------------------------------------
static FORCEINLINE int32 CTZ64(uint64 x)
{
#if defined(_MSC_VER)
    unsigned long idx;
    if (_BitScanForward64(&idx, x)) return (int32)idx;
    return 64;
#else
    return x ? (int32)__builtin_ctzll(x) : 64;
#endif
}

// Count trailing 1-bits from LSB. Equivalent to CTZ64(~x) with x!=~0ull edge.
static FORCEINLINE int32 CTONES64(uint64 x)
{
    if (x == ~0ull) return 64;
    return CTZ64(~x);
}

// --- small math utils ------------------------------------------------------
static FORCEINLINE FIntVector Neg(const FIntVector& v)
{
    return FIntVector(-v.X, -v.Y, -v.Z);
}

// ============================================================================
// UNIFIED TRIANGLE WINDING
// ============================================================================
// All CPU meshers should use this function to ensure consistent triangle order.
//
// Triangle winding must be consistent with face normal direction to ensure:
// - Correct backface culling (faces point outward)
// - Proper lighting calculations
// - Consistent behavior across all meshing algorithms
//
// The winding direction depends on the face normal to ensure CCW winding when
// viewed from outside the mesh. Different face directions need different index
// patterns to achieve this.
// ============================================================================

static FORCEINLINE void AddTrianglesWithCorrectWinding(
    FMeshBuffers& Out,
    int32 baseVertexIndex,
    const FVector& normal)
{
    // Determine winding based on face normal direction
    // This ensures counter-clockwise winding when viewed from outside
    const bool useStandardWinding = (normal.X < 0.0f) || (normal.Y > 0.0f) || (normal.Z < 0.0f);

    if (useStandardWinding)
    {
        // Standard winding: 0->1->2, 0->2->3
        Out.Triangles.Add(baseVertexIndex + 0);
        Out.Triangles.Add(baseVertexIndex + 1);
        Out.Triangles.Add(baseVertexIndex + 2);
        Out.Triangles.Add(baseVertexIndex + 0);
        Out.Triangles.Add(baseVertexIndex + 2);
        Out.Triangles.Add(baseVertexIndex + 3);
    }
    else
    {
        // Flipped winding: 0->2->1, 0->3->2
        Out.Triangles.Add(baseVertexIndex + 0);
        Out.Triangles.Add(baseVertexIndex + 2);
        Out.Triangles.Add(baseVertexIndex + 1);
        Out.Triangles.Add(baseVertexIndex + 0);
        Out.Triangles.Add(baseVertexIndex + 3);
        Out.Triangles.Add(baseVertexIndex + 2);
    }
}

static FORCEINLINE void EmitQuad_Fast(
    FMeshBuffers& Out,
    const FVector& base,
    const FVector& uvec,
    const FVector& vvec,
    const FVector& normal,
    int32 w, int32 h,
    bool bTintSemi,
    float ao00, float ao10, float ao11, float ao01)
{
    const int32 v0 = Out.Vertices.Num();

    Out.Vertices.Add(base);
    Out.Vertices.Add(base + uvec);
    Out.Vertices.Add(base + uvec + vvec);
    Out.Vertices.Add(base + vvec);

    // Flip normals to fix inverted faces
    Out.Normals.Add(-normal); Out.Normals.Add(-normal);
    Out.Normals.Add(-normal); Out.Normals.Add(-normal);

    Out.UVs.Add({ 0,0 });
    Out.UVs.Add({ (float)w,0 });
    Out.UVs.Add({ (float)w,(float)h });
    Out.UVs.Add({ 0,(float)h });

    const float shade = bTintSemi ? 0.7f : 1.0f;
    auto addGray = [&](float a) { const float v = shade * a; Out.Colors.Add(FColor(v, v, v, 1)); };
    addGray(ao00); addGray(ao10); addGray(ao11); addGray(ao01);

    // Use unified winding function
    AddTrianglesWithCorrectWinding(Out, v0, normal);
}



// Category access with neighbor support.
// OPTIMIZATION: RESTRICT keyword tells compiler that Cats pointer doesn't alias with Nbh
static FORCEINLINE uint8 CatAt_WithNbh(
    const TArray<uint8>& Cats, const FIntVector& Size,
    const FChunkNeighbors* VOXEL_RESTRICT Nbh,
    int32 x, int32 y, int32 z)
{
    const int32 SX = Size.X, SY = Size.Y, SZ = Size.Z;

    // OPTIMIZATION: Use LIKELY hint for common case (inside chunk)
    auto inside = [&](int32 X, int32 Y, int32 Z) { return (unsigned)X < (unsigned)SX && (unsigned)Y < (unsigned)SY && (unsigned)Z < (unsigned)SZ; };
    auto idx = [&](int32 X, int32 Y, int32 Z) { return X + Y * SX + Z * SX * SY; };

    if (VOXEL_LIKELY(inside(x, y, z))) return Cats[idx(x, y, z)];
    if (!Nbh) return 0;

    // Z-
    if (z < 0) { if ((unsigned)x < (unsigned)SX && (unsigned)y < (unsigned)SY && Nbh->bHasZNeg) return VoxelBlockCategory(Nbh->ZNeg[x + y * SX]); return 0; }
    // Z+
    if (z >= SZ) { if ((unsigned)x < (unsigned)SX && (unsigned)y < (unsigned)SY && Nbh->bHasZPos) return VoxelBlockCategory(Nbh->ZPos[x + y * SX]); return 0; }
    // X-
    if (x < 0) { if ((unsigned)y < (unsigned)SY && Nbh->bHasXNeg) return VoxelBlockCategory(Nbh->XNeg[y + z * SY]); return 2; }
    // X+
    if (x >= SX) { if ((unsigned)y < (unsigned)SY && Nbh->bHasXPos) return VoxelBlockCategory(Nbh->XPos[y + z * SY]); return 0; }
    // Y-
    if (y < 0) { if ((unsigned)x < (unsigned)SX && Nbh->bHasYNeg) return VoxelBlockCategory(Nbh->YNeg[x + z * SX]); return 0; }
    // Y+
    if (y >= SY) { if ((unsigned)x < (unsigned)SX && Nbh->bHasYPos) return VoxelBlockCategory(Nbh->YPos[x + z * SX]); return 0; }
    return 0;
}

// Thread-local reusable mask buffer to avoid allocations
static thread_local TArray<uint8> GReusableMask;
// Return the solid voxel that owns a face (depends on face direction and chunk borders).
// OPTIMIZATION: RESTRICT keyword enables better vectorization
static FORCEINLINE EVoxelBlockID OwnerBlockForFace(
    const TArray<EVoxelBlockID>& V, const FIntVector& Size,
    const FChunkNeighbors* VOXEL_RESTRICT Nbh, int x, int y, int z, EVoxelFaceDir dir)
{
    const int SX = Size.X, SY = Size.Y, SZ = Size.Z;

    // OPTIMIZATION: Mark common case as LIKELY for better branch prediction
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

    // “Back” = solid side of the face.  If that is empty, fall back to the other side.
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
// OPTIMIZATION: RESTRICT keywords on pointer parameters for better vectorization
void UVoxelMesher::BuildGreedyMesh(
    const TArray<EVoxelBlockID>& Voxels,
    const FIntVector& Size,
    const FChunkNeighbors* VOXEL_RESTRICT Nbh,
    float VoxelUU,
    int32 XYScale,
    bool bUseAO,
    const class UVoxelBlockTable* VOXEL_RESTRICT BlockTable,
    FMeshBuffers& Out,
    const FVoxelLightData* LightData)
{
    SCOPE_CYCLE_COUNTER(STAT_VoxelGreedyMesh);

    const int32 SX = Size.X, SY = Size.Y, SZ = Size.Z;
    Out.Vertices.Reset(); Out.Triangles.Reset(); Out.UVs.Reset(); Out.Colors.Reset(); Out.Normals.Reset();

    // OPTIMIZATION: Pre-allocate output buffers to avoid reallocations
    // Estimate: each slice can have at most DimU*DimV quads, but greedy merging reduces this significantly
    const int32 approxQuads = FMath::Max(1, (SX * SY + SY * SZ + SX * SZ) / 4);
    Out.Vertices.Reserve(approxQuads * 4);
    Out.Triangles.Reserve(approxQuads * 6);
    Out.UVs.Reserve(approxQuads * 4);
    Out.Colors.Reserve(approxQuads * 4);
    Out.Normals.Reserve(approxQuads * 4);

    // Pre-calculate max mask size needed
    const int32 MaxMaskSize = FMath::Max3(SX * SY, SY * SZ, SX * SZ);
    GReusableMask.SetNumUninitialized(MaxMaskSize);

    // Anisotropic world scale: XY scaled by XYScale; Z stays 1×
    const double ScaleX = (double)VoxelUU * (double)XYScale;
    const double ScaleY = (double)VoxelUU * (double)XYScale;
    const double ScaleZ = (double)VoxelUU;

    // Precompute a compact category array for all voxels within this chunk.  This
    // avoids repeatedly calling VoxelBlockCategory() in the inner loops of the
    // mesher and improves cache locality.  Each entry is 0 for air, 1 for
    // semi‑solid and 2 for fully solid voxels.  Note: the neighbour arrays are
    // not transformed and will still be queried via VoxelBlockCategory().
    TArray<uint8> LocalCats;
    LocalCats.SetNumUninitialized(SX * SY * SZ);
    for (int32 idx = 0; idx < SX * SY * SZ; ++idx)
    {
        LocalCats[idx] = VoxelBlockCategory(Voxels[idx]);
    }

    // Returns true if the voxel at (x,y,z) is non‑transparent (air returns
    // false).  Uses the precomputed LocalCats array for voxels within the
    // current chunk.  For neighbours, fall back to VoxelBlockCategory().
    auto Solid = [&](int32 x, int32 y, int32 z) -> bool
        {
            if (IsInside(x, y, z, SX, SY, SZ))
            {
                return LocalCats[Idx(x, y, z, SX, SY)] != 0;
            }
            // Out of bounds: sample neighbour chunk categories
            // Out of bounds: sample neighbour chunk categories.  We handle
            // vertical neighbours (z) as well as horizontal ones.  Note that
            // z refers to the index relative to this chunk (0..SZ‑1) but
            // may fall outside when sampling neighbours.
            if (!Nbh) return false;
            // Sample vertical neighbours first
            if (z < 0)
            {
                if (x >= 0 && x < SX && y >= 0 && y < SY && Nbh->bHasZNeg)
                    return VoxelBlockCategory(Nbh->ZNeg[x + y * SX]) != 0;
                return false;
            }
            if (z >= SZ)
            {
                if (x >= 0 && x < SX && y >= 0 && y < SY && Nbh->bHasZPos)
                    return VoxelBlockCategory(Nbh->ZPos[x + y * SX]) != 0;
                return false;
            }
            // Horizontal neighbours
            if (x < 0 && y >= 0 && y < SY && Nbh->bHasXNeg)
                return VoxelBlockCategory(Nbh->XNeg[y + z * SY]) != 0;
            if (x >= SX && y >= 0 && y < SY && Nbh->bHasXPos)
                return VoxelBlockCategory(Nbh->XPos[y + z * SY]) != 0;
            if (y < 0 && x >= 0 && x < SX && Nbh->bHasYNeg)
                return VoxelBlockCategory(Nbh->YNeg[x + z * SX]) != 0;
            if (y >= SY && x >= 0 && x < SX && Nbh->bHasYPos)
                return VoxelBlockCategory(Nbh->YPos[x + z * SX]) != 0;
            return false;
        };

    // Returns a coarse block category for the voxel at (x,y,z).  See
    // VoxelBlockCategory() for details.  Category 0 means empty/air.
    auto BlockCategoryAt = [&](int32 x, int32 y, int32 z) -> uint8
        {
            if (IsInside(x, y, z, SX, SY, SZ))
            {
                return LocalCats[Idx(x, y, z, SX, SY)];
            }
            if (!Nbh) return 0;
            // Vertical neighbours
            if (z < 0)
            {
                if (x >= 0 && x < SX && y >= 0 && y < SY && Nbh->bHasZNeg)
                    return VoxelBlockCategory(Nbh->ZNeg[x + y * SX]);
                return 0;
            }
            if (z >= SZ)
            {
                if (x >= 0 && x < SX && y >= 0 && y < SY && Nbh->bHasZPos)
                    return VoxelBlockCategory(Nbh->ZPos[x + y * SX]);
                return 0;
            }
            // Horizontal neighbours
            if (x < 0 && y >= 0 && y < SY && Nbh->bHasXNeg)
                return VoxelBlockCategory(Nbh->XNeg[y + z * SY]);
            if (x >= SX && y >= 0 && y < SY && Nbh->bHasXPos)
                return VoxelBlockCategory(Nbh->XPos[y + z * SY]);
            if (y < 0 && x >= 0 && x < SX && Nbh->bHasYNeg)
                return VoxelBlockCategory(Nbh->YNeg[x + z * SX]);
            if (y >= SY && x >= 0 && x < SX && Nbh->bHasYPos)
                return VoxelBlockCategory(Nbh->YPos[x + z * SX]);
            return 0;
        };
    // decide which block is the “owner” of this face: the solid one
    auto BlockAt = [&](int X, int Y, int Z)->EVoxelBlockID {
        const int SX = Size.X, SY = Size.Y, SZ = Size.Z;
        if ((unsigned)X < (unsigned)SX && (unsigned)Y < (unsigned)SY && (unsigned)Z < (unsigned)SZ)
            return Voxels[X + Y * SX + Z * SX * SY];
        // optional: fall back to neighbor slices for layer, or default
        return EVoxelBlockID::Stone;
        };
    auto Greyscale = [](float V) -> FColor { return FColor(V, V, V, 1.0f); };
    auto Idx2D = [](int32 u, int32 v, int32 DimU) { return u + v * DimU; };

    auto SampleAO = [&](FIntVector P, FIntVector N, FIntVector U, FIntVector V) -> float
        {
            if (!bUseAO) return 1.0f;
            const FIntVector A = P + U;
            const FIntVector B = P + V;
            const FIntVector C = P + U + V;
            const bool SolidA = Solid(A.X, A.Y, A.Z);
            const bool SolidB = Solid(B.X, B.Y, B.Z);
            const bool SolidC = Solid(C.X, C.Y, C.Z);
            if (SolidA && SolidB) return 0.0f;
            return 1.0f - ((int32)SolidA + (int32)SolidB + (int32)SolidC) / 3.0f;
        };

    // OPTIMIZATION: Define hot-path lambdas once instead of per-quad
    auto ToByte = [](float v) { return (uint8)FMath::Clamp(FMath::RoundToInt(v * 255.f), 0, 255); };
    auto FaceDirToEnum = [](const FIntVector& N)->EVoxelFaceDir {
        if (N.X == 1) return EVoxelFaceDir::XPos;
        if (N.X == -1) return EVoxelFaceDir::XNeg;
        if (N.Y == 1) return EVoxelFaceDir::YPos;
        if (N.Y == -1) return EVoxelFaceDir::YNeg;
        if (N.Z == 1) return EVoxelFaceDir::ZPos;
        return EVoxelFaceDir::ZNeg;
    };

    for (const FFaceData& Face : GFaceDefs)
    {
        const FIntVector N = Face.Normal, U = Face.TangentU, V = Face.TangentV;
        const int32 SliceCount = (N.X != 0) ? SX : (N.Y != 0) ? SY : SZ;
        const int32 DimU = (N.X != 0) ? SY : (N.Y != 0) ? SX : SX;
        const int32 DimV = (N.X != 0) ? SZ : (N.Y != 0) ? SZ : SY;
        // OPTIMIZATION: Compute face direction once per face instead of per quad
        const EVoxelFaceDir FaceDir = FaceDirToEnum(N);
        auto MakeP = [&](int32 s, int32 u, int32 v)->FIntVector
            {
                if (N.X != 0) return FIntVector(s, u, v);
                if (N.Y != 0) return FIntVector(u, s, v);
                return FIntVector(u, v, s);
            };

        for (int32 slice = 0; slice < SliceCount; ++slice)
        {
            // Reuse mask buffer - zero only what we need
            const int32 MaskSize = DimU * DimV;
            FMemory::Memzero(GReusableMask.GetData(), MaskSize * sizeof(uint8));

            // OPTIMIZATION: Track if slice has any faces to avoid empty slice processing
            bool bHasAnyFaces = false;
            for (int32 vv = 0; vv < DimV; ++vv)
            {
                // OPTIMIZATION: Row-level early exit for fully air rows
                bool bRowHasFaces = false;

                for (int32 uu = 0; uu < DimU; ++uu)
                {
                    const FIntVector P = MakeP(slice, uu, vv);
                    const FIntVector Q = P + N;
                    // Determine block categories for the current voxel and its neighbor.
                    const uint8 CatA = BlockCategoryAt(P.X, P.Y, P.Z);
                    const uint8 CatB = BlockCategoryAt(Q.X, Q.Y, Q.Z);
                    // Emit a face only when the categories differ and the current voxel is non‑empty.
                    const bool bFacesBetweenNonAir = false; // cvar or setting
                    const bool bExpose = (CatA != 0) && ((CatB == 0) || (bFacesBetweenNonAir && (CatA != CatB)));
                    const uint8 MaskVal = bExpose ? CatA : 0;

                    GReusableMask[Idx2D(uu, vv, DimU)] = MaskVal;
                    bRowHasFaces |= (MaskVal != 0);
                }

                bHasAnyFaces |= bRowHasFaces;
            }

            // OPTIMIZATION: Skip greedy merging entirely if this slice has no faces
            if (!bHasAnyFaces)
                continue;

            int32 v = 0;
            while (v < DimV)
            {
                // OPTIMIZATION: Skip entirely empty rows (common in sparse voxel data)
                bool bRowHasData = false;
                for (int32 uu = 0; uu < DimU; ++uu)
                {
                    if (GReusableMask[Idx2D(uu, v, DimU)] != 0)
                    {
                        bRowHasData = true;
                        break;
                    }
                }

                if (!bRowHasData)
                {
                    ++v;
                    continue;
                }

                int32 u = 0;
                while (u < DimU)
                {
                    const int32 idx = Idx2D(u, v, DimU);
                    const uint8 CurrentType = GReusableMask[idx];
                    // Skip empty entries (no face)
                    if (CurrentType == 0)
                    {
                        ++u;
                        continue;
                    }

                    // Extend the quad along U while the mask value stays the same
                    int32 Width = 1;
                    while ((u + Width) < DimU && GReusableMask[Idx2D(u + Width, v, DimU)] == CurrentType)
                    {
                        ++Width;
                    }

                    // Extend the quad along V while all mask values in the current row are the same
                    int32 Height = 1;
                    bool Stop = false;
                    while ((v + Height) < DimV && !Stop)
                    {
                        for (int32 w = 0; w < Width; ++w)
                        {
                            if (GReusableMask[Idx2D(u + w, v + Height, DimU)] != CurrentType)
                            {
                                Stop = true;
                                break;
                            }
                        }
                        if (!Stop)
                        {
                            ++Height;
                        }
                    }

                    for (int32 dv = 0; dv < Height; ++dv)
                        for (int32 du = 0; du < Width; ++du)
                        {
                            GReusableMask[Idx2D(u + du, v + dv, DimU)] = 0;
                        }

                    const FIntVector Base = MakeP(slice, u, v);
                    const FIntVector Offset(FMath::Max(0, N.X), FMath::Max(0, N.Y), FMath::Max(0, N.Z));
                    const FIntVector FaceBase = Base + Offset;

                    // Anisotropic scale
                    const FVector WorldBase(
                        FaceBase.X * ScaleX,
                        FaceBase.Y * ScaleY,
                        FaceBase.Z * ScaleZ);

                    const FVector VecU(
                        U.X * (double)Width * ScaleX,
                        U.Y * (double)Width * ScaleY,
                        U.Z * (double)Width * ScaleZ);

                    const FVector VecV(
                        V.X * (double)Height * ScaleX,
                        V.Y * (double)Height * ScaleY,
                        V.Z * (double)Height * ScaleZ);

                    const int32 VStart = Out.Vertices.Num();

                    Out.Vertices.Add(WorldBase);
                    Out.Vertices.Add(WorldBase + VecU);
                    Out.Vertices.Add(WorldBase + VecU + VecV);
                    Out.Vertices.Add(WorldBase + VecV);

                    const FVector FaceNormal = FVector(N); // outward
                    Out.Normals.Add(FaceNormal); Out.Normals.Add(FaceNormal);
                    Out.Normals.Add(FaceNormal); Out.Normals.Add(FaceNormal);

                    // Use unified winding function
                    AddTrianglesWithCorrectWinding(Out, VStart, FaceNormal);

                    const EVoxelBlockID Owner = OwnerBlockForFace(Voxels, Size, Nbh, FaceBase.X, FaceBase.Y, FaceBase.Z, FaceDir);
                    const uint8 Layer = BlockTable ? (uint8)FMath::Clamp(BlockTable->GetLayer(FaceDir, Owner), 0, 255) : 0;


                    Out.UVs.Add(FVector2D(0, 0));
                    Out.UVs.Add(FVector2D((float)Width, 0));
                    Out.UVs.Add(FVector2D((float)Width, (float)Height));
                    Out.UVs.Add(FVector2D(0, (float)Height));

                    const FIntVector UNeg(-U.X, -U.Y, -U.Z), VNeg(-V.X, -V.Y, -V.Z);
                    const FIntVector AOBase = FaceBase;
                    // Apply a tint factor based on the block category.  Semi‑solid blocks
                    // (category 1) are tinted slightly darker to distinguish them visually.
                    const float Shade = (CurrentType == 1 ? 0.7f : 1.0f);
                    const uint8 AO00 = ToByte(Shade * SampleAO(AOBase, N, UNeg, VNeg));
                    const uint8 AO10 = ToByte(Shade * SampleAO(AOBase + U * Width, N, U, VNeg));
                    const uint8 AO11 = ToByte(Shade * SampleAO(AOBase + U * Width + V * Height, N, U, V));
                    const uint8 AO01 = ToByte(Shade * SampleAO(AOBase + V * Height, N, UNeg, V));

                    Out.Colors.Add(FColor(AO00, AO00, AO00, Layer));
                    Out.Colors.Add(FColor(AO10, AO10, AO10, Layer));
                    Out.Colors.Add(FColor(AO11, AO11, AO11, Layer));
                    Out.Colors.Add(FColor(AO01, AO01, AO01, Layer));
                    u += Width;
                }
                ++v;
            }
        }
    }

    // VOXEL LIGHTING: Apply light values to vertex colors
    if (LightData && LightData->Data.Num() > 0)
    {
        const float VoxelScale = VoxelUU * XYScale;
        for (int32 i = 0; i < Out.Vertices.Num(); ++i)
        {
            // Convert vertex world position back to voxel coordinates
            const FVector& WorldPos = Out.Vertices[i];
            const int32 VX = FMath::FloorToInt(WorldPos.X / VoxelScale);
            const int32 VY = FMath::FloorToInt(WorldPos.Y / VoxelScale);
            const int32 VZ = FMath::FloorToInt(WorldPos.Z / VoxelUU);  // Z scale is always 1x

            // Sample light value (with bounds check)
            uint8 LightValue = 15;  // Default to full brightness
            if (VX >= 0 && VX < LightData->SizeX && VY >= 0 && VY < LightData->SizeY && VZ >= 0 && VZ < LightData->SizeZ)
            {
                LightValue = LightData->GetCombinedLight(VX, VY, VZ);
            }

            // Convert light (0-15) to brightness (0.0-1.0)
            // Add minimum ambient light to avoid pure black (creative choice)
            const float MinAmbient = 0.05f;  // 5% minimum visibility
            const float LightBrightness = MinAmbient + (1.0f - MinAmbient) * (LightValue / 15.0f);

            // Multiply existing vertex color (which contains AO) by light brightness
            FColor& Col = Out.Colors[i];
            Col.R = FMath::Clamp(FMath::RoundToInt(Col.R * LightBrightness), 0, 255);
            Col.G = FMath::Clamp(FMath::RoundToInt(Col.G * LightBrightness), 0, 255);
            Col.B = FMath::Clamp(FMath::RoundToInt(Col.B * LightBrightness), 0, 255);
        }
    }

    // Update memory stats
    const int32 BufferMemory =
        Out.Vertices.Num() * sizeof(FVector) +
        Out.Triangles.Num() * sizeof(int32) +
        Out.UVs.Num() * sizeof(FVector2D) +
        Out.Colors.Num() * sizeof(FLinearColor) +
        Out.Normals.Num() * sizeof(FVector);
    INC_MEMORY_STAT_BY(STAT_VoxelMeshBufferMemory, BufferMemory);
}

//=== Binary Greedy Mesher ===//

void UVoxelMesher::BuildBinaryGreedyMesh(
    const TArray<uint8>& Cats,
    const TArray<EVoxelBlockID>& Voxels,
    const FIntVector& Size,
    const FChunkNeighbors* Nbh,
    float VoxelUU,
    int32 XYScale,
    bool bUseAO,
    const class UVoxelBlockTable* BlockTable,
    FMeshBuffers& Out,
    const FVoxelLightData* LightData)
{
    // OPTIMIZATION: Use pre-computed Cats array (eliminates 32K+ conversion loop)
    // Now directly call TrueBinaryMesher
    BuildTrueBinaryGreedyMesh(Cats, Voxels, Size, Nbh, VoxelUU, XYScale, bUseAO, BlockTable, Out);

    // VOXEL LIGHTING: Apply light values to vertex colors
    if (LightData && LightData->Data.Num() > 0)
    {
        const float VoxelScale = VoxelUU * XYScale;
        for (int32 i = 0; i < Out.Vertices.Num(); ++i)
        {
            const FVector& WorldPos = Out.Vertices[i];
            const int32 VX = FMath::FloorToInt(WorldPos.X / VoxelScale);
            const int32 VY = FMath::FloorToInt(WorldPos.Y / VoxelScale);
            const int32 VZ = FMath::FloorToInt(WorldPos.Z / VoxelUU);

            uint8 LightValue = 15;
            if (VX >= 0 && VX < LightData->SizeX && VY >= 0 && VY < LightData->SizeY && VZ >= 0 && VZ < LightData->SizeZ)
            {
                LightValue = LightData->GetCombinedLight(VX, VY, VZ);
            }

            const float MinAmbient = 0.05f;
            const float LightBrightness = MinAmbient + (1.0f - MinAmbient) * (LightValue / 15.0f);

            FColor& Col = Out.Colors[i];
            Col.R = FMath::Clamp(FMath::RoundToInt(Col.R * LightBrightness), 0, 255);
            Col.G = FMath::Clamp(FMath::RoundToInt(Col.G * LightBrightness), 0, 255);
            Col.B = FMath::Clamp(FMath::RoundToInt(Col.B * LightBrightness), 0, 255);
        }
    }
}

void UVoxelMesher::BuildHeightfieldMesh(
    const TArray<int32>& Heights,
    int32 SamplesX,
    int32 SamplesY,
    int32 ChunkSizeX,
    int32 ChunkSizeY,
    int32 XYScale,
    float VoxelUU,
    FMeshBuffers& Out)
{
    SCOPE_CYCLE_COUNTER(STAT_VoxelHeightfieldMesh);

    // Pre-calculate sample positions once
    TArray<double> Xpos, Ypos;
    Xpos.SetNumUninitialized(SamplesX);
    Ypos.SetNumUninitialized(SamplesY);

    const double UU = (double)VoxelUU;

    for (int32 x = 0; x < SamplesX; ++x)
    {
        const int32 LocalX = (x == SamplesX - 1) ? ChunkSizeX : FMath::Min(x * XYScale, ChunkSizeX);
        Xpos[x] = (double)LocalX * UU;
    }
    for (int32 y = 0; y < SamplesY; ++y)
    {
        const int32 LocalY = (y == SamplesY - 1) ? ChunkSizeY : FMath::Min(y * XYScale, ChunkSizeY);
        Ypos[y] = (double)LocalY * UU;
    }

    auto H = [&](int32 x, int32 y)->double
        {
            const int32 cx = FMath::Clamp(x, 0, SamplesX - 1);
            const int32 cy = FMath::Clamp(y, 0, SamplesY - 1);
            return (double)Heights[cx + cy * SamplesX] * UU;
        };

    Out.Vertices.Reset(); Out.Triangles.Reset(); Out.UVs.Reset(); Out.Colors.Reset(); Out.Normals.Reset();

    // Reserve memory to avoid reallocations
    const int32 QuadCount = (SamplesX - 1) * (SamplesY - 1);
    Out.Vertices.Reserve(QuadCount * 4);
    Out.Triangles.Reserve(QuadCount * 6);
    Out.UVs.Reserve(QuadCount * 4);
    Out.Colors.Reserve(QuadCount * 4);
    Out.Normals.Reserve(QuadCount * 4);

    auto Greyscale = [](float V) -> FColor { return FColor(V, V, V, 1.0f); };

    for (int32 y = 0; y < SamplesY - 1; ++y)
    {
        for (int32 x = 0; x < SamplesX - 1; ++x)
        {
            const FVector P00(Xpos[x], Ypos[y], H(x, y));
            const FVector P10(Xpos[x + 1], Ypos[y], H(x + 1, y));
            const FVector P01(Xpos[x], Ypos[y + 1], H(x, y + 1));
            const FVector P11(Xpos[x + 1], Ypos[y + 1], H(x + 1, y + 1));

            const int32 VStart = Out.Vertices.Num();
            Out.Vertices.Add(P00);
            Out.Vertices.Add(P10);
            Out.Vertices.Add(P11);
            Out.Vertices.Add(P01);

            // Flip normals to fix inverted faces
            const FVector N = -FVector::CrossProduct(P10 - P00, P01 - P00).GetSafeNormal();
            Out.Normals.Add(N); Out.Normals.Add(N); Out.Normals.Add(N); Out.Normals.Add(N);

            Out.UVs.Add(FVector2D(0, 0));
            Out.UVs.Add(FVector2D(1, 0));
            Out.UVs.Add(FVector2D(1, 1));
            Out.UVs.Add(FVector2D(0, 1));

            Out.Colors.Add(Greyscale(1.0f));
            Out.Colors.Add(Greyscale(1.0f));
            Out.Colors.Add(Greyscale(1.0f));
            Out.Colors.Add(Greyscale(1.0f));

            // Use unified winding function
            AddTrianglesWithCorrectWinding(Out, VStart, N);
        }
    }

    // Update memory stats
    const int32 BufferMemory =
        Out.Vertices.Num() * sizeof(FVector) +
        Out.Triangles.Num() * sizeof(int32) +
        Out.UVs.Num() * sizeof(FVector2D) +
        Out.Colors.Num() * sizeof(FLinearColor) +
        Out.Normals.Num() * sizeof(FVector);
    INC_MEMORY_STAT_BY(STAT_VoxelMeshBufferMemory, BufferMemory);
}

void UVoxelMesher::ApplyToPMC(
    UProceduralMeshComponent* PMC,
    const FMeshBuffers& Bufs,
    bool bCreateCollision)
{
    SCOPE_CYCLE_COUNTER(STAT_VoxelApplyToPMC);
    if (!PMC) return;

    PMC->ClearAllMeshSections();

    // Use FColor overload (smaller vertex stream than LinearColor)
    PMC->CreateMeshSection(
        0,
        Bufs.Vertices,
        Bufs.Triangles,
        Bufs.Normals,
        Bufs.UVs,
        Bufs.Colors,
        TArray<FProcMeshTangent>{},
        bCreateCollision
    );

    PMC->SetCollisionEnabled(
        bCreateCollision ? ECollisionEnabled::QueryAndPhysics
        : ECollisionEnabled::NoCollision);
}

static void BuildStreamSetFromBuffers(
    const FMeshBuffers& Bufs,
    RealtimeMesh::FRealtimeMeshStreamSet& OutStreams)
{
    using FIndexType = uint32;
    RealtimeMesh::TRealtimeMeshBuilderLocal<FIndexType, FPackedNormal, FVector2DHalf, 1> Builder(OutStreams);

    const int32 NumV = Bufs.Vertices.Num();
    const bool bHaveNormals = (Bufs.Normals.Num() == NumV);
    const bool bHaveUV0 = (Bufs.UVs.Num() == NumV);
    const bool bHaveColors = (Bufs.Colors.Num() == NumV);

    Builder.EnableTangents();
    Builder.EnableTexCoords();
    Builder.EnableColors();
    Builder.EnablePolyGroups();

    // Add all vertices first
    Builder.ReserveAdditionalVertices(NumV);
    for (int32 i = 0; i < NumV; ++i)
    {
        const FVector3f P = (FVector3f)Bufs.Vertices[i];
        auto V = Builder.AddVertex(P);

        if (bHaveNormals)
        {
            // Use the provided normal as-is, and build a reasonable tangent orthonormal to it.
            const FVector3f N = ((FVector3f)Bufs.Normals[i]).GetSafeNormal();
            const FVector3f Up = (FMath::Abs(N.Z) < 0.999f) ? FVector3f(0, 0, 1) : FVector3f(0, 1, 0);
            const FVector3f T = (Up ^ N).GetSafeNormal();
            V.SetNormalAndTangent(N, T);
        }

        // ALWAYS set color - default to white if not provided
        if (bHaveColors)
        {
            V.SetColor(Bufs.Colors[i]);
        }
        else
        {
            V.SetColor(FColor::White); // Default to white
        }

        if (bHaveUV0)
        {
            V.SetTexCoord((FVector2f)Bufs.UVs[i]);
        }
    }

    // Add all triangles
    const int32 NumI = Bufs.Triangles.Num();
    Builder.ReserveAdditionalTriangles(NumI / 3);

    for (int32 t = 0; t < NumI; t += 3)
    {
        Builder.AddTriangle(
            (FIndexType)Bufs.Triangles[t + 0],
            (FIndexType)Bufs.Triangles[t + 1],
            (FIndexType)Bufs.Triangles[t + 2],
            /*PolyGroup*/ 0);
    }
}

void UVoxelMesher::ApplyToRMC(
    URealtimeMeshComponent* RMC,
    const FMeshBuffers& Bufs,
    bool bCreateCollision)
{
    if (!RMC) return;

    // Early out for empty geometry: just remove group, disable collision
    const int32 NumVertices = Bufs.Vertices.Num();
    const int32 NumIndices = Bufs.Triangles.Num();
    const bool bHasGeometry = (NumVertices > 0 && NumIndices >= 3);

    URealtimeMeshSimple* MeshAsset = RMC->InitializeRealtimeMesh<URealtimeMeshSimple>();
    if (!MeshAsset) return;

    const FRealtimeMeshLODKey LOD0(0);
    const FRealtimeMeshSectionGroupKey GroupKey =
        FRealtimeMeshSectionGroupKey::Create(LOD0, FName(*FString::Printf(TEXT("ChunkGroup_%s"), *RMC->GetName())));
    const FRealtimeMeshSectionKey SectionKey =
        FRealtimeMeshSectionKey::CreateForPolyGroup(GroupKey, 0);

    auto SetCollision = [RMC, bCreateCollision, bHasGeometry]() {
        RMC->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
        };

    // Always remove first, then recreate (this prevents all races)
    MeshAsset->RemoveSectionGroup(GroupKey)
        .Next([MeshAsset, GroupKey, SectionKey, Bufs, bCreateCollision, bHasGeometry, SetCollision](ERealtimeMeshProxyUpdateStatus /*RemoveStatus*/)
            {
                if (!bHasGeometry)
                {
                    SetCollision();
                    return;
                }

                RealtimeMesh::FRealtimeMeshStreamSet Streams;
                BuildStreamSetFromBuffers(Bufs, Streams);

                MeshAsset->CreateSectionGroup(GroupKey, MoveTemp(Streams))
                    .Next([MeshAsset, SectionKey, bCreateCollision, bHasGeometry, SetCollision](ERealtimeMeshProxyUpdateStatus Status)
                        {
                            if (Status == ERealtimeMeshProxyUpdateStatus::NoUpdate)
                            {
                                FRealtimeMeshSectionConfig SectionConfig(0); // Material slot 0
                                SectionConfig.bIsVisible = bHasGeometry;
                                //UE_LOG(LogTemp, Warning, TEXT("[ApplyRMC:: Huh, on es passé par la est les collision sont: %s"), (bCreateCollision ? TEXT("true") : TEXT("false")));
                                MeshAsset->UpdateSectionConfig(SectionKey, SectionConfig, bCreateCollision);
                            }
                            SetCollision();
                        });
            });
}



bool UVoxelMesher::BuildGreedyMesh_GPU(
    const TArray<EVoxelBlockID>& Voxels,
    const FIntVector& Size,
    const FChunkNeighbors* Neighbors,
    int32 XYScale,
    float VoxelUU,
    FMeshBuffers& Out,
    const UVoxelSettings* Settings)
{
    SCOPE_CYCLE_COUNTER(STAT_VoxelGPUMeshing);

    FGPUMeshBuildParams P;
    // Convert the EVoxelBlockID array into a temporary category array on the stack.
    // Each voxel category is an 8‑bit value: 0 = air, 1 = semi‑solid, 2 = solid.
    const int32 TotalCount = Size.X * Size.Y * Size.Z;
    TArray<uint8> CatData;
    CatData.SetNumUninitialized(TotalCount);
    for (int32 idx = 0; idx < TotalCount; ++idx)
    {
        CatData[idx] = VoxelBlockCategory(Voxels[idx]);
    }

    P.Voxels = CatData.GetData();
    P.SizeX = Size.X;
    P.SizeY = Size.Y;
    P.SizeZ = Size.Z;
    P.XYScale = FMath::Max(1, XYScale);
   P.DefaultAO = 3;
    P.VoxelUU = VoxelUU;
    const int32 MaxVertsSetting = (Settings && Settings->MaxGPUVertexBufferSize > 0)
        ? FMath::Clamp(Settings->MaxGPUVertexBufferSize, 1, INT32_MAX)
        : INT32_MAX;
    P.MaxOutputVerts = MaxVertsSetting;
    P.bAggressiveCulling = Settings && Settings->bGPUAggressiveCulling;
    WarnUnsupportedTileSize(Settings);

    // Convert neighbour border arrays to category arrays on the fly.  The
    // neighbour arrays store full EVoxelBlockID values; we must convert them
    // into categories for the GPU mesher.  These arrays are sized to match
    // the current chunk’s border dimensions and live until this function
    // returns.
    TArray<uint8> NXN, NXP, NYN, NYP;
    TArray<uint8> NZN, NZP;

    if (Neighbors)
    {
        P.bHasNeighborXN = Neighbors->bHasXNeg;
        P.bHasNeighborXP = Neighbors->bHasXPos;
        P.bHasNeighborYN = Neighbors->bHasYNeg;
        P.bHasNeighborYP = Neighbors->bHasYPos;

        // Negative X neighbour: size is SizeY * SizeZ
        if (P.bHasNeighborXN)
        {
            const int32 BorderCount = Size.Y * Size.Z;
            NXN.SetNumUninitialized(BorderCount);
            for (int32 i = 0; i < BorderCount; ++i)
            {
                NXN[i] = VoxelBlockCategory(Neighbors->XNeg[i]);
            }
            P.NeighborXN = NXN.GetData();
        }
        // Positive X neighbour
        if (P.bHasNeighborXP)
        {
            const int32 BorderCount = Size.Y * Size.Z;
            NXP.SetNumUninitialized(BorderCount);
            for (int32 i = 0; i < BorderCount; ++i)
            {
                NXP[i] = VoxelBlockCategory(Neighbors->XPos[i]);
            }
            P.NeighborXP = NXP.GetData();
        }
        // Negative Y neighbour: size is SizeX * SizeZ
        if (P.bHasNeighborYN)
        {
            const int32 BorderCount = Size.X * Size.Z;
            NYN.SetNumUninitialized(BorderCount);
            for (int32 i = 0; i < BorderCount; ++i)
            {
                NYN[i] = VoxelBlockCategory(Neighbors->YNeg[i]);
            }
            P.NeighborYN = NYN.GetData();
        }
        // Positive Y neighbour
        if (P.bHasNeighborYP)
        {
            const int32 BorderCount = Size.X * Size.Z;
            NYP.SetNumUninitialized(BorderCount);
            for (int32 i = 0; i < BorderCount; ++i)
            {
                NYP[i] = VoxelBlockCategory(Neighbors->YPos[i]);
            }
            P.NeighborYP = NYP.GetData();
        }
        // Negative Z neighbour: size is SizeX * SizeY
        P.bHasNeighborZN = Neighbors->bHasZNeg;
        if (P.bHasNeighborZN)
        {
            const int32 BorderCount = Size.X * Size.Y;
            NZN.SetNumUninitialized(BorderCount);
            for (int32 i = 0; i < BorderCount; ++i)
            {
                NZN[i] = VoxelBlockCategory(Neighbors->ZNeg[i]);
            }
            P.NeighborZN = NZN.GetData();
        }
        // Positive Z neighbour: size is SizeX * SizeY
        P.bHasNeighborZP = Neighbors->bHasZPos;
        if (P.bHasNeighborZP)
        {
            const int32 BorderCount = Size.X * Size.Y;
            NZP.SetNumUninitialized(BorderCount);
            for (int32 i = 0; i < BorderCount; ++i)
            {
                NZP[i] = VoxelBlockCategory(Neighbors->ZPos[i]);
            }
            P.NeighborZP = NZP.GetData();
        }
    }

    TArray<uint32> Packed;
    const bool bGPUOk = FVoxelGPUMesher::BuildPackedVerts_GPU(P, Packed);
    const UVoxelBlockTable* BT = (Settings && Settings->BlockTable.Get()) ? Settings->BlockTable.Get() : nullptr;
    // Optional: Compare against CPU greedy for diagnostics (even on GPU failure)
    static auto* CVarCompareCPU = IConsoleManager::Get().FindConsoleVariable(TEXT("r.Voxel.GPU.CompareCPU"));
    const bool bDoCompare = (CVarCompareCPU && CVarCompareCPU->GetInt() != 0);
    if (bDoCompare)
    {
        FMeshBuffers Cpu;
        UVoxelMesher::BuildGreedyMesh(Voxels, Size, Neighbors, VoxelUU, XYScale, /*bUseAO=*/false,BT,Cpu);
        const int32 CTri = Cpu.Triangles.Num() / 3;
        const int32 GTri = bGPUOk ? (Packed.Num() / 3) : 0;
        UE_LOG(LogTemp, Warning, TEXT("[VoxelGPU Compare] GPU Tris=%d CPU Tris=%d Size=(%d,%d,%d)"), GTri, CTri, Size.X, Size.Y, Size.Z);
        if (!bGPUOk && CTri > 0)
        {
            UE_LOG(LogTemp, Warning, TEXT("[VoxelGPU Compare] GPU path failed while CPU produced triangles."));
        }
    }

    if (!bGPUOk)
    {
        return false;
    }

    FVoxelGPUMesher::DecodePackedVertsToMeshBuffers(Packed, Out, VoxelUU, XYScale, Size.X, Size.Y, Size.Z);

    // Optional: Compare against CPU greedy for diagnostics

    if (CVarCompareCPU && CVarCompareCPU->GetInt() != 0)
    {
        FMeshBuffers Cpu;
        UVoxelMesher::BuildGreedyMesh(Voxels, Size, Neighbors, VoxelUU, XYScale, /*bUseAO=*/false,BT, Cpu);
        const int32 GTri = Out.Triangles.Num() / 3;
        const int32 CTri = Cpu.Triangles.Num() / 3;
        UE_LOG(LogTemp, Warning, TEXT("[VoxelGPU Compare] GPU Tris=%d CPU Tris=%d Size=(%d,%d,%d)"), GTri, CTri, Size.X, Size.Y, Size.Z);
        if (CTri > 0 && GTri == 0)
        {
            UE_LOG(LogTemp, Warning, TEXT("[VoxelGPU Compare] GPU produced empty mesh while CPU has data."));
        }
    }

    // Update memory stats
    const int32 BufferMemory =
        Out.Vertices.Num() * sizeof(FVector) +
        Out.Triangles.Num() * sizeof(int32) +
        Out.UVs.Num() * sizeof(FVector2D) +
        Out.Colors.Num() * sizeof(FLinearColor) +
        Out.Normals.Num() * sizeof(FVector);
    INC_MEMORY_STAT_BY(STAT_VoxelMeshBufferMemory, BufferMemory);

    return true;
}

bool UVoxelMesher::BuildGreedyMesh_GPU_Async(
    const TArray<EVoxelBlockID>& Voxels,
    const FIntVector& Size,
    const FChunkNeighbors* Neighbors,
    int32 XYScale,
    float VoxelUU,
    TFunction<void(bool bSuccess, TArray<uint32>&& Packed, int32 SizeX, int32 SizeY, int32 SizeZ, int32 XYScaleParam, float VoxelUUParam)> Completion,
    const UVoxelSettings* Settings)
{
    if (!Completion)
    {
        return false;
    }

    const int32 TotalCount = Size.X * Size.Y * Size.Z;
    if (TotalCount <= 0)
    {
        Completion(false, TArray<uint32>(), Size.X, Size.Y, Size.Z, XYScale, VoxelUU);
        return false;
    }

    FGPUMeshBuildParams P;
    TArray<uint8> CatData;
    CatData.SetNumUninitialized(TotalCount);
    for (int32 idx = 0; idx < TotalCount; ++idx)
    {
        CatData[idx] = VoxelBlockCategory(Voxels[idx]);
    }

    P.Voxels = CatData.GetData();
    P.SizeX = Size.X;
    P.SizeY = Size.Y;
    P.SizeZ = Size.Z;
    P.XYScale = FMath::Max(1, XYScale);
    P.DefaultAO = 3;
    P.VoxelUU = VoxelUU;
    const int32 MaxVertsSettingAsync = (Settings && Settings->MaxGPUVertexBufferSize > 0)
        ? FMath::Clamp(Settings->MaxGPUVertexBufferSize, 1, INT32_MAX)
        : INT32_MAX;
    P.MaxOutputVerts = MaxVertsSettingAsync;
    P.bAggressiveCulling = Settings && Settings->bGPUAggressiveCulling;
    WarnUnsupportedTileSize(Settings);

    TArray<uint8> NXN, NXP, NYN, NYP;
    TArray<uint8> NZN, NZP;

    if (Neighbors)
    {
        P.bHasNeighborXN = Neighbors->bHasXNeg;
        P.bHasNeighborXP = Neighbors->bHasXPos;
        P.bHasNeighborYN = Neighbors->bHasYNeg;
        P.bHasNeighborYP = Neighbors->bHasYPos;

        if (P.bHasNeighborXN)
        {
            const int32 Count = Size.Y * Size.Z;
            NXN.SetNumUninitialized(Count);
            for (int32 i = 0; i < Count; ++i)
            {
                NXN[i] = VoxelBlockCategory(Neighbors->XNeg[i]);
            }
            P.NeighborXN = NXN.GetData();
        }
        if (P.bHasNeighborXP)
        {
            const int32 Count = Size.Y * Size.Z;
            NXP.SetNumUninitialized(Count);
            for (int32 i = 0; i < Count; ++i)
            {
                NXP[i] = VoxelBlockCategory(Neighbors->XPos[i]);
            }
            P.NeighborXP = NXP.GetData();
        }
        if (P.bHasNeighborYN)
        {
            const int32 Count = Size.X * Size.Z;
            NYN.SetNumUninitialized(Count);
            for (int32 i = 0; i < Count; ++i)
            {
                NYN[i] = VoxelBlockCategory(Neighbors->YNeg[i]);
            }
            P.NeighborYN = NYN.GetData();
        }
        if (P.bHasNeighborYP)
        {
            const int32 Count = Size.X * Size.Z;
            NYP.SetNumUninitialized(Count);
            for (int32 i = 0; i < Count; ++i)
            {
                NYP[i] = VoxelBlockCategory(Neighbors->YPos[i]);
            }
            P.NeighborYP = NYP.GetData();
        }
        P.bHasNeighborZN = Neighbors->bHasZNeg;
        if (P.bHasNeighborZN)
        {
            const int32 Count = Size.X * Size.Y;
            NZN.SetNumUninitialized(Count);
            for (int32 i = 0; i < Count; ++i)
            {
                NZN[i] = VoxelBlockCategory(Neighbors->ZNeg[i]);
            }
            P.NeighborZN = NZN.GetData();
        }
        P.bHasNeighborZP = Neighbors->bHasZPos;
        if (P.bHasNeighborZP)
        {
            const int32 Count = Size.X * Size.Y;
            NZP.SetNumUninitialized(Count);
            for (int32 i = 0; i < Count; ++i)
            {
                NZP[i] = VoxelBlockCategory(Neighbors->ZPos[i]);
            }
            P.NeighborZP = NZP.GetData();
        }
    }

    const bool bLaunched = FVoxelGPUMesher::BuildPackedVerts_GPU_Async(P,
        [Completion, SizeX = Size.X, SizeY = Size.Y, SizeZ = Size.Z, XYParam = XYScale, VoxelUUParam = VoxelUU](bool bSuccess, TArray<uint32>&& Packed)
        {
            Completion(bSuccess, MoveTemp(Packed), SizeX, SizeY, SizeZ, XYParam, VoxelUUParam);
        });

    return bLaunched;
}

// OPTIMIZATION: RESTRICT keywords on pointer parameters for better vectorization
void UVoxelMesher::BuildBinaryGreedyMesh_Cats(
    const TArray<uint8>& Cats,                 // 0=air,1=semi,2=solid
    const TArray<EVoxelBlockID>& Voxels,       // full IDs for layer lookups
    const FIntVector& Size,
    const FChunkNeighbors* VOXEL_RESTRICT Nbh,
    float VoxelUU,
    int32 XYScale,
    bool bUseAO,
    const class UVoxelBlockTable* VOXEL_RESTRICT BlockTable,  // face→layer map
    FMeshBuffers& Out)
{
    SCOPE_CYCLE_COUNTER(STAT_VoxelGreedyMesh);

    const int32 SX = Size.X, SY = Size.Y, SZ = Size.Z;

    // SAFETY: Validate input dimensions
    if (SX <= 0 || SY <= 0 || SZ <= 0)
    {
        UE_LOG(LogTemp, Error, TEXT("[VoxelMesher] Invalid chunk dimensions: %dx%dx%d"), SX, SY, SZ);
        return;
    }

    if (Cats.Num() != SX * SY * SZ)
    {
        UE_LOG(LogTemp, Error, TEXT("[VoxelMesher] Category array size mismatch: got %d, expected %d"),
            Cats.Num(), SX * SY * SZ);
        return;
    }

    Out.Vertices.Reset();
    Out.Triangles.Reset();
    Out.UVs.Reset();
    Out.Colors.Reset();
    Out.Normals.Reset();

    const int32 approxQuads = FMath::Max(1, (SX * SY + SY * SZ + SX * SZ) / 2);
    Out.Vertices.Reserve(approxQuads * 4);
    Out.Triangles.Reserve(approxQuads * 6);
    Out.UVs.Reserve(approxQuads * 4);
    Out.Colors.Reserve(approxQuads * 4);
    Out.Normals.Reserve(approxQuads * 4);

    const double Sx = (double)VoxelUU * (double)XYScale;
    const double Sy = (double)VoxelUU * (double)XYScale;
    const double Sz = (double)VoxelUU;

    auto Idx3 = [&](int32 x, int32 y, int32 z) -> int32
        {
            // SAFETY: Bounds checking
            if (x < 0 || y < 0 || z < 0 || x >= SX || y >= SY || z >= SZ)
                return -1;
            return x + y * SX + z * SX * SY;
        };

    // OPTIMIZATION: Hot-path accessors with branch hints to eliminate call overhead
    auto Inside = [&](int32 x, int32 y, int32 z) -> bool
        {
            // Use unsigned comparison trick: single comparison checks both bounds
            return (unsigned)x < (unsigned)SX && (unsigned)y < (unsigned)SY && (unsigned)z < (unsigned)SZ;
        };
    auto BlockAt = [&](int x, int y, int z) ->EVoxelBlockID
        {
            // OPTIMIZATION: Fast path for inside chunk (most common case) - mark as LIKELY
            if (VOXEL_LIKELY((unsigned)x < (unsigned)SX && (unsigned)y < (unsigned)SY && (unsigned)z < (unsigned)SZ))
                return Voxels[x + y * SX + z * SX * SY];

            // Slow path: neighbor lookup
            if (!Nbh) return EVoxelBlockID::Air;

            if (z < 0)       return (Nbh->bHasZNeg && (unsigned)x < (unsigned)SX && (unsigned)y < (unsigned)SY) ? Nbh->ZNeg[x + y * SX] : EVoxelBlockID::Air;
            if (z >= SZ)     return (Nbh->bHasZPos && (unsigned)x < (unsigned)SX && (unsigned)y < (unsigned)SY) ? Nbh->ZPos[x + y * SX] : EVoxelBlockID::Air;
            if (x < 0)       return (Nbh->bHasXNeg && (unsigned)y < (unsigned)SY && (unsigned)z < (unsigned)SZ) ? Nbh->XNeg[y + z * SY] : EVoxelBlockID::Air;
            if (x >= SX)     return (Nbh->bHasXPos && (unsigned)y < (unsigned)SY && (unsigned)z < (unsigned)SZ) ? Nbh->XPos[y + z * SY] : EVoxelBlockID::Air;
            if (y < 0)       return (Nbh->bHasYNeg && (unsigned)x < (unsigned)SX && (unsigned)z < (unsigned)SZ) ? Nbh->YNeg[x + z * SX] : EVoxelBlockID::Air;
            /* y >= SY */    return (Nbh->bHasYPos && (unsigned)x < (unsigned)SX && (unsigned)z < (unsigned)SZ) ? Nbh->YPos[x + z * SX] : EVoxelBlockID::Air;
        };

    auto CatAt = [&](int32 x, int32 y, int32 z) -> uint8
        {
            // OPTIMIZATION: Fast path for inside chunk - mark as LIKELY
            if (VOXEL_LIKELY((unsigned)x < (unsigned)SX && (unsigned)y < (unsigned)SY && (unsigned)z < (unsigned)SZ))
                return Cats[x + y * SX + z * SX * SY];

            // Slow path: neighbor lookup
            if (!Nbh) return 0;

            // Vertical neighbors - reordered for better branch prediction (Z most common)
            if (z < 0 && Nbh->bHasZNeg && (unsigned)x < (unsigned)SX && (unsigned)y < (unsigned)SY)
                return VoxelBlockCategory(Nbh->ZNeg[x + y * SX]);
            if (z >= SZ && Nbh->bHasZPos && (unsigned)x < (unsigned)SX && (unsigned)y < (unsigned)SY)
                return VoxelBlockCategory(Nbh->ZPos[x + y * SX]);

            // Horizontal neighbors
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

    auto Neg = [](const FIntVector& v) -> FIntVector
        {
            return FIntVector(-v.X, -v.Y, -v.Z);
        };

    struct Axis
    {
        FIntVector N, U, V;
        int32 Slice, DimU, DimV;
    };

    auto Axes = [&](int dir) -> Axis
        {
            switch (dir)
            {
            case 0: return { { 1,0,0},{0,1,0},{0,0,1}, SX, SY, SZ };
            case 1: return { {-1,0,0},{0,1,0},{0,0,1}, SX, SY, SZ };
            case 2: return { {0, 1,0},{1,0,0},{0,0,1}, SY, SX, SZ };
            case 3: return { {0,-1,0},{1,0,0},{0,0,1}, SY, SX, SZ };
            case 4: return { {0,0, 1},{1,0,0},{0,1,0}, SZ, SX, SY };
            default:return { {0,0,-1},{1,0,0},{0,1,0}, SZ, SX, SY };
            }
        };

    auto MakeP = [&](const FIntVector& Nrm, int s, int u, int v) -> FIntVector
        {
            if (Nrm.X) return { s, u, v };
            if (Nrm.Y) return { u, s, v };
            return { u, v, s };
        };

    // Use thread-safe local buffer instead of thread_local to prevent race conditions
    TArray<uint64> Rows;

    // OPTIMIZATION: Hoist hot-path lambdas outside loops
    auto ToByte = [](float v)->uint8 { return (uint8)FMath::Clamp((int32)(v * 255.f + 0.5f), 0, 255); };
    auto FaceDirFromNormal = [](const FIntVector& n)->EVoxelFaceDir {
        if (n.X == 1) return EVoxelFaceDir::XPos; if (n.X == -1) return EVoxelFaceDir::XNeg;
        if (n.Y == 1) return EVoxelFaceDir::YPos; if (n.Y == -1) return EVoxelFaceDir::YNeg;
        return n.Z == 1 ? EVoxelFaceDir::ZPos : EVoxelFaceDir::ZNeg;
    };

    // Process categories: solids then semis
    for (uint8 CatType : { uint8(2), uint8(1) })
    {
        const bool bTintSemi = (CatType == 1);

        for (int dir = 0; dir < 6; ++dir)
        {
            const Axis A = Axes(dir);
            const FIntVector Udir = A.U;
            const FIntVector Vdir = A.V;
            const FIntVector Nrm = A.N;
            // OPTIMIZATION: Compute face direction once per face direction
            const EVoxelFaceDir FaceDir = FaceDirFromNormal(Nrm);

            for (int s = 0; s < A.Slice; ++s)
            {
                // SAFETY: Limit tile width to prevent overflow
                const int32 MaxTileWidth = FMath::Min(64, A.DimU);

                for (int uTile = 0; uTile < A.DimU; uTile += MaxTileWidth)
                {
                    const int uCount = FMath::Min(MaxTileWidth, A.DimU - uTile);

                    // SAFETY: Initialize rows array
                    Rows.Reset();
                    Rows.SetNumZeroed(A.DimV);

                    // OPTIMIZATION: Build face-visibility masks with early empty check
                    bool bTileHasAnyFaces = false;
                    for (int v = 0; v < A.DimV; ++v)
                    {
                        uint64 bits = 0ull;
                        for (int du = 0; du < uCount; ++du)
                        {
                            const int u = uTile + du;
                            const FIntVector P = MakeP(Nrm, s, u, v);
                            const FIntVector Q = P + Nrm;
                            const uint8 Ac = CatAt(P.X, P.Y, P.Z);
                            const uint8 Bc = CatAt(Q.X, Q.Y, Q.Z);
                            const bool visible = (Ac == CatType) && (Bc == 0);
                            bits |= (uint64)visible << du;
                        }

                        Rows[v] = bits;
                        bTileHasAnyFaces |= (bits != 0);
                    }

                    // OPTIMIZATION: Skip greedy merging entirely if this tile has no faces
                    if (!bTileHasAnyFaces)
                        continue;

                    // Greedy merge rectangles
                    for (int v = 0; v < A.DimV; ++v)
                    {
                        // SAFETY: Bounds check
                        if (v < 0 || v >= Rows.Num())
                            continue;

                        uint64 row = Rows[v];

                        while (row)
                        {
                            // OPTIMIZATION: Use CTZ64 intrinsic instead of manual loop
                            const int du0 = CTZ64(row);
                            if (du0 >= 64) break;

                            // Count consecutive ones - but stop if block ID changes
                            const uint64 run = row >> du0;
                            int w = 0;
                            uint64 temp = run;

                            // Get the base block ID for the first visible face
                            const int u0 = uTile + du0;
                            const FIntVector P0 = MakeP(Nrm, s, u0, v);
                            const EVoxelBlockID baseBlockID = BlockAt(P0.X, P0.Y, P0.Z);

                            while ((temp & 1ull) != 0ull && w < uCount - du0)
                            {
                                // Check if this voxel has the same block ID
                                const int uCheck = u0 + w;
                                const FIntVector PCheck = MakeP(Nrm, s, uCheck, v);
                                const EVoxelBlockID checkBlockID = BlockAt(PCheck.X, PCheck.Y, PCheck.Z);

                                if (checkBlockID != baseBlockID)
                                    break; // Don't merge different block types

                                temp >>= 1;
                                ++w;
                            }
                            if (w == 0) break;

                            // SAFETY: Clamp width
                            w = FMath::Min(w, uCount - du0);
                            const uint64 colMask = (w >= 64) ? ~0ull : ((1ull << w) - 1);

                            // Find height - stop if block ID changes
                            int h = 1;
                            while ((v + h) < A.DimV && (v + h) < Rows.Num())
                            {
                                const uint64 checkMask = (Rows[v + h] >> du0) & colMask;
                                if (checkMask != colMask)
                                    break;

                                // CRITICAL: Check all voxels in the row have same block ID
                                bool blockIDMatches = true;
                                for (int du = 0; du < w; ++du)
                                {
                                    const int uCheck = u0 + du;
                                    const FIntVector PCheck = MakeP(Nrm, s, uCheck, v + h);
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

                            // Clear consumed bits
                            const uint64 clearMask = ~(colMask << du0);
                            for (int dv = 0; dv < h; ++dv)
                            {
                                const int vIdx = v + dv;
                                if (vIdx >= 0 && vIdx < Rows.Num())
                                    Rows[vIdx] &= clearMask;
                            }
                            row = Rows[v];

                            // Emit quad
                            const FIntVector FaceBaseGrid = MakeP(Nrm, s, u0, v)
                                + FIntVector(FMath::Max(0, Nrm.X), FMath::Max(0, Nrm.Y), FMath::Max(0, Nrm.Z));
                            const EVoxelBlockID Owner = OwnerBlockForFace(Voxels, Size, Nbh, FaceBaseGrid.X, FaceBaseGrid.Y, FaceBaseGrid.Z, FaceDir);
                            const uint8 Layer = BlockTable ? (uint8)FMath::Clamp(BlockTable->GetLayer(FaceDir, Owner), 0, 255) : 0;

                            // prepare vectors
                            const FVector base(FaceBaseGrid.X * Sx, FaceBaseGrid.Y * Sy, FaceBaseGrid.Z * Sz);
                            const FVector uvec(Udir.X * w * Sx, Udir.Y * w * Sy, Udir.Z * w * Sz);
                            const FVector vvec(Vdir.X * h * Sx, Vdir.Y * h * Sy, Vdir.Z * h * Sz);
                            const FVector normal(Nrm);                      // outward

                            // AO as floats in [0,1]
                            float ao00 = 1, ao10 = 1, ao11 = 1, ao01 = 1;
                            if (bUseAO) {
                                ao00 = SampleAO(FaceBaseGrid, Neg(Udir), Neg(Vdir));
                                ao10 = SampleAO(FaceBaseGrid + Udir * w, Udir, Neg(Vdir));
                                ao11 = SampleAO(FaceBaseGrid + Udir * w + Vdir * h, Udir, Vdir);
                                ao01 = SampleAO(FaceBaseGrid + Vdir * h, Neg(Udir), Vdir);
                            }


                            // add verts
                            const int v0 = Out.Vertices.Num();
                            Out.Vertices.Add(base);
                            Out.Vertices.Add(base + uvec);
                            Out.Vertices.Add(base + uvec + vvec);
                            Out.Vertices.Add(base + vvec);

                            // Flip normals to fix inverted faces
                            Out.Normals.Add(normal); Out.Normals.Add(normal);
                            Out.Normals.Add(normal); Out.Normals.Add(normal);

                            // UVs (scaled by quad size for proper texture tiling)
                            Out.UVs.Add(FVector2D(0, 0));
                            Out.UVs.Add(FVector2D((float)w, 0));
                            Out.UVs.Add(FVector2D((float)w, (float)h));
                            Out.UVs.Add(FVector2D(0, (float)h));

                            // AO→bytes, layer in A
                            const uint8 AO00 = ToByte(ao00), AO10 = ToByte(ao10), AO11 = ToByte(ao11), AO01 = ToByte(ao01);
                            Out.Colors.Add(FColor(AO00, AO00, AO00, Layer));
                            Out.Colors.Add(FColor(AO10, AO10, AO10, Layer));
                            Out.Colors.Add(FColor(AO11, AO11, AO11, Layer));
                            Out.Colors.Add(FColor(AO01, AO01, AO01, Layer));

                            // robust CCW winding relative to outward normal
                            const FVector Uf = FVector(uvec).GetSafeNormal();
                            const FVector Vf = FVector(vvec).GetSafeNormal();
                            const float sign = FVector::DotProduct(FVector::CrossProduct(Uf, Vf), normal);
                            if (sign < 0.f)
                            {
                                Out.Triangles.Add(v0 + 0); Out.Triangles.Add(v0 + 1); Out.Triangles.Add(v0 + 2);
                                Out.Triangles.Add(v0 + 0); Out.Triangles.Add(v0 + 2); Out.Triangles.Add(v0 + 3);
                            }
                            else
                            {
                                Out.Triangles.Add(v0 + 0); Out.Triangles.Add(v0 + 2); Out.Triangles.Add(v0 + 1);
                                Out.Triangles.Add(v0 + 0); Out.Triangles.Add(v0 + 3); Out.Triangles.Add(v0 + 2);
                            }

                        }
                    }
                }
            }
        }
    }

    const int32 bytes =
        Out.Vertices.Num() * sizeof(FVector) +
        Out.Triangles.Num() * sizeof(int32) +
        Out.UVs.Num() * sizeof(FVector2D) +
        Out.Colors.Num() * sizeof(FColor)+
        Out.Normals.Num() * sizeof(FVector);
    INC_MEMORY_STAT_BY(STAT_VoxelMeshBufferMemory, bytes);
}

// ============================================================================
// NAIVE MESHER (DEBUG ONLY - NO GREEDY OPTIMIZATION)
// ============================================================================

void UVoxelMesher::BuildNaiveMesh(
    const TArray<EVoxelBlockID>& Voxels,
    const FIntVector& Size,
    const FChunkNeighbors* Nbh,
    float VoxelUU,
    int32 XYScale,
    bool bUseAO,
    const class UVoxelBlockTable* BlockTable,
    FMeshBuffers& Out)
{
    SCOPE_CYCLE_COUNTER(STAT_VoxelGreedyMesh);

    const int32 SX = Size.X, SY = Size.Y, SZ = Size.Z;

    if (SX <= 0 || SY <= 0 || SZ <= 0)
    {
        UE_LOG(LogTemp, Error, TEXT("[NaiveMesher] Invalid chunk dimensions: %dx%dx%d"), SX, SY, SZ);
        return;
    }

    if (Voxels.Num() != SX * SY * SZ)
    {
        UE_LOG(LogTemp, Error, TEXT("[NaiveMesher] Voxel array size mismatch: got %d, expected %d"),
            Voxels.Num(), SX * SY * SZ);
        return;
    }

    Out.Vertices.Reset();
    Out.Triangles.Reset();
    Out.UVs.Reset();
    Out.Colors.Reset();
    Out.Normals.Reset();

    const double Sx = (double)VoxelUU * (double)XYScale;
    const double Sy = (double)VoxelUU * (double)XYScale;
    const double Sz = (double)VoxelUU;

    // Helper: get voxel at position (handles neighbors)
    auto GetVoxel = [&](int32 x, int32 y, int32 z) -> EVoxelBlockID
    {
        // Inside chunk
        if (x >= 0 && x < SX && y >= 0 && y < SY && z >= 0 && z < SZ)
            return Voxels[x + y * SX + z * SX * SY];

        // Check neighbors
        if (!Nbh) return EVoxelBlockID::Air;

        if (z < 0 && x >= 0 && x < SX && y >= 0 && y < SY && Nbh->bHasZNeg)
            return Nbh->ZNeg[x + y * SX];
        if (z >= SZ && x >= 0 && x < SX && y >= 0 && y < SY && Nbh->bHasZPos)
            return Nbh->ZPos[x + y * SX];
        if (x < 0 && y >= 0 && y < SY && z >= 0 && z < SZ && Nbh->bHasXNeg)
            return Nbh->XNeg[y + z * SY];
        if (x >= SX && y >= 0 && y < SY && z >= 0 && z < SZ && Nbh->bHasXPos)
            return Nbh->XPos[y + z * SY];
        if (y < 0 && x >= 0 && x < SX && z >= 0 && z < SZ && Nbh->bHasYNeg)
            return Nbh->YNeg[x + z * SX];
        if (y >= SY && x >= 0 && x < SX && z >= 0 && z < SZ && Nbh->bHasYPos)
            return Nbh->YPos[x + z * SX];

        return EVoxelBlockID::Air;
    };

    // Helper: check if voxel is solid
    auto IsSolid = [](EVoxelBlockID id) -> bool
    {
        return VoxelBlockCategory(id) != 0; // 0 = air
    };

    // Helper: emit a quad face
// Outward normals, CCW from outside. No flipping.
    auto EmitFace = [&](const FVector& v0, const FVector& v1, const FVector& v2, const FVector& v3,
        const FVector& normal, EVoxelBlockID blockId, EVoxelFaceDir faceDir)
        {
            const int32 baseIdx = Out.Vertices.Num();

            Out.Vertices.Add(v0);
            Out.Vertices.Add(v1);
            Out.Vertices.Add(v2);
            Out.Vertices.Add(v3);

            Out.Normals.Add(normal);
            Out.Normals.Add(normal);
            Out.Normals.Add(normal);
            Out.Normals.Add(normal);

            Out.UVs.Add(FVector2D(0, 0));
            Out.UVs.Add(FVector2D(1, 0));
            Out.UVs.Add(FVector2D(1, 1));
            Out.UVs.Add(FVector2D(0, 1));

            const uint8 layer = BlockTable ? (uint8)FMath::Clamp(BlockTable->GetLayer(faceDir, blockId), 0, 255) : 0;
            const uint8 ao = bUseAO ? 200 : 255;
            Out.Colors.Add(FColor(ao, ao, ao, layer));
            Out.Colors.Add(FColor(ao, ao, ao, layer));
            Out.Colors.Add(FColor(ao, ao, ao, layer));
            Out.Colors.Add(FColor(ao, ao, ao, layer));

            // CCW winding for front faces
            Out.Triangles.Add(baseIdx + 0);
            Out.Triangles.Add(baseIdx + 1);
            Out.Triangles.Add(baseIdx + 2);
            Out.Triangles.Add(baseIdx + 0);
            Out.Triangles.Add(baseIdx + 2);
            Out.Triangles.Add(baseIdx + 3);
        };


    // DEBUG: Count air voxels that should be solid
    int32 AirCount = 0, SolidCount = 0;

    // Iterate through all voxels and emit faces
    for (int32 z = 0; z < SZ; ++z)
    {
        for (int32 y = 0; y < SY; ++y)
        {
            for (int32 x = 0; x < SX; ++x)
            {
                const EVoxelBlockID current = GetVoxel(x, y, z);
                if (!IsSolid(current))
                {
                    AirCount++;
                    // DEBUG: Log unexpected air blocks at surface level
                    if (z >= SZ / 2 && z <= SZ / 2 + 10)
                    {
                        const EVoxelBlockID below = GetVoxel(x, y, z - 1);
                        if (IsSolid(below))
                        {
                            UE_LOG(LogTemp, Warning, TEXT("[NaiveMesher] Unexpected air at (%d,%d,%d), below is solid"), x, y, z);
                        }
                    }
                    continue; // Skip air
                }

                SolidCount++;
                const FVector basePos(x * Sx, y * Sy, z * Sz);

                // Check all 6 faces
                // X- face (looking from -X toward +X, CCW from outside)
                if (!IsSolid(GetVoxel(x - 1, y, z)))
                {
                    EmitFace(
                        basePos + FVector(0, 0, 0),
                        basePos + FVector(0, Sy, 0),
                        basePos + FVector(0, Sy, Sz),
                        basePos + FVector(0, 0, Sz),
                        FVector(-1, 0, 0),
                        current,
                        EVoxelFaceDir::XNeg
                    );
                }

                // X+ face (looking from +X toward -X, CCW from outside)
                if (!IsSolid(GetVoxel(x + 1, y, z)))
                {
                    EmitFace(
                        basePos + FVector(Sx, 0, 0),
                        basePos + FVector(Sx, 0, Sz),
                        basePos + FVector(Sx, Sy, Sz),
                        basePos + FVector(Sx, Sy, 0),
                        FVector(1, 0, 0),
                        current,
                        EVoxelFaceDir::XPos
                    );
                }

                // Y- face (looking from -Y toward +Y, CCW from outside)
                if (!IsSolid(GetVoxel(x, y - 1, z)))
                {
                    EmitFace(
                        basePos + FVector(0, 0, 0),
                        basePos + FVector(0, 0, Sz),
                        basePos + FVector(Sx, 0, Sz),
                        basePos + FVector(Sx, 0, 0),
                        FVector(0, -1, 0),
                        current,
                        EVoxelFaceDir::YNeg
                    );
                }

                // Y+ face (looking from +Y toward -Y, CCW from outside)
                if (!IsSolid(GetVoxel(x, y + 1, z)))
                {
                    EmitFace(
                        basePos + FVector(0, Sy, 0),
                        basePos + FVector(Sx, Sy, 0),
                        basePos + FVector(Sx, Sy, Sz),
                        basePos + FVector(0, Sy, Sz),
                        FVector(0, 1, 0),
                        current,
                        EVoxelFaceDir::YPos
                    );
                }

                // Z- face (bottom, looking from -Z toward +Z, CCW from outside)
                if (!IsSolid(GetVoxel(x, y, z - 1)))
                {
                    EmitFace(
                        basePos + FVector(0, 0, 0),
                        basePos + FVector(Sx, 0, 0),
                        basePos + FVector(Sx, Sy, 0),
                        basePos + FVector(0, Sy, 0),
                        FVector(0, 0, -1),
                        current,
                        EVoxelFaceDir::ZNeg
                    );
                }

                // Z+ face (top, looking from +Z toward -Z, CCW from outside)
                if (!IsSolid(GetVoxel(x, y, z + 1)))
                {
                    EmitFace(
                        basePos + FVector(0, 0, Sz),
                        basePos + FVector(0, Sy, Sz),
                        basePos + FVector(Sx, Sy, Sz),
                        basePos + FVector(Sx, 0, Sz),
                        FVector(0, 0, 1),
                        current,
                        EVoxelFaceDir::ZPos
                    );
                }
            }
        }
    }

    const int32 bufferMemory =
        Out.Vertices.Num() * sizeof(FVector) +
        Out.Triangles.Num() * sizeof(int32) +
        Out.UVs.Num() * sizeof(FVector2D) +
        Out.Colors.Num() * sizeof(FColor) +
        Out.Normals.Num() * sizeof(FVector);
    INC_MEMORY_STAT_BY(STAT_VoxelMeshBufferMemory, bufferMemory);

    const int32 TotalVoxels = SX * SY * SZ;
    const float AirPercent = (float)AirCount / TotalVoxels * 100.0f;
    UE_LOG(LogTemp, Warning, TEXT("[NaiveMesher] Chunk %dx%dx%d: %d vertices, %d tris | Air: %d (%.1f%%), Solid: %d"),
        SX, SY, SZ, Out.Vertices.Num(), Out.Triangles.Num() / 3, AirCount, AirPercent, SolidCount);
}

// ============================================================================
// NOTE: BuildTrueBinaryGreedyMesh() is implemented in VoxelTrueBinaryMesher.cpp
// ============================================================================
// The true binary greedy mesher uses bitwise operations to process 64 voxels
// at once, achieving 10-50x speedup for face-finding compared to traditional
// per-voxel processing. See VoxelTrueBinaryMesher.cpp for implementation.