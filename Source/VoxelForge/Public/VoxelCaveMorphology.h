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
// - Origin room: guaranteed large room at (0,0) per strate — the hub
// - Hash rooms: ellipsoids, rounded boxes, or elongated capsules
// - All blended with smooth-min for organic junctions
//
// TUNNELS:
// - Connect pairs of rooms based on distance and probability
// - Tapered (variable min/max radius at each endpoint)
// - Curved (midpoint displaced perpendicular to tunnel direction)
// - Horizontal bias (vertical connections penalized)
// - Endpoint Z offset (tunnels enter rooms at different heights)
// - Origin room forces connections to all nearby rooms
//
// PIPELINE POSITION:
// Step 3 (cave warp) bends the SDF query coordinates before we evaluate.
// This step (Step 4) builds the SDF skeleton of rooms + tunnels.
// Surface roughness (Step 4b), terrain ops, and worm tunnels layer on top.

#pragma once

#include "CoreMinimal.h"

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
 * A source query answer selects the landing's XY/Z anchor.  Because that pure query does not
 * flood-fill the live network, every inter-strate end also gets a deterministic root connector
 * (`bHasNetworkConnector`).  Its flat floor ramps to the same common interior root level for that
 * strate, so both mouths enter one walkable hub even when their authored floors are at opposite
 * sides of the strate.  The hub is an annular walkable network around (0,0), not the pre-existing
 * vertical air shaft.  `RootSpineRadius` keeps both the hub air and support slab outside that shaft,
 * so the spine remains open.  If the direct run would be steeper than 44 degrees, the connector
 * uses a deterministic level dog-leg before its final ramp; the bend is geometry, not a per-voxel
 * search.
 */
struct VOXELFORGE_API FVoxelPassageLanding
{
    FVector StandingPoint = FVector::ZeroVector;
    FVector DoorPoint = FVector::ZeroVector;
    FVector DoorDirection = FVector(1.0f, 0.0f, 0.0f);

    FVector ConnectorStart = FVector::ZeroVector;
    FVector ConnectorControl = FVector::ZeroVector;
    FVector ConnectorEnd = FVector::ZeroVector;

    float FloorZ = 0.0f;
    float CeilingZ = 0.0f;
    float HalfWidth = 0.0f;
    float FloorThickness = 3.0f;
    float ConnectorRadius = 0.0f;
    float ConnectorCeilingZ = 0.0f;
    float RootFloorZ = 0.0f;
    float RootCeilingZ = 0.0f;
    float RootSpineRadius = 0.0f;

    bool bSourcePlayerFit = false;
    bool bHasNetworkConnector = false;
    bool bHasConnectorBend = false;
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
    bool bSourcePlayerFit,
    bool bHasNetworkConnector,
    const FVector& NetworkPoint,
    float RootSpineRadius = 0.0f);

/** Signed distance of the room + optional root connector. Negative means passage air. */
VOXELFORGE_API float VF_EvaluatePassageLandingSDF(
    const FVector& Position,
    const FVoxelPassageLanding& Landing);

/** True when a point belongs to the guaranteed solid floor slab of the landing/connector. */
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
// à la pile d'opérateurs. Déplacement LITTÉRAL : mêmes opérations, même ordre, même passage par
// `FVector` (donc par des doubles) — l'égalité binaire du portage TunnelNetwork en dépend.
// `UVoxelGenerator`'s copy is now a one-line forwarder; the body moved verbatim.
//
// Algorithme : distance au point-feature le plus proche dans une grille hachée.
//   1. cellule entière du point   2. voisinage 3×3×3   3. rendre (F2 − F1), normalisé ~[-1, 1]
// F2−F1 donne des frontières de cellules lisses avec des arêtes entre elles.
namespace VoxelNoise
{
    FORCEINLINE float Cellular3D(const FVector& Position)
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
//   - Compute tunnel radii, Z offsets, midpoint warping
//
// With caching: build once, evaluate 32K times with just SDF math.
// This is the single biggest CPU performance win for cave generation.

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
};

