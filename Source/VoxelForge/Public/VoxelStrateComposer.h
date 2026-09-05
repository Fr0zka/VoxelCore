// Offline strate corpus and parameter composer.
//
// This is deliberately not part of the runtime generation path.  The composer is a build-box
// tool/test API: it loads authored vectors, measures their spread, and makes deterministic
// candidates by blending those vectors before applying spread-relative (or season-zero bootstrap)
// jitter.

#pragma once

#include "CoreMinimal.h"
#include "VoxelStrateTypes.h"
#include "VoxelDensityOpStack.h"
#include "VoxelStrateMeasure.h"

#include "VoxelStrateComposer.generated.h"

class UVoxelSettings;
class UVoxelStrateDefinition;
class UVoxelStrateManager;

#if WITH_EDITOR
// The complete definitions live in VoxelSeasonManifest.h. Keeping this forward declaration here
// makes the Tier 4c entry point discoverable from the composer header without creating a circular
// include (the manifest header includes this one for the existing vector/recipe types).
struct FVoxelSeasonManifest;
struct FVoxelSeasonCompositionSettings;
VOXELFORGE_API FVoxelSeasonManifest VF_ComposeSeason(
    int32 SeasonSeed, const FVoxelSeasonCompositionSettings& Settings);
#endif

/** The one-bit identity choice at the root of a rolled structure. */
UENUM(BlueprintType)
enum class EVoxelStrateRootPolarity : uint8
{
    RockCarve = 0,
    VoidFill = 1,
};

/** Native parameter family an op reads when a recipe is materialised. */
UENUM(BlueprintType)
enum class EVoxelStrateParamBlock : uint8
{
    None = 0,
    TunnelNetwork = 1,
    Slab = 2,
    Maze = 3,
    Surface = 4,
    VerticalShaft = 5,
    FloatingIsland = 6,
};

/** Stable manifest id for every op the offline structure roller may place. */
UENUM(BlueprintType)
enum class EVoxelStrateOpClass : uint8
{
    ConstantRockSource = 0,
    ConstantVoidSource = 1,
    RoomGraphSource = 2,
    LatticeCorridorSource = 3,
    ShaftFieldSource = 4,
    IslandBlobSource = 5,
    NoiseRibbonSource = 6,
    SdfRoughnessMod = 7,
    SdfCarve = 8,
    SdfFill = 9,
    GridColumnMod = 10,
    CaveRoughnessMod = 11,
    CaveTerraceMod = 12,
    LayerLineMod = 13,
    RibbingMod = 14,
    CaveOverhangMod = 15,
    CaveCliffMod = 16,
    ScallopMod = 17,
    CaveArchMod = 18,
    RoomColumnMod = 19,
    DomeMod = 20,
    PinchMod = 21,
    FloorBiasMod = 22,
    WormFieldSource = 23,
    ShaftLedgeMod = 24,
    DensityNoiseCarveMod = 25,
    DensityNoiseFillMod = 26,
};

/** One creative op in the serialisable structure manifest. */
USTRUCT(BlueprintType)
struct VOXELFORGE_API FVoxelOpRecipeEntry
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Recipe")
    EVoxelStrateOpClass OpClass = EVoxelStrateOpClass::ConstantRockSource;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Recipe")
    EVoxelStrateParamBlock ParamBlock = EVoxelStrateParamBlock::None;
};

/**
 * Serialisable creative structure. Structural posts are intentionally absent: the recipe builder
 * appends spine, vertical seal, passage carve, and XY edge seal in that fixed order every time.
 */
USTRUCT(BlueprintType)
struct VOXELFORGE_API FVoxelOpStackRecipe
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Recipe")
    EVoxelStrateRootPolarity RootPolarity = EVoxelStrateRootPolarity::RockCarve;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Recipe")
    FVoxelOpRecipeEntry Root;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Recipe")
    FVoxelOpRecipeEntry ShapeSource;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Recipe")
    FVoxelOpRecipeEntry Conversion;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Recipe")
    TArray<FVoxelOpRecipeEntry> Modifiers;

    // Which native block supplies the mandatory structural post parameters.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Recipe")
    EVoxelStrateParamBlock StructuralParamBlock = EVoxelStrateParamBlock::None;
};

/** The kind of value represented by one entry of VF_STRATE_PARAM_FIELDS. */
enum class EVoxelStrateFieldKind : uint8
{
    Continuous,
    Integer,
    Boolean,
    Enum,
};

