// Offline strate corpus and parameter composer.
//
// This is deliberately not part of the runtime generation path.  The composer is a build-box
// tool/test API: it loads authored vectors, measures their spread, and makes deterministic
// candidates by blending those vectors before applying a small spread-relative jitter.

#pragma once

#include "CoreMinimal.h"
#include "VoxelStrateTypes.h"

class UVoxelSettings;
class UVoxelStrateDefinition;

/** The kind of value represented by one entry of VF_STRATE_PARAM_FIELDS. */
enum class EVoxelStrateFieldKind : uint8
{
    Continuous,
    Integer,
    Boolean,
    Enum,
};

/** A field deliberately kept out of the parameter roll. */
struct VOXELFORGE_API FVoxelStrateFieldExclusion
{
    FString FieldName;
    FString Reason;
};

/** Measured statistics for one field across the currently loaded corpus. */
struct VOXELFORGE_API FVoxelStrateFieldSpread
{
    FString FieldName;
    EVoxelStrateFieldKind Kind = EVoxelStrateFieldKind::Continuous;
    bool bExcluded = false;
    bool bReflected = false;

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

/** One known-good authored vector and the archetype that gives it meaning. */
struct VOXELFORGE_API FVoxelStrateCorpusEntry
{
    FString SourcePath;
    FString SourceName;
    ECaveGeneratorType Archetype = ECaveGeneratorType::TunnelNetwork;
    FStrateGenerationParams Params;

    // The current asset format has no corpus weight field. Loaded hand-authored entries therefore
    // use 1.0. The field remains explicit so a future offline corpus can assign weights without
    // changing the roll algorithm.
    float Weight = 1.0f;
};

/** Result of one deterministic roll, including provenance for the offline report. */
struct VOXELFORGE_API FVoxelStrateRollInfo
{
    bool bValid = false;
    FString FailureReason;

    ECaveGeneratorType Archetype = ECaveGeneratorType::TunnelNetwork;
    FStrateGenerationParams Params;

    TArray<int32> ParentEntryIndices;
    TArray<float> ParentWeights;
    int32 DominantParentPosition = INDEX_NONE;
};

/**
 * Known-good strate vectors plus their measured field spread.
 *
 * `LoadFromSettings` walks both the pool and fixed-strate references, resolves each definition,
 * de-duplicates it by asset path, and admits only TunnelNetwork/Underwater definitions because
 * those are the two archetypes whose authored vector is `GenerationParams`. Other archetypes have
 * different parameter structs; treating their default `GenerationParams` as authored data would
 * manufacture a false corpus.
 */
class VOXELFORGE_API FVoxelStrateCorpus
{
public:
    void Reset();

    bool LoadFromSettings(const UVoxelSettings* Settings, FString& OutReport);
    bool LoadFromDefinitions(const TArray<UVoxelStrateDefinition*>& Definitions, FString& OutReport);

    /** Add an already-resolved vector, primarily for offline tools and focused tests. */
    bool AddEntry(const FString& SourcePath, const FString& SourceName,
                  ECaveGeneratorType Archetype, const FStrateGenerationParams& Params,
                  float Weight = 1.0f);

    bool IsValid() const { return bSchemaValid && Entries.Num() > 0; }
    bool IsSchemaValid() const { return bSchemaValid; }
    const FString& GetSchemaError() const { return SchemaError; }

    int32 Num() const { return Entries.Num(); }
    const TArray<FVoxelStrateCorpusEntry>& GetEntries() const { return Entries; }
    const TArray<FVoxelStrateFieldSpread>& GetFieldSpreads() const { return FieldSpreads; }
    const TArray<FString>& GetSkippedDefinitions() const { return SkippedDefinitions; }

    const FVoxelStrateFieldSpread* FindSpread(const FString& FieldName) const;

    /** Stable hash of source paths, archetypes, weights, and every listed scalar field. */
    uint32 GetContentsHash() const;

    /** Explicit exclusion list discovered from the struct reflection/use audit. */
    static const TArray<FVoxelStrateFieldExclusion>& GetNonTunableFields();

    /** True in an editor build where UPROPERTY metadata was available while spreads were built. */
    bool AreClampMetadataAvailableAtBuild() const { return bClampMetadataAvailableAtBuild; }

    /** False by policy: metadata is not promised to survive into a cooked runtime. */
    bool AreClampMetadataAvailableAtRuntime() const { return false; }

private:
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

/** Full provenance result. Pure with respect to the corpus: no global or retained RNG state. */
VOXELFORGE_API FVoxelStrateRollInfo VF_RollStrateParamsDetailed(
    const FVoxelStrateCorpus& Corpus, int32 Seed, int32 Index);

/** The compact API requested by the composer design. */
VOXELFORGE_API FStrateGenerationParams VF_RollStrateParams(
    const FVoxelStrateCorpus& Corpus, int32 Seed, int32 Index);

/** Bitwise comparison over exactly the fields in FStrateGenerationParams::Lerp. */
VOXELFORGE_API bool VF_AreStrateParamsBitIdentical(
    const FStrateGenerationParams& A, const FStrateGenerationParams& B);

