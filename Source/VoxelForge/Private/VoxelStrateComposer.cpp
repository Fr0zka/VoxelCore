// Offline strate corpus and parameter roll.

#include "VoxelStrateComposer.h"

#include "Math/RandomStream.h"
#include "Misc/Crc.h"
#include "UObject/FieldIterator.h"
#include "UObject/UnrealType.h"

#include "VoxelSettings.h"
#include "VoxelStrateDefinition.h"

#include <cmath>
#include <type_traits>
#include <utility>

namespace
{
    constexpr float GJitterFraction = 0.15f;

    template <typename T>
    EVoxelStrateFieldKind VF_FieldKindFor()
    {
        using ValueType = typename std::remove_cv<
            typename std::remove_reference<T>::type>::type;

        if (std::is_same<ValueType, bool>::value)
        {
            return EVoxelStrateFieldKind::Boolean;
        }
        if (std::is_enum<ValueType>::value)
        {
            return EVoxelStrateFieldKind::Enum;
        }
        if (std::is_integral<ValueType>::value)
        {
            return EVoxelStrateFieldKind::Integer;
        }
        return EVoxelStrateFieldKind::Continuous;
    }

    const FVoxelStrateFieldExclusion* VF_FindExclusion(const FString& FieldName)
    {
        for (const FVoxelStrateFieldExclusion& Exclusion : FVoxelStrateCorpus::GetNonTunableFields())
        {
            if (Exclusion.FieldName == FieldName)
            {
                return &Exclusion;
            }
        }
        return nullptr;
    }

    void VF_AddExclusion(TArray<FVoxelStrateFieldExclusion>& Out,
                         const TCHAR* FieldName, const TCHAR* Reason)
    {
        FVoxelStrateFieldExclusion& Entry = Out.AddDefaulted_GetRef();
        Entry.FieldName = FieldName;
        Entry.Reason = Reason;
    }

    void VF_CollectGenerationProperties(TMap<FString, FProperty*>& OutProperties)
    {
        OutProperties.Reset();
        UStruct* Struct = FStrateGenerationParams::StaticStruct();
        for (TFieldIterator<FProperty> It(Struct, EFieldIteratorFlags::IncludeSuper); It; ++It)
        {
            FProperty* Property = *It;
            if (Property != nullptr)
            {
                OutProperties.Add(Property->GetName(), Property);
            }
        }
    }

    void VF_AddPropertyMetadata(FVoxelStrateFieldSpread& Spread, const FProperty* Property)
    {
#if WITH_EDITOR
        if (Property == nullptr)
        {
            return;
        }

        const FString ClampMinText = Property->GetMetaData(TEXT("ClampMin"));
        const FString ClampMaxText = Property->GetMetaData(TEXT("ClampMax"));

        if (!ClampMinText.IsEmpty())
        {
            const double Value = FCString::Atod(*ClampMinText);
            if (FMath::IsFinite(Value))
            {
                Spread.bHasClampMin = true;
                Spread.ClampMin = Value;
            }
        }
        if (!ClampMaxText.IsEmpty())
        {
            const double Value = FCString::Atod(*ClampMaxText);
            if (FMath::IsFinite(Value))
            {
                Spread.bHasClampMax = true;
                Spread.ClampMax = Value;
            }
        }
#else
        // UPROPERTY metadata is not a cooked-runtime contract. The composer is an offline tool;
        // runtime/cook-time clamp data must be baked by a future commandlet.
        (void)Spread;
        (void)Property;
#endif
    }

    void VF_AddSpreadDescriptor(TArray<FVoxelStrateFieldSpread>& OutSpreads,
                                TMap<FString, int32>& OutIndices,
                                const TMap<FString, FProperty*>& Properties,
                                const TCHAR* Name, EVoxelStrateFieldKind Kind)
    {
        FVoxelStrateFieldSpread& Spread = OutSpreads.AddDefaulted_GetRef();
        Spread.FieldName = Name;
        Spread.Kind = Kind;
        Spread.bExcluded = VF_FindExclusion(Spread.FieldName) != nullptr;

        if (const FProperty* const* Property = Properties.Find(Spread.FieldName))
        {
            Spread.bReflected = (*Property != nullptr);
            VF_AddPropertyMetadata(Spread, *Property);
        }

        OutIndices.Add(Spread.FieldName, OutSpreads.Num() - 1);
    }

    void VF_AccumulateValue(const TCHAR* Name, double Value,
                            const TMap<FString, int32>& Indices,
                            TArray<TArray<double>>& Values)
    {
        const int32* Index = Indices.Find(FString(Name));
        if (Index != nullptr && Values.IsValidIndex(*Index) && FMath::IsFinite(Value))
        {
            Values[*Index].Add(Value);
        }
    }