/** A field deliberately kept out of the parameter roll after the measured liveness audit. */
struct VOXELFORGE_API FVoxelStrateFieldExclusion
{
    FString FieldName;
    FString Reason;
};

/** Measured statistics for one field in one archetype's corpus group. */
struct VOXELFORGE_API FVoxelStrateFieldSpread
{
    ECaveGeneratorType Archetype = ECaveGeneratorType::TunnelNetwork;
    FString ParamStructName;
    FString FieldName;
    EVoxelStrateFieldKind Kind = EVoxelStrateFieldKind::Continuous;
    bool bExcluded = false;
    bool bReflected = false;

    // Actual authored corpus samples and synthetic per-operation default samples are kept
    // separate in the report. SampleCount is their effective total used for the statistics.
    int32 AuthoredSampleCount = 0;
    int32 PromotedSampleCount = 0;
    int32 DefaultSeedCount = 0;
    int32 SampleCount = 0;
    double Min = 0.0;
    double Max = 0.0;
    double Mean = 0.0;
    double StdDev = 0.0;

    // These are copied from UPROPERTY ClampMin/ClampMax when reflection metadata is available in
    // the current build. They are safety limits, never the source of a roll range.
    bool bHasClampMin = false;
    bool bHasClampMax = false;
    double ClampMin = 0.0;
    double ClampMax = 0.0;
};

/** Where a member of the offline corpus came from. */
enum class EVoxelStrateCorpusProvenance : uint8
{
    Project,
    Default,
    Promoted,
};

/** The finite, diffable metric summary stored with a promoted strate. */
struct VOXELFORGE_API FVoxelStrateMeasuredMetrics
{
    bool bValid = false;

    int64 NumSampled = 0;
    int64 NumAir = 0;
    int64 NumSolid = 0;
    float AirFraction = 0.0f;
    int32 NumAirComponents = 0;
    float LargestComponentShare = 0.0f;
    FVector LargestComponentPoint = FVector::ZeroVector;
    int64 LargestComponentCells = 0;
    int32 NumComponentsAtLeast1Pct = 0;
    float WalkableFraction = 0.0f;
    int64 WalkableFloorColumns = 0;
    float WalkableFloorAreaFraction = 0.0f;
    int32 NumWalkableSurfaceComponents = 0;
    int64 LargestWalkableSurfaceColumns = 0;
    float LargestWalkableSurfaceShare = 0.0f;
    float MedianFeatureScale = 0.0f;
    int32 MedianVerticalClearance = 0;

    bool bPlayerFitResolved = false;
    FString PlayerFitRefusalReason;
    int64 NumPlayerFitCells = 0;
    float PlayerFitFraction = 0.0f;
    int32 NumTraversableComponents = 0;
    int64 LargestTraversableComponentCells = 0;
    float TraversableComponentShare = 0.0f;
    float MinimumPlayerClearanceVoxels = 0.0f;

    int32 ResolvedMarginVoxels = 0;
    int32 SampledMinZ = 0;
    int32 SampledMaxZ = 0;
    int32 SampledNumX = 0;
    int32 SampledNumY = 0;
    int32 SampledNumZ = 0;
    float SampledMinX = 0.0f;
    float SampledMaxX = 0.0f;
    float SampledMinY = 0.0f;
    float SampledMaxY = 0.0f;

    // This is the component-size audit trail, not a retained density grid. It is bounded by the
    // number of discovered air components and is useful when reviewing a promoted record.
    TArray<int64> AirComponentCells;

    bool IsUsable() const;
};

/** Copy the measured facts that are safe and useful to persist; no sampler or grid is retained. */
VOXELFORGE_API FVoxelStrateMeasuredMetrics VF_SummarizeStrateMetrics(
    const FVoxelStrateMetrics& Metrics);

/** Native parameter storage for every currently supported strate archetype. */
struct VOXELFORGE_API FVoxelStrateArchetypeParams
{
    FStrateGenerationParams TunnelNetworkParams;
    FSlabGenerationParams SlabParams;
    FMazeGenerationParams MazeParams;
    FSurfaceGenerationParams SurfaceParams;
    FVerticalShaftParams VerticalShaftParams;
    FFloatingIslandParams FloatingIslandParams;
};

/**
 * One lateral region in a strate.
 *
 * This is deliberately a plain manifest record rather than a label on the strate.  The recipe
 * and the native vector belong to the region, and the runtime combines the resulting density
 * fields.  Structural posts are not region-owned.
 */
