// Offline strate corpus and parameter roll.

#include "VoxelStrateComposer.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Math/RandomStream.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/EnumProperty.h"
#include "UObject/FieldIterator.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectGlobals.h"

#include "VoxelSettings.h"
#include "VoxelStrateDefinition.h"
#include "VoxelTerrainOpDefinition.h"
#include "VoxelDensityOpStack.h"
#include "VoxelCaveMorphology.h"
#include "VoxelTypes.h"

#include <type_traits>
#include <utility>

#if WITH_EDITOR
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

    struct FCorpusFreeRange
    {
        float Min = 0.0f;
        float Max = 0.0f;
    };

    bool VF_SetCorpusFreeRange(FCorpusFreeRange& Out, float Min, float Max)
    {
        if (!FMath::IsFinite(Min) || !FMath::IsFinite(Max) || Min > Max)
        {
            return false;
        }
        Out.Min = Min;
        Out.Max = Max;
        return true;
    }

    /**
     * Finite envelopes for the corpus-free control. These are deliberately not copied from a
     * corpus entry. UPROPERTY clamps are used where the type declares them; fields with only a
     * lower clamp get the broadest finite envelope implied by the generator's voxel geometry.
     * ConstraintSampled uses the same envelopes for its independent draws and clamps only after
     * its dependent equations have been evaluated.
     */
    bool VF_GetCorpusFreeRange(ECaveGeneratorType Archetype, const FString& FieldName,
                               float StrateHeightInVoxels, FCorpusFreeRange& Out)
    {
        const float H = FMath::Max(StrateHeightInVoxels, static_cast<float>(CHUNK_SIZE));
        const float HQuarter = H * 0.25f;
        const float HHalf = H * 0.5f;

        if (VF_IsRuntimeField(FieldName) || VF_IsExcludedForArchetype(Archetype, FieldName))
        {
            return false;
        }

        auto Set = [&Out](float Min, float Max) -> bool
        {
            return VF_SetCorpusFreeRange(Out, Min, Max);
        };

        if (VF_IsTunnelArchetype(Archetype))
        {
            if (FieldName == TEXT("BaseDensity")) return Set(1.0f, 32.0f);
            if (FieldName == TEXT("VerticalScale")) return Set(0.25f, 2.5f);
            if (FieldName == TEXT("WormFrequency")) return Set(0.002f, 0.10f);
            if (FieldName == TEXT("WormHorizontalBias")) return Set(1.0f, 10.0f);
            if (FieldName == TEXT("WormThreshold")) return Set(0.01f, 0.50f);
            if (FieldName == TEXT("WormStrength")) return Set(1.0f, 64.0f);
            if (FieldName == TEXT("WormNetworkRange")) return Set(0.0f, H * 1.5f);
            if (FieldName == TEXT("RoomSpacing")) return Set(16.0f, H * 2.5f);
            if (FieldName == TEXT("RoomDensity")) return Set(0.0f, 1.0f);
            if (FieldName == TEXT("MinRoomRadius")) return Set(1.0f, HHalf);
            if (FieldName == TEXT("MaxRoomRadius")) return Set(1.0f, H);
            if (FieldName == TEXT("RoomHeightRatio")) return Set(0.10f, 1.0f);
            if (FieldName == TEXT("RoomShapeVariety")) return Set(0.0f, 1.0f);
            if (FieldName == TEXT("RoomFloorCutMin")) return Set(0.0f, 1.0f);
            if (FieldName == TEXT("RoomFloorCutMax")) return Set(0.0f, 1.0f);
            if (FieldName == TEXT("FloorReliefStrength")) return Set(0.0f, HQuarter);
            if (FieldName == TEXT("FloorReliefFrequency")) return Set(0.001f, 0.10f);
            if (FieldName == TEXT("OriginRoomRadius")) return Set(0.0f, H * 0.75f);
            if (FieldName == TEXT("TunnelMinRadius")) return Set(1.0f, HQuarter);
            if (FieldName == TEXT("TunnelMaxRadius")) return Set(1.0f, HHalf);
            if (FieldName == TEXT("TunnelDensity")) return Set(0.0f, 1.0f);
            if (FieldName == TEXT("MaxTunnelLength")) return Set(1.0f, H * 4.0f);
            if (FieldName == TEXT("TunnelWarpStrength")) return Set(0.0f, H);
            if (FieldName == TEXT("TunnelHorizontalBias")) return Set(0.0f, 1.0f);
            if (FieldName == TEXT("TunnelEndpointZOffset")) return Set(0.0f, 1.0f);
            if (FieldName == TEXT("SDFBlendRadius")) return Set(0.0f, H * 0.15f);
            if (FieldName == TEXT("WaterLevelRelative")) return Set(0.0f, 1.0f);
            if (FieldName == TEXT("CaveWarpStrength")) return Set(0.0f, HHalf);
            if (FieldName == TEXT("CaveWarpFrequency")) return Set(0.001f, 0.10f);
            if (FieldName == TEXT("SurfaceRoughness")) return Set(0.0f, HQuarter);
            if (FieldName == TEXT("RoughnessFrequency")) return Set(0.001f, 0.20f);
            if (FieldName == TEXT("BoundarySealThickness")) return Set(0.0f, HQuarter);
            if (FieldName == TEXT("DomainWarpStrength")) return Set(0.0f, HHalf);
            if (FieldName == TEXT("DomainWarpFrequency")) return Set(0.001f, 0.20f);
            if (FieldName == TEXT("FloorBias")) return Set(0.0f, HQuarter);
            if (FieldName == TEXT("TerraceStepHeight")) return Set(0.0f, HQuarter);
            if (FieldName == TEXT("TerraceHardness")) return Set(0.0f, 1.0f);
            if (FieldName == TEXT("TerraceNoiseDisplacement")) return Set(0.0f, 1.0f);
            if (FieldName == TEXT("LayerLineSpacing")) return Set(0.0f, H);
            if (FieldName == TEXT("LayerLineDepth")) return Set(0.0f, H * 0.10f);
            if (FieldName == TEXT("OverhangStrength")) return Set(0.0f, 1.0f);
            if (FieldName == TEXT("OverhangDepth")) return Set(0.0f, HQuarter);
            if (FieldName == TEXT("OverhangFrequency")) return Set(0.001f, 0.20f);
            if (FieldName == TEXT("RibbingSpacing")) return Set(0.0f, HQuarter);
            if (FieldName == TEXT("RibbingDepth")) return Set(0.0f, H * 0.10f);
            if (FieldName == TEXT("CliffStrength")) return Set(0.0f, 1.0f);
            if (FieldName == TEXT("ScallopStrength")) return Set(0.0f, 1.0f);
            if (FieldName == TEXT("ScallopFrequency")) return Set(0.001f, 0.20f);
            if (FieldName == TEXT("ArchDensity")) return Set(0.0f, 1.0f);
            if (FieldName == TEXT("ArchMinRadius")) return Set(1.0f, H * 0.20f);
            if (FieldName == TEXT("ArchMaxRadius")) return Set(1.0f, H * 0.20f);
            if (FieldName == TEXT("ColumnDensity")) return Set(0.0f, 1.0f);
            if (FieldName == TEXT("ColumnMinRadius")) return Set(1.0f, H * 0.20f);
            if (FieldName == TEXT("ColumnMaxRadius")) return Set(1.0f, H * 0.40f);
            if (FieldName == TEXT("PitDensity")) return Set(0.0f, 1.0f);
            if (FieldName == TEXT("PitMinRadius")) return Set(1.0f, H * 0.20f);
            if (FieldName == TEXT("PitMaxRadius")) return Set(1.0f, H * 0.40f);
            if (FieldName == TEXT("PitDepth")) return Set(0.0f, HHalf);
            if (FieldName == TEXT("ChimneyDensity")) return Set(0.0f, 1.0f);
            if (FieldName == TEXT("ChimneyMinRadius")) return Set(1.0f, H * 0.20f);
            if (FieldName == TEXT("ChimneyMaxRadius")) return Set(1.0f, H * 0.40f);
            if (FieldName == TEXT("ChimneyHeight")) return Set(0.0f, HHalf);
            if (FieldName == TEXT("DomeDensity")) return Set(0.0f, 1.0f);
            if (FieldName == TEXT("DomeMinRadius")) return Set(1.0f, H * 0.30f);
            if (FieldName == TEXT("DomeMaxRadius")) return Set(1.0f, H * 0.50f);
            if (FieldName == TEXT("DomeHeightRatio")) return Set(0.0f, 1.0f);
            if (FieldName == TEXT("PinchDensity")) return Set(0.0f, 1.0f);
            if (FieldName == TEXT("PinchStrength")) return Set(0.0f, H);
            if (FieldName == TEXT("PinchLength")) return Set(0.0f, H * 2.0f);
            return false;
        }

        switch (Archetype)
        {
        case ECaveGeneratorType::FlatPlain:
        case ECaveGeneratorType::CrystalChamber:
            if (FieldName == TEXT("FloorRelativeHeight")) return Set(0.0f, 0.95f);
            if (FieldName == TEXT("CeilingRelativeHeight")) return Set(0.05f, 1.0f);
            if (FieldName == TEXT("FloorRoughness")) return Set(0.0f, HQuarter);
            if (FieldName == TEXT("FloorRoughnessFrequency")) return Set(0.001f, 0.15f);
            if (FieldName == TEXT("CeilingRoughness")) return Set(0.0f, HHalf);
            if (FieldName == TEXT("CeilingRoughnessFrequency")) return Set(0.001f, 0.15f);
            if (FieldName == TEXT("ColumnDensity")) return Set(0.0f, 1.0f);
            if (FieldName == TEXT("ColumnMinRadius")) return Set(1.0f, H * 0.20f);
            if (FieldName == TEXT("ColumnMaxRadius")) return Set(1.0f, H * 0.40f);
            if (FieldName == TEXT("ColumnSpacing")) return Set(10.0f, H * 1.5f);
            if (FieldName == TEXT("BoundarySealThickness")) return Set(0.0f, HQuarter);
            if (FieldName == TEXT("BaseDensity")) return Set(1.0f, 32.0f);
            return false;

        case ECaveGeneratorType::Maze:
            if (FieldName == TEXT("CellSize")) return Set(8.0f, H * 2.0f);
            if (FieldName == TEXT("CorridorRadius")) return Set(1.0f, HQuarter);
            if (FieldName == TEXT("BranchProbability")) return Set(0.05f, 1.0f);
            if (FieldName == TEXT("Verticality")) return Set(0.0f, 1.0f);
            if (FieldName == TEXT("SurfaceRoughness")) return Set(0.0f, HQuarter);
            if (FieldName == TEXT("BoundarySealThickness")) return Set(0.0f, HQuarter);
            if (FieldName == TEXT("BaseDensity")) return Set(1.0f, 32.0f);
            return false;

        case ECaveGeneratorType::SurfaceWorld:
            if (FieldName == TEXT("BaseGroundRelative")) return Set(0.05f, 0.90f);
            if (FieldName == TEXT("ElevationRange")) return Set(0.0f, H * 2.0f);
            if (FieldName == TEXT("ContinentFrequency")) return Set(0.0005f, 0.10f);
            if (FieldName == TEXT("MountainStrength")) return Set(0.0f, 1.0f);
            if (FieldName == TEXT("MountainFrequency")) return Set(0.0005f, 0.10f);
            if (FieldName == TEXT("DetailFrequency")) return Set(0.0005f, 0.20f);
            if (FieldName == TEXT("SurfaceRoughness")) return Set(0.0f, HQuarter);
            if (FieldName == TEXT("HeightWarpStrength")) return Set(0.0f, H);
            if (FieldName == TEXT("HeightWarpFrequency")) return Set(0.0005f, 0.10f);
            if (FieldName == TEXT("ReliefFrequency")) return Set(0.0002f, 0.02f);
            if (FieldName == TEXT("ReliefStrength")) return Set(0.0f, 1.0f);
            if (FieldName == TEXT("ReliefContrast")) return Set(0.25f, 4.0f);
            if (FieldName == TEXT("TerraceStrength")) return Set(0.0f, 1.0f);
            if (FieldName == TEXT("TerraceHeight")) return Set(1.0f, HHalf);
            if (FieldName == TEXT("TerraceHardness")) return Set(0.0f, 1.0f);
            if (FieldName == TEXT("LayerLineDepth")) return Set(0.0f, HQuarter);
            if (FieldName == TEXT("LayerLineSpacing")) return Set(1.0f, H);
            if (FieldName == TEXT("CliffStrength")) return Set(0.0f, 1.0f);
            if (FieldName == TEXT("CliffSlopeThreshold")) return Set(0.05f, 2.0f);
            if (FieldName == TEXT("CliffSharpness")) return Set(0.0f, 4.0f);
            if (FieldName == TEXT("CliffSampleDist")) return Set(0.5f, HQuarter);
            if (FieldName == TEXT("OverhangStrength")) return Set(0.0f, 1.0f);
            if (FieldName == TEXT("OverhangReach")) return Set(0.0f, HHalf);
            if (FieldName == TEXT("OverhangHeight")) return Set(0.0f, HHalf);
            if (FieldName == TEXT("OverhangFrequency")) return Set(0.0005f, 0.20f);
            if (FieldName == TEXT("OverhangZScale")) return Set(0.0f, 1.0f);
            if (FieldName == TEXT("OverhangSlopeThreshold")) return Set(0.05f, 2.0f);
            if (FieldName == TEXT("WaterLevelRelative")) return Set(0.0f, 1.0f);
            if (FieldName == TEXT("BeachWidth")) return Set(0.0f, HQuarter);
            if (FieldName == TEXT("CeilingRelative")) return Set(0.30f, 1.0f);
            if (FieldName == TEXT("CeilingRoughness")) return Set(0.0f, HHalf);
            if (FieldName == TEXT("CeilingRoughnessFrequency")) return Set(0.0005f, 0.20f);
            if (FieldName == TEXT("CeilingUndulation")) return Set(0.0f, HHalf);
            if (FieldName == TEXT("CeilingUndulationFrequency")) return Set(0.0005f, 0.10f);
            if (FieldName == TEXT("CeilingRidgeStrength")) return Set(0.0f, HHalf);
            if (FieldName == TEXT("CeilingRidgeFrequency")) return Set(0.0005f, 0.20f);
            if (FieldName == TEXT("CeilingWarpStrength")) return Set(0.0f, H);
            if (FieldName == TEXT("CeilingWarpFrequency")) return Set(0.0005f, 0.10f);
            if (FieldName == TEXT("BoundarySealThickness")) return Set(0.0f, HQuarter);
            if (FieldName == TEXT("BaseDensity")) return Set(1.0f, 32.0f);
            return false;

        case ECaveGeneratorType::VerticalShafts:
            if (FieldName == TEXT("ShaftSpacing")) return Set(10.0f, H * 2.0f);
            if (FieldName == TEXT("ShaftDensity")) return Set(0.0f, 1.0f);
            if (FieldName == TEXT("ShaftMinRadius")) return Set(1.0f, H * 0.20f);
            if (FieldName == TEXT("ShaftMaxRadius")) return Set(1.0f, H * 0.40f);
            if (FieldName == TEXT("CrossConnectChance")) return Set(0.0f, 1.0f);
            if (FieldName == TEXT("ConnectorRadius")) return Set(1.0f, H * 0.20f);
            if (FieldName == TEXT("LedgeSpacing")) return Set(0.0f, H);
            if (FieldName == TEXT("LedgeDepth")) return Set(0.0f, HQuarter);
            if (FieldName == TEXT("SurfaceRoughness")) return Set(0.0f, HQuarter);
            if (FieldName == TEXT("BoundarySealThickness")) return Set(0.0f, HQuarter);
            if (FieldName == TEXT("BaseDensity")) return Set(1.0f, 32.0f);
            return false;

        case ECaveGeneratorType::FloatingIslands:
            if (FieldName == TEXT("IslandSpacing")) return Set(20.0f, H * 3.0f);
            if (FieldName == TEXT("IslandDensity")) return Set(0.0f, 1.0f);
            if (FieldName == TEXT("IslandMinRadius")) return Set(2.0f, H * 0.30f);
            if (FieldName == TEXT("IslandMaxRadius")) return Set(2.0f, H * 0.75f);
            if (FieldName == TEXT("ThicknessRatio")) return Set(0.10f, 1.5f);
            if (FieldName == TEXT("VerticalJitter")) return Set(0.0f, 1.0f);
            if (FieldName == TEXT("TopFlatten")) return Set(0.0f, 1.0f);
            if (FieldName == TEXT("SurfaceRoughness")) return Set(0.0f, HQuarter);
            if (FieldName == TEXT("SDFBlendRadius")) return Set(0.0f, HQuarter);
            if (FieldName == TEXT("BoundarySealThickness")) return Set(0.0f, HQuarter);
            if (FieldName == TEXT("BaseDensity")) return Set(1.0f, 32.0f);
            return false;

        default:
            return false;
        }
    }

    uint32 VF_CorpusFreeRollSeed(ECaveGeneratorType Archetype, int32 Seed, int32 Index)
    {
        uint32 Value = 0xC0A5FEE1u ^ (static_cast<uint32>(static_cast<uint8>(Archetype)) * 0x9E3779B9u);
        Value = VF_Avalanche(Value ^ static_cast<uint32>(Seed));
        return VF_Avalanche(Value ^ (static_cast<uint32>(Index) + 0x85EBCA6Bu));
    }

    float VF_RollCorpusFreeFloat(FRandomStream& Rng, const FCorpusFreeRange& Range)
    {
        return FMath::Lerp(Range.Min, Range.Max, Rng.FRand());
    }

    bool VF_IsConstraintDerivedTunnelField(const TCHAR* FieldName)
    {
        return FCString::Strcmp(FieldName, TEXT("BoundarySealThickness")) == 0
            || FCString::Strcmp(FieldName, TEXT("WormStrength")) == 0
            || FCString::Strcmp(FieldName, TEXT("WormNetworkRange")) == 0
            || FCString::Strcmp(FieldName, TEXT("MinRoomRadius")) == 0
            || FCString::Strcmp(FieldName, TEXT("MaxRoomRadius")) == 0
            || FCString::Strcmp(FieldName, TEXT("RoomFloorCutMax")) == 0
            || FCString::Strcmp(FieldName, TEXT("OriginRoomRadius")) == 0
            || FCString::Strcmp(FieldName, TEXT("TunnelMinRadius")) == 0
            || FCString::Strcmp(FieldName, TEXT("TunnelMaxRadius")) == 0
            || FCString::Strcmp(FieldName, TEXT("MaxTunnelLength")) == 0
            || FCString::Strcmp(FieldName, TEXT("TunnelWarpStrength")) == 0
            || FCString::Strcmp(FieldName, TEXT("SDFBlendRadius")) == 0
            || FCString::Strcmp(FieldName, TEXT("CaveWarpStrength")) == 0
            || FCString::Strcmp(FieldName, TEXT("SurfaceRoughness")) == 0
            || FCString::Strcmp(FieldName, TEXT("FloorBias")) == 0
            || FCString::Strcmp(FieldName, TEXT("TerraceStepHeight")) == 0
            || FCString::Strcmp(FieldName, TEXT("LayerLineSpacing")) == 0
            || FCString::Strcmp(FieldName, TEXT("LayerLineDepth")) == 0
            || FCString::Strcmp(FieldName, TEXT("OverhangDepth")) == 0
            || FCString::Strcmp(FieldName, TEXT("ArchMinRadius")) == 0
            || FCString::Strcmp(FieldName, TEXT("ArchMaxRadius")) == 0
            || FCString::Strcmp(FieldName, TEXT("DomeMinRadius")) == 0
            || FCString::Strcmp(FieldName, TEXT("DomeMaxRadius")) == 0
            || FCString::Strcmp(FieldName, TEXT("DomeHeightRatio")) == 0
            || FCString::Strcmp(FieldName, TEXT("PinchLength")) == 0;
    }

    bool VF_SampleTunnelContinuousField(FStrateGenerationParams& Params,
                                        const TCHAR* FieldName,
                                        FRandomStream& ScalarRng,
                                        float StrateHeightInVoxels,
                                        bool bConstraintSampled)
    {
        const FString Name(FieldName);
        if (VF_IsExcludedForArchetype(ECaveGeneratorType::TunnelNetwork, Name))
        {
            return true;
        }
        if (bConstraintSampled && VF_IsConstraintDerivedTunnelField(FieldName))
        {
            return true;
        }

        FCorpusFreeRange Range;
        if (!VF_GetCorpusFreeRange(ECaveGeneratorType::TunnelNetwork, Name,
                                   StrateHeightInVoxels, Range))
        {
            return false;
        }
        return VF_WriteFStrateField(Params, Name, VF_RollCorpusFreeFloat(ScalarRng, Range));
    }

    bool VF_SampleTunnelDiscreteField(FStrateGenerationParams& Params,
                                      const TCHAR* FieldName,
                                      FRandomStream& CategoryRng)
    {
        if (FCString::Strcmp(FieldName, TEXT("OriginRoomMaxConnections")) == 0)
        {
            Params.OriginRoomMaxConnections = CategoryRng.RandRange(0, 12);
            return true;
        }
        if (FCString::Strcmp(FieldName, TEXT("bTunnelsFlowTowardOrigin")) == 0)
        {
            Params.bTunnelsFlowTowardOrigin = CategoryRng.FRand() < 0.5f;
            return true;
        }
        if (FCString::Strcmp(FieldName, TEXT("RoughnessNoiseType")) == 0)
        {
            Params.RoughnessNoiseType = static_cast<EVoxelNoiseType>(CategoryRng.RandRange(0, 3));
            return true;
        }
        return false;
    }

    bool VF_SampleTunnelFields(FStrateGenerationParams& Params,
                               FRandomStream& ScalarRng,
                               FRandomStream& CategoryRng,
                               float StrateHeightInVoxels,
                               bool bConstraintSampled)
    {
        bool bValid = true;
#define VF_SAMPLE_CORPUS_FREE_LERP(Name) \
        { \
            const bool bFieldValid = VF_SampleTunnelContinuousField( \
                Params, TEXT(#Name), ScalarRng, StrateHeightInVoxels, bConstraintSampled); \
            bValid = bFieldValid && bValid; \
        }
#define VF_SAMPLE_CORPUS_FREE_SNAP(Name) \
        { \
            const bool bFieldValid = VF_SampleTunnelDiscreteField(Params, TEXT(#Name), CategoryRng); \
            bValid = bFieldValid && bValid; \
        }
        VF_STRATE_PARAM_FIELDS(VF_SAMPLE_CORPUS_FREE_LERP, VF_SAMPLE_CORPUS_FREE_SNAP)
#undef VF_SAMPLE_CORPUS_FREE_LERP
#undef VF_SAMPLE_CORPUS_FREE_SNAP
        return bValid;
    }

    bool VF_SampleReflectedFields(ECaveGeneratorType Archetype,
                                  void* Memory,
                                  FRandomStream& ScalarRng,
                                  FRandomStream& CategoryRng,
                                  float StrateHeightInVoxels)
    {
        UStruct* Struct = VF_GetParamStruct(Archetype);
        if (Struct == nullptr || Memory == nullptr)
        {
            return false;
        }

        TMap<FString, FProperty*> Properties;
        VF_CollectProperties(Struct, Properties);
        TArray<FString> FieldNames;
        FieldNames.Reserve(Properties.Num());
        for (const TPair<FString, FProperty*>& Pair : Properties)
        {
            FieldNames.Add(Pair.Key);
        }
        FieldNames.Sort([](const FString& A, const FString& B) { return A < B; });

        bool bValid = true;
        for (const FString& FieldName : FieldNames)
        {
            FProperty* const* PropertyPtr = Properties.Find(FieldName);
            FProperty* Property = PropertyPtr != nullptr ? *PropertyPtr : nullptr;
            if (Property == nullptr || VF_IsRuntimeField(FieldName)
                || VF_IsExcludedForArchetype(Archetype, FieldName))
            {
                continue;
            }
            if (!VF_IsScalarProperty(Property))
            {
                bValid = false;
                continue;
            }

            if (VF_PropertyKind(Property) == EVoxelStrateFieldKind::Boolean)
            {
                bValid = VF_WritePropertyValue(Property, Memory,
                    CategoryRng.FRand() < 0.5f ? 0.0 : 1.0) && bValid;
                continue;
            }
            if (VF_PropertyKind(Property) == EVoxelStrateFieldKind::Enum)
            {
                // No current non-tunnel family has an enum. Keep the failure explicit if one is
                // added without a legal-value declaration instead of silently writing a default.
                bValid = false;
                continue;
            }

            FCorpusFreeRange Range;
            if (!VF_GetCorpusFreeRange(Archetype, FieldName,
                                       StrateHeightInVoxels, Range))
            {
                bValid = false;
                continue;
            }
            bValid = VF_WritePropertyValue(Property, Memory,
                VF_RollCorpusFreeFloat(ScalarRng, Range)) && bValid;
        }
        return bValid;
    }

    float VF_CorpusFreeU(FRandomStream& Rng, float Min, float Max)
    {
        return FMath::Lerp(Min, Max, Rng.FRand());
    }

    void VF_ClampCorpusFreeField(void* Memory, ECaveGeneratorType Archetype,
                                 const TCHAR* FieldName, float StrateHeightInVoxels)
    {
        if (Memory == nullptr || VF_IsRuntimeField(FieldName)
            || VF_IsExcludedForArchetype(Archetype, FString(FieldName)))
        {
            return;
        }
        FCorpusFreeRange Range;
        if (!VF_GetCorpusFreeRange(Archetype, FString(FieldName), StrateHeightInVoxels, Range))
        {
            return;
        }
        double Value = 0.0;
        if (VF_ReadNamedField(Memory, Archetype, FString(FieldName), Value))
        {
            Value = FMath::Clamp(Value, static_cast<double>(Range.Min), static_cast<double>(Range.Max));
            VF_WriteNamedField(Memory, Archetype, FString(FieldName), Value);
        }
    }

    void VF_ClampCorpusFreeFields(FVoxelStrateArchetypeParams& Params,
                                  ECaveGeneratorType Archetype,
                                  float StrateHeightInVoxels)
    {
        void* Memory = VF_GetParamMemory(Params, Archetype);
        if (VF_IsTunnelArchetype(Archetype))
        {
#define VF_CLAMP_CORPUS_FREE_LERP(Name) \
            VF_ClampCorpusFreeField(Memory, Archetype, TEXT(#Name), StrateHeightInVoxels);
#define VF_CLAMP_CORPUS_FREE_SNAP(Name) \
            do { } while (false);
            VF_STRATE_PARAM_FIELDS(VF_CLAMP_CORPUS_FREE_LERP, VF_CLAMP_CORPUS_FREE_SNAP)
#undef VF_CLAMP_CORPUS_FREE_LERP
#undef VF_CLAMP_CORPUS_FREE_SNAP
            return;
        }

        UStruct* Struct = VF_GetParamStruct(Archetype);
        if (Memory == nullptr || Struct == nullptr)
        {
            return;
        }
        TMap<FString, FProperty*> Properties;
        VF_CollectProperties(Struct, Properties);
        for (const TPair<FString, FProperty*>& Pair : Properties)
        {
            VF_ClampCorpusFreeField(Memory, Archetype, *Pair.Key, StrateHeightInVoxels);
        }
    }

    bool VF_FailCorpusFreeConstraint(FString& OutViolation, const TCHAR* Relation,
                                     float Left, float Right)
    {
        OutViolation = FString::Printf(TEXT("%s (%.6g vs %.6g)"), Relation, Left, Right);
        return false;
    }

    bool VF_ApplyConstraintTunnel(FStrateGenerationParams& P,
                                  FRandomStream& Rng,
                                  float StrateHeightInVoxels)
    {
        const float H = FMath::Max(StrateHeightInVoxels, static_cast<float>(CHUNK_SIZE));
        const float SpacingMin = FMath::Min(48.0f, H * 0.35f);
        const float SpacingMax = FMath::Max(SpacingMin + 8.0f, FMath::Min(160.0f, H * 0.94f));
        const float Coverage = VF_CorpusFreeU(Rng, 0.08f, 0.18f);

        P.BoundarySealThickness = VF_CorpusFreeU(
            Rng, FMath::Min(3.0f, H * 0.02f), FMath::Min(8.0f, H * 0.08f));
        P.RoomSpacing = VF_CorpusFreeU(Rng, SpacingMin, SpacingMax);
        P.RoomDensity = VF_CorpusFreeU(Rng, 0.25f, 0.75f);
        const float RadiusRatio = FMath::Sqrt(
            Coverage / (PI * FMath::Max(P.RoomDensity, 0.001f)));
        P.MaxRoomRadius = P.RoomSpacing * RadiusRatio;
        P.MinRoomRadius = P.MaxRoomRadius * VF_CorpusFreeU(Rng, 0.35f, 0.75f);
        P.RoomHeightRatio = VF_CorpusFreeU(Rng, 0.25f, 0.85f);
        P.OriginRoomRadius = P.MaxRoomRadius * VF_CorpusFreeU(Rng, 0.60f, 0.95f);

        P.RoomFloorCutMin = VF_CorpusFreeU(Rng, 0.35f, 0.85f);
        P.RoomFloorCutMax = P.RoomFloorCutMin + VF_CorpusFreeU(
            Rng, 0.0f, 1.0f - P.RoomFloorCutMin);
        P.FloorReliefStrength = VF_CorpusFreeU(
            Rng, 0.0f,
            FMath::Min(12.0f, P.MaxRoomRadius * P.RoomHeightRatio * 0.25f));

        const float TunnelMinCap = FMath::Max(1.5f, P.MinRoomRadius * 0.45f);
        P.TunnelMinRadius = FMath::Max(
            1.5f, P.MinRoomRadius * VF_CorpusFreeU(Rng, 0.25f, 0.45f));
        P.TunnelMinRadius = FMath::Min(P.TunnelMinRadius, TunnelMinCap);
        P.TunnelMaxRadius = FMath::Max(
            P.TunnelMinRadius,
            P.MinRoomRadius * VF_CorpusFreeU(Rng, 0.55f, 0.90f));

        // Adjacent cells can differ in both X and Y. The placement jitter is [0.15, 0.85]
        // cell, so the worst local diagonal separation is 1.7 * S * sqrt(2), not the
        // axis-aligned 1.7 * S case.
        const float HorizontalReach = 1.7f * P.RoomSpacing * FMath::Sqrt(2.0f);
        const float VerticalReach = 2.0f * P.MaxRoomRadius * P.RoomHeightRatio;
        const float NeighborReach = FMath::Sqrt(
            HorizontalReach * HorizontalReach + VerticalReach * VerticalReach);
        P.MaxTunnelLength = NeighborReach * VF_CorpusFreeU(Rng, 1.0f, 1.40f);
        P.TunnelWarpStrength = P.MaxTunnelLength * VF_CorpusFreeU(Rng, 0.02f, 0.12f);
        P.WormStrength = P.BaseDensity * VF_CorpusFreeU(Rng, 1.10f, 1.80f);
        P.WormNetworkRange = P.RoomSpacing * VF_CorpusFreeU(Rng, 0.20f, 0.60f);
        P.CaveWarpStrength = P.RoomSpacing * VF_CorpusFreeU(Rng, 0.02f, 0.12f);

        P.SDFBlendRadius = VF_CorpusFreeU(
            Rng, 0.5f, FMath::Max(0.5f, FMath::Min(4.0f, P.TunnelMinRadius * 0.35f)));
        const float RoughnessMax = FMath::Max(
            0.0f, (P.TunnelMinRadius - P.SDFBlendRadius - 0.25f) / VOXEL_NOISE_SCALE);
        P.SurfaceRoughness = VF_CorpusFreeU(Rng, 0.0f, RoughnessMax * 0.70f);
        P.FloorBias = VF_CorpusFreeU(
            Rng, 0.0f, P.MaxRoomRadius * P.RoomHeightRatio * 0.25f);

        // Internal terrain-operation transport is kept within the same room/strate envelope.
        P.TerraceStepHeight = VF_CorpusFreeU(
            Rng, 0.0f, FMath::Min(16.0f, P.MaxRoomRadius * P.RoomHeightRatio * 0.35f));
        P.LayerLineSpacing = VF_CorpusFreeU(
            Rng, 4.0f, FMath::Max(4.0f, FMath::Min(H * 0.25f, P.MaxRoomRadius)));
        P.LayerLineDepth = VF_CorpusFreeU(Rng, 0.0f, P.BaseDensity * 0.10f);
        P.OverhangDepth = VF_CorpusFreeU(
            Rng, 0.0f, FMath::Min(P.MaxRoomRadius * 0.25f, H * 0.20f));
        P.ArchMinRadius = VF_CorpusFreeU(
            Rng, 1.0f, FMath::Max(1.0f, P.TunnelMinRadius));
        P.ArchMaxRadius = FMath::Max(
            P.ArchMinRadius,
            P.MinRoomRadius * VF_CorpusFreeU(Rng, 0.20f, 0.45f));
        P.DomeMaxRadius = P.MaxRoomRadius * VF_CorpusFreeU(Rng, 0.30f, 0.60f);
        P.DomeMinRadius = P.DomeMaxRadius * VF_CorpusFreeU(Rng, 0.35f, 0.75f);
        P.DomeHeightRatio = FMath::Min(
            1.0f,
            (P.MaxRoomRadius * P.RoomHeightRatio)
                / FMath::Max(P.DomeMaxRadius, 0.001f)
                * VF_CorpusFreeU(Rng, 0.35f, 0.90f));
        P.PinchLength = P.MaxTunnelLength * VF_CorpusFreeU(Rng, 0.10f, 0.40f);
        return true;
    }

    bool VF_ApplyConstraintSlab(FSlabGenerationParams& P,
                                FRandomStream& Rng,
                                float StrateHeightInVoxels)
    {
        const float H = FMath::Max(StrateHeightInVoxels, static_cast<float>(CHUNK_SIZE));
        P.BoundarySealThickness = VF_CorpusFreeU(
            Rng, FMath::Min(3.0f, H * 0.02f), FMath::Min(8.0f, H * 0.08f));
        P.FloorRoughness = VF_CorpusFreeU(Rng, 0.0f, H * 0.04f);
        P.CeilingRoughness = VF_CorpusFreeU(Rng, 0.0f, H * 0.08f);

        const float FloorNoise = VOXEL_NOISE_SCALE * P.FloorRoughness;
        const float CeilingNoise = VOXEL_NOISE_SCALE * P.CeilingRoughness;
        const float FloorClearance = VF_CorpusFreeU(Rng, H * 0.08f, H * 0.16f);
        const float CeilingClearance = VF_CorpusFreeU(Rng, H * 0.08f, H * 0.16f);
        P.FloorRelativeHeight = (P.BoundarySealThickness + FloorNoise + FloorClearance) / H;
        P.CeilingRelativeHeight = 1.0f
            - (P.BoundarySealThickness + CeilingNoise + CeilingClearance) / H;

        P.ColumnSpacing = VF_CorpusFreeU(
            Rng, FMath::Max(24.0f, H * 0.30f), FMath::Max(32.0f, H * 0.75f));
        P.ColumnMinRadius = VF_CorpusFreeU(Rng, 1.0f, P.ColumnSpacing * 0.08f);
        P.ColumnMaxRadius = FMath::Max(
            P.ColumnMinRadius,
            FMath::Min(P.ColumnSpacing * 0.40f, H * 0.38f));
        return true;
    }

    bool VF_ApplyConstraintMaze(FMazeGenerationParams& P,
                                FRandomStream& Rng,
                                float StrateHeightInVoxels)
    {
        const float H = FMath::Max(StrateHeightInVoxels, static_cast<float>(CHUNK_SIZE));
        P.CellSize = VF_CorpusFreeU(
            Rng, FMath::Max(24.0f, H * 0.25f), FMath::Min(96.0f, H * 0.75f));
        P.CorridorRadius = P.CellSize * VF_CorpusFreeU(Rng, 0.12f, 0.24f);
        const float RoughnessCap = FMath::Max(
            0.0f, (P.CorridorRadius + 2.0f) / (VOXEL_NOISE_SCALE * 1.25f));
        P.SurfaceRoughness = VF_CorpusFreeU(Rng, 0.0f, RoughnessCap * 0.70f);
        P.BoundarySealThickness = VF_CorpusFreeU(
            Rng, FMath::Min(3.0f, H * 0.02f), FMath::Min(8.0f, H * 0.08f));
        return true;
    }

    bool VF_ApplyConstraintSurface(FSurfaceGenerationParams& P,
                                   FRandomStream& Rng,
                                   float StrateHeightInVoxels)
    {
        const float H = FMath::Max(StrateHeightInVoxels, static_cast<float>(CHUNK_SIZE));
        P.BoundarySealThickness = VF_CorpusFreeU(
            Rng, FMath::Min(3.0f, H * 0.02f), FMath::Min(8.0f, H * 0.08f));
        P.ElevationRange = VF_CorpusFreeU(Rng, H * 0.06f, H * 0.16f);
        P.MountainStrength = VF_CorpusFreeU(Rng, 0.20f, 0.80f);
        P.SurfaceRoughness = VF_CorpusFreeU(Rng, 0.0f, H * 0.02f);

        const float GroundLowerDeviation = P.ElevationRange * 0.50f
            + P.SurfaceRoughness * VOXEL_NOISE_SCALE;
        const float GroundUpperDeviation = P.ElevationRange * (0.50f + P.MountainStrength)
            + P.SurfaceRoughness * VOXEL_NOISE_SCALE;
        const float GroundClearance = VF_CorpusFreeU(Rng, H * 0.04f, H * 0.08f);
        P.CeilingRoughness = VF_CorpusFreeU(Rng, 0.0f, H * 0.02f);
        P.CeilingUndulation = VF_CorpusFreeU(Rng, 0.0f, H * 0.02f);
        P.CeilingRidgeStrength = VF_CorpusFreeU(Rng, 0.0f, H * 0.02f);

        P.TerraceStrength = VF_CorpusFreeU(Rng, 0.0f, 0.50f);
        P.TerraceHeight = VF_CorpusFreeU(
            Rng, 1.0f, FMath::Max(1.0f, FMath::Min(10.0f, H * 0.08f)));
        P.LayerLineDepth = VF_CorpusFreeU(Rng, 0.0f, FMath::Min(2.0f, H * 0.02f));
        P.LayerLineSpacing = VF_CorpusFreeU(
            Rng, FMath::Min(4.0f, H * 0.01f), FMath::Min(32.0f, H * 0.25f));
        P.OverhangStrength = VF_CorpusFreeU(Rng, 0.0f, 0.50f);
        P.OverhangReach = VF_CorpusFreeU(
            Rng, FMath::Min(4.0f, H * 0.01f), FMath::Min(20.0f, H * 0.15f));
        P.OverhangHeight = VF_CorpusFreeU(
            Rng, FMath::Min(2.0f, H * 0.02f), FMath::Min(10.0f, H * 0.05f));
        P.BeachWidth = VF_CorpusFreeU(Rng, 0.0f, FMath::Min(16.0f, H * 0.10f));

        const float CapClearance = VF_CorpusFreeU(Rng, H * 0.04f, H * 0.08f);
        const float CapLowering = P.CeilingRoughness * VOXEL_NOISE_SCALE
            + P.CeilingUndulation * VOXEL_NOISE_SCALE + P.CeilingRidgeStrength;
        // Cliff is a height-space operation, not a categorical switch. Its equation pushes the
        // structural height away from the four-sample local mean by
        // CliffStrength * CliffSharpness. The structural field's total possible spread is
        // bounded by ElevationRange * (1 + MountainStrength) + 2 * noiseScale * roughness.
        // Reserve room for that bound, then reduce the requested strength if the remaining
        // bottom/cap budget cannot contain it. This is a derived relation, not the final envelope
        // clamp below; the other height operations receive the same budget treatment.
        const float CliffSpread = P.ElevationRange * (1.0f + P.MountainStrength)
            + 2.0f * VOXEL_NOISE_SCALE * P.SurfaceRoughness;
        const float CliffCoefficient = FMath::Max(P.CliffSharpness, 0.0f) * CliffSpread;
        const float OtherGroundFeatureBudget = FMath::Max(P.TerraceHeight, 0.0f)
            + FMath::Abs(P.LayerLineDepth) + FMath::Max(P.BeachWidth, 0.0f);
        const float RequiredGap = CapClearance + FMath::Max(P.OverhangHeight, 0.0f)
            + 2.0f;
        const float GroundWithoutFeatures = P.BoundarySealThickness + GroundClearance
            + GroundLowerDeviation + GroundUpperDeviation;
        constexpr float FitMargin = 1.0f;
        const float FeatureCapacityFromBase = H * 0.90f
            - (P.BoundarySealThickness + GroundClearance + GroundLowerDeviation)
            - OtherGroundFeatureBudget - FitMargin;
        const float FeatureCapacityFromCap = 0.5f * (
            H - P.BoundarySealThickness - RequiredGap - GroundWithoutFeatures - FitMargin)
            - OtherGroundFeatureBudget;
        const float MaxCliffBudget = FMath::Max(0.0f,
            FMath::Min(FeatureCapacityFromBase, FeatureCapacityFromCap));
        if (CliffCoefficient > 0.0f)
        {
            P.CliffStrength = FMath::Min(
                FMath::Max(P.CliffStrength, 0.0f), MaxCliffBudget / CliffCoefficient);
        }
        else
        {
            P.CliffStrength = 0.0f;
        }

        const float CliffBudget = FMath::Max(P.CliffStrength, 0.0f) * CliffCoefficient;
        const float GroundFeatureBudget = OtherGroundFeatureBudget + CliffBudget;
        P.BaseGroundRelative = (P.BoundarySealThickness + GroundClearance
                                 + GroundLowerDeviation + GroundFeatureBudget) / H;
        const float GroundUpper = P.BoundarySealThickness + GroundClearance
            + GroundLowerDeviation + GroundUpperDeviation + 2.0f * GroundFeatureBudget;
        // Compute the cap BASE from the worst terrain height. The generator subtracts
        // CapLowering afterwards (VoxelGenerator.cpp:2627-2652), so adding it here makes the
        // final cap, rather than the unlowered base line, the dependent quantity.
        P.CeilingRelative = (GroundUpper + RequiredGap + CapLowering) / H;

        return true;
    }

    bool VF_ApplyConstraintVertical(FVerticalShaftParams& P,
                                    FRandomStream& Rng,
                                    float StrateHeightInVoxels)
    {
        const float H = FMath::Max(StrateHeightInVoxels, static_cast<float>(CHUNK_SIZE));
        P.BoundarySealThickness = VF_CorpusFreeU(
            Rng, FMath::Min(3.0f, H * 0.02f), FMath::Min(8.0f, H * 0.08f));
        P.ShaftSpacing = VF_CorpusFreeU(
            Rng, FMath::Max(32.0f, H * 0.30f),
            FMath::Max(32.0f, FMath::Min(112.0f, H * 0.90f)));
        P.SurfaceRoughness = VF_CorpusFreeU(Rng, 0.0f, H * 0.04f);
        const float RoughnessReach = P.SurfaceRoughness * VOXEL_NOISE_SCALE * 1.5f;
        P.ShaftMinRadius = RoughnessReach + 0.5f + VF_CorpusFreeU(Rng, 1.0f, 4.0f);
        P.ShaftMaxRadius = FMath::Max(
            P.ShaftMinRadius + 0.5f,
            FMath::Min(P.ShaftSpacing * 0.40f, P.ShaftMinRadius + 16.0f));
        P.ConnectorRadius = RoughnessReach + 1.0f + VF_CorpusFreeU(Rng, 0.0f, 3.0f);
        P.LedgeDepth = VF_CorpusFreeU(
            Rng, 0.5f, FMath::Max(0.5f, FMath::Min(4.0f, P.ShaftSpacing * 0.08f)));
        const float FloorClearance = FMath::Max(
            1.0f, P.SurfaceRoughness * VOXEL_NOISE_SCALE + 1.0f);
        P.LedgeSpacing = 2.0f * (P.LedgeDepth + FloorClearance)
            + VF_CorpusFreeU(
                Rng, FMath::Min(4.0f, H * 0.02f), FMath::Min(16.0f, H * 0.10f));
        return true;
    }

    bool VF_ApplyConstraintFloating(FFloatingIslandParams& P,
                                    FRandomStream& Rng,
                                    float StrateHeightInVoxels)
    {
        const float H = FMath::Max(StrateHeightInVoxels, static_cast<float>(CHUNK_SIZE));
        P.BoundarySealThickness = VF_CorpusFreeU(
            Rng, FMath::Min(3.0f, H * 0.02f), FMath::Min(8.0f, H * 0.08f));
        P.IslandSpacing = VF_CorpusFreeU(
            Rng, FMath::Max(40.0f, H * 0.30f),
            FMath::Max(40.0f, FMath::Min(120.0f, H * 0.75f)));
        P.ThicknessRatio = VF_CorpusFreeU(Rng, 0.30f, 0.90f);
        const float MaxByHeight = (H * 0.50f - P.BoundarySealThickness)
            / FMath::Max(P.ThicknessRatio, 0.25f);
        const float MaxRadius = FMath::Max(
            2.0f, FMath::Min(P.IslandSpacing * 0.45f, MaxByHeight * 0.85f));
        P.IslandMaxRadius = MaxRadius;
        P.IslandMinRadius = P.IslandMaxRadius * VF_CorpusFreeU(Rng, 0.45f, 0.80f);
        P.SDFBlendRadius = VF_CorpusFreeU(
            Rng, 0.5f, FMath::Min(6.0f, P.IslandMinRadius * 0.20f));
        const float RoughnessMax = FMath::Max(
            0.0f, (P.IslandMinRadius - P.SDFBlendRadius) / VOXEL_NOISE_SCALE);
        P.SurfaceRoughness = VF_CorpusFreeU(Rng, 0.0f, RoughnessMax * 0.70f);
        return true;
    }

    bool VF_ApplyCorpusFreeConstraints(ECaveGeneratorType Archetype,
                                       FVoxelStrateArchetypeParams& Params,
                                       FRandomStream& Rng,
                                       float StrateHeightInVoxels)
    {
        switch (Archetype)
        {
        case ECaveGeneratorType::TunnelNetwork:
        case ECaveGeneratorType::Underwater:
            return VF_ApplyConstraintTunnel(Params.TunnelNetworkParams, Rng,
                                            StrateHeightInVoxels);
        case ECaveGeneratorType::FlatPlain:
        case ECaveGeneratorType::CrystalChamber:
            return VF_ApplyConstraintSlab(Params.SlabParams, Rng,
                                          StrateHeightInVoxels);
        case ECaveGeneratorType::Maze:
            return VF_ApplyConstraintMaze(Params.MazeParams, Rng,
                                          StrateHeightInVoxels);
        case ECaveGeneratorType::SurfaceWorld:
            return VF_ApplyConstraintSurface(Params.SurfaceParams, Rng,
                                             StrateHeightInVoxels);
        case ECaveGeneratorType::VerticalShafts:
            return VF_ApplyConstraintVertical(Params.VerticalShaftParams, Rng,
                                               StrateHeightInVoxels);
        case ECaveGeneratorType::FloatingIslands:
            return VF_ApplyConstraintFloating(Params.FloatingIslandParams, Rng,
                                               StrateHeightInVoxels);
        default:
            return false;
        }
    }

    FVoxelStrateRollInfo VF_RollCorpusFreeInternal(
        ECaveGeneratorType Archetype, int32 Seed, int32 Index,
        EVoxelStrateCorpusFreeSamplingMode Mode, float StrateHeightInVoxels)
    {
        FVoxelStrateRollInfo Result;
        Result.Archetype = Archetype;
        if (!VF_IsSupportedArchetype(Archetype))
        {
            Result.FailureReason = TEXT("The requested archetype is not supported.");
            return Result;
        }
        if (!FMath::IsFinite(StrateHeightInVoxels) || StrateHeightInVoxels <= 0.0f)
        {
            Result.FailureReason = TEXT("StrateHeightInVoxels must be finite and positive.");
            return Result;
        }

        const uint32 BaseSeed = VF_CorpusFreeRollSeed(Archetype, Seed, Index);
        FRandomStream ScalarRng(static_cast<int32>(VF_Avalanche(BaseSeed ^ 0x51A1A1A1u)));
        // Keep categorical choices on a mode-independent stream. The comparison then changes
        // only scalar coupling, never the bool/enum draw policy.
        FRandomStream CategoryRng(static_cast<int32>(VF_Avalanche(BaseSeed ^ 0xC47E6070u)));
        FVoxelStrateArchetypeParams Params;
        const bool bConstraintSampled =
            Mode == EVoxelStrateCorpusFreeSamplingMode::ConstraintSampled;

        bool bValid = false;
        if (VF_IsTunnelArchetype(Archetype))
        {
            bValid = VF_SampleTunnelFields(Params.TunnelNetworkParams, ScalarRng,
                                           CategoryRng, StrateHeightInVoxels,
                                           bConstraintSampled);
        }
        else
        {
            bValid = VF_SampleReflectedFields(Archetype, VF_GetParamMemory(Params, Archetype),
                                              ScalarRng, CategoryRng, StrateHeightInVoxels);
        }
        if (bValid && bConstraintSampled)
        {
            bValid = VF_ApplyCorpusFreeConstraints(Archetype, Params, ScalarRng,
                                                   StrateHeightInVoxels);
        }
        if (!bValid)
        {
            Result.FailureReason = TEXT("Corpus-free field schema has no declared roll range.");
            return Result;
        }

        // Last safety net only: all dependent values above are derived before this envelope
        // clamp, and no corpus/default value is consulted.
        VF_ClampCorpusFreeFields(Params, Archetype, StrateHeightInVoxels);
        VF_ResetExcludedFields(Params, Archetype);
        Result.ArchetypeParams = Params;
        Result.Params = Params.TunnelNetworkParams;
        Result.bValid = true;
        return Result;
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

FVoxelStrateMeasuredMetrics VF_SummarizeStrateMetrics(
    const FVoxelStrateMetrics& Metrics)
{
    FVoxelStrateMeasuredMetrics Result;
    Result.bValid = Metrics.bValid;
    Result.NumSampled = Metrics.NumSampled;
    Result.NumAir = Metrics.NumAir;
    Result.NumSolid = Metrics.NumSolid;
    Result.AirFraction = Metrics.AirFraction;
    Result.NumAirComponents = Metrics.NumAirComponents;
    Result.LargestComponentShare = Metrics.LargestComponentShare;
    Result.LargestComponentPoint = Metrics.LargestComponentPoint;
    Result.LargestComponentCells = Metrics.LargestComponentCells;
    Result.NumComponentsAtLeast1Pct = Metrics.NumComponentsAtLeast1Pct;
    Result.WalkableFraction = Metrics.WalkableFraction;
    Result.WalkableFloorColumns = Metrics.WalkableFloorColumns;
    Result.WalkableFloorAreaFraction = Metrics.WalkableFloorAreaFraction;
    Result.NumWalkableSurfaceComponents = Metrics.NumWalkableSurfaceComponents;
    Result.LargestWalkableSurfaceColumns = Metrics.LargestWalkableSurfaceColumns;
    Result.LargestWalkableSurfaceShare = Metrics.LargestWalkableSurfaceShare;
    Result.MedianFeatureScale = Metrics.MedianFeatureScale;
    Result.MedianVerticalClearance = Metrics.MedianVerticalClearance;
    Result.bPlayerFitResolved = Metrics.bPlayerFitResolved;
    Result.PlayerFitRefusalReason = Metrics.PlayerFitRefusalReason;
    Result.NumPlayerFitCells = Metrics.NumPlayerFitCells;
    Result.PlayerFitFraction = Metrics.PlayerFitFraction;
    Result.NumTraversableComponents = Metrics.NumTraversableComponents;
    Result.LargestTraversableComponentCells = Metrics.LargestTraversableComponentCells;
    Result.TraversableComponentShare = Metrics.TraversableComponentShare;
    Result.MinimumPlayerClearanceVoxels = Metrics.MinimumPlayerClearanceVoxels;
    Result.ResolvedMarginVoxels = Metrics.ResolvedMarginVoxels;
    Result.SampledMinZ = Metrics.SampledMinZ;
    Result.SampledMaxZ = Metrics.SampledMaxZ;
    Result.SampledNumX = Metrics.SampledNumX;
    Result.SampledNumY = Metrics.SampledNumY;
    Result.SampledNumZ = Metrics.SampledNumZ;
    Result.SampledMinX = Metrics.SampledMinX;
    Result.SampledMaxX = Metrics.SampledMaxX;
    Result.SampledMinY = Metrics.SampledMinY;
    Result.SampledMaxY = Metrics.SampledMaxY;
    Result.AirComponentCells = Metrics.AirComponentCells;
    return Result;
}

namespace
{
    constexpr int32 GPromotionStoreSchemaVersion = 2;

    bool VF_GetJsonValue(const TSharedPtr<FJsonObject>& Object,
                         const TCHAR* FieldName,
                         EJson ExpectedType,
                         TSharedPtr<FJsonValue>& OutValue)
    {
        OutValue.Reset();
        if (!Object.IsValid())
        {
            return false;
        }

        const TSharedPtr<FJsonValue>* Found = Object->Values.Find(FieldName);
        if (Found == nullptr || !Found->IsValid() || (*Found)->Type != ExpectedType)
        {
            return false;
        }
        OutValue = *Found;
        return true;
    }

    bool VF_ReadJsonNumber(const TSharedPtr<FJsonObject>& Object,
                           const TCHAR* FieldName, double& OutValue)
    {
        TSharedPtr<FJsonValue> Value;
        if (!VF_GetJsonValue(Object, FieldName, EJson::Number, Value))
        {
            return false;
        }
        OutValue = Value->AsNumber();
        return FMath::IsFinite(OutValue);
    }

    bool VF_ReadJsonNumber(const TSharedPtr<FJsonObject>& Object,
                           const FString& FieldName, double& OutValue)
    {
        return VF_ReadJsonNumber(Object, *FieldName, OutValue);
    }

    bool VF_ReadJsonInt64(const TSharedPtr<FJsonObject>& Object,
                          const TCHAR* FieldName, int64& OutValue)
    {
        double Number = 0.0;
        if (!VF_ReadJsonNumber(Object, FieldName, Number)
            || Number < -9223372036854775807.0
            || Number > 9223372036854775807.0)
        {
            return false;
        }
        const int64 Integral = static_cast<int64>(Number);
        if (static_cast<double>(Integral) != Number)
        {
            return false;
        }
        OutValue = Integral;
        return true;
    }

    bool VF_ReadJsonInt32(const TSharedPtr<FJsonObject>& Object,
                          const TCHAR* FieldName, int32& OutValue)
    {
        int64 Value = 0;
        if (!VF_ReadJsonInt64(Object, FieldName, Value)
            || Value < static_cast<int64>(TNumericLimits<int32>::Lowest())
            || Value > static_cast<int64>(TNumericLimits<int32>::Max()))
        {
            return false;
        }
        OutValue = static_cast<int32>(Value);
        return true;
    }

    bool VF_ReadJsonFloat(const TSharedPtr<FJsonObject>& Object,
                          const TCHAR* FieldName, float& OutValue)
    {
        double Value = 0.0;
        if (!VF_ReadJsonNumber(Object, FieldName, Value)
            || Value < -static_cast<double>(TNumericLimits<float>::Max())
            || Value > static_cast<double>(TNumericLimits<float>::Max()))
        {
            return false;
        }
        OutValue = static_cast<float>(Value);
        return FMath::IsFinite(OutValue);
    }

    bool VF_ReadJsonBool(const TSharedPtr<FJsonObject>& Object,
                         const TCHAR* FieldName, bool& OutValue)
    {
        TSharedPtr<FJsonValue> Value;
        if (!VF_GetJsonValue(Object, FieldName, EJson::Boolean, Value))
        {
            return false;
        }
        OutValue = Value->AsBool();
        return true;
    }

    bool VF_ReadJsonString(const TSharedPtr<FJsonObject>& Object,
                           const TCHAR* FieldName, FString& OutValue)
    {
        TSharedPtr<FJsonValue> Value;
        if (!VF_GetJsonValue(Object, FieldName, EJson::String, Value))
        {
            return false;
        }
        OutValue = Value->AsString();
        return true;
    }

    bool VF_ReadJsonObject(const TSharedPtr<FJsonObject>& Object,
                           const TCHAR* FieldName,
                           TSharedPtr<FJsonObject>& OutValue)
    {
        TSharedPtr<FJsonValue> Value;
        if (!VF_GetJsonValue(Object, FieldName, EJson::Object, Value))
        {
            return false;
        }
        OutValue = Value->AsObject();
        return OutValue.IsValid();
    }

    bool VF_ReadJsonArray(const TSharedPtr<FJsonObject>& Object,
                          const TCHAR* FieldName,
                          const TArray<TSharedPtr<FJsonValue>>*& OutValue)
    {
        TSharedPtr<FJsonValue> Value;
        if (!VF_GetJsonValue(Object, FieldName, EJson::Array, Value))
        {
            return false;
        }
        OutValue = &Value->AsArray();
        return true;
    }

    TSharedPtr<FJsonObject> VF_SerializeRecipeEntry(const FVoxelOpRecipeEntry& Entry)
    {
        TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
        Object->SetNumberField(TEXT("op_class"), static_cast<int32>(Entry.OpClass));
        Object->SetNumberField(TEXT("param_block"), static_cast<int32>(Entry.ParamBlock));
        return Object;
    }

    bool VF_DeserializeRecipeEntry(const TSharedPtr<FJsonObject>& Object,
                                   FVoxelOpRecipeEntry& OutEntry)
    {
        int32 OpClass = 0;
        int32 ParamBlock = 0;
        if (!VF_ReadJsonInt32(Object, TEXT("op_class"), OpClass)
            || !VF_ReadJsonInt32(Object, TEXT("param_block"), ParamBlock)
            || OpClass < static_cast<int32>(EVoxelStrateOpClass::ConstantRockSource)
            || OpClass > static_cast<int32>(EVoxelStrateOpClass::DensityNoiseFillMod)
            || ParamBlock < static_cast<int32>(EVoxelStrateParamBlock::None)
            || ParamBlock > static_cast<int32>(EVoxelStrateParamBlock::FloatingIsland))
        {
            return false;
        }
        OutEntry.OpClass = static_cast<EVoxelStrateOpClass>(OpClass);
        OutEntry.ParamBlock = static_cast<EVoxelStrateParamBlock>(ParamBlock);
        return true;
    }

    TSharedPtr<FJsonObject> VF_SerializeRecipe(const FVoxelOpStackRecipe& Recipe)
    {
        TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
        Object->SetNumberField(TEXT("root_polarity"), static_cast<int32>(Recipe.RootPolarity));
        Object->SetObjectField(TEXT("root"), VF_SerializeRecipeEntry(Recipe.Root));
        Object->SetObjectField(TEXT("shape_source"), VF_SerializeRecipeEntry(Recipe.ShapeSource));
        Object->SetObjectField(TEXT("conversion"), VF_SerializeRecipeEntry(Recipe.Conversion));
        Object->SetNumberField(TEXT("structural_param_block"),
                               static_cast<int32>(Recipe.StructuralParamBlock));

        TArray<TSharedPtr<FJsonValue>> Modifiers;
        Modifiers.Reserve(Recipe.Modifiers.Num());
        for (const FVoxelOpRecipeEntry& Modifier : Recipe.Modifiers)
        {
            Modifiers.Add(MakeShared<FJsonValueObject>(VF_SerializeRecipeEntry(Modifier)));
        }
        Object->SetArrayField(TEXT("modifiers"), MoveTemp(Modifiers));
        return Object;
    }

    bool VF_DeserializeRecipe(const TSharedPtr<FJsonObject>& Object,
                              FVoxelOpStackRecipe& OutRecipe)
    {
        int32 RootPolarity = 0;
        int32 StructuralParamBlock = 0;
        TSharedPtr<FJsonObject> Root;
        TSharedPtr<FJsonObject> ShapeSource;
        TSharedPtr<FJsonObject> Conversion;
        const TArray<TSharedPtr<FJsonValue>>* Modifiers = nullptr;
        if (!VF_ReadJsonInt32(Object, TEXT("root_polarity"), RootPolarity)
            || !VF_ReadJsonObject(Object, TEXT("root"), Root)
            || !VF_ReadJsonObject(Object, TEXT("shape_source"), ShapeSource)
            || !VF_ReadJsonObject(Object, TEXT("conversion"), Conversion)
            || !VF_ReadJsonInt32(Object, TEXT("structural_param_block"), StructuralParamBlock)
            || !VF_ReadJsonArray(Object, TEXT("modifiers"), Modifiers)
            || RootPolarity < static_cast<int32>(EVoxelStrateRootPolarity::RockCarve)
            || RootPolarity > static_cast<int32>(EVoxelStrateRootPolarity::VoidFill)
            || StructuralParamBlock < static_cast<int32>(EVoxelStrateParamBlock::None)
            || StructuralParamBlock > static_cast<int32>(EVoxelStrateParamBlock::FloatingIsland)
            || Modifiers->Num() < 4 || Modifiers->Num() > 8
            || !VF_DeserializeRecipeEntry(Root, OutRecipe.Root)
            || !VF_DeserializeRecipeEntry(ShapeSource, OutRecipe.ShapeSource)
            || !VF_DeserializeRecipeEntry(Conversion, OutRecipe.Conversion))
        {
            return false;
        }

        OutRecipe.RootPolarity = static_cast<EVoxelStrateRootPolarity>(RootPolarity);
        OutRecipe.StructuralParamBlock = static_cast<EVoxelStrateParamBlock>(StructuralParamBlock);
        OutRecipe.Modifiers.Reset();
        OutRecipe.Modifiers.Reserve(Modifiers->Num());
        for (const TSharedPtr<FJsonValue>& Value : *Modifiers)
        {
            if (!Value.IsValid() || Value->Type != EJson::Object)
            {
                return false;
            }
            FVoxelOpRecipeEntry& Entry = OutRecipe.Modifiers.AddDefaulted_GetRef();
            if (!VF_DeserializeRecipeEntry(Value->AsObject(), Entry))
            {
                return false;
            }
        }
        return true;
    }

    TSharedPtr<FJsonObject> VF_SerializeArchetypeParams(
        ECaveGeneratorType Archetype,
        const FVoxelStrateArchetypeParams& Params)
    {
        TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
        const void* Memory = VF_GetParamMemory(Params, Archetype);
        UStruct* Struct = VF_GetParamStruct(Archetype);
        if (Memory == nullptr || Struct == nullptr)
        {
            return Object;
        }

        if (VF_IsTunnelArchetype(Archetype))
        {
#define VF_JSON_WRITE_TUNNEL_LERPF(Name) \
            do { \
                const FString FieldName(TEXT(#Name)); \
                if (!VF_IsExcludedForArchetype(Archetype, FieldName)) { \
                    double Value = 0.0; \
                    if (VF_ReadNamedField(Memory, Archetype, FieldName, Value)) \
                    { Object->SetNumberField(FieldName, Value); } \
                } \
            } while (false);
#define VF_JSON_WRITE_TUNNEL_SNAPF(Name) VF_JSON_WRITE_TUNNEL_LERPF(Name)
            VF_STRATE_PARAM_FIELDS(VF_JSON_WRITE_TUNNEL_LERPF, VF_JSON_WRITE_TUNNEL_SNAPF)
#undef VF_JSON_WRITE_TUNNEL_LERPF
#undef VF_JSON_WRITE_TUNNEL_SNAPF
            return Object;
        }

        TMap<FString, FProperty*> Properties;
        VF_CollectProperties(Struct, Properties);
        TArray<FString> Names;
        for (const TPair<FString, FProperty*>& Pair : Properties)
        {
            Names.Add(Pair.Key);
        }
        Names.Sort([](const FString& A, const FString& B) { return A < B; });
        for (const FString& FieldName : Names)
        {
            if (VF_IsRuntimeField(FieldName) || VF_IsExcludedForArchetype(Archetype, FieldName))
            {
                continue;
            }
            FProperty* const* PropertyPtr = Properties.Find(FieldName);
            if (PropertyPtr == nullptr || *PropertyPtr == nullptr
                || !VF_IsScalarProperty(*PropertyPtr))
            {
                continue;
            }
            double Value = 0.0;
            if (VF_ReadPropertyValue(*PropertyPtr, Memory, Value))
            {
                Object->SetNumberField(FieldName, Value);
            }
        }
        return Object;
    }

    TSharedPtr<FJsonObject> VF_SerializeAllArchetypeParams(
        const FVoxelStrateArchetypeParams& Params)
    {
        // A recipe may read more than the active archetype's family (for example a tunnel
        // modifier can be selected while the shape source belongs to another family). Persist
        // the complete native parameter vector so re-verification measures the same recipe.
        TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
        Object->SetObjectField(TEXT("tunnel_network"),
                               VF_SerializeArchetypeParams(
                                   ECaveGeneratorType::TunnelNetwork, Params));
        Object->SetObjectField(TEXT("slab"),
                               VF_SerializeArchetypeParams(
                                   ECaveGeneratorType::FlatPlain, Params));
        Object->SetObjectField(TEXT("maze"),
                               VF_SerializeArchetypeParams(
                                   ECaveGeneratorType::Maze, Params));
        Object->SetObjectField(TEXT("surface"),
                               VF_SerializeArchetypeParams(
                                   ECaveGeneratorType::SurfaceWorld, Params));
        Object->SetObjectField(TEXT("vertical_shaft"),
                               VF_SerializeArchetypeParams(
                                   ECaveGeneratorType::VerticalShafts, Params));
        Object->SetObjectField(TEXT("floating_island"),
                               VF_SerializeArchetypeParams(
                                   ECaveGeneratorType::FloatingIslands, Params));
        return Object;
    }

    void VF_ResetParamFamily(FVoxelStrateArchetypeParams& Params,
                             ECaveGeneratorType Archetype)
    {
        switch (Archetype)
        {
        case ECaveGeneratorType::TunnelNetwork:
        case ECaveGeneratorType::Underwater:
            Params.TunnelNetworkParams = FStrateGenerationParams();
            break;
        case ECaveGeneratorType::FlatPlain:
        case ECaveGeneratorType::CrystalChamber:
            Params.SlabParams = FSlabGenerationParams();
            break;
        case ECaveGeneratorType::Maze:
            Params.MazeParams = FMazeGenerationParams();
            break;
        case ECaveGeneratorType::SurfaceWorld:
            Params.SurfaceParams = FSurfaceGenerationParams();
            break;
        case ECaveGeneratorType::VerticalShafts:
            Params.VerticalShaftParams = FVerticalShaftParams();
            break;
        case ECaveGeneratorType::FloatingIslands:
            Params.FloatingIslandParams = FFloatingIslandParams();
            break;
        default:
            break;
        }
    }

    bool VF_DeserializeArchetypeParams(
        ECaveGeneratorType Archetype,
        const TSharedPtr<FJsonObject>& Object,
        FVoxelStrateArchetypeParams& OutParams)
    {
        if (!Object.IsValid() || !VF_IsSupportedArchetype(Archetype))
        {
            return false;
        }

        // Reset only the family being read. The promoted record stores all families, and each
        // successive call must preserve the values already decoded for the other families.
        VF_ResetParamFamily(OutParams, Archetype);
        void* Memory = VF_GetParamMemory(OutParams, Archetype);
        UStruct* Struct = VF_GetParamStruct(Archetype);
        if (Memory == nullptr || Struct == nullptr)
        {
            return false;
        }

        bool bValid = true;
        if (VF_IsTunnelArchetype(Archetype))
        {
#define VF_JSON_READ_TUNNEL_LERPF(Name) \
            do { \
                const FString FieldName(TEXT(#Name)); \
                if (!VF_IsExcludedForArchetype(Archetype, FieldName)) { \
                    double Value = 0.0; \
                    const bool bFieldValid = VF_ReadJsonNumber(Object, FieldName, Value) \
                        && VF_WriteNamedField(Memory, Archetype, FieldName, Value); \
                    bValid = bFieldValid && bValid; \
                } \
            } while (false);
#define VF_JSON_READ_TUNNEL_SNAPF(Name) VF_JSON_READ_TUNNEL_LERPF(Name)
            VF_STRATE_PARAM_FIELDS(VF_JSON_READ_TUNNEL_LERPF, VF_JSON_READ_TUNNEL_SNAPF)
#undef VF_JSON_READ_TUNNEL_LERPF
#undef VF_JSON_READ_TUNNEL_SNAPF
        }
        else
        {
            TMap<FString, FProperty*> Properties;
            VF_CollectProperties(Struct, Properties);
            for (const TPair<FString, FProperty*>& Pair : Properties)
            {
                const FString& FieldName = Pair.Key;
                if (VF_IsRuntimeField(FieldName) || VF_IsExcludedForArchetype(Archetype, FieldName)
                    || Pair.Value == nullptr || !VF_IsScalarProperty(Pair.Value))
                {
                    continue;
                }
                double Value = 0.0;
                const bool bFieldValid = VF_ReadJsonNumber(Object, FieldName, Value)
                    && VF_WritePropertyValue(Pair.Value, Memory, Value);
                bValid = bFieldValid && bValid;
            }
        }

        VF_ResetExcludedFields(OutParams, Archetype);
        return bValid;
    }

    bool VF_DeserializeAllArchetypeParams(
        const TSharedPtr<FJsonObject>& Object,
        FVoxelStrateArchetypeParams& OutParams)
    {
        if (!Object.IsValid())
        {
            return false;
        }

        TSharedPtr<FJsonObject> Tunnel;
        TSharedPtr<FJsonObject> Slab;
        TSharedPtr<FJsonObject> Maze;
        TSharedPtr<FJsonObject> Surface;
        TSharedPtr<FJsonObject> Vertical;
        TSharedPtr<FJsonObject> Floating;
        if (!VF_ReadJsonObject(Object, TEXT("tunnel_network"), Tunnel)
            || !VF_ReadJsonObject(Object, TEXT("slab"), Slab)
            || !VF_ReadJsonObject(Object, TEXT("maze"), Maze)
            || !VF_ReadJsonObject(Object, TEXT("surface"), Surface)
            || !VF_ReadJsonObject(Object, TEXT("vertical_shaft"), Vertical)
            || !VF_ReadJsonObject(Object, TEXT("floating_island"), Floating))
        {
            return false;
        }

        OutParams = FVoxelStrateArchetypeParams();
        bool bValid = true;
        bValid = VF_DeserializeArchetypeParams(
            ECaveGeneratorType::TunnelNetwork, Tunnel, OutParams) && bValid;
        bValid = VF_DeserializeArchetypeParams(
            ECaveGeneratorType::FlatPlain, Slab, OutParams) && bValid;
        bValid = VF_DeserializeArchetypeParams(
            ECaveGeneratorType::Maze, Maze, OutParams) && bValid;
        bValid = VF_DeserializeArchetypeParams(
            ECaveGeneratorType::SurfaceWorld, Surface, OutParams) && bValid;
        bValid = VF_DeserializeArchetypeParams(
            ECaveGeneratorType::VerticalShafts, Vertical, OutParams) && bValid;
        bValid = VF_DeserializeArchetypeParams(
            ECaveGeneratorType::FloatingIslands, Floating, OutParams) && bValid;
        return bValid;
    }

    void VF_SetJsonMetricNumbers(const TSharedPtr<FJsonObject>& Object,
                                 const FVoxelStrateMeasuredMetrics& Metrics)
    {
        Object->SetBoolField(TEXT("valid"), Metrics.bValid);
        Object->SetNumberField(TEXT("num_sampled"), static_cast<double>(Metrics.NumSampled));
        Object->SetNumberField(TEXT("num_air"), static_cast<double>(Metrics.NumAir));
        Object->SetNumberField(TEXT("num_solid"), static_cast<double>(Metrics.NumSolid));
        Object->SetNumberField(TEXT("air_fraction"), Metrics.AirFraction);
        Object->SetNumberField(TEXT("num_air_components"), Metrics.NumAirComponents);
        Object->SetNumberField(TEXT("largest_component_share"), Metrics.LargestComponentShare);
        Object->SetNumberField(TEXT("largest_component_cells"),
                               static_cast<double>(Metrics.LargestComponentCells));
        Object->SetNumberField(TEXT("num_components_at_least_1_pct"),
                               Metrics.NumComponentsAtLeast1Pct);
        Object->SetNumberField(TEXT("walkable_fraction"), Metrics.WalkableFraction);
        Object->SetNumberField(TEXT("walkable_floor_columns"),
                               static_cast<double>(Metrics.WalkableFloorColumns));
        Object->SetNumberField(TEXT("walkable_floor_area_fraction"),
                               Metrics.WalkableFloorAreaFraction);
        Object->SetNumberField(TEXT("num_walkable_surface_components"),
                               Metrics.NumWalkableSurfaceComponents);
        Object->SetNumberField(TEXT("largest_walkable_surface_columns"),
                               static_cast<double>(Metrics.LargestWalkableSurfaceColumns));
        Object->SetNumberField(TEXT("largest_walkable_surface_share"),
                               Metrics.LargestWalkableSurfaceShare);
        Object->SetNumberField(TEXT("median_feature_scale"), Metrics.MedianFeatureScale);
        Object->SetNumberField(TEXT("median_vertical_clearance"), Metrics.MedianVerticalClearance);
        Object->SetBoolField(TEXT("player_fit_resolved"), Metrics.bPlayerFitResolved);
        Object->SetStringField(TEXT("player_fit_refusal_reason"), Metrics.PlayerFitRefusalReason);
        Object->SetNumberField(TEXT("num_player_fit_cells"),
                               static_cast<double>(Metrics.NumPlayerFitCells));
        Object->SetNumberField(TEXT("player_fit_fraction"), Metrics.PlayerFitFraction);
        Object->SetNumberField(TEXT("num_traversable_components"),
                               Metrics.NumTraversableComponents);
        Object->SetNumberField(TEXT("largest_traversable_component_cells"),
                               static_cast<double>(Metrics.LargestTraversableComponentCells));
        Object->SetNumberField(TEXT("traversable_component_share"),
                               Metrics.TraversableComponentShare);
        Object->SetNumberField(TEXT("minimum_player_clearance_voxels"),
                               Metrics.MinimumPlayerClearanceVoxels);
        Object->SetNumberField(TEXT("resolved_margin_voxels"), Metrics.ResolvedMarginVoxels);
        Object->SetNumberField(TEXT("sampled_min_z"), Metrics.SampledMinZ);
        Object->SetNumberField(TEXT("sampled_max_z"), Metrics.SampledMaxZ);
        Object->SetNumberField(TEXT("sampled_num_x"), Metrics.SampledNumX);
        Object->SetNumberField(TEXT("sampled_num_y"), Metrics.SampledNumY);
        Object->SetNumberField(TEXT("sampled_num_z"), Metrics.SampledNumZ);
        Object->SetNumberField(TEXT("sampled_min_x"), Metrics.SampledMinX);
        Object->SetNumberField(TEXT("sampled_max_x"), Metrics.SampledMaxX);
        Object->SetNumberField(TEXT("sampled_min_y"), Metrics.SampledMinY);
        Object->SetNumberField(TEXT("sampled_max_y"), Metrics.SampledMaxY);

        TSharedPtr<FJsonObject> Point = MakeShared<FJsonObject>();
        Point->SetNumberField(TEXT("x"), Metrics.LargestComponentPoint.X);
        Point->SetNumberField(TEXT("y"), Metrics.LargestComponentPoint.Y);
        Point->SetNumberField(TEXT("z"), Metrics.LargestComponentPoint.Z);
        Object->SetObjectField(TEXT("largest_component_point"), Point);

        TArray<TSharedPtr<FJsonValue>> Components;
        Components.Reserve(Metrics.AirComponentCells.Num());
        for (const int64 Cells : Metrics.AirComponentCells)
        {
            Components.Add(MakeShared<FJsonValueNumber>(static_cast<double>(Cells)));
        }
        Object->SetArrayField(TEXT("air_component_cells"), MoveTemp(Components));
    }

    TSharedPtr<FJsonObject> VF_SerializeMetrics(const FVoxelStrateMeasuredMetrics& Metrics)
    {
        TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
        VF_SetJsonMetricNumbers(Object, Metrics);
        return Object;
    }

    bool VF_DeserializeMetrics(const TSharedPtr<FJsonObject>& Object,
                               FVoxelStrateMeasuredMetrics& OutMetrics)
    {
        if (!Object.IsValid())
        {
            return false;
        }

        bool bValid = true;
        bValid = VF_ReadJsonBool(Object, TEXT("valid"), OutMetrics.bValid) && bValid;
        bValid = VF_ReadJsonInt64(Object, TEXT("num_sampled"), OutMetrics.NumSampled) && bValid;
        bValid = VF_ReadJsonInt64(Object, TEXT("num_air"), OutMetrics.NumAir) && bValid;
        bValid = VF_ReadJsonInt64(Object, TEXT("num_solid"), OutMetrics.NumSolid) && bValid;
        bValid = VF_ReadJsonFloat(Object, TEXT("air_fraction"), OutMetrics.AirFraction) && bValid;
        bValid = VF_ReadJsonInt32(Object, TEXT("num_air_components"), OutMetrics.NumAirComponents) && bValid;
        bValid = VF_ReadJsonFloat(Object, TEXT("largest_component_share"),
                                  OutMetrics.LargestComponentShare) && bValid;
        bValid = VF_ReadJsonInt64(Object, TEXT("largest_component_cells"),
                                  OutMetrics.LargestComponentCells) && bValid;
        bValid = VF_ReadJsonInt32(Object, TEXT("num_components_at_least_1_pct"),
                                  OutMetrics.NumComponentsAtLeast1Pct) && bValid;
        bValid = VF_ReadJsonFloat(Object, TEXT("walkable_fraction"), OutMetrics.WalkableFraction) && bValid;
        // These fields were added after the first promotion-store schema. Keep old records
        // readable: their absence means the new diagnostic is unknown, not zero evidence.
        if (Object->HasField(TEXT("walkable_floor_columns")))
        {
            bValid = VF_ReadJsonInt64(Object, TEXT("walkable_floor_columns"),
                                      OutMetrics.WalkableFloorColumns) && bValid;
            bValid = VF_ReadJsonFloat(Object, TEXT("walkable_floor_area_fraction"),
                                      OutMetrics.WalkableFloorAreaFraction) && bValid;
            bValid = VF_ReadJsonInt32(Object, TEXT("num_walkable_surface_components"),
                                      OutMetrics.NumWalkableSurfaceComponents) && bValid;
            bValid = VF_ReadJsonInt64(Object, TEXT("largest_walkable_surface_columns"),
                                      OutMetrics.LargestWalkableSurfaceColumns) && bValid;
            bValid = VF_ReadJsonFloat(Object, TEXT("largest_walkable_surface_share"),
                                      OutMetrics.LargestWalkableSurfaceShare) && bValid;
        }
        bValid = VF_ReadJsonFloat(Object, TEXT("median_feature_scale"),
                                  OutMetrics.MedianFeatureScale) && bValid;
        bValid = VF_ReadJsonInt32(Object, TEXT("median_vertical_clearance"),
                                  OutMetrics.MedianVerticalClearance) && bValid;
        if (Object->HasField(TEXT("player_fit_resolved")))
        {
            bValid = VF_ReadJsonBool(Object, TEXT("player_fit_resolved"),
                                     OutMetrics.bPlayerFitResolved) && bValid;
            bValid = VF_ReadJsonString(Object, TEXT("player_fit_refusal_reason"),
                                       OutMetrics.PlayerFitRefusalReason) && bValid;
            bValid = VF_ReadJsonInt64(Object, TEXT("num_player_fit_cells"),
                                      OutMetrics.NumPlayerFitCells) && bValid;
            bValid = VF_ReadJsonFloat(Object, TEXT("player_fit_fraction"),
                                      OutMetrics.PlayerFitFraction) && bValid;
            bValid = VF_ReadJsonInt32(Object, TEXT("num_traversable_components"),
                                      OutMetrics.NumTraversableComponents) && bValid;
            bValid = VF_ReadJsonInt64(Object, TEXT("largest_traversable_component_cells"),
                                      OutMetrics.LargestTraversableComponentCells) && bValid;
            bValid = VF_ReadJsonFloat(Object, TEXT("traversable_component_share"),
                                      OutMetrics.TraversableComponentShare) && bValid;
            bValid = VF_ReadJsonFloat(Object, TEXT("minimum_player_clearance_voxels"),
                                      OutMetrics.MinimumPlayerClearanceVoxels) && bValid;
        }
        bValid = VF_ReadJsonInt32(Object, TEXT("resolved_margin_voxels"),
                                  OutMetrics.ResolvedMarginVoxels) && bValid;
        bValid = VF_ReadJsonInt32(Object, TEXT("sampled_min_z"), OutMetrics.SampledMinZ) && bValid;
        bValid = VF_ReadJsonInt32(Object, TEXT("sampled_max_z"), OutMetrics.SampledMaxZ) && bValid;
        bValid = VF_ReadJsonInt32(Object, TEXT("sampled_num_x"), OutMetrics.SampledNumX) && bValid;
        bValid = VF_ReadJsonInt32(Object, TEXT("sampled_num_y"), OutMetrics.SampledNumY) && bValid;
        bValid = VF_ReadJsonInt32(Object, TEXT("sampled_num_z"), OutMetrics.SampledNumZ) && bValid;
        bValid = VF_ReadJsonFloat(Object, TEXT("sampled_min_x"), OutMetrics.SampledMinX) && bValid;
        bValid = VF_ReadJsonFloat(Object, TEXT("sampled_max_x"), OutMetrics.SampledMaxX) && bValid;
        bValid = VF_ReadJsonFloat(Object, TEXT("sampled_min_y"), OutMetrics.SampledMinY) && bValid;
        bValid = VF_ReadJsonFloat(Object, TEXT("sampled_max_y"), OutMetrics.SampledMaxY) && bValid;

        TSharedPtr<FJsonObject> Point;
        bValid = VF_ReadJsonObject(Object, TEXT("largest_component_point"), Point) && bValid;
        if (Point.IsValid())
        {
            bValid = VF_ReadJsonNumber(Point, TEXT("x"), OutMetrics.LargestComponentPoint.X) && bValid;
            bValid = VF_ReadJsonNumber(Point, TEXT("y"), OutMetrics.LargestComponentPoint.Y) && bValid;
            bValid = VF_ReadJsonNumber(Point, TEXT("z"), OutMetrics.LargestComponentPoint.Z) && bValid;
        }

        const TArray<TSharedPtr<FJsonValue>>* Components = nullptr;
        bValid = VF_ReadJsonArray(Object, TEXT("air_component_cells"), Components) && bValid;
        OutMetrics.AirComponentCells.Reset();
        if (Components != nullptr)
        {
            OutMetrics.AirComponentCells.Reserve(Components->Num());
            for (const TSharedPtr<FJsonValue>& Value : *Components)
            {
                if (!Value.IsValid() || Value->Type != EJson::Number
                    || !FMath::IsFinite(Value->AsNumber()))
                {
                    bValid = false;
                    continue;
                }
                const double Number = Value->AsNumber();
                const int64 Cells = static_cast<int64>(Number);
                if (Number < 0.0 || static_cast<double>(Cells) != Number)
                {
                    bValid = false;
                    continue;
                }
                OutMetrics.AirComponentCells.Add(Cells);
            }
        }
        return bValid;
    }

    TSharedPtr<FJsonObject> VF_SerializePromotableRecord(
        const FVoxelStratePromotableRecord& Record)
    {
        TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
        Object->SetStringField(TEXT("record_id"), Record.RecordId);
        Object->SetNumberField(TEXT("season"), Record.Season);
        Object->SetNumberField(TEXT("seed"), Record.Seed);
        Object->SetNumberField(TEXT("candidate_index"), Record.CandidateIndex);
        Object->SetNumberField(TEXT("input_corpus_hash"),
                               static_cast<double>(Record.InputCorpusHash));
        Object->SetStringField(TEXT("archetype"), VF_GetStrateArchetypeName(Record.Archetype));
        Object->SetObjectField(TEXT("parameters"),
                               VF_SerializeAllArchetypeParams(Record.Params));
        Object->SetObjectField(TEXT("recipe"), VF_SerializeRecipe(Record.Recipe));
        Object->SetObjectField(TEXT("measured_metrics"), VF_SerializeMetrics(Record.MeasuredMetrics));
        Object->SetBoolField(TEXT("passed_non_vacuous"), Record.bPassedNonVacuous);
        Object->SetBoolField(TEXT("passed_largest_component"), Record.bPassedLargestComponent);
        Object->SetBoolField(TEXT("passed_primordial_law"), Record.bPassedPrimordialLaw);
        return Object;
    }

    bool VF_DeserializePromotableRecord(
        const TSharedPtr<FJsonObject>& Object,
        FVoxelStratePromotableRecord& OutRecord)
    {
        if (!Object.IsValid()
            || !VF_ReadJsonString(Object, TEXT("record_id"), OutRecord.RecordId)
            || OutRecord.RecordId.IsEmpty()
            || !VF_ReadJsonInt32(Object, TEXT("season"), OutRecord.Season)
            || !VF_ReadJsonInt32(Object, TEXT("seed"), OutRecord.Seed)
            || !VF_ReadJsonInt32(Object, TEXT("candidate_index"), OutRecord.CandidateIndex))
        {
            return false;
        }

        int64 CorpusHash = 0;
        FString ArchetypeName;
        TSharedPtr<FJsonObject> Params;
        TSharedPtr<FJsonObject> Recipe;
        TSharedPtr<FJsonObject> Metrics;
        if (!VF_ReadJsonInt64(Object, TEXT("input_corpus_hash"), CorpusHash)
            || CorpusHash <= 0 || CorpusHash > static_cast<int64>(TNumericLimits<uint32>::Max())
            || !VF_ReadJsonString(Object, TEXT("archetype"), ArchetypeName)
            || !VF_ReadJsonObject(Object, TEXT("parameters"), Params)
            || !VF_ReadJsonObject(Object, TEXT("recipe"), Recipe)
            || !VF_ReadJsonObject(Object, TEXT("measured_metrics"), Metrics)
            || !VF_ReadJsonBool(Object, TEXT("passed_non_vacuous"), OutRecord.bPassedNonVacuous)
            || !VF_ReadJsonBool(Object, TEXT("passed_largest_component"), OutRecord.bPassedLargestComponent)
            || !VF_ReadJsonBool(Object, TEXT("passed_primordial_law"), OutRecord.bPassedPrimordialLaw))
        {
            return false;
        }

        bool bArchetypeFound = false;
        for (const ECaveGeneratorType Archetype : GAllArchetypes)
        {
            if (ArchetypeName == VF_GetStrateArchetypeName(Archetype))
            {
                OutRecord.Archetype = Archetype;
                bArchetypeFound = true;
                break;
            }
        }
        if (!bArchetypeFound
            || !VF_DeserializeAllArchetypeParams(Params, OutRecord.Params)
            || !VF_DeserializeRecipe(Recipe, OutRecord.Recipe)
            || !VF_DeserializeMetrics(Metrics, OutRecord.MeasuredMetrics))
        {
            return false;
        }
        OutRecord.InputCorpusHash = static_cast<uint32>(CorpusHash);
        return true;
    }

    bool VF_MeasuredMetricsExactlyEqual(const FVoxelStrateMeasuredMetrics& A,
                                        const FVoxelStrateMeasuredMetrics& B)
    {
        return A.bValid == B.bValid
            && A.NumSampled == B.NumSampled
            && A.NumAir == B.NumAir
            && A.NumSolid == B.NumSolid
            && A.AirFraction == B.AirFraction
            && A.NumAirComponents == B.NumAirComponents
            && A.LargestComponentShare == B.LargestComponentShare
            && A.LargestComponentPoint == B.LargestComponentPoint
            && A.LargestComponentCells == B.LargestComponentCells
            && A.NumComponentsAtLeast1Pct == B.NumComponentsAtLeast1Pct
            && A.WalkableFraction == B.WalkableFraction
            && A.WalkableFloorColumns == B.WalkableFloorColumns
            && A.WalkableFloorAreaFraction == B.WalkableFloorAreaFraction
            && A.NumWalkableSurfaceComponents == B.NumWalkableSurfaceComponents
            && A.LargestWalkableSurfaceColumns == B.LargestWalkableSurfaceColumns
            && A.LargestWalkableSurfaceShare == B.LargestWalkableSurfaceShare
            && A.MedianFeatureScale == B.MedianFeatureScale
            && A.MedianVerticalClearance == B.MedianVerticalClearance
            && A.bPlayerFitResolved == B.bPlayerFitResolved
            && A.PlayerFitRefusalReason == B.PlayerFitRefusalReason
            && A.NumPlayerFitCells == B.NumPlayerFitCells
            && A.PlayerFitFraction == B.PlayerFitFraction
            && A.NumTraversableComponents == B.NumTraversableComponents
            && A.LargestTraversableComponentCells == B.LargestTraversableComponentCells
            && A.TraversableComponentShare == B.TraversableComponentShare
            && A.MinimumPlayerClearanceVoxels == B.MinimumPlayerClearanceVoxels
            && A.ResolvedMarginVoxels == B.ResolvedMarginVoxels
            && A.SampledMinZ == B.SampledMinZ
            && A.SampledMaxZ == B.SampledMaxZ
            && A.SampledNumX == B.SampledNumX
            && A.SampledNumY == B.SampledNumY
            && A.SampledNumZ == B.SampledNumZ
            && A.SampledMinX == B.SampledMinX
            && A.SampledMaxX == B.SampledMaxX
            && A.SampledMinY == B.SampledMinY
            && A.SampledMaxY == B.SampledMaxY
            && A.AirComponentCells == B.AirComponentCells;
    }

    bool VF_SaveJsonObject(const FString& FilePath,
                           const TSharedPtr<FJsonObject>& Object,
                           FString& OutError)
    {
        const FString Directory = FPaths::GetPath(FilePath);
        if (!Directory.IsEmpty() && !IFileManager::Get().MakeDirectory(*Directory, true))
        {
            OutError = FString::Printf(TEXT("could not create JSON directory %s"), *Directory);
            return false;
        }

        FString JsonText;
        TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
            TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&JsonText);
        if (!FJsonSerializer::Serialize(Object.ToSharedRef(), Writer) || !Writer->Close())
        {
            OutError = FString::Printf(TEXT("could not serialise JSON for %s"), *FilePath);
            return false;
        }
        if (!FFileHelper::SaveStringToFile(JsonText, *FilePath))
        {
            OutError = FString::Printf(TEXT("could not write JSON file %s"), *FilePath);
            return false;
        }
        return true;
    }

    bool VF_LoadJsonObject(const FString& FilePath,
                           TSharedPtr<FJsonObject>& OutObject,
                           FString& OutError)
    {
        FString JsonText;
        if (!FFileHelper::LoadFileToString(JsonText, *FilePath))
        {
            OutError = FString::Printf(TEXT("could not read JSON file %s"), *FilePath);
            return false;
        }

        TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
        if (!FJsonSerializer::Deserialize(Reader, OutObject) || !OutObject.IsValid())
        {
            OutError = FString::Printf(TEXT("invalid JSON in %s"), *FilePath);
            return false;
        }
        return true;
    }

    FString VF_PromotionSortKey(const FVoxelStratePromotableRecord& Record)
    {
        if (!Record.RecordId.IsEmpty())
        {
            return Record.RecordId;
        }
        return FString::Printf(TEXT("season_%08d_seed_%08d_candidate_%08d_archetype_%d_recipe_%08x"),
                               Record.Season, Record.Seed, Record.CandidateIndex,
                               static_cast<int32>(static_cast<uint8>(Record.Archetype)),
                               VF_HashStrateStructureRecipe(Record.Recipe));
    }

    bool VF_MetricsAreFinite(const FVoxelStrateMeasuredMetrics& Metrics)
    {
        return FMath::IsFinite(Metrics.AirFraction)
            && FMath::IsFinite(Metrics.LargestComponentShare)
            && FMath::IsFinite(Metrics.WalkableFraction)
            && FMath::IsFinite(Metrics.WalkableFloorAreaFraction)
            && FMath::IsFinite(Metrics.LargestWalkableSurfaceShare)
            && FMath::IsFinite(Metrics.MedianFeatureScale)
            && (!Metrics.bPlayerFitResolved
                || (FMath::IsFinite(Metrics.PlayerFitFraction)
                    && FMath::IsFinite(Metrics.TraversableComponentShare)
                    && FMath::IsFinite(Metrics.MinimumPlayerClearanceVoxels)))
            && FMath::IsFinite(Metrics.LargestComponentPoint.X)
            && FMath::IsFinite(Metrics.LargestComponentPoint.Y)
            && FMath::IsFinite(Metrics.LargestComponentPoint.Z);
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
        int32 AuthoredSampleCount = 0;
        int32 PromotedSampleCount = 0;
        for (const FVoxelStrateCorpusEntry& Entry : Entries)
        {
            if (Entry.Archetype != FieldSpreads[FieldIndex].Archetype)
            {
                continue;
            }
            double Value = 0.0;
            const void* Memory = VF_GetParamMemory(Entry.ArchetypeParams, Entry.Archetype);
            if (Memory != nullptr
                && VF_ReadNamedField(Memory, Entry.Archetype, FieldSpreads[FieldIndex].FieldName, Value)
                && FMath::IsFinite(Value))
            {
                if (Entry.Provenance == EVoxelStrateCorpusProvenance::Promoted)
                {
                    ++PromotedSampleCount;
                }
                else
                {
                    ++AuthoredSampleCount;
                }
            }
        }
        FieldSpreads[FieldIndex].AuthoredSampleCount = AuthoredSampleCount;
        FieldSpreads[FieldIndex].PromotedSampleCount = PromotedSampleCount;
    }
    VF_AddTerrainOperationDefaultSamples(Values, FieldSpreads);

    for (int32 FieldIndex = 0; FieldIndex < FieldSpreads.Num(); ++FieldIndex)
    {
        FVoxelStrateFieldSpread& Spread = FieldSpreads[FieldIndex];
        const TArray<double>& Samples = Values[FieldIndex];
        Spread.DefaultSeedCount = Samples.Num() - Spread.AuthoredSampleCount
            - Spread.PromotedSampleCount;
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
    return AddEntryInternal(SourcePath, SourceName, Archetype, Params, Weight,
                            EVoxelStrateCorpusProvenance::Project,
                            nullptr, nullptr, FString(), INDEX_NONE, 0, INDEX_NONE, 0, true);
}

bool FVoxelStrateCorpus::AddEntryInternal(
    const FString& SourcePath, const FString& SourceName,
    ECaveGeneratorType Archetype, const FVoxelStrateArchetypeParams& Params,
    float Weight, EVoxelStrateCorpusProvenance Provenance,
    const FVoxelOpStackRecipe* Recipe,
    const FVoxelStrateMeasuredMetrics* Metrics,
    const FString& PromotionRecordId,
    int32 PromotionSeason,
    int32 PromotionSeed,
    int32 PromotionCandidateIndex,
    uint32 PromotionInputCorpusHash,
    bool bRebuild)
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
    Entry.Provenance = Provenance;
    Entry.bHasRecipe = Recipe != nullptr;
    if (Recipe != nullptr)
    {
        Entry.Recipe = *Recipe;
    }
    Entry.bHasMeasuredMetrics = Metrics != nullptr;
    if (Metrics != nullptr)
    {
        Entry.MeasuredMetrics = *Metrics;
    }
    Entry.PromotionRecordId = PromotionRecordId;
    Entry.PromotionSeason = PromotionSeason;
    Entry.PromotionSeed = PromotionSeed;
    Entry.PromotionCandidateIndex = PromotionCandidateIndex;
    Entry.PromotionInputCorpusHash = PromotionInputCorpusHash;

    Entries.Sort([](const FVoxelStrateCorpusEntry& A, const FVoxelStrateCorpusEntry& B)
    {
        if (A.SourcePath != B.SourcePath) { return A.SourcePath < B.SourcePath; }
        if (static_cast<uint8>(A.Archetype) != static_cast<uint8>(B.Archetype))
        {
            return static_cast<uint8>(A.Archetype) < static_cast<uint8>(B.Archetype);
        }
        return A.SourceName < B.SourceName;
    });
    if (bRebuild)
    {
        RebuildSpreads();
    }
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
        if (AddEntryInternal(DefaultPath, DefaultName, Archetype, Defaults, 1.0f,
                             EVoxelStrateCorpusProvenance::Default,
                             nullptr, nullptr, FString(), INDEX_NONE, 0, INDEX_NONE, 0, true))
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

bool FVoxelStrateCorpus::LoadFromAssetRegistry(
    FString& OutReport,
    const FString& PromotedStorePath,
    const IVoxelStratePromotionVerifier* PromotionVerifier)
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
    int32 NumBaseMetricsMeasured = 0;
    int32 NumBaseMetricsUnusable = 0;
    int32 NumBaseMetricsUnavailable = 0;
    if (PromotionVerifier != nullptr)
    {
        for (FVoxelStrateCorpusEntry& Entry : Entries)
        {
            if (Entry.Provenance == EVoxelStrateCorpusProvenance::Promoted)
            {
                continue;
            }

            FVoxelStrateMeasuredMetrics Metrics;
            if (PromotionVerifier->MeasureCorpusEntry(Entry, Metrics)
                && Metrics.bValid)
            {
                Entry.MeasuredMetrics = Metrics;
                Entry.bHasMeasuredMetrics = true;
                ++NumBaseMetricsMeasured;
                if (!Metrics.IsUsable())
                {
                    ++NumBaseMetricsUnusable;
                }
            }
            else
            {
                Entry.bHasMeasuredMetrics = false;
                ++NumBaseMetricsUnavailable;
            }
        }
    }
    OutReport = FString::Printf(
        TEXT("Asset Registry strate scan: %d records; %d project candidates; %d Saved/Autosaves "
             "or Saved/Cooked copies ignored; %d outside project; %d duplicate records; %d unresolved. "
             "%s Base metric audit: %d measured (%d usable, %d vacuous), %d unavailable."),
        AssetData.Num(), SeenObjectPaths.Num(), NumIgnoredSaved, NumOutsideProject,
        NumDuplicateRecords, NumUnresolved, *DefinitionReport,
        NumBaseMetricsMeasured, NumBaseMetricsMeasured - NumBaseMetricsUnusable,
        NumBaseMetricsUnusable, NumBaseMetricsUnavailable);

    const FString ResolvedPromotedStorePath = PromotedStorePath.IsEmpty()
        ? FPaths::ProjectSavedDir() / TEXT("VoxelForge") / TEXT("StrateCorpus")
            / TEXT("promoted_strates.json")
        : PromotedStorePath;
    FString PromotedReport;
    const bool bPromotedLoaded = LoadPromotedRecords(
        ResolvedPromotedStorePath, PromotionVerifier, PromotedReport);
    OutReport += FString::Printf(
        TEXT(" Promotion store: %s Membership provenance: project=%d default=%d promoted=%d."),
        *PromotedReport,
        NumForProvenance(EVoxelStrateCorpusProvenance::Project),
        NumForProvenance(EVoxelStrateCorpusProvenance::Default),
        NumForProvenance(EVoxelStrateCorpusProvenance::Promoted));
    return bLoaded && NumUnresolved == 0 && bPromotedLoaded;
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

int32 FVoxelStrateCorpus::NumForProvenance(EVoxelStrateCorpusProvenance Provenance) const
{
    int32 Count = 0;
    for (const FVoxelStrateCorpusEntry& Entry : Entries)
    {
        if (Entry.Provenance == Provenance)
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
        const bool bPromotedEntry = Entry.Provenance == EVoxelStrateCorpusProvenance::Promoted;

        // Preserve the Tier 4 hash byte-for-byte for the existing project/default corpus. A
        // promoted member is a new input, however, and its recipe must participate in the hash so
        // a season can be reproduced from exactly the same cumulative corpus contents.
        if (bPromotedEntry)
        {
            const uint8 PromotedMarker = 1;
            HashBytes(&PromotedMarker, sizeof(PromotedMarker));
            const uint32 RecipeHash = Entry.bHasRecipe
                ? VF_HashStrateStructureRecipe(Entry.Recipe) : 0U;
            HashBytes(&RecipeHash, sizeof(RecipeHash));
        }

        const void* Memory = VF_GetParamMemory(Entry.ArchetypeParams, Entry.Archetype);
        UStruct* Struct = VF_GetParamStruct(Entry.Archetype);
        if (Memory == nullptr || Struct == nullptr)
        {
            continue;
        }

        if (VF_IsTunnelArchetype(Entry.Archetype))
        {
#define VF_HASH_FSTRATE_FIELD(Name) \
            if (!bPromotedEntry || !VF_IsRuntimeField(TEXT(#Name))) { \
                HashString(TEXT(#Name)); \
                HashBytes(&Entry.ArchetypeParams.TunnelNetworkParams.Name, \
                          sizeof(Entry.ArchetypeParams.TunnelNetworkParams.Name)); \
            }
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
                if (!bPromotedEntry
                    && VF_ReadNamedField(Memory, Entry.Archetype, RuntimeField, Value))
                {
                    HashString(RuntimeField);
                    HashBytes(&Value, sizeof(Value));
                }
            }
        }

        if (bPromotedEntry)
        {
            // Promoted recipes carry all six native families. Include the complete vector in the
            // cumulative corpus identity, while omitting runtime Z bounds so a saved record and
            // its reloaded form hash identically.
            const ECaveGeneratorType Families[] =
            {
                ECaveGeneratorType::TunnelNetwork,
                ECaveGeneratorType::FlatPlain,
                ECaveGeneratorType::Maze,
                ECaveGeneratorType::SurfaceWorld,
                ECaveGeneratorType::VerticalShafts,
                ECaveGeneratorType::FloatingIslands,
            };
            for (const ECaveGeneratorType Family : Families)
            {
                const uint8 FamilyCode = static_cast<uint8>(Family);
                HashBytes(&FamilyCode, sizeof(FamilyCode));
                const void* FamilyMemory = VF_GetParamMemory(Entry.ArchetypeParams, Family);
                UStruct* FamilyStruct = VF_GetParamStruct(Family);
                if (FamilyMemory == nullptr || FamilyStruct == nullptr)
                {
                    continue;
                }

                if (VF_IsTunnelArchetype(Family))
                {
#define VF_HASH_PROMOTED_TUNNEL_FIELD(Name) \
                    if (!VF_IsRuntimeField(TEXT(#Name))) { \
                        HashString(TEXT(#Name)); \
                        HashBytes(&Entry.ArchetypeParams.TunnelNetworkParams.Name, \
                                  sizeof(Entry.ArchetypeParams.TunnelNetworkParams.Name)); \
                    }
                    VF_STRATE_PARAM_FIELDS(VF_HASH_PROMOTED_TUNNEL_FIELD, VF_HASH_PROMOTED_TUNNEL_FIELD)
#undef VF_HASH_PROMOTED_TUNNEL_FIELD
                }
                else
                {
                    TMap<FString, FProperty*> FamilyProperties;
                    VF_CollectProperties(FamilyStruct, FamilyProperties);
                    TArray<FString> FamilyNames;
                    for (const TPair<FString, FProperty*>& Pair : FamilyProperties)
                    {
                        FamilyNames.Add(Pair.Key);
                    }
                    FamilyNames.Sort([](const FString& A, const FString& B)
                    {
                        return A < B;
                    });
                    for (const FString& FieldName : FamilyNames)
                    {
                        const FProperty* const* PropertyPtr = FamilyProperties.Find(FieldName);
                        if (PropertyPtr == nullptr || *PropertyPtr == nullptr
                            || !VF_IsScalarProperty(*PropertyPtr))
                        {
                            continue;
                        }
                        HashString(FieldName);
                        const void* ValuePtr = (*PropertyPtr)->ContainerPtrToValuePtr<void>(FamilyMemory);
                        HashBytes(ValuePtr, (*PropertyPtr)->GetSize());
                    }
                }
            }
        }
    }
    return Hash;
}

const TCHAR* VF_GetStrateCorpusProvenanceName(EVoxelStrateCorpusProvenance Provenance)
{
    switch (Provenance)
    {
    case EVoxelStrateCorpusProvenance::Project:  return TEXT("project");
    case EVoxelStrateCorpusProvenance::Default:  return TEXT("default");
    case EVoxelStrateCorpusProvenance::Promoted: return TEXT("promoted");
    }
    return TEXT("unknown");
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

double VF_NormalizedMeasuredMetricDistance(
    const FVoxelStrateMeasuredMetrics& A,
    const FVoxelStrateMeasuredMetrics& B,
    const FVoxelStratePromotionPolicy& Policy)
{
    if (!A.IsUsable() || !B.IsUsable()
        || !FMath::IsFinite(Policy.FeatureScaleNormalizationVoxels)
        || !FMath::IsFinite(Policy.ClearanceNormalizationVoxels)
        || Policy.FeatureScaleNormalizationVoxels <= 0.0f
        || Policy.ClearanceNormalizationVoxels <= 0.0f)
    {
        return TNumericLimits<double>::Max();
    }

    const double FeatureDelta = static_cast<double>(A.MedianFeatureScale - B.MedianFeatureScale)
        / static_cast<double>(Policy.FeatureScaleNormalizationVoxels);
    const double ClearanceDelta = static_cast<double>(A.MedianVerticalClearance
                                                       - B.MedianVerticalClearance)
        / static_cast<double>(Policy.ClearanceNormalizationVoxels);
    const double AirDelta = static_cast<double>(A.AirFraction - B.AirFraction);
    const double LargestDelta = static_cast<double>(A.LargestComponentShare
                                                    - B.LargestComponentShare);
    const double WalkableDelta = static_cast<double>(A.WalkableFraction - B.WalkableFraction);
    const double DistanceSquared = AirDelta * AirDelta
        + LargestDelta * LargestDelta
        + WalkableDelta * WalkableDelta
        + FeatureDelta * FeatureDelta
        + ClearanceDelta * ClearanceDelta;
    return FMath::Sqrt(FMath::Max(0.0, DistanceSquared));
}

FVoxelStratePromotionBatchResult VF_SelectStratePromotions(
    const FVoxelStrateCorpus& Corpus,
    const TArray<FVoxelStratePromotableRecord>& Candidates,
    const FVoxelStratePromotionPolicy& Policy)
{
    FVoxelStratePromotionBatchResult Result;
    Result.NumCandidates = Candidates.Num();
    Result.MinimumAcceptedDistance = 0.0;

    if (!FMath::IsFinite(Policy.MinimumNormalizedMeasuredMetricDistance)
        || Policy.MinimumNormalizedMeasuredMetricDistance <= 0.0
        || Policy.MaxPromotionsPerSeason < 0
        || !FMath::IsFinite(Policy.FeatureScaleNormalizationVoxels)
        || !FMath::IsFinite(Policy.ClearanceNormalizationVoxels)
        || Policy.FeatureScaleNormalizationVoxels <= 0.0f
        || Policy.ClearanceNormalizationVoxels <= 0.0f)
    {
        return Result;
    }
    Result.bPolicyValid = true;

    const uint32 ExpectedCorpusHash = Corpus.GetContentsHash();
    TArray<FVoxelStratePromotableRecord> Ordered = Candidates;
    Ordered.Sort([](const FVoxelStratePromotableRecord& A,
                   const FVoxelStratePromotableRecord& B)
    {
        const FString AKey = VF_PromotionSortKey(A);
        const FString BKey = VF_PromotionSortKey(B);
        if (AKey != BKey)
        {
            return AKey < BKey;
        }
        if (A.InputCorpusHash != B.InputCorpusHash)
        {
            return A.InputCorpusHash < B.InputCorpusHash;
        }
        return A.CandidateIndex < B.CandidateIndex;
    });

    double MinimumDistance = TNumericLimits<double>::Max();
    for (const FVoxelStratePromotableRecord& Candidate : Ordered)
    {
        if (Candidate.InputCorpusHash != ExpectedCorpusHash)
        {
            ++Result.NumCorpusHashRejected;
            continue;
        }
        if (!Candidate.bPassedNonVacuous
            || !Candidate.bPassedLargestComponent
            || !Candidate.bPassedPrimordialLaw
            || !Candidate.MeasuredMetrics.IsUsable()
            || !VF_MetricsAreFinite(Candidate.MeasuredMetrics)
            || !VF_IsSupportedArchetype(Candidate.Archetype)
            || Candidate.RecordId.IsEmpty())
        {
            ++Result.NumValidationRejected;
            continue;
        }

        bool bNearDuplicate = false;
        for (const FVoxelStrateCorpusEntry& Entry : Corpus.GetEntries())
        {
            if (Entry.bHasMeasuredMetrics && Entry.MeasuredMetrics.IsUsable())
            {
                const double Distance = VF_NormalizedMeasuredMetricDistance(
                    Candidate.MeasuredMetrics, Entry.MeasuredMetrics, Policy);
                if (Distance < Policy.MinimumNormalizedMeasuredMetricDistance)
                {
                    bNearDuplicate = true;
                    break;
                }
            }
            else if (Entry.Archetype == Candidate.Archetype
                     && VF_AreStrateArchetypeParamsBitIdentical(
                         Entry.ArchetypeParams, Candidate.Params, Candidate.Archetype))
            {
                bNearDuplicate = true;
                break;
            }
        }
        if (!bNearDuplicate)
        {
            for (const FVoxelStratePromotableRecord& Accepted : Result.Promoted)
            {
                const double Distance = VF_NormalizedMeasuredMetricDistance(
                    Candidate.MeasuredMetrics, Accepted.MeasuredMetrics, Policy);
                if (Distance < Policy.MinimumNormalizedMeasuredMetricDistance
                    || (Candidate.Archetype == Accepted.Archetype
                        && VF_AreStrateArchetypeParamsBitIdentical(
                            Candidate.Params, Accepted.Params, Candidate.Archetype)))
                {
                    bNearDuplicate = true;
                    break;
                }
                MinimumDistance = FMath::Min(MinimumDistance, Distance);
            }
        }
        if (bNearDuplicate)
        {
            ++Result.NumNearDuplicateRejected;
            continue;
        }
        if (Result.Promoted.Num() >= Policy.MaxPromotionsPerSeason)
        {
            ++Result.NumCapRejected;
            continue;
        }

        Result.Promoted.Add(Candidate);
        // A first accepted record has no pairwise distance. Keep the public value at zero until
        // the batch contains a meaningful comparison.
        if (MinimumDistance < TNumericLimits<double>::Max())
        {
            Result.MinimumAcceptedDistance = MinimumDistance;
        }
    }
    return Result;
}

bool VF_SaveStratePromotedRecords(
    const FString& StorePath,
    const TArray<FVoxelStratePromotableRecord>& Records,
    FString& OutReport)
{
    if (StorePath.IsEmpty())
    {
        OutReport = TEXT("promotion store path is empty");
        return false;
    }

    TArray<FVoxelStratePromotableRecord> SortedRecords = Records;
    SortedRecords.Sort([](const FVoxelStratePromotableRecord& A,
                          const FVoxelStratePromotableRecord& B)
    {
        return VF_PromotionSortKey(A) < VF_PromotionSortKey(B);
    });

    TSet<FString> RecordIds;
    TArray<TSharedPtr<FJsonValue>> JsonRecords;
    JsonRecords.Reserve(SortedRecords.Num());
    for (const FVoxelStratePromotableRecord& Record : SortedRecords)
    {
        if (Record.RecordId.IsEmpty() || RecordIds.Contains(Record.RecordId))
        {
            OutReport = FString::Printf(
                TEXT("promotion store contains an empty or duplicate record id (%s)"),
                *Record.RecordId);
            return false;
        }
        RecordIds.Add(Record.RecordId);
        JsonRecords.Add(MakeShared<FJsonValueObject>(VF_SerializePromotableRecord(Record)));
    }

    TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetStringField(TEXT("format"), TEXT("VoxelForgePromotedStrates"));
    Root->SetNumberField(TEXT("schema_version"), GPromotionStoreSchemaVersion);
    Root->SetNumberField(TEXT("record_count"), SortedRecords.Num());
    Root->SetArrayField(TEXT("records"), MoveTemp(JsonRecords));

    FString Error;
    if (!VF_SaveJsonObject(StorePath, Root, Error))
    {
        OutReport = Error;
        return false;
    }
    OutReport = FString::Printf(TEXT("saved %d promoted records to %s"),
                                SortedRecords.Num(), *StorePath);
    return true;
}

bool VF_SaveStrateSeasonManifest(
    const FString& ManifestPath,
    const FVoxelStrateSeasonManifest& Manifest,
    FString& OutReport)
{
    if (ManifestPath.IsEmpty())
    {
        OutReport = TEXT("season manifest path is empty");
        return false;
    }

    TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetStringField(TEXT("format"), TEXT("VoxelForgeSeasonManifest"));
    Root->SetNumberField(TEXT("schema_version"), GPromotionStoreSchemaVersion);
    Root->SetNumberField(TEXT("season"), Manifest.Season);
    Root->SetNumberField(TEXT("seed"), Manifest.Seed);
    Root->SetNumberField(TEXT("input_corpus_hash"),
                         static_cast<double>(Manifest.InputCorpusHash));
    Root->SetNumberField(TEXT("candidate_count"), Manifest.CandidateCount);
    Root->SetNumberField(TEXT("survivor_count"), Manifest.SurvivorCount);
    Root->SetNumberField(TEXT("promoted_count"), Manifest.PromotedCount);
    Root->SetNumberField(TEXT("corpus_size"), Manifest.CorpusSize);
    Root->SetNumberField(TEXT("project_count"), Manifest.ProjectCount);
    Root->SetNumberField(TEXT("default_count"), Manifest.DefaultCount);
    Root->SetNumberField(TEXT("promoted_corpus_count"), Manifest.PromotedCorpusCount);
    Root->SetNumberField(TEXT("survival_rate"), Manifest.SurvivalRate);
    Root->SetNumberField(TEXT("measured_spread"), Manifest.MeasuredSpread);

    FString Error;
    if (!VF_SaveJsonObject(ManifestPath, Root, Error))
    {
        OutReport = Error;
        return false;
    }
    OutReport = FString::Printf(TEXT("saved season %d manifest to %s"),
                                Manifest.Season, *ManifestPath);
    return true;
}

bool FVoxelStrateCorpus::LoadPromotedRecords(
    const FString& StorePath,
    const IVoxelStratePromotionVerifier* PromotionVerifier,
    FString& OutReport)
{
    if (StorePath.IsEmpty())
    {
        OutReport = TEXT("no promotion store path supplied; 0 promoted members loaded");
        return false;
    }
    if (!IFileManager::Get().FileExists(*StorePath))
    {
        OutReport = FString::Printf(TEXT("%s is absent; 0 promoted members loaded"), *StorePath);
        return true;
    }

    TSharedPtr<FJsonObject> Root;
    FString JsonError;
    if (!VF_LoadJsonObject(StorePath, Root, JsonError))
    {
        OutReport = JsonError;
        return false;
    }

    int32 SchemaVersion = 0;
    int32 StoredRecordCount = 0;
    const TArray<TSharedPtr<FJsonValue>>* JsonRecords = nullptr;
    if (!VF_ReadJsonInt32(Root, TEXT("schema_version"), SchemaVersion)
        || SchemaVersion != GPromotionStoreSchemaVersion
        || !VF_ReadJsonInt32(Root, TEXT("record_count"), StoredRecordCount)
        || !VF_ReadJsonArray(Root, TEXT("records"), JsonRecords))
    {
        OutReport = FString::Printf(TEXT("%s has unsupported or malformed promotion-store schema"),
                                    *StorePath);
        return false;
    }
    if (StoredRecordCount != JsonRecords->Num())
    {
        OutReport = FString::Printf(
            TEXT("%s has record_count=%d but contains %d records"),
            *StorePath, StoredRecordCount, JsonRecords->Num());
        return false;
    }

    if (PromotionVerifier == nullptr)
    {
        // A stored metric is evidence for a human, never an admission ticket. Without a
        // world-specific verifier there is no safe way to establish the primordial law, so this
        // compatibility call intentionally leaves the base corpus unchanged.
        OutReport = FString::Printf(
            TEXT("%s contains %d records; verifier was null, so all promoted records were skipped"),
            *StorePath, JsonRecords->Num());
        return true;
    }

    int32 NumLoaded = 0;
    int32 NumMalformed = 0;
    int32 NumRejected = 0;
    int32 NumMetricReplaced = 0;
    TSet<FString> SeenRecordIds;
    for (const TSharedPtr<FJsonValue>& JsonValue : *JsonRecords)
    {
        if (!JsonValue.IsValid() || JsonValue->Type != EJson::Object)
        {
            ++NumMalformed;
            continue;
        }

        FVoxelStratePromotableRecord Record;
        if (!VF_DeserializePromotableRecord(JsonValue->AsObject(), Record)
            || SeenRecordIds.Contains(Record.RecordId))
        {
            ++NumMalformed;
            continue;
        }
        SeenRecordIds.Add(Record.RecordId);

        FVoxelStratePromotionVerification Verification;
        if (!PromotionVerifier->Verify(Record, Verification)
            || !Verification.PassedAllGates())
        {
            ++NumRejected;
            continue;
        }

        if (!VF_MeasuredMetricsExactlyEqual(Record.MeasuredMetrics,
                                            Verification.MeasuredMetrics))
        {
            ++NumMetricReplaced;
        }
        const FString SourcePath = FString::Printf(
            TEXT("/VoxelForge/Promoted/%s"), *Record.RecordId);
        if (!AddEntryInternal(SourcePath, Record.RecordId, Record.Archetype, Record.Params, 1.0f,
                              EVoxelStrateCorpusProvenance::Promoted,
                              &Record.Recipe, &Verification.MeasuredMetrics,
                              Record.RecordId, Record.Season, Record.Seed,
                              Record.CandidateIndex, Record.InputCorpusHash, false))
        {
            ++NumRejected;
            continue;
        }
        ++NumLoaded;
    }

    RebuildSpreads();
    OutReport = FString::Printf(
        TEXT("%s: %d records re-verified; %d promoted members loaded; %d rejected by fresh gates; "
             "%d malformed/duplicate; stored metrics replaced for %d records"),
        *StorePath, NumLoaded + NumRejected, NumLoaded, NumRejected, NumMalformed,
        NumMetricReplaced);
    return true;
}

bool FVoxelStrateCorpus::SetMeasuredMetrics(
    const FString& SourcePath,
    const FVoxelStrateMeasuredMetrics& Metrics)
{
    for (FVoxelStrateCorpusEntry& Entry : Entries)
    {
        if (Entry.SourcePath == SourcePath)
        {
            Entry.MeasuredMetrics = Metrics;
            Entry.bHasMeasuredMetrics = Metrics.bValid;
            return true;
        }
    }
    return false;
}
#endif // WITH_EDITOR — corpus loading, promotion and parameter/structure roll implementation

void VF_SetStrateArchetypeRuntimeBounds(
    FVoxelStrateArchetypeParams& Params, float TopWorldZ, float BottomWorldZ)
{
    Params.TunnelNetworkParams.StrateTopWorldZ = TopWorldZ;
    Params.TunnelNetworkParams.StrateBottomWorldZ = BottomWorldZ;
    Params.SlabParams.StrateTopWorldZ = TopWorldZ;
    Params.SlabParams.StrateBottomWorldZ = BottomWorldZ;
    Params.MazeParams.StrateTopWorldZ = TopWorldZ;
    Params.MazeParams.StrateBottomWorldZ = BottomWorldZ;
    Params.SurfaceParams.StrateTopWorldZ = TopWorldZ;
    Params.SurfaceParams.StrateBottomWorldZ = BottomWorldZ;
    Params.VerticalShaftParams.StrateTopWorldZ = TopWorldZ;
    Params.VerticalShaftParams.StrateBottomWorldZ = BottomWorldZ;
    Params.FloatingIslandParams.StrateTopWorldZ = TopWorldZ;
    Params.FloatingIslandParams.StrateBottomWorldZ = BottomWorldZ;
}

namespace VoxelStrateRegionPrivate
{
    constexpr uint32 PartitionSalt = 0x5245474Eu;       // "REGN"
    constexpr uint32 PositionSaltX = 0x584F4646u;        // "XOFF"
    constexpr uint32 PositionSaltY = 0x594F4646u;        // "YOFF"
    constexpr uint32 RegionSalt = 0x52494458u;           // "RIDX"

    static int32 CellAt(float World, float CellSize)
    {
        return FMath::FloorToInt(World / CellSize);
    }

    static FVoxelStrateRegionSite MakeSite(int32 CellX, int32 CellY,
                                           int32 InRegionCount, uint32 InPartitionSeed,
                                           float CellSize)
    {
        FVoxelStrateRegionSite Site;
        Site.CellX = CellX;
        Site.CellY = CellY;

        const uint32 H = VoxelHash::Cell(CellX, CellY, InPartitionSeed ^ PartitionSalt);
        const float JitterX = VoxelHash::ToFloatSigned(
            VoxelHash::Mix(H ^ PositionSaltX)) * 0.35f;
        const float JitterY = VoxelHash::ToFloatSigned(
            VoxelHash::Mix(H ^ PositionSaltY)) * 0.35f;
        Site.WorldX = (static_cast<float>(CellX) + 0.5f + JitterX) * CellSize;
        Site.WorldY = (static_cast<float>(CellY) + 0.5f + JitterY) * CellSize;

        const uint32 RegionHash = VoxelHash::Cell(
            CellX, CellY, InPartitionSeed ^ RegionSalt);
        Site.Region = InRegionCount > 1
            ? static_cast<int32>(RegionHash % static_cast<uint32>(InRegionCount)) : 0;
        return Site;
    }

    static void GatherSites(float WorldX, float WorldY, int32 InRegionCount,
                            uint32 InPartitionSeed, float CellSize,
                            TArray<FVoxelStrateRegionSite>& OutSites)
    {
        OutSites.Reset();
        if (!(CellSize > 0.0f) || !FMath::IsFinite(CellSize))
        {
            return;
        }

        const int32 CenterX = CellAt(WorldX, CellSize);
        const int32 CenterY = CellAt(WorldY, CellSize);
        OutSites.Reserve(25);
        for (int32 DY = -2; DY <= 2; ++DY)
        {
            for (int32 DX = -2; DX <= 2; ++DX)
            {
                OutSites.Add(MakeSite(CenterX + DX, CenterY + DY,
                                      InRegionCount, InPartitionSeed, CellSize));
            }
        }
    }

    static bool IsEarlierSite(const FVoxelStrateRegionSite& A,
                              const FVoxelStrateRegionSite& B)
    {
        if (A.CellX != B.CellX) { return A.CellX < B.CellX; }
        return A.CellY < B.CellY;
    }

    static FVoxelStrateRegionQuery QuerySites(
        const TArray<FVoxelStrateRegionSite>& Sites, int32 InRegionCount,
        float BlendWidth, float CellSize, float WorldX, float WorldY)
    {
        FVoxelStrateRegionQuery Result;
        if (InRegionCount <= 1 || Sites.Num() == 0)
        {
            return Result;
        }
        Result.NearestDifferentRegionGap = 0.0f;
        const int32 CenterCellX = CellAt(WorldX, CellSize);
        const int32 CenterCellY = CellAt(WorldY, CellSize);

        int32 PrimaryIndex = INDEX_NONE;
        int32 NeighborIndex = INDEX_NONE;
        float PrimaryDistanceSq = FLT_MAX;
        float NeighborDistanceSq = FLT_MAX;
        for (int32 Index = 0; Index < Sites.Num(); ++Index)
        {
            const FVoxelStrateRegionSite& Site = Sites[Index];
            if (FMath::Abs(Site.CellX - CenterCellX) > 2
                || FMath::Abs(Site.CellY - CenterCellY) > 2)
            {
                continue;
            }
            const float DX = WorldX - Site.WorldX;
            const float DY = WorldY - Site.WorldY;
            const float DistanceSq = DX * DX + DY * DY;
            if (PrimaryIndex == INDEX_NONE
                || DistanceSq < PrimaryDistanceSq
                || (DistanceSq == PrimaryDistanceSq
                    && IsEarlierSite(Site, Sites[PrimaryIndex])))
            {
                PrimaryIndex = Index;
                PrimaryDistanceSq = DistanceSq;
            }
        }

        const int32 PrimaryRegion = Sites[PrimaryIndex].Region;
        for (int32 Index = 0; Index < Sites.Num(); ++Index)
        {
            if (FMath::Abs(Sites[Index].CellX - CenterCellX) > 2
                || FMath::Abs(Sites[Index].CellY - CenterCellY) > 2)
            {
                continue;
            }
            if (Sites[Index].Region == PrimaryRegion) { continue; }
            const float DX = WorldX - Sites[Index].WorldX;
            const float DY = WorldY - Sites[Index].WorldY;
            const float DistanceSq = DX * DX + DY * DY;
            if (NeighborIndex == INDEX_NONE
                || DistanceSq < NeighborDistanceSq
                || (DistanceSq == NeighborDistanceSq
                    && IsEarlierSite(Sites[Index], Sites[NeighborIndex])))
            {
                NeighborIndex = Index;
                NeighborDistanceSq = DistanceSq;
            }
        }

        Result.PrimaryRegion = PrimaryRegion;
        if (NeighborIndex == INDEX_NONE)
        {
            return Result;
        }

        const float NeighborDistance = FMath::Sqrt(FMath::Max(NeighborDistanceSq, 0.0f));
        const float Gap = FMath::Max(
            NeighborDistance
                - FMath::Sqrt(FMath::Max(PrimaryDistanceSq, 0.0f)), 0.0f);
        Result.NeighborRegion = Sites[NeighborIndex].Region;
        Result.NearestDifferentRegionGap = Gap;

        // A site outside the ±2-cell window is at least 2.15 cell lengths from a point in the
        // center cell (site jitter is bounded to ±0.35 around the cell midpoint).  A candidate
        // within 2.0 cell lengths is therefore known to beat every omitted site.  Otherwise the
        // density path can conservatively use the candidate/zero weight, but box proof must not
        // treat the local window as an infinite search.
        Result.bNearestDifferentRegionKnown = FMath::IsFinite(CellSize)
            && CellSize > 0.0f
            && NeighborDistance <= 2.0f * CellSize;

        if (!(BlendWidth > 0.0f))
        {
            return Result;
        }

        // The vertical transition is linear from 0 at the outer edge to 1 at the boundary.  A
        // Voronoi boundary is shared by two regions, so the same curve is split symmetrically:
        // each side contributes 0.5 at the bisector and 0 outside the band.
        const float Curve = FMath::Clamp(1.0f - Gap / BlendWidth, 0.0f, 1.0f);
        Result.NeighborWeight = 0.5f * Curve;
        Result.bInBlendBand = Result.NeighborWeight > 0.0f;
        return Result;
    }

    static FVoxelStrateRegionQuery QueryWithGather(
        float WorldX, float WorldY, int32 InRegionCount, uint32 InPartitionSeed,
        float CellSize, float BlendWidth)
    {
        TArray<FVoxelStrateRegionSite> Sites;
        GatherSites(WorldX, WorldY, InRegionCount, InPartitionSeed, CellSize, Sites);
        return QuerySites(Sites, InRegionCount, BlendWidth, CellSize, WorldX, WorldY);
    }
}

#if WITH_EDITOR
int32 VF_RollStrateRegionCount(int32 Seed, int32 StrateIndex)
{
    return 1 + static_cast<int32>(VoxelHash::Cell(
        StrateIndex, StrateIndex ^ 0x6D, static_cast<uint32>(Seed) ^ 0x524F4C4Cu) % 3u);
}
#endif

uint32 VF_GetStrateRegionPartitionSeed(int32 Seed, int32 StrateIndex)
{
    return VoxelHash::Cell(StrateIndex, StrateIndex ^ 0x6D,
                           static_cast<uint32>(Seed) ^ 0x52454750u);
}

void VF_RekeyStrateRegionManifest(
    FVoxelStrateRegionManifest& Manifest, int32 InSeed, int32 InStrateIndex)
{
    Manifest.Seed = InSeed;
    Manifest.StrateIndex = InStrateIndex;
    Manifest.PartitionSeed = VF_GetStrateRegionPartitionSeed(InSeed, InStrateIndex);
}

FVoxelStrateRegionQuery VF_QueryStrateRegion(
    const FVoxelStrateRegionManifest& Manifest, float WorldX, float WorldY)
{
    // The retained seed is an offline artifact, not an alternate identity. Reject a stale or
    // hand-edited partition key instead of silently evaluating a different world for the same
    // (seed, strate-index) pair.
    if (!Manifest.IsValid()
        || Manifest.PartitionSeed != VF_GetStrateRegionPartitionSeed(
            Manifest.Seed, Manifest.StrateIndex))
    {
        return FVoxelStrateRegionQuery();
    }
    return VoxelStrateRegionPrivate::QueryWithGather(
        WorldX, WorldY, Manifest.RegionCount, Manifest.PartitionSeed,
        Manifest.LatticeCellSize, Manifest.BlendWidth);
}

void FVoxelStrateRegionPartitionCache::PrepareForChunk(
    const FVoxelStrateRegionManifest& Manifest, const FIntVector& ChunkCoord)
{
    RegionCount = Manifest.RegionCount;
    PartitionSeed = Manifest.PartitionSeed;
    LatticeCellSize = Manifest.LatticeCellSize;
    BlendWidth = Manifest.BlendWidth;
    BaseX = ChunkCoord.X * CHUNK_SIZE - 1;
    BaseY = ChunkCoord.Y * CHUNK_SIZE - 1;
    Dim = CHUNK_SIZE + 3;
    Sites.Reset();
    IntegerSamples.Reset();

    if (!Manifest.IsValid()
        || Manifest.PartitionSeed != VF_GetStrateRegionPartitionSeed(
            Manifest.Seed, Manifest.StrateIndex))
    {
        RegionCount = 1;
        Dim = 0;
        return;
    }

    const int32 MinCellX = VoxelStrateRegionPrivate::CellAt(
        static_cast<float>(BaseX), LatticeCellSize) - 2;
    const int32 MaxCellX = VoxelStrateRegionPrivate::CellAt(
        static_cast<float>(BaseX + Dim - 1), LatticeCellSize) + 2;
    const int32 MinCellY = VoxelStrateRegionPrivate::CellAt(
        static_cast<float>(BaseY), LatticeCellSize) - 2;
    const int32 MaxCellY = VoxelStrateRegionPrivate::CellAt(
        static_cast<float>(BaseY + Dim - 1), LatticeCellSize) + 2;
    Sites.Reserve((MaxCellX - MinCellX + 1) * (MaxCellY - MinCellY + 1));
    for (int32 CY = MinCellY; CY <= MaxCellY; ++CY)
    {
        for (int32 CX = MinCellX; CX <= MaxCellX; ++CX)
        {
            Sites.Add(VoxelStrateRegionPrivate::MakeSite(
                CX, CY, RegionCount, PartitionSeed, LatticeCellSize));
        }
    }

    IntegerSamples.SetNum(Dim * Dim);
    for (int32 Y = 0; Y < Dim; ++Y)
    {
        for (int32 X = 0; X < Dim; ++X)
        {
            IntegerSamples[Y * Dim + X] = VoxelStrateRegionPrivate::QuerySites(
                Sites, RegionCount, BlendWidth, LatticeCellSize,
                static_cast<float>(BaseX + X), static_cast<float>(BaseY + Y));
        }
    }
}

FVoxelStrateRegionQuery FVoxelStrateRegionPartitionCache::Query(
    float WorldX, float WorldY) const
{
    if (Dim > 0
        && WorldX == FMath::FloorToFloat(WorldX)
        && WorldY == FMath::FloorToFloat(WorldY))
    {
        const int32 IX = FMath::FloorToInt(WorldX);
        const int32 IY = FMath::FloorToInt(WorldY);
        const int32 LocalX = IX - BaseX;
        const int32 LocalY = IY - BaseY;
        if (LocalX >= 0 && LocalX < Dim && LocalY >= 0 && LocalY < Dim)
        {
            return IntegerSamples[LocalY * Dim + LocalX];
        }
    }

    // The production stack is prepared for the current chunk before its first sample.  Reuse the
    // prepared local site set for the rare fractional probe as well; rebuilding a lattice window
    // here would turn a cache miss into a per-voxel allocation/hash walk.  The prepared set has a
    // one-voxel halo, which covers the mesher's gradient probes.  The pure public query remains the
    // fallback for an intentionally out-of-window diagnostic call.
    const float PreparedMaxX = static_cast<float>(BaseX + Dim - 1);
    const float PreparedMaxY = static_cast<float>(BaseY + Dim - 1);
    if (Sites.Num() > 0 && Dim > 0
        && WorldX >= static_cast<float>(BaseX) && WorldX <= PreparedMaxX
        && WorldY >= static_cast<float>(BaseY) && WorldY <= PreparedMaxY)
    {
        return VoxelStrateRegionPrivate::QuerySites(
            Sites, RegionCount, BlendWidth, LatticeCellSize, WorldX, WorldY);
    }
    return VoxelStrateRegionPrivate::QueryWithGather(
        WorldX, WorldY, RegionCount, PartitionSeed,
        LatticeCellSize, BlendWidth);
}

FVoxelStrateRegionBoxProof VF_AnalyzeStrateRegionBox(
    const FVoxelStrateRegionManifest& Manifest, const FBox& VoxelBox)
{
    FVoxelStrateRegionBoxProof Proof;
    if (!Manifest.IsValid()
        || Manifest.PartitionSeed != VF_GetStrateRegionPartitionSeed(
            Manifest.Seed, Manifest.StrateIndex))
    {
        return Proof;
    }
    if (Manifest.RegionCount <= 1)
    {
        Proof.bProvablySingleRegion = true;
        Proof.bTouchesRegionBoundary = false;
        Proof.bTouchesBlendBand = false;
        return Proof;
    }

    const float HalfX = FMath::Max((float)(VoxelBox.Max.X - VoxelBox.Min.X) * 0.5f, 0.0f);
    const float HalfY = FMath::Max((float)(VoxelBox.Max.Y - VoxelBox.Min.Y) * 0.5f, 0.0f);
    const float Radius = FMath::Sqrt(HalfX * HalfX + HalfY * HalfY);
    const float CenterX = ((float)VoxelBox.Min.X + (float)VoxelBox.Max.X) * 0.5f;
    const float CenterY = ((float)VoxelBox.Min.Y + (float)VoxelBox.Max.Y) * 0.5f;
    const FVoxelStrateRegionQuery Center = VF_QueryStrateRegion(Manifest, CenterX, CenterY);
    if (!Center.bNearestDifferentRegionKnown)
    {
        return Proof;
    }
    const float GapLowerBound = Center.NearestDifferentRegionGap - 2.0f * Radius;

    // The difference of two Euclidean distances is 2-Lipschitz.  Strict inequalities are
    // intentional: an equality may put a sample exactly on a bisector or at the blend cutoff,
    // and that is not a proof a mesher is allowed to skip.
    Proof.bTouchesRegionBoundary = !(GapLowerBound > 0.0f);
    Proof.bTouchesBlendBand = !(GapLowerBound > Manifest.BlendWidth);
    Proof.bProvablySingleRegion = GapLowerBound > Manifest.BlendWidth;
    return Proof;
}

#if WITH_EDITOR
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

FVoxelStrateRollInfo VF_RollStrateParamsCorpusFree(
    ECaveGeneratorType Archetype, int32 Seed, int32 Index,
    EVoxelStrateCorpusFreeSamplingMode Mode, float StrateHeightInVoxels)
{
    const FVoxelStrateRollInfo Result = VF_RollCorpusFreeInternal(
        Archetype, Seed, Index, Mode, StrateHeightInVoxels);

#if DO_CHECK
    if (Result.bValid)
    {
        const FVoxelStrateRollInfo Repeat = VF_RollCorpusFreeInternal(
            Archetype, Seed, Index, Mode, StrateHeightInVoxels);
        checkf(Repeat.bValid && Repeat.Archetype == Result.Archetype
                   && VF_AreStrateArchetypeParamsBitIdentical(
                       Repeat.ArchetypeParams, Result.ArchetypeParams, Archetype),
               TEXT("VF_RollStrateParamsCorpusFree lost determinism."));
    }
#endif
    return Result;
}

bool VF_ValidateStrateCorpusFreeConstraints(
    ECaveGeneratorType Archetype,
    const FVoxelStrateArchetypeParams& Params,
    float StrateHeightInVoxels,
    FString& OutViolation)
{
    OutViolation.Empty();
    if (!VF_IsSupportedArchetype(Archetype)
        || !FMath::IsFinite(StrateHeightInVoxels) || StrateHeightInVoxels <= 0.0f)
    {
        OutViolation = TEXT("unsupported archetype or non-positive strate height");
        return false;
    }

    const float H = FMath::Max(StrateHeightInVoxels, static_cast<float>(CHUNK_SIZE));
    auto Fail = [&OutViolation](const TCHAR* Relation, float Left, float Right) -> bool
    {
        return VF_FailCorpusFreeConstraint(OutViolation, Relation, Left, Right);
    };

    switch (Archetype)
    {
    case ECaveGeneratorType::TunnelNetwork:
    case ECaveGeneratorType::Underwater:
    {
        const FStrateGenerationParams& P = Params.TunnelNetworkParams;
        if (!FMath::IsFinite(P.BaseDensity) || P.BaseDensity <= 0.0f)
        {
            return Fail(TEXT("BaseDensity must be > 0"), P.BaseDensity, 0.0f);
        }
        if (!FMath::IsFinite(P.RoomSpacing) || P.RoomSpacing <= 0.0f)
        {
            return Fail(TEXT("RoomSpacing must be > 0"), P.RoomSpacing, 0.0f);
        }
        if (!FMath::IsFinite(P.RoomDensity) || P.RoomDensity <= 0.0f
            || P.RoomDensity > 1.0f)
        {
            return Fail(TEXT("RoomDensity must be in (0,1]"), P.RoomDensity, 1.0f);
        }
        if (!FMath::IsFinite(P.MinRoomRadius) || !FMath::IsFinite(P.MaxRoomRadius)
            || P.MinRoomRadius <= 0.0f || P.MaxRoomRadius < P.MinRoomRadius)
        {
            return Fail(TEXT("room radius interval is ordered and positive"),
                        P.MinRoomRadius, P.MaxRoomRadius);
        }
        if (!FMath::IsFinite(P.RoomHeightRatio) || P.RoomHeightRatio <= 0.0f
            || P.RoomHeightRatio > 1.0f)
        {
            return Fail(TEXT("RoomHeightRatio must be in (0,1]"), P.RoomHeightRatio, 1.0f);
        }
        if (P.RoomFloorCutMin < 0.0f || P.RoomFloorCutMax < P.RoomFloorCutMin
            || P.RoomFloorCutMax > 1.0f)
        {
            return Fail(TEXT("room floor-cut interval is ordered in [0,1]"),
                        P.RoomFloorCutMin, P.RoomFloorCutMax);
        }
        if (P.TunnelMinRadius <= 0.0f || P.TunnelMaxRadius < P.TunnelMinRadius)
        {
            return Fail(TEXT("tunnel radius interval is ordered and positive"),
                        P.TunnelMinRadius, P.TunnelMaxRadius);
        }
        if (P.TunnelMaxRadius > P.MinRoomRadius)
        {
            return Fail(TEXT("TunnelMaxRadius must fit through the smallest room"),
                        P.TunnelMaxRadius, P.MinRoomRadius);
        }
        const float RoomCoverage = PI * P.MaxRoomRadius * P.MaxRoomRadius
            * P.RoomDensity / (P.RoomSpacing * P.RoomSpacing);
        if (RoomCoverage < 0.02f || RoomCoverage > 0.35f)
        {
            return Fail(TEXT("room area coverage must stay in [0.02,0.35]"),
                        RoomCoverage, 0.35f);
        }
        if (P.MaxRoomRadius > P.RoomSpacing * 0.50f)
        {
            return Fail(TEXT("MaxRoomRadius must be <= half the room-cell spacing"),
                        P.MaxRoomRadius, P.RoomSpacing * 0.50f);
        }
        const float LargestRoomRadius = FMath::Max(P.MaxRoomRadius, P.OriginRoomRadius);
        const float RoomZBuffer = LargestRoomRadius * P.RoomHeightRatio;
        if (P.BoundarySealThickness < 0.0f
            || P.BoundarySealThickness + RoomZBuffer >= H * 0.50f)
        {
            return Fail(TEXT("seal + tallest room half-height must fit in half the strate"),
                        P.BoundarySealThickness + RoomZBuffer, H * 0.50f);
        }
        const float HorizontalReach = 1.7f * P.RoomSpacing * FMath::Sqrt(2.0f);
        const float VerticalReach = 2.0f * P.MaxRoomRadius * P.RoomHeightRatio;
        const float RequiredTunnelReach = FMath::Sqrt(
            HorizontalReach * HorizontalReach + VerticalReach * VerticalReach);
        if (P.MaxTunnelLength < RequiredTunnelReach)
        {
            return Fail(TEXT("MaxTunnelLength must reach a jittered neighbouring room"),
                        P.MaxTunnelLength, RequiredTunnelReach);
        }
        if (P.WormStrength <= P.BaseDensity)
        {
            return Fail(TEXT("WormStrength must overcome BaseDensity"),
                        P.WormStrength, P.BaseDensity);
        }
        if (P.DomeMaxRadius > P.MaxRoomRadius)
        {
            return Fail(TEXT("DomeMaxRadius must fit inside MaxRoomRadius"),
                        P.DomeMaxRadius, P.MaxRoomRadius);
        }
        if (P.DomeHeightRatio * P.DomeMaxRadius
            > P.RoomHeightRatio * P.MaxRoomRadius)
        {
            return Fail(TEXT("dome height must fit the room vertical radius"),
                        P.DomeHeightRatio * P.DomeMaxRadius,
                        P.RoomHeightRatio * P.MaxRoomRadius);
        }
        return true;
    }

    case ECaveGeneratorType::FlatPlain:
    case ECaveGeneratorType::CrystalChamber:
    {
        const FSlabGenerationParams& P = Params.SlabParams;
        const float FloorWorst = H * P.FloorRelativeHeight
            + VOXEL_NOISE_SCALE * FMath::Max(P.FloorRoughness, 0.0f);
        const float CeilingWorst = H * P.CeilingRelativeHeight
            - VOXEL_NOISE_SCALE * FMath::Max(P.CeilingRoughness, 0.0f);
        if (P.FloorRelativeHeight < 0.0f || P.CeilingRelativeHeight > 1.0f
            || P.FloorRelativeHeight >= P.CeilingRelativeHeight)
        {
            return Fail(TEXT("slab floor must be below slab ceiling"),
                        P.FloorRelativeHeight, P.CeilingRelativeHeight);
        }
        if (P.BoundarySealThickness < 0.0f
            || FloorWorst <= P.BoundarySealThickness
            || CeilingWorst >= H - P.BoundarySealThickness)
        {
            return Fail(TEXT("slab roughness must stay inside the seal bands"),
                        FloorWorst, CeilingWorst);
        }
        if (FloorWorst + 2.0f >= CeilingWorst)
        {
            return Fail(TEXT("slab worst-case noise must leave two voxels of void"),
                        FloorWorst + 2.0f, CeilingWorst);
        }
        if (P.ColumnMinRadius <= 0.0f || P.ColumnMaxRadius < P.ColumnMinRadius)
        {
            return Fail(TEXT("slab column radius interval is ordered and positive"),
                        P.ColumnMinRadius, P.ColumnMaxRadius);
        }
        if (P.ColumnSpacing <= 0.0f || P.ColumnMaxRadius > P.ColumnSpacing * 0.50f)
        {
            return Fail(TEXT("slab columns must fit within half a column cell"),
                        P.ColumnMaxRadius, P.ColumnSpacing * 0.50f);
        }
        if (P.BaseDensity <= 0.0f)
        {
            return Fail(TEXT("BaseDensity must be > 0"), P.BaseDensity, 0.0f);
        }
        return true;
    }

    case ECaveGeneratorType::Maze:
    {
        const FMazeGenerationParams& P = Params.MazeParams;
        if (P.CellSize <= 0.0f || P.CorridorRadius <= 0.0f)
        {
            return Fail(TEXT("maze CellSize and CorridorRadius must be > 0"),
                        P.CellSize, P.CorridorRadius);
        }
        if (P.CorridorRadius > P.CellSize * 0.25f)
        {
            return Fail(TEXT("CorridorRadius must leave lattice walls"),
                        P.CorridorRadius, P.CellSize * 0.25f);
        }
        if (P.SurfaceRoughness * VOXEL_NOISE_SCALE
            >= P.CorridorRadius + 2.0f)
        {
            return Fail(TEXT("maze roughness must not erase the corridor centreline"),
                        P.SurfaceRoughness * VOXEL_NOISE_SCALE,
                        P.CorridorRadius + 2.0f);
        }
        if (P.BoundarySealThickness < 0.0f
            || P.BoundarySealThickness * 2.0f >= H)
        {
            return Fail(TEXT("maze seals must leave an interior"),
                        P.BoundarySealThickness * 2.0f, H);
        }
        if (P.BaseDensity <= 0.0f)
        {
            return Fail(TEXT("BaseDensity must be > 0"), P.BaseDensity, 0.0f);
        }
        return true;
    }

    case ECaveGeneratorType::SurfaceWorld:
    {
        const FSurfaceGenerationParams& P = Params.SurfaceParams;
        const float RoughBound = VOXEL_NOISE_SCALE * FMath::Max(P.SurfaceRoughness, 0.0f);
        const float CliffSpread = P.ElevationRange
            * (1.0f + FMath::Max(P.MountainStrength, 0.0f))
            + 2.0f * RoughBound;
        const float CliffBudget = FMath::Max(P.CliffStrength, 0.0f)
            * FMath::Max(P.CliffSharpness, 0.0f) * FMath::Max(CliffSpread, 0.0f);
        const float GroundFeatureBudget = CliffBudget
            + FMath::Max(P.TerraceHeight, 0.0f)
            + FMath::Abs(P.LayerLineDepth)
            + FMath::Max(P.BeachWidth, 0.0f);
        const float GroundLower = H * P.BaseGroundRelative
            - P.ElevationRange * 0.50f - RoughBound - GroundFeatureBudget;
        const float GroundUpper = H * P.BaseGroundRelative
            + P.ElevationRange * (0.50f + FMath::Max(P.MountainStrength, 0.0f))
            + RoughBound + GroundFeatureBudget;
        const float CapLower = H * P.CeilingRelative
            - VOXEL_NOISE_SCALE * FMath::Max(P.CeilingUndulation, 0.0f)
            - VOXEL_NOISE_SCALE * FMath::Max(P.CeilingRoughness, 0.0f)
            - FMath::Max(P.CeilingRidgeStrength, 0.0f);
        if (P.BoundarySealThickness < 0.0f
            || GroundLower <= P.BoundarySealThickness)
        {
            return Fail(TEXT("surface ground lower bound must clear the bottom seal"),
                        GroundLower, P.BoundarySealThickness);
        }
        if (CapLower >= H - P.BoundarySealThickness)
        {
            return Fail(TEXT("surface cap lower bound must stay below the top seal"),
                        CapLower, H - P.BoundarySealThickness);
        }
        if (GroundUpper + 2.0f >= CapLower)
        {
            return Fail(TEXT("surface ground and cap worst-case bounds need two voxels"),
                        GroundUpper + 2.0f, CapLower);
        }
        if (P.OverhangStrength > 0.0f
            && GroundUpper + P.OverhangHeight + 2.0f >= CapLower)
        {
            return Fail(TEXT("surface overhang height must fit below the cap"),
                        GroundUpper + P.OverhangHeight + 2.0f, CapLower);
        }
        if (P.BaseDensity <= 0.0f)
        {
            return Fail(TEXT("BaseDensity must be > 0"), P.BaseDensity, 0.0f);
        }
        return true;
    }

    case ECaveGeneratorType::VerticalShafts:
    {
        const FVerticalShaftParams& P = Params.VerticalShaftParams;
        const float RoughnessReach = FMath::Max(P.SurfaceRoughness, 0.0f)
            * VOXEL_NOISE_SCALE * 1.5f;
        const float FloorClearance = FMath::Max(
            1.0f, P.SurfaceRoughness * VOXEL_NOISE_SCALE + 1.0f);
        if (P.ShaftSpacing <= 0.0f || P.ShaftMinRadius <= 0.0f
            || P.ShaftMaxRadius < P.ShaftMinRadius)
        {
            return Fail(TEXT("shaft spacing/radius values must be ordered and positive"),
                        P.ShaftMinRadius, P.ShaftMaxRadius);
        }
        if (P.ShaftMaxRadius > P.ShaftSpacing * 0.50f)
        {
            return Fail(TEXT("shaft radius must leave the neighbouring cell wall"),
                        P.ShaftMaxRadius, P.ShaftSpacing * 0.50f);
        }
        if (P.ShaftMinRadius <= RoughnessReach + 0.25f)
        {
            return Fail(TEXT("shaft radius must exceed the landing roughness margin"),
                        P.ShaftMinRadius, RoughnessReach + 0.25f);
        }
        if (P.ConnectorRadius < RoughnessReach + 1.0f)
        {
            return Fail(TEXT("ConnectorRadius must exceed the tree roughness margin"),
                        P.ConnectorRadius, RoughnessReach + 1.0f);
        }
        if (P.LedgeSpacing > 0.0f && P.LedgeDepth > 0.0f
            && P.LedgeSpacing <= 2.0f * (P.LedgeDepth + FloorClearance))
        {
            return Fail(TEXT("ledge spacing must leave a safe band on both sides"),
                        P.LedgeSpacing, 2.0f * (P.LedgeDepth + FloorClearance));
        }
        if (P.BoundarySealThickness < 0.0f
            || P.BoundarySealThickness * 2.0f >= H)
        {
            return Fail(TEXT("shaft seals must leave an interior"),
                        P.BoundarySealThickness * 2.0f, H);
        }
        if (P.BaseDensity <= 0.0f)
        {
            return Fail(TEXT("BaseDensity must be > 0"), P.BaseDensity, 0.0f);
        }
        return true;
    }

    case ECaveGeneratorType::FloatingIslands:
    {
        const FFloatingIslandParams& P = Params.FloatingIslandParams;
        const float UnderDepth = P.IslandMaxRadius * FMath::Max(P.ThicknessRatio, 0.25f);
        const float TopHalf = P.IslandMaxRadius * 0.20f;
        const float MaxIslandHalfHeight = FMath::Max(TopHalf, UnderDepth);
        if (P.IslandSpacing <= 0.0f || P.IslandMinRadius <= 0.0f
            || P.IslandMaxRadius < P.IslandMinRadius)
        {
            return Fail(TEXT("island spacing/radius values must be ordered and positive"),
                        P.IslandMinRadius, P.IslandMaxRadius);
        }
        if (P.IslandMaxRadius > P.IslandSpacing * 0.50f)
        {
            return Fail(TEXT("island radius must leave a cell-scale separation budget"),
                        P.IslandMaxRadius, P.IslandSpacing * 0.50f);
        }
        if (P.BoundarySealThickness < 0.0f
            || P.BoundarySealThickness + MaxIslandHalfHeight >= H * 0.50f)
        {
            return Fail(TEXT("island thickness + seal must fit in half the strate"),
                        P.BoundarySealThickness + MaxIslandHalfHeight, H * 0.50f);
        }
        if (P.IslandMinRadius
            <= P.SurfaceRoughness * VOXEL_NOISE_SCALE + P.SDFBlendRadius)
        {
            return Fail(TEXT("island minimum radius must survive roughness and SDF blend"),
                        P.IslandMinRadius,
                        P.SurfaceRoughness * VOXEL_NOISE_SCALE + P.SDFBlendRadius);
        }
        if (P.BaseDensity <= 0.0f)
        {
            return Fail(TEXT("BaseDensity must be > 0"), P.BaseDensity, 0.0f);
        }
        return true;
    }

    default:
        break;
    }
    OutViolation = TEXT("no constraint rule for archetype");
    return false;
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

namespace VoxelStrateRegionPrivate
{
    static ECaveGeneratorType ArchetypeForRecipe(const FVoxelOpStackRecipe& Recipe)
    {
        switch (Recipe.StructuralParamBlock)
        {
        case EVoxelStrateParamBlock::TunnelNetwork: return ECaveGeneratorType::TunnelNetwork;
        case EVoxelStrateParamBlock::Slab:           return ECaveGeneratorType::FlatPlain;
        case EVoxelStrateParamBlock::Maze:           return ECaveGeneratorType::Maze;
        case EVoxelStrateParamBlock::Surface:        return ECaveGeneratorType::SurfaceWorld;
        case EVoxelStrateParamBlock::VerticalShaft:  return ECaveGeneratorType::VerticalShafts;
        case EVoxelStrateParamBlock::FloatingIsland: return ECaveGeneratorType::FloatingIslands;
        default:                                     return ECaveGeneratorType::TunnelNetwork;
        }
    }

    static EVoxelStrateParamBlock ParamBlockForArchetype(ECaveGeneratorType Archetype)
    {
        switch (Archetype)
        {
        case ECaveGeneratorType::FlatPlain:
        case ECaveGeneratorType::CrystalChamber: return EVoxelStrateParamBlock::Slab;
        case ECaveGeneratorType::Maze:            return EVoxelStrateParamBlock::Maze;
        case ECaveGeneratorType::SurfaceWorld:   return EVoxelStrateParamBlock::Surface;
        case ECaveGeneratorType::VerticalShafts: return EVoxelStrateParamBlock::VerticalShaft;
        case ECaveGeneratorType::FloatingIslands:return EVoxelStrateParamBlock::FloatingIsland;
        case ECaveGeneratorType::Underwater:
        case ECaveGeneratorType::TunnelNetwork:
        default:                                  return EVoxelStrateParamBlock::TunnelNetwork;
        }
    }

    static void CopyParamBlock(FVoxelStrateArchetypeParams& OutParams,
                               const FVoxelStrateRollInfo& Roll,
                               ECaveGeneratorType Archetype)
    {
        switch (Archetype)
        {
        case ECaveGeneratorType::TunnelNetwork:
        case ECaveGeneratorType::Underwater:
            OutParams.TunnelNetworkParams = Roll.ArchetypeParams.TunnelNetworkParams;
            break;
        case ECaveGeneratorType::FlatPlain:
        case ECaveGeneratorType::CrystalChamber:
            OutParams.SlabParams = Roll.ArchetypeParams.SlabParams;
            break;
        case ECaveGeneratorType::Maze:
            OutParams.MazeParams = Roll.ArchetypeParams.MazeParams;
            break;
        case ECaveGeneratorType::SurfaceWorld:
            OutParams.SurfaceParams = Roll.ArchetypeParams.SurfaceParams;
            break;
        case ECaveGeneratorType::VerticalShafts:
            OutParams.VerticalShaftParams = Roll.ArchetypeParams.VerticalShaftParams;
            break;
        case ECaveGeneratorType::FloatingIslands:
            OutParams.FloatingIslandParams = Roll.ArchetypeParams.FloatingIslandParams;
            break;
        default:
            break;
        }
    }

    static float BoundarySeal(const FVoxelStrateArchetypeParams& Params,
                              EVoxelStrateParamBlock Block)
    {
        switch (Block)
        {
        case EVoxelStrateParamBlock::Slab:           return Params.SlabParams.BoundarySealThickness;
        case EVoxelStrateParamBlock::Maze:           return Params.MazeParams.BoundarySealThickness;
        case EVoxelStrateParamBlock::Surface:        return Params.SurfaceParams.BoundarySealThickness;
        case EVoxelStrateParamBlock::VerticalShaft:  return Params.VerticalShaftParams.BoundarySealThickness;
        case EVoxelStrateParamBlock::FloatingIsland: return Params.FloatingIslandParams.BoundarySealThickness;
        case EVoxelStrateParamBlock::TunnelNetwork:
        default:                                     return Params.TunnelNetworkParams.BoundarySealThickness;
        }
    }

    static float BaseDensity(const FVoxelStrateArchetypeParams& Params,
                             EVoxelStrateParamBlock Block)
    {
        switch (Block)
        {
        case EVoxelStrateParamBlock::Slab:           return Params.SlabParams.BaseDensity;
        case EVoxelStrateParamBlock::Maze:           return Params.MazeParams.BaseDensity;
        case EVoxelStrateParamBlock::Surface:        return Params.SurfaceParams.BaseDensity;
        case EVoxelStrateParamBlock::VerticalShaft:  return Params.VerticalShaftParams.BaseDensity;
        case EVoxelStrateParamBlock::FloatingIsland: return Params.FloatingIslandParams.BaseDensity;
        case EVoxelStrateParamBlock::TunnelNetwork:
        default:                                     return Params.TunnelNetworkParams.BaseDensity;
        }
    }

    static float Top(const FVoxelStrateArchetypeParams& Params,
                     EVoxelStrateParamBlock Block)
    {
        switch (Block)
        {
        case EVoxelStrateParamBlock::Slab:           return Params.SlabParams.StrateTopWorldZ;
        case EVoxelStrateParamBlock::Maze:           return Params.MazeParams.StrateTopWorldZ;
        case EVoxelStrateParamBlock::Surface:        return Params.SurfaceParams.StrateTopWorldZ;
        case EVoxelStrateParamBlock::VerticalShaft:  return Params.VerticalShaftParams.StrateTopWorldZ;
        case EVoxelStrateParamBlock::FloatingIsland: return Params.FloatingIslandParams.StrateTopWorldZ;
        case EVoxelStrateParamBlock::TunnelNetwork:
        default:                                     return Params.TunnelNetworkParams.StrateTopWorldZ;
        }
    }

    static float Bottom(const FVoxelStrateArchetypeParams& Params,
                        EVoxelStrateParamBlock Block)
    {
        switch (Block)
        {
        case EVoxelStrateParamBlock::Slab:           return Params.SlabParams.StrateBottomWorldZ;
        case EVoxelStrateParamBlock::Maze:           return Params.MazeParams.StrateBottomWorldZ;
        case EVoxelStrateParamBlock::Surface:        return Params.SurfaceParams.StrateBottomWorldZ;
        case EVoxelStrateParamBlock::VerticalShaft:  return Params.VerticalShaftParams.StrateBottomWorldZ;
        case EVoxelStrateParamBlock::FloatingIsland: return Params.FloatingIslandParams.StrateBottomWorldZ;
        case EVoxelStrateParamBlock::TunnelNetwork:
        default:                                     return Params.TunnelNetworkParams.StrateBottomWorldZ;
        }
    }
}

bool VF_LateralRegionsAreShippable()
{
    // ⛔ FALSE ON PURPOSE. See the declaration in VoxelStrateComposer.h for the full reasoning.
    //
    // FR : volontairement désactivé — la loi primordiale ne tient pas encore au travers d'une
    // couture entre régions (9/16 graines mesurées le 2026-09-05).
    //
    // Measured: arrival -> departure across a region seam passes 9/16 seeds. A strate that mixes
    // archetypes but strands the player at the boundary is worse than one that does not mix.
    // Flipping this without an explicit cross-seam corridor contract turns the suite red by design.
    return false;
}

FVoxelStrateRegionManifest VF_RollStrateRegionManifest(
    const FVoxelStrateCorpus& Corpus, int32 Seed, int32 StrateIndex, bool bRollStructure)
{
    FVoxelStrateRegionManifest Manifest;
    Manifest.RegionCount = VF_RollStrateRegionCount(Seed, StrateIndex);
    VF_RekeyStrateRegionManifest(Manifest, Seed, StrateIndex);
    Manifest.Regions.Reserve(Manifest.RegionCount);
    Manifest.bValid = true;

    static const ECaveGeneratorType StructureBlocks[] =
    {
        ECaveGeneratorType::TunnelNetwork,
        ECaveGeneratorType::FlatPlain,
        ECaveGeneratorType::Maze,
        ECaveGeneratorType::SurfaceWorld,
        ECaveGeneratorType::VerticalShafts,
        ECaveGeneratorType::FloatingIslands,
    };
    static const EVoxelStrateParamBlock BlockIds[] =
    {
        EVoxelStrateParamBlock::TunnelNetwork,
        EVoxelStrateParamBlock::Slab,
        EVoxelStrateParamBlock::Maze,
        EVoxelStrateParamBlock::Surface,
        EVoxelStrateParamBlock::VerticalShaft,
        EVoxelStrateParamBlock::FloatingIsland,
    };
    static const uint32 BlockSalts[] = { 0x1001u, 0x1003u, 0x1005u,
                                        0x1007u, 0x1009u, 0x100Bu };

    for (int32 RegionIndex = 0; RegionIndex < Manifest.RegionCount; ++RegionIndex)
    {
        FVoxelStrateRegion& Region = Manifest.Regions.AddDefaulted_GetRef();
        Region.RegionIndex = RegionIndex;
        Region.Seed = RegionIndex == 0
            ? Seed
            : static_cast<int32>(VoxelHash::Cell(
                StrateIndex, RegionIndex, static_cast<uint32>(Seed) ^ 0x52454753u));
        Region.bUsesRecipe = bRollStructure;

        if (!bRollStructure)
        {
            const FVoxelStrateRollInfo Roll = VF_RollStrateParamsDetailed(
                Corpus, Region.Seed, StrateIndex);
            if (!Roll.bValid)
            {
                Manifest.bValid = false;
                if (Manifest.FailureReason.IsEmpty())
                {
                    Manifest.FailureReason = Roll.FailureReason;
                }
                continue;
            }
            Region.Archetype = Roll.Archetype;
            Region.ArchetypeParams = Roll.ArchetypeParams;
        }
        else
        {
            Region.Recipe = VF_RollStrateStructure(Region.Seed, StrateIndex);
            Region.Archetype = VoxelStrateRegionPrivate::ArchetypeForRecipe(Region.Recipe);
            for (int32 BlockIndex = 0; BlockIndex < UE_ARRAY_COUNT(StructureBlocks); ++BlockIndex)
            {
                const FVoxelStrateRollInfo Roll = VF_RollStrateParamsDetailedForArchetype(
                    Corpus, StructureBlocks[BlockIndex],
                    Region.Seed ^ static_cast<int32>(BlockSalts[BlockIndex]), StrateIndex);
                if (!Roll.bValid)
                {
                    Manifest.bValid = false;
                    if (Manifest.FailureReason.IsEmpty())
                    {
                        Manifest.FailureReason = Roll.FailureReason;
                    }
                    continue;
                }
                VoxelStrateRegionPrivate::CopyParamBlock(
                    Region.ArchetypeParams, Roll, StructureBlocks[BlockIndex]);
            }
        }
    }

    if (Manifest.Regions.Num() == Manifest.RegionCount && Manifest.RegionCount > 0)
    {
        const FVoxelStrateRegion& First = Manifest.Regions[0];
        Manifest.StructuralParamBlock = bRollStructure
            ? First.Recipe.StructuralParamBlock
            : VoxelStrateRegionPrivate::ParamBlockForArchetype(First.Archetype);
        Manifest.StrateTopWorldZ = VoxelStrateRegionPrivate::Top(
            First.ArchetypeParams, Manifest.StructuralParamBlock);
        Manifest.StrateBottomWorldZ = VoxelStrateRegionPrivate::Bottom(
            First.ArchetypeParams, Manifest.StructuralParamBlock);
        Manifest.BoundarySealThickness = VoxelStrateRegionPrivate::BoundarySeal(
            First.ArchetypeParams, Manifest.StructuralParamBlock);
        Manifest.BaseDensity = VoxelStrateRegionPrivate::BaseDensity(
            First.ArchetypeParams, Manifest.StructuralParamBlock);
        Manifest.bHasGlobalStructuralParams = FMath::IsFinite(Manifest.StrateTopWorldZ)
            && FMath::IsFinite(Manifest.StrateBottomWorldZ)
            && Manifest.StrateTopWorldZ > Manifest.StrateBottomWorldZ
            && FMath::IsFinite(Manifest.BoundarySealThickness)
            && FMath::IsFinite(Manifest.BaseDensity);
    }
    return Manifest;
}

#if WITH_EDITOR
namespace
{
    struct FComposerStructureBlockSpec
    {
        EVoxelStrateParamBlock Block;
        ECaveGeneratorType Archetype;
        uint32 SeedSalt;
    };

    // Keep these salts in the composer implementation so the editor action and the offline
    // structure test cannot silently acquire different block streams.
    static const FComposerStructureBlockSpec GComposerStructureBlocks[] =
    {
        { EVoxelStrateParamBlock::TunnelNetwork,  ECaveGeneratorType::TunnelNetwork,  0x1001u },
        { EVoxelStrateParamBlock::Slab,            ECaveGeneratorType::FlatPlain,       0x1003u },
        { EVoxelStrateParamBlock::Maze,            ECaveGeneratorType::Maze,            0x1005u },
        { EVoxelStrateParamBlock::Surface,         ECaveGeneratorType::SurfaceWorld,    0x1007u },
        { EVoxelStrateParamBlock::VerticalShaft,   ECaveGeneratorType::VerticalShafts,  0x1009u },
        { EVoxelStrateParamBlock::FloatingIsland,  ECaveGeneratorType::FloatingIslands, 0x100Bu },
    };

    void VF_CopyComposerBlock(FVoxelStrateArchetypeParams& OutParams,
                              const FVoxelStrateRollInfo& Roll,
                              ECaveGeneratorType Archetype)
    {
        switch (Archetype)
        {
        case ECaveGeneratorType::TunnelNetwork:
        case ECaveGeneratorType::Underwater:
            OutParams.TunnelNetworkParams = Roll.ArchetypeParams.TunnelNetworkParams;
            break;
        case ECaveGeneratorType::FlatPlain:
        case ECaveGeneratorType::CrystalChamber:
            OutParams.SlabParams = Roll.ArchetypeParams.SlabParams;
            break;
        case ECaveGeneratorType::Maze:
            OutParams.MazeParams = Roll.ArchetypeParams.MazeParams;
            break;
        case ECaveGeneratorType::SurfaceWorld:
            OutParams.SurfaceParams = Roll.ArchetypeParams.SurfaceParams;
            break;
        case ECaveGeneratorType::VerticalShafts:
            OutParams.VerticalShaftParams = Roll.ArchetypeParams.VerticalShaftParams;
            break;
        case ECaveGeneratorType::FloatingIslands:
            OutParams.FloatingIslandParams = Roll.ArchetypeParams.FloatingIslandParams;
            break;
        default:
            break;
        }
    }

    ECaveGeneratorType VF_ComposerArchetypeForRecipe(const FVoxelOpStackRecipe& Recipe)
    {
        switch (Recipe.StructuralParamBlock)
        {
        case EVoxelStrateParamBlock::TunnelNetwork: return ECaveGeneratorType::TunnelNetwork;
        case EVoxelStrateParamBlock::Slab:           return ECaveGeneratorType::FlatPlain;
        case EVoxelStrateParamBlock::Maze:           return ECaveGeneratorType::Maze;
        case EVoxelStrateParamBlock::Surface:        return ECaveGeneratorType::SurfaceWorld;
        case EVoxelStrateParamBlock::VerticalShaft:  return ECaveGeneratorType::VerticalShafts;
        case EVoxelStrateParamBlock::FloatingIsland: return ECaveGeneratorType::FloatingIslands;
        default:                                     return ECaveGeneratorType::TunnelNetwork;
        }
    }
}

FVoxelStrateComposerCandidate VF_RollStrateCandidate(
    const FVoxelStrateCorpus& Corpus, int32 Seed, int32 Index, bool bRollStructure)
{
    FVoxelStrateComposerCandidate Candidate;
    Candidate.Seed = Seed;
    Candidate.Index = Index;
    Candidate.bStructureRoll = bRollStructure;

    if (!bRollStructure)
    {
        // This is the exact Tier 4a call used by the parameter-roll measurement test.
        Candidate.ParameterRoll = VF_RollStrateParamsDetailed(Corpus, Seed, Index);
        if (!Candidate.ParameterRoll.bValid)
        {
            Candidate.FailureReason = Candidate.ParameterRoll.FailureReason;
            return Candidate;
        }

        Candidate.Archetype = Candidate.ParameterRoll.Archetype;
        Candidate.ArchetypeParams = Candidate.ParameterRoll.ArchetypeParams;
        Candidate.Regions = VF_RollStrateRegionManifest(Corpus, Seed, Index, false);
        if (!Candidate.Regions.IsValid())
        {
            Candidate.FailureReason = Candidate.Regions.FailureReason.IsEmpty()
                ? TEXT("lateral region roll failed") : Candidate.Regions.FailureReason;
            return Candidate;
        }
        Candidate.bValid = true;
        return Candidate;
    }

    // This is the exact Tier 4b structure call plus the six independent native-family rolls used
    // to materialise every structure candidate in the offline test.
    Candidate.Recipe = VF_RollStrateStructure(Seed, Index);
    Candidate.Archetype = VF_ComposerArchetypeForRecipe(Candidate.Recipe);
    Candidate.StructureBlockRolls.Reserve(UE_ARRAY_COUNT(GComposerStructureBlocks));

    bool bAllBlocksValid = true;
    for (const FComposerStructureBlockSpec& Spec : GComposerStructureBlocks)
    {
        const FVoxelStrateRollInfo Roll = VF_RollStrateParamsDetailedForArchetype(
            Corpus, Spec.Archetype, Seed ^ static_cast<int32>(Spec.SeedSalt), Index);
        Candidate.StructureBlockRolls.Add(Roll);
        if (!Roll.bValid)
        {
            bAllBlocksValid = false;
            if (Candidate.FailureReason.IsEmpty())
            {
                Candidate.FailureReason = FString::Printf(
                    TEXT("parameter block %d (%s) failed: %s"),
                    static_cast<int32>(Spec.Block),
                    VF_GetStrateArchetypeName(Spec.Archetype),
                    *Roll.FailureReason);
            }
            continue;
        }
        VF_CopyComposerBlock(Candidate.ArchetypeParams, Roll, Spec.Archetype);
    }

    Candidate.Regions = VF_RollStrateRegionManifest(Corpus, Seed, Index, true);
    if (!Candidate.Regions.IsValid() && Candidate.FailureReason.IsEmpty())
    {
        Candidate.FailureReason = Candidate.Regions.FailureReason.IsEmpty()
            ? TEXT("lateral region roll failed") : Candidate.Regions.FailureReason;
    }
    Candidate.bValid = bAllBlocksValid && Candidate.Regions.IsValid();
    return Candidate;
}

FVoxelStratePromotableRecord VF_MakeStratePromotableRecord(
    const FVoxelStrateComposerCandidate& Candidate,
    const FVoxelStrateMetrics& Metrics,
    int32 Season,
    uint32 InputCorpusHash,
    bool bPassedNonVacuous,
    bool bPassedLargestComponent,
    bool bPassedPrimordialLaw)
{
    FVoxelStratePromotableRecord Record;
    Record.Season = Season;
    Record.Seed = Candidate.Seed;
    Record.CandidateIndex = Candidate.Index;
    Record.InputCorpusHash = InputCorpusHash;
    Record.Archetype = Candidate.Archetype;
    Record.Params = Candidate.ArchetypeParams;
    Record.Recipe = Candidate.Recipe;
    Record.MeasuredMetrics = VF_SummarizeStrateMetrics(Metrics);
    Record.bPassedNonVacuous = bPassedNonVacuous;
    Record.bPassedLargestComponent = bPassedLargestComponent;
    Record.bPassedPrimordialLaw = bPassedPrimordialLaw;
    Record.RecordId = FString::Printf(
        TEXT("season_%04d_seed_%d_candidate_%04d_corpus_%08x_recipe_%08x"),
        Season, Candidate.Seed, Candidate.Index, InputCorpusHash,
        VF_HashStrateStructureRecipe(Candidate.Recipe));
    return Record;
}
#endif

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
#endif // WITH_EDITOR — no roller/corpus entry point is compiled into Shipping
