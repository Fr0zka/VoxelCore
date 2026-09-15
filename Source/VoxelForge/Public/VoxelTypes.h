// VoxelTypes.h
// Core type definitions, coordinate conversions, and mesh data.
//
// Ce header est le socle du plugin — tout le monde l'inclut.
// Pas de dépendance sur des UClasses ici (on reste léger).

#pragma once

#include "CoreMinimal.h"

//=============================================================================
// EXACT FINITE PREDICATE / PREDICAT FINI EXACT
//=============================================================================
// The CRT-backed FMath::IsFinite is an out-of-line call on the MSVC game path.  A finite
// IEEE-754 value is identified entirely by its exponent bits, so this helper has the same
// boolean result for every bit pattern without doing a floating-point comparison.
namespace VoxelMath
{
    // Runtime policy switch.  The fast helper below is deliberately independent of this value so
    // the equivalence test can compare it directly with FMath::IsFinite.  IsFinite() is the A/B
    // entry point used by generation code.
    VOXELFORGE_API extern int32 GFastIsFinite;

    FORCEINLINE bool IsFiniteFast(float Value)
    {
        static_assert(sizeof(float) == sizeof(uint32), "VoxelMath::IsFiniteFast requires IEEE float");
        uint32 Bits = 0;
        FMemory::Memcpy(&Bits, &Value, sizeof(Bits));
        constexpr uint32 ExponentMask = 0x7F800000u;
        return (Bits & ExponentMask) != ExponentMask;
    }

    FORCEINLINE bool IsFiniteFast(double Value)
    {
        static_assert(sizeof(double) == sizeof(uint64), "VoxelMath::IsFiniteFast requires IEEE double");
        uint64 Bits = 0;
        FMemory::Memcpy(&Bits, &Value, sizeof(Bits));
        constexpr uint64 ExponentMask = 0x7FF0000000000000ull;
        return (Bits & ExponentMask) != ExponentMask;
    }

    // Keep the switch at the call site while keeping the exact bit test separate and directly
    // testable.  In the default-on game path this inlines to the bit load/mask/branch; the 0 path
    // is retained for within-round performance and sampler A/B runs.
    FORCEINLINE bool IsFinite(float Value)
    {
        return GFastIsFinite != 0 ? IsFiniteFast(Value) : FMath::IsFinite(Value);
    }

    FORCEINLINE bool IsFinite(double Value)
    {
        return GFastIsFinite != 0 ? IsFiniteFast(Value) : FMath::IsFinite(Value);
    }