struct VOXELFORGE_API FVoxelStrateRegion
{
    int32 RegionIndex = 0;
    int32 Seed = 0;
    ECaveGeneratorType Archetype = ECaveGeneratorType::TunnelNetwork;
    FVoxelStrateArchetypeParams ArchetypeParams;
    bool bUsesRecipe = false;
    FVoxelOpStackRecipe Recipe;
};

/**
 * Offline-composed lateral region manifest for one strate.
 *
 * `StrateIndex` is part of the partition identity.  A candidate can be moved to another live
 * slot, so the editor hand-off must call VF_RekeyStrateRegionManifest before installing it.
 * `PartitionSeed` is retained in the manifest so the runtime never has to recreate an RNG stream.
 */
struct VOXELFORGE_API FVoxelStrateRegionManifest
{
    bool bValid = false;
    FString FailureReason;

    int32 Seed = 0;
    int32 StrateIndex = 0;
    int32 RegionCount = 1;
    uint32 PartitionSeed = 0;

    // A 32-voxel chunk sees a tiny portion of this lattice.  The values are manifest data, not
    // hidden runtime tuning knobs; changing them changes the authored world and its hash.
    float LatticeCellSize = 256.0f;
    float BlendWidth = 24.0f;

    // One global structural post configuration.  Region recipes may use different native blocks,
    // but no region is allowed to choose the spine/seal/passage/edge-seal values independently.
    bool bHasGlobalStructuralParams = false;
    EVoxelStrateParamBlock StructuralParamBlock = EVoxelStrateParamBlock::None;
    float StrateTopWorldZ = 0.0f;
    float StrateBottomWorldZ = 0.0f;
    float BoundarySealThickness = 0.0f;
    float BaseDensity = 8.0f;

    TArray<FVoxelStrateRegion> Regions;

    bool IsSingleRegion() const { return RegionCount == 1 && Regions.Num() == 1; }

    bool IsValid() const
    {
        return bValid && RegionCount >= 1 && RegionCount <= 3
            && Regions.Num() == RegionCount
            && FMath::IsFinite(LatticeCellSize) && LatticeCellSize > 0.0f
            && FMath::IsFinite(BlendWidth) && BlendWidth >= 0.0f;
    }
};

/** Result of the positional Voronoi query used by the lateral density combiner. */
struct VOXELFORGE_API FVoxelStrateRegionQuery
{
    int32 PrimaryRegion = 0;
    int32 NeighborRegion = INDEX_NONE;
    float NeighborWeight = 0.0f;
    float NearestDifferentRegionGap = FLT_MAX;
    // False means the bounded lattice neighborhood could not prove that its different-region
    // candidate is the nearest one. Density evaluation may still use a zero blend weight, but a
    // ClassifyBox proof must refuse to call that an infinite boundary distance.
    bool bNearestDifferentRegionKnown = false;
    bool bInBlendBand = false;
};

/** Conservative XY proof used before a tile may be skipped. */
struct VOXELFORGE_API FVoxelStrateRegionBoxProof
{
    bool bProvablySingleRegion = false;
    bool bTouchesRegionBoundary = true;
    bool bTouchesBlendBand = true;
};

/** One hash-lattice site retained by the per-chunk partition cache. */
struct VOXELFORGE_API FVoxelStrateRegionSite
{
    int32 CellX = 0;
    int32 CellY = 0;
    int32 Region = 0;
    float WorldX = 0.0f;
    float WorldY = 0.0f;
};

/**
 * Worker-local cache for the lateral partition.
 *
 * Hash/site discovery and the integer XY grid are prepared once for a chunk.  The voxel path
 * then does an O(1) lookup for the ordinary integer density samples; only fractional gradient
 * probes use the small prepared site list.  No seed lattice is searched or allocated per voxel.
 */
struct VOXELFORGE_API FVoxelStrateRegionPartitionCache
{
    void PrepareForChunk(const FVoxelStrateRegionManifest& Manifest,
                         const FIntVector& ChunkCoord);
    FVoxelStrateRegionQuery Query(float WorldX, float WorldY) const;

private:
    int32 RegionCount = 1;
    uint32 PartitionSeed = 0;
    float LatticeCellSize = 256.0f;
    float BlendWidth = 24.0f;
    int32 BaseX = 0;
    int32 BaseY = 0;
    int32 Dim = 0;
    TArray<FVoxelStrateRegionSite> Sites;
    TArray<FVoxelStrateRegionQuery> IntegerSamples;
};

