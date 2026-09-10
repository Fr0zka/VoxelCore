// VoxelStrateManager.h
// Runtime system that maps world Z coordinates to strate definitions.
//
// HOW IT WORKS:
// -------------
// 1. At world startup, Initialize() builds the strate layout:
//    - Fixed strates go into their assigned slots
//    - Random strates are shuffled from a pool using the world seed
//    - Each strate's height (in chunks) is accumulated to compute Z ranges
//
// 2. During gameplay, GetStrateAt(WorldZ) returns which strate definition
//    applies at a given depth. GetGenerationParams() returns blended params
//    (with smooth transitions at strate boundaries).
//
// 3. The generator, world manager, and future systems (decorations, creatures,
//    audio) all query this manager to know "what goes here."

#pragma once

#include "CoreMinimal.h"
#include "VoxelStrateTypes.h"
#include "VoxelStrateDefinition.h"
#include "VoxelSeasonManifest.h"
#include "VoxelCaveMorphology.h"
#include "VoxelStrateManager.generated.h"

class UVoxelSettings;

#if WITH_EDITOR
struct FVoxelStrateRegionManifest;
struct FVoxelStrateComposerSlotOverride;
#endif

/**
 * FVoxelPassage — A navigable connection between two strates.
 *
 * Generated deterministically from the world seed during Initialize().
 * Each passage is a path from a point in strate A down through the
 * boundary seal to a point in strate B. It's carved as a series of
 * capsule SDFs in the density function.
 *
 * Passages are the PRIMARY progression path — players find these
 * through exploration. The elevator is a FAST TRAVEL shortcut unlocked later.
 */
USTRUCT()
struct FVoxelPassage
{
    GENERATED_BODY()

    // Which strates this passage connects (indices into StrateLayout)
    int32 UpperStrateIndex = 0;  // The strate above
    int32 LowerStrateIndex = 0;  // The strate below

    // World-space positions of the passage endpoints
    FVector UpperPoint = FVector::ZeroVector;  // Entry in upper strate
    FVector LowerPoint = FVector::ZeroVector;  // Exit in lower strate

    // The deterministic upper-mouth request before source landing-site adjustment. Kept for
    // diagnostics/tests so the bounded lateral snap can be measured against the original spine-
    // relative placement; not used by the SDF evaluator.
    FVector RequestedUpperPoint = FVector::ZeroVector;

    // The deterministic lower-mouth request before destination landing-site adjustment. Kept for
    // diagnostics/tests so the bounded lateral snap can be measured against the original spine-
    // relative placement; not used by the SDF evaluator.
    FVector RequestedLowerPoint = FVector::ZeroVector;

    // Passage dimensions — how wide the carved tunnel is (in voxels).
    // The default SlopedTunnel is a single-file 2.5 m clear bore at its narrowest point;
    // authored exotic styles retain their own widths and are not covered by the walkable law.
    float Radius = 5.0f;

    // The shape/style of this passage. Determines how control points are generated
    // and how the passage feels to navigate (shaft, spiral, ledges, crack, etc.).
    EVoxelPassageType PassageType = EVoxelPassageType::SlopedTunnel;

    // Multi-segment control points for passage shapes. The default SlopedTunnel contains the two
    // gentle ramp legs of its deterministic switchback; exotic styles retain their authored chain.
    // When non-empty, the SDF evaluator walks this array as a capsule chain.
    TArray<FVector> ControlPoints;

    // Per-control-point tube radius (parallel to ControlPoints) for tapered tunnels —
    // wider chambers at the mouths, a squeeze in the middle, etc. If shorter than
    // ControlPoints, the uniform Radius is used as a fallback.
    TArray<float> ControlRadii;

    // Body-sized, flat-floored chambers at both tube ends. UpperPoint/LowerPoint remain the
    // standing anchors returned by the source query; the tube enters each room at DoorPoint.
    FVoxelPassageLanding UpperLanding;
    FVoxelPassageLanding LowerLanding;

