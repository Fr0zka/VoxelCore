// Offline Tier 4c composition plus the runtime-readable, diffable Tier A manifest.

#pragma once

#include "CoreMinimal.h"
#include "VoxelStrateComposer.h"
#include "VoxelStrateMeasure.h"

class UVoxelSettings;

/** Why the provisional season selector put one surviving candidate in a slot. */
enum class EVoxelSeasonSelectionReason : uint8
{
    Grounded,
    Outlier,
    Fixed,
};

/**
 * Explicit, reviewable selection knobs for one season.
 *
 * None of these values is a settled definition of quality. They are the first policy Jahni can
 * change after walking the review page. Hard validation remains separate from this policy.
 */
struct VOXELFORGE_API FVoxelSeasonSelectionPolicy
{
    // Placeholder mix only: ordinary strates give an outlier contrast. Revisit after the first
    // season review; this is not a learned or final quality score.
    float GroundedFraction = 0.75f;
    float OutlierFraction = 0.25f;

    // Below this measured distance two neighbours look too similar. The selector subtracts a
    // penalty proportional to the gap; provisional because the useful contrast is visual.
    double AdjacentSimilarityDistanceThreshold = 0.20;
    double AdjacentSimilarityPenaltyWeight = 1.0;

    // Rewards an unseen archetype/recipe in the already chosen prefix. These are variety
    // tie-breakers, not claims that one archetype or recipe is better. Provisional review knobs.
    double NewArchetypeBonus = 0.10;
    double NewRecipeBonus = 0.05;

    // GDD §6 says roughly every five levels, and a boss is intermediary rather than terminal.
    // This interval is therefore an explicit provisional layout knob, not a quality score.
    int32 BossSlotInterval = 5;

    // Tier 2 hard gate: a surviving air component must retain at least this share. Provisional
    // only in its threshold; the existence of the gate is non-negotiable for a shippable season.
    float MinimumLargestComponentShare = 0.50f;

    // Normalizers for the two unbounded measured dimensions in the adjacency distance. Fractions
    // already live in [0,1]. These physical scales are provisional and visible in the manifest.
    float FeatureScaleNormalizationVoxels = 256.0f;
    float ClearanceNormalizationVoxels = 64.0f;
};

/** A caller-supplied fixed slot for tests/tools that already have a complete manifest record. */
struct VOXELFORGE_API FVoxelSeasonFixedStrate
{
    int32 Seed = 0;
    int32 HeightInChunks = 8;
    ECaveGeneratorType Archetype = ECaveGeneratorType::TunnelNetwork;
    FVoxelStrateArchetypeParams Params;
    FVoxelOpStackRecipe Recipe;
    FVoxelStrateRegionManifest Regions;
    bool bUsesRecipe = true;
    bool bUsesRegions = false;
    FString SourceDefinitionPath;
};

/** Settings for the offline build-box pass. No member is consulted by normal runtime generation. */
struct VOXELFORGE_API FVoxelSeasonCompositionSettings
{
    // Optional live-world settings. TotalStrates, spine/gap values, and FixedStrates are read
    // from it when present. The pointer is borrowed for the duration of VF_ComposeSeason only.
    const UVoxelSettings* WorldSettings = nullptr;

    // Optional preloaded corpus. With nullptr the composer loads the project asset corpus on the
    // build box, exactly like the existing offline composer tests.
    const FVoxelStrateCorpus* Corpus = nullptr;

    // 24 gives a useful minute-scale automation pass at the default coarse measurement window.
    // Production can raise this to the overnight budget without changing the policy.
    int32 CandidateCount = 24;
    int32 SelectedStrateCount = 10;

    // Generated candidates use one stable height. Fixed authored slots may provide their own
    // height. World layout bounds are written into every selected parameter family below.
    int32 StrateHeightInChunks = 8;
    int32 InterStrateGapChunks = 0;
    float OriginSpineRadius = 14.0f;
    float WorldRadiusVoxels = 0.0f;

    // Coarse measurement is deliberately bounded. Tests may reduce the window; production may
    // spend the overnight budget on a finer pass after the selector has been judged.
    FVoxelStrateMeasureSettings MeasureSettings;
    // Player-fit is a separate, explicitly fine pass. SampleStep values other than one are
    // refused by the metric, never interpreted as an approximate capsule answer.
    FVoxelStrateMeasureSettings PlayerFitMeasureSettings;
    FVoxelSeasonSelectionPolicy SelectionPolicy;

