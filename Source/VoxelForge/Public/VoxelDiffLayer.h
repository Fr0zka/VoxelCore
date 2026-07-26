// VoxelDiffLayer.h
// Runtime terrain modification system (player carving & filling).
//
// HOW IT WORKS:
// =============
// The procedural density function generates cave terrain deterministically.
// When the player carves or fills terrain, we DON'T modify the procedural
// output — instead, we store modifications as a "diff layer" on top.
//
// During density evaluation:
//   Final density = Procedural density + Diff offset
//
// This keeps procedural generation deterministic while supporting freeform editing.
// Modifications are stored spatially (per-chunk) for O(1) lookup.
//
// MODIFICATION LIFECYCLE:
// 1. Player uses a tool at position P with radius R and strength S
// 2. ApplyModification() stores the mod in all overlapping chunks
// 3. Returns the list of affected chunk coords → world re-meshes those chunks
// 4. During re-meshing, GetDensityOffset() returns the combined diff at each voxel
//
// PERSISTENCE:
// Diffs are stored in memory. The game layer is responsible for saving/loading
// them to disk (server-side) and clearing them on season reset.

#pragma once

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"   // FRWLock
#include "Misc/ScopeRWLock.h"      // FReadScopeLock / FWriteScopeLock
#include "VoxelTypes.h"
#include <atomic>
#include "VoxelDiffLayer.generated.h"

/**
 * EVoxelBrushShape — geometry of a modification brush.
 *   Sphere  : Center + Radius. Classic round brush.
 *   Box     : Center + BoxExtent (half-extents) + Falloff band.
 *   Capsule : segment Center→CapsuleEnd + Radius (tube) + Falloff band. Good for tunnels.
 */
UENUM(BlueprintType)
enum class EVoxelBrushShape : uint8
{
    Sphere  UMETA(DisplayName = "Sphere"),
    Box     UMETA(DisplayName = "Box"),
    Capsule UMETA(DisplayName = "Capsule")
};

/**
 * FVoxelModification — A single edit to the density field.
 *
 * Represents one brush stroke at a world position. Multiple overlapping
 * modifications combine additively.
 *
 * Strength convention (INTERNAL density, before MC negate):
 *   Negative → subtract density → create air → CARVE (remove rock)
 *   Positive → add density → create solid → FILL (add rock)
 */
USTRUCT(BlueprintType)
struct VOXELFORGE_API FVoxelModification
{
    GENERATED_BODY()

    // World-space center of the brush (endpoint A for Capsule).
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Modification")
    FVector Center = FVector::ZeroVector;

    // Sphere radius / capsule tube radius, in voxels.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Modification", meta = (ClampMin = "0.5"))
    float Radius = 3.0f;

    // Density change: negative = carve (remove rock), positive = fill (add rock).
    //   -10 → strong carve (punches through BaseDensity=8) · +10 → strong fill
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Modification")
    float Strength = -10.0f;

    // Brush geometry.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Modification")
    EVoxelBrushShape Shape = EVoxelBrushShape::Sphere;

    // Box half-extents in voxels (Box shape only).
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Modification",
        meta = (EditCondition = "Shape == EVoxelBrushShape::Box"))
    FVector BoxExtent = FVector(5.0f, 5.0f, 5.0f);

    // Capsule second endpoint in world voxel coords (Capsule shape only).
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Modification",
        meta = (EditCondition = "Shape == EVoxelBrushShape::Capsule"))
    FVector CapsuleEnd = FVector::ZeroVector;

    // Soft falloff band (voxels) around the Box/Capsule surface for smooth edges.
    // Sphere ignores this — its whole Radius is the falloff.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Modification", meta = (ClampMin = "0.1"))
    float Falloff = 2.0f;

    // Conservative world-space AABB the brush can affect (used to find overlapping chunks).
    void GetWorldBounds(FVector& OutMin, FVector& OutMax) const
    {
        switch (Shape)
        {
        case EVoxelBrushShape::Box:
        {
            const FVector E = BoxExtent + FVector(Falloff);
            OutMin = Center - E;
            OutMax = Center + E;
            break;
        }
        case EVoxelBrushShape::Capsule:
        {
            const float Pad = Radius + Falloff;
            OutMin = FVector(FMath::Min(Center.X, CapsuleEnd.X), FMath::Min(Center.Y, CapsuleEnd.Y), FMath::Min(Center.Z, CapsuleEnd.Z)) - FVector(Pad);
            OutMax = FVector(FMath::Max(Center.X, CapsuleEnd.X), FMath::Max(Center.Y, CapsuleEnd.Y), FMath::Max(Center.Z, CapsuleEnd.Z)) + FVector(Pad);
            break;
        }
        case EVoxelBrushShape::Sphere:
        default:
            OutMin = Center - FVector(Radius);
            OutMax = Center + FVector(Radius);
            break;
        }
    }
};

