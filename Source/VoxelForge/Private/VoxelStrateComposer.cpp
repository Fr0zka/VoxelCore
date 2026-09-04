// Offline strate corpus and parameter roll.

#include "VoxelStrateComposer.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Math/RandomStream.h"
#include "Modules/ModuleManager.h"
#include "UObject/EnumProperty.h"
#include "UObject/FieldIterator.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectGlobals.h"

#include "VoxelSettings.h"
#include "VoxelStrateDefinition.h"

#include <type_traits>
#include <utility>

namespace
{
    constexpr float GJitterFraction = 0.15f;

    static const ECaveGeneratorType GAllArchetypes[] =
    {
        ECaveGeneratorType::TunnelNetwork,
        ECaveGeneratorType::FlatPlain,
        ECaveGeneratorType::CrystalChamber,
        ECaveGeneratorType::Maze,
        ECaveGeneratorType::SurfaceWorld,
        ECaveGeneratorType::VerticalShafts,
        ECaveGeneratorType::FloatingIslands,
        ECaveGeneratorType::Underwater,
    };

    bool VF_IsTunnelArchetype(ECaveGeneratorType Archetype)
    {
        return Archetype == ECaveGeneratorType::TunnelNetwork
            || Archetype == ECaveGeneratorType::Underwater;
    }

    bool VF_IsSupportedArchetype(ECaveGeneratorType Archetype)
    {
        switch (Archetype)
        {
        case ECaveGeneratorType::TunnelNetwork:
        case ECaveGeneratorType::FlatPlain:
        case ECaveGeneratorType::CrystalChamber:
        case ECaveGeneratorType::Maze:
        case ECaveGeneratorType::SurfaceWorld:
        case ECaveGeneratorType::VerticalShafts:
        case ECaveGeneratorType::FloatingIslands:
        case ECaveGeneratorType::Underwater:
            return true;
        default:
            return false;
        }
    }

    UStruct* VF_GetParamStruct(ECaveGeneratorType Archetype)
    {
        switch (Archetype)
        {
        case ECaveGeneratorType::TunnelNetwork:
        case ECaveGeneratorType::Underwater:
            return FStrateGenerationParams::StaticStruct();
        case ECaveGeneratorType::FlatPlain:
        case ECaveGeneratorType::CrystalChamber:
            return FSlabGenerationParams::StaticStruct();
        case ECaveGeneratorType::Maze:
            return FMazeGenerationParams::StaticStruct();
        case ECaveGeneratorType::SurfaceWorld:
            return FSurfaceGenerationParams::StaticStruct();
        case ECaveGeneratorType::VerticalShafts:
            return FVerticalShaftParams::StaticStruct();
        case ECaveGeneratorType::FloatingIslands:
            return FFloatingIslandParams::StaticStruct();
        default:
            return nullptr;
        }
    }

    void* VF_GetParamMemory(FVoxelStrateArchetypeParams& Params,
                            ECaveGeneratorType Archetype)
    {
        switch (Archetype)
        {
        case ECaveGeneratorType::TunnelNetwork:
        case ECaveGeneratorType::Underwater:
            return &Params.TunnelNetworkParams;
        case ECaveGeneratorType::FlatPlain:
        case ECaveGeneratorType::CrystalChamber:
            return &Params.SlabParams;
        case ECaveGeneratorType::Maze:
            return &Params.MazeParams;
        case ECaveGeneratorType::SurfaceWorld:
            return &Params.SurfaceParams;
        case ECaveGeneratorType::VerticalShafts:
            return &Params.VerticalShaftParams;
        case ECaveGeneratorType::FloatingIslands:
            return &Params.FloatingIslandParams;
        default:
            return nullptr;
        }
    }

    const void* VF_GetParamMemory(const FVoxelStrateArchetypeParams& Params,
                                  ECaveGeneratorType Archetype)
    {
        switch (Archetype)
        {
        case ECaveGeneratorType::TunnelNetwork:
        case ECaveGeneratorType::Underwater:
            return &Params.TunnelNetworkParams;
        case ECaveGeneratorType::FlatPlain:
        case ECaveGeneratorType::CrystalChamber:
            return &Params.SlabParams;
        case ECaveGeneratorType::Maze:
            return &Params.MazeParams;
        case ECaveGeneratorType::SurfaceWorld:
            return &Params.SurfaceParams;
        case ECaveGeneratorType::VerticalShafts:
            return &Params.VerticalShaftParams;
        case ECaveGeneratorType::FloatingIslands:
            return &Params.FloatingIslandParams;
        default:
            return nullptr;
        }
    }

    FString VF_SpreadKey(ECaveGeneratorType Archetype, const FString& FieldName)
    {
        return FString::Printf(TEXT("%d:%s"), static_cast<int32>(static_cast<uint8>(Archetype)),
                               *FieldName);
    }

    FString VF_SpreadKey(ECaveGeneratorType Archetype, const TCHAR* FieldName)
    {
        return VF_SpreadKey(Archetype, FString(FieldName));
    }

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

    bool VF_IsRuntimeField(const FString& FieldName)
    {
        return FieldName == TEXT("StrateTopWorldZ")
            || FieldName == TEXT("StrateBottomWorldZ");
    }

    bool VF_IsExcludedForArchetype(ECaveGeneratorType Archetype, const FString& FieldName)
    {
        // These names are transport slots only for FStrateGenerationParams. Sibling families
        // have real authored fields with some of the same names (e.g. Surface::TerraceHardness).
        if (VF_IsTunnelArchetype(Archetype))
        {
            return VF_FindExclusion(FieldName) != nullptr;
        }
        return VF_IsRuntimeField(FieldName);
    }

    void VF_AddExclusion(TArray<FVoxelStrateFieldExclusion>& Out,
                         const TCHAR* FieldName, const TCHAR* Reason)
    {
        FVoxelStrateFieldExclusion& Entry = Out.AddDefaulted_GetRef();
        Entry.FieldName = FieldName;
        Entry.Reason = Reason;
    }

    void VF_CollectProperties(UStruct* Struct, TMap<FString, FProperty*>& OutProperties)
    {
        OutProperties.Reset();
        if (Struct == nullptr)
        {
            return;
        }

        for (TFieldIterator<FProperty> It(Struct, EFieldIteratorFlags::IncludeSuper); It; ++It)
        {
            FProperty* Property = *It;
            if (Property != nullptr)
            {
                OutProperties.Add(Property->GetName(), Property);
            }
        }
    }