    // Construction-time walkability facts for diagnostics and the commandlet report. These are
    // metadata, not a second runtime decision: the final-density sampler remains authoritative.
    bool bWalkableTunnelContract = false;
    float TunnelMaxGradientDegrees = 0.0f;
    float TunnelVerticalDropVoxels = 0.0f;
    float TunnelRequiredHorizontalRunVoxels = 0.0f;
    float TunnelHorizontalPathLengthVoxels = 0.0f;
    float TunnelMinimumClearWidthVoxels = 0.0f;
    float TunnelClearHeightVoxels = 0.0f;

    // Bounding sphere enclosing the whole passage (+ radius + blend), in voxel coords.
    // Computed once in GeneratePassages; lets EvaluateModifierSDF reject far voxels with
    // a single squared-distance test instead of walking every segment per voxel.
    // BoundRadius (linear) feeds the per-chunk shortlist reach; BoundRadiusSq the per-voxel test.
    FVector BoundCenter = FVector::ZeroVector;
    float BoundRadius = 0.0f;
    float BoundRadiusSq = 0.0f;
};

/**
 * FStrateSlot — Runtime info for one strate in the layout.
 *
 * Created during Initialize() and stored in the StrateLayout array.
 * Each slot knows its definition, its Z-range in chunk coordinates,
 * and its index in the sequence.
 */
USTRUCT()
struct FStrateSlot
{
    GENERATED_BODY()

    // The strate definition asset (content bag)
    UPROPERTY()
    UVoxelStrateDefinition* Definition = nullptr;

    // Strate index (0 = topmost, increases downward)
    int32 StrateIndex = 0;

    // Z chunk range (inclusive). TopZ > BottomZ since Z decreases downward.
    // Example: TopZ = 0, BottomZ = -3 means this strate spans chunks Z=0 to Z=-3
    int32 TopChunkZ = 0;
    int32 BottomChunkZ = 0;

    // Height in chunks (from the definition)
    int32 HeightInChunks = 4;
};

/**
 * UVoxelStrateManager — Maps depth to strate definitions at runtime.
 */
UCLASS(BlueprintType)
class VOXELFORGE_API UVoxelStrateManager : public UObject
{
    GENERATED_BODY()

public:
    UVoxelStrateManager();

    //=========================================================================
    // INITIALIZATION
    //=========================================================================

    /**
     * Build the strate layout from settings and seed.
     *
     * STEPS:
     * 1. For each strate index (0 to TotalStrates-1):
     *    - If it's in FixedStrates → use that definition
     *    - Else → pick from the shuffled pool (seeded random)
     * 2. Stack strates top-to-bottom, accumulating heights
     * 3. Store the layout in StrateLayout array
     *
     * @param Settings - VoxelSettings with pool/fixed strate config
     * @param WorldSeed - Seed for randomizing non-fixed strates
     */
    /** Returns false without a usable layout; an assigned invalid/stale season never falls back. */
    bool Initialize(UVoxelSettings* Settings, int32 WorldSeed);

    /**
     * Copy the immutable recipe payload for this chunk. In packaged builds this comes only from
     * the cooked season; editor candidate overrides use the same read-only worker hand-off.
     */
    bool GetRecipeForChunk(
        const FIntVector& ChunkCoord, int32& OutRecipeSeed,
        ECaveGeneratorType& OutArchetype, FVoxelStrateArchetypeParams& OutParams,
        FVoxelOpStackRecipe& OutRecipe) const;

    bool IsUsingSeason() const { return !ActiveSeasonContentHash.IsEmpty(); }
    const FString& GetSeasonContentHash() const { return ActiveSeasonContentHash; }
    int32 GetWorldSeed() const { return CachedSeed; }

