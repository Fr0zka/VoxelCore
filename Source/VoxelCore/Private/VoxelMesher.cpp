#include "VoxelMesher.h"
#include "ProceduralMeshComponent.h"
#include "GenericPlatform/GenericPlatformMath.h"
#if WITH_RUNTIME_MESHCOMPONENT
#include "RuntimeMeshComponent.h"
#endif
#include <VoxelGPUMesher.h>

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

static FORCEINLINE int32 Idx2D(int32 u, int32 v, int32 USize)
{
    return u + v * USize;
}

void BuildNeighborSolidMasks(
    const FChunkNeighbors* Neighbors,
    int32 SizeX, int32 SizeY, int32 SizeZ,
    FNeighborSolidMasks& MasksOut)
{
    MasksOut.Reset(SizeX, SizeY, SizeZ);
    if (!Neighbors) return;

    // X- / X+ faces (y,z)
    if (Neighbors->bHasXNeg && Neighbors->XNeg.Num() == SizeY * SizeZ)
    {
        MasksOut.bHasXNeg = true;
        for (int32 z = 0; z < SizeZ; ++z)
            for (int32 y = 0; y < SizeY; ++y)
            {
                const EVoxelBlockID id = Neighbors->XNeg[Idx2D(y, z, SizeY)];
                MasksOut.XNeg[Idx2D(y, z, SizeY)] = VoxelIsSolid(id);
            }
    }
    if (Neighbors->bHasXPos && Neighbors->XPos.Num() == SizeY * SizeZ)
    {
        MasksOut.bHasXPos = true;
        for (int32 z = 0; z < SizeZ; ++z)
            for (int32 y = 0; y < SizeY; ++y)
            {
                const EVoxelBlockID id = Neighbors->XPos[Idx2D(y, z, SizeY)];
                MasksOut.XPos[Idx2D(y, z, SizeY)] = VoxelIsSolid(id);
            }
    }

    // Y- / Y+ faces (x,z)
    if (Neighbors->bHasYNeg && Neighbors->YNeg.Num() == SizeX * SizeZ)
    {
        MasksOut.bHasYNeg = true;
        for (int32 z = 0; z < SizeZ; ++z)
            for (int32 x = 0; x < SizeX; ++x)
            {
                const EVoxelBlockID id = Neighbors->YNeg[Idx2D(x, z, SizeX)];
                MasksOut.YNeg[Idx2D(x, z, SizeX)] = VoxelIsSolid(id);
            }
    }
    if (Neighbors->bHasYPos && Neighbors->YPos.Num() == SizeX * SizeZ)
    {
        MasksOut.bHasYPos = true;
        for (int32 z = 0; z < SizeZ; ++z)
            for (int32 x = 0; x < SizeX; ++x)
            {
                const EVoxelBlockID id = Neighbors->YPos[Idx2D(x, z, SizeX)];
                MasksOut.YPos[Idx2D(x, z, SizeX)] = VoxelIsSolid(id);
            }
    }

    // Z- / Z+ faces (x,y)
    if (Neighbors->bHasZNeg && Neighbors->ZNeg.Num() == SizeX * SizeY)
    {
        MasksOut.bHasZNeg = true;
        for (int32 y = 0; y < SizeY; ++y)
            for (int32 x = 0; x < SizeX; ++x)
            {
                const EVoxelBlockID id = Neighbors->ZNeg[Idx2D(x, y, SizeX)];
                MasksOut.ZNeg[Idx2D(x, y, SizeX)] = VoxelIsSolid(id);
            }
    }
    if (Neighbors->bHasZPos && Neighbors->ZPos.Num() == SizeX * SizeY)
    {
        MasksOut.bHasZPos = true;
        for (int32 y = 0; y < SizeY; ++y)
            for (int32 x = 0; x < SizeX; ++x)
            {
                const EVoxelBlockID id = Neighbors->ZPos[Idx2D(x, y, SizeX)];
                MasksOut.ZPos[Idx2D(x, y, SizeX)] = VoxelIsSolid(id);
            }
    }
}


void UVoxelMesher::BuildGreedyMesh(
    const TArray<EVoxelBlockID>& Voxels,
    const FIntVector& Size,
    const FChunkNeighbors* Nbh,
    float VoxelUU,
    int32 XYScale,
    bool bUseAO,
    FMeshBuffers& Out)
{
    TRACE_CPUPROFILER_EVENT_SCOPE(Voxel_Mesher_GreedyCPU);
    SCOPE_CYCLE_COUNTER(STAT_Voxel_GreedyCPU);
    const int32 SX = Size.X, SY = Size.Y, SZ = Size.Z;
    Out.Vertices.Reset(); Out.Triangles.Reset(); Out.UVs.Reset(); Out.Colors.Reset(); Out.Normals.Reset();

    // Anisotropic world scale: XY scaled by XYScale; Z stays 1×
    const double ScaleX = (double)VoxelUU * (double)XYScale;
    const double ScaleY = (double)VoxelUU * (double)XYScale;
    const double ScaleZ = (double)VoxelUU;

    // Build compact 1-bit neighbor masks
    FNeighborSolidMasks Masks;
    BuildNeighborSolidMasks(Nbh, SX, SY, SZ, Masks);

    auto Idx3D = [&](int32 x, int32 y, int32 z) -> int32
        {
            return x + y * SX + z * (SX * SY);
        };

    auto Solid = [&](int32 x, int32 y, int32 z) -> bool
        {
            // Inside the chunk: direct lookup (EVoxelBlockID)
            if (x >= 0 && x < SX &&
                y >= 0 && y < SY &&
                z >= 0 && z < SZ)
            {
                const EVoxelBlockID id = Voxels[Idx3D(x, y, z)];
                return VoxelIsSolid(id);
            }

            // Outside: use 1-bit neighbor masks
            if (z >= 0 && z < SZ)
            {
                if (x < 0 && y >= 0 && y < SY && Masks.bHasXNeg)
                    return Masks.XNeg[y + z * SY];
                if (x >= SX && y >= 0 && y < SY && Masks.bHasXPos)
                    return Masks.XPos[y + z * SY];

                if (y < 0 && x >= 0 && x < SX && Masks.bHasYNeg)
                    return Masks.YNeg[x + z * SX];
                if (y >= SY && x >= 0 && x < SX && Masks.bHasYPos)
                    return Masks.YPos[x + z * SX];
            }

            if (x >= 0 && x < SX && y >= 0 && y < SY)
            {
                if (z < 0 && Masks.bHasZNeg)
                    return Masks.ZNeg[x + y * SX];
                if (z >= SZ && Masks.bHasZPos)
                    return Masks.ZPos[x + y * SX];
            }

            // If we don't have that neighbor face, treat as air
            return false;
        };

    auto Greyscale = [](float V) -> FLinearColor { return FLinearColor(V, V, V, 1.0f); };
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

    for (const FFaceData& Face : GFaceDefs)
    {
        const FIntVector N = Face.Normal, U = Face.TangentU, V = Face.TangentV;
        const int32 SliceCount = (N.X != 0) ? SX : (N.Y != 0) ? SY : SZ;
        const int32 DimU = (N.X != 0) ? SY : (N.Y != 0) ? SX : SX;
        const int32 DimV = (N.X != 0) ? SZ : (N.Y != 0) ? SZ : SY;

        auto MakeP = [&](int32 s, int32 u, int32 v)->FIntVector
            {
                if (N.X != 0) return FIntVector(s, u, v);
                if (N.Y != 0) return FIntVector(u, s, v);
                return FIntVector(u, v, s);
            };

        for (int32 slice = 0; slice < SliceCount; ++slice)
        {
            TArray<uint8> Mask; Mask.SetNumZeroed(DimU * DimV);

            for (int32 vv = 0; vv < DimV; ++vv)
                for (int32 uu = 0; uu < DimU; ++uu)
                {
                    const FIntVector P = MakeP(slice, uu, vv);
                    const FIntVector Q = P + N;
                    const bool A = Solid(P.X, P.Y, P.Z);
                    const bool B = Solid(Q.X, Q.Y, Q.Z);
                    Mask[Idx2D(uu, vv, DimU)] = (A && !B) ? 1 : 0;
                }

            int32 v = 0;
            while (v < DimV)
            {
                int32 u = 0;
                while (u < DimU)
                {
                    const int32 idx = Idx2D(u, v, DimU);
                    if (!Mask[idx]) { ++u; continue; }

                    int32 Width = 1;
                    while ((u + Width) < DimU && Mask[Idx2D(u + Width, v, DimU)]) ++Width;

                    int32 Height = 1;
                    bool Stop = false;
                    while ((v + Height) < DimV && !Stop)
                    {
                        for (int32 w = 0; w < Width; ++w)
                            if (!Mask[Idx2D(u + w, v + Height, DimU)]) { Stop = true; break; }
                        if (!Stop) ++Height;
                    }

                    for (int32 dv = 0; dv < Height; ++dv)
                        for (int32 du = 0; du < Width; ++du)
                            Mask[Idx2D(u + du, v + dv, DimU)] = 0;

                    const FIntVector Base = MakeP(slice, u, v);
                    const FIntVector Offset(FMath::Max(0, N.X), FMath::Max(0, N.Y), FMath::Max(0, N.Z));
                    const FIntVector FaceBase = Base + Offset;

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

                    const FVector FaceNormal = FVector(N);
                    Out.Normals.Add(FaceNormal); Out.Normals.Add(FaceNormal);
                    Out.Normals.Add(FaceNormal); Out.Normals.Add(FaceNormal);

                    const FVector Uf = FVector(U), Vf = FVector(V);
                    const float Sign = FVector::DotProduct(FVector::CrossProduct(Uf, Vf), FVector(N));
                    if (Sign < 0.f)
                    {
                        Out.Triangles.Add(VStart + 0); Out.Triangles.Add(VStart + 1); Out.Triangles.Add(VStart + 2);
                        Out.Triangles.Add(VStart + 0); Out.Triangles.Add(VStart + 2); Out.Triangles.Add(VStart + 3);
                    }
                    else
                    {
                        Out.Triangles.Add(VStart + 0); Out.Triangles.Add(VStart + 2); Out.Triangles.Add(VStart + 1);
                        Out.Triangles.Add(VStart + 0); Out.Triangles.Add(VStart + 3); Out.Triangles.Add(VStart + 2);
                    }

                    Out.UVs.Add(FVector2D(0, 0));
                    Out.UVs.Add(FVector2D((float)Width, 0));
                    Out.UVs.Add(FVector2D((float)Width, (float)Height));
                    Out.UVs.Add(FVector2D(0, (float)Height));

                    const FIntVector UNeg(-U.X, -U.Y, -U.Z), VNeg(-V.X, -V.Y, -V.Z);
                    const FIntVector AOBase = FaceBase;
                    Out.Colors.Add(Greyscale(SampleAO(AOBase, N, UNeg, VNeg)));
                    Out.Colors.Add(Greyscale(SampleAO(AOBase + U * Width, N, U, VNeg)));
                    Out.Colors.Add(Greyscale(SampleAO(AOBase + U * Width + V * Height, N, U, V)));
                    Out.Colors.Add(Greyscale(SampleAO(AOBase + V * Height, N, UNeg, V)));
                    u += Width;
                }
                ++v;
            }
        }
    }
}