    void VF_ResetNonTunableFields(FStrateGenerationParams& Params)
    {
        // These fields are deliberately not inherited from a parent, jittered, or clamped. The
        // first group is terrain-op transport populated per room by UVoxelTerrainOpDefinition;
        // the last two are manager-owned runtime Z bounds.
        Params.TerraceStepHeight = 0.0f;
        Params.TerraceHardness = 0.0f;
        Params.TerraceNoiseDisplacement = 0.0f;
        Params.LayerLineSpacing = 0.0f;
        Params.LayerLineDepth = 0.0f;
        Params.OverhangStrength = 0.0f;
        Params.OverhangDepth = 0.0f;
        Params.OverhangFrequency = 0.0f;
        Params.RibbingSpacing = 0.0f;
        Params.RibbingDepth = 0.0f;
        Params.CliffStrength = 0.0f;
        Params.ScallopStrength = 0.0f;
        Params.ScallopFrequency = 0.0f;
        Params.ArchDensity = 0.0f;
        Params.ArchMinRadius = 0.0f;
        Params.ArchMaxRadius = 0.0f;
        Params.ColumnDensity = 0.0f;
        Params.ColumnMinRadius = 0.0f;
        Params.ColumnMaxRadius = 0.0f;
        Params.PitDensity = 0.0f;
        Params.PitMinRadius = 0.0f;
        Params.PitMaxRadius = 0.0f;
        Params.PitDepth = 0.0f;
        Params.ChimneyDensity = 0.0f;
        Params.ChimneyMinRadius = 0.0f;
        Params.ChimneyMaxRadius = 0.0f;
        Params.ChimneyHeight = 0.0f;
        Params.DomeDensity = 0.0f;
        Params.DomeMinRadius = 0.0f;
        Params.DomeMaxRadius = 0.0f;
        Params.DomeHeightRatio = 0.0f;
        Params.PinchDensity = 0.0f;
        Params.PinchStrength = 0.0f;
        Params.PinchLength = 0.0f;
        Params.StrateTopWorldZ = 0.0f;
        Params.StrateBottomWorldZ = 0.0f;
    }

    void VF_JitterFloat(float& Value, const FVoxelStrateFieldSpread* Spread, FRandomStream& Rng)
    {
        if (Spread == nullptr || Spread->bExcluded || Spread->Kind != EVoxelStrateFieldKind::Continuous)
        {
            return;
        }

        const float CorpusRange = (float)FMath::Max(0.0, Spread->Max - Spread->Min);
        if (CorpusRange > 0.0f)
        {
            Value += (Rng.FRand() * 2.0f - 1.0f) * GJitterFraction * CorpusRange;
        }
    }

    void VF_ClampFloat(float& Value, const FVoxelStrateFieldSpread* Spread)
    {
        if (Spread == nullptr || Spread->bExcluded)
        {
            return;
        }
        if (Spread->bHasClampMin)
        {
            Value = FMath::Max(Value, (float)Spread->ClampMin);
        }
        if (Spread->bHasClampMax)
        {
            Value = FMath::Min(Value, (float)Spread->ClampMax);
        }
    }

    void VF_ClampInt(int32& Value, const FVoxelStrateFieldSpread* Spread)
    {
        if (Spread == nullptr || Spread->bExcluded)
        {
            return;
        }
        if (Spread->bHasClampMin)
        {
            Value = FMath::Max(Value, FMath::CeilToInt((float)Spread->ClampMin));
        }
        if (Spread->bHasClampMax)
        {
            Value = FMath::Min(Value, FMath::FloorToInt((float)Spread->ClampMax));
        }
    }

    void VF_RepairOrderedPair(float& MinValue, float& MaxValue)
    {
        // Convex blending preserves these relations, but independent jitter is allowed to cross
        // one. Repairing the pair after the jitter keeps the relation without inventing a new
        // range or touching generation code.
        if (MinValue > MaxValue)
        {
            Swap(MinValue, MaxValue);
        }
    }

    uint32 VF_Avalanche(uint32 Value)
    {
        Value ^= Value >> 16;
        Value *= 0x7feb352dU;
        Value ^= Value >> 15;
        Value *= 0x846ca68bU;
        Value ^= Value >> 16;
        return Value;
    }

    uint32 VF_RollSeed(uint32 CorpusHash, int32 Seed, int32 Index)
    {
        uint32 Value = CorpusHash ^ 0x9e3779b9U;
        Value = VF_Avalanche(Value ^ (uint32)Seed);
        Value = VF_Avalanche(Value ^ ((uint32)Index + 0x85ebca6bU));
        return Value;
    }

    struct FParentGroup
    {
        ECaveGeneratorType Archetype = ECaveGeneratorType::TunnelNetwork;
        TArray<int32> EntryIndices;
    };