    /**
     * Monotonic identity of this manager UObject instance.
     *
     * PassagesVersion intentionally starts over for every layout, so it cannot distinguish a
     * cached stack built by an older PIE manager that happened to have the same layout version.
     * Worker-local generator caches use this identity as their manager-lifetime boundary.
     */
    uint64 GetCacheLifetimeId() const { return CacheLifetimeId; }

#if WITH_EDITOR
    /**
     * Install an editor-only candidate over one already-built slot. The slot's definition,
     * dimensions, content, and passages remain intact; only the density archetype/parameters
     * (or the materialised recipe) are replaced. The caller must hold the world's generation pause.
     */
    bool SetComposerOverrideForStrate(
        int32 StrateIndex, int32 CandidateSeed, ECaveGeneratorType Archetype,
        const FVoxelStrateArchetypeParams& Params, bool bUseRecipe,
        const FVoxelOpStackRecipe* Recipe, FString& OutError,
        const FVoxelStrateRegionManifest* Regions = nullptr);

    /** Read the immutable editor override for a chunk into worker-local storage. */
    bool GetComposerOverrideForChunk(
        const FIntVector& ChunkCoord, int32& OutCandidateSeed,
        ECaveGeneratorType& OutArchetype, FVoxelStrateArchetypeParams& OutParams,
        bool& bOutUseRecipe, FVoxelOpStackRecipe& OutRecipe) const;

    /** Read the immutable editor region override into worker-local storage. */
    bool GetComposerRegionOverrideForChunk(
        const FIntVector& ChunkCoord, FVoxelStrateRegionManifest& OutRegions) const;

#endif

    //=========================================================================
    // QUERIES
    //=========================================================================

    /**
     * Get the strate definition at a world Z coordinate.
     *
     * @param WorldZ - Z position in world space (Unreal units)
     * @return The strate definition, or nullptr if above/below all strates
     */
    UFUNCTION(BlueprintCallable, Category = "Strate")
    UVoxelStrateDefinition* GetStrateAt(float WorldZ) const;

    /**
     * Get the strate index at a world Z coordinate.
     *
     * @param WorldZ - Z position in world space
     * @return Strate index (0 = topmost), or -1 if outside strate range
     */
    UFUNCTION(BlueprintCallable, Category = "Strate")
    int32 GetStrateIndex(float WorldZ) const;

    /** Layout/passage generation counter (bumped by every Initialize → GeneratePassages).
     *  Hot-path callers key thread_local memos on this so an editor rebuild (RebuildStrates /
     *  live edit) can never serve a stale strate index or passage shortlist. */
    uint32 GetLayoutVersion() const { return PassagesVersion; }

    /**
     * Get the strate definition for a specific chunk coordinate.
     *
     * @param ChunkCoord - The chunk position
     * @return The strate definition, or nullptr if outside strate range
     */
    UVoxelStrateDefinition* GetStrateForChunk(const FIntVector& ChunkCoord) const;

    /**
     * Strate-aware vertical streaming: fills the chunk-Z span of the strate containing
     * ChunkZ. Returns false if ChunkZ is in the inter-strate bedrock gap (or outside the
     * layout) — the caller then leaves the vertical view unclamped (the gap is a brief
     * see-both-sides descent transition). TopChunkZ > BottomChunkZ (Z decreases downward).
     */
    bool GetStrateChunkZBounds(int32 ChunkZ, int32& OutTopChunkZ, int32& OutBottomChunkZ) const;

    /**
     * Get generation params for a chunk, with boundary blending.
     *
     * BLENDING CONCEPT:
     * When a chunk is near a strate boundary (within BlendChunks of the edge),
     * the params are lerped between the two adjacent strates. This prevents
     * hard visual seams where one strate ends and another begins.
     *
     * @param ChunkCoord - The chunk position
     * @return Blended generation params for this chunk
     */
    FStrateGenerationParams GetGenerationParams(const FIntVector& ChunkCoord) const;

    /**
     * Get the generator type for a specific chunk.
     *
     * Returns TunnelNetwork if the chunk is outside all strates or if the
     * strate definition is null. The generator uses this to pick which
     * density function to call (GetDensityWithParams vs GetSlabDensity).
     *
     * @param ChunkCoord - The chunk position
     * @return The ECaveGeneratorType for the strate containing this chunk
     */
    ECaveGeneratorType GetGeneratorTypeForChunk(const FIntVector& ChunkCoord) const;