/** Set every native family’s runtime Z bounds to the same strate interval. */
VOXELFORGE_API void VF_SetStrateArchetypeRuntimeBounds(
    FVoxelStrateArchetypeParams& Params, float TopWorldZ, float BottomWorldZ);

/** Stable identity helpers for the deterministic lateral partition. */
#if WITH_EDITOR
VOXELFORGE_API int32 VF_RollStrateRegionCount(int32 Seed, int32 StrateIndex);
#endif
VOXELFORGE_API uint32 VF_GetStrateRegionPartitionSeed(int32 Seed, int32 StrateIndex);
VOXELFORGE_API void VF_RekeyStrateRegionManifest(
    FVoxelStrateRegionManifest& Manifest, int32 Seed, int32 StrateIndex);

/** Pure positional query; no retained state and no ordering-dependent RNG. */
VOXELFORGE_API FVoxelStrateRegionQuery VF_QueryStrateRegion(
    const FVoxelStrateRegionManifest& Manifest, float WorldX, float WorldY);

/** Conservative proof for an XY projection of a voxel box. */
VOXELFORGE_API FVoxelStrateRegionBoxProof VF_AnalyzeStrateRegionBox(
    const FVoxelStrateRegionManifest& Manifest, const FBox& VoxelBox);

/** One known-good authored vector and the archetype that gives it meaning. */
struct VOXELFORGE_API FVoxelStrateCorpusEntry
{
    FString SourcePath;
    FString SourceName;
    ECaveGeneratorType Archetype = ECaveGeneratorType::TunnelNetwork;

    // Native storage is selected by Archetype. Keeping every family here makes the corpus
    // type-safe without changing any runtime generation parameter struct.
    FVoxelStrateArchetypeParams ArchetypeParams;

    // Compatibility view for callers of the original Tier 4a API. It is meaningful for
    // TunnelNetwork/Underwater entries and is the same value as ArchetypeParams.TunnelNetworkParams.
    FStrateGenerationParams Params;

    // The current asset format has no corpus weight field. Loaded hand-authored entries therefore
    // use 1.0. The field remains explicit so a future offline corpus can assign weights without
    // changing the roll algorithm.
    float Weight = 1.0f;

    EVoxelStrateCorpusProvenance Provenance = EVoxelStrateCorpusProvenance::Project;

    // Promoted entries retain their recipe and re-measured facts so the next policy decision can
    // compare measured geometry, not only parameter-space distance. Project/default entries leave
    // these fields unset unless an offline simulation explicitly measures them.
    bool bHasRecipe = false;
    FVoxelOpStackRecipe Recipe;
    bool bHasMeasuredMetrics = false;
    FVoxelStrateMeasuredMetrics MeasuredMetrics;

    FString PromotionRecordId;
    int32 PromotionSeason = INDEX_NONE;
    int32 PromotionSeed = 0;
    int32 PromotionCandidateIndex = INDEX_NONE;
    uint32 PromotionInputCorpusHash = 0;
};

/** A candidate record that is eligible to be considered for corpus promotion. */
struct VOXELFORGE_API FVoxelStratePromotableRecord
{
    FString RecordId;
    int32 Season = 0;
    int32 Seed = 0;
    int32 CandidateIndex = INDEX_NONE;
    uint32 InputCorpusHash = 0;

    ECaveGeneratorType Archetype = ECaveGeneratorType::TunnelNetwork;
    FVoxelStrateArchetypeParams Params;
    FVoxelOpStackRecipe Recipe;
    FVoxelStrateMeasuredMetrics MeasuredMetrics;

    // These are recorded evidence for review. The loader deliberately ignores the stored metric
    // and gate values and asks its verifier to rebuild/re-measure the recipe instead.
    bool bPassedNonVacuous = false;
    bool bPassedLargestComponent = false;
    bool bPassedPrimordialLaw = false;
};

/** Fresh validation output used by the safe promoted-record loader. */
struct VOXELFORGE_API FVoxelStratePromotionVerification
{
    bool bPassedNonVacuous = false;
    bool bPassedLargestComponent = false;
    bool bPassedPrimordialLaw = false;
    FVoxelStrateMeasuredMetrics MeasuredMetrics;
    FString FailureReason;

    bool PassedAllGates() const
    {
        return bPassedNonVacuous && bPassedLargestComponent && bPassedPrimordialLaw
            && MeasuredMetrics.IsUsable();
    }
};

/**
 * World-specific re-measurement seam for promoted records.
 *
 * A corpus store cannot know the active fixture, mouths, or validation window. Its caller must
 * provide this verifier; a null verifier means promoted records are skipped rather than trusted.
 */