    // Deterministic trigonometry for world decisions.
    //
    // The implementation deliberately owns both the range reduction and the polynomial.  It
    // does not call the CRT, FMath, or a platform vector intrinsic: the same finite float is
    // reduced by the same constants and evaluated by the same scalar arithmetic on every host.
    // The reduction is done in double so world-coordinate phases do not lose all low bits before
    // the period is removed.  The polynomial itself is float arithmetic, matching the values
    // consumed by the density field and keeping the hot path small.
    FORCEINLINE void DetSinCos(float& OutSin, float& OutCos, float Value)
    {
        if (!IsFiniteFast(Value))
        {
            // Non-finite values are outside the world contract.  Preserve the input rather than
            // entering an undefined integer conversion during range reduction.
            OutSin = Value;
            OutCos = Value;
            return;
        }

        constexpr double TwoPi = 6.283185307179586476925286766559;
        constexpr double HalfPi = 1.57079632679489661923132169164;

        // Round the number of complete periods toward the nearest integer.  All measured world
        // phases are many orders of magnitude below INT64_MAX.  For a pathological finite input
        // whose period count does not fit in int64, use the exact integer significand from the
        // float representation and reduce it by fixed binary doubling instead of converting an
        // out-of-range quotient.
        const double PeriodsReal = static_cast<double>(Value) / TwoPi;
        constexpr double MaxSafeInt64 = 9223372036854774784.0;
        double Reduced = 0.0;
        if (PeriodsReal >= MaxSafeInt64 || PeriodsReal <= -MaxSafeInt64)
        {
            uint32 ValueBits = 0;
            FMemory::Memcpy(&ValueBits, &Value, sizeof(ValueBits));
            const uint32 MagnitudeBits = ValueBits & 0x7FFFFFFFu;
            const uint32 ExponentBits = (MagnitudeBits >> 23) & 0xFFu;
            const uint32 Significand = (MagnitudeBits & 0x007FFFFFu) | 0x00800000u;
            const int32 BinaryExponent = static_cast<int32>(ExponentBits) - 127 - 23;

            double ReducedInteger = 0.0;
            for (int32 Bit = 23; Bit >= 0; --Bit)
            {
                ReducedInteger *= 2.0;
                ReducedInteger += static_cast<double>((Significand >> Bit) & 1u);
                if (ReducedInteger >= TwoPi)
                {
                    ReducedInteger -= TwoPi;
                }
            }
            for (int32 Shift = 0; Shift < BinaryExponent; ++Shift)
            {
                ReducedInteger *= 2.0;
                if (ReducedInteger >= TwoPi)
                {
                    ReducedInteger -= TwoPi;
                }
            }
            Reduced = (ValueBits & 0x80000000u) != 0u ? -ReducedInteger : ReducedInteger;
        }
        else
        {
            int64 Periods = 0;
            if (PeriodsReal >= 0.0)
            {
                Periods = static_cast<int64>(PeriodsReal + 0.5);
            }
            else
            {
                Periods = static_cast<int64>(PeriodsReal - 0.5);
            }

            Reduced = static_cast<double>(Value)
                - static_cast<double>(Periods) * TwoPi;
        }

        // Reduce the remaining angle to [-pi/4, pi/4].  Both reduction branches above keep
        // Reduced bounded by one period, so this quadrant conversion is always in int32 range.
        const double QuadrantReal = Reduced / HalfPi;
        int32 Quadrant = 0;
        if (QuadrantReal >= 0.0)
        {
            Quadrant = static_cast<int32>(QuadrantReal + 0.5);
        }
        else
        {
            Quadrant = static_cast<int32>(QuadrantReal - 0.5);
        }
        Reduced -= static_cast<double>(Quadrant) * HalfPi;
        const float Y = static_cast<float>(Reduced);
        const float Y2 = Y * Y;

        // Fixed minimax polynomials, the same degree as UE's scalar SinCos implementation.  The
        // quarter-period reduction gives them a smaller interval than UE's [-pi/2, pi/2] path.
        const float SinP = (((((-2.3889859e-08f * Y2 + 2.7525562e-06f) * Y2
            - 0.00019840874f) * Y2 + 0.0083333310f) * Y2
            - 0.16666667f) * Y2 + 1.0f) * Y;
        const float CosP = (((((-2.6051615e-07f * Y2 + 2.4760495e-05f) * Y2
            - 0.0013888378f) * Y2 + 0.041666638f) * Y2
            - 0.5f) * Y2 + 1.0f);

        int32 QuadrantMod = Quadrant % 4;
        if (QuadrantMod < 0)
        {
            QuadrantMod += 4;
        }
        switch (QuadrantMod)
        {
            case 0:
                OutSin = SinP;
                OutCos = CosP;
                break;
            case 1:
                OutSin = CosP;
                OutCos = -SinP;
                break;
            case 2:
                OutSin = -SinP;
                OutCos = -CosP;
                break;
            default:
                OutSin = -CosP;
                OutCos = SinP;
                break;
        }
    }

    FORCEINLINE float DetSin(float Value)
    {
        float SinValue = 0.0f;
        float CosValue = 0.0f;
        DetSinCos(SinValue, CosValue, Value);
        return SinValue;
    }

    FORCEINLINE float DetCos(float Value)
    {
        float SinValue = 0.0f;
        float CosValue = 0.0f;
        DetSinCos(SinValue, CosValue, Value);
        return CosValue;
    }
}

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
// TRIVIAL-TILE CLASSIFICATION (T1.d)
//=============================================================================
// Verdict de ClassifyTile pour une tuile AVANT le pré-échantillonnage 33³+ :
// AllSolid / AllAir guarantee every core cell vertex of the mesher (g=0..Cells;
// the ±1 halo is normal-only) is on the same side of the iso ⇒ empty mesh, GenerateMesh is
// sautée. Mixed = "je ne peux pas le prouver" ⇒ génération normale. Un faux
// Mixed coûte juste du CPU ; un faux AllSolid/AllAir ferait un TROU — les
// verdicts ne sont donc émis que sur des bornes exactes (colonnes surface
// échantillonnées au MÊME treillis que le mesher) + gardes conservatives sur
// tout ce qui peut creuser/remplir (spine, passages, disturbances, diff layer).
//
// Vit ici plutôt que dans VoxelGenerator.h pour que VoxelDensityOp.h (le contrat
// de la pile d'opérateurs) puisse s'en servir sans tirer un header UCLASS.
// Lives here rather than in VoxelGenerator.h so VoxelDensityOp.h (the operator-stack
// contract) can use it without pulling in a UCLASS header.
enum class EVoxelTileClass : uint8
{
    Mixed,      // peut contenir une surface → mesher normalement
    AllSolid,   // chaque échantillon prouvé solide → maillage vide
    AllAir,     // chaque échantillon prouvé air   → maillage vide
};

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