    /**
     * True when this chunk's strate opts into the density OPERATOR STACK instead of the hardcoded
     * archetype switch (`UVoxelStrateDefinition::bUseOperatorStack`).
     *
     * Returns false for archetypes that have no port yet, so the flag can be set on any strate
     * without changing its output until that archetype lands. Only `Maze` is ported today — this
     * predicate is where that list grows, and it is deliberately the ONLY place it is written down.
     */
    bool UsesOperatorStackForChunk(const FIntVector& ChunkCoord) const;

    /**
     * True if this chunk is in the solid-bedrock GAP between two strates (inside the
     * overall stack's Z range but not in any strate slot). Chunks above the top strate
     * or below the bottom strate are NOT gaps (they're open air). Driven by
     * VoxelSettings::InterStrateGapChunks.
     */
    bool IsGapChunk(const FIntVector& ChunkCoord) const;

    /**
     * Get slab generation params for a chunk, with runtime Z bounds filled in.
     *
     * Used when GetGeneratorTypeForChunk returns FlatPlain or CrystalChamber.
     * Does NOT blend between adjacent strates — slab strates use Hard transition
     * at their boundaries (blending between fundamentally different generator types
     * is not meaningful).
     *
     * @param ChunkCoord - The chunk position
     * @return FSlabGenerationParams with StrateTopWorldZ / StrateBottomWorldZ set
     */
    FSlabGenerationParams GetSlabParamsForChunk(const FIntVector& ChunkCoord) const;

    /**
     * Per-archetype param getters. Each copies the designer params from the strate
     * definition and fills in the runtime Z bounds (voxel coords). Like the slab
     * getter, these do NOT blend across boundaries — fundamentally different
     * archetypes meet at Hard boundaries.
     */
    FMazeGenerationParams        GetMazeParamsForChunk(const FIntVector& ChunkCoord) const;
    FSurfaceGenerationParams     GetSurfaceParamsForChunk(const FIntVector& ChunkCoord) const;
    FVerticalShaftParams         GetVerticalShaftParamsForChunk(const FIntVector& ChunkCoord) const;
    FFloatingIslandParams        GetFloatingIslandParamsForChunk(const FIntVector& ChunkCoord) const;

    /**
     * Flatten the strate's Biomes[] + BiomeMapParams into a POD FBiomeContext for the
     * biome field. Returns an empty (invalid) context when the strate has no biomes —
     * callers treat that as "biomes disabled" and fall back to base params. The result
     * is window-invariant (depends only on the strate at this Z, not the chunk window).
     */
    FBiomeContext GetBiomeContextForChunk(const FIntVector& ChunkCoord) const;

    /**
     * World-space Z (voxel coords) of this strate's water surface, or -FLT_MAX if the
     * strate has no water. Derived from the active archetype's WaterLevelRelative and
     * the strate's Z range. Used by the water render system.
     */
    float GetWaterLevelWorldZForChunk(const FIntVector& ChunkCoord) const;

    /**
     * Unreal-unit (cm) world Z range of the strate containing WorldZ (Unreal units).
     * OutTopZ = ceiling, OutBottomZ = floor. Returns false if WorldZ is outside all strates.
     * Used by the atmosphere system to glue ceiling/floor layers to the strate.
     */
    bool GetStrateUnrealZRange(float WorldZ, float& OutTopZ, float& OutBottomZ) const;

    /**
     * Disturbance params for a chunk (chasms/bridges/ridges), with runtime Z bounds
     * filled in. Returns an all-disabled default when outside the strate range.
     */
    FStrateDisturbanceParams GetDisturbanceParamsForChunk(const FIntVector& ChunkCoord) const;

    /**
     * Get the full strate layout (for debug display or UI).
     */
    const TArray<FStrateSlot>& GetLayout() const { return StrateLayout; }

    /**
     * Get the total number of strates in the layout.
     */
    int32 GetNumStrates() const { return StrateLayout.Num(); }

    //=========================================================================
    // PASSAGES & ELEVATOR
    //=========================================================================

    /**
     * Evaluate the SDF for all passages and the elevator shaft at a world position.
     * Called by the density function to carve these modifiers into the terrain.
     *
     * Returns: negative = inside a passage/shaft, positive = solid rock.
     * FLT_MAX = no modifier nearby.
     */
    float EvaluateModifierSDF(float WorldX, float WorldY, float WorldZ) const;