// A pre-computed tunnel segment — all connection decisions and hash-derived
// properties (radius, Z offset, midpoint warp) are resolved during cache build.
struct FCachedTunnel
{
    FVector EndpointA;    // First room's connection point (with Z offset)
    FVector EndpointB;    // Second room's connection point (with Z offset)
    float RadiusA;        // Tube radius at endpoint A
    float RadiusB;        // Tube radius at endpoint B
    // Warped midpoint — only used if bHasMidpoint is true.
    // Creates a two-segment curved path instead of a straight tube.
    FVector Midpoint;
    float RadiusMid;      // Radius at the midpoint (average of A and B)
    bool bHasMidpoint;    // True when TunnelWarpStrength > 0 and tunnel is long enough
    // Bounding sphere for quick per-voxel rejection
    FVector BoundCenter;  // Center of the bounding sphere
    float BoundRadiusSq;  // Squared radius — if voxel is further, skip this tunnel
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

// The complete SDF cache for a chunk region.
// Built once per chunk by BuildChunkCache(), then passed to EvaluateSDFCached()
// for every voxel in the chunk. Typically contains 5-15 rooms and 10-30 tunnels.
struct FChunkSDFCache
{
    TArray<FCachedRoom>    Rooms;
    TArray<FCachedTunnel>  Tunnels;
    TArray<FCachedPit>     Pits;
    TArray<FCachedChimney> Chimneys;
    TArray<FCachedColumn>  Columns;
};

//=============================================================================
// CAVE MORPHOLOGY EVALUATOR
//=============================================================================
// Two-phase evaluation: BuildChunkCache (once per chunk) + EvaluateSDFCached (per voxel).
// The original EvaluateSDF is kept as a convenience wrapper for backward compatibility.

namespace VoxelCaveMorphology
{
    // The room graph's seed is the world seed salted by the strate index. Keep this formula
    // shared by BuildChunkCache and landing-site queries so a query cannot inspect another
    // strate's room layout. / La seed du graphe des salles est la seed monde salée par l'index
    // de strate. La formule est partagée entre BuildChunkCache et les requêtes d'atterrissage.
    FORCEINLINE uint32 MakeStrateSeed(uint32 WorldSeed, int32 StrateIndex)
    {
        return VoxelHash::Mix(WorldSeed ^ (uint32)(StrateIndex * 7919 + 104729));
    }

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
    void BuildChunkCache(
        FChunkSDFCache& OutCache,
        float SearchMinX, float SearchMinY,
        float SearchMaxX, float SearchMaxY,
        const FStrateGenerationParams& Params,
        uint32 Seed, int32 StrateIndex,
        const TArray<FStrateTerrainOpEntry>* TerrainOps = nullptr
    );

    // PHASE 2: Evaluate the SDF at a single world position using cached data.
    // Loops through cached rooms and tunnels, computes SDF primitives, applies
    // SmoothMin. Includes per-room/tunnel distance culling to skip primitives
    // that are too far to contribute.
    //
    // @param WorldX, WorldY, WorldZ  — position in voxel coordinates (may be warped)
    // @param Cache                   — pre-built cache from BuildChunkCache
    // @param SDFBlendRadius          — SmoothMin blend radius (from Params.SDFBlendRadius)
    // @param OutNearestRoomIdx       — optional out: index of the room with minimum SDF
    //                                  contribution. -1 if no room passed the cull test.
    //                                  Used by the terrain ops system to look up the
    //                                  per-room terrain op assigned to this voxel's room.
    // (Room shape variety is baked into FCachedRoom by BuildChunkCache — no per-voxel roll.)
    // @return negative = inside cave, positive = solid rock
    float EvaluateSDFCached(
        float WorldX, float WorldY, float WorldZ,
        const FChunkSDFCache& Cache,
        float SDFBlendRadius,
        int32* OutNearestRoomIdx = nullptr
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
 * VoxelCaveMorphology::MakeStrateSeed; for slab, maze, shaft, and island archetypes, Seed is the
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
    FVector& OutPoint
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
    FVector& OutPoint
);