    EVoxelStrateFieldKind VF_PropertyKind(const FProperty* Property)
    {
        if (Property == nullptr)
        {
            return EVoxelStrateFieldKind::Continuous;
        }
        if (CastField<FBoolProperty>(Property) != nullptr)
        {
            return EVoxelStrateFieldKind::Boolean;
        }
        if (CastField<FEnumProperty>(Property) != nullptr)
        {
            return EVoxelStrateFieldKind::Enum;
        }
        if (const FNumericProperty* NumericProperty = CastField<FNumericProperty>(Property))
        {
            if (NumericProperty->IsEnum())
            {
                return EVoxelStrateFieldKind::Enum;
            }
            if (NumericProperty->IsInteger())
            {
                return EVoxelStrateFieldKind::Integer;
            }
        }
        return EVoxelStrateFieldKind::Continuous;
    }

    bool VF_IsScalarProperty(const FProperty* Property)
    {
        return CastField<FBoolProperty>(Property) != nullptr
            || CastField<FNumericProperty>(Property) != nullptr
            || CastField<FEnumProperty>(Property) != nullptr;
    }

    bool VF_ReadPropertyValue(const FProperty* Property, const void* Memory, double& OutValue)
    {
        if (Property == nullptr || Memory == nullptr || !VF_IsScalarProperty(Property))
        {
            return false;
        }

        const void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Memory);
        if (const FBoolProperty* BoolProperty = CastField<FBoolProperty>(Property))
        {
            OutValue = BoolProperty->GetPropertyValue(ValuePtr) ? 1.0 : 0.0;
            return true;
        }

        const FNumericProperty* NumericProperty = CastField<FNumericProperty>(Property);
        const FNumericProperty* UnderlyingProperty = NumericProperty;
        if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
        {
            UnderlyingProperty = EnumProperty->GetUnderlyingProperty();
        }
        if (UnderlyingProperty == nullptr)
        {
            return false;
        }

