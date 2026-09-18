// VoxelCaveMorphology.h
// Hash-based cave room/tunnel generation and SDF evaluation.
//
// HOW IT WORKS:
// =============
// The world is divided into a coarse 2D grid (cell size = RoomSpacing).
// Each cell deterministically either contains a room or doesn't, based on
// a hash of (CellX, CellY, Seed, StrateIndex). No pre-computation needed —
// room positions are derived on-the-fly from the hash, so this works for
// infinite worlds without storing anything.
//
// ROOMS:
// - Origin room: guaranteed large room at (0,0) per strate — the future shaft landing
// - Hash rooms: ellipsoids, rounded boxes, or elongated capsules
// - All blended with smooth-min for organic junctions
//
// TUNNELS:
// - Connect pairs of rooms based on distance and probability
// - Tapered (variable min/max radius at each endpoint)
// - Curved wandering chains with deterministic perpendicular control-point jitter
// - Horizontal bias (vertical connections penalized)
// - Floor-aligned endpoints and variable radii preserve walkable room joins
// - The origin room is not joined to landings by radial connector roads
//
// PIPELINE POSITION:
// Step 3 (cave warp) bends the SDF query coordinates before we evaluate.
// This step (Step 4) builds the SDF skeleton of rooms + tunnels.
// Surface roughness (Step 4b), terrain ops, and worm tunnels layer on top.

#pragma once

#include "CoreMinimal.h"
#include "VoxelDensityProfile.h"

// Forward declarations
struct FStrateGenerationParams;
struct FSlabGenerationParams;
struct FMazeGenerationParams;
struct FVerticalShaftParams;
struct FFloatingIslandParams;
struct FStrateTerrainOpEntry;
class UVoxelTerrainOpDefinition;
enum class ECaveGeneratorType : uint8;

/**
 * The geometry contract for one end of an inter-strate passage.
 *
 * All values are actor-space voxels.  `StandingPoint` is the player-fit anchor: its floor is
 * `FloorZ = StandingPoint.Z - 0.5`.  The chamber is deliberately separate from the tube endpoint
 * so a sloped/vertical tube never presents the player with a hole as its first standing position.
 *
 * A source query answer selects the landing's XY/Z anchor. The landing is deliberately local to
 * that answer: it is a flat-floored room where the inter-strate tube meets a legal player-fit
 * pose. The (0,0) landing is the only place reserved for the future straight surface shaft;
 * progression mouths do not receive hidden radial roads to that spine room.
 */
struct VOXELFORGE_API FVoxelPassageLanding
{
    FVector StandingPoint = FVector::ZeroVector;
    FVector DoorPoint = FVector::ZeroVector;
    FVector DoorDirection = FVector(1.0f, 0.0f, 0.0f);

    float FloorZ = 0.0f;
    float CeilingZ = 0.0f;
    float HalfWidth = 0.0f;
    float FloorThickness = 3.0f;

    bool bSourcePlayerFit = false;
};

//=============================================================================
// SDF PRIMITIVES
//=============================================================================
// Signed Distance Field functions.
// Return value: negative = inside the shape, positive = outside.
// The "distance" is how far from the surface — 0.0 = exactly on the surface.

namespace VoxelSDF
{
    // Distance from point P to the surface of a sphere at Center with Radius.
    // Negative when P is inside the sphere.
    FORCEINLINE float Sphere(const FVector& P, const FVector& Center, float Radius)
    {
        return FVector::Dist(P, Center) - Radius;
    }

    // Distance from P to an ellipsoid (squished sphere).
    // Radii = (RadiusX, RadiusY, RadiusZ) — different size per axis.
    // We scale the point into a unit sphere, compute distance, then scale back.
    // This isn't an exact SDF (distances are approximate near the surface),
    // but it's good enough for marching cubes density evaluation.
    FORCEINLINE float Ellipsoid(const FVector& P, const FVector& Center, const FVector& Radii)
    {
        // Transform to unit sphere space
        FVector Scaled = (P - Center) / Radii;
        float ScaledDist = Scaled.Size();
        // Approximate: scale the distance back by the average radius
        float AvgRadius = (Radii.X + Radii.Y + Radii.Z) / 3.0f;
        return (ScaledDist - 1.0f) * AvgRadius;
    }

    // Distance from P to a capsule (line segment with thickness).
    // A and B are the endpoints, Radius is the tube thickness.
    // This is an EXACT SDF — used for tunnels.
    FORCEINLINE float Capsule(const FVector& P, const FVector& A, const FVector& B, float Radius)
    {
        FVector AB = B - A;
        FVector AP = P - A;
        // Project P onto line AB, clamped to [0,1] (stays within segment)
        float T = FMath::Clamp(FVector::DotProduct(AP, AB) / FMath::Max(FVector::DotProduct(AB, AB), KINDA_SMALL_NUMBER), 0.0f, 1.0f);
        // Closest point on the segment
        FVector Closest = A + AB * T;
        return FVector::Dist(P, Closest) - Radius;
    }

    // Distance from P to a rounded box (box with rounded edges).
    // Center = box center, HalfExtent = half-size per axis, Rounding = edge radius.
    // This creates angular chambers (rectangular rooms with smooth corners).
    // Without rounding (Rounding=0), it's a sharp box.
    // With rounding, edges and corners are smoothed — essential for MC quality.
    FORCEINLINE float RoundedBox(const FVector& P, const FVector& Center, const FVector& HalfExtent, float Rounding)
    {
        // Vector from center to P, take absolute value (box symmetry)
        FVector D = FVector(
            FMath::Abs(P.X - Center.X),
            FMath::Abs(P.Y - Center.Y),
            FMath::Abs(P.Z - Center.Z)
        ) - HalfExtent;

        // Distance outside the box (positive when outside)
        float Outside = FVector(
            FMath::Max(D.X, 0.0f),
            FMath::Max(D.Y, 0.0f),
            FMath::Max(D.Z, 0.0f)
        ).Size();

        // Distance inside the box (negative when inside)
        float Inside = FMath::Min(FMath::Max(D.X, FMath::Max(D.Y, D.Z)), 0.0f);

        return Outside + Inside - Rounding;
    }

    // Distance from P to a tapered capsule (cone-like tube with spherical caps).
    // A and B are endpoints, RadiusA and RadiusB are the tube radius at each end.
    // When RadiusA != RadiusB, the tube narrows/widens along its length.
    // This creates natural-looking corridors that aren't perfectly uniform.
    FORCEINLINE float TaperedCapsule(const FVector& P, const FVector& A, const FVector& B, float RadiusA, float RadiusB)
    {
        FVector AB = B - A;
        FVector AP = P - A;
        float LenSq = FVector::DotProduct(AB, AB);
        // T = how far along AB the closest point is (0 = at A, 1 = at B)
        float T = (LenSq > KINDA_SMALL_NUMBER)
            ? FMath::Clamp(FVector::DotProduct(AP, AB) / LenSq, 0.0f, 1.0f)
            : 0.0f;
        FVector Closest = A + AB * T;
        // Radius interpolates from RadiusA at A to RadiusB at B
        float R = FMath::Lerp(RadiusA, RadiusB, T);
        return FVector::Dist(P, Closest) - R;
    }