class VOXELFORGE_API IVoxelStratePromotionVerifier
{
public:
    virtual ~IVoxelStratePromotionVerifier() = default;
    virtual bool Verify(const FVoxelStratePromotableRecord& Record,
                        FVoxelStratePromotionVerification& OutVerification) const = 0;

    // Optional companion audit for project/default members. A policy pass can then compare a
    // candidate against the measured corpus rather than falling back to parameter identity. A
    // world-specific verifier may return false when that fixture cannot measure the member.
    virtual bool MeasureCorpusEntry(const FVoxelStrateCorpusEntry& Entry,
                                    FVoxelStrateMeasuredMetrics& OutMetrics) const
    {
        (void)Entry;
        (void)OutMetrics;
        return false;
    }
};

/** Explicit, provisional Tier 5 policy. The owner can change these after seeing the preview. */
struct VOXELFORGE_API FVoxelStratePromotionPolicy
{
    // Five normalized measured dimensions are compared with Euclidean distance. 0.20 is large
    // enough to reject close copies while still admitting genuinely different geometry; it is a
    // provisional review threshold, not a claim that this is the owner's final definition of good.
    double MinimumNormalizedMeasuredMetricDistance = 0.20;
    int32 MaxPromotionsPerSeason = 6;

    // Physical normalization for the two unbounded-in-principle metric dimensions. Air fraction,
    // largest share, and walkable fraction already live in [0,1].
    float FeatureScaleNormalizationVoxels = 256.0f;
    float ClearanceNormalizationVoxels = 64.0f;
};

/** Counters and accepted records from one deterministic policy pass. */
struct VOXELFORGE_API FVoxelStratePromotionBatchResult
{
    bool bPolicyValid = false;
    TArray<FVoxelStratePromotableRecord> Promoted;
    int32 NumCandidates = 0;
    int32 NumValidationRejected = 0;
    int32 NumCorpusHashRejected = 0;
    int32 NumNearDuplicateRejected = 0;
    int32 NumCapRejected = 0;
    double MinimumAcceptedDistance = 0.0;
};

/** One line of the diffable season manifest written beside the cumulative record store. */
struct VOXELFORGE_API FVoxelStrateSeasonManifest
{
    int32 Season = 0;
    int32 Seed = 0;
    uint32 InputCorpusHash = 0;
    int32 CandidateCount = 0;
    int32 SurvivorCount = 0;
    int32 PromotedCount = 0;
    int32 CorpusSize = 0;
    int32 ProjectCount = 0;
    int32 DefaultCount = 0;
    int32 PromotedCorpusCount = 0;
    float SurvivalRate = 0.0f;
    double MeasuredSpread = 0.0;
};

/** Result of one deterministic roll, including provenance for the offline report. */
struct VOXELFORGE_API FVoxelStrateRollInfo
{
    bool bValid = false;
    FString FailureReason;

    ECaveGeneratorType Archetype = ECaveGeneratorType::TunnelNetwork;
    FVoxelStrateArchetypeParams ArchetypeParams;

    // Compatibility view for the original tunnel-only API. For a non-tunnel roll, the native
    // result is in ArchetypeParams and this member is not the active candidate vector.
    FStrateGenerationParams Params;

    TArray<int32> ParentEntryIndices;
    TArray<float> ParentWeights;
    int32 DominantParentPosition = INDEX_NONE;

    // Provenance for the starved-corpus fallback. Names are keyed as "archetype:field" so a
    // report can distinguish field descriptors from the number of times they were applied.
    int32 BootstrapJitterFieldCount = 0;
    TArray<FString> BootstrapJitterFieldNames;
};

#if WITH_EDITOR
/**
 * One candidate materialised by the same deterministic calls used by the offline composer tests.
 * This is an editor-only hand-off object: it is never a reflected asset and never part of the
 * shipping generation contract.
 */
struct VOXELFORGE_API FVoxelStrateComposerCandidate
{
    bool bValid = false;
    FString FailureReason;

    int32 Seed = 0;
    int32 Index = 0;
    bool bStructureRoll = false;

    ECaveGeneratorType Archetype = ECaveGeneratorType::TunnelNetwork;
    FVoxelStrateArchetypeParams ArchetypeParams;
    FVoxelOpStackRecipe Recipe;
    FVoxelStrateRegionManifest Regions;