    // Explicit composed fixed slots take precedence. When empty, the composer adapts the
    // existing UVoxelSettings::FixedStrates map, preserving its absolute slot indices.
    TMap<int32, FVoxelSeasonFixedStrate> FixedStrates;

    // Empty means ProjectSavedDir()/VoxelForge/Seasons. Files are never written to the runtime
    // content directory by this API.
    FString OutputDirectory;
    bool bWriteArtifacts = true;
    bool bWriteReviewPage = true;
    bool bIncludeCandidateAudit = true;

    // Informational only; when WorldSettings is present CurrentSeason supplies the default.
    int32 SeasonNumber = 0;

    FVoxelSeasonCompositionSettings()
    {
        // The existing Tier 4b test uses step 4. Season composition defaults to step 8 so the
        // default automation artifact is measured in minutes, while MaxCells remains bounded.
        MeasureSettings.SampleStep = 8;
        MeasureSettings.RadiusInVoxels = 256;
        MeasureSettings.MaxCells = 8000000;
        MeasureSettings.MaxRouteRetries = 16;
        MeasureSettings.HeadroomCells = 2;
        MeasureSettings.InteriorMarginVoxels = -1;

        PlayerFitMeasureSettings.SampleStep = 1;
        PlayerFitMeasureSettings.RadiusInVoxels = 64;
        // A fitted mouth-to-mouth XY window across an 8-chunk (256-voxel) strate can approach
        // four million cells. Keep the cap finite and below the old multi-gigabyte retry cost.
        PlayerFitMeasureSettings.MaxCells = 4000000;
        PlayerFitMeasureSettings.MaxRouteRetries = 16;
        PlayerFitMeasureSettings.HeadroomCells = 2;
        PlayerFitMeasureSettings.InteriorMarginVoxels = -1;
    }
};

// Friendly aliases for callers that use the task's shorter naming.
using FVoxelSeasonComposerSettings = FVoxelSeasonCompositionSettings;
using FVoxelSeasonComposeSettings = FVoxelSeasonCompositionSettings;

/** Compact audit row; the full vectors live only for selected strates. */
struct VOXELFORGE_API FVoxelSeasonCandidateAudit
{
    int32 CandidateIndex = INDEX_NONE;
    int32 Seed = 0;
    ECaveGeneratorType Archetype = ECaveGeneratorType::TunnelNetwork;
    uint32 RecipeHash = 0;
    double DistanceFromCorpusCentroid = 0.0;
    bool bPassedHardGates = false;
    bool bSelected = false;
    FString RejectionReason;
};

/** Count of one explicit hard-gate or final-placement rejection reason. */
struct VOXELFORGE_API FVoxelSeasonRejectionCount
{
    FString Reason;
    int32 Count = 0;
};

/** One ordered Tier A spine entry. */
struct VOXELFORGE_API FVoxelSeasonStrate
{
    // DepthIndex is the absolute world-layout slot. CandidateIndex is the offline attempt id.
    int32 DepthIndex = INDEX_NONE;
    int32 CandidateIndex = INDEX_NONE;
    int32 Seed = 0;
    int32 HeightInChunks = 0;
    int32 TopWorldZ = 0;
    int32 BottomWorldZ = 0;

    ECaveGeneratorType Archetype = ECaveGeneratorType::TunnelNetwork;
    FVoxelOpStackRecipe Recipe;
    FVoxelStrateArchetypeParams Params;
    // When true this is the complete lateral parent description. Params/Archetype remain the
    // compatibility identity of region zero; runtime rebuilds use Regions and never blend params.
    FVoxelStrateRegionManifest Regions;
    FVoxelStrateMeasuredMetrics Metrics;

    double DistanceFromCorpusCentroid = 0.0;
    FString SelectionReason;
    FString SourceDefinitionPath;

    bool bGrounded = false;
    bool bOutlier = false;
    bool bBossSlot = false;
    bool bFixed = false;
    bool bUsesRecipe = true;
    bool bUsesRegions = false;

    bool bPassedNonVacuous = false;
    bool bPassedLargestComponent = false;
    bool bPassedPrimordialLaw = false;
    EVoxelConnectivityResult PrimordialLawResult = EVoxelConnectivityResult::OutOfWindow;
};

/**
 * The stored Tier A season artifact.
 *
 * Tier B is intentionally absent: its hash-lattice field remains an arithmetic/enumeration API,
 * not a second stored table. Strates is ordered from the surface down and is the only finite spine
 * persisted by this task.
 */
struct VOXELFORGE_API FVoxelSeasonManifest
{
    static constexpr int32 CurrentSchemaVersion = 2;