    // Polynomial smooth minimum — blends two SDF shapes together.
    // K controls the blend radius: higher K = rounder, smoother junctions.
    // With K=0, this is just FMath::Min(A, B) (hard intersection).
    // With K=4, room/tunnel junctions look organic and natural.
    FORCEINLINE float SmoothMin(float A, float B, float K)
    {
        if (K <= 0.0f) return FMath::Min(A, B);
        float H = FMath::Max(K - FMath::Abs(A - B), 0.0f) / K;
        return FMath::Min(A, B) - H * H * H * K * (1.0f / 6.0f);
    }

    // Polynomial smooth maximum — dual of SmoothMin, enforces the larger value softly.
    // Used for soft SDF intersections (e.g., floor cut planes) so the boundary
    // rounds off near tunnel/pit openings instead of creating a hard lip.
    // With K=0, this is just FMath::Max(A, B).
    FORCEINLINE float SmoothMax(float A, float B, float K)
    {
        return -SmoothMin(-A, -B, K);
    }
}

/** Build the deterministic, body-sized room geometry at one standing anchor. */
VOXELFORGE_API FVoxelPassageLanding VF_BuildPassageLanding(
    const FVector& StandingPoint,
    float MouthRadius,
    const FVector& DoorDirection,
    float StrateTopZ,
    float StrateBottomZ,
    float BoundarySealThickness,
    bool bSourcePlayerFit);

/** Signed distance of the local landing room. Negative means passage air. */
VOXELFORGE_API float VF_EvaluatePassageLandingSDF(
    const FVector& Position,
    const FVoxelPassageLanding& Landing);

/** True when a point belongs to the guaranteed solid floor slab of the landing room. */
VOXELFORGE_API bool VF_IsPassageLandingFloor(
    const FVector& Position,
    const FVoxelPassageLanding& Landing);

//=============================================================================
// HASH FUNCTIONS
//=============================================================================
// Deterministic integer hashing for room placement.
// Given (CellX, CellY, Seed), always returns the same result.
// No randomness — fully reproducible across sessions and clients.

namespace VoxelHash
{
    // Mix a single uint32 — scrambles bits to reduce patterns
    FORCEINLINE uint32 Mix(uint32 X)
    {
        X ^= X >> 16;
        X *= 0x45d9f3bu;
        X ^= X >> 16;
        X *= 0x45d9f3bu;
        X ^= X >> 16;
        return X;
    }

    /**
     * AUDIT §C1 — décalage de bruit BORNÉ et salé par site. Remplace le motif `SeedF * K`.
     *
     * LE BUG QUE ÇA CORRIGE : les sites de bruit s'écrivaient
     * `WorldX * Freq + (float)Seed * 97.7f`. Le float a 24 bits de mantisse, donc à magnitude `V`
     * l'ULP vaut `V · 2⁻²³`. Avec `Seed = 10⁷` le terme atteint 10⁹, où l'ULP vaut **117** — la
     * coordonnée du voxel (qui avance de ~0.02 par voxel) est **entièrement absorbée** et le champ
     * de bruit devient CONSTANT. Terrain plat. `ChangeSeed` est `BlueprintCallable`, donc un
     * `FMath::Rand()` suffit à déclencher ça. Ça ne marchait que parce que les seeds restaient petits.
     *
     * ⚠️ LE CORRECTIF ÉVIDENT EST FAUX. Borner `SeedF` à 16383 en gardant le `· 97.7` laisse le
     * terme atteindre 1.6e6, où l'ULP vaut 0.19 — **9.5× le pas par voxel**. Ça rend le bug moins
     * spectaculaire tout en le laissant vivant, et referme le ticket. C'est le multiplicateur qu'il
     * faut supprimer, pas le seed qu'il faut réduire.
     *
     * CE QUE FAIT CETTE FONCTION : le multiplicateur ne SERT plus à décorréler par amplification —
     * il IDENTIFIE le site, et c'est le hash qui décorrèle. La sortie est déjà dans les unités
     * finales, bornée à [0, 16383] : l'ULP y vaut 0.002, soit 10 % d'un pas de voxel.
     *
     * ET C'EST PLUS SÛR QU'UN SEEDF BORNÉ PARTAGÉ : avec un offset unique par monde, deux seeds qui
     * collident donneraient un bruit identique PARTOUT. Salé par site, il faudrait qu'ils
     * collident sur les ~50 sites à la fois — c'est-à-dire jamais.
     *
     * The multiplier no longer decorrelates by amplifying — it IDENTIFIES the site, and the hash
     * decorrelates. Output is already in final units and bounded, so the ULP is 10% of a voxel step.
     *
     * @param SiteKey  la constante littérale d'origine (`7.3f`, `97.7f`, …). Gardée VISIBLE au site
     *                 d'appel pour que la correspondance avec le code d'avant reste vérifiable à l'œil.
     */
    FORCEINLINE float SeedOffset(uint32 Seed, float SiteKey)
    {
        // ×100 puis arrondi : les constantes ont au plus 2 décimales, donc `0.31f` → 31 et
        // `3.1f` → 310 restent distincts. Le site est une identité entière, pas un flottant.
        const uint32 Site = (uint32)(SiteKey * 100.0f + 0.5f);
        return (float)(Mix(Seed ^ (Site * 2654435761u)) & 0x3FFFu);   // [0, 16383]
    }

    // Hash a 2D cell coordinate with a seed → deterministic uint32
    FORCEINLINE uint32 Cell(int32 CellX, int32 CellY, uint32 Seed)
    {
        uint32 H = Seed;
        H ^= Mix((uint32)(CellX + 0x7FFFFFFF));
        H ^= Mix((uint32)(CellY + 0x7FFFFFFF) * 2654435761u);
        return Mix(H);
    }

    // Hash a pair of cells (for tunnel connectivity decisions).
    // Order-independent: Cell(A,B) == Cell(B,A) so each tunnel is evaluated once.
    FORCEINLINE uint32 Pair(int32 AX, int32 AY, int32 BX, int32 BY, uint32 Seed)
    {
        // Sort the pair so (A,B) and (B,A) give the same hash
        int32 MinX = FMath::Min(AX, BX), MinY = FMath::Min(AY, BY);
        int32 MaxX = FMath::Max(AX, BX), MaxY = FMath::Max(AY, BY);
        uint32 H = Seed ^ 0xDEADBEEF;
        H ^= Mix((uint32)(MinX + 0x7FFFFFFF));
        H ^= Mix((uint32)(MinY + 0x7FFFFFFF) * 2654435761u);
        H ^= Mix((uint32)(MaxX + 0x7FFFFFFF) * 374761393u);
        H ^= Mix((uint32)(MaxY + 0x7FFFFFFF) * 668265263u);
        return Mix(H);
    }

    // Convert a hash to a float in [0, 1) — uniform distribution
    FORCEINLINE float ToFloat01(uint32 Hash)
    {
        return (float)(Hash & 0xFFFFFF) / (float)0x1000000;
    }

    // Convert a hash to a float in [-1, 1) — for centering
    FORCEINLINE float ToFloatSigned(uint32 Hash)
    {
        return ToFloat01(Hash) * 2.0f - 1.0f;
    }
}

//=============================================================================
// MAZE TOPOLOGY — ORIGIN-DIRECTED SPANNING TREE
//=============================================================================
// A Maze node never waits for a probabilistic edge to connect it to the network. Every node
// except the origin chooses exactly one parent among the coordinate axes that point toward the
// origin. The chosen parent lowers |X|+|Y|+|Z| by one, so following parents terminates at (0,0,0)
// and the undirected parent edges are a spanning tree of the infinite lattice.
//
// The optional loop rolls below are deliberately capped and are never used for the parent edge.
// They add alternate routes and visual irregularity, but deleting every loop still leaves the
// complete tree. This is the locality contract: the parent is a function of the child node and
// seed only; an emitted voxel cell needs the eight child nodes in its one-cell {-1,0} halo.
namespace VoxelMazeTopology
{
    enum class EAxis : uint8
    {
        X = 0,
        Y = 1,
        Z = 2,
    };

    // These are authoring knobs for optional loops, not connectivity probabilities. The scales
    // keep old authored BranchProbability/Verticality values useful without allowing a lattice
    // full of redundant parallel edges to turn the tree back into a grid.
    constexpr float HorizontalLoopScale = 0.18f;
    constexpr float VerticalLoopScale   = 0.10f;

    FORCEINLINE uint32 NodeHash(int32 X, int32 Y, int32 Z, uint32 Salt, uint32 DomainSalt)
    {
        uint32 H = VoxelHash::Cell(X, Y, Salt ^ DomainSalt);
        H ^= VoxelHash::Mix(static_cast<uint32>(Z) * 73856093u ^ DomainSalt);
        return VoxelHash::Mix(H);
    }

    FORCEINLINE uint32 AxisSalt(EAxis Axis)
    {
        switch (Axis)
        {
        case EAxis::X: return 0xA1u;
        case EAxis::Y: return 0xB2u;
        case EAxis::Z: return 0xC3u;
        default:       return 0xA1u;
        }
    }

    FORCEINLINE bool TryGetParent(int32 X, int32 Y, int32 Z, uint32 Salt,
                                  FIntVector& OutParent)
    {
        if (X == 0 && Y == 0 && Z == 0)
        {
            return false;
        }

        EAxis Available[3];
        int32 Steps[3];
        int32 NumAvailable = 0;
        if (X != 0)
        {
            Available[NumAvailable] = EAxis::X;
            Steps[NumAvailable++] = X > 0 ? -1 : 1;
        }
        if (Y != 0)
        {
            Available[NumAvailable] = EAxis::Y;
            Steps[NumAvailable++] = Y > 0 ? -1 : 1;
        }
        if (Z != 0)
        {
            Available[NumAvailable] = EAxis::Z;
            Steps[NumAvailable++] = Z > 0 ? -1 : 1;
        }

        const uint32 H = NodeHash(X, Y, Z, Salt, 0xD4E1A5E1u);
        const int32 ParentIndex = static_cast<int32>(H % static_cast<uint32>(NumAvailable));
        const EAxis ParentAxis = Available[ParentIndex];
        const int32 ParentStep = Steps[ParentIndex];
        OutParent = FIntVector(X, Y, Z);
        switch (ParentAxis)
        {
        case EAxis::X: OutParent.X += ParentStep; break;
        case EAxis::Y: OutParent.Y += ParentStep; break;
        case EAxis::Z: OutParent.Z += ParentStep; break;
        default: break;
        }
        return true;
    }

    FORCEINLINE FIntVector AxisNeighbour(int32 X, int32 Y, int32 Z, EAxis Axis)
    {
        switch (Axis)
        {
        case EAxis::X: return FIntVector(X + 1, Y, Z);
        case EAxis::Y: return FIntVector(X, Y + 1, Z);
        case EAxis::Z: return FIntVector(X, Y, Z + 1);
        default:       return FIntVector(X, Y, Z);
        }
    }

    FORCEINLINE bool IsTreeEdge(int32 X, int32 Y, int32 Z, EAxis Axis, uint32 Salt)
    {
        const FIntVector A(X, Y, Z);
        const FIntVector B = AxisNeighbour(X, Y, Z, Axis);
        FIntVector Parent;
        return (TryGetParent(A.X, A.Y, A.Z, Salt, Parent) && Parent == B)
            || (TryGetParent(B.X, B.Y, B.Z, Salt, Parent) && Parent == A);
    }

    FORCEINLINE float LoopProbability(EAxis Axis, float BranchProbability, float Verticality)
    {
        return Axis == EAxis::Z
            ? FMath::Clamp(Verticality, 0.0f, 1.0f) * VerticalLoopScale
            : FMath::Clamp(BranchProbability, 0.0f, 1.0f) * HorizontalLoopScale;
    }

    FORCEINLINE bool IsLoopEdgeOpen(int32 X, int32 Y, int32 Z, EAxis Axis, uint32 Salt,
                                    float BranchProbability, float Verticality)
    {
        if (IsTreeEdge(X, Y, Z, Axis, Salt))
        {
            return false;
        }

        const float Probability = LoopProbability(Axis, BranchProbability, Verticality);
        return Probability > 0.0f
            && VoxelHash::ToFloat01(NodeHash(X, Y, Z, Salt, AxisSalt(Axis))) < Probability;
    }

    FORCEINLINE bool IsOpenEdge(int32 X, int32 Y, int32 Z, EAxis Axis, uint32 Salt,
                                float BranchProbability, float Verticality)
    {
        return IsTreeEdge(X, Y, Z, Axis, Salt)
            || IsLoopEdgeOpen(X, Y, Z, Axis, Salt, BranchProbability, Verticality);
    }
}

//=============================================================================
// BRUIT CELLULAIRE / CELLULAR (WORLEY) NOISE — 3D
//=============================================================================
// ⚠️ POURQUOI CE CORPS VIT ICI ET NON DANS VoxelNoise.h.
// Il a besoin de `VoxelHash::Mix` / `ToFloat01`, qui vivent dans CE fichier. Faire dépendre
// VoxelNoise.h (le socle bas niveau, inclus partout) du header de morphologie de grotte serait une
// inversion de dépendance ; dupliquer les 50 lignes serait un FORK d'une fonction pure — exactement
// le motif qui a produit `AUDIT §C1` (un correctif appliqué à une copie sur deux). Il monte donc au
// point le plus bas qui voit déjà le hash, et le générateur comme la pile d'opérateurs l'appellent.
//
// Ce corps était `static float CellularNoise3D(const FVector&)` dans VoxelGenerator.cpp, invisible
// à la pile d'opérateurs. Il vit ici pour que les deux chemins partagent la même implémentation.
// The hot overload uses FVector3f: noise coordinates are floats, so no double round-trip is paid
// before the hash/lattice arithmetic. Output compatibility with the old field is not a contract.
//
// Algorithme : distance au point-feature le plus proche dans une grille hachée.
//   1. cellule entière du point   2. voisinage 3×3×3   3. rendre (F2 − F1), normalisé ~[-1, 1]
// F2−F1 donne des frontières de cellules lisses avec des arêtes entre elles.
namespace VoxelNoise
{
    FORCEINLINE float Cellular3D(const FVector3f& Position)
    {
        // Integer cell coordinates
        int32 CellX = FMath::FloorToInt(Position.X);
        int32 CellY = FMath::FloorToInt(Position.Y);
        int32 CellZ = FMath::FloorToInt(Position.Z);

        // Fractional position within cell
        float FracX = Position.X - CellX;
        float FracY = Position.Y - CellY;
        float FracZ = Position.Z - CellZ;

        float F1 = FLT_MAX;  // Distance to nearest feature point
        float F2 = FLT_MAX;  // Distance to 2nd nearest

        // Search 3x3x3 neighborhood
        for (int32 DZ = -1; DZ <= 1; DZ++)
        {
            for (int32 DY = -1; DY <= 1; DY++)
            {
                for (int32 DX = -1; DX <= 1; DX++)
                {
                    int32 NX = CellX + DX;
                    int32 NY = CellY + DY;
                    int32 NZ = CellZ + DZ;

                    // Hash the neighbor cell to get a feature point position [0,1)
                    // Using three different hash mixes for X, Y, Z offsets
                    uint32 H = VoxelHash::Mix(
                        (uint32)(NX + 0x7FFFFFFF)
                        ^ VoxelHash::Mix((uint32)(NY + 0x7FFFFFFF) * 2654435761u)
                        ^ VoxelHash::Mix((uint32)(NZ + 0x7FFFFFFF) * 374761393u)
                    );

                    float FPX = (float)DX + VoxelHash::ToFloat01(H) - FracX;
                    float FPY = (float)DY + VoxelHash::ToFloat01(VoxelHash::Mix(H ^ 0x12345678u)) - FracY;
                    float FPZ = (float)DZ + VoxelHash::ToFloat01(VoxelHash::Mix(H ^ 0x9ABCDEF0u)) - FracZ;

                    float DistSq = FPX * FPX + FPY * FPY + FPZ * FPZ;

                    // Track closest two distances
                    if (DistSq < F1)
                    {
                        F2 = F1;
                        F1 = DistSq;
                    }
                    else if (DistSq < F2)
                    {
                        F2 = DistSq;
                    }
                }
            }
        }

        // F2 - F1: smooth cell boundaries with ridges between cells
        // Sqrt for actual distance, then normalize to ~[-1, 1]
        float Result = FMath::Sqrt(F2) - FMath::Sqrt(F1);
        // Result is in [0, ~1.0]. Map to [-1, 1] for compatibility with other noise types.
        return Result * 2.0f - 1.0f;
    }

    FORCEINLINE float Cellular3D(const FVector& Position)
    {
        return Cellular3D(FVector3f((float)Position.X, (float)Position.Y, (float)Position.Z));
    }
}

//=============================================================================
// PER-CHUNK SDF CACHE
//=============================================================================
// The room list and tunnel connections are IDENTICAL for all voxels in a chunk.
// Without caching, EvaluateSDF rebuilds these 32,768 times per 32³ chunk:
//   - Hash every grid cell in the search area
//   - Determine room existence, position, size
//   - Run O(N²) nearest-neighbor backbone
//   - Decide O(N²) tunnel connections with hash lookups
//   - Compute tunnel radii, floor-aligned endpoints, and wandering control chains
//
// With caching: build once, evaluate 32K times with just SDF math.
// This is the single biggest CPU performance win for cave generation.

// Room-relative detail geometry whose hash/trigonometry is constant for a cached room.
// These are built once with the room's terrain-op parameters and then only evaluated per voxel.
struct FCachedArch
{
    bool bActive = false;
    FVector EndpointA = FVector::ZeroVector;
    FVector EndpointB = FVector::ZeroVector;
    float Radius = 0.0f;
    float BaseDensity = 0.0f;
};

struct FCachedPinch
{
    bool bActive = false;
    float CenterX = 0.0f;
    float CenterY = 0.0f;
    float CenterZ = 0.0f;
    float CosAngle = 1.0f;
    float SinAngle = 0.0f;
    float MaxExtent = 0.0f;
    float HalfLength = 0.0f;
    float HalfNarrow = 0.0f;
    float HalfVertical = 0.0f;
    float BaseDensity = 0.0f;
};

// A build-time room-floor bench authored from one tunnel mouth.  The room evaluator only reads
// these immutable descriptors; it never searches the graph or decides a correction per sample.
struct FCachedRoomMouthRise
{
    FVector Mouth = FVector::ZeroVector;
    float TargetFloorZ = 0.0f;
    float BlendRadius = 8.0f;
    float Strength = 0.0f;
};

// A room in the cache — everything needed for per-voxel SDF evaluation.
// Internal details (CellX, CellY) used during cache building are NOT stored here.
struct FCachedRoom
{
    FVector Center;       // World position of room center
    float RadiusXY;       // Horizontal radius (used for SDF + shape selection)
    float RadiusZ;        // Vertical radius (usually squished: RadiusXY * HeightRatio)
    uint32 Hash;          // Cell hash — used for deterministic shape selection per voxel
    bool bIsOrigin;       // True = origin room at (0,0), always uses ellipsoid shape
    float CullRadiusSq;   // Squared distance beyond which this room can't affect a voxel

    // Flat floor cut: base world Z of the soft floor plane.
    // Applied via SmoothMax so tunnels/pits pass through without a hard lip seam.
    // Set to -FLT_MAX when the hash-rolled floor cut = 1.0 (full bubble, no cut).
    float FloorCutZ;

    // Floor relief: noise amplitude (voxels) and frequency for floor undulation.
    // 0 strength = perfectly flat floor (just the cut plane, no variation).
    // Stored per-room so EvaluateSDFCached can apply it without a params lookup.
    float FloorReliefStrength;
    float FloorReliefFrequency;
    uint32 FloorSeed;  // Deterministic seed offset for this room's floor noise

    // Optional build-time floor benches leading to higher tunnel mouths.  Empty in the default
    // compatibility profile, so the baseline field is unchanged until an archetype opts in.
    TArray<FCachedRoomMouthRise> MouthRises;

    // Per-room terrain operation, hash-rolled during BuildChunkCache from the
    // strate's probability pool (FStrateTerrainOpEntry::Probability).
    // null = this room has no terrain op (the "no op" slot in the probability pool).
    // Raw pointer is safe — assets stay loaded for the entire generation session.
    const UVoxelTerrainOpDefinition* RoomOp = nullptr;

    // Intensity scale for this room's op (from FStrateTerrainOpEntry::Weight).
    // 1.0 = use op as configured, 0.5 = half intensity, 2.0 = double.
    float RoomOpWeight = 1.0f;

    // PRE-BAKED SHAPE (BuildChunkCache). The shape roll, variety thresholds and the capsule's
    // Cos/Sin direction used to be re-derived PER VOXEL per room inside EvaluateSDFCached — hash
    // mixes + trig in the hottest loop of the plugin for values that are constants of the room.
    // Bit-identical to the old per-voxel roll (same hashes, same math, done once per chunk).
    //   0 = ellipsoid    → ShapeA = radii (x=y=RadiusXY, z=RadiusZ)
    //   1 = rounded box  → ShapeA = half-extents, ShapeR = corner rounding
    //   2 = capsule      → ShapeA/ShapeB = world endpoints, ShapeR = tube radius
    uint8   ShapeType = 0;
    FVector ShapeA = FVector::ZeroVector;
    FVector ShapeB = FVector::ZeroVector;
    float   ShapeR = 0.0f;

    // Hash-derived room details.  The old evaluator rebuilt these hashes and angles for every
    // surface sample; the cache build is the only place where their geometry can change.
    FCachedArch  Arches[3];
    FCachedPinch Pinches[3];
};

// A local flat-floor bridge over the overlap of two room bodies. It is not a new graph edge: it
// only gives an existing spherical-room intersection one shared walkable floor level, removing
// the high lip that otherwise appears when the two room floor cuts disagree.
struct FCachedRoomFloorJoin
{
    FVector Start;
    FVector End;
    float Radius = 0.0f;
    float FloorZ = 0.0f;
    float CeilingZ = 0.0f;
    FVector BoundCenter;
    float BoundRadiusSq = 0.0f;
};

// One immutable floor segment authored while the tunnel chain is built. The floor is continuous
// unless bLedgeTransition is set; that marker means the segment terminates at a deliberate
// dramatic ledge and the vertical riser is placed at the next control-point boundary. NumSteps is
// retained as a compatibility/diagnostic field and is always zero for newly authored tunnels: a
// steep floor is never quantised into a staircase. ReliefScale is baked with a derivative bound,
// so evaluation only reads this profile and never decides a floor shape per voxel.
struct FTunnelFloorSegmentProfile
{
    float StartFloorZ = 0.0f;
    float EndFloorZ = 0.0f;
    // The unmodified tapered-capsule bottom at the same two control points.  Keeping these
    // alongside the authored floor lets evaluation translate the arch to the baked floor without
    // re-deriving a second profile (and makes the build-time floor/arch relationship explicit).
    float NaturalStartFloorZ = 0.0f;
    float NaturalEndFloorZ = 0.0f;
    float ReliefScale = 0.0f;
    bool bLedgeTransition = false;
    // Deprecated staircase count. New profiles leave this at zero; keep the member so old
    // diagnostic consumers can still compile while the authored representation has one meaning.
    int32 NumSteps = 0;
};

// A pre-computed wandering tunnel chain — all connection decisions and hash-derived properties
// (radii, Z offsets, control points) are resolved during cache build.
struct FCachedTunnel
{
    FVector EndpointA;    // First room's floor-tangent connection point in SDF coordinates
    FVector EndpointB;    // Second room's floor-tangent connection point in SDF coordinates
    float RadiusA;        // Tube radius at endpoint A
    float RadiusB;        // Tube radius at endpoint B
    // Compatibility summary of the chain's first/middle/last points. These fields are retained
    // for diagnostics and old cache consumers; EvaluateSDFCached uses ControlPoints when present.
    FVector Midpoint;
    float RadiusMid;
    bool bHasMidpoint;
    TArray<FVector> ControlPoints;
    TArray<float> ControlRadii;
    // World-space copy used by the final structural air backstop. Evaluating this copy avoids
    // turning a gentle authored floor ramp into a vertical break when the cave warp is nonlinear.
    TArray<FVector> WorldControlPoints;
    TArray<float> WorldControlRadii;
    // Build-time floor authoring. SDF and world chains each keep their own profile because cave
    // warp changes the control-point Z values. The normal path has one entry per chain segment;
    // malformed/legacy caches fall back to the old local evaluator.
    TArray<FTunnelFloorSegmentProfile> FloorProfiles;
    TArray<FTunnelFloorSegmentProfile> WorldFloorProfiles;
    // For a dramatic ledge, these monotonic levels identify the control-point boundaries at which
    // the few large drops occur. Both SDF and world profiles share the boundaries, while each
    // coordinate space supplies its own endpoint heights.
    TArray<int32> FloorLedgeLevels;
    bool bHasCompleteFloorProfile = false;
    bool bHasCompleteWorldFloorProfile = false;
    // The corridor floor is a swept SmoothMax cut, not a later slab.  These are copied from the
    // same strate fields used by room floors; the seed is derived once from the tunnel pair hash.
    float FloorReliefStrength = 0.0f;
    float FloorReliefFrequency = 0.015f;
    uint32 FloorSeed = 0;
    // Only the two rooms that actually form this edge may own its mouth floor. A nearby unrelated
    // room must not clip a tunnel merely because the two shapes overlap in a crowded graph.
    bool bHasFloorRoomOwnership = false;
    uint32 FloorRoomHashA = 0;
    uint32 FloorRoomHashB = 0;
    // World-space mouth ownership is resolved from the baked chain, not by comparing a warped
    // query against an SDF-space room. These values make the post-disturbance hand-off exact in
    // the same coordinate space in which the player and the support backstop are evaluated.
    // The room's full radius is deliberately not used as the ownership region.  A tunnel's
    // floor should hand off to the room over a short mouth apron, not overwrite the room floor
    // across the entire chamber.  These are the build-time capped hand-off radii in both query
    // coordinate spaces.
    float SDFMouthBlendRadiusA = 0.0f;
    float SDFMouthBlendRadiusB = 0.0f;
    float WorldMouthBlendRadiusA = 0.0f;
    float WorldMouthBlendRadiusB = 0.0f;
    float SDFMouthFloorZA = -FLT_MAX;
    float SDFMouthFloorZB = -FLT_MAX;
    float WorldMouthFloorZA = -FLT_MAX;
    float WorldMouthFloorZB = -FLT_MAX;
    bool bTunnelFloorTerracingEnabled = true;
    float TunnelFloorTerraceStepHeight = 1.71f;
    float TunnelFloorMaxLedgeHeight = 12.0f;
    // Cached internal gradient derived from the authored degree value. This is not serialized.
    float TunnelFloorGentleSlopeGradient = 0.9656888f;
    int32 TunnelFloorLedgeCountPreference = 0;
    int32 TunnelFloorMaxLedges = 4096;
    bool bDramaticLedge = false;
    bool bLedgeGraphEligible = false;
    bool bFloorRouteWasWound = false;
    int32 TunnelFloorAuthoredLedgeCount = 0;
    FVector WorldBoundCenter = FVector::ZeroVector;
    float WorldBoundRadiusSq = 0.0f;
    // Centerline AABBs and scalar influence radii used by the immutable broad phase.  The
    // AABB test is a cheap lower bound on distance to the whole chain; the existing squared
    // sphere test remains the exact conservative second-stage reject.
    FVector SDFCenterlineMin = FVector::ZeroVector;
    FVector SDFCenterlineMax = FVector::ZeroVector;
    float SDFInfluenceRadius = 0.0f;
    FVector WorldCenterlineMin = FVector::ZeroVector;
    FVector WorldCenterlineMax = FVector::ZeroVector;
    float WorldInfluenceRadius = 0.0f;
    // Bounding sphere for quick per-voxel rejection
    FVector BoundCenter;  // Center of the bounding sphere
    float BoundRadiusSq;  // Squared radius — if voxel is further, skip this tunnel
};

struct FTunnelSupportFloorInterval
{
    int32 TunnelIndex = INDEX_NONE;
    float FloorZ = 0.0f;
    float MinZ = 0.0f;
    float MaxZ = 0.0f;
};

struct FTunnelSupportFloorColumn
{
    TArray<FTunnelSupportFloorInterval, TInlineAllocator<16>> Intervals;

    FTunnelSupportFloorColumn() = default;

    FTunnelSupportFloorColumn(const FTunnelSupportFloorColumn& Other)
        : Intervals(Other.Intervals)
    {
    }

    FTunnelSupportFloorColumn& operator=(const FTunnelSupportFloorColumn& Other)
    {
        Intervals = Other.Intervals;
        return *this;
    }

    FTunnelSupportFloorColumn(FTunnelSupportFloorColumn&& Other)
        : Intervals(MoveTemp(Other.Intervals))
    {
    }

    FTunnelSupportFloorColumn& operator=(FTunnelSupportFloorColumn&& Other)
    {
        Intervals = MoveTemp(Other.Intervals);
        return *this;
    }

    void Reset()
    {
        Intervals.Reset();
    }
};

// A pre-baked pit shaft — position and dimensions resolved during BuildChunkCache.
//
// Pits are included in the main SDF evaluation (SmoothMin alongside rooms and
// tunnels) rather than as a separate density subtraction. This lets SmoothMin
// handle the pit-to-room junction organically — same mechanism as tunnel junctions.
// No more hard seam at PitTopZ. BlendK drives the junction roundness and comes
// from the strate's SDFBlendRadius, making it tweakable in the data asset.
struct FCachedPit
{
    float CenterX, CenterY;    // XY center in world coords (unwarped)
    float TopZ;                // World Z where the pit starts (anchored inside room air)
    float Radius;              // Shaft cylinder radius
    float Depth;               // Max depth the pit reaches below TopZ
    float FlareDist;           // Over how many voxels the opening flares (Radius * 2)
    float FlareExtra;          // Extra radius at the lip (Radius * 1)
    float BaseDensity;         // Carving strength — matches the strate's BaseDensity
    float BlendK;              // SmoothMin blend radius — set from Params.SDFBlendRadius
    float BoundXYRadiusSq;     // XY rejection: skip if (dx^2+dy^2) > this
};

// A pre-baked chimney shaft — inverse of a pit (carves upward from room ceiling).
struct FCachedChimney
{
    float CenterX, CenterY;
    float BottomZ;             // World Z where the chimney starts (anchored inside room air)
    float Radius;
    float Height;              // How far the chimney reaches above BottomZ
    float FlareDist;
    float FlareExtra;
    float BaseDensity;
    float BlendK;              // SmoothMin blend radius — set from Params.SDFBlendRadius
    float BoundXYRadiusSq;
};

// A pre-baked column — vertical solid cylinder inside a room.
// Pre-baking fixes the NearestRoomIdx ownership issue for columns too:
// a column's XY position is fixed, but which room "owns" the voxel can change
// with depth, which would cause columns to appear/disappear mid-height.
struct FCachedColumn
{
    float CenterX, CenterY;
    float Radius;
    float BaseDensity;
    float BoundXYRadiusSq;
};

// Build-site attribution is diagnostic only. It is deliberately passed to BuildChunkCache
// rather than inferred from the caller's stack so the game report can separate the generator,
// ordinary op-source, shared op-source, and classifier fallback windows without changing the
// cache key or the generated field.
enum class ERoomGraphBuildSite : uint8
{
    Unknown,
    GeneratorTile,
    GeneratorTunnelCore,
    OpShared,
    OpLocal,
    ClassifierShared,
    ClassifierLocal,
};

// Immutable XY broad phase for one cached primitive array.  Each bucket stores source-array
// indices in ascending order, so switching from a full scan to a bucket scan cannot change the
// order of SmoothMin/Min reductions or the selected nearest-room owner.  Invalid/oversized
// bounds leave bValid false and callers deliberately fall back to the exact full scan.
struct FChunkSDFSpatialIndex
{
    int32 MinChunkX = 0;
    int32 MinChunkY = 0;
    int32 NumCellsX = 0;
    int32 NumCellsY = 0;
    bool bValid = false;
    TArray<int32> CellOffsets;
    TArray<int32> ItemIndices;

    void Reset();
    void Release();
    SIZE_T GetAllocatedSize() const;
    bool GetRange(float WorldX, float WorldY, int32& OutBegin, int32& OutEnd) const;
};

template <typename FVisit>
FORCEINLINE int32 VF_ForEachChunkSDFSpatialCandidate(
    const FChunkSDFSpatialIndex& Index, int32 ItemCount,
    float WorldX, float WorldY, FVisit&& Visit,
    bool bUseSpatialIndex = true)
{
    int32 Begin = 0;
    int32 End = 0;
    int32 CandidateCount = 0;
    if (bUseSpatialIndex && Index.GetRange(WorldX, WorldY, Begin, End))
    {
        for (int32 Cursor = Begin; Cursor < End; ++Cursor)
        {
            const int32 ItemIndex = Index.ItemIndices[Cursor];
            if (ItemIndex >= 0 && ItemIndex < ItemCount)
            {
                ++CandidateCount;
                Visit(ItemIndex);
            }
        }
        return CandidateCount;
    }

    for (int32 ItemIndex = 0; ItemIndex < ItemCount; ++ItemIndex)
    {
        ++CandidateCount;
        Visit(ItemIndex);
    }
    return CandidateCount;
}

// The complete SDF cache for a chunk region.
// Built once per chunk by BuildChunkCache(), then passed to EvaluateSDFCached()
// for every voxel in the chunk. Typically contains 5-15 rooms and 10-30 tunnels.
struct FChunkSDFCache
{
    TArray<FCachedRoom>          Rooms;
    TArray<FCachedRoomFloorJoin> RoomFloorJoins;
    TArray<FCachedTunnel>  Tunnels;
    TArray<FCachedPit>     Pits;
    TArray<FCachedChimney> Chimneys;
    TArray<FCachedColumn>  Columns;

    // Per-primitive XY filters.  The bounds are conservative unions of every exact evaluator
    // broad phase, so a failed index build is a performance fallback, never a density fallback.
    FChunkSDFSpatialIndex RoomSpatialIndex;
    FChunkSDFSpatialIndex RoomFloorJoinSpatialIndex;
    FChunkSDFSpatialIndex TunnelSpatialIndex;
    FChunkSDFSpatialIndex PitSpatialIndex;
    FChunkSDFSpatialIndex ChimneySpatialIndex;
    FChunkSDFSpatialIndex ColumnSpatialIndex;

    // Kept with the cache because the world-space structural evaluator has no params argument.
    // A zero value is a valid fallback for legacy/empty caches.
    float SDFBlendRadius = 0.0f;

    // Allocator-backed bytes owned by this cache, including the nested tunnel chains. The returned
    // value excludes sizeof(FChunkSDFCache) itself so callers can add the enclosing entry's inline
    // storage exactly once.
    // Reset keeps the allocated backing storage and clears the immutable contents.
    // It is used when a classifier proves that the current search window has no
    // possible room/tunnel feature.
    void Reset();
    // Release all backing storage. Used when a worker has moved from a populated
    // classifier window to a feature-free one and the retained capacity is no
    // longer useful; this is intentionally separate from hot-path Reset().
    void Release();
    SIZE_T GetAllocatedSize() const;
    VoxelDensityProfile::FCacheMemoryBreakdown GetAllocatedSizeBreakdown() const;
};

/** World-space tunnel shape query. Legacy support-floor fields remain source-compatible but are
 * no longer produced or consumed by the graph-tunnel generation path. */
struct FTunnelCoreWorldEvaluation
{
    float SDF = FLT_MAX;
    // Deprecated compatibility output. The separate graph-tunnel support slab was removed.
    bool bSupportFloor = false;
    // Room geometry owns the lower side while the world-space tunnel is inside a room. The
    // tunnel SDF may still reopen air there, but its swept bottom must not turn that room air back
    // into a shelf or wall.
    bool bRoomFloor = false;
    // Shape-level floor ownership. This is not an authored slab: it identifies the bottom side
    // of the swept tunnel SDF so a room that is already air cannot erase the tunnel's own floor.
    bool bHasSweptFloor = false;
    float SweptFloorZ = -FLT_MAX;
    float SweptFloorRadius = 0.0f;
};

//=============================================================================
// CAVE MORPHOLOGY EVALUATOR
//=============================================================================
// Two-phase evaluation: BuildChunkCache (once per chunk) + EvaluateSDFCached (per voxel).
// The original EvaluateSDF is kept as a convenience wrapper for backward compatibility.

namespace VoxelCaveMorphology
{
    // The player-fit memo is process-wide and uses a fixed-size table. These controls are kept
    // beside the cave source so the command-line value is applied before generation workers start,
    // while the CVar remains available for an in-process A/B switch.
    VOXELFORGE_API void ConfigurePlayerFitMemoFromCommandLine();
    VOXELFORGE_API void ResetPlayerFitMemoStats();
    VOXELFORGE_API void LogPlayerFitMemoStats();

    // The room graph's seed is the world seed salted by the strate index. Keep this formula
    // shared by BuildChunkCache and landing-site queries so a query cannot inspect another
    // strate's room layout. / La seed du graphe des salles est la seed monde salée par l'index
    // de strate. La formule est partagée entre BuildChunkCache et les requêtes d'atterrissage.
    FORCEINLINE uint32 MakeStrateSeed(uint32 WorldSeed, int32 StrateIndex)
    {
        return VoxelHash::Mix(WorldSeed ^ (uint32)(StrateIndex * 7919 + 104729));
    }

    // Map a world-space query to the SDF coordinate space used by the room/tunnel cache. Keeping
    // this transform in the morphology module lets the generator's post-disturbance structural
    // check share the exact same warp as the source and the landing query.
    VOXELFORGE_API FVector ApplyCaveWarp(
        const FVector& WorldPoint,
        const FStrateGenerationParams& Params,
        uint32 Seed
    );

    // PHASE 1: Build the SDF cache for a rectangular region.
    // Collects all rooms, computes nearest-neighbor backbone, decides tunnel
    // connections, and pre-computes all tunnel geometry (radii, offsets, midpoints).
    // Also hash-rolls a terrain op per room from the optional probability pool.
    //
    // Call once per chunk before the voxel loop. The region bounds should cover
    // the chunk's XY extent PLUS CaveWarpStrength (warped queries can shift)
    // PLUS a small margin for gradient normal sampling (~2 voxels).
    // MaxInfluence (room/tunnel reach) is computed internally from Params.
    //
    // @param OutCache     — filled with rooms and tunnels for this region
    // @param SearchMinX/Y, SearchMaxX/Y — XY world bounds to search (expanded by caller)
    // @param Params       — strate generation parameters
    // @param Seed         — world seed
    // @param StrateIndex  — which strate (offsets hash for variety)
    // @param TerrainOps   — optional probability pool; null = no per-room ops
    VOXELFORGE_API void BuildChunkCache(
        FChunkSDFCache& OutCache,
        float SearchMinX, float SearchMinY,
        float SearchMaxX, float SearchMaxY,
        const FStrateGenerationParams& Params,
        uint32 Seed, int32 StrateIndex,
        const TArray<FStrateTerrainOpEntry>* TerrainOps = nullptr,
        ERoomGraphBuildSite BuildSite = ERoomGraphBuildSite::Unknown
    );

    // Conservative, geometry-free preflight for BuildChunkCache. It recreates
    // the deterministic room candidates and edge decisions, but does not build
    // player-fit points or tunnel control chains. False means that the same
    // cache window is guaranteed to contain no room, join, decoration, or
    // connected tunnel. Any malformed or unbounded input returns true.
    VOXELFORGE_API bool MayHaveFeatureInSearchBox(
        float SearchMinX, float SearchMinY,
        float SearchMaxX, float SearchMaxY,
        const FStrateGenerationParams& Params,
        uint32 Seed, int32 StrateIndex,
        bool bUseZ = false,
        float SearchMinZ = 0.0f, float SearchMaxZ = 0.0f,
        const TArray<FStrateTerrainOpEntry>* TerrainOps = nullptr
    );

    // True only for the finite parameter envelope for which BuildChunkCache's fixed collect
    // margin proves window invariance. Callers may widen the XY store window to a whole tile only
    // when this returns true; malformed or unbounded authored input must retain the legacy path.
    VOXELFORGE_API bool IsRoomGraphWindowInvariant(
        const FStrateGenerationParams& Params,
        const TArray<FStrateTerrainOpEntry>* TerrainOps = nullptr
    );

    // PHASE 2: Evaluate the SDF at a single world position using cached data.
    // Loops through cached rooms and tunnels, computes SDF primitives, applies
    // SmoothMin. Includes per-room/tunnel distance culling to skip primitives
    // that are too far to contribute.
    //
    // @param WorldX, WorldY, WorldZ  — room/query position in voxel coordinates (may be warped)
    // @param Cache                   — pre-built cache from BuildChunkCache
    // @param SDFBlendRadius          — SmoothMin blend radius (from Params.SDFBlendRadius)
    // @param OutNearestRoomIdx       — optional out: index of the room with minimum SDF
    //                                  contribution. -1 if no room passed the cull test.
    //                                  Used by the terrain ops system to look up the
    //                                  per-room terrain op assigned to this voxel's room.
    // @param WorldTunnelPosition    — optional unwarped position for the tunnel primitive. When
    //                                  supplied, rooms remain at the query position but the tunnel
    //                                  uses its authored world chain. Null preserves the legacy
    //                                  all-query behavior.
    // @param OutRoomOwnsBottom      — optional out: true when the query position is inside any
    //                                  cached room's raw authored shape. When the caller evaluates
    //                                  the tunnel core for this same sample, pass this result back
    //                                  to EvaluateTunnelCoreWorld to avoid a second room query.
    // (Room shape variety is baked into FCachedRoom by BuildChunkCache — no per-voxel roll.)
    // @return negative = inside cave, positive = solid rock
    VOXELFORGE_API float EvaluateSDFCached(
        float WorldX, float WorldY, float WorldZ,
        const FChunkSDFCache& Cache,
        float SDFBlendRadius,
        int32* OutNearestRoomIdx = nullptr,
        bool bUseSpatialIndex = true,
        const FVector* WorldTunnelPosition = nullptr,
        bool* OutRoomOwnsBottom = nullptr
    );

    // Evaluate the raw union of cached graph-tunnel capsules. This is intentionally separate from
    // the SmoothMin morphology: the generator uses it after terrain/passage posts to reassert the
    // tunnel's walkable air core without reintroducing a connector or changing room ownership.
    VOXELFORGE_API float EvaluateTunnelCoreSDF(
        float WorldX, float WorldY, float WorldZ,
        const FChunkSDFCache& Cache,
        bool bUseSpatialIndex = true
    );

    // Evaluate the final structural air contract against the world-space wandering chain. Room
    // SDFs remain evaluated in warped coordinates; only this post uses world coordinates so its
    // walkable floor follows the authored chain exactly.
    VOXELFORGE_API float EvaluateTunnelCoreWorldSDF(
        float WorldX, float WorldY, float WorldZ,
        const FChunkSDFCache& Cache,
        bool bUseSpatialIndex = true
    );

    VOXELFORGE_API FTunnelCoreWorldEvaluation EvaluateTunnelCoreWorld(
        float WorldX, float WorldY, float WorldZ,
        const FChunkSDFCache& Cache,
        // Deprecated legacy argument; graph-tunnel evaluation ignores the old support column.
        const FTunnelSupportFloorColumn* SupportColumn = nullptr,
        bool bUseSpatialIndex = true,
        // The room graph is evaluated in warped coordinates while the tunnel core is evaluated in
        // the authored world frame. Pass the former when available so room ownership is tested in
        // the same domain as the generic room field.
        const FVector* RoomQueryPosition = nullptr,
        // Optional result from EvaluateSDFCached for this exact sample. When supplied, it avoids
        // repeating the room-shape containment query; null preserves standalone-call behavior.
        const bool* CachedRoomOwnsBottom = nullptr
    );

    // Per-tile broad reach for the world-space core/floor post.  The bound includes the swept
    // chain, floor band, relief envelope, and the finite room-mouth ownership apron.
    VOXELFORGE_API bool AnyTunnelCoreWorldNearLattice(
        const FChunkSDFCache& Cache,
        const FBox& VoxelBox,
        float ReachScale = 1.0f
    );

    // Deprecated compatibility query for the removed graph-tunnel support slab. Production
    // generation does not call this predicate; the tunnel's own swept shape owns its floor.
    VOXELFORGE_API bool IsTunnelSupportFloorWorldPoint(
        float WorldX, float WorldY, float WorldZ,
        const FChunkSDFCache& Cache,
        bool bUseSpatialIndex = true
    );

    // Deprecated compatibility helper for callers that still inspect the old support-band
    // diagnostic. It is detached from production density generation.
    VOXELFORGE_API void BuildTunnelSupportFloorColumn(
        float WorldX, float WorldY,
        const FChunkSDFCache& Cache,
        FTunnelSupportFloorColumn& OutColumn,
        bool bUseSpatialIndex = true
    );

    VOXELFORGE_API bool IsTunnelSupportFloorColumnZ(
        float WorldZ,
        const FTunnelSupportFloorColumn& Column
    );

    // Returns the legacy projected band for one tunnel at this XY column. The world evaluator no
    // longer uses it to write a support slab.
    VOXELFORGE_API bool GetTunnelSupportFloorColumnBand(
        int32 TunnelIndex,
        const FTunnelSupportFloorColumn& Column,
        float& OutFloorZ,
        float& OutMinZ,
        float& OutMaxZ
    );

    // CONVENIENCE WRAPPER: builds a temporary cache and evaluates in one call.
    // Use this for one-off queries (debug visualization, single-point sampling).
    // For chunk generation, use BuildChunkCache + EvaluateSDFCached instead.
    float EvaluateSDF(
        float WorldX, float WorldY, float WorldZ,
        const FStrateGenerationParams& Params,
        uint32 Seed, int32 StrateIndex
    );
}

/**
 * Suggest a deterministic player-fit landing point for an inter-strate passage mouth.
 *
 * This is deliberately a pure source query: it does not touch a generator, an operator stack,
 * a strate manager, a cache, or mutable state. DesiredX/DesiredY is the passage's deliberate
 * placement around the (0,0) spine. Sparse sources may move laterally, but never farther than
 * MaxLateralSnap; false means no confident player-fit pose exists inside that bound. A successful
 * point has a locally supported floor within player step height, a walkable support patch, and a
 * clear capsule volume. The local source stencil deliberately does not claim flood-fill
 * connectivity or a measured component size; it selects a topology-backed feature core instead.
 * For TunnelNetwork/Underwater, Seed must be the strate-specific room seed returned by
 * VoxelCaveMorphology::MakeStrateSeed; the optional WorldSeed selects the production cave-warp
 * field and should be the generator world seed. If omitted, the room seed is used for both for
 * backward-compatible standalone queries. For slab, maze, shaft, and island archetypes, Seed is the
 * generator world seed consumed by their source. SurfaceWorld deliberately has no answer here:
 * its production height can be selected by manager-owned biome context and per-chunk state,
 * which this re-entrant query cannot inspect.
 *
 * Propose un point d'atterrissage déterministe où la capsule du joueur tient réellement pour la
 * bouche d'un passage inter-strates. Cette requête reste pure et le déplacement latéral est
 * strictement borné par MaxLateralSnap; elle ne prétend pas prouver une composante connectée.
 * Pour VerticalShafts, la réponse s'appuie sur l'axe d'un puits sélectionné et peut se déplacer
 * vers une banquette déterministe (+X/+Y) pour obtenir un support; ce n'est pas un point ouvert
 * arbitraire à l'intérieur de son disque.
 *
 * @return true when the archetype can provide a confident point; false when it cannot.
 */
VOXELFORGE_API bool VF_SuggestLandingPoint(
    ECaveGeneratorType Archetype,
    const FStrateGenerationParams& CaveParams,
    const FSlabGenerationParams& SlabParams,
    int32 Seed,
    float StrateTopZ,
    float StrateBottomZ,
    float DesiredX,
    float DesiredY,
    float MaxLateralSnap,
    FVector& OutPoint,
    int32 WorldSeed = MIN_int32
);

/**
 * Complete player-fit source-query overload for archetypes whose placement parameters are not part of
 * FStrateGenerationParams/FSlabGenerationParams. The extra structs are read-only inputs only;
 * the function remains pure and SurfaceWorld still returns false for the reason above. The
 * returned point is either at the requested XY or within MaxLateralSnap of it, and is accepted
 * only after the local player capsule/support stencil passes. VerticalShafts uses a selected
 * shaft's deterministic feature core and ledge-side pose; the axis identifies the topology but
 * is not itself claimed to be a standable point.
 */
VOXELFORGE_API bool VF_SuggestLandingPoint(
    ECaveGeneratorType Archetype,
    const FStrateGenerationParams& CaveParams,
    const FSlabGenerationParams& SlabParams,
    const FMazeGenerationParams& MazeParams,
    const FVerticalShaftParams& VerticalShaftParams,
    const FFloatingIslandParams& FloatingIslandParams,
    int32 Seed,
    float StrateTopZ,
    float StrateBottomZ,
    float DesiredX,
    float DesiredY,
    float MaxLateralSnap,
    FVector& OutPoint,
    int32 WorldSeed = MIN_int32
);
