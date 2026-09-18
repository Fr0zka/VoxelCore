// VoxelGeometryHash.h
// The compact geometry identity shared by offline export and runtime streaming diagnostics.

#pragma once

#include "CoreMinimal.h"
#include "Misc/Crc.h"
#include "VoxelTypes.h"

namespace VoxelForgeGeometryHash
{
    // Keep this deliberately identical to the historical explorer hash.  It is a cheap,
    // deterministic identity for a tile's complete mesh payload; changing it would invalidate
    // the commandlet's existing canonical evidence.
    inline FString Compute(const FVoxelMeshData& MeshData)
    {
        uint32 Crc = 0;
        const int32 Counts[] = {
            MeshData.Vertices.Num(),
            MeshData.Normals.Num(),
            MeshData.UVs.Num(),
            MeshData.Colors.Num(),
            MeshData.Triangles.Num(),
            MeshData.NumCeilingTriangles,
        };
        Crc = FCrc::MemCrc32(Counts, sizeof(Counts), Crc);
        if (MeshData.Vertices.Num() > 0)
        {
            Crc = FCrc::MemCrc32(
                MeshData.Vertices.GetData(),
                MeshData.Vertices.Num() * sizeof(FVector), Crc);
        }
        if (MeshData.Normals.Num() > 0)
        {
            Crc = FCrc::MemCrc32(
                MeshData.Normals.GetData(),
                MeshData.Normals.Num() * sizeof(FVector), Crc);
        }
        if (MeshData.UVs.Num() > 0)
        {
            Crc = FCrc::MemCrc32(
                MeshData.UVs.GetData(),
                MeshData.UVs.Num() * sizeof(FVector2D), Crc);
        }
        if (MeshData.Colors.Num() > 0)
        {
            Crc = FCrc::MemCrc32(
                MeshData.Colors.GetData(),
                MeshData.Colors.Num() * sizeof(FColor), Crc);
        }
        if (MeshData.Triangles.Num() > 0)
        {
            Crc = FCrc::MemCrc32(
                MeshData.Triangles.GetData(),
                MeshData.Triangles.Num() * sizeof(int32), Crc);
        }
        return FString::Printf(TEXT("%08X"), Crc);
    }
}