/**
 * UVoxelDiffLayer — Stores all player modifications as a spatial diff.
 *
 * Owned by AVoxelWorld, passed to the generator for density evaluation.
 * Modifications are grouped by chunk coord for fast spatial lookup —
 * when evaluating density for a chunk, we only check modifications
 * stored in that chunk (no global scan).
 *
 * When a modification sphere overlaps multiple chunks, it's stored in ALL
 * of them so each chunk's lookup is self-contained.
 */
UCLASS()
class VOXELFORGE_API UVoxelDiffLayer : public UObject
{
    GENERATED_BODY()

public:
    //=========================================================================
    // BUDGET CONFIGURATION
    //=========================================================================

    /**
     * Set the budget limits for player modifications.
     * Called by AVoxelWorld during BeginPlay from VoxelSettings values.
     * A value of 0 means unlimited (no cap).
     *
     * @param InMaxMods - Max number of operations (0 = unlimited)
     * @param InMaxRadius - Max brush radius per operation
     * @param InMaxVolume - Max cumulative brush volume in cubic voxels (0 = unlimited)
     */
    void SetBudget(int32 InMaxMods, float InMaxRadius, float InMaxVolume);

    /**
     * Check if a modification would be allowed under the current budget.
     * Does NOT consume budget — use this for UI feedback (greyed-out tool, etc.)
     *
     * @param Radius - The brush radius the player wants to use
     * @return true if the modification is allowed
     */
    bool CanModify(float Radius) const;

    /**
     * Get remaining modification count (how many more operations the player can do).
     * Returns -1 if unlimited.
     */
    UFUNCTION(BlueprintPure, Category = "Voxel Diff Layer")
    int32 GetRemainingModifications() const;

    /**
     * Get remaining volume budget (cubic voxels of modification still allowed).
     * Returns -1.0 if unlimited.
     */
    UFUNCTION(BlueprintPure, Category = "Voxel Diff Layer")
    float GetRemainingVolume() const;

    //=========================================================================
    // APPLY MODIFICATIONS
    //=========================================================================

    /**
     * Apply a carve or fill operation.
     *
     * Stores the modification in every chunk the brush sphere overlaps.
     * Returns the list of affected chunk coordinates so the world knows
     * which chunks need re-meshing.
     *
     * Budget enforcement: if the modification would exceed any configured
     * limit (count, radius, or volume), it is REJECTED and an empty array
     * is returned. Check CanModify() first for UI feedback.
     *
     * @param Mod - The modification to apply
     * @return Array of chunk coordinates that need re-meshing (empty if rejected)
     */
    TArray<FIntVector> ApplyModification(const FVoxelModification& Mod);

    //=========================================================================
    // DENSITY EVALUATION
    //=========================================================================

    /**
     * Get the combined density offset from all modifications at a world position.
     *
     * Called per-voxel by the generator during density evaluation.
     * Only checks modifications stored for the given chunk coordinate,
     * so it's fast when most chunks have no modifications.
     *
     * Each modification contributes with a smoothstep falloff from center to edge.
     * Multiple overlapping modifications combine additively.
     *
     * @param ChunkCoord - The chunk being evaluated (for spatial lookup)
     * @param WorldX, WorldY, WorldZ - The voxel position
     * @return Total density offset (negative = carve, positive = fill)
     */
    float GetDensityOffset(const FIntVector& ChunkCoord,
                           float WorldX, float WorldY, float WorldZ) const;