void UVoxelMesher::BuildGreedyMesh_FaceMask(
    const TArray<EVoxelBlockID>& Voxels,
    const FIntVector& Size,
    const FChunkNeighbors* Nbh,
    float VoxelUU,
    int32 XYScale,
    bool bUseAO,
    FMeshBuffers& Out)
{
    TRACE_CPUPROFILER_EVENT_SCOPE(Voxel_Mesher_GreedyCPU);
    SCOPE_CYCLE_COUNTER(STAT_Voxel_GreedyCPU);

    const int32 SX = Size.X, SY = Size.Y, SZ = Size.Z;
    Out = FMeshBuffers{};
    if (SX <= 0 || SY <= 0 || SZ <= 0 || Voxels.Num() != SX * SY * SZ)
        return;

    // ---- Reserve generous bounds (shell faces) to reduce reallocs
    const int32 MaxFaces = 2 * (SX * SY + SX * SZ + SY * SZ);
    Out.Vertices.Reserve(MaxFaces * 4);
    Out.Normals.Reserve(MaxFaces * 4);
    Out.UVs.Reserve(MaxFaces * 4);
    Out.Colors.Reserve(MaxFaces * 4);
    Out.Triangles.Reserve(MaxFaces * 6);

    // ---- Anisotropic world scale (XY × XYScale, Z × 1)
    const double ScaleX = (double)VoxelUU * (double)XYScale;
    const double ScaleY = (double)VoxelUU * (double)XYScale;
    const double ScaleZ = (double)VoxelUU;

    // ---- Neighbor solid masks
    FNeighborSolidMasks Masks;
    BuildNeighborSolidMasks(Nbh, SX, SY, SZ, Masks);

    // ---- Solid bytes inside the chunk
    auto Idx3D = [&](int32 x, int32 y, int32 z) { return x + y * SX + z * SX * SY; };
    TArray<uint8> Solid; Solid.SetNumUninitialized(Voxels.Num());
    for (int32 i = 0; i < Voxels.Num(); ++i)
        Solid[i] = VoxelIsSolid(Voxels[i]) ? 1 : 0;

    // ---- Fast "solid" including neighbors
    auto SolidFast = [&](int32 x, int32 y, int32 z) -> bool
        {
            if (x >= 0 && x < SX && y >= 0 && y < SY && z >= 0 && z < SZ)
                return Solid[Idx3D(x, y, z)] != 0;

            if (z >= 0 && z < SZ)
            {
                if (x < 0 && y >= 0 && y < SY && Masks.bHasXNeg)  return Masks.XNeg[y + z * SY];
                if (x >= SX && y >= 0 && y < SY && Masks.bHasXPos)  return Masks.XPos[y + z * SY];

                if (y < 0 && x >= 0 && x < SX && Masks.bHasYNeg)  return Masks.YNeg[x + z * SX];
                if (y >= SY && x >= 0 && x < SX && Masks.bHasYPos)  return Masks.YPos[x + z * SX];
            }
            if (x >= 0 && x < SX && y >= 0 && y < SY)
            {
                if (z < 0 && Masks.bHasZNeg) return Masks.ZNeg[x + y * SX];
                if (z >= SZ && Masks.bHasZPos) return Masks.ZPos[x + y * SX];
            }
            return false;
        };

    // ---- AO (LUT)
    static const float AOlut[8] = { 1.f, 2.f / 3.f, 2.f / 3.f, 1.f / 3.f, 2.f / 3.f, 1.f / 3.f, 1.f / 3.f, 0.f };
    auto AO = [&](const FIntVector& P, const FIntVector& N, const FIntVector& U, const FIntVector& V) -> float
        {
            if (!bUseAO) return 1.0f;
            const int Sa = SolidFast((P + U).X, (P + U).Y, (P + U).Z) ? 1 : 0;
            const int Sb = SolidFast((P + V).X, (P + V).Y, (P + V).Z) ? 1 : 0;
            const int Sc = SolidFast((P + U + V).X, (P + U + V).Y, (P + U + V).Z) ? 1 : 0;
            return AOlut[Sa | (Sb << 1) | (Sc << 2)];
        };
    auto Grey = [](float V) { return FLinearColor(V, V, V, 1); };

    auto EmitQuad = [&](const FIntVector& FaceBase, const FIntVector& N, const FIntVector& U, const FIntVector& V,
        int32 W, int32 H)
        {
            const FVector WorldBase(FaceBase.X * ScaleX, FaceBase.Y * ScaleY, FaceBase.Z * ScaleZ);
            const FVector VecU(U.X * (double)W * ScaleX, U.Y * (double)W * ScaleY, U.Z * (double)W * ScaleZ);
            const FVector VecV(V.X * (double)H * ScaleX, V.Y * (double)H * ScaleY, V.Z * (double)H * ScaleZ);

            const int32 VStart = Out.Vertices.Num();
            Out.Vertices.Add(WorldBase);
            Out.Vertices.Add(WorldBase + VecU);
            Out.Vertices.Add(WorldBase + VecU + VecV);
            Out.Vertices.Add(WorldBase + VecV);

            const FVector FaceNormal(N);
            Out.Normals.Add(FaceNormal); Out.Normals.Add(FaceNormal);
            Out.Normals.Add(FaceNormal); Out.Normals.Add(FaceNormal);

            const float Sign = FVector::DotProduct(FVector::CrossProduct(FVector(U), FVector(V)), FVector(N));
            if (Sign < 0.f)
            {
                Out.Triangles.Add(VStart + 0); Out.Triangles.Add(VStart + 1); Out.Triangles.Add(VStart + 2);
                Out.Triangles.Add(VStart + 0); Out.Triangles.Add(VStart + 2); Out.Triangles.Add(VStart + 3);
            }
            else
            {
                Out.Triangles.Add(VStart + 0); Out.Triangles.Add(VStart + 2); Out.Triangles.Add(VStart + 1);
                Out.Triangles.Add(VStart + 0); Out.Triangles.Add(VStart + 3); Out.Triangles.Add(VStart + 2);
            }

            const FIntVector UNeg(-U.X, -U.Y, -U.Z), VNeg(-V.X, -V.Y, -V.Z);
            const FIntVector AOBase = FaceBase;
            Out.Colors.Add(Grey(AO(AOBase, N, UNeg, VNeg)));
            Out.Colors.Add(Grey(AO(AOBase + U * W, N, U, VNeg)));
            Out.Colors.Add(Grey(AO(AOBase + U * W + V * H, N, U, V)));
            Out.Colors.Add(Grey(AO(AOBase + V * H, N, UNeg, V)));
        };

    // ---- 64-bit pack/unpack helpers (support strided rows)
    auto Pack64_Strided = [](const uint8* Base, int32 Stride, int32 Count, int32 StartU) -> uint64
        {
            uint64 w = 0;
            const int32 MaxI = FMath::Min(64, Count - StartU);
            const uint8* p = Base + StartU * Stride;
            for (int32 i = 0; i < MaxI; ++i)
            {
                w |= (uint64)(p[0] ? 1u : 0u) << i;
                p += Stride;
            }
            return w;
        };
    auto Pack64_Contig = [](const uint8* Base, int32 Count, int32 StartU) -> uint64
        {
            uint64 w = 0;
            const int32 MaxI = FMath::Min(64, Count - StartU);
            const uint8* p = Base + StartU;
            for (int32 i = 0; i < MaxI; ++i) w |= (uint64)(p[i] ? 1u : 0u) << i;
            return w;
        };
    auto Unpack64_ToBytes = [](uint8* Row, int32 Count, int32 StartU, uint64 Word)
        {
            const int32 MaxI = FMath::Min(64, Count - StartU);
            for (int32 i = 0; i < MaxI; ++i) Row[StartU + i] = (uint8)((Word >> i) & 1ull);
        };

    // ---- Bit-pack helpers for TBitArray neighbor masks
    auto Pack64_Bits = [](const TBitArray<>& Bits, int32 StartAbs, int32 EndAbsExclusive) -> uint64
        {
            uint64 w = 0;
            const int32 MaxI = FMath::Min(64, EndAbsExclusive - StartAbs);
            for (int32 i = 0; i < MaxI; ++i)
                if (Bits[StartAbs + i]) w |= (1ull << i);
            return w;
        };
    auto Pack64_BitsRow = [&](const TBitArray<>& Bits, int32 RowOffset, int32 StartU, int32 RowLenU) -> uint64
        {
            const int32 StartAbs = RowOffset + StartU;
            const int32 EndAbsExclusive = RowOffset + RowLenU;
            return Pack64_Bits(Bits, StartAbs, EndAbsExclusive);
        };

    // ---- RLE greedy (row-runs)
    struct FRun { int32 U0, U1; }; // [U0,U1)

    auto Greedy2D_RLE = [&](uint8* Mask, int32 DimU, int32 DimV,
        const FIntVector& N, const FIntVector& U, const FIntVector& V,
        const TFunction<FIntVector(int32 slice, int32 uu, int32 vv)>& MakeBase,
        int32 Slice)
        {
            TArray<int32> H;  H.Init(0, DimU);
            TArray<FRun> Prev, Curr; Prev.Reserve(128); Curr.Reserve(128);

            for (int32 vv = 0; vv < DimV; ++vv)
            {
                // runs for this row
                Curr.Reset();
                uint8* R = Mask + vv * DimU;
                int32 u = 0;
                while (u < DimU)
                {
                    while (u < DimU && R[u] == 0) ++u;
                    if (u >= DimU) break;
                    int32 w = u;
                    while (w < DimU && R[w] != 0) ++w;
                    Curr.Add({ u,w });
                    u = w;
                }

                // extend heights in overlaps, flush non-overlaps
                int32 iA = 0, iB = 0;
                while (iA < Prev.Num() || iB < Curr.Num())
                {
                    const int32 a0 = (iA < Prev.Num()) ? Prev[iA].U0 : INT_MAX;
                    const int32 a1 = (iA < Prev.Num()) ? Prev[iA].U1 : INT_MAX;
                    const int32 b0 = (iB < Curr.Num()) ? Curr[iB].U0 : INT_MAX;
                    const int32 b1 = (iB < Curr.Num()) ? Curr[iB].U1 : INT_MAX;

                    if (a0 < b0)
                    {
                        const int32 e = FMath::Min(a1, b0);
                        for (int32 uu = a0; uu < e; ++uu)
                        {
                            const int32 hh = H[uu];
                            if (hh > 0)
                            {
                                const FIntVector Base = MakeBase(Slice, uu, vv - hh);
                                const FIntVector Off(FMath::Max(0, N.X), FMath::Max(0, N.Y), FMath::Max(0, N.Z));
                                EmitQuad(Base + Off, N, U, V, 1, hh);
                                H[uu] = 0;
                            }
                        }
                        if (a1 <= b0) { ++iA; continue; }
                    }

                    if (a0 != INT_MAX && b0 != INT_MAX)
                    {
                        const int32 u0 = FMath::Max(a0, b0);
                        const int32 u1 = FMath::Min(a1, b1);
                        if (u0 < u1) for (int32 uu = u0; uu < u1; ++uu) H[uu] += 1;
                    }

                    if (a1 <= b1) ++iA;
                    if (b1 <= a1) ++iB;
                }

                // columns that started this row
                for (const FRun& r : Curr)
                    for (int32 uu = r.U0; uu < r.U1; ++uu)
                        if (H[uu] == 0) H[uu] = 1;

                // coalesce equal heights into wide quads
                int32 uu = 0;
                while (uu < DimU)
                {
                    while (uu < DimU && H[uu] == 0) ++uu;
                    if (uu >= DimU) break;
                    const int32 hh = H[uu];
                    int32 w = uu + 1;
                    while (w < DimU && H[w] == hh) ++w;

                    const FIntVector Base = MakeBase(Slice, uu, vv - hh + 1);
                    const FIntVector Off(FMath::Max(0, N.X), FMath::Max(0, N.Y), FMath::Max(0, N.Z));
                    EmitQuad(Base + Off, N, U, V, w - uu, hh);
                    for (int32 k = uu; k < w; ++k) H[k] = 0;
                    uu = w;
                }

                Prev = MoveTemp(Curr);
            }

            // flush remaining columns
            for (int32 uu = 0; uu < DimU; ++uu)
            {
                const int32 hh = H[uu];
                if (hh > 0)
                {
                    const FIntVector Base = MakeBase(Slice, uu, DimV - hh);
                    const FIntVector Off(FMath::Max(0, N.X), FMath::Max(0, N.Y), FMath::Max(0, N.Z));
                    EmitQuad(Base + Off, N, U, V, 1, hh);
                }
            }
        };

    // Reused 2D byte mask
    TArray<uint8> Mask;

    // ========================== per-axis, per-slice ==========================

    // +X : mask(u=y, v=z) = S(x,y,z) & !S(x+1,y,z)
    {
        const int32 DimU = SY, DimV = SZ;
        for (int32 x = 0; x < SX; ++x)
        {
            Mask.SetNumZeroed(DimU * DimV);

            for (int32 z = 0; z < SZ; ++z)
            {
                // rows are along y, stride = SX
                const uint8* ARow = &Solid[Idx3D(x, 0, z)];
                const uint8* BRow = (x + 1 < SX) ? &Solid[Idx3D(x + 1, 0, z)] : nullptr;

                uint8* MRow = Mask.GetData() + z * DimU;
                for (int32 u = 0; u < DimU; u += 64)
                {
                    const uint64 A = Pack64_Strided(ARow, SX, DimU, u);
                    uint64 B = 0ull;
                    if (BRow)
                    {
                        B = Pack64_Strided(BRow, SX, DimU, u);
                    }
                    else if (Masks.bHasXPos)
                    {
                        const int32 rowOffset = z * SY;
                        B = Pack64_BitsRow(Masks.XPos, rowOffset, u, DimU);
                    }
                    Unpack64_ToBytes(MRow, DimU, u, A & ~B);
                }
            }

            const FIntVector N(+1, 0, 0), U(0, +1, 0), V(0, 0, +1);
            auto MakeBase = [&](int32 slice, int32 u, int32 v) { return FIntVector(slice, u, v); };
            Greedy2D_RLE(Mask.GetData(), DimU, DimV, N, U, V, MakeBase, x);
        }
    }

    // -X : mask(u=y, v=z) = S(x,y,z) & !S(x-1,y,z)
    {
        const int32 DimU = SY, DimV = SZ;
        for (int32 x = 0; x < SX; ++x)
        {
            Mask.SetNumZeroed(DimU * DimV);

            for (int32 z = 0; z < SZ; ++z)
            {
                const uint8* ARow = &Solid[Idx3D(x, 0, z)];
                const uint8* BRow = (x - 1 >= 0) ? &Solid[Idx3D(x - 1, 0, z)] : nullptr;

                uint8* MRow = Mask.GetData() + z * DimU;
                for (int32 u = 0; u < DimU; u += 64)
                {
                    const uint64 A = Pack64_Strided(ARow, SX, DimU, u);
                    uint64 B = 0ull;
                    if (BRow)
                    {
                        B = Pack64_Strided(BRow, SX, DimU, u);
                    }
                    else if (Masks.bHasXNeg)
                    {
                        const int32 rowOffset = z * SY;
                        B = Pack64_BitsRow(Masks.XNeg, rowOffset, u, DimU);
                    }
                    Unpack64_ToBytes(MRow, DimU, u, A & ~B);
                }
            }

            const FIntVector N(-1, 0, 0), U(0, +1, 0), V(0, 0, +1);
            auto MakeBase = [&](int32 slice, int32 u, int32 v) { return FIntVector(slice, u, v); };
            Greedy2D_RLE(Mask.GetData(), DimU, DimV, N, U, V, MakeBase, x);
        }
    }

    // +Y : mask(u=x, v=z) = S(x,y,z) & !S(x,y+1,z)
    {
        const int32 DimU = SX, DimV = SZ;
        for (int32 y = 0; y < SY; ++y)
        {
            Mask.SetNumZeroed(DimU * DimV);

            for (int32 z = 0; z < SZ; ++z)
            {
                // rows are along x, stride = 1
                const uint8* ARow = &Solid[Idx3D(0, y, z)];
                const uint8* BRow = (y + 1 < SY) ? &Solid[Idx3D(0, y + 1, z)] : nullptr;

                uint8* MRow = Mask.GetData() + z * DimU;
                for (int32 u = 0; u < DimU; u += 64)
                {
                    const uint64 A = Pack64_Contig(ARow, DimU, u);
                    uint64 B = 0ull;
                    if (BRow)
                    {
                        B = Pack64_Contig(BRow, DimU, u);
                    }
                    else if (Masks.bHasYPos)
                    {
                        const int32 rowOffset = z * SX;
                        B = Pack64_BitsRow(Masks.YPos, rowOffset, u, DimU);
                    }
                    Unpack64_ToBytes(MRow, DimU, u, A & ~B);
                }
            }

            const FIntVector N(0, +1, 0), U(+1, 0, 0), V(0, 0, +1);
            auto MakeBase = [&](int32 slice, int32 u, int32 v) { return FIntVector(u, slice, v); };
            Greedy2D_RLE(Mask.GetData(), DimU, DimV, N, U, V, MakeBase, y);
        }
    }

    // -Y : mask(u=x, v=z) = S(x,y,z) & !S(x,y-1,z)
    {
        const int32 DimU = SX, DimV = SZ;
        for (int32 y = 0; y < SY; ++y)
        {
            Mask.SetNumZeroed(DimU * DimV);

            for (int32 z = 0; z < SZ; ++z)
            {
                const uint8* ARow = &Solid[Idx3D(0, y, z)];
                const uint8* BRow = (y - 1 >= 0) ? &Solid[Idx3D(0, y - 1, z)] : nullptr;

                uint8* MRow = Mask.GetData() + z * DimU;
                for (int32 u = 0; u < DimU; u += 64)
                {
                    const uint64 A = Pack64_Contig(ARow, DimU, u);
                    uint64 B = 0ull;
                    if (BRow)
                    {
                        B = Pack64_Contig(BRow, DimU, u);
                    }
                    else if (Masks.bHasYNeg)
                    {
                        const int32 rowOffset = z * SX;
                        B = Pack64_BitsRow(Masks.YNeg, rowOffset, u, DimU);
                    }
                    Unpack64_ToBytes(MRow, DimU, u, A & ~B);
                }
            }

            const FIntVector N(0, -1, 0), U(+1, 0, 0), V(0, 0, +1);
            auto MakeBase = [&](int32 slice, int32 u, int32 v) { return FIntVector(u, slice, v); };
            Greedy2D_RLE(Mask.GetData(), DimU, DimV, N, U, V, MakeBase, y);
        }
    }

    // +Z : mask(u=x, v=y) = S(x,y,z) & !S(x,y,z+1)
    {
        const int32 DimU = SX, DimV = SY;
        for (int32 z = 0; z < SZ; ++z)
        {
            Mask.SetNumZeroed(DimU * DimV);

            for (int32 y = 0; y < SY; ++y)
            {
                // rows are along x, stride = 1
                const uint8* ARow = &Solid[Idx3D(0, y, z)];
                const uint8* BRow = (z + 1 < SZ) ? &Solid[Idx3D(0, y, z + 1)] : nullptr;

                uint8* MRow = Mask.GetData() + y * DimU; // v=y
                for (int32 u = 0; u < DimU; u += 64)
                {
                    const uint64 A = Pack64_Contig(ARow, DimU, u);
                    uint64 B = 0ull;
                    if (BRow)
                    {
                        B = Pack64_Contig(BRow, DimU, u);
                    }
                    else if (Masks.bHasZPos)
                    {
                        const int32 rowOffset = y * SX;
                        B = Pack64_BitsRow(Masks.ZPos, rowOffset, u, DimU);
                    }
                    Unpack64_ToBytes(MRow, DimU, u, A & ~B);
                }
            }

            const FIntVector N(0, 0, +1), U(+1, 0, 0), V(0, +1, 0);
            auto MakeBase = [&](int32 slice, int32 u, int32 v) { return FIntVector(u, v, slice); };
            Greedy2D_RLE(Mask.GetData(), DimU, DimV, N, U, V, MakeBase, z);
        }
    }

    // -Z : mask(u=x, v=y) = S(x,y,z) & !S(x,y,z-1)
    {
        const int32 DimU = SX, DimV = SY;
        for (int32 z = 0; z < SZ; ++z)
        {
            Mask.SetNumZeroed(DimU * DimV);

            for (int32 y = 0; y < SY; ++y)
            {
                const uint8* ARow = &Solid[Idx3D(0, y, z)];
                const uint8* BRow = (z - 1 >= 0) ? &Solid[Idx3D(0, y, z - 1)] : nullptr;

                uint8* MRow = Mask.GetData() + y * DimU; // v=y
                for (int32 u = 0; u < DimU; u += 64)
                {
                    const uint64 A = Pack64_Contig(ARow, DimU, u);
                    uint64 B = 0ull;
                    if (BRow)
                    {
                        B = Pack64_Contig(BRow, DimU, u);
                    }
                    else if (Masks.bHasZNeg)
                    {
                        const int32 rowOffset = y * SX;
                        B = Pack64_BitsRow(Masks.ZNeg, rowOffset, u, DimU);
                    }
                    Unpack64_ToBytes(MRow, DimU, u, A & ~B);
                }
            }

            const FIntVector N(0, 0, -1), U(+1, 0, 0), V(0, +1, 0);
            auto MakeBase = [&](int32 slice, int32 u, int32 v) { return FIntVector(u, v, slice); };
            Greedy2D_RLE(Mask.GetData(), DimU, DimV, N, U, V, MakeBase, z);
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
    // Build non-uniform sample positions so the last cell exactly reaches the chunk edge
    TArray<double> Xpos, Ypos;
    Xpos.SetNumUninitialized(SamplesX);
    Ypos.SetNumUninitialized(SamplesY);

    // Base LOD0->world scale per voxel
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
            return (double)Heights[cx + cy * SamplesX] * UU; // Z uses base UU (no XYScale)
        };

    Out.Vertices.Reset(); Out.Triangles.Reset(); Out.UVs.Reset(); Out.Colors.Reset(); Out.Normals.Reset();

    auto Greyscale = [](float V) -> FLinearColor { return FLinearColor(V, V, V, 1.0f); };

    // Quads: (SamplesX-1) * (SamplesY-1) – covers full chunk width, no gap
    for (int32 y = 0; y < SamplesY - 1; ++y)
    {
        for (int32 x = 0; x < SamplesX - 1; ++x)
        {
            const FVector P00(Xpos[x], Ypos[y], H(x, y));
            const FVector P10(Xpos[x + 1], Ypos[y], H(x + 1, y));
            const FVector P01(Xpos[x], Ypos[y + 1], H(x, y + 1));
            const FVector P11(Xpos[x + 1], Ypos[y + 1], H(x + 1, y + 1));

            const int32 VStart = Out.Vertices.Num();
            Out.Vertices.Add(P00); // 0
            Out.Vertices.Add(P10); // 1
            Out.Vertices.Add(P11); // 2
            Out.Vertices.Add(P01); // 3

            const FVector N = FVector::CrossProduct(P10 - P00, P01 - P00).GetSafeNormal();
            Out.Normals.Add(N); Out.Normals.Add(N); Out.Normals.Add(N); Out.Normals.Add(N);

            Out.UVs.Add(FVector2D(0, 0));
            Out.UVs.Add(FVector2D(1, 0));
            Out.UVs.Add(FVector2D(1, 1));
            Out.UVs.Add(FVector2D(0, 1));

            Out.Colors.Add(Greyscale(1.0f));
            Out.Colors.Add(Greyscale(1.0f));
            Out.Colors.Add(Greyscale(1.0f));
            Out.Colors.Add(Greyscale(1.0f));

            // Same outward winding as voxel quads
            Out.Triangles.Add(VStart + 0);
            Out.Triangles.Add(VStart + 2);
            Out.Triangles.Add(VStart + 1);

            Out.Triangles.Add(VStart + 0);
            Out.Triangles.Add(VStart + 3);
            Out.Triangles.Add(VStart + 2);
        }
    }
}

void UVoxelMesher::BuildHeightfieldMesh_Grid(
    const TArray<int32>& Heights,
    int32 SamplesX,
    int32 SamplesY,
    int32 ChunkSizeX,
    int32 ChunkSizeY,
    int32 XYScale,
    float VoxelUU,
    const TArray<int32>& SharedIB,
    FMeshBuffers& Out)
{
    TRACE_CPUPROFILER_EVENT_SCOPE(Voxel_Mesher_HeightfieldGrid);

    if (SamplesX < 2 || SamplesY < 2) { Out = FMeshBuffers{}; return; }
    if (Heights.Num() != SamplesX * SamplesY) { Out = FMeshBuffers{}; return; }

    // Precompute non-uniform XY to hit exact chunk edges
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

    const int32 VCount = SamplesX * SamplesY;
    Out.Vertices.SetNumUninitialized(VCount);
    Out.Normals.SetNumUninitialized(VCount);
    Out.UVs.SetNumUninitialized(VCount);
    Out.Colors.SetNumUninitialized(VCount);

    // Positions, UVs
    for (int32 y = 0; y < SamplesY; ++y)
    {
        for (int32 x = 0; x < SamplesX; ++x)
        {
            const int32 idx = x + y * SamplesX;
            Out.Vertices[idx] = FVector((float)Xpos[x], (float)Ypos[y], (float)H(x, y));
            Out.UVs[idx] = FVector2D(
                (SamplesX > 1) ? (float)x / (float)(SamplesX - 1) : 0.0f,
                (SamplesY > 1) ? (float)y / (float)(SamplesY - 1) : 0.0f);
            Out.Colors[idx] = FLinearColor::White;
        }
    }

    // Vertex normals from central differences
    for (int32 y = 0; y < SamplesY; ++y)
    {
        const int32 ym = FMath::Max(0, y - 1);
        const int32 yp = FMath::Min(SamplesY - 1, y + 1);
        for (int32 x = 0; x < SamplesX; ++x)
        {
            const int32 xm = FMath::Max(0, x - 1);
            const int32 xp = FMath::Min(SamplesX - 1, x + 1);

            const FVector PL = FVector((float)Xpos[xm], (float)Ypos[y], (float)H(xm, y));
            const FVector PR = FVector((float)Xpos[xp], (float)Ypos[y], (float)H(xp, y));
            const FVector PD = FVector((float)Xpos[x], (float)Ypos[ym], (float)H(x, ym));
            const FVector PU = FVector((float)Xpos[x], (float)Ypos[yp], (float)H(x, yp));

            const FVector Dx = PR - PL;
            const FVector Dy = PU - PD;
            FVector N = FVector::CrossProduct(Dx, Dy).GetSafeNormal();
            if (!N.IsNormalized()) N = FVector(0, 0, 1);
            Out.Normals[x + y * SamplesX] = N;
        }
    }

    // Reuse shared topology
    Out.Triangles = SharedIB; // identical every frame for a given SamplesX×SamplesY
}

void UVoxelMesher::ApplyToPMC(UProceduralMeshComponent* PMC,
    const FMeshBuffers& Bufs,
    bool bCreateCollision)
{
    if (!PMC) return;

    // Recreate the section (this implicitly rebuilds BodySetup if collision is enabled)
    PMC->ClearAllMeshSections();

    PMC->CreateMeshSection_LinearColor(
        /*SectionIndex*/ 0,
        Bufs.Vertices,
        Bufs.Triangles,
        Bufs.Normals,
        Bufs.UVs,
        Bufs.Colors,
        TArray<FProcMeshTangent>{},   // tangents
        bCreateCollision               // <- pass the bool (do NOT assign)
    );

    PMC->SetCollisionEnabled(
        bCreateCollision
        ? ECollisionEnabled::QueryAndPhysics
        : ECollisionEnabled::NoCollision);
}

#if WITH_RUNTIME_MESHCOMPONENT
void UVoxelMesher::ApplyToRMC(
    URuntimeMeshComponent* RMC,
    const FMeshBuffers& Bufs,
    bool bCreateCollision)
{
    if (!RMC) return;

    // Best-effort equivalents; tweak if your RMC version differs
    RMC->ClearAllMeshSections();

    RMC->CreateMeshSection(
        0,
        Bufs.Vertices,
        Bufs.Triangles,
        Bufs.Normals,
        Bufs.UVs,
        Bufs.Colors,
        TArray<FRuntimeMeshTangent>(),
        bCreateCollision);

    RMC->SetCollisionEnabled(bCreateCollision ? ECollisionEnabled::QueryAndPhysics
        : ECollisionEnabled::NoCollision);
    RMC->SetCollisionUseComplexAsSimple(bCreateCollision);
# if WITH_PHYSX || WITH_CHAOS
    RMC->SetUseAsyncCooking(false);
# endif
}
#endif
bool UVoxelMesher::BuildGreedyMesh_GPU(const TArray<EVoxelBlockID>& Voxels,
    FIntVector Size,
    int32 XYScale,
    float VoxelUU,
    FMeshBuffers& Out)
{
    FGPUMeshBuildParams P;
    P.Voxels = reinterpret_cast<const uint8*>(Voxels.GetData());
    P.SizeX = Size.X;
    P.SizeY = Size.Y;
    P.SizeZ = Size.Z;
    P.XYScale = FMath::Max(1, XYScale);
    P.DefaultAO = 3;
    P.VoxelUU = VoxelUU;

    TArray<uint32> Packed;
    if (!FVoxelGPUMesher::BuildPackedVerts_GPU(P, Packed))
    {
        UE_LOG(LogTemp, Display, TEXT("[VoxelCore] GPU pas dispo"), P.SizeX, P.SizeY, P.SizeZ);

        return false; // GPU path not available or failed — caller should fall back to CPU builder
    }

    // TEMP: decode packed verts back to your standard mesh buffers so you can keep using PMC/RMC
    //FVoxelGPUMesher::DecodePackedVertsToMeshBuffers(Packed, Out, VoxelUU);
    FVoxelGPUMesher::DecodePackedVertsToMeshBuffers(Packed, Out, VoxelUU, XYScale);
    UE_LOG(LogTemp, Display, TEXT("[VoxelCore] GPU mesher invoked (%dx%dx%d)"), P.SizeX, P.SizeY, P.SizeZ);

    return true;
}
// ---------- ProceduralMeshComponent path ----------
void UVoxelMesher::ApplyToPMC_Create(
    UProceduralMeshComponent* PMC, int32 SectionIndex,
    const FMeshBuffers& B, bool bCreateCollision)
{
    check(PMC);
    // ProceduralMesh wants tangents as FProcMeshTangent if provided; we omit for speed
    static const TArray<FVector2D> EmptyUV2;
    static const TArray<FProcMeshTangent> EmptyTangents;

    PMC->CreateMeshSection_LinearColor(
        SectionIndex,
        B.Vertices,
        B.Triangles,
        B.Normals,
        B.UVs,
        B.Colors,
        EmptyTangents,
        bCreateCollision);

    PMC->SetMeshSectionVisible(SectionIndex, true);
}

void UVoxelMesher::ApplyToPMC_Update(
    UProceduralMeshComponent* PMC, int32 SectionIndex,
    const FMeshBuffers& B)
{
    check(PMC);
    static const TArray<FVector2D> EmptyUV2;
    static const TArray<FProcMeshTangent> EmptyTangents;

    PMC->UpdateMeshSection_LinearColor(
        SectionIndex,
        B.Vertices,
        B.Normals,
        B.UVs,
        B.Colors,
        EmptyTangents);
}

// ---------- RuntimeMeshComponent path ----------
#if WITH_RUNTIME_MESHCOMPONENT
void UVoxelMesher::ApplyToRMC_Create(
    URuntimeMeshComponent* RMC, int32 SectionIndex,
    const FMeshBuffers& B, bool bCreateCollision)
{
    check(RMC);

    // Build a RMC section from components. Choose a simple material slot 0.
    RMC->CreateSectionFromComponents(
        0, SectionIndex,
        B.Vertices, B.Triangles, B.Normals, B.UVs, B.Colors,
        /*Tangents*/ TArray<FRuntimeMeshTangent>(),
        /*bCreateCollision*/ bCreateCollision,
        /*EUpdateFrequency*/ ERuntimeMeshUpdateFrequency::Frequent);

    RMC->SetSectionVisible(0, SectionIndex, true);
}

void UVoxelMesher::ApplyToRMC_Update(
    URuntimeMeshComponent* RMC, int32 SectionIndex,
    const FMeshBuffers& B)
{
    check(RMC);

    RMC->UpdateSectionFromComponents(
        0, SectionIndex,
        B.Vertices, B.Normals, B.UVs, B.Colors,
        /*Tangents*/ TArray<FRuntimeMeshTangent>());
}
#endif


// VoxelMesher.cpp
void UVoxelMesher::BuildGreedyMesh_FastLOD1(
    const TArray<EVoxelBlockID>& V,
    const FIntVector& S,
    const FChunkNeighbors* /*Nbh*/,
    float UU,
    int32 XYScale,
    FMeshBuffers& Out)
{
    TRACE_CPUPROFILER_EVENT_SCOPE(Voxel_GreedyMesh_FastLOD1);
    SCOPE_CYCLE_COUNTER(STAT_Voxel_GreedyMesh_FastLOD1);

    // reset output (no Out.Clear(); construct-assign instead)
    Out = FMeshBuffers{};

    if (S.X <= 0 || S.Y <= 0 || S.Z <= 0) return;
    const int32 Count = S.X * S.Y * S.Z;
    if (V.Num() != Count) return;

    auto Solid = [&](int32 x, int32 y, int32 z)->bool {
        return ((uint32)x < (uint32)S.X) & ((uint32)y < (uint32)S.Y) & ((uint32)z < (uint32)S.Z)
            ? (uint16)V[x + y * S.X + z * S.X * S.Y] != 0
            : false; // out of bounds -> empty; fast LOD1 ignores neighbors
        };

    struct Quad { int32 x, y, z, w, h; uint8 dir; };
    TArray<Quad> Quads; Quads.Reserve(S.X * S.Y + S.Y * S.Z + S.Z * S.X);

    auto MergeLayer = [&](uint8 dir, int32 U, int32 Vdim, int32 Wdim,
        auto atSolid, auto atOpposite)
        {
            TArray<uint8> mask; mask.SetNumZeroed(Vdim * Wdim);

            for (int32 u = 0; u < U; ++u)
            {
                // build face mask for this slice
                for (int32 w = 0; w < Wdim; ++w)
                    for (int32 v = 0; v < Vdim; ++v)
                    {
                        const bool a = atSolid(u, v, w);
                        const bool b = atOpposite(u, v, w);
                        // emit only if one side solid and the other empty
                        mask[v + w * Vdim] = (a ^ b) ? (a ? 1 : 2) : 0; // 1=+dir, 2=-dir
                    }

                // greedy pack rectangles
                int32 i = 0;
                while (i < Vdim * Wdim)
                {
                    if (mask[i] == 0) { ++i; continue; }
                    const uint8 val = mask[i];
                    const int32 v0 = i % Vdim;
                    const int32 w0 = i / Vdim;

                    int32 width = 1;
                    while (v0 + width < Vdim && mask[i + width] == val) ++width;

                    int32 height = 1; bool extend = true;
                    while (w0 + height < Wdim && extend)
                    {
                        for (int32 k = 0; k < width; ++k)
                            if (mask[i + k + height * Vdim] != val) { extend = false; break; }
                        if (extend) ++height;
                    }

                    // clear mask
                    for (int32 hh = 0; hh < height; ++hh)
                        FMemory::Memset(mask.GetData() + (i + hh * Vdim), 0, width);

                    Quad q; q.dir = (val == 1) ? dir : (dir ^ 1);
                    if (dir < 2) { q.x = u; q.y = v0; q.z = w0; q.w = width; q.h = height; }          // ±X
                    else if (dir < 4) { q.x = v0; q.y = u; q.z = w0; q.w = width; q.h = height; }     // ±Y
                    else { q.x = v0; q.y = w0; q.z = u; q.w = width; q.h = height; }                // ±Z
                    Quads.Add(q);
                }
            }
        };

    // ±X
    MergeLayer(/*+X*/0, S.X, S.Y, S.Z,
        [&](int32 u, int32 v, int32 w) { return Solid(u, v, w); },
        [&](int32 u, int32 v, int32 w) { return Solid(u + 1, v, w); });
    // ±Y
    MergeLayer(/*+Y*/2, S.Y, S.X, S.Z,
        [&](int32 u, int32 v, int32 w) { return Solid(v, u, w); },
        [&](int32 u, int32 v, int32 w) { return Solid(v, u + 1, w); });
    // ±Z
    MergeLayer(/*+Z*/4, S.Z, S.X, S.Y,
        [&](int32 u, int32 v, int32 w) { return Solid(v, w, u); },
        [&](int32 u, int32 v, int32 w) { return Solid(v, w, u + 1); });

    const int32 reserveVerts = Quads.Num() * 4;
    const int32 reserveIdx = Quads.Num() * 6;
    Out.Vertices.Reserve(reserveVerts);
    Out.Normals.Reserve(reserveVerts);
    Out.UVs.Reserve(reserveVerts);
    Out.Triangles.Reserve(reserveIdx);

    auto pushQuad = [&](const Quad& q)
        {
            const float sx = UU * float(XYScale);
            const float sy = UU * float(XYScale);
            const float sz = UU;

            FVector o, ux, vy, n;
            switch (q.dir)
            {
            case 0: o = FVector((q.x + 1) * sx, q.y * sy, q.z * sz); ux = FVector(0, q.w * sy, 0); vy = FVector(0, 0, q.h * sz); n = FVector(+1, 0, 0); break;
            case 1: o = FVector(q.x * sx, (q.y + q.w) * sy, q.z * sz); ux = FVector(0, -q.w * sy, 0); vy = FVector(0, 0, q.h * sz); n = FVector(-1, 0, 0); break;
            case 2: o = FVector(q.x * sx, (q.y + 1) * sy, q.z * sz); ux = FVector(q.w * sx, 0, 0); vy = FVector(0, 0, q.h * sz); n = FVector(0, +1, 0); break;
            case 3: o = FVector((q.x + q.w) * sx, q.y * sy, q.z * sz); ux = FVector(-q.w * sx, 0, 0); vy = FVector(0, 0, q.h * sz); n = FVector(0, -1, 0); break;
            case 4: o = FVector(q.x * sx, q.y * sy, (q.z + 1) * sz); ux = FVector(q.w * sx, 0, 0); vy = FVector(0, q.h * sy, 0); n = FVector(0, 0, +1); break;
            default:o = FVector(q.x * sx, (q.y + q.h) * sy, q.z * sz);   ux = FVector(q.w * sx, 0, 0); vy = FVector(0, -q.h * sy, 0); n = FVector(0, 0, -1); break;
            }

            const int32 i0 = Out.Vertices.Num();
            Out.Vertices.Add(o);
            Out.Vertices.Add(o + ux);
            Out.Vertices.Add(o + ux + vy);
            Out.Vertices.Add(o + vy);

            Out.Normals.Add(n); Out.Normals.Add(n); Out.Normals.Add(n); Out.Normals.Add(n);
            Out.UVs.Add(FVector2D(0, 0)); Out.UVs.Add(FVector2D(1, 0)); Out.UVs.Add(FVector2D(1, 1)); Out.UVs.Add(FVector2D(0, 1));

            Out.Triangles.Add(i0 + 0); Out.Triangles.Add(i0 + 2); Out.Triangles.Add(i0 + 1);
            Out.Triangles.Add(i0 + 0); Out.Triangles.Add(i0 + 3); Out.Triangles.Add(i0 + 2);
        };

    for (const Quad& q : Quads) pushQuad(q);
}
namespace
{
    FORCEINLINE int32 VoxIndex3D(int32 x, int32 y, int32 z, int32 SX, int32 SY)
    {
        return x + y * SX + z * SX * SY;
    }

    // rank1 = number of set bits in [0..idx)
    FORCEINLINE int32 Rank1(const TArray<uint64>& Bits, int32 idx)
    {
        const int32 w = idx >> 6;
        const int32 b = idx & 63;

        int32 r = 0;
        for (int32 i = 0; i < w; ++i) r += FMath::CountBits(Bits[i]);
        if (b) { const uint64 m = (b == 64) ? ~0ULL : ((1ULL << b) - 1ULL); r += FMath::CountBits(Bits[w] & m); }
        return r;
    }
    FORCEINLINE bool BitGet(const TArray<uint64>& Bits, int32 bitIndex)
    {
        const int32 w = bitIndex >> 6;
        const int32 b = bitIndex & 63;
        return ((uint32)w < (uint32)Bits.Num()) ? ((Bits[w] >> b) & 1ULL) != 0 : false;
    }
}

void UVoxelMesher::BuildGreedyMesh_FromBitset(
    const FCompactVoxelData& C,
    float VoxelUU,
    FMeshBuffers& Out)
{
    TRACE_CPUPROFILER_EVENT_SCOPE(Voxel_Mesher_FastLOD1_Bitset);

    Out = FMeshBuffers{};
    const int32 SX = C.SizeX, SY = C.SizeY, SZ = C.SizeZ;
    if (SX <= 0 || SY <= 0 || SZ <= 0) return;

    auto Solid = [&](int32 x, int32 y, int32 z)->bool
        {
            if ((uint32)x >= (uint32)SX || (uint32)y >= (uint32)SY || (uint32)z >= (uint32)SZ) return false;
            const int32 i = VoxIndex3D(x, y, z, SX, SY);
            return BitGet(C.Occupancy, i);
        };

    struct Quad { int32 x, y, z, w, h; uint8 dir; };
    TArray<Quad> Q; Q.Reserve(SX * SY + SY * SZ + SZ * SX);

    auto MergeLayer = [&](uint8 dir, int32 U, int32 Vd, int32 Wd,
        auto atSolid, auto atOpp)
        {
            TArray<uint8> mask; mask.SetNumZeroed(Vd * Wd);

            for (int32 u = 0; u < U; ++u)
            {
                // build face mask for slice u
                for (int32 w = 0; w < Wd; ++w)
                    for (int32 v = 0; v < Vd; ++v)
                    {
                        const bool a = atSolid(u, v, w);
                        const bool b = atOpp(u, v, w);
                        mask[v + w * Vd] = (a ^ b) ? (a ? 1 : 2) : 0;
                    }

                int32 i = 0;
                while (i < Vd * Wd)
                {
                    if (mask[i] == 0) { ++i; continue; }
                    const uint8 val = mask[i];
                    const int32 v0 = i % Vd;
                    const int32 w0 = i / Vd;

                    int32 width = 1;  while (v0 + width < Vd && mask[i + width] == val) ++width;
                    int32 height = 1; bool ok = true;
                    while (w0 + height < Wd && ok)
                    {
                        for (int32 k = 0; k < width; ++k)
                            if (mask[i + k + height * Vd] != val) { ok = false; break; }
                        if (ok) ++height;
                    }
                    for (int32 hh = 0; hh < height; ++hh)
                        FMemory::Memset(mask.GetData() + (i + hh * Vd), 0, width);

                    Quad q; q.dir = (val == 1) ? dir : (dir ^ 1);
                    if (dir < 2) { q.x = u; q.y = v0; q.z = w0; q.w = width; q.h = height; }       // ±X
                    else if (dir < 4) { q.x = v0; q.y = u; q.z = w0; q.w = width; q.h = height; }  // ±Y
                    else { q.x = v0; q.y = w0; q.z = u; q.w = width; q.h = height; }  // ±Z
                    Q.Add(q);
                }
            }
        };

    // ±X
    MergeLayer(/*+X*/0, SX, SY, SZ,
        [&](int32 u, int32 v, int32 w) { return Solid(u, v, w); },
        [&](int32 u, int32 v, int32 w) { return Solid(u + 1, v, w); });
    // ±Y
    MergeLayer(/*+Y*/2, SY, SX, SZ,
        [&](int32 u, int32 v, int32 w) { return Solid(v, u, w); },
        [&](int32 u, int32 v, int32 w) { return Solid(v, u + 1, w); });
    // ±Z
    MergeLayer(/*+Z*/4, SZ, SX, SY,
        [&](int32 u, int32 v, int32 w) { return Solid(v, w, u); },
        [&](int32 u, int32 v, int32 w) { return Solid(v, w, u + 1); });

    const float sx = VoxelUU * float(C.XYScale);
    const float sy = VoxelUU * float(C.XYScale);
    const float sz = VoxelUU;

    Out.Vertices.Reserve(Q.Num() * 4);
    Out.Normals.Reserve(Q.Num() * 4);
    Out.UVs.Reserve(Q.Num() * 4);
    Out.Triangles.Reserve(Q.Num() * 6);

    auto pushQuad = [&](const Quad& q)
        {
            FVector o, ux, vy, n;
            switch (q.dir)
            {
            case 0: o = FVector((q.x + 1) * sx, q.y * sy, q.z * sz); ux = FVector(0, q.w * sy, 0); vy = FVector(0, 0, q.h * sz); n = FVector(+1, 0, 0); break;
            case 1: o = FVector(q.x * sx, (q.y + q.w) * sy, q.z * sz); ux = FVector(0, -q.w * sy, 0); vy = FVector(0, 0, q.h * sz); n = FVector(-1, 0, 0); break;
            case 2: o = FVector(q.x * sx, (q.y + 1) * sy, q.z * sz); ux = FVector(q.w * sx, 0, 0); vy = FVector(0, 0, q.h * sz); n = FVector(0, +1, 0); break;
            case 3: o = FVector((q.x + q.w) * sx, q.y * sy, q.z * sz); ux = FVector(-q.w * sx, 0, 0); vy = FVector(0, 0, q.h * sz); n = FVector(0, -1, 0); break;
            case 4: o = FVector(q.x * sx, q.y * sy, (q.z + 1) * sz); ux = FVector(q.w * sx, 0, 0); vy = FVector(0, q.h * sy, 0); n = FVector(0, 0, +1); break;
            default:o = FVector(q.x * sx, (q.y + q.h) * sy, q.z * sz);   ux = FVector(q.w * sx, 0, 0); vy = FVector(0, -q.h * sy, 0); n = FVector(0, 0, -1); break;
            }

            const int32 i0 = Out.Vertices.Num();
            Out.Vertices.Add(o);
            Out.Vertices.Add(o + ux);
            Out.Vertices.Add(o + ux + vy);
            Out.Vertices.Add(o + vy);

            Out.Normals.Add(n); Out.Normals.Add(n); Out.Normals.Add(n); Out.Normals.Add(n);
            Out.UVs.Add(FVector2D(0, 0)); Out.UVs.Add(FVector2D(1, 0)); Out.UVs.Add(FVector2D(1, 1)); Out.UVs.Add(FVector2D(0, 1));

            Out.Triangles.Add(i0 + 0); Out.Triangles.Add(i0 + 2); Out.Triangles.Add(i0 + 1);
            Out.Triangles.Add(i0 + 0); Out.Triangles.Add(i0 + 3); Out.Triangles.Add(i0 + 2);
        };

    for (const Quad& q : Q) pushQuad(q);
}
namespace
{

    // popcount of lower b bits
    FORCEINLINE int32 PopCountLower(uint64 word, int32 b)
    {
        if (b >= 64) return FMath::CountBits(word);
        const uint64 mask = (b == 0) ? 0ULL : ((1ULL << b) - 1ULL);
        return FMath::CountBits(word & mask);
    }
}

void UVoxelMesher::RebuildPrefix64(FCompactVoxelData& C)
{
    const int32 W = C.Occupancy.Num();
    C.Prefix64.SetNumUninitialized(W);
    uint32 acc = 0;
    for (int32 i = 0; i < W; ++i)
    {
        C.Prefix64[i] = acc;
        acc += (uint32)FMath::CountBits(C.Occupancy[i]);
    }
}

int32 UVoxelMesher::Rank1_Prefix(const FCompactVoxelData& C, int32 linearIdx)
{
    const int32 w = linearIdx >> 6;
    const int32 b = linearIdx & 63;
    const uint32 pref = (uint32)((uint32)w < (uint32)C.Prefix64.Num() ? C.Prefix64[w] : 0u);
    const uint64 word = ((uint32)w < (uint32)C.Occupancy.Num()) ? C.Occupancy[w] : 0ULL;
    return (int32)(pref + PopCountLower(word, b));
}

int32 UVoxelMesher::Idx3D(int32 x, int32 y, int32 z, int32 SX, int32 SY)
{
    return x + y * SX + z * SX * SY;
}

void UVoxelMesher::BuildGreedyMesh_FromBitset_AO(
    const FCompactVoxelData& C,
    const FChunkNeighbors* Nbh,
    float UU,
    bool bUseAO,
    FMeshBuffers& Out)
{
    TRACE_CPUPROFILER_EVENT_SCOPE(Voxel_Mesher_LOD0_BitsetAO);

    Out = FMeshBuffers{};
    const int32 SX = C.SizeX, SY = C.SizeY, SZ = C.SizeZ;
    if (SX <= 0 || SY <= 0 || SZ <= 0 || C.Occupancy.Num() == 0) return;

    // --- neighbor solid masks (your existing helper) ---
    FNeighborSolidMasks Masks;
    BuildNeighborSolidMasks(Nbh, SX, SY, SZ, Masks);

    auto SolidLocal = [&](int32 x, int32 y, int32 z)->bool
        {
            if ((uint32)x >= (uint32)SX || (uint32)y >= (uint32)SY || (uint32)z >= (uint32)SZ) return false;
            const int32 i = x + y * SX + z * SX * SY;
            const int32 w = i >> 6, b = i & 63;
            return (C.Occupancy[w] >> b) & 1ULL;
        };

    auto SolidWithHalo = [&](int32 x, int32 y, int32 z)->bool
        {
            // inside chunk
            if ((uint32)x < (uint32)SX && (uint32)y < (uint32)SY && (uint32)z < (uint32)SZ)
                return SolidLocal(x, y, z);

            // outside → use 1-bit neighbor masks like in BuildGreedyMesh_FaceMask
            if (z >= 0 && z < SZ)
            {
                if (x < 0 && y >= 0 && y < SY && Masks.bHasXNeg) return Masks.XNeg[y + z * SY];
                if (x >= SX && y >= 0 && y < SY && Masks.bHasXPos) return Masks.XPos[y + z * SY];

                if (y < 0 && x >= 0 && x < SX && Masks.bHasYNeg) return Masks.YNeg[x + z * SX];
                if (y >= SY && x >= 0 && x < SX && Masks.bHasYPos) return Masks.YPos[x + z * SX];
            }
            if (x >= 0 && x < SX && y >= 0 && y < SY)
            {
                if (z < 0 && Masks.bHasZNeg) return Masks.ZNeg[x + y * SX];
                if (z >= SZ && Masks.bHasZPos) return Masks.ZPos[x + y * SX];
            }
            return false;
        };

    struct Quad { int32 x, y, z, w, h; uint8 dir; };
    TArray<Quad> Q; Q.Reserve(SX * SY + SY * SZ + SZ * SX);

    auto MergeLayer = [&](uint8 dir, int32 U, int32 Vd, int32 Wd,
        auto atSolid, auto atOpp)
        {
            TArray<uint8> mask; mask.SetNumZeroed(Vd * Wd);

            for (int32 u = 0; u < U; ++u)
            {
                // face mask for slice u
                for (int32 w = 0; w < Wd; ++w)
                    for (int32 v = 0; v < Vd; ++v)
                    {
                        const bool a = atSolid(u, v, w);
                        const bool b = atOpp(u, v, w);
                        mask[v + w * Vd] = (a ^ b) ? (a ? 1 : 2) : 0; // 1=+dir,2=-dir
                    }

                int32 i = 0;
                while (i < Vd * Wd)
                {
                    if (mask[i] == 0) { ++i; continue; }
                    const uint8 val = mask[i];
                    const int32 v0 = i % Vd;
                    const int32 w0 = i / Vd;

                    int32 width = 1;  while (v0 + width < Vd && mask[i + width] == val) ++width;
                    int32 height = 1; bool ok = true;
                    while (w0 + height < Wd && ok)
                    {
                        for (int32 k = 0; k < width; ++k)
                            if (mask[i + k + height * Vd] != val) { ok = false; break; }
                        if (ok) ++height;
                    }

                    for (int32 hh = 0; hh < height; ++hh)
                        FMemory::Memset(mask.GetData() + (i + hh * Vd), 0, width);

                    Quad q; q.dir = (val == 1) ? dir : (dir ^ 1);
                    if (dir < 2) { q.x = u; q.y = v0; q.z = w0; q.w = width; q.h = height; }       // ±X
                    else if (dir < 4) { q.x = v0; q.y = u; q.z = w0; q.w = width; q.h = height; }  // ±Y
                    else { q.x = v0; q.y = w0; q.z = u; q.w = width; q.h = height; }  // ±Z
                    Q.Add(q);
                }
            }
        };

    // ±X
    MergeLayer(/*+X*/0, SX, SY, SZ,
        [&](int32 u, int32 v, int32 w) { return SolidLocal(u, v, w); },
        [&](int32 u, int32 v, int32 w) { return SolidWithHalo(u + 1, v, w); });
    // ±Y
    MergeLayer(/*+Y*/2, SY, SX, SZ,
        [&](int32 u, int32 v, int32 w) { return SolidLocal(v, u, w); },
        [&](int32 u, int32 v, int32 w) { return SolidWithHalo(v, u + 1, w); });
    // ±Z
    MergeLayer(/*+Z*/4, SZ, SX, SY,
        [&](int32 u, int32 v, int32 w) { return SolidLocal(v, w, u); },
        [&](int32 u, int32 v, int32 w) { return SolidWithHalo(v, w, u + 1); });

    const float sx = UU * float(C.XYScale);
    const float sy = UU * float(C.XYScale);
    const float sz = UU;

    Out.Vertices.Reserve(Q.Num() * 4);
    Out.Normals.Reserve(Q.Num() * 4);
    Out.UVs.Reserve(Q.Num() * 4);
    Out.Colors.Reserve(Q.Num() * 4);
    Out.Triangles.Reserve(Q.Num() * 6);

    auto AOShade = [&](int32 x, int32 y, int32 z, const FVector& n)->float
        {
            if (!bUseAO) return 1.0f;
            // simple 7-sample AO
            const int ax = (n.X > 0) - (n.X < 0), ay = (n.Y > 0) - (n.Y < 0), az = (n.Z > 0) - (n.Z < 0);
            const bool s1 = SolidWithHalo(x + ax, y, z);
            const bool s2 = SolidWithHalo(x, y + ay, z);
            const bool c1 = SolidWithHalo(x + ax, y + ay, z);
            const bool s3 = SolidWithHalo(x, y, z + az);
            const bool s4 = SolidWithHalo(x + ax, y, z + az);
            const bool s5 = SolidWithHalo(x, y + ay, z + az);
            const bool c2 = SolidWithHalo(x + ax, y + ay, z + az);
            int occ = (s1 ? 1 : 0) + (s2 ? 1 : 0) + (c1 ? 1 : 0) + (s3 ? 1 : 0) + (s4 ? 1 : 0) + (s5 ? 1 : 0) + (c2 ? 1 : 0);
            return FMath::Clamp(1.0f - 0.07f * occ, 0.5f, 1.0f);
        };

    auto pushQuad = [&](const Quad& q)
        {
            FVector o, ux, vy, n;
            switch (q.dir)
            {
            case 0: o = FVector((q.x + 1) * sx, q.y * sy, q.z * sz); ux = FVector(0, q.w * sy, 0); vy = FVector(0, 0, q.h * sz); n = FVector(+1, 0, 0); break;
            case 1: o = FVector(q.x * sx, (q.y + q.w) * sy, q.z * sz); ux = FVector(0, -q.w * sy, 0); vy = FVector(0, 0, q.h * sz); n = FVector(-1, 0, 0); break;
            case 2: o = FVector(q.x * sx, (q.y + 1) * sy, q.z * sz); ux = FVector(q.w * sx, 0, 0); vy = FVector(0, 0, q.h * sz); n = FVector(0, +1, 0); break;
            case 3: o = FVector((q.x + q.w) * sx, q.y * sy, q.z * sz); ux = FVector(-q.w * sx, 0, 0); vy = FVector(0, 0, q.h * sz); n = FVector(0, -1, 0); break;
            case 4: o = FVector(q.x * sx, q.y * sy, (q.z + 1) * sz); ux = FVector(q.w * sx, 0, 0); vy = FVector(0, q.h * sy, 0); n = FVector(0, 0, +1); break;
            default:o = FVector(q.x * sx, (q.y + q.h) * sy, q.z * sz);   ux = FVector(q.w * sx, 0, 0); vy = FVector(0, -q.h * sy, 0); n = FVector(0, 0, -1); break;
            }

            const int32 i0 = Out.Vertices.Num();
            Out.Vertices.Add(o);
            Out.Vertices.Add(o + ux);
            Out.Vertices.Add(o + ux + vy);
            Out.Vertices.Add(o + vy);

            Out.Normals.Add(n); Out.Normals.Add(n); Out.Normals.Add(n); Out.Normals.Add(n);
            Out.UVs.Add(FVector2D(0, 0)); Out.UVs.Add(FVector2D(1, 0)); Out.UVs.Add(FVector2D(1, 1)); Out.UVs.Add(FVector2D(0, 1));

            const float a0 = AOShade(q.x, q.y, q.z, n);
            Out.Colors.Add(FLinearColor(a0, a0, a0, 1));
            Out.Colors.Add(FLinearColor(a0, a0, a0, 1));
            Out.Colors.Add(FLinearColor(a0, a0, a0, 1));
            Out.Colors.Add(FLinearColor(a0, a0, a0, 1));

            Out.Triangles.Add(i0 + 0); Out.Triangles.Add(i0 + 2); Out.Triangles.Add(i0 + 1);
            Out.Triangles.Add(i0 + 0); Out.Triangles.Add(i0 + 3); Out.Triangles.Add(i0 + 2);
        };

    for (const Quad& q : Q) pushQuad(q);
}