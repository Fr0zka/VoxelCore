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
// VOXEL_SIZE = 25 cm/voxel (Unreal travaille en centimètres). 1 chunk = 8 m.
//
// NOTE: 64³ a été essayé ("B", 2026-06-16) pour couper les draw calls (8× moins de
// chunks) mais le streaming devenait trop saccadé (briques 8× plus lourdes → applies
// + spawns de contenu en gros à-coups sur le game thread). Reverté à 32³ : le fps se
// règle côté RENDU (ombres off sur LOD lointain, voir ApplyMeshToChunk), pas via la
// taille de chunk. La taille de chunk reste le levier streaming-vs-draws si besoin.

constexpr int32 CHUNK_SIZE         = 32;
constexpr int32 CHUNK_SIZE_SQUARED = CHUNK_SIZE * CHUNK_SIZE;     // 1024
constexpr int32 CHUNK_VOLUME       = CHUNK_SIZE * CHUNK_SIZE * CHUNK_SIZE; // 32768

constexpr float VOXEL_SIZE = 25.0f;

//=============================================================================
// DENSITY → R8 QUANTIZATION (density clipmap / mini-sun shadows)
//=============================================================================
//
// MC convention: NÉGATIF = solide, POSITIF = air, 0 = isosurface. On encode la densité
// dans un R8 où SOLIDE = HAUT, AIR = BAS, iso ≈ 0.5 (128), clampé ±1 autour de la surface
// (loin de la surface ⇒ sature plein solide / plein air). Le trilinear garde la traversée
// iso sub-voxel nette.
//
// SHARED entre deux producteurs qui DOIVENT rester bit-identiques :
//   1. UVoxelDensityVolume (fill worker re-évaluant GetDensityAt),
//   2. UVoxelMarchingCubesMesher (capture-during-meshing : réutilise la grille déjà
//      échantillonnée par le mesher au lieu de re-sampler — voir GenerateMesh OutCaptureGrid).
// Même densité d'entrée ⇒ même octet. Ne PAS dupliquer cette formule ailleurs.
FORCEINLINE uint8 VF_QuantizeDensity(float MCDensity)
{
    const float S = FMath::Clamp(0.5f - 0.5f * MCDensity, 0.0f, 1.0f);
    return (uint8)FMath::RoundToInt(S * 255.0f);
}

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

// Le coeur de bruit (VoxelNoise::Perlin3D, T2.a) renvoie ~[-0.8, 0.8] comme l'ancien
// FMath::PerlinNoise3D — ce facteur remet à ~[-1, 1] pour les formules de densité.
constexpr float VOXEL_NOISE_SCALE = 1.25f;

//=============================================================================
// CLIPMAP TILE KEY
//=============================================================================
//
// A "tile" generalises a chunk for the chunked-LOD clipmap. A level-L tile spans
// (CHUNK_SIZE << Level) voxels per axis and is meshed at step (1 << Level), so it always
// produces a constant CHUNK_SIZE³-cell mesh (one component, one draw) but covers 8^Level ×
// the volume. Level 0 = a full-resolution chunk; each level up doubles linear size.
// Streaming loads concentric shells: level 0 near the player, coarser levels farther out,
// so chunk/draw count stays ~flat regardless of view distance.

struct FVoxelTileKey
{
    FIntVector Coord = FIntVector::ZeroValue;   // in units of (CHUNK_SIZE << Level) voxels
    int32      Level = 0;                        // 0 = full res

    FVoxelTileKey() = default;
    FVoxelTileKey(const FIntVector& InCoord, int32 InLevel) : Coord(InCoord), Level(InLevel) {}

    // Cell size (sampling step) in voxels, and tile extent in voxels per axis.
    int32 StepVoxels()   const { return 1 << Level; }
    int32 ExtentVoxels() const { return CHUNK_SIZE << Level; }

    // Min-corner of the tile in VOXEL coords (what the mesher takes).
    FIntVector OriginVoxels() const { return Coord * (CHUNK_SIZE << Level); }

    // Tile centre in world cm (for distance sorting / LOD selection).
    FVector CenterCm() const
    {
        const double Ext = (double)(CHUNK_SIZE << Level) * (double)VOXEL_SIZE;
        return FVector((Coord.X + 0.5) * Ext, (Coord.Y + 0.5) * Ext, (Coord.Z + 0.5) * Ext);
    }

    bool operator==(const FVoxelTileKey& O) const { return Level == O.Level && Coord == O.Coord; }
    bool operator!=(const FVoxelTileKey& O) const { return !(*this == O); }
};

FORCEINLINE uint32 GetTypeHash(const FVoxelTileKey& K)
{
    return HashCombine(GetTypeHash(K.Coord), ::GetTypeHash(K.Level));
}

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
    TArray<FColor>    Colors;     // Masques matériau F6 (R=palette biome dominant, G=pente,
                                  // B=poids de fondu de bordure, A=palette biome voisin)

    // F17 — classe de surface par TRIANGLE, portée par l'ORDRE du buffer d'indices : les
    // NumCeilingTriangles DERNIERS triangles de Triangles sont la classe "plafond sky-cap"
    // (polygroup 1 → slot matériau 1, sans ombre) ; tout ce qui précède est "sol/roche"
    // (polygroup 0). Runs contigus exigés par RMC (une section par polygroup).
    int32 NumCeilingTriangles = 0;

    void Clear()
    {
        Vertices.Empty();
        Triangles.Empty();
        UVs.Empty();
        Normals.Empty();
        Colors.Empty();
        NumCeilingTriangles = 0;
    }

    bool IsEmpty() const { return Vertices.Num() == 0; }
};