    /** Apply the passage tube/landing carve and the guaranteed landing floor in internal density. */
    void ApplyPassageModifier(float& Density, float WorldX, float WorldY, float WorldZ,
                              float BaseDensity, float SealThickness) const;

    /** Apply only the passage SDF carve. The shared MC post owns landing/tunnel support writes. */
    void ApplyPassageCarvingOnly(float& Density, float WorldX, float WorldY, float WorldZ,
                                 float BaseDensity, float SealThickness) const;

    /** Reapply the landing-only air carve in internal density (used after visual disturbances). */
    void ApplyPassageLandingAir(float& Density, float WorldX, float WorldY, float WorldZ,
                                float BaseDensity, float SealThickness) const;

    /** Reassert the default walkable tunnel's clear air in internal density after any post. */
    void ApplyPassageTunnelAir(float& Density, float WorldX, float WorldY, float WorldZ,
                               float BaseDensity, float SealThickness) const;

    /** Re-assert landing air after MC-space disturbances, still before the final XY seal. */
    void ApplyPassageLandingAirMC(float& Density, float WorldX, float WorldY, float WorldZ,
                                  float BaseDensity, float SealThickness) const;

    /** Re-assert walkable tunnel air after MC-space disturbances, still before the final XY seal. */
    void ApplyPassageTunnelAirMC(float& Density, float WorldX, float WorldY, float WorldZ,
                                 float BaseDensity, float SealThickness) const;

    /** Apply the landing/tunnel MC backstop in one cached passage scan after disturbances. */
    void ApplyPassageStructuralPostsMC(float& Density, float WorldX, float WorldY, float WorldZ,
                                       float BaseDensity, float SealThickness) const;

    /** Re-assert a landing floor after the landing air pass, still before the final XY seal. */
    void ApplyPassageLandingFloorMC(float& Density, float WorldX, float WorldY, float WorldZ,
                                    float BaseDensity) const;

    /** Re-assert only the flat room floors after the tunnel-air post; never writes tunnel floors. */
    void ApplyPassageLandingRoomFloorMC(float& Density, float WorldX, float WorldY, float WorldZ,
                                        float BaseDensity) const;

    /**
     * True si la sphère englobante d'un passage (élargie du rayon de blend de carve) touche la
     * boîte VOXEL [MinVoxel, MaxVoxel]. Test conservatif O(Passages) — utilisé par ClassifyTile
     * (rejet des tuiles trivialement pleines) une fois PAR TUILE, jamais par voxel.
     */
    bool AnyPassageNearBox(const FVector& MinVoxel, const FVector& MaxVoxel) const;

    /** Conservative box guard for the floor fill; unlike a tube it can affect the all-air proof. */
    bool AnyPassageLandingFloorNearBox(const FVector& MinVoxel, const FVector& MaxVoxel) const;

    /** Exact-lattice counterparts used by ClassifyTile's MC-grid proof. */
    bool AnyPassageNearLattice(const FBox& VoxelBox,
                               const FIntVector& LatticeOrigin, int32 Step) const;
    /** Exact maximum of the passage carve factor on the mesher lattice; 0 means no tube/landing
     * carve reaches a sampled point. Invalid geometry returns 1 conservatively. */
    float MaxPassageCarveFactorNearLattice(const FBox& VoxelBox,
                                           const FIntVector& LatticeOrigin, int32 Step) const;
    /** Exact conservative candidate for the post-stack passage air writers on the MC lattice.
     * This deliberately excludes the passage modifier already folded by FPassageCarveOp. */
    bool AnyPassageAirPostNearLattice(const FBox& VoxelBox,
                                      const FIntVector& LatticeOrigin, int32 Step,
                                      float BaseDensity, float SealThickness) const;
    bool AnyPassageLandingFloorNearLattice(const FBox& VoxelBox,
                                           const FIntVector& LatticeOrigin, int32 Step) const;
    /** Exact presence test for a structural floor write on the MC lattice. */
    bool AnyLandingFloorAtLattice(const FBox& VoxelBox,
                                  const FIntVector& LatticeOrigin, int32 Step) const;