    // Kept so automation/editor callers can audit the exact six independent structure blocks
    // without re-rolling them through a second implementation.
    FVoxelStrateRollInfo ParameterRoll;
    TArray<FVoxelStrateRollInfo> StructureBlockRolls;
};
#endif

/**
 * Known-good strate vectors plus their measured field spread.
 *
 * `LoadFromAssetRegistry` enumerates every project UVoxelStrateDefinition through the Asset
 * Registry, explicitly excluding Saved/Autosaves and Saved/Cooked copies. It also adds one
 * known-good default vector for every archetype/family in VoxelStrateTypes.h, then optionally
 * loads the cumulative promoted-record JSON store. Promoted records enter only after the caller's
 * world-specific verifier rebuilds and remeasures them; with no verifier they are skipped safely.
 * Parents are selected only inside the exact archetype group; no cross-archetype blend is attempted.
 */
class VOXELFORGE_API FVoxelStrateCorpus
{
public:
    void Reset();

    bool LoadFromAssetRegistry(
        FString& OutReport,
        const FString& PromotedStorePath = FString(),
        const IVoxelStratePromotionVerifier* PromotionVerifier = nullptr);

    /**
     * Compatibility alias retained for callers compiled against the first Tier 4a pass. The
     * settings argument is audited only for diagnostics; it is not a corpus input.
     */
    bool LoadFromSettings(const UVoxelSettings* Settings, FString& OutReport);
    bool LoadFromDefinitions(const TArray<UVoxelStrateDefinition*>& Definitions, FString& OutReport);

    /** Add an already-resolved vector, primarily for offline tools and focused tests. */
    bool AddEntry(const FString& SourcePath, const FString& SourceName,
                  ECaveGeneratorType Archetype, const FStrateGenerationParams& Params,
                  float Weight = 1.0f);
    bool AddEntry(const FString& SourcePath, const FString& SourceName,
                  ECaveGeneratorType Archetype, const FVoxelStrateArchetypeParams& Params,
                  float Weight = 1.0f);

    /** Load cumulative promoted records, re-verifying every record before it enters Entries. */
    bool LoadPromotedRecords(const FString& StorePath,
                             const IVoxelStratePromotionVerifier* PromotionVerifier,
                             FString& OutReport);

    /** Attach an offline measurement to an existing member without changing corpus identity. */
    bool SetMeasuredMetrics(const FString& SourcePath,
                            const FVoxelStrateMeasuredMetrics& Metrics);

    bool IsValid() const { return bSchemaValid && Entries.Num() > 0; }
    bool IsSchemaValid() const { return bSchemaValid; }
    const FString& GetSchemaError() const { return SchemaError; }

    int32 Num() const { return Entries.Num(); }
    const TArray<FVoxelStrateCorpusEntry>& GetEntries() const { return Entries; }
    const TArray<FVoxelStrateFieldSpread>& GetFieldSpreads() const { return FieldSpreads; }
    const TArray<FString>& GetSkippedDefinitions() const { return SkippedDefinitions; }

    int32 NumForArchetype(ECaveGeneratorType Archetype) const;
    int32 NumForProvenance(EVoxelStrateCorpusProvenance Provenance) const;

    const FVoxelStrateFieldSpread* FindSpread(
        ECaveGeneratorType Archetype, const FString& FieldName) const;
    const FVoxelStrateFieldSpread* FindSpread(const FString& FieldName) const;

    /** Stable hash of source paths, archetypes, weights, and every listed scalar corpus field. */
    uint32 GetContentsHash() const;

    /** Explicit exclusion list retained after the fixed-lattice terrain-detail liveness audit. */
    static const TArray<FVoxelStrateFieldExclusion>& GetNonTunableFields();

    /** True in an editor build where UPROPERTY metadata was available while spreads were built. */
    bool AreClampMetadataAvailableAtBuild() const { return bClampMetadataAvailableAtBuild; }

    /** False by policy: metadata is not promised to survive into a cooked runtime. */
    bool AreClampMetadataAvailableAtRuntime() const { return false; }

private:
    bool AddEntryInternal(const FString& SourcePath, const FString& SourceName,
                          ECaveGeneratorType Archetype,
                          const FVoxelStrateArchetypeParams& Params,
                          float Weight, EVoxelStrateCorpusProvenance Provenance,
                          const FVoxelOpStackRecipe* Recipe,
                          const FVoxelStrateMeasuredMetrics* Metrics,
                          const FString& PromotionRecordId,
                          int32 PromotionSeason,
                          int32 PromotionSeed,
                          int32 PromotionCandidateIndex,
                          uint32 PromotionInputCorpusHash,
                          bool bRebuild);
    void RebuildSpreads();