    /**
     * Check if a chunk has any modifications (fast reject for hot path).
     *
     * @param ChunkCoord - The chunk to check
     * @return true if there are modifications affecting this chunk
     */
    bool HasModifications(const FIntVector& ChunkCoord) const;

    //=========================================================================
    // HOT-PATH SNAPSHOT API (per-chunk, lock-amortised)
    //=========================================================================
    // Once ANY carve exists, calling HasModifications + GetDensityOffset per voxel costs two
    // ModsLock acquisitions + two TMap finds per density sample (~86k lock ops per tile task —
    // during carve gameplay, exactly when re-mesh latency matters). Instead, a worker snapshots a
    // chunk's mod list ONCE per (chunk, version) and evaluates it lock-free via EvaluateMods; the
    // version bump on ApplyModification/Clear invalidates worker-side caches.

    /** Lock-free: true once any modification exists anywhere (false = the common streaming case). */
    bool HasAnyMods() const { return bHasAnyMods.load(std::memory_order_acquire); }

    /** Monotonic mod-state version — bumped by ApplyModification and Clear. */
    uint32 GetModsVersion() const { return ModsVersion.load(std::memory_order_acquire); }

    /** Copy this chunk's modification list under ONE read lock (Out emptied if none). */
    void GetChunkModsSnapshot(const FIntVector& ChunkCoord, TArray<FVoxelModification>& Out) const;

    /** True si un chunk modifié intersecte [MinChunk, MaxChunk] (inclusif). Conservatif par
     *  construction : ApplyModification enregistre le mod dans TOUS les chunks que son rayon
     *  touche, donc le test par clé de chunk suffit. Une passe de lecture sur les clés (les
     *  mondes édités ont peu de chunks modifiés) — utilisé par ClassifyTile, PAS par voxel. */
    bool HasAnyModInChunkRange(const FIntVector& MinChunk, const FIntVector& MaxChunk) const;

    /** Evaluate a mod list at a voxel — the lock-free core shared by GetDensityOffset and the
     *  generator's snapshot path. Pure function (deterministic). Returns the combined offset
     *  (negative = carve, positive = fill). */
    static float EvaluateMods(const TArray<FVoxelModification>& Mods,
                              float WorldX, float WorldY, float WorldZ);

    //=========================================================================
    // MANAGEMENT
    //=========================================================================

    /** Clear ALL modifications (e.g., on season reset). */
    void Clear();

    /** Get total number of stored modification entries (for debug/stats). */
    int32 GetTotalModificationCount() const;

    /** Get number of chunks that have modifications. */
    int32 GetModifiedChunkCount() const;

private:
    //=========================================================================
    // STORAGE
    //=========================================================================

    // Modifications grouped by chunk coordinate.
    // Each chunk's list contains ALL modifications that overlap it
    // (including mods centered in neighboring chunks whose radius reaches here).
    //
    // THREADING: written on the game thread (ApplyModification / Clear) but read by MANY mesher
    // WORKER threads (GetDensityAt -> HasModifications / GetDensityOffset). TMap is not thread-safe —
    // a read landing during a write's rehash reads freed hash memory (EXCEPTION_ACCESS_VIOLATION in
    // TSet::FindId). All access to ChunkMods MUST hold ModsLock (read lock for queries, write lock for
    // mutation). bHasAnyMods is a lock-free fast reject: while it's false (the common streaming case,
    // no carves) readers skip the map AND the lock entirely, so unmodified worlds pay nothing.
    TMap<FIntVector, TArray<FVoxelModification>> ChunkMods;
    mutable FRWLock ModsLock;
    std::atomic<bool> bHasAnyMods{ false };
    std::atomic<uint32> ModsVersion{ 1 };   // see the snapshot API above

    //=========================================================================
    // BUDGET TRACKING
    //=========================================================================

    // Configured limits (0 = unlimited)
    int32 BudgetMaxMods = 0;
    float BudgetMaxRadius = 50.0f;
    float BudgetMaxVolume = 0.0f;

    // Running totals — reset when Clear() is called
    int32 ModificationCount = 0;       // Total operations applied
    float AccumulatedVolume = 0.0f;    // Sum of all brush sphere volumes (4/3 * PI * R^3)
};
