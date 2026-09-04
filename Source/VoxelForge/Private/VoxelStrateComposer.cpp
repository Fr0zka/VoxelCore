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
#include "VoxelTerrainOpDefinition.h"
#include "VoxelDensityOpStack.h"

#include <type_traits>
#include <utility>

namespace
{
    constexpr float GJitterFraction = 0.15f;
    constexpr float GBootstrapJitterFraction = 0.25f;
    constexpr double GNearZeroSpread = 1.0e-6;
    constexpr double GBootstrapMagnitudeFloor = 1.0;

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

    const TCHAR* VF_GetTerrainOperationFieldPrefix(EVoxelTerrainOpType Type)
    {
        switch (Type)
        {
        case EVoxelTerrainOpType::Terrace:    return TEXT("Terrace");
        case EVoxelTerrainOpType::LayerLines: return TEXT("LayerLine");
        case EVoxelTerrainOpType::Ribbing:    return TEXT("Ribbing");
        case EVoxelTerrainOpType::Cliff:      return TEXT("Cliff");
        case EVoxelTerrainOpType::Scallop:    return TEXT("Scallop");
        case EVoxelTerrainOpType::Overhang:   return TEXT("Overhang");
        case EVoxelTerrainOpType::Arch:       return TEXT("Arch");
        case EVoxelTerrainOpType::Column:     return TEXT("Column");
        case EVoxelTerrainOpType::Pit:        return TEXT("Pit");
        case EVoxelTerrainOpType::Chimney:    return TEXT("Chimney");
        case EVoxelTerrainOpType::Dome:       return TEXT("Dome");
        case EVoxelTerrainOpType::Pinch:      return TEXT("Pinch");
        default:                              return TEXT("");
        }
    }