    TArray<FVoxelStrateCorpusEntry> Entries;
    TArray<FVoxelStrateFieldSpread> FieldSpreads;
    TArray<FString> SkippedDefinitions;
    FString SchemaError;
    bool bSchemaValid = true;
    bool bClampMetadataAvailableAtBuild = false;
};

/** Human-readable name used by the offline report. */
VOXELFORGE_API const TCHAR* VF_GetStrateArchetypeName(ECaveGeneratorType Archetype);
VOXELFORGE_API const TCHAR* VF_GetStrateCorpusProvenanceName(
    EVoxelStrateCorpusProvenance Provenance);

/** Full provenance result. Pure with respect to the corpus: no global or retained RNG state. */
VOXELFORGE_API FVoxelStrateRollInfo VF_RollStrateParamsDetailed(
    const FVoxelStrateCorpus& Corpus, int32 Seed, int32 Index);

/** Roll one exact native family, used to feed the independent blocks of a novel recipe. */
VOXELFORGE_API FVoxelStrateRollInfo VF_RollStrateParamsDetailedForArchetype(
    const FVoxelStrateCorpus& Corpus, ECaveGeneratorType Archetype, int32 Seed, int32 Index);

/** Pure structure roll: no retained RNG state and no corpus/runtime dependency. */
VOXELFORGE_API FVoxelOpStackRecipe VF_RollStrateStructure(int32 Seed, int32 Index);

/** Roll one-to-three independent region recipes/vectors plus their deterministic partition. */
VOXELFORGE_API FVoxelStrateRegionManifest VF_RollStrateRegionManifest(
    const FVoxelStrateCorpus& Corpus, int32 Seed, int32 StrateIndex, bool bRollStructure);

/**
 * ⛔ THE LATERAL-REGION GATE. Returns false, and must keep returning false until the seam law holds.
 *
 * Lateral regions (archetypes mixing inside one strate — "tunnel network leading to a big chamber")
 * are BUILT and MEASURED but NOT SHIPPABLE. Measured 2026-09-05: `arrival -> departure` across a
 * region seam passes **9 of 16 seeds**, against 11/16 for same-region controls in the same harness.
 *
 * Density blending alone does not guarantee a player can cross a seam. A strate that mixes archetypes
 * and strands you at the boundary is strictly worse than one that does not mix — so this stays off.
 *
 * ⚠️ DO NOT flip this to true to "enable the feature". The fix is an explicit corridor/landing
 * contract across seams (a design decision, deliberately not made unilaterally), after which the
 * measured rate must be 16/16. `VoxelForge.Composer.LateralRegions` asserts that this gate is shut
 * whenever the rate is imperfect, so flipping it without fixing the law turns the suite red.
 *
 * Safe today regardless: single-region output is bit-identical to legacy, and cross-boundary box
 * verdicts returned 0 violations over 199,800 brute-forced samples (all correctly `Mixed`).
 */
VOXELFORGE_API bool VF_LateralRegionsAreShippable();

/**
 * Provenance for a parameter block that does not read the authored corpus.
 *
 * NaiveUniform is the §3.3 control: every scalar is rolled independently inside the same
 * finite code-declared envelope used by the constraint arm. ConstraintSampled draws the
 * independent geometry quantities and derives the coupled quantities from generator equations.
 * Neither mode reads a strate asset or a corpus entry.
 */
enum class EVoxelStrateCorpusFreeSamplingMode : uint8
{
    NaiveUniform,
    ConstraintSampled,
};

/** Roll one native parameter family without consulting FVoxelStrateCorpus. */
VOXELFORGE_API FVoxelStrateRollInfo VF_RollStrateParamsCorpusFree(
    ECaveGeneratorType Archetype,
    int32 Seed,
    int32 Index,
    EVoxelStrateCorpusFreeSamplingMode Mode,
    float StrateHeightInVoxels);

/** Validate the hard geometry relations used by ConstraintSampled and the corpus audit. */
VOXELFORGE_API bool VF_ValidateStrateCorpusFreeConstraints(
    ECaveGeneratorType Archetype,
    const FVoxelStrateArchetypeParams& Params,
    float StrateHeightInVoxels,
    FString& OutViolation);

/** Normalized Euclidean distance in the five policy metric dimensions. */
VOXELFORGE_API double VF_NormalizedMeasuredMetricDistance(
    const FVoxelStrateMeasuredMetrics& A,
    const FVoxelStrateMeasuredMetrics& B,
    const FVoxelStratePromotionPolicy& Policy);