    /** Conservative box guards for the per-strate (0,0) landing rooms and their floor slabs. */
    bool AnyOriginLandingNearBox(const FVector& MinVoxel, const FVector& MaxVoxel) const;
    bool AnyOriginLandingFloorNearBox(const FVector& MinVoxel, const FVector& MaxVoxel) const;

    bool AnyOriginLandingNearLattice(const FBox& VoxelBox,
                                     const FIntVector& LatticeOrigin, int32 Step) const;
    /** Exact conservative candidate for the post-disturbance origin-room air reassertion. */
    bool AnyOriginLandingAirNearLattice(const FBox& VoxelBox,
                                        const FIntVector& LatticeOrigin, int32 Step,
                                        float BaseDensity, float SealThickness) const;
    bool AnyOriginLandingFloorNearLattice(const FBox& VoxelBox,
                                          const FIntVector& LatticeOrigin, int32 Step) const;

    /** Get all generated passages (for debug display). */
    const TArray<FVoxelPassage>& GetPassages() const { return Passages; }

protected:
    //=========================================================================
    // INTERNAL DATA
    //=========================================================================

    // The stacked strate layout (index 0 = topmost strate)
    UPROPERTY()
    TArray<FStrateSlot> StrateLayout;

    // Parallel to StrateLayout only while a cooked season is active. Plain immutable data is
    // copied into worker-local caches on the existing layout-versioned refetch path.
    TArray<FVoxelSeasonStrate> SeasonStrates;
    FString ActiveSeasonContentHash;

    // Passages connecting consecutive strates
    TArray<FVoxelPassage> Passages;

    // Bumped every time Passages is rebuilt (GeneratePassages), and for an editor composer
    // override. EvaluateModifierSDF and all generator thread_local memos use this to invalidate
    // cached passage/strate data after either kind of live change.
    uint32 PassagesVersion = 0;

    // Never reused for the lifetime of the process. This is deliberately not a UObject pointer:
    // allocator address reuse must not make a stale worker-local stack look current.
    uint64 CacheLifetimeId = 0;

    // How many chunks at strate boundaries are blended (transition zone)
    int32 BlendChunks = 2;

    // World seed (stored for passage generation)
    int32 CachedSeed = 0;

    // Whether to auto-open the (0,0) origin landing room from above in strate 0.
    // Copied from VoxelSettings::bOpenSurfaceEntry during Initialize().
    bool bOpenSurfaceEntry = true;

    // Authored radius (voxels) of the origin landing room and its optional surface opening.
    // Copied from VoxelSettings::OriginSpineRadius.
    float OriginSpineRadius = 14.0f;

    // Solid-bedrock gap between consecutive strates, in chunks. Copied from
    // VoxelSettings::InterStrateGapChunks during Initialize().
    int32 InterStrateGapChunks = 0;

    // Per-passage tunnel shape now lives on each UVoxelStrateDefinition::PassageConfig
    // (the upper strate of each boundary controls its own descent tunnels).

    //=========================================================================
    // HELPERS
    //=========================================================================

    // Find which slot a chunk Z coordinate falls into.
    // Returns INDEX into StrateLayout, or -1 if not found.
    int32 FindSlotIndexForChunkZ(int32 ChunkZ) const;

#if WITH_EDITOR
    const FVoxelStrateComposerSlotOverride* FindComposerOverride(int32 StrateIndex) const;
    TMap<int32, TSharedPtr<FVoxelStrateComposerSlotOverride>> ComposerOverrides;
#endif

    // Build FStrateGenerationParams from a strate definition:
    // copies base GenerationParams, then applies all referenced terrain op assets.
    // This is the single point where terrain op data assets are merged into params.
    static FStrateGenerationParams BuildParamsFromDefinition(const UVoxelStrateDefinition* Definition);

    // Generate passages between consecutive strates. Called from Initialize().
    void GeneratePassages();
};
