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
struct FStrateTerrainOpEntry;
class UVoxelTerrainOpDefinition;

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