/** Select deterministic, validated, varied records for one season. */
VOXELFORGE_API FVoxelStratePromotionBatchResult VF_SelectStratePromotions(
    const FVoxelStrateCorpus& Corpus,
    const TArray<FVoxelStratePromotableRecord>& Candidates,
    const FVoxelStratePromotionPolicy& Policy);

/** Write/readable cumulative JSON store; records are sorted by RecordId before writing. */
VOXELFORGE_API bool VF_SaveStratePromotedRecords(
    const FString& StorePath,
    const TArray<FVoxelStratePromotableRecord>& Records,
    FString& OutReport);

/** Write the matching season summary beside the promoted-record store. */
VOXELFORGE_API bool VF_SaveStrateSeasonManifest(
    const FString& ManifestPath,
    const FVoxelStrateSeasonManifest& Manifest,
    FString& OutReport);

#if WITH_EDITOR
/**
 * Roll one candidate for the editor walk-through path. Parameter rolls call
 * VF_RollStrateParamsDetailed; structure rolls call VF_RollStrateStructure and the exact six
 * VF_RollStrateParamsDetailedForArchetype block rolls used by the structure test.
 */
VOXELFORGE_API FVoxelStrateComposerCandidate VF_RollStrateCandidate(
    const FVoxelStrateCorpus& Corpus, int32 Seed, int32 Index, bool bRollStructure);

/**
 * Build a parameter-roll candidate through the same archetype-to-stack mapping used by production
 * GetDensityAt/ClassifyTile. This is editor-only so the offline composer remains absent from a
 * shipping build; the result is used only for the post-apply density sanity check.
 */
VOXELFORGE_API bool VF_BuildNativeStrateStackForCandidate(
    ECaveGeneratorType Archetype,
    const FVoxelStrateArchetypeParams& Params,
    int32 Seed,
    float SpineRadius,
    float WorldRadiusVoxels,
    float EdgeSealThickness,
    const UVoxelStrateManager* StrateManager,
    FVoxelOpStack& OutStack,
    FVoxelOpContext& OutContext);

/** Build the persisted record from the exact editor candidate and its just-measured gate facts. */
VOXELFORGE_API FVoxelStratePromotableRecord VF_MakeStratePromotableRecord(
    const FVoxelStrateComposerCandidate& Candidate,
    const FVoxelStrateMetrics& Metrics,
    int32 Season,
    uint32 InputCorpusHash,
    bool bPassedNonVacuous,
    bool bPassedLargestComponent,
    bool bPassedPrimordialLaw);
#endif

/** Stable compact representation and equality/hash helpers for manifests and reports. */
VOXELFORGE_API FString VF_FormatStrateStructureRecipe(const FVoxelOpStackRecipe& Recipe);
VOXELFORGE_API uint32 VF_HashStrateStructureRecipe(const FVoxelOpStackRecipe& Recipe);
VOXELFORGE_API bool VF_AreStrateStructureRecipesIdentical(
    const FVoxelOpStackRecipe& A, const FVoxelOpStackRecipe& B);

/** The compact API requested by the composer design. */
VOXELFORGE_API FStrateGenerationParams VF_RollStrateParams(
    const FVoxelStrateCorpus& Corpus, int32 Seed, int32 Index);

/** Bitwise comparison over exactly the fields in FStrateGenerationParams::Lerp. */
VOXELFORGE_API bool VF_AreStrateParamsBitIdentical(
    const FStrateGenerationParams& A, const FStrateGenerationParams& B);

/** Bitwise comparison of the native vector for one archetype family. */
VOXELFORGE_API bool VF_AreStrateArchetypeParamsBitIdentical(
    const FVoxelStrateArchetypeParams& A,
    const FVoxelStrateArchetypeParams& B,
    ECaveGeneratorType Archetype);

/**
 * Normalised distance from the measured centroid of one exact-archetype corpus group.
 *
 * This is an offline ordering aid for the preview only. Continuous/integer fields are measured
 * in their corpus units; zero-spread fields use a conservative scale floor so the value remains
 * finite without turning clamps into roll ranges. It never participates in generation.
 */
VOXELFORGE_API double VF_DistanceFromStrateCorpusCentroid(
    const FVoxelStrateCorpus& Corpus,
    const FVoxelStrateArchetypeParams& Params,
    ECaveGeneratorType Archetype);