    bool bValid = false;
    int32 SchemaVersion = CurrentSchemaVersion;
    FString Error;

    int32 Season = 0;
    int32 Seed = 0;
    uint32 InputCorpusHash = 0;
    // SHA-1 of the canonical manifest JSON with this field omitted.  This detects stale cooked
    // season assets without requiring the editor-only input corpus on a player's machine.
    FString ContentHash;
    int32 CandidateCount = 0;
    int32 SurvivorCount = 0;
    int32 SelectedCount = 0;
    int32 RejectedCount = 0;
    int32 GroundedSelectedCount = 0;
    int32 OutlierSelectedCount = 0;
    int32 FixedSelectedCount = 0;
    int32 TotalStrates = 0;
    int32 InterStrateGapChunks = 0;
    float OriginSpineRadius = 0.0f;
    float WorldRadiusVoxels = 0.0f;

    FVoxelSeasonSelectionPolicy SelectionPolicy;
    FString PolicyNote;

    TArray<FVoxelSeasonStrate> Strates;
    TArray<FVoxelSeasonCandidateAudit> CandidateAudit;
    TArray<FVoxelSeasonRejectionCount> RejectionCounts;

    // Derived artifact locations; intentionally not serialized into the portable JSON.
    FString ManifestPath;
    FString ReviewPagePath;
    FString SerializedJson;

    bool IsUsable() const
    {
        if (!bValid || !Error.IsEmpty() || ContentHash.Len() != 40 || WorldRadiusVoxels != 0.0f
            || Strates.Num() != SelectedCount || SelectedCount <= 0)
        {
            return false;
        }
        for (const FVoxelSeasonStrate& Strate : Strates)
        {
            // Region composition is intentionally not a shippable season format yet.  Keeping
            // this rejection in the portable manifest contract prevents an editor experiment
            // from becoming packaged terrain merely because its density evaluator exists.
            if (Strate.bUsesRegions)
            {
                return false;
            }
        }
        return true;
    }
};

/** Human-readable enum names used by JSON and the review page. */
VOXELFORGE_API const TCHAR* VF_GetVoxelSeasonSelectionReasonName(
    EVoxelSeasonSelectionReason Reason);

/** Compose, validate, select, serialize, and (by default) write one season. Editor/build-box only. */
#if WITH_EDITOR
VOXELFORGE_API FVoxelSeasonManifest VF_ComposeSeason(
    int32 SeasonSeed, const FVoxelSeasonCompositionSettings& Settings);

/** Convenience overload that adapts the existing runtime settings asset. */
VOXELFORGE_API FVoxelSeasonManifest VF_ComposeSeason(
    int32 SeasonSeed, const UVoxelSettings* WorldSettings);
#endif

/** Serialize/read the complete Tier A manifest as stable pretty JSON. */
VOXELFORGE_API FString VF_SerializeVoxelSeasonManifest(
    const FVoxelSeasonManifest& Manifest);
/** Parse an in-memory/cooked JSON manifest and verify its embedded content hash. */
VOXELFORGE_API bool VF_DeserializeVoxelSeasonManifest(
    const FString& ManifestJson, FVoxelSeasonManifest& OutManifest, FString& OutReport);
/** Stable SHA-1 over the canonical manifest payload (the content_hash field itself is omitted). */
VOXELFORGE_API FString VF_ComputeVoxelSeasonManifestContentHash(
    const FVoxelSeasonManifest& Manifest);
VOXELFORGE_API bool VF_SaveVoxelSeasonManifest(
    const FString& ManifestPath, const FVoxelSeasonManifest& Manifest, FString& OutReport);
VOXELFORGE_API bool VF_LoadVoxelSeasonManifest(
    const FString& ManifestPath, FVoxelSeasonManifest& OutManifest, FString& OutReport);

/** Rebuild exactly the stack represented by one manifest entry. */
VOXELFORGE_API bool VF_BuildSeasonStrateStack(
    const FVoxelSeasonStrate& Strate,
    float SpineRadius,
    const UVoxelStrateManager* StrateManager,
    FVoxelOpStack& OutStack,
    FVoxelOpContext& OutContext,
    FString* OutError = nullptr);

/** Verbose alias for tests/tools that want the round-trip operation named explicitly. */
VOXELFORGE_API bool VF_RebuildVoxelSeasonStrate(
    const FVoxelSeasonStrate& Strate,
    float SpineRadius,
    const UVoxelStrateManager* StrateManager,
    FVoxelOpStack& OutStack,
    FVoxelOpContext& OutContext,
    FString* OutError = nullptr);