    void VF_AddTerrainOperationDefaultSamples(
        TArray<TArray<double>>& Values,
        const TArray<FVoxelStrateFieldSpread>& Spreads)
    {
        if (Values.Num() != Spreads.Num())
        {
            return;
        }

        UVoxelTerrainOpDefinition* Operation = NewObject<UVoxelTerrainOpDefinition>(
            GetTransientPackage(), NAME_None, RF_Transient);
        if (Operation == nullptr)
        {
            return;
        }

        static const EVoxelTerrainOpType OperationTypes[] =
        {
            EVoxelTerrainOpType::Terrace,
            EVoxelTerrainOpType::LayerLines,
            EVoxelTerrainOpType::Ribbing,
            EVoxelTerrainOpType::Cliff,
            EVoxelTerrainOpType::Scallop,
            EVoxelTerrainOpType::Overhang,
            EVoxelTerrainOpType::Arch,
            EVoxelTerrainOpType::Column,
            EVoxelTerrainOpType::Pit,
            EVoxelTerrainOpType::Chimney,
            EVoxelTerrainOpType::Dome,
            EVoxelTerrainOpType::Pinch,
        };

        for (const EVoxelTerrainOpType Type : OperationTypes)
        {
            const TCHAR* Prefix = VF_GetTerrainOperationFieldPrefix(Type);
            Operation->Type = Type;
            FStrateGenerationParams Defaults;
            Operation->ApplyTo(Defaults);

            // Seed only the fields owned by this operation. The values come from the actual
            // UVoxelTerrainOpDefinition UPROPERTY initializers through ApplyTo; no second asset
            // corpus and no duplicated default constants are introduced here.
            for (int32 FieldIndex = 0; FieldIndex < Spreads.Num(); ++FieldIndex)
            {
                const FVoxelStrateFieldSpread& Spread = Spreads[FieldIndex];
                if (!VF_IsTunnelArchetype(Spread.Archetype)
                    || Spread.bExcluded
                    || !Spread.FieldName.StartsWith(Prefix))
                {
                    continue;
                }

                double Value = 0.0;
                if (VF_ReadNamedField(&Defaults, Spread.Archetype, Spread.FieldName, Value)
                    && FMath::IsFinite(Value))
                {
                    Values[FieldIndex].Add(Value);
                }
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

    void VF_ResetDeadTerrainTransportFields(FStrateGenerationParams& Params)
    {
        // Empirical liveness proved these direct FStrate slots are dead: the runtime bakes these
        // three operation families from UVoxelTerrainOpDefinition values into room caches before
        // sampling. Keep them at their neutral values until terrain-op structs are rolled too.
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
            VF_ResetDeadTerrainTransportFields(Params.TunnelNetworkParams);
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
        double NativeValue = 0.0;
        FStrateGenerationParams NativeDefaults;
        const bool bNativeFStrateField = VF_IsTunnelArchetype(Archetype)
            && VF_ReadFStrateField(NativeDefaults, FieldName, NativeValue);
        if (Spread != nullptr && !Spread->bReflected && !Spread->bExcluded
            && !bNativeFStrateField)
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

    void VF_JitterMeasuredField(double& Value,
                                const FVoxelStrateFieldSpread& Spread,
                                ECaveGeneratorType Archetype,
                                FRandomStream& Rng,
                                int32& OutBootstrapFieldCount,
                                TArray<FString>& OutBootstrapFieldNames)
    {
        const double CorpusRange = FMath::Max(0.0, Spread.Max - Spread.Min);
        const bool bUseBootstrap = CorpusRange <= GNearZeroSpread;

        // Spread-derived jitter is the real rule. This bootstrap is only for a starved corpus:
        // when season zero has supplied no useful measured range, use a fraction of this field's
        // own magnitude, with a one-native-unit floor near zero. Promotion (Tier 5) should retire
        // this fallback as the corpus grows.
        const double JitterScale = bUseBootstrap
            ? FMath::Max(FMath::Abs(Value), GBootstrapMagnitudeFloor)
            : CorpusRange;
        const float JitterFraction = bUseBootstrap
            ? GBootstrapJitterFraction : GJitterFraction;
        Value += static_cast<double>((Rng.FRand() * 2.0f - 1.0f) * JitterFraction)
            * JitterScale;

        if (bUseBootstrap)
        {
            ++OutBootstrapFieldCount;
            OutBootstrapFieldNames.AddUnique(VF_SpreadKey(Archetype, Spread.FieldName));
        }
    }

    void VF_JitterNativeParams(FVoxelStrateArchetypeParams& Params,
                               ECaveGeneratorType Archetype,
                               const FVoxelStrateCorpus& Corpus,
                               FRandomStream& Rng,
                               int32& OutBootstrapFieldCount,
                               TArray<FString>& OutBootstrapFieldNames)
    {
        void* Memory = VF_GetParamMemory(Params, Archetype);
        UStruct* Struct = VF_GetParamStruct(Archetype);
        if (Memory == nullptr || Struct == nullptr)
        {
            return;
        }

        // FStrateGenerationParams deliberately keeps the terrain-detail transport fields as
        // native, non-UPROPERTY members. Handle those fields through the same measured spread
        // policy as reflected fields; otherwise removing an exclusion would only change the
        // report while the candidate value stayed at its blended parent value.
        if (VF_IsTunnelArchetype(Archetype))
        {
            for (const FVoxelStrateFieldSpread& Spread : Corpus.GetFieldSpreads())
            {
                if (Spread.Archetype != Archetype
                    || Spread.bReflected
                    || Spread.bExcluded
                    || Spread.Kind != EVoxelStrateFieldKind::Continuous)
                {
                    continue;
                }

                double Value = 0.0;
                if (VF_ReadNamedField(Memory, Archetype, Spread.FieldName, Value))
                {
                    VF_JitterMeasuredField(Value, Spread, Archetype, Rng,
                                           OutBootstrapFieldCount, OutBootstrapFieldNames);
                    VF_WriteNamedField(Memory, Archetype, Spread.FieldName, Value);
                }
            }
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

            double Value = 0.0;
            if (VF_ReadPropertyValue(Pair.Value, Memory, Value))
            {
                VF_JitterMeasuredField(Value, *Spread, Archetype, Rng,
                                       OutBootstrapFieldCount, OutBootstrapFieldNames);
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
                                         int32 Seed, int32 Index,
                                         const ECaveGeneratorType* ForcedArchetype = nullptr)
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
        if (ForcedArchetype != nullptr)
        {
            GroupIndex = INDEX_NONE;
            for (int32 CandidateGroup = 0; CandidateGroup < Groups.Num(); ++CandidateGroup)
            {
                if (Groups[CandidateGroup].Archetype == *ForcedArchetype)
                {
                    GroupIndex = CandidateGroup;
                    break;
                }
            }
            if (GroupIndex == INDEX_NONE)
            {
                Result.FailureReason = TEXT("The requested archetype has no corpus group.");
                return Result;
            }
        }
        else if (TotalGroupWeight > 0.0f)
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
        VF_JitterNativeParams(Result.ArchetypeParams, Result.Archetype, Corpus, Rng,
                              Result.BootstrapJitterFieldCount,
                              Result.BootstrapJitterFieldNames);
        VF_ClampNativeParams(Result.ArchetypeParams, Result.Archetype, Corpus);
        VF_RepairNativeOrderedPairs(Result.ArchetypeParams, Result.Archetype);
        VF_ResetExcludedFields(Result.ArchetypeParams, Result.Archetype);

        // Preserve the original compact API's compatibility view. Detailed callers use the
        // correctly typed active family in ArchetypeParams.
        Result.Params = Result.ArchetypeParams.TunnelNetworkParams;
        Result.bValid = true;
        return Result;
    }

    struct FStructureModifierCandidate
    {
        EVoxelStrateOpClass OpClass;
        EVoxelStrateParamBlock ParamBlock;
    };

    static const EVoxelStrateOpClass GStructureShapeSources[] =
    {
        EVoxelStrateOpClass::RoomGraphSource,
        EVoxelStrateOpClass::LatticeCorridorSource,
        EVoxelStrateOpClass::ShaftFieldSource,
        EVoxelStrateOpClass::IslandBlobSource,
        EVoxelStrateOpClass::NoiseRibbonSource,
    };

    // Fixed order is intentional. This is a source-controlled catalogue, not a hash-container
    // iteration; changing it is a manifest-versioned design change rather than an accidental RNG
    // change.
    static const FStructureModifierCandidate GStructureModifiers[] =
    {
        { EVoxelStrateOpClass::SdfRoughnessMod,       EVoxelStrateParamBlock::None },
        { EVoxelStrateOpClass::SdfCarve,              EVoxelStrateParamBlock::None },
        { EVoxelStrateOpClass::SdfFill,               EVoxelStrateParamBlock::None },
        { EVoxelStrateOpClass::CaveRoughnessMod,     EVoxelStrateParamBlock::TunnelNetwork },
        { EVoxelStrateOpClass::WormFieldSource,      EVoxelStrateParamBlock::TunnelNetwork },
        { EVoxelStrateOpClass::GridColumnMod,        EVoxelStrateParamBlock::Slab },
        { EVoxelStrateOpClass::DensityNoiseCarveMod, EVoxelStrateParamBlock::None },
        { EVoxelStrateOpClass::DensityNoiseFillMod,  EVoxelStrateParamBlock::None },
        { EVoxelStrateOpClass::CaveTerraceMod,       EVoxelStrateParamBlock::TunnelNetwork },
        { EVoxelStrateOpClass::LayerLineMod,         EVoxelStrateParamBlock::TunnelNetwork },
        { EVoxelStrateOpClass::RibbingMod,           EVoxelStrateParamBlock::TunnelNetwork },
        { EVoxelStrateOpClass::CaveOverhangMod,      EVoxelStrateParamBlock::TunnelNetwork },
        { EVoxelStrateOpClass::CaveCliffMod,         EVoxelStrateParamBlock::TunnelNetwork },
        { EVoxelStrateOpClass::ScallopMod,           EVoxelStrateParamBlock::TunnelNetwork },
        { EVoxelStrateOpClass::CaveArchMod,          EVoxelStrateParamBlock::TunnelNetwork },
        { EVoxelStrateOpClass::RoomColumnMod,        EVoxelStrateParamBlock::TunnelNetwork },
        { EVoxelStrateOpClass::DomeMod,              EVoxelStrateParamBlock::TunnelNetwork },
        { EVoxelStrateOpClass::PinchMod,             EVoxelStrateParamBlock::TunnelNetwork },
        { EVoxelStrateOpClass::FloorBiasMod,         EVoxelStrateParamBlock::TunnelNetwork },
        { EVoxelStrateOpClass::ShaftLedgeMod,        EVoxelStrateParamBlock::VerticalShaft },
    };

    uint32 VF_StructureRollSeed(int32 Seed, int32 Index)
    {
        uint32 Value = VF_Avalanche(static_cast<uint32>(Seed) ^ 0xD1CEB00Bu);
        Value = VF_Avalanche(Value ^ (static_cast<uint32>(Index) + 0x7F4A7C15u));
        return Value;
    }

    bool VF_IsContractLegalAfter(EVoxelOpChannelMask AvailableChannels,
                                 EVoxelOpResourceMask AvailableResources,
                                 const FVoxelStrateOpContract& Contract)
    {
        if ((Contract.Reads & static_cast<EVoxelOpChannelMask>(~AvailableChannels)) != 0
            || (Contract.RequiredResources
                & static_cast<EVoxelOpResourceMask>(~AvailableResources)) != 0)
        {
            return false;
        }
        if (Contract.bAdditive
            && (Contract.Writes == VoxelOpChannels::None
                || (Contract.Writes & static_cast<EVoxelOpChannelMask>(~Contract.Reads)) != 0))
        {
            return false;
        }
        for (const EVoxelOpChannelMask Channel :
             { VoxelOpChannels::Density, VoxelOpChannels::Sdf })
        {
            if ((Contract.Writes & Channel) == 0 || (Contract.Reads & Channel) != 0)
            {
                continue;
            }
            // All channels are already published by root + shape. A write-only candidate would
            // therefore be a replacement clobber and is excluded before it can enter a recipe.
            return false;
        }
        return true;
    }

    FVoxelOpRecipeEntry VF_MakeRecipeEntry(EVoxelStrateOpClass OpClass,
                                            EVoxelStrateParamBlock ParamBlock)
    {
        FVoxelOpRecipeEntry Entry;
        Entry.OpClass = OpClass;
        Entry.ParamBlock = ParamBlock;
        return Entry;
    }

    FVoxelOpStackRecipe VF_RollStructureInternal(int32 Seed, int32 Index)
    {
        FVoxelOpStackRecipe Recipe;
        FRandomStream Rng(static_cast<int32>(VF_StructureRollSeed(Seed, Index)));

        Recipe.RootPolarity = Rng.RandRange(0, 1) == 0
            ? EVoxelStrateRootPolarity::RockCarve
            : EVoxelStrateRootPolarity::VoidFill;

        const EVoxelStrateOpClass ShapeClass = GStructureShapeSources[
            Rng.RandRange(0, UE_ARRAY_COUNT(GStructureShapeSources) - 1)];
        const EVoxelStrateParamBlock ShapeBlock =
            ShapeClass == EVoxelStrateOpClass::RoomGraphSource
                ? EVoxelStrateParamBlock::TunnelNetwork
                : ShapeClass == EVoxelStrateOpClass::ShaftFieldSource
                    ? EVoxelStrateParamBlock::VerticalShaft
                    : ShapeClass == EVoxelStrateOpClass::IslandBlobSource
                        ? EVoxelStrateParamBlock::FloatingIsland
                        : EVoxelStrateParamBlock::Maze;

        Recipe.Root = VF_MakeRecipeEntry(
            Recipe.RootPolarity == EVoxelStrateRootPolarity::VoidFill
                ? EVoxelStrateOpClass::ConstantVoidSource
                : EVoxelStrateOpClass::ConstantRockSource,
            ShapeBlock);
        Recipe.ShapeSource = VF_MakeRecipeEntry(ShapeClass, ShapeBlock);
        Recipe.Conversion = VF_MakeRecipeEntry(
            Recipe.RootPolarity == EVoxelStrateRootPolarity::VoidFill
                ? EVoxelStrateOpClass::SdfFill
                : EVoxelStrateOpClass::SdfCarve,
            ShapeBlock);
        Recipe.StructuralParamBlock = ShapeBlock;

        FVoxelStrateOpContract ShapeContract;
        const bool bShapeDeclared = VoxelDensityOps::GetStrateOpContract(ShapeClass, ShapeContract);
        checkf(bShapeDeclared, TEXT("Structure catalogue contains an undeclared shape source."));

        const EVoxelOpChannelMask AvailableChannels = VoxelOpChannels::Density | VoxelOpChannels::Sdf;
        const EVoxelOpResourceMask AvailableResources = ShapeContract.ProvidedResources;

        TArray<FStructureModifierCandidate> Legal;
        for (const FStructureModifierCandidate& Candidate : GStructureModifiers)
        {
            FVoxelStrateOpContract Contract;
            if (!VoxelDensityOps::GetStrateOpContract(Candidate.OpClass, Contract))
            {
                checkf(false, TEXT("Structure catalogue contains an undeclared modifier."));
                continue;
            }
            if (VF_IsContractLegalAfter(AvailableChannels, AvailableResources, Contract))
            {
                Legal.Add(Candidate);
            }
        }

        const int32 K = Rng.RandRange(4, 8);
        checkf(Legal.Num() >= K,
               TEXT("Structure roller has fewer legal modifiers than its rolled k."));
        for (int32 ModifierIndex = 0; ModifierIndex < K && Legal.Num() > 0; ++ModifierIndex)
        {
            const int32 Pick = Rng.RandRange(0, Legal.Num() - 1);
            const FStructureModifierCandidate Candidate = Legal[Pick];
            Recipe.Modifiers.Add(VF_MakeRecipeEntry(
                Candidate.OpClass,
                Candidate.ParamBlock == EVoxelStrateParamBlock::None
                    ? ShapeBlock : Candidate.ParamBlock));
            Legal.RemoveAt(Pick, 1, EAllowShrinking::No);
        }
        return Recipe;
    }
}

const TArray<FVoxelStrateFieldExclusion>& FVoxelStrateCorpus::GetNonTunableFields()
{
    static const TArray<FVoxelStrateFieldExclusion> Exclusions = []
    {
        TArray<FVoxelStrateFieldExclusion> Result;

        const TCHAR* DeadTerrainOpReason = TEXT(
            "Empirically dead when only this FStrateGenerationParams slot changes: the runtime "
            "bakes Column/Pit/Chimney geometry from UVoxelTerrainOpDefinition values into the "
            "room cache, so this direct strata slot does not feed GetDensityAt.");
        VF_AddExclusion(Result, TEXT("ColumnDensity"), DeadTerrainOpReason);
        VF_AddExclusion(Result, TEXT("ColumnMinRadius"), DeadTerrainOpReason);
        VF_AddExclusion(Result, TEXT("ColumnMaxRadius"), DeadTerrainOpReason);
        VF_AddExclusion(Result, TEXT("PitDensity"), DeadTerrainOpReason);
        VF_AddExclusion(Result, TEXT("PitMinRadius"), DeadTerrainOpReason);
        VF_AddExclusion(Result, TEXT("PitMaxRadius"), DeadTerrainOpReason);
        VF_AddExclusion(Result, TEXT("PitDepth"), DeadTerrainOpReason);
        VF_AddExclusion(Result, TEXT("ChimneyDensity"), DeadTerrainOpReason);
        VF_AddExclusion(Result, TEXT("ChimneyMinRadius"), DeadTerrainOpReason);
        VF_AddExclusion(Result, TEXT("ChimneyMaxRadius"), DeadTerrainOpReason);
        VF_AddExclusion(Result, TEXT("ChimneyHeight"), DeadTerrainOpReason);

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
        FieldSpreads[FieldIndex].AuthoredSampleCount = Values[FieldIndex].Num();
    }
    VF_AddTerrainOperationDefaultSamples(Values, FieldSpreads);

    for (int32 FieldIndex = 0; FieldIndex < FieldSpreads.Num(); ++FieldIndex)
    {
        FVoxelStrateFieldSpread& Spread = FieldSpreads[FieldIndex];
        const TArray<double>& Samples = Values[FieldIndex];
        Spread.DefaultSeedCount = Samples.Num() - Spread.AuthoredSampleCount;
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

double VF_DistanceFromStrateCorpusCentroid(
    const FVoxelStrateCorpus& Corpus,
    const FVoxelStrateArchetypeParams& Params,
    ECaveGeneratorType Archetype)
{
    const void* Memory = VF_GetParamMemory(Params, Archetype);
    if (Memory == nullptr)
    {
        return 0.0;
    }

    double DistanceSquared = 0.0;
    for (const FVoxelStrateFieldSpread& Spread : Corpus.GetFieldSpreads())
    {
        if (Spread.Archetype != Archetype || Spread.bExcluded
            || Spread.Kind == EVoxelStrateFieldKind::Boolean
            || Spread.Kind == EVoxelStrateFieldKind::Enum)
        {
            continue;
        }

        double Value = 0.0;
        if (!VF_ReadNamedField(Memory, Archetype, Spread.FieldName, Value))
        {
            continue;
        }

        const double CorpusRange = FMath::Max(0.0, Spread.Max - Spread.Min);
        const double Scale = CorpusRange > GNearZeroSpread
            ? CorpusRange
            : FMath::Max(FMath::Abs(Spread.Mean) * GBootstrapJitterFraction,
                         GBootstrapMagnitudeFloor);
        if (FMath::IsFinite(Value) && FMath::IsFinite(Spread.Mean) && Scale > 0.0)
        {
            const double Normalized = (Value - Spread.Mean) / Scale;
            DistanceSquared += Normalized * Normalized;
        }
    }
    return FMath::Sqrt(FMath::Max(0.0, DistanceSquared));
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

FVoxelStrateRollInfo VF_RollStrateParamsDetailedForArchetype(
    const FVoxelStrateCorpus& Corpus, ECaveGeneratorType Archetype, int32 Seed, int32 Index)
{
    const FVoxelStrateRollInfo Result = VF_RollInternal(Corpus, Seed, Index, &Archetype);

#if DO_CHECK
    if (Result.bValid)
    {
        const FVoxelStrateRollInfo Repeat = VF_RollInternal(Corpus, Seed, Index, &Archetype);
        checkf(Repeat.bValid && Repeat.Archetype == Result.Archetype
                   && VF_AreStrateArchetypeParamsBitIdentical(
                       Repeat.ArchetypeParams, Result.ArchetypeParams, Result.Archetype)
                   && Repeat.ParentEntryIndices == Result.ParentEntryIndices
                   && Repeat.ParentWeights == Result.ParentWeights,
               TEXT("VF_RollStrateParamsDetailedForArchetype lost determinism."));
    }
#endif
    return Result;
}

FStrateGenerationParams VF_RollStrateParams(const FVoxelStrateCorpus& Corpus,
                                             int32 Seed, int32 Index)
{
    return VF_RollStrateParamsDetailed(Corpus, Seed, Index).Params;
}

namespace
{
    const TCHAR* VF_RecipeOpCode(EVoxelStrateOpClass OpClass)
    {
        switch (OpClass)
        {
        case EVoxelStrateOpClass::ConstantRockSource: return TEXT("rk");
        case EVoxelStrateOpClass::ConstantVoidSource: return TEXT("vd");
        case EVoxelStrateOpClass::RoomGraphSource: return TEXT("rg");
        case EVoxelStrateOpClass::LatticeCorridorSource: return TEXT("lt");
        case EVoxelStrateOpClass::ShaftFieldSource: return TEXT("sh");
        case EVoxelStrateOpClass::IslandBlobSource: return TEXT("is");
        case EVoxelStrateOpClass::NoiseRibbonSource: return TEXT("nr");
        case EVoxelStrateOpClass::SdfRoughnessMod: return TEXT("sr");
        case EVoxelStrateOpClass::SdfCarve: return TEXT("cv");
        case EVoxelStrateOpClass::SdfFill: return TEXT("fl");
        case EVoxelStrateOpClass::GridColumnMod: return TEXT("gc");
        case EVoxelStrateOpClass::CaveRoughnessMod: return TEXT("cr");
        case EVoxelStrateOpClass::CaveTerraceMod: return TEXT("tr");
        case EVoxelStrateOpClass::LayerLineMod: return TEXT("ll");
        case EVoxelStrateOpClass::RibbingMod: return TEXT("rb");
        case EVoxelStrateOpClass::CaveOverhangMod: return TEXT("co");
        case EVoxelStrateOpClass::CaveCliffMod: return TEXT("cc");
        case EVoxelStrateOpClass::ScallopMod: return TEXT("sc");
        case EVoxelStrateOpClass::CaveArchMod: return TEXT("ar");
        case EVoxelStrateOpClass::RoomColumnMod: return TEXT("rc");
        case EVoxelStrateOpClass::DomeMod: return TEXT("dm");
        case EVoxelStrateOpClass::PinchMod: return TEXT("pn");
        case EVoxelStrateOpClass::FloorBiasMod: return TEXT("fb");
        case EVoxelStrateOpClass::WormFieldSource: return TEXT("wm");
        case EVoxelStrateOpClass::ShaftLedgeMod: return TEXT("sl");
        case EVoxelStrateOpClass::DensityNoiseCarveMod: return TEXT("nc");
        case EVoxelStrateOpClass::DensityNoiseFillMod: return TEXT("nf");
        default: return TEXT("??");
        }
    }

    const TCHAR* VF_RecipeBlockCode(EVoxelStrateParamBlock Block)
    {
        switch (Block)
        {
        case EVoxelStrateParamBlock::TunnelNetwork: return TEXT("T");
        case EVoxelStrateParamBlock::Slab: return TEXT("B");
        case EVoxelStrateParamBlock::Maze: return TEXT("M");
        case EVoxelStrateParamBlock::Surface: return TEXT("S");
        case EVoxelStrateParamBlock::VerticalShaft: return TEXT("V");
        case EVoxelStrateParamBlock::FloatingIsland: return TEXT("I");
        default: return TEXT("-");
        }
    }

    FString VF_FormatRecipeEntry(const FVoxelOpRecipeEntry& Entry)
    {
        return FString::Printf(TEXT("%s/%s"), VF_RecipeOpCode(Entry.OpClass),
                               VF_RecipeBlockCode(Entry.ParamBlock));
    }
}

FVoxelOpStackRecipe VF_RollStrateStructure(int32 Seed, int32 Index)
{
    const FVoxelOpStackRecipe Recipe = VF_RollStructureInternal(Seed, Index);

#if DO_CHECK
    const FVoxelOpStackRecipe Repeat = VF_RollStructureInternal(Seed, Index);
    checkf(VF_AreStrateStructureRecipesIdentical(Recipe, Repeat),
           TEXT("VF_RollStrateStructure lost determinism for the same seed/index."));
#endif
    return Recipe;
}

FString VF_FormatStrateStructureRecipe(const FVoxelOpStackRecipe& Recipe)
{
    FString Modifiers;
    for (int32 Index = 0; Index < Recipe.Modifiers.Num(); ++Index)
    {
        if (Index > 0) { Modifiers += TEXT(","); }
        Modifiers += VF_FormatRecipeEntry(Recipe.Modifiers[Index]);
    }
    return FString::Printf(TEXT("%s|r=%s|s=%s|x=%s|m=[%s]|posts=%s"),
                           Recipe.RootPolarity == EVoxelStrateRootPolarity::VoidFill ? TEXT("VF") : TEXT("RC"),
                           *VF_FormatRecipeEntry(Recipe.Root),
                           *VF_FormatRecipeEntry(Recipe.ShapeSource),
                           *VF_FormatRecipeEntry(Recipe.Conversion),
                           *Modifiers,
                           VF_RecipeBlockCode(Recipe.StructuralParamBlock));
}

uint32 VF_HashStrateStructureRecipe(const FVoxelOpStackRecipe& Recipe)
{
    uint32 Hash = 2166136261u;
    auto AddByte = [&Hash](uint8 Value)
    {
        Hash ^= static_cast<uint32>(Value);
        Hash *= 16777619u;
    };
    auto AddEntry = [&AddByte](const FVoxelOpRecipeEntry& Entry)
    {
        AddByte(static_cast<uint8>(Entry.OpClass));
        AddByte(static_cast<uint8>(Entry.ParamBlock));
    };

    AddByte(static_cast<uint8>(Recipe.RootPolarity));
    AddEntry(Recipe.Root);
    AddEntry(Recipe.ShapeSource);
    AddEntry(Recipe.Conversion);
    AddByte(static_cast<uint8>(Recipe.Modifiers.Num()));
    for (const FVoxelOpRecipeEntry& Entry : Recipe.Modifiers) { AddEntry(Entry); }
    AddByte(static_cast<uint8>(Recipe.StructuralParamBlock));
    return Hash;
}

bool VF_AreStrateStructureRecipesIdentical(const FVoxelOpStackRecipe& A,
                                           const FVoxelOpStackRecipe& B)
{
    if (A.RootPolarity != B.RootPolarity
        || A.Root.OpClass != B.Root.OpClass || A.Root.ParamBlock != B.Root.ParamBlock
        || A.ShapeSource.OpClass != B.ShapeSource.OpClass
        || A.ShapeSource.ParamBlock != B.ShapeSource.ParamBlock
        || A.Conversion.OpClass != B.Conversion.OpClass
        || A.Conversion.ParamBlock != B.Conversion.ParamBlock
        || A.StructuralParamBlock != B.StructuralParamBlock
        || A.Modifiers.Num() != B.Modifiers.Num())
    {
        return false;
    }
    for (int32 Index = 0; Index < A.Modifiers.Num(); ++Index)
    {
        if (A.Modifiers[Index].OpClass != B.Modifiers[Index].OpClass
            || A.Modifiers[Index].ParamBlock != B.Modifiers[Index].ParamBlock)
        {
            return false;
        }
    }
    return true;
}