    int32 VF_WeightedPick(const TArray<int32>& Candidates,
                          const TArray<FVoxelStrateCorpusEntry>& Entries,
                          FRandomStream& Rng)
    {
        if (Candidates.Num() == 0)
        {
            return INDEX_NONE;
        }

        float TotalWeight = 0.0f;
        for (const int32 Candidate : Candidates)
        {
            if (Entries.IsValidIndex(Candidate))
            {
                TotalWeight += FMath::Max(0.0f, Entries[Candidate].Weight);
            }
        }

        if (TotalWeight <= 0.0f)
        {
            return Candidates[Rng.RandRange(0, Candidates.Num() - 1)];
        }

        float Cursor = Rng.FRand() * TotalWeight;
        for (const int32 Candidate : Candidates)
        {
            if (!Entries.IsValidIndex(Candidate))
            {
                continue;
            }
            Cursor -= FMath::Max(0.0f, Entries[Candidate].Weight);
            if (Cursor <= 0.0f)
            {
                return Candidate;
            }
        }
        return Candidates.Last();
    }

    FVoxelStrateRollInfo VF_RollInternal(const FVoxelStrateCorpus& Corpus,
                                         int32 Seed, int32 Index)
    {
        FVoxelStrateRollInfo Result;
        if (!Corpus.IsValid())
        {
            Result.FailureReason = Corpus.IsSchemaValid()
                ? TEXT("The corpus is empty.")
                : Corpus.GetSchemaError();
            return Result;
        }

        const TArray<FVoxelStrateCorpusEntry>& Entries = Corpus.GetEntries();
        TArray<FParentGroup> Groups;
        for (int32 EntryIndex = 0; EntryIndex < Entries.Num(); ++EntryIndex)
        {
            int32 GroupIndex = INDEX_NONE;
            for (int32 CandidateGroup = 0; CandidateGroup < Groups.Num(); ++CandidateGroup)
            {
                if (Groups[CandidateGroup].Archetype == Entries[EntryIndex].Archetype)
                {
                    GroupIndex = CandidateGroup;
                    break;
                }
            }
            if (GroupIndex == INDEX_NONE)
            {
                FParentGroup& Group = Groups.AddDefaulted_GetRef();
                Group.Archetype = Entries[EntryIndex].Archetype;
                GroupIndex = Groups.Num() - 1;
            }
            Groups[GroupIndex].EntryIndices.Add(EntryIndex);
        }

        if (Groups.Num() == 0)
        {
            Result.FailureReason = TEXT("The corpus contains no usable archetype group.");
            return Result;
        }

        FRandomStream Rng((int32)VF_RollSeed(Corpus.GetContentsHash(), Seed, Index));

        float TotalGroupWeight = 0.0f;
        for (const FParentGroup& Group : Groups)
        {
            for (const int32 EntryIndex : Group.EntryIndices)
            {
                TotalGroupWeight += FMath::Max(0.0f, Entries[EntryIndex].Weight);
            }
        }

        int32 GroupIndex = 0;
        if (TotalGroupWeight > 0.0f)
        {
            float Cursor = Rng.FRand() * TotalGroupWeight;
            for (int32 CandidateGroup = 0; CandidateGroup < Groups.Num(); ++CandidateGroup)
            {
                float GroupWeight = 0.0f;
                for (const int32 EntryIndex : Groups[CandidateGroup].EntryIndices)
                {
                    GroupWeight += FMath::Max(0.0f, Entries[EntryIndex].Weight);
                }
                Cursor -= GroupWeight;
                if (Cursor <= 0.0f)
                {
                    GroupIndex = CandidateGroup;
                    break;
                }
            }
        }
        else
        {
            GroupIndex = Rng.RandRange(0, Groups.Num() - 1);
        }

        const FParentGroup& Group = Groups[GroupIndex];
        const int32 ParentCount = Rng.RandRange(2, 3);
        TArray<int32> Available = Group.EntryIndices;
        Result.ParentEntryIndices.Reserve(ParentCount);
        Result.ParentWeights.Reserve(ParentCount);

        for (int32 ParentSlot = 0; ParentSlot < ParentCount; ++ParentSlot)
        {
            // Once a group is exhausted, replacement is intentional. It keeps the requested
            // 2–3-parent algorithm defined for a one-entry corpus and makes that limitation
            // visible in the provenance list rather than silently inventing a second vector.
            const TArray<int32>& Candidates = Available.Num() > 0 ? Available : Group.EntryIndices;
            const int32 ParentIndex = VF_WeightedPick(Candidates, Entries, Rng);
            if (ParentIndex == INDEX_NONE)
            {
                Result.FailureReason = TEXT("Parent selection failed for a non-empty group.");
                return Result;
            }
            Result.ParentEntryIndices.Add(ParentIndex);
            Result.ParentWeights.Add(0.1f + Rng.FRand());
            Available.Remove(ParentIndex);
        }

        float WeightSum = 0.0f;
        for (const float Weight : Result.ParentWeights)
        {
            WeightSum += Weight;
        }
        if (!(WeightSum > 0.0f))
        {
            Result.FailureReason = TEXT("Parent blend weights were not positive.");
            return Result;
        }
        for (float& Weight : Result.ParentWeights)
        {
            Weight /= WeightSum;
        }

        Result.DominantParentPosition = 0;
        for (int32 ParentSlot = 1; ParentSlot < Result.ParentWeights.Num(); ++ParentSlot)
        {
            if (Result.ParentWeights[ParentSlot] > Result.ParentWeights[Result.DominantParentPosition])
            {
                Result.DominantParentPosition = ParentSlot;
            }
        }

        FStrateGenerationParams Params = Entries[Result.ParentEntryIndices[0]].Params;
        float AccumulatedWeight = Result.ParentWeights[0];
        for (int32 ParentSlot = 1; ParentSlot < Result.ParentEntryIndices.Num(); ++ParentSlot)
        {
            const float ThisWeight = Result.ParentWeights[ParentSlot];
            const float Alpha = ThisWeight / (AccumulatedWeight + ThisWeight);
            Params = FStrateGenerationParams::Lerp(
                Params, Entries[Result.ParentEntryIndices[ParentSlot]].Params, Alpha);
            AccumulatedWeight += ThisWeight;
        }

        // Discrete values are not jittered. Lerp already snaps int/enum fields at 0.5; bools use
        // the explicit dominant-parent policy because a boolean has no meaningful linear blend.
        const int32 DominantParent = Result.ParentEntryIndices[Result.DominantParentPosition];
        Params.bTunnelsFlowTowardOrigin = Entries[DominantParent].Params.bTunnelsFlowTowardOrigin;

#define VF_COMPOSER_JITTER_LERPF(Name) \
        VF_JitterFloat(Params.Name, Corpus.FindSpread(TEXT(#Name)), Rng);
#define VF_COMPOSER_JITTER_SNAP(Name)
        VF_STRATE_PARAM_FIELDS(VF_COMPOSER_JITTER_LERPF, VF_COMPOSER_JITTER_SNAP)
#undef VF_COMPOSER_JITTER_LERPF
#undef VF_COMPOSER_JITTER_SNAP

        // Clamp metadata is consulted only after blend+jitter. It is a hard editor-build safety
        // net, not a distribution range.
#define VF_COMPOSER_CLAMP_LERPF(Name) \
        VF_ClampFloat(Params.Name, Corpus.FindSpread(TEXT(#Name)));
#define VF_COMPOSER_CLAMP_INT(Name) \
        VF_ClampInt(Params.Name, Corpus.FindSpread(TEXT(#Name)));
#define VF_COMPOSER_CLAMP_SNAP_OriginRoomMaxConnections() \
        VF_ClampInt(Params.OriginRoomMaxConnections, Corpus.FindSpread(TEXT("OriginRoomMaxConnections")));
#define VF_COMPOSER_CLAMP_SNAP_bTunnelsFlowTowardOrigin()
#define VF_COMPOSER_CLAMP_SNAP_RoughnessNoiseType()
#define VF_COMPOSER_CLAMP_SNAP(Name) VF_COMPOSER_CLAMP_SNAP_##Name()
        /* The SNAP expansion is dispatched field-by-field because the list contains one int,
         * one bool, and one enum. */
        VF_STRATE_PARAM_FIELDS(VF_COMPOSER_CLAMP_LERPF, VF_COMPOSER_CLAMP_SNAP)
#undef VF_COMPOSER_CLAMP_LERPF
#undef VF_COMPOSER_CLAMP_INT
#undef VF_COMPOSER_CLAMP_SNAP
#undef VF_COMPOSER_CLAMP_SNAP_OriginRoomMaxConnections
#undef VF_COMPOSER_CLAMP_SNAP_bTunnelsFlowTowardOrigin
#undef VF_COMPOSER_CLAMP_SNAP_RoughnessNoiseType

        VF_RepairOrderedPair(Params.MinRoomRadius, Params.MaxRoomRadius);
        VF_RepairOrderedPair(Params.RoomFloorCutMin, Params.RoomFloorCutMax);
        VF_RepairOrderedPair(Params.TunnelMinRadius, Params.TunnelMaxRadius);
        VF_RepairOrderedPair(Params.ArchMinRadius, Params.ArchMaxRadius);
        VF_RepairOrderedPair(Params.ColumnMinRadius, Params.ColumnMaxRadius);
        VF_RepairOrderedPair(Params.PitMinRadius, Params.PitMaxRadius);
        VF_RepairOrderedPair(Params.ChimneyMinRadius, Params.ChimneyMaxRadius);
        VF_RepairOrderedPair(Params.DomeMinRadius, Params.DomeMaxRadius);

        VF_ResetNonTunableFields(Params);

        Result.Params = Params;
        Result.Archetype = Group.Archetype;
        Result.bValid = true;
        return Result;
    }
}

const TArray<FVoxelStrateFieldExclusion>& FVoxelStrateCorpus::GetNonTunableFields()
{
    static const TArray<FVoxelStrateFieldExclusion> Exclusions = []
    {
        TArray<FVoxelStrateFieldExclusion> Result;

        const TCHAR* TerrainOpReason = TEXT(
            "Internal terrain-op transport: populated per room by UVoxelTerrainOpDefinition, not an authored GenerationParams tunable.");
        VF_AddExclusion(Result, TEXT("TerraceStepHeight"), TerrainOpReason);
        VF_AddExclusion(Result, TEXT("TerraceHardness"), TerrainOpReason);
        VF_AddExclusion(Result, TEXT("TerraceNoiseDisplacement"), TerrainOpReason);
        VF_AddExclusion(Result, TEXT("LayerLineSpacing"), TerrainOpReason);
        VF_AddExclusion(Result, TEXT("LayerLineDepth"), TerrainOpReason);
        VF_AddExclusion(Result, TEXT("OverhangStrength"), TerrainOpReason);
        VF_AddExclusion(Result, TEXT("OverhangDepth"), TerrainOpReason);
        VF_AddExclusion(Result, TEXT("OverhangFrequency"), TerrainOpReason);
        VF_AddExclusion(Result, TEXT("RibbingSpacing"), TerrainOpReason);
        VF_AddExclusion(Result, TEXT("RibbingDepth"), TerrainOpReason);
        VF_AddExclusion(Result, TEXT("CliffStrength"), TerrainOpReason);
        VF_AddExclusion(Result, TEXT("ScallopStrength"), TerrainOpReason);
        VF_AddExclusion(Result, TEXT("ScallopFrequency"), TerrainOpReason);
        VF_AddExclusion(Result, TEXT("ArchDensity"), TerrainOpReason);
        VF_AddExclusion(Result, TEXT("ArchMinRadius"), TerrainOpReason);
        VF_AddExclusion(Result, TEXT("ArchMaxRadius"), TerrainOpReason);
        VF_AddExclusion(Result, TEXT("ColumnDensity"), TerrainOpReason);
        VF_AddExclusion(Result, TEXT("ColumnMinRadius"), TerrainOpReason);
        VF_AddExclusion(Result, TEXT("ColumnMaxRadius"), TerrainOpReason);
        VF_AddExclusion(Result, TEXT("PitDensity"), TerrainOpReason);
        VF_AddExclusion(Result, TEXT("PitMinRadius"), TerrainOpReason);
        VF_AddExclusion(Result, TEXT("PitMaxRadius"), TerrainOpReason);
        VF_AddExclusion(Result, TEXT("PitDepth"), TerrainOpReason);
        VF_AddExclusion(Result, TEXT("ChimneyDensity"), TerrainOpReason);
        VF_AddExclusion(Result, TEXT("ChimneyMinRadius"), TerrainOpReason);
        VF_AddExclusion(Result, TEXT("ChimneyMaxRadius"), TerrainOpReason);
        VF_AddExclusion(Result, TEXT("ChimneyHeight"), TerrainOpReason);
        VF_AddExclusion(Result, TEXT("DomeDensity"), TerrainOpReason);
        VF_AddExclusion(Result, TEXT("DomeMinRadius"), TerrainOpReason);
        VF_AddExclusion(Result, TEXT("DomeMaxRadius"), TerrainOpReason);
        VF_AddExclusion(Result, TEXT("DomeHeightRatio"), TerrainOpReason);
        VF_AddExclusion(Result, TEXT("PinchDensity"), TerrainOpReason);
        VF_AddExclusion(Result, TEXT("PinchStrength"), TerrainOpReason);
        VF_AddExclusion(Result, TEXT("PinchLength"), TerrainOpReason);

        VF_AddExclusion(Result, TEXT("StrateTopWorldZ"), TEXT(
            "Runtime Z bound supplied by UVoxelStrateManager for the active layout slot; not a tunable."));
        VF_AddExclusion(Result, TEXT("StrateBottomWorldZ"), TEXT(
            "Runtime Z bound supplied by UVoxelStrateManager for the active layout slot; not a tunable."));
        return Result;
    }();
    return Exclusions;
}

void FVoxelStrateCorpus::Reset()
{
    Entries.Reset();
    FieldSpreads.Reset();
    SkippedDefinitions.Reset();
    SchemaError.Reset();
    bSchemaValid = true;
    bClampMetadataAvailableAtBuild = false;
}

void FVoxelStrateCorpus::RebuildSpreads()
{
    FieldSpreads.Reset();
    SchemaError.Reset();
    bSchemaValid = true;

    TMap<FString, FProperty*> Properties;
    VF_CollectGenerationProperties(Properties);

    TArray<FString> ExpectedNames;
    TSet<FString> ExpectedNameSet;
    TMap<FString, int32> Indices;

#define VF_COMPOSER_ADD_LERPF(Name) \
    ExpectedNames.Add(TEXT(#Name)); \
    ExpectedNameSet.Add(TEXT(#Name)); \
    VF_AddSpreadDescriptor(FieldSpreads, Indices, Properties, TEXT(#Name), EVoxelStrateFieldKind::Continuous);
#define VF_COMPOSER_ADD_SNAP(Name) \
    ExpectedNames.Add(TEXT(#Name)); \
    ExpectedNameSet.Add(TEXT(#Name)); \
    VF_AddSpreadDescriptor(FieldSpreads, Indices, Properties, TEXT(#Name), \
        VF_FieldKindFor<decltype(std::declval<FStrateGenerationParams>().Name)>());
    VF_STRATE_PARAM_FIELDS(VF_COMPOSER_ADD_LERPF, VF_COMPOSER_ADD_SNAP)
#undef VF_COMPOSER_ADD_LERPF
#undef VF_COMPOSER_ADD_SNAP

    for (const TPair<FString, FProperty*>& Pair : Properties)
    {
        if (!ExpectedNameSet.Contains(Pair.Key))
        {
            bSchemaValid = false;
            SchemaError += FString::Printf(
                TEXT("Reflected FStrateGenerationParams field '%s' is absent from VF_STRATE_PARAM_FIELDS. "),
                *Pair.Key);
        }
    }
    for (const FVoxelStrateFieldExclusion& Exclusion : GetNonTunableFields())
    {
        if (!ExpectedNameSet.Contains(Exclusion.FieldName))
        {
            bSchemaValid = false;
            SchemaError += FString::Printf(
                TEXT("Explicit exclusion '%s' is absent from VF_STRATE_PARAM_FIELDS. "),
                *Exclusion.FieldName);
        }
    }
    for (const FString& ExpectedName : ExpectedNames)
    {
        const FVoxelStrateFieldSpread* Spread = FindSpread(ExpectedName);
        if (Spread != nullptr && !Spread->bReflected && !Spread->bExcluded)
        {
            bSchemaValid = false;
            SchemaError += FString::Printf(
                TEXT("Field '%s' is neither reflected nor explicitly excluded. "), *ExpectedName);
        }
    }

    TArray<TArray<double>> Values;
    Values.SetNum(FieldSpreads.Num());
    for (const FVoxelStrateCorpusEntry& Entry : Entries)
    {
#define VF_COMPOSER_ACCUM_LERPF(Name) \
        VF_AccumulateValue(TEXT(#Name), static_cast<double>(Entry.Params.Name), Indices, Values);
#define VF_COMPOSER_ACCUM_SNAP(Name) \
        VF_AccumulateValue(TEXT(#Name), static_cast<double>(Entry.Params.Name), Indices, Values);
        VF_STRATE_PARAM_FIELDS(VF_COMPOSER_ACCUM_LERPF, VF_COMPOSER_ACCUM_SNAP)
#undef VF_COMPOSER_ACCUM_LERPF
#undef VF_COMPOSER_ACCUM_SNAP
    }

    for (int32 FieldIndex = 0; FieldIndex < FieldSpreads.Num(); ++FieldIndex)
    {
        FVoxelStrateFieldSpread& Spread = FieldSpreads[FieldIndex];
        const TArray<double>& Samples = Values[FieldIndex];
        Spread.SampleCount = Samples.Num();
        if (Samples.Num() == 0)
        {
            continue;
        }

        Spread.Min = Samples[0];
        Spread.Max = Samples[0];
        double Sum = 0.0;
        for (const double Sample : Samples)
        {
            Spread.Min = FMath::Min(Spread.Min, Sample);
            Spread.Max = FMath::Max(Spread.Max, Sample);
            Sum += Sample;
        }
        Spread.Mean = Sum / (double)Samples.Num();

        double Variance = 0.0;
        for (const double Sample : Samples)
        {
            const double Delta = Sample - Spread.Mean;
            Variance += Delta * Delta;
        }
        Spread.StdDev = FMath::Sqrt(FMath::Max(0.0, Variance / (double)Samples.Num()));
    }

#if WITH_EDITOR
    bClampMetadataAvailableAtBuild = true;
#else
    bClampMetadataAvailableAtBuild = false;
#endif
}

bool FVoxelStrateCorpus::AddEntry(const FString& SourcePath, const FString& SourceName,
                                  ECaveGeneratorType Archetype,
                                  const FStrateGenerationParams& Params, float Weight)
{
    if (Archetype != ECaveGeneratorType::TunnelNetwork
        && Archetype != ECaveGeneratorType::Underwater)
    {
        SkippedDefinitions.Add(FString::Printf(
            TEXT("%s skipped: archetype %s does not author FStrateGenerationParams."),
            *SourcePath, VF_GetStrateArchetypeName(Archetype)));
        return false;
    }
    if (!FMath::IsFinite(Weight) || Weight <= 0.0f)
    {
        SkippedDefinitions.Add(FString::Printf(
            TEXT("%s skipped: corpus weight %.9g is not positive and finite."),
            *SourcePath, Weight));
        return false;
    }

    FVoxelStrateCorpusEntry& Entry = Entries.AddDefaulted_GetRef();
    Entry.SourcePath = SourcePath;
    Entry.SourceName = SourceName.IsEmpty() ? SourcePath : SourceName;
    Entry.Archetype = Archetype;
    Entry.Params = Params;
    Entry.Weight = Weight;

    Entries.Sort([](const FVoxelStrateCorpusEntry& A, const FVoxelStrateCorpusEntry& B)
    {
        if (A.SourcePath != B.SourcePath) { return A.SourcePath < B.SourcePath; }
        if ((uint8)A.Archetype != (uint8)B.Archetype)
        {
            return (uint8)A.Archetype < (uint8)B.Archetype;
        }
        return A.SourceName < B.SourceName;
    });
    RebuildSpreads();
    return true;
}

bool FVoxelStrateCorpus::LoadFromDefinitions(
    const TArray<UVoxelStrateDefinition*>& Definitions, FString& OutReport)
{
    Reset();

    TArray<UVoxelStrateDefinition*> SortedDefinitions;
    for (UVoxelStrateDefinition* Definition : Definitions)
    {
        if (Definition != nullptr)
        {
            SortedDefinitions.Add(Definition);
        }
    }
    SortedDefinitions.Sort([](const UVoxelStrateDefinition& A, const UVoxelStrateDefinition& B)
    {
        return A.GetPathName() < B.GetPathName();
    });

    TSet<FString> SeenPaths;
    for (UVoxelStrateDefinition* Definition : SortedDefinitions)
    {
        const FString SourcePath = Definition->GetPathName();
        if (SeenPaths.Contains(SourcePath))
        {
            continue;
        }
        SeenPaths.Add(SourcePath);

        const FString SourceName = Definition->StrateName.ToString().IsEmpty()
            ? Definition->GetName()
            : Definition->StrateName.ToString();

        if (Definition->GeneratorType != ECaveGeneratorType::TunnelNetwork
            && Definition->GeneratorType != ECaveGeneratorType::Underwater)
        {
            SkippedDefinitions.Add(FString::Printf(
                TEXT("%s (%s) skipped: %s uses a different authored parameter struct; its ")
                TEXT("GenerationParams default is not corpus data."),
                *SourcePath, *SourceName,
                VF_GetStrateArchetypeName(Definition->GeneratorType)));
            continue;
        }

        FVoxelStrateCorpusEntry& Entry = Entries.AddDefaulted_GetRef();
        Entry.SourcePath = SourcePath;
        Entry.SourceName = SourceName;
        Entry.Archetype = Definition->GeneratorType;
        Entry.Params = Definition->GenerationParams;
        Entry.Weight = 1.0f; // There is no authoring weight on UVoxelStrateDefinition.
    }

    Entries.Sort([](const FVoxelStrateCorpusEntry& A, const FVoxelStrateCorpusEntry& B)
    {
        if (A.SourcePath != B.SourcePath) { return A.SourcePath < B.SourcePath; }
        return (uint8)A.Archetype < (uint8)B.Archetype;
    });
    RebuildSpreads();

    OutReport = FString::Printf(
        TEXT("Corpus definitions: %d resolved usable entries, %d skipped, %d fields measured. "),
        Entries.Num(), SkippedDefinitions.Num(), FieldSpreads.Num());
    for (const FString& Skipped : SkippedDefinitions)
    {
        OutReport += Skipped;
        OutReport += TEXT(" ");
    }
    if (!SchemaError.IsEmpty())
    {
        OutReport += TEXT("Schema error: ");
        OutReport += SchemaError;
    }
    return IsValid();
}

bool FVoxelStrateCorpus::LoadFromSettings(const UVoxelSettings* Settings, FString& OutReport)
{
    Reset();
    if (Settings == nullptr)
    {
        OutReport = TEXT("Cannot load strate corpus: settings is null.");
        return false;
    }

    TArray<TSoftObjectPtr<UVoxelStrateDefinition>> References;
    for (const TPair<int32, TSoftObjectPtr<UVoxelStrateDefinition>>& Pair : Settings->FixedStrates)
    {
        References.Add(Pair.Value);
    }
    References.Append(Settings->StratePool);
    References.Sort([](const TSoftObjectPtr<UVoxelStrateDefinition>& A,
                       const TSoftObjectPtr<UVoxelStrateDefinition>& B)
    {
        return A.ToSoftObjectPath().ToString() < B.ToSoftObjectPath().ToString();
    });

    TArray<UVoxelStrateDefinition*> Definitions;
    TSet<FString> SeenPaths;
    FString ResolutionNotes;
    for (const TSoftObjectPtr<UVoxelStrateDefinition>& Reference : References)
    {
        const FString Path = Reference.ToSoftObjectPath().ToString();
        if (Path.IsEmpty() || SeenPaths.Contains(Path))
        {
            continue;
        }
        SeenPaths.Add(Path);

        TSoftObjectPtr<UVoxelStrateDefinition> LoadReference = Reference;
        UVoxelStrateDefinition* Definition = LoadReference.LoadSynchronous();
        if (Definition == nullptr)
        {
            ResolutionNotes += FString::Printf(TEXT("%s could not be resolved. "), *Path);
            continue;
        }
        Definitions.Add(Definition);
    }

    FString DefinitionReport;
    const bool bLoaded = LoadFromDefinitions(Definitions, DefinitionReport);
    OutReport = FString::Printf(
        TEXT("Settings references: %d unique soft paths, %d resolved. %s%s"),
        SeenPaths.Num(), Definitions.Num(), *ResolutionNotes, *DefinitionReport);
    return bLoaded;
}

const FVoxelStrateFieldSpread* FVoxelStrateCorpus::FindSpread(const FString& FieldName) const
{
    for (const FVoxelStrateFieldSpread& Spread : FieldSpreads)
    {
        if (Spread.FieldName == FieldName)
        {
            return &Spread;
        }
    }
    return nullptr;
}

uint32 FVoxelStrateCorpus::GetContentsHash() const
{
    uint32 Hash = 2166136261U;

    auto HashBytes = [&Hash](const void* Data, SIZE_T Size)
    {
        const uint8* Bytes = static_cast<const uint8*>(Data);
        for (SIZE_T ByteIndex = 0; ByteIndex < Size; ++ByteIndex)
        {
            Hash ^= (uint32)Bytes[ByteIndex];
            Hash *= 16777619U;
        }
    };
    auto HashString = [&HashBytes](const FString& String)
    {
        for (int32 CharIndex = 0; CharIndex < String.Len(); ++CharIndex)
        {
            const uint32 Code = (uint32)String[CharIndex];
            HashBytes(&Code, sizeof(Code));
        }
        const uint32 Terminator = 0U;
        HashBytes(&Terminator, sizeof(Terminator));
    };

    for (const FVoxelStrateCorpusEntry& Entry : Entries)
    {
        HashString(Entry.SourcePath);
        HashString(Entry.SourceName);
        const uint8 Archetype = (uint8)Entry.Archetype;
        HashBytes(&Archetype, sizeof(Archetype));
        HashBytes(&Entry.Weight, sizeof(Entry.Weight));

#define VF_COMPOSER_HASH_LERPF(Name) \
        HashBytes(&Entry.Params.Name, sizeof(Entry.Params.Name));
#define VF_COMPOSER_HASH_SNAP(Name) \
        HashBytes(&Entry.Params.Name, sizeof(Entry.Params.Name));
        VF_STRATE_PARAM_FIELDS(VF_COMPOSER_HASH_LERPF, VF_COMPOSER_HASH_SNAP)
#undef VF_COMPOSER_HASH_LERPF
#undef VF_COMPOSER_HASH_SNAP
    }
    return Hash;
}

const TCHAR* VF_GetStrateArchetypeName(ECaveGeneratorType Archetype)
{
    switch (Archetype)
    {
    case ECaveGeneratorType::TunnelNetwork:  return TEXT("TunnelNetwork");
    case ECaveGeneratorType::FlatPlain:      return TEXT("FlatPlain");
    case ECaveGeneratorType::CrystalChamber: return TEXT("CrystalChamber");
    case ECaveGeneratorType::Maze:            return TEXT("Maze");
    case ECaveGeneratorType::SurfaceWorld:    return TEXT("SurfaceWorld");
    case ECaveGeneratorType::VerticalShafts:  return TEXT("VerticalShafts");
    case ECaveGeneratorType::FloatingIslands: return TEXT("FloatingIslands");
    case ECaveGeneratorType::Underwater:      return TEXT("Underwater");
    }
    return TEXT("Unknown");
}

bool VF_AreStrateParamsBitIdentical(const FStrateGenerationParams& A,
                                    const FStrateGenerationParams& B)
{
#define VF_COMPOSER_COMPARE_LERPF(Name) \
    if (FMemory::Memcmp(&A.Name, &B.Name, sizeof(A.Name)) != 0) { return false; }
#define VF_COMPOSER_COMPARE_SNAP(Name) \
    if (A.Name != B.Name) { return false; }
    VF_STRATE_PARAM_FIELDS(VF_COMPOSER_COMPARE_LERPF, VF_COMPOSER_COMPARE_SNAP)
#undef VF_COMPOSER_COMPARE_LERPF
#undef VF_COMPOSER_COMPARE_SNAP
    return true;
}

FVoxelStrateRollInfo VF_RollStrateParamsDetailed(const FVoxelStrateCorpus& Corpus,
                                                 int32 Seed, int32 Index)
{
    const FVoxelStrateRollInfo Result = VF_RollInternal(Corpus, Seed, Index);

#if DO_CHECK
    if (Result.bValid)
    {
        const FVoxelStrateRollInfo Repeat = VF_RollInternal(Corpus, Seed, Index);
        checkf(Repeat.bValid && Repeat.Archetype == Result.Archetype
                   && VF_AreStrateParamsBitIdentical(Repeat.Params, Result.Params)
                   && Repeat.ParentEntryIndices == Result.ParentEntryIndices
                   && Repeat.ParentWeights == Result.ParentWeights,
               TEXT("VF_RollStrateParams lost determinism for the same corpus/seed/index."));
    }
#endif
    return Result;
}

FStrateGenerationParams VF_RollStrateParams(const FVoxelStrateCorpus& Corpus,
                                             int32 Seed, int32 Index)
{
    return VF_RollStrateParamsDetailed(Corpus, Seed, Index).Params;
}
