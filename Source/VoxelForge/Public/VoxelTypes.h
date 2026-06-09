// VoxelTypes.h
// Core type definitions, coordinate conversions, and mesh data.
//
// Ce header est le socle du plugin — tout le monde l'inclut.
// Pas de dépendance sur des UClasses ici (on reste léger).

#pragma once

#include "CoreMinimal.h"

//=============================================================================
// CHUNK CONSTANTS
//=============================================================================
//
// Taille de chunk: 32^3 = 32 768 voxels.
// Pourquoi 32 ? Puissance de 2 → astuces bit à bit + bon alignement GPU.
// VOXEL_SIZE = 25 cm/voxel (Unreal travaille en centimètres).

constexpr int32 CHUNK_SIZE         = 32;
constexpr int32 CHUNK_SIZE_SQUARED = CHUNK_SIZE * CHUNK_SIZE;     // 1024
constexpr int32 CHUNK_VOLUME       = CHUNK_SIZE * CHUNK_SIZE * CHUNK_SIZE; // 32768

constexpr float VOXEL_SIZE = 25.0f;

//=============================================================================
// FACE DIRECTIONS
//=============================================================================
//
// Les 6 faces d'un cube. Gardé ici car le mesher peut encore en avoir besoin
// (ex. normales orientées), et les strates pourraient s'en servir pour des
// décorations "sur le sol" vs "au plafond".

enum class EVoxelFace : uint8
{
    PositiveX,  // East  (+X)
    NegativeX,  // West  (-X)
    PositiveY,  // North (+Y)
    NegativeY,  // South (-Y)
    PositiveZ,  // Up    (+Z)
    NegativeZ,  // Down  (-Z)
};

inline FIntVector GetFaceDirection(EVoxelFace Face)
{
    switch (Face)
    {
        case EVoxelFace::PositiveX: return FIntVector( 1,  0,  0);
        case EVoxelFace::NegativeX: return FIntVector(-1,  0,  0);
        case EVoxelFace::PositiveY: return FIntVector( 0,  1,  0);
        case EVoxelFace::NegativeY: return FIntVector( 0, -1,  0);
        case EVoxelFace::PositiveZ: return FIntVector( 0,  0,  1);
        case EVoxelFace::NegativeZ: return FIntVector( 0,  0, -1);
        default:                    return FIntVector( 0,  0,  0);
    }
}

inline FVector GetFaceNormal(EVoxelFace Face)
{
    const FIntVector Dir = GetFaceDirection(Face);
    return FVector(Dir.X, Dir.Y, Dir.Z);
}

//=============================================================================
// COORDINATE CONVERSION
//=============================================================================
//
// Trois espaces:
//   WORLD (FVector, cm)     → position Unreal
//   CHUNK (FIntVector)      → quel chunk (32 voxels)
//   LOCAL (FIntVector 0-31) → position dans le chunk
//
// Relation: WorldPos = (ChunkCoord * CHUNK_SIZE + LocalPos) * VOXEL_SIZE

inline FIntVector WorldToChunkCoord(const FVector& WorldPos)
{
    // FloorToInt (pas division int) pour gérer proprement les coords négatives:
    //   -5 / 32 = 0  (faux — on veut -1)
    //   floor(-5 / 32) = floor(-0.156) = -1  (correct)
    return FIntVector(
        FMath::FloorToInt((WorldPos.X / VOXEL_SIZE) / CHUNK_SIZE),
        FMath::FloorToInt((WorldPos.Y / VOXEL_SIZE) / CHUNK_SIZE),
        FMath::FloorToInt((WorldPos.Z / VOXEL_SIZE) / CHUNK_SIZE)
    );
}

inline FIntVector WorldToLocalCoord(const FVector& WorldPos)
{
    // ((x % n) + n) % n → modulo positif même pour x négatif.
    //   (-5 % 32) = -5 en C++, mais on veut 27.
    return FIntVector(
        ((FMath::FloorToInt(WorldPos.X / VOXEL_SIZE) % CHUNK_SIZE) + CHUNK_SIZE) % CHUNK_SIZE,
        ((FMath::FloorToInt(WorldPos.Y / VOXEL_SIZE) % CHUNK_SIZE) + CHUNK_SIZE) % CHUNK_SIZE,
        ((FMath::FloorToInt(WorldPos.Z / VOXEL_SIZE) % CHUNK_SIZE) + CHUNK_SIZE) % CHUNK_SIZE
    );
}

inline FVector ChunkToWorldPos(const FIntVector& ChunkCoord)
{
    return FVector(
        ChunkCoord.X * CHUNK_SIZE * VOXEL_SIZE,
        ChunkCoord.Y * CHUNK_SIZE * VOXEL_SIZE,
        ChunkCoord.Z * CHUNK_SIZE * VOXEL_SIZE
    );
}

// Index 3D → 1D pour un tableau plat (voir doc CLAUDE.md: "x + y*SizeX + z*SizeX*SizeY").
inline int32 LocalToIndex(int32 X, int32 Y, int32 Z)
{
    return X + (Y * CHUNK_SIZE) + (Z * CHUNK_SIZE_SQUARED);
}

inline FIntVector IndexToLocal(int32 Index)
{
    return FIntVector(
        Index % CHUNK_SIZE,
        (Index / CHUNK_SIZE) % CHUNK_SIZE,
        Index / CHUNK_SIZE_SQUARED
    );
}

inline bool IsValidLocalCoord(int32 X, int32 Y, int32 Z)
{
    return X >= 0 && X < CHUNK_SIZE
        && Y >= 0 && Y < CHUNK_SIZE
        && Z >= 0 && Z < CHUNK_SIZE;
}

inline bool IsValidLocalCoord(const FIntVector& Coord)
{
    return IsValidLocalCoord(Coord.X, Coord.Y, Coord.Z);
}

//=============================================================================
// MATH HELPERS (partagés entre générateur, morphologie, manager)
//=============================================================================

// Smoothstep classique: 3x² - 2x³. Maps 0→0, 1→1, avec pente nulle aux bornes.
// Utilisé partout pour adoucir les transitions de densité (blend SDF, seal boundary).
// Entrée: x doit être dans [0, 1] (clamp avant si besoin).
inline float SmoothStep01(float x)
{
    return x * x * (3.0f - 2.0f * x);
}

// UE's PerlinNoise3D renvoie ~[-0.8, 0.8] — ce facteur remet à ~[-1, 1]
// pour correspondre aux attentes des formules de densité.
constexpr float VOXEL_NOISE_SCALE = 1.25f;

//=============================================================================
// MESH DATA
//=============================================================================
//
// Sortie du mesher: géométrie prête pour l'upload GPU.
// Struct plain C++ (pas de USTRUCT) — uniquement utilisé entre tâches C++.
// Si lighting est ajouté plus tard, ré-ajouter un champ Colors dédié.

struct FVoxelMeshData
{
    TArray<FVector>   Vertices;   // Positions monde
    TArray<int32>     Triangles;  // Indices, 3 par triangle
    TArray<FVector2D> UVs;        // Coords de texture (une par vertex)
    TArray<FVector>   Normals;    // Normale lissée (gradient de densité)

    void Clear()
    {
        Vertices.Empty();
        Triangles.Empty();
        UVs.Empty();
        Normals.Empty();
    }

    bool IsEmpty() const { return Vertices.Num() == 0; }
};