        if (UnderlyingProperty->IsFloatingPoint())
        {
            OutValue = UnderlyingProperty->GetFloatingPointPropertyValue(ValuePtr);
        }
        else
        {
            // Current integer/enum fields are non-negative; the signed accessor gives the
            // expected value for the byte-backed enum properties used by the strate structs.
            OutValue = static_cast<double>(UnderlyingProperty->GetSignedIntPropertyValue(ValuePtr));
        }
        return FMath::IsFinite(OutValue);
    }

    bool VF_WritePropertyValue(const FProperty* Property, void* Memory, double Value)
    {
        if (Property == nullptr || Memory == nullptr || !VF_IsScalarProperty(Property)
            || !FMath::IsFinite(Value))
        {
            return false;
        }

        void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Memory);
        if (const FBoolProperty* BoolProperty = CastField<FBoolProperty>(Property))
        {
            BoolProperty->SetPropertyValue(ValuePtr, Value >= 0.5);
            return true;
        }

        const FNumericProperty* NumericProperty = CastField<FNumericProperty>(Property);
        const FNumericProperty* UnderlyingProperty = NumericProperty;
        if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
        {
            UnderlyingProperty = EnumProperty->GetUnderlyingProperty();
        }
        if (UnderlyingProperty == nullptr)
        {
            return false;
        }

        if (UnderlyingProperty->IsFloatingPoint())
        {
            UnderlyingProperty->SetFloatingPointPropertyValue(ValuePtr, Value);
        }
        else
        {
            UnderlyingProperty->SetIntPropertyValue(ValuePtr, static_cast<int64>(FMath::RoundToInt(Value)));
        }
        return true;
    }

    template <typename T>
    double VF_NativeValueAsDouble(T Value)
    {
        if constexpr (std::is_enum<T>::value)
        {
            using UnderlyingType = typename std::underlying_type<T>::type;
            return static_cast<double>(static_cast<UnderlyingType>(Value));
        }
        else
        {
            return static_cast<double>(Value);
        }
    }

    template <typename T, typename std::enable_if<!std::is_enum<T>::value, int>::type = 0>
    void VF_AssignNativeValue(T& Target, double Value)
    {
        Target = static_cast<T>(Value);
    }

    template <typename T, typename std::enable_if<std::is_enum<T>::value, int>::type = 0>
    void VF_AssignNativeValue(T& Target, double Value)
    {
        using UnderlyingType = typename std::underlying_type<T>::type;
        Target = static_cast<T>(static_cast<UnderlyingType>(FMath::RoundToInt(Value)));
    }

    bool VF_ReadFStrateField(const FStrateGenerationParams& Params,
                             const FString& FieldName, double& OutValue)
    {
#define VF_READ_FSTRATE_FIELD(Name) \
        if (FieldName == TEXT(#Name)) { OutValue = VF_NativeValueAsDouble(Params.Name); return true; }
        VF_STRATE_PARAM_FIELDS(VF_READ_FSTRATE_FIELD, VF_READ_FSTRATE_FIELD)
#undef VF_READ_FSTRATE_FIELD
        return false;
    }

    bool VF_WriteFStrateField(FStrateGenerationParams& Params,
                              const FString& FieldName, double Value)
    {
#define VF_WRITE_FSTRATE_FIELD(Name) \
        if (FieldName == TEXT(#Name)) { VF_AssignNativeValue(Params.Name, Value); return true; }
        VF_STRATE_PARAM_FIELDS(VF_WRITE_FSTRATE_FIELD, VF_WRITE_FSTRATE_FIELD)
#undef VF_WRITE_FSTRATE_FIELD
        return false;
    }

    bool VF_ReadRuntimeField(const void* Memory, ECaveGeneratorType Archetype,
                             const FString& FieldName, double& OutValue)
    {
        if (Memory == nullptr || !VF_IsRuntimeField(FieldName))
        {
            return false;
        }

        if (FieldName == TEXT("StrateTopWorldZ"))
        {
            switch (Archetype)
            {
            case ECaveGeneratorType::TunnelNetwork:
            case ECaveGeneratorType::Underwater:
                OutValue = static_cast<const FStrateGenerationParams*>(Memory)->StrateTopWorldZ;
                return true;
            case ECaveGeneratorType::FlatPlain:
            case ECaveGeneratorType::CrystalChamber:
                OutValue = static_cast<const FSlabGenerationParams*>(Memory)->StrateTopWorldZ;
                return true;
            case ECaveGeneratorType::Maze:
                OutValue = static_cast<const FMazeGenerationParams*>(Memory)->StrateTopWorldZ;
                return true;
            case ECaveGeneratorType::SurfaceWorld:
                OutValue = static_cast<const FSurfaceGenerationParams*>(Memory)->StrateTopWorldZ;
                return true;
            case ECaveGeneratorType::VerticalShafts:
                OutValue = static_cast<const FVerticalShaftParams*>(Memory)->StrateTopWorldZ;
                return true;
            case ECaveGeneratorType::FloatingIslands:
                OutValue = static_cast<const FFloatingIslandParams*>(Memory)->StrateTopWorldZ;
                return true;
            default:
                return false;
            }
        }

        switch (Archetype)
        {
        case ECaveGeneratorType::TunnelNetwork:
        case ECaveGeneratorType::Underwater:
            OutValue = static_cast<const FStrateGenerationParams*>(Memory)->StrateBottomWorldZ;
            return true;
        case ECaveGeneratorType::FlatPlain:
        case ECaveGeneratorType::CrystalChamber:
            OutValue = static_cast<const FSlabGenerationParams*>(Memory)->StrateBottomWorldZ;
            return true;
        case ECaveGeneratorType::Maze:
            OutValue = static_cast<const FMazeGenerationParams*>(Memory)->StrateBottomWorldZ;
            return true;
        case ECaveGeneratorType::SurfaceWorld:
            OutValue = static_cast<const FSurfaceGenerationParams*>(Memory)->StrateBottomWorldZ;
            return true;
        case ECaveGeneratorType::VerticalShafts:
            OutValue = static_cast<const FVerticalShaftParams*>(Memory)->StrateBottomWorldZ;
            return true;
        case ECaveGeneratorType::FloatingIslands:
            OutValue = static_cast<const FFloatingIslandParams*>(Memory)->StrateBottomWorldZ;
            return true;
        default:
            return false;
        }
    }

    bool VF_WriteRuntimeField(void* Memory, ECaveGeneratorType Archetype,
                              const FString& FieldName, double Value)
    {
        if (Memory == nullptr || !VF_IsRuntimeField(FieldName))
        {
            return false;
        }

        if (FieldName == TEXT("StrateTopWorldZ"))
        {
            switch (Archetype)
            {
            case ECaveGeneratorType::TunnelNetwork:
            case ECaveGeneratorType::Underwater:
                static_cast<FStrateGenerationParams*>(Memory)->StrateTopWorldZ = static_cast<float>(Value);
                return true;
            case ECaveGeneratorType::FlatPlain:
            case ECaveGeneratorType::CrystalChamber:
                static_cast<FSlabGenerationParams*>(Memory)->StrateTopWorldZ = static_cast<float>(Value);
                return true;
            case ECaveGeneratorType::Maze:
                static_cast<FMazeGenerationParams*>(Memory)->StrateTopWorldZ = static_cast<float>(Value);
                return true;
            case ECaveGeneratorType::SurfaceWorld:
                static_cast<FSurfaceGenerationParams*>(Memory)->StrateTopWorldZ = static_cast<float>(Value);
                return true;
            case ECaveGeneratorType::VerticalShafts:
                static_cast<FVerticalShaftParams*>(Memory)->StrateTopWorldZ = static_cast<float>(Value);
                return true;
            case ECaveGeneratorType::FloatingIslands:
                static_cast<FFloatingIslandParams*>(Memory)->StrateTopWorldZ = static_cast<float>(Value);
                return true;
            default:
                return false;
            }
        }

        switch (Archetype)
        {
        case ECaveGeneratorType::TunnelNetwork:
        case ECaveGeneratorType::Underwater:
            static_cast<FStrateGenerationParams*>(Memory)->StrateBottomWorldZ = static_cast<float>(Value);
            return true;
        case ECaveGeneratorType::FlatPlain:
        case ECaveGeneratorType::CrystalChamber:
            static_cast<FSlabGenerationParams*>(Memory)->StrateBottomWorldZ = static_cast<float>(Value);
            return true;
        case ECaveGeneratorType::Maze:
            static_cast<FMazeGenerationParams*>(Memory)->StrateBottomWorldZ = static_cast<float>(Value);
            return true;
        case ECaveGeneratorType::SurfaceWorld:
            static_cast<FSurfaceGenerationParams*>(Memory)->StrateBottomWorldZ = static_cast<float>(Value);
            return true;
        case ECaveGeneratorType::VerticalShafts:
            static_cast<FVerticalShaftParams*>(Memory)->StrateBottomWorldZ = static_cast<float>(Value);
            return true;
        case ECaveGeneratorType::FloatingIslands:
            static_cast<FFloatingIslandParams*>(Memory)->StrateBottomWorldZ = static_cast<float>(Value);
            return true;
        default:
            return false;
        }
    }

    bool VF_ReadNamedField(const void* Memory, ECaveGeneratorType Archetype,
                           const FString& FieldName, double& OutValue)
    {
        UStruct* Struct = VF_GetParamStruct(Archetype);
        if (Struct == nullptr || Memory == nullptr)
        {
            return false;
        }

        if (VF_IsTunnelArchetype(Archetype)
            && VF_ReadFStrateField(*static_cast<const FStrateGenerationParams*>(Memory),
                                   FieldName, OutValue))
        {
            return true;
        }

        if (const FProperty* Property = FindFProperty<FProperty>(Struct, FName(*FieldName)))
        {
            return VF_ReadPropertyValue(Property, Memory, OutValue);
        }
        return VF_ReadRuntimeField(Memory, Archetype, FieldName, OutValue);
    }

    bool VF_WriteNamedField(void* Memory, ECaveGeneratorType Archetype,
                            const FString& FieldName, double Value)
    {
        UStruct* Struct = VF_GetParamStruct(Archetype);
        if (Struct == nullptr || Memory == nullptr)
        {
            return false;
        }

        if (VF_IsTunnelArchetype(Archetype)
            && VF_WriteFStrateField(*static_cast<FStrateGenerationParams*>(Memory),
                                    FieldName, Value))
        {
            return true;
        }

        if (FProperty* Property = FindFProperty<FProperty>(Struct, FName(*FieldName)))
        {
            return VF_WritePropertyValue(Property, Memory, Value);
        }
        return VF_WriteRuntimeField(Memory, Archetype, FieldName, Value);
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
                                ECaveGeneratorType Archetype,
                                const FString& ParamStructName,
                                const TMap<FString, FProperty*>& Properties,
                                const FString& Name, EVoxelStrateFieldKind Kind)
    {
        FVoxelStrateFieldSpread& Spread = OutSpreads.AddDefaulted_GetRef();
        Spread.Archetype = Archetype;
        Spread.ParamStructName = ParamStructName;
        Spread.FieldName = Name;
        Spread.Kind = Kind;
        Spread.bExcluded = VF_IsExcludedForArchetype(Archetype, Name);

        if (const FProperty* const* Property = Properties.Find(Name))
        {
            Spread.bReflected = (*Property != nullptr);
            VF_AddPropertyMetadata(Spread, *Property);
        }

        OutIndices.Add(VF_SpreadKey(Archetype, Name), OutSpreads.Num() - 1);
    }

    void VF_ResetNonTunableFields(FStrateGenerationParams& Params)
    {
        // These are terrain-op transport values (written per room by UVoxelTerrainOpDefinition)
        // plus manager-owned runtime Z bounds. They are not rolled in this composer pass.
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

    void VF_ResetRuntimeFields(FVoxelStrateArchetypeParams& Params,
                               ECaveGeneratorType Archetype)
    {
        void* Memory = VF_GetParamMemory(Params, Archetype);
        VF_WriteRuntimeField(Memory, Archetype, TEXT("StrateTopWorldZ"), 0.0);
        VF_WriteRuntimeField(Memory, Archetype, TEXT("StrateBottomWorldZ"), 0.0);
    }

    void VF_ResetExcludedFields(FVoxelStrateArchetypeParams& Params,
                                ECaveGeneratorType Archetype)
    {
        if (VF_IsTunnelArchetype(Archetype))
        {
            VF_ResetNonTunableFields(Params.TunnelNetworkParams);
        }
        else
        {
            VF_ResetRuntimeFields(Params, Archetype);
        }
    }

    void VF_AppendSchemaError(FString& SchemaError, const FString& Message)
    {
        SchemaError += Message;
        SchemaError += TEXT(" ");
    }

    void VF_ValidateFieldDescriptor(const TArray<FVoxelStrateFieldSpread>& Spreads,
                                    ECaveGeneratorType Archetype,
                                    const FString& FieldName,
                                    bool& bSchemaValid, FString& SchemaError)
    {
        const FVoxelStrateFieldSpread* Spread = nullptr;
        for (const FVoxelStrateFieldSpread& Candidate : Spreads)
        {
            if (Candidate.Archetype == Archetype && Candidate.FieldName == FieldName)
            {
                Spread = &Candidate;
                break;
            }
        }
        if (Spread != nullptr && !Spread->bReflected && !Spread->bExcluded)
        {
            bSchemaValid = false;
            VF_AppendSchemaError(SchemaError,
                FString::Printf(TEXT("Field '%s.%s' is neither reflected nor explicitly excluded."),
                                *Spread->ParamStructName, *FieldName));
        }
    }

    void VF_ValidateStructProperties(const TMap<FString, FProperty*>& Properties,
                                     const TSet<FString>& ExpectedNames,
                                     const FString& ParamStructName,
                                     bool& bSchemaValid, FString& SchemaError)
    {
        for (const TPair<FString, FProperty*>& Pair : Properties)
        {
            if (!ExpectedNames.Contains(Pair.Key))
            {
                bSchemaValid = false;
                VF_AppendSchemaError(SchemaError,
                    FString::Printf(TEXT("Reflected %s field '%s' is absent from the composer field list."),
                                    *ParamStructName, *Pair.Key));
            }
            if (!VF_IsScalarProperty(Pair.Value))
            {
                bSchemaValid = false;
                VF_AppendSchemaError(SchemaError,
                    FString::Printf(TEXT("Reflected %s field '%s' is not scalar and cannot be rolled."),
                                    *ParamStructName, *Pair.Key));
            }
        }
    }

    void VF_BlendParamStruct(FVoxelStrateArchetypeParams& Destination,
                             const FVoxelStrateArchetypeParams& Source,
                             ECaveGeneratorType Archetype, float Alpha)
    {
        void* DestinationMemory = VF_GetParamMemory(Destination, Archetype);
        const void* SourceMemory = VF_GetParamMemory(Source, Archetype);
        UStruct* Struct = VF_GetParamStruct(Archetype);
        if (DestinationMemory == nullptr || SourceMemory == nullptr || Struct == nullptr)
        {
            return;
        }

        if (VF_IsTunnelArchetype(Archetype))
        {
            Destination.TunnelNetworkParams = FStrateGenerationParams::Lerp(
                Destination.TunnelNetworkParams, Source.TunnelNetworkParams, Alpha);
            return;
        }

        TMap<FString, FProperty*> Properties;
        VF_CollectProperties(Struct, Properties);
        for (const TPair<FString, FProperty*>& Pair : Properties)
        {
            if (!VF_IsScalarProperty(Pair.Value))
            {
                continue;
            }

            double A = 0.0;
            double B = 0.0;
            if (!VF_ReadPropertyValue(Pair.Value, DestinationMemory, A)
                || !VF_ReadPropertyValue(Pair.Value, SourceMemory, B))
            {
                continue;
            }

            const EVoxelStrateFieldKind Kind = VF_PropertyKind(Pair.Value);
            const double Value = Kind == EVoxelStrateFieldKind::Continuous
                ? static_cast<double>(FMath::Lerp(static_cast<float>(A), static_cast<float>(B), Alpha))
                : (Alpha < 0.5f ? A : B);
            VF_WritePropertyValue(Pair.Value, DestinationMemory, Value);
        }
    }

    void VF_CopyDominantBooleans(FVoxelStrateArchetypeParams& Destination,
                                 const FVoxelStrateArchetypeParams& Dominant,
                                 ECaveGeneratorType Archetype)
    {
        void* DestinationMemory = VF_GetParamMemory(Destination, Archetype);
        const void* DominantMemory = VF_GetParamMemory(Dominant, Archetype);
        UStruct* Struct = VF_GetParamStruct(Archetype);
        if (DestinationMemory == nullptr || DominantMemory == nullptr || Struct == nullptr)
        {
            return;
        }

        TMap<FString, FProperty*> Properties;
        VF_CollectProperties(Struct, Properties);
        for (const TPair<FString, FProperty*>& Pair : Properties)
        {
            if (VF_PropertyKind(Pair.Value) != EVoxelStrateFieldKind::Boolean)
            {
                continue;
            }
            double Value = 0.0;
            if (VF_ReadPropertyValue(Pair.Value, DominantMemory, Value))
            {
                VF_WritePropertyValue(Pair.Value, DestinationMemory, Value);
            }
        }
    }

    void VF_JitterNativeParams(FVoxelStrateArchetypeParams& Params,
                               ECaveGeneratorType Archetype,
                               const FVoxelStrateCorpus& Corpus,
                               FRandomStream& Rng)
    {
        void* Memory = VF_GetParamMemory(Params, Archetype);
        UStruct* Struct = VF_GetParamStruct(Archetype);
        if (Memory == nullptr || Struct == nullptr)
        {
            return;
        }

        TMap<FString, FProperty*> Properties;
        VF_CollectProperties(Struct, Properties);
        for (const TPair<FString, FProperty*>& Pair : Properties)
        {
            if (VF_PropertyKind(Pair.Value) != EVoxelStrateFieldKind::Continuous)
            {
                continue;
            }

            const FVoxelStrateFieldSpread* Spread = Corpus.FindSpread(Archetype, Pair.Key);
            if (Spread == nullptr || Spread->bExcluded)
            {
                continue;
            }

            const double CorpusRange = FMath::Max(0.0, Spread->Max - Spread->Min);
            if (!(CorpusRange > 0.0))
            {
                continue;
            }

            double Value = 0.0;
            if (VF_ReadPropertyValue(Pair.Value, Memory, Value))
            {
                Value += static_cast<double>((Rng.FRand() * 2.0f - 1.0f) * GJitterFraction)
                    * CorpusRange;
                VF_WritePropertyValue(Pair.Value, Memory, Value);
            }
        }
    }

    void VF_ClampNativeParams(FVoxelStrateArchetypeParams& Params,
                              ECaveGeneratorType Archetype,
                              const FVoxelStrateCorpus& Corpus)
    {
        void* Memory = VF_GetParamMemory(Params, Archetype);
        UStruct* Struct = VF_GetParamStruct(Archetype);
        if (Memory == nullptr || Struct == nullptr)
        {
            return;
        }

        TMap<FString, FProperty*> Properties;
        VF_CollectProperties(Struct, Properties);
        for (const TPair<FString, FProperty*>& Pair : Properties)
        {
            const EVoxelStrateFieldKind Kind = VF_PropertyKind(Pair.Value);
            if (Kind == EVoxelStrateFieldKind::Boolean)
            {
                continue;
            }

            const FVoxelStrateFieldSpread* Spread = Corpus.FindSpread(Archetype, Pair.Key);
            if (Spread == nullptr || Spread->bExcluded)
            {
                continue;
            }

            double Value = 0.0;
            if (!VF_ReadPropertyValue(Pair.Value, Memory, Value))
            {
                continue;
            }
            if (Spread->bHasClampMin)
            {
                Value = FMath::Max(Value, Spread->ClampMin);
            }
            if (Spread->bHasClampMax)
            {
                Value = FMath::Min(Value, Spread->ClampMax);
            }
            VF_WritePropertyValue(Pair.Value, Memory, Value);
        }
    }

    void VF_RepairOrderedPair(FVoxelStrateArchetypeParams& Params,
                              ECaveGeneratorType Archetype,
                              const TCHAR* MinField, const TCHAR* MaxField)
    {
        void* Memory = VF_GetParamMemory(Params, Archetype);
        double MinValue = 0.0;
        double MaxValue = 0.0;
        if (VF_ReadNamedField(Memory, Archetype, MinField, MinValue)
            && VF_ReadNamedField(Memory, Archetype, MaxField, MaxValue)
            && MinValue > MaxValue)
        {
            VF_WriteNamedField(Memory, Archetype, MinField, MaxValue);
            VF_WriteNamedField(Memory, Archetype, MaxField, MinValue);
        }
    }

    void VF_RepairNativeOrderedPairs(FVoxelStrateArchetypeParams& Params,
                                     ECaveGeneratorType Archetype)
    {
        switch (Archetype)
        {
        case ECaveGeneratorType::TunnelNetwork:
        case ECaveGeneratorType::Underwater:
            VF_RepairOrderedPair(Params, Archetype, TEXT("MinRoomRadius"), TEXT("MaxRoomRadius"));
            VF_RepairOrderedPair(Params, Archetype, TEXT("RoomFloorCutMin"), TEXT("RoomFloorCutMax"));
            VF_RepairOrderedPair(Params, Archetype, TEXT("TunnelMinRadius"), TEXT("TunnelMaxRadius"));
            break;
        case ECaveGeneratorType::FlatPlain:
        case ECaveGeneratorType::CrystalChamber:
            VF_RepairOrderedPair(Params, Archetype, TEXT("FloorRelativeHeight"), TEXT("CeilingRelativeHeight"));
            VF_RepairOrderedPair(Params, Archetype, TEXT("ColumnMinRadius"), TEXT("ColumnMaxRadius"));
            break;
        case ECaveGeneratorType::SurfaceWorld:
            VF_RepairOrderedPair(Params, Archetype, TEXT("BaseGroundRelative"), TEXT("CeilingRelative"));
            break;
        case ECaveGeneratorType::VerticalShafts:
            VF_RepairOrderedPair(Params, Archetype, TEXT("ShaftMinRadius"), TEXT("ShaftMaxRadius"));
            break;
        case ECaveGeneratorType::FloatingIslands:
            VF_RepairOrderedPair(Params, Archetype, TEXT("IslandMinRadius"), TEXT("IslandMaxRadius"));
            break;
        case ECaveGeneratorType::Maze:
        default:
            break;
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
        Value = VF_Avalanche(Value ^ static_cast<uint32>(Seed));
        Value = VF_Avalanche(Value ^ (static_cast<uint32>(Index) + 0x85ebca6bU));
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

        FRandomStream Rng(static_cast<int32>(VF_RollSeed(Corpus.GetContentsHash(), Seed, Index)));

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
            // 2–3-parent algorithm defined for a one-entry group and exposes that limitation in
            // provenance instead of silently inventing a second vector.
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

        Result.Archetype = Group.Archetype;
        Result.ArchetypeParams = Entries[Result.ParentEntryIndices[0]].ArchetypeParams;
        float AccumulatedWeight = Result.ParentWeights[0];
        for (int32 ParentSlot = 1; ParentSlot < Result.ParentEntryIndices.Num(); ++ParentSlot)
        {
            const float ThisWeight = Result.ParentWeights[ParentSlot];
            const float Alpha = ThisWeight / (AccumulatedWeight + ThisWeight);
            VF_BlendParamStruct(Result.ArchetypeParams,
                                Entries[Result.ParentEntryIndices[ParentSlot]].ArchetypeParams,
                                Result.Archetype, Alpha);
            AccumulatedWeight += ThisWeight;
        }

        const int32 DominantParent = Result.ParentEntryIndices[Result.DominantParentPosition];
        VF_CopyDominantBooleans(Result.ArchetypeParams,
                                Entries[DominantParent].ArchetypeParams,
                                Result.Archetype);
        VF_JitterNativeParams(Result.ArchetypeParams, Result.Archetype, Corpus, Rng);
        VF_ClampNativeParams(Result.ArchetypeParams, Result.Archetype, Corpus);
        VF_RepairNativeOrderedPairs(Result.ArchetypeParams, Result.Archetype);
        VF_ResetExcludedFields(Result.ArchetypeParams, Result.Archetype);

        // Preserve the original compact API's compatibility view. Detailed callers use the
        // correctly typed active family in ArchetypeParams.
        Result.Params = Result.ArchetypeParams.TunnelNetworkParams;
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
            "Transport slot only: generation overlays it per room from a UVoxelTerrainOpDefinition "
            "selected through the active strate's TerrainOperations pool; it is not the authored "
            "strate-generation source.");
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

    TMap<FString, int32> Indices;
    for (const ECaveGeneratorType Archetype : GAllArchetypes)
    {
        UStruct* Struct = VF_GetParamStruct(Archetype);
        if (Struct == nullptr)
        {
            bSchemaValid = false;
            VF_AppendSchemaError(SchemaError,
                FString::Printf(TEXT("No parameter struct is registered for archetype %s."),
                                VF_GetStrateArchetypeName(Archetype)));
            continue;
        }

        const FString ParamStructName = Struct->GetName();
        TMap<FString, FProperty*> Properties;
        VF_CollectProperties(Struct, Properties);
        TSet<FString> ExpectedNames;

        if (VF_IsTunnelArchetype(Archetype))
        {
#define VF_COMPOSER_ADD_LERPF(Name) \
            ExpectedNames.Add(TEXT(#Name)); \
            VF_AddSpreadDescriptor(FieldSpreads, Indices, Archetype, ParamStructName, Properties, \
                                   TEXT(#Name), EVoxelStrateFieldKind::Continuous);
#define VF_COMPOSER_ADD_SNAP(Name) \
            ExpectedNames.Add(TEXT(#Name)); \
            VF_AddSpreadDescriptor(FieldSpreads, Indices, Archetype, ParamStructName, Properties, \
                                   TEXT(#Name), VF_FieldKindFor<decltype(std::declval<FStrateGenerationParams>().Name)>());
            VF_STRATE_PARAM_FIELDS(VF_COMPOSER_ADD_LERPF, VF_COMPOSER_ADD_SNAP)
#undef VF_COMPOSER_ADD_LERPF
#undef VF_COMPOSER_ADD_SNAP
        }
        else
        {
            for (const TPair<FString, FProperty*>& Pair : Properties)
            {
                if (VF_IsScalarProperty(Pair.Value))
                {
                    ExpectedNames.Add(Pair.Key);
                    VF_AddSpreadDescriptor(FieldSpreads, Indices, Archetype, ParamStructName,
                                           Properties, Pair.Key, VF_PropertyKind(Pair.Value));
                }
            }
        }

        // Sibling structs keep runtime bounds as ordinary C++ fields, just as
        // FStrateGenerationParams does. Include them in the report and mark them excluded even
        // though reflection cannot see them.
        if (!VF_IsTunnelArchetype(Archetype))
        {
            ExpectedNames.Add(TEXT("StrateTopWorldZ"));
            ExpectedNames.Add(TEXT("StrateBottomWorldZ"));
            VF_AddSpreadDescriptor(FieldSpreads, Indices, Archetype, ParamStructName, Properties,
                                   TEXT("StrateTopWorldZ"), EVoxelStrateFieldKind::Continuous);
            VF_AddSpreadDescriptor(FieldSpreads, Indices, Archetype, ParamStructName, Properties,
                                   TEXT("StrateBottomWorldZ"), EVoxelStrateFieldKind::Continuous);
        }

        VF_ValidateStructProperties(Properties, ExpectedNames, ParamStructName,
                                     bSchemaValid, SchemaError);

        if (VF_IsTunnelArchetype(Archetype))
        {
            for (const FVoxelStrateFieldExclusion& Exclusion : GetNonTunableFields())
            {
                if (!ExpectedNames.Contains(Exclusion.FieldName))
                {
                    bSchemaValid = false;
                    VF_AppendSchemaError(SchemaError,
                        FString::Printf(TEXT("Explicit exclusion '%s' is absent from %s."),
                                        *Exclusion.FieldName, *ParamStructName));
                }
            }
        }

        for (const FString& ExpectedName : ExpectedNames)
        {
            VF_ValidateFieldDescriptor(FieldSpreads, Archetype, ExpectedName,
                                       bSchemaValid, SchemaError);
        }
    }

    TArray<TArray<double>> Values;
    Values.SetNum(FieldSpreads.Num());
    for (const FVoxelStrateCorpusEntry& Entry : Entries)
    {
        const void* Memory = VF_GetParamMemory(Entry.ArchetypeParams, Entry.Archetype);
        if (Memory == nullptr)
        {
            continue;
        }

        for (int32 FieldIndex = 0; FieldIndex < FieldSpreads.Num(); ++FieldIndex)
        {
            const FVoxelStrateFieldSpread& Spread = FieldSpreads[FieldIndex];
            if (Spread.Archetype != Entry.Archetype)
            {
                continue;
            }

            double Value = 0.0;
            if (VF_ReadNamedField(Memory, Entry.Archetype, Spread.FieldName, Value)
                && FMath::IsFinite(Value))
            {
                Values[FieldIndex].Add(Value);
            }
        }
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
        Spread.Mean = Sum / static_cast<double>(Samples.Num());

        double Variance = 0.0;
        for (const double Sample : Samples)
        {
            const double Delta = Sample - Spread.Mean;
            Variance += Delta * Delta;
        }
        Spread.StdDev = FMath::Sqrt(FMath::Max(0.0, Variance / static_cast<double>(Samples.Num())));
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
    FVoxelStrateArchetypeParams ArchetypeParams;
    ArchetypeParams.TunnelNetworkParams = Params;
    return AddEntry(SourcePath, SourceName, Archetype, ArchetypeParams, Weight);
}

bool FVoxelStrateCorpus::AddEntry(const FString& SourcePath, const FString& SourceName,
                                  ECaveGeneratorType Archetype,
                                  const FVoxelStrateArchetypeParams& Params, float Weight)
{
    if (!VF_IsSupportedArchetype(Archetype))
    {
        SkippedDefinitions.Add(FString::Printf(
            TEXT("%s skipped: unknown archetype value %d."),
            *SourcePath, static_cast<int32>(static_cast<uint8>(Archetype))));
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
    Entry.ArchetypeParams = Params;
    Entry.Params = Params.TunnelNetworkParams;
    Entry.Weight = Weight;

    Entries.Sort([](const FVoxelStrateCorpusEntry& A, const FVoxelStrateCorpusEntry& B)
    {
        if (A.SourcePath != B.SourcePath) { return A.SourcePath < B.SourcePath; }
        if (static_cast<uint8>(A.Archetype) != static_cast<uint8>(B.Archetype))
        {
            return static_cast<uint8>(A.Archetype) < static_cast<uint8>(B.Archetype);
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
    int32 NumDefinitionEntries = 0;
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

        if (!VF_IsSupportedArchetype(Definition->GeneratorType))
        {
            SkippedDefinitions.Add(FString::Printf(
                TEXT("%s (%s) skipped: unknown GeneratorType value %d."),
                *SourcePath, *SourceName,
                static_cast<int32>(static_cast<uint8>(Definition->GeneratorType))));
            continue;
        }

        FVoxelStrateArchetypeParams Params;
        Params.TunnelNetworkParams = Definition->GenerationParams;
        Params.SlabParams = Definition->SlabParams;
        Params.MazeParams = Definition->MazeParams;
        Params.SurfaceParams = Definition->SurfaceParams;
        Params.VerticalShaftParams = Definition->VerticalShaftParams;
        Params.FloatingIslandParams = Definition->FloatingIslandParams;

        if (AddEntry(SourcePath, SourceName, Definition->GeneratorType, Params, 1.0f))
        {
            ++NumDefinitionEntries;
        }
    }

    // C++ member initializers in VoxelStrateTypes.h are hand-tuned, known-good starting points.
    // Add one member for every exact archetype so an empty group still has a valid parent and so
    // the defaults are measured alongside project assets.
    FVoxelStrateArchetypeParams Defaults;
    int32 NumDefaultEntries = 0;
    for (const ECaveGeneratorType Archetype : GAllArchetypes)
    {
        const FString ArchetypeName = VF_GetStrateArchetypeName(Archetype);
        const FString DefaultPath = FString::Printf(
            TEXT("/VoxelForge/ComposerDefaults/%s"), *ArchetypeName);
        const FString DefaultName = FString::Printf(TEXT("Default_%s"), *ArchetypeName);
        if (AddEntry(DefaultPath, DefaultName, Archetype, Defaults, 1.0f))
        {
            ++NumDefaultEntries;
        }
    }

    RebuildSpreads();
    TSet<uint8> ArchetypeSet;
    for (const FVoxelStrateCorpusEntry& Entry : Entries)
    {
        ArchetypeSet.Add(static_cast<uint8>(Entry.Archetype));
    }
    OutReport = FString::Printf(
        TEXT("Corpus definitions: %d resolved project vectors + %d archetype defaults = %d corpus "
             "members; %d exact-archetype groups; %d field descriptors measured. "),
        NumDefinitionEntries, NumDefaultEntries, Entries.Num(), ArchetypeSet.Num(),
        FieldSpreads.Num());
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

static bool VF_IsProjectPackagePath(const FString& PackagePath)
{
    return PackagePath == TEXT("/Game")
        || PackagePath.StartsWith(TEXT("/Game/"), ESearchCase::IgnoreCase);
}

static bool VF_IsIgnoredSavedCopy(const FString& PackagePath)
{
    return PackagePath.Contains(TEXT("/Saved/Autosaves"), ESearchCase::IgnoreCase)
        || PackagePath.Contains(TEXT("/Saved/Cooked"), ESearchCase::IgnoreCase);
}

bool FVoxelStrateCorpus::LoadFromAssetRegistry(FString& OutReport)
{
    FAssetRegistryModule& AssetRegistryModule =
        FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));

    TArray<FAssetData> AssetData;
    AssetRegistryModule.Get().GetAssetsByClass(
        UVoxelStrateDefinition::StaticClass()->GetClassPathName(), AssetData, true);
    AssetData.Sort([](const FAssetData& A, const FAssetData& B)
    {
        return A.GetObjectPathString() < B.GetObjectPathString();
    });

    TArray<UVoxelStrateDefinition*> Definitions;
    TSet<FString> SeenObjectPaths;
    int32 NumIgnoredSaved = 0;
    int32 NumOutsideProject = 0;
    int32 NumDuplicateRecords = 0;
    int32 NumUnresolved = 0;
    for (const FAssetData& Asset : AssetData)
    {
        const FString PackagePath = Asset.PackageName.ToString();
        const FString ObjectPath = Asset.GetObjectPathString();
        if (!VF_IsProjectPackagePath(PackagePath))
        {
            ++NumOutsideProject;
            continue;
        }
        if (VF_IsIgnoredSavedCopy(PackagePath) || VF_IsIgnoredSavedCopy(ObjectPath))
        {
            ++NumIgnoredSaved;
            continue;
        }
        if (SeenObjectPaths.Contains(ObjectPath))
        {
            ++NumDuplicateRecords;
            continue;
        }
        SeenObjectPaths.Add(ObjectPath);

        UVoxelStrateDefinition* Definition = Cast<UVoxelStrateDefinition>(Asset.GetAsset());
        if (Definition == nullptr)
        {
            ++NumUnresolved;
            continue;
        }
        Definitions.Add(Definition);
    }

    FString DefinitionReport;
    const bool bLoaded = LoadFromDefinitions(Definitions, DefinitionReport);
    OutReport = FString::Printf(
        TEXT("Asset Registry strate scan: %d records; %d project candidates; %d Saved/Autosaves "
             "or Saved/Cooked copies ignored; %d outside project; %d duplicate records; %d unresolved. "
             "%s"),
        AssetData.Num(), SeenObjectPaths.Num(), NumIgnoredSaved, NumOutsideProject,
        NumDuplicateRecords, NumUnresolved, *DefinitionReport);
    return bLoaded && NumUnresolved == 0;
}

bool FVoxelStrateCorpus::LoadFromSettings(const UVoxelSettings* Settings, FString& OutReport)
{
    // Compatibility/audit entry point. It intentionally does not load any soft reference: the
    // Asset Registry is the corpus source, while the settings pool is reported as the reason the
    // first Tier 4a run saw only one vector.
    TArray<FString> SettingsPaths;
    if (Settings != nullptr)
    {
        TSet<FString> UniquePaths;
        for (const TPair<int32, TSoftObjectPtr<UVoxelStrateDefinition>>& Pair : Settings->FixedStrates)
        {
            const FString Path = Pair.Value.ToSoftObjectPath().ToString();
            if (!Path.IsEmpty())
            {
                UniquePaths.Add(Path);
            }
        }
        for (const TSoftObjectPtr<UVoxelStrateDefinition>& Reference : Settings->StratePool)
        {
            const FString Path = Reference.ToSoftObjectPath().ToString();
            if (!Path.IsEmpty())
            {
                UniquePaths.Add(Path);
            }
        }
        for (const FString& Path : UniquePaths)
        {
            SettingsPaths.Add(Path);
        }
        SettingsPaths.Sort();
    }

    FString RegistryReport;
    const bool bLoaded = LoadFromAssetRegistry(RegistryReport);
    OutReport = FString::Printf(
        TEXT("Settings audit: %d unique fixed/pool soft paths"), SettingsPaths.Num());
    for (const FString& Path : SettingsPaths)
    {
        OutReport += FString::Printf(TEXT(" [%s]"), *Path);
    }
    if (Settings == nullptr)
    {
        OutReport += TEXT(" [settings object was null]");
    }
    OutReport += TEXT("; settings references are diagnostic only. ");
    OutReport += RegistryReport;
    return bLoaded;
}

int32 FVoxelStrateCorpus::NumForArchetype(ECaveGeneratorType Archetype) const
{
    int32 Count = 0;
    for (const FVoxelStrateCorpusEntry& Entry : Entries)
    {
        if (Entry.Archetype == Archetype)
        {
            ++Count;
        }
    }
    return Count;
}

const FVoxelStrateFieldSpread* FVoxelStrateCorpus::FindSpread(
    ECaveGeneratorType Archetype, const FString& FieldName) const
{
    for (const FVoxelStrateFieldSpread& Spread : FieldSpreads)
    {
        if (Spread.Archetype == Archetype && Spread.FieldName == FieldName)
        {
            return &Spread;
        }
    }
    return nullptr;
}

const FVoxelStrateFieldSpread* FVoxelStrateCorpus::FindSpread(const FString& FieldName) const
{
    // Compatibility lookup: the original API had one tunnel-family namespace. Prefer
    // TunnelNetwork, then fall back to the first matching family for old diagnostics.
    if (const FVoxelStrateFieldSpread* Spread = FindSpread(ECaveGeneratorType::TunnelNetwork, FieldName))
    {
        return Spread;
    }
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
            Hash ^= static_cast<uint32>(Bytes[ByteIndex]);
            Hash *= 16777619U;
        }
    };
    auto HashString = [&HashBytes](const FString& String)
    {
        for (int32 CharIndex = 0; CharIndex < String.Len(); ++CharIndex)
        {
            const uint32 Code = static_cast<uint32>(String[CharIndex]);
            HashBytes(&Code, sizeof(Code));
        }
        const uint32 Terminator = 0U;
        HashBytes(&Terminator, sizeof(Terminator));
    };

    for (const FVoxelStrateCorpusEntry& Entry : Entries)
    {
        HashString(Entry.SourcePath);
        HashString(Entry.SourceName);
        const uint8 Archetype = static_cast<uint8>(Entry.Archetype);
        HashBytes(&Archetype, sizeof(Archetype));
        HashBytes(&Entry.Weight, sizeof(Entry.Weight));

        const void* Memory = VF_GetParamMemory(Entry.ArchetypeParams, Entry.Archetype);
        UStruct* Struct = VF_GetParamStruct(Entry.Archetype);
        if (Memory == nullptr || Struct == nullptr)
        {
            continue;
        }

        if (VF_IsTunnelArchetype(Entry.Archetype))
        {
#define VF_HASH_FSTRATE_FIELD(Name) \
            HashString(TEXT(#Name)); \
            HashBytes(&Entry.ArchetypeParams.TunnelNetworkParams.Name, \
                      sizeof(Entry.ArchetypeParams.TunnelNetworkParams.Name));
            VF_STRATE_PARAM_FIELDS(VF_HASH_FSTRATE_FIELD, VF_HASH_FSTRATE_FIELD)
#undef VF_HASH_FSTRATE_FIELD
        }
        else
        {
            TMap<FString, FProperty*> Properties;
            VF_CollectProperties(Struct, Properties);
            for (const TPair<FString, FProperty*>& Pair : Properties)
            {
                HashString(Pair.Key);
                if (VF_IsScalarProperty(Pair.Value))
                {
                    const void* ValuePtr = Pair.Value->ContainerPtrToValuePtr<void>(Memory);
                    HashBytes(ValuePtr, Pair.Value->GetSize());
                }
            }

            static const TCHAR* RuntimeFields[] =
            {
                TEXT("StrateTopWorldZ"), TEXT("StrateBottomWorldZ")
            };
            for (const TCHAR* RuntimeField : RuntimeFields)
            {
                double Value = 0.0;
                if (VF_ReadNamedField(Memory, Entry.Archetype, RuntimeField, Value))
                {
                    HashString(RuntimeField);
                    HashBytes(&Value, sizeof(Value));
                }
            }
        }
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

bool VF_AreStrateArchetypeParamsBitIdentical(
    const FVoxelStrateArchetypeParams& A,
    const FVoxelStrateArchetypeParams& B,
    ECaveGeneratorType Archetype)
{
    if (VF_IsTunnelArchetype(Archetype))
    {
        return VF_AreStrateParamsBitIdentical(A.TunnelNetworkParams, B.TunnelNetworkParams);
    }

    const void* AMemory = VF_GetParamMemory(A, Archetype);
    const void* BMemory = VF_GetParamMemory(B, Archetype);
    UStruct* Struct = VF_GetParamStruct(Archetype);
    if (AMemory == nullptr || BMemory == nullptr || Struct == nullptr)
    {
        return false;
    }

    TMap<FString, FProperty*> Properties;
    VF_CollectProperties(Struct, Properties);
    for (const TPair<FString, FProperty*>& Pair : Properties)
    {
        const FProperty* Property = Pair.Value;
        const void* AValue = Property->ContainerPtrToValuePtr<void>(AMemory);
        const void* BValue = Property->ContainerPtrToValuePtr<void>(BMemory);
        if (const FBoolProperty* BoolProperty = CastField<FBoolProperty>(Property))
        {
            if (BoolProperty->GetPropertyValue(AValue) != BoolProperty->GetPropertyValue(BValue))
            {
                return false;
            }
        }
        else if (FMemory::Memcmp(AValue, BValue, Property->GetSize()) != 0)
        {
            return false;
        }
    }

    double ATop = 0.0;
    double BTop = 0.0;
    double ABottom = 0.0;
    double BBottom = 0.0;
    return VF_ReadRuntimeField(AMemory, Archetype, TEXT("StrateTopWorldZ"), ATop)
        && VF_ReadRuntimeField(BMemory, Archetype, TEXT("StrateTopWorldZ"), BTop)
        && VF_ReadRuntimeField(AMemory, Archetype, TEXT("StrateBottomWorldZ"), ABottom)
        && VF_ReadRuntimeField(BMemory, Archetype, TEXT("StrateBottomWorldZ"), BBottom)
        && FMemory::Memcmp(&ATop, &BTop, sizeof(ATop)) == 0
        && FMemory::Memcmp(&ABottom, &BBottom, sizeof(ABottom)) == 0;
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
                   && VF_AreStrateArchetypeParamsBitIdentical(
                       Repeat.ArchetypeParams, Result.ArchetypeParams, Result.Archetype)
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
