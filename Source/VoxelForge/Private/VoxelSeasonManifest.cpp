// Offline Tier 4c season composition, manifest persistence, and review artifact.

#include "VoxelSeasonManifest.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/EnumProperty.h"
#include "UObject/FieldIterator.h"
#include "UObject/UnrealType.h"

#include "VoxelDensityOpStack.h"
#include "VoxelSettings.h"
#include "VoxelStrateDefinition.h"
#include "VoxelStrateManager.h"
#include "VoxelStratePreview.h"
#include "VoxelCaveMorphology.h"

#include <limits>

namespace VoxelSeasonManifestPrivate
{
    enum class EScalarKind : uint8
    {
        Float,
        Integer,
        Boolean,
    };

    struct FScalarValue
    {
        EScalarKind Kind = EScalarKind::Float;
        double Number = 0.0;
        bool Boolean = false;
    };

    bool IsTunnelArchetype(ECaveGeneratorType Archetype)
    {
        return Archetype == ECaveGeneratorType::TunnelNetwork
            || Archetype == ECaveGeneratorType::Underwater;
    }

    bool IsSupportedArchetype(ECaveGeneratorType Archetype)
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

    UStruct* ParamStruct(ECaveGeneratorType Archetype)
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

    const void* ParamMemory(const FVoxelStrateArchetypeParams& Params,
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

    void* ParamMemory(FVoxelStrateArchetypeParams& Params,
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

    template <typename T>
    double NativeNumber(T Value)
    {
        if constexpr (std::is_enum<T>::value)
        {
            using Underlying = typename std::underlying_type<T>::type;
            return static_cast<double>(static_cast<Underlying>(Value));
        }
        else
        {
            return static_cast<double>(Value);
        }
    }

    template <typename T>
    EScalarKind NativeKind()
    {
        using ValueType = typename std::remove_cv<
            typename std::remove_reference<T>::type>::type;
        if (std::is_same<ValueType, bool>::value)
        {
            return EScalarKind::Boolean;
        }
        if (std::is_integral<ValueType>::value || std::is_enum<ValueType>::value)
        {
            return EScalarKind::Integer;
        }
        return EScalarKind::Float;
    }

    template <typename T>
    typename std::enable_if<!std::is_enum<T>::value, void>::type
    AssignNative(T& Target, double Value)
    {
        Target = static_cast<T>(Value);
    }

    template <typename T>
    typename std::enable_if<std::is_enum<T>::value, void>::type
    AssignNative(T& Target, double Value)
    {
        using Underlying = typename std::underlying_type<T>::type;
        Target = static_cast<T>(static_cast<Underlying>(FMath::RoundToInt(Value)));
    }

    template <typename T>
    void AssignNativeScalar(T& Target, const FScalarValue& Value)
    {
        using ValueType = typename std::remove_cv<T>::type;
        if constexpr (std::is_same<ValueType, bool>::value)
        {
            Target = Value.Boolean;
        }
        else
        {
            AssignNative(Target, Value.Number);
        }
    }

    template <typename T>
    bool NativeBoolValue(T Value)
    {
        using ValueType = typename std::remove_cv<T>::type;
        if constexpr (std::is_same<ValueType, bool>::value)
        {
            return Value;
        }
        else
        {
            return NativeNumber(Value) != 0.0;
        }
    }

    bool IsScalarProperty(const FProperty* Property)
    {
        return CastField<FBoolProperty>(Property) != nullptr
            || CastField<FNumericProperty>(Property) != nullptr
            || CastField<FEnumProperty>(Property) != nullptr;
    }

    bool ReadProperty(const FProperty* Property, const void* Memory, FScalarValue& OutValue)
    {
        if (Property == nullptr || Memory == nullptr || !IsScalarProperty(Property))
        {
            return false;
        }
        const void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Memory);
        if (const FBoolProperty* Bool = CastField<FBoolProperty>(Property))
        {
            OutValue.Kind = EScalarKind::Boolean;
            OutValue.Boolean = Bool->GetPropertyValue(ValuePtr);
            OutValue.Number = OutValue.Boolean ? 1.0 : 0.0;
            return true;
        }

        const FNumericProperty* Numeric = CastField<FNumericProperty>(Property);
        const FNumericProperty* Underlying = Numeric;
        if (const FEnumProperty* Enum = CastField<FEnumProperty>(Property))
        {
            Underlying = Enum->GetUnderlyingProperty();
        }
        if (Underlying == nullptr)
        {
            return false;
        }
        OutValue.Kind = Underlying->IsFloatingPoint()
            ? EScalarKind::Float : EScalarKind::Integer;
        OutValue.Number = Underlying->IsFloatingPoint()
            ? Underlying->GetFloatingPointPropertyValue(ValuePtr)
            : static_cast<double>(Underlying->GetSignedIntPropertyValue(ValuePtr));
        return FMath::IsFinite(OutValue.Number);
    }

    bool WriteProperty(const FProperty* Property, void* Memory, const FScalarValue& Value)
    {
        if (Property == nullptr || Memory == nullptr || !IsScalarProperty(Property))
        {
            return false;
        }
        void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Memory);
        if (const FBoolProperty* Bool = CastField<FBoolProperty>(Property))
        {
            if (Value.Kind != EScalarKind::Boolean)
            {
                return false;
            }
            Bool->SetPropertyValue(ValuePtr, Value.Boolean);
            return true;
        }

        const FNumericProperty* Numeric = CastField<FNumericProperty>(Property);
        const FNumericProperty* Underlying = Numeric;
        if (const FEnumProperty* Enum = CastField<FEnumProperty>(Property))
        {
            Underlying = Enum->GetUnderlyingProperty();
        }
        if (Underlying == nullptr || !FMath::IsFinite(Value.Number))
        {
            return false;
        }
        if (Underlying->IsFloatingPoint())
        {
            if (Value.Kind != EScalarKind::Float)
            {
                return false;
            }
            Underlying->SetFloatingPointPropertyValue(ValuePtr, Value.Number);
        }
        else
        {
            if (Value.Kind != EScalarKind::Integer
                || FMath::FloorToDouble(Value.Number) != Value.Number)
            {
                return false;
            }
            Underlying->SetIntPropertyValue(ValuePtr, static_cast<int64>(Value.Number));
        }
        return true;
    }

    bool ReadTunnelField(const FStrateGenerationParams& Params,
                         const FString& FieldName, FScalarValue& OutValue)
    {
#define VF_SEASON_READ_TUNNEL(Name) \
        if (FieldName == TEXT(#Name)) \
        { \
            OutValue.Kind = NativeKind<decltype(Params.Name)>(); \
            OutValue.Number = NativeNumber(Params.Name); \
            OutValue.Boolean = NativeBoolValue(Params.Name); \
            return FMath::IsFinite(OutValue.Number); \
        }
        VF_STRATE_PARAM_FIELDS(VF_SEASON_READ_TUNNEL, VF_SEASON_READ_TUNNEL)
#undef VF_SEASON_READ_TUNNEL
        return false;
    }

    bool WriteTunnelField(FStrateGenerationParams& Params,
                          const FString& FieldName, const FScalarValue& Value)
    {
#define VF_SEASON_WRITE_TUNNEL(Name) \
        if (FieldName == TEXT(#Name)) \
        { \
            if (Value.Kind != NativeKind<decltype(Params.Name)>()) return false; \
            AssignNativeScalar(Params.Name, Value); \
            return true; \
        }
        VF_STRATE_PARAM_FIELDS(VF_SEASON_WRITE_TUNNEL, VF_SEASON_WRITE_TUNNEL)
#undef VF_SEASON_WRITE_TUNNEL
        return false;
    }

    bool ReadRuntimeField(const void* Memory, ECaveGeneratorType Archetype,
                          const FString& FieldName, FScalarValue& OutValue)
    {
        if (Memory == nullptr || (FieldName != TEXT("StrateTopWorldZ")
                                  && FieldName != TEXT("StrateBottomWorldZ")))
        {
            return false;
        }
        const bool bTop = FieldName == TEXT("StrateTopWorldZ");
        float Number = 0.0f;
        switch (Archetype)
        {
        case ECaveGeneratorType::TunnelNetwork:
        case ECaveGeneratorType::Underwater:
            Number = bTop ? static_cast<const FStrateGenerationParams*>(Memory)->StrateTopWorldZ
                          : static_cast<const FStrateGenerationParams*>(Memory)->StrateBottomWorldZ;
            break;
        case ECaveGeneratorType::FlatPlain:
        case ECaveGeneratorType::CrystalChamber:
            Number = bTop ? static_cast<const FSlabGenerationParams*>(Memory)->StrateTopWorldZ
                          : static_cast<const FSlabGenerationParams*>(Memory)->StrateBottomWorldZ;
            break;
        case ECaveGeneratorType::Maze:
            Number = bTop ? static_cast<const FMazeGenerationParams*>(Memory)->StrateTopWorldZ
                          : static_cast<const FMazeGenerationParams*>(Memory)->StrateBottomWorldZ;
            break;
        case ECaveGeneratorType::SurfaceWorld:
            Number = bTop ? static_cast<const FSurfaceGenerationParams*>(Memory)->StrateTopWorldZ
                          : static_cast<const FSurfaceGenerationParams*>(Memory)->StrateBottomWorldZ;
            break;
        case ECaveGeneratorType::VerticalShafts:
            Number = bTop ? static_cast<const FVerticalShaftParams*>(Memory)->StrateTopWorldZ
                          : static_cast<const FVerticalShaftParams*>(Memory)->StrateBottomWorldZ;
            break;
        case ECaveGeneratorType::FloatingIslands:
            Number = bTop ? static_cast<const FFloatingIslandParams*>(Memory)->StrateTopWorldZ
                          : static_cast<const FFloatingIslandParams*>(Memory)->StrateBottomWorldZ;
            break;
        default:
            return false;
        }
        OutValue.Kind = EScalarKind::Float;
        OutValue.Number = Number;
        return FMath::IsFinite(Number);
    }

    bool WriteRuntimeField(void* Memory, ECaveGeneratorType Archetype,
                           const FString& FieldName, const FScalarValue& Value)
    {
        if (Memory == nullptr || Value.Kind != EScalarKind::Float
            || !FMath::IsFinite(Value.Number)
            || (FieldName != TEXT("StrateTopWorldZ")
                && FieldName != TEXT("StrateBottomWorldZ")))
        {
            return false;
        }
        const bool bTop = FieldName == TEXT("StrateTopWorldZ");
        const float Number = static_cast<float>(Value.Number);
        if (!FMath::IsFinite(Number))
        {
            return false;
        }
        switch (Archetype)
        {
        case ECaveGeneratorType::TunnelNetwork:
        case ECaveGeneratorType::Underwater:
            if (bTop) static_cast<FStrateGenerationParams*>(Memory)->StrateTopWorldZ = Number;
            else static_cast<FStrateGenerationParams*>(Memory)->StrateBottomWorldZ = Number;
            return true;
        case ECaveGeneratorType::FlatPlain:
        case ECaveGeneratorType::CrystalChamber:
            if (bTop) static_cast<FSlabGenerationParams*>(Memory)->StrateTopWorldZ = Number;
            else static_cast<FSlabGenerationParams*>(Memory)->StrateBottomWorldZ = Number;
            return true;
        case ECaveGeneratorType::Maze:
            if (bTop) static_cast<FMazeGenerationParams*>(Memory)->StrateTopWorldZ = Number;
            else static_cast<FMazeGenerationParams*>(Memory)->StrateBottomWorldZ = Number;
            return true;
        case ECaveGeneratorType::SurfaceWorld:
            if (bTop) static_cast<FSurfaceGenerationParams*>(Memory)->StrateTopWorldZ = Number;
            else static_cast<FSurfaceGenerationParams*>(Memory)->StrateBottomWorldZ = Number;
            return true;
        case ECaveGeneratorType::VerticalShafts:
            if (bTop) static_cast<FVerticalShaftParams*>(Memory)->StrateTopWorldZ = Number;
            else static_cast<FVerticalShaftParams*>(Memory)->StrateBottomWorldZ = Number;
            return true;
        case ECaveGeneratorType::FloatingIslands:
            if (bTop) static_cast<FFloatingIslandParams*>(Memory)->StrateTopWorldZ = Number;
            else static_cast<FFloatingIslandParams*>(Memory)->StrateBottomWorldZ = Number;
            return true;
        default:
            return false;
        }
    }

    bool ReadNamedField(const void* Memory, ECaveGeneratorType Archetype,
                        const FString& FieldName, FScalarValue& OutValue)
    {
        if (FieldName == TEXT("StrateTopWorldZ") || FieldName == TEXT("StrateBottomWorldZ"))
        {
            return ReadRuntimeField(Memory, Archetype, FieldName, OutValue);
        }
        if (Memory == nullptr || !IsSupportedArchetype(Archetype))
        {
            return false;
        }
        if (IsTunnelArchetype(Archetype)
            && ReadTunnelField(*static_cast<const FStrateGenerationParams*>(Memory),
                               FieldName, OutValue))
        {
            return true;
        }
        UStruct* Struct = ParamStruct(Archetype);
        for (TFieldIterator<FProperty> It(Struct, EFieldIteratorFlags::IncludeSuper); It; ++It)
        {
            if ((*It)->GetName() == FieldName)
            {
                return ReadProperty(*It, Memory, OutValue);
            }
        }
        return false;
    }

    bool WriteNamedField(void* Memory, ECaveGeneratorType Archetype,
                         const FString& FieldName, const FScalarValue& Value)
    {
        if (FieldName == TEXT("StrateTopWorldZ") || FieldName == TEXT("StrateBottomWorldZ"))
        {
            return WriteRuntimeField(Memory, Archetype, FieldName, Value);
        }
        if (Memory == nullptr || !IsSupportedArchetype(Archetype))
        {
            return false;
        }
        if (IsTunnelArchetype(Archetype)
            && WriteTunnelField(*static_cast<FStrateGenerationParams*>(Memory),
                                FieldName, Value))
        {
            return true;
        }
        UStruct* Struct = ParamStruct(Archetype);
        for (TFieldIterator<FProperty> It(Struct, EFieldIteratorFlags::IncludeSuper); It; ++It)
        {
            if ((*It)->GetName() == FieldName)
            {
                return WriteProperty(*It, Memory, Value);
            }
        }
        return false;
    }

    TArray<FString> FieldNames(ECaveGeneratorType Archetype)
    {
        TArray<FString> Result;
        if (IsTunnelArchetype(Archetype))
        {
#define VF_SEASON_ADD_TUNNEL(Name) Result.AddUnique(TEXT(#Name));
            VF_STRATE_PARAM_FIELDS(VF_SEASON_ADD_TUNNEL, VF_SEASON_ADD_TUNNEL)
#undef VF_SEASON_ADD_TUNNEL
        }
        else if (UStruct* Struct = ParamStruct(Archetype))
        {
            for (TFieldIterator<FProperty> It(Struct, EFieldIteratorFlags::IncludeSuper); It; ++It)
            {
                if (IsScalarProperty(*It))
                {
                    Result.AddUnique((*It)->GetName());
                }
            }
        }
        Result.Add(TEXT("StrateTopWorldZ"));
        Result.Add(TEXT("StrateBottomWorldZ"));
        Result.Sort([](const FString& A, const FString& B) { return A < B; });
        return Result;
    }

    FString FloatBits(float Value)
    {
        uint32 Bits = 0;
        FMemory::Memcpy(&Bits, &Value, sizeof(Bits));
        return FString::Printf(TEXT("0x%08X"), Bits);
    }

    bool ParseFloatBits(const FString& Text, float& OutValue)
    {
        FString Digits = Text;
        if (Digits.StartsWith(TEXT("0x"), ESearchCase::IgnoreCase))
        {
            Digits.RightChopInline(2);
        }
        if (Digits.IsEmpty() || Digits.Len() > 8)
        {
            return false;
        }
        uint32 Bits = 0;
        for (const TCHAR Ch : Digits)
        {
            uint32 Nibble = 0;
            if (Ch >= TEXT('0') && Ch <= TEXT('9')) Nibble = static_cast<uint32>(Ch - TEXT('0'));
            else if (Ch >= TEXT('a') && Ch <= TEXT('f')) Nibble = static_cast<uint32>(Ch - TEXT('a') + 10);
            else if (Ch >= TEXT('A') && Ch <= TEXT('F')) Nibble = static_cast<uint32>(Ch - TEXT('A') + 10);
            else return false;
            Bits = (Bits << 4) | Nibble;
        }
        FMemory::Memcpy(&OutValue, &Bits, sizeof(OutValue));
        return FMath::IsFinite(OutValue);
    }

    TSharedPtr<FJsonObject> SerializeScalar(const FScalarValue& Value)
    {
        TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
        if (Value.Kind == EScalarKind::Boolean)
        {
            Object->SetStringField(TEXT("kind"), TEXT("bool"));
            Object->SetBoolField(TEXT("value"), Value.Boolean);
            return Object;
        }
        if (Value.Kind == EScalarKind::Integer)
        {
            Object->SetStringField(TEXT("kind"), TEXT("int"));
            Object->SetNumberField(TEXT("value"), Value.Number);
            return Object;
        }
        const float FloatValue = static_cast<float>(Value.Number);
        Object->SetStringField(TEXT("kind"), TEXT("float"));
        Object->SetNumberField(TEXT("value"), static_cast<double>(FloatValue));
        Object->SetStringField(TEXT("bits"), FloatBits(FloatValue));
        return Object;
    }

    bool ReadJsonNumber(const TSharedPtr<FJsonObject>& Object,
                        const TCHAR* Name, double& OutValue)
    {
        if (!Object.IsValid()) return false;
        const TSharedPtr<FJsonValue>* Found = Object->Values.Find(Name);
        if (Found == nullptr || !Found->IsValid() || (*Found)->Type != EJson::Number)
        {
            return false;
        }
        OutValue = (*Found)->AsNumber();
        return FMath::IsFinite(OutValue);
    }

    bool ReadJsonInt32(const TSharedPtr<FJsonObject>& Object,
                       const TCHAR* Name, int32& OutValue)
    {
        double Number = 0.0;
        if (!ReadJsonNumber(Object, Name, Number)
            || FMath::FloorToDouble(Number) != Number
            || Number < static_cast<double>(TNumericLimits<int32>::Lowest())
            || Number > static_cast<double>(TNumericLimits<int32>::Max()))
        {
            return false;
        }
        OutValue = static_cast<int32>(Number);
        return true;
    }

    bool ReadJsonInt64(const TSharedPtr<FJsonObject>& Object,
                       const TCHAR* Name, int64& OutValue)
    {
        double Number = 0.0;
        if (!ReadJsonNumber(Object, Name, Number)
            || FMath::FloorToDouble(Number) != Number
            || Number < -9223372036854775807.0
            || Number > 9223372036854775807.0)
        {
            return false;
        }
        OutValue = static_cast<int64>(Number);
        return static_cast<double>(OutValue) == Number;
    }

    bool ReadJsonString(const TSharedPtr<FJsonObject>& Object,
                        const TCHAR* Name, FString& OutValue)
    {
        if (!Object.IsValid()) return false;
        const TSharedPtr<FJsonValue>* Found = Object->Values.Find(Name);
        if (Found == nullptr || !Found->IsValid() || (*Found)->Type != EJson::String)
        {
            return false;
        }
        OutValue = (*Found)->AsString();
        return true;
    }

    bool ReadJsonBool(const TSharedPtr<FJsonObject>& Object,
                      const TCHAR* Name, bool& OutValue)
    {
        if (!Object.IsValid()) return false;
        const TSharedPtr<FJsonValue>* Found = Object->Values.Find(Name);
        if (Found == nullptr || !Found->IsValid() || (*Found)->Type != EJson::Boolean)
        {
            return false;
        }
        OutValue = (*Found)->AsBool();
        return true;
    }

    bool ReadJsonObject(const TSharedPtr<FJsonObject>& Object,
                        const TCHAR* Name, TSharedPtr<FJsonObject>& OutValue)
    {
        if (!Object.IsValid()) return false;
        const TSharedPtr<FJsonValue>* Found = Object->Values.Find(Name);
        if (Found == nullptr || !Found->IsValid() || (*Found)->Type != EJson::Object)
        {
            return false;
        }
        OutValue = (*Found)->AsObject();
        return OutValue.IsValid();
    }

    bool ReadJsonArray(const TSharedPtr<FJsonObject>& Object,
                       const TCHAR* Name,
                       const TArray<TSharedPtr<FJsonValue>>*& OutValue)
    {
        if (!Object.IsValid()) return false;
        const TSharedPtr<FJsonValue>* Found = Object->Values.Find(Name);
        if (Found == nullptr || !Found->IsValid() || (*Found)->Type != EJson::Array)
        {
            return false;
        }
        OutValue = &(*Found)->AsArray();
        return true;
    }

    bool ReadScalar(const TSharedPtr<FJsonObject>& Object,
                    const FString& Name, FScalarValue& OutValue)
    {
        TSharedPtr<FJsonObject> Scalar;
        if (!ReadJsonObject(Object, *Name, Scalar)) return false;
        FString Kind;
        if (!ReadJsonString(Scalar, TEXT("kind"), Kind)) return false;
        if (Kind == TEXT("bool"))
        {
            OutValue.Kind = EScalarKind::Boolean;
            return ReadJsonBool(Scalar, TEXT("value"), OutValue.Boolean);
        }
        if (Kind == TEXT("int"))
        {
            OutValue.Kind = EScalarKind::Integer;
            return ReadJsonNumber(Scalar, TEXT("value"), OutValue.Number)
                && FMath::FloorToDouble(OutValue.Number) == OutValue.Number;
        }
        if (Kind != TEXT("float")) return false;
        FString Bits;
        if (!ReadJsonString(Scalar, TEXT("bits"), Bits)) return false;
        float FloatValue = 0.0f;
        if (!ParseFloatBits(Bits, FloatValue)) return false;
        OutValue.Kind = EScalarKind::Float;
        OutValue.Number = FloatValue;
        return true;
    }

    TSharedPtr<FJsonObject> SerializeParamsForArchetype(
        ECaveGeneratorType Archetype, const FVoxelStrateArchetypeParams& Params)
    {
        TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
        const void* Memory = ParamMemory(Params, Archetype);
        for (const FString& FieldName : FieldNames(Archetype))
        {
            FScalarValue Value;
            if (ReadNamedField(Memory, Archetype, FieldName, Value))
            {
                Object->SetObjectField(FieldName, SerializeScalar(Value));
            }
        }
        return Object;
    }

    bool DeserializeParamsForArchetype(
        ECaveGeneratorType Archetype, const TSharedPtr<FJsonObject>& Object,
        FVoxelStrateArchetypeParams& OutParams)
    {
        if (!Object.IsValid()) return false;
        void* Memory = ParamMemory(OutParams, Archetype);
        if (Memory == nullptr) return false;
        for (const FString& FieldName : FieldNames(Archetype))
        {
            FScalarValue Value;
            if (!ReadScalar(Object, FieldName, Value)
                || !WriteNamedField(Memory, Archetype, FieldName, Value))
            {
                return false;
            }
        }
        return true;
    }

    TSharedPtr<FJsonObject> SerializeAllParams(const FVoxelStrateArchetypeParams& Params)
    {
        TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
        Object->SetObjectField(TEXT("tunnel_network"),
                               SerializeParamsForArchetype(ECaveGeneratorType::TunnelNetwork, Params));
        Object->SetObjectField(TEXT("slab"),
                               SerializeParamsForArchetype(ECaveGeneratorType::FlatPlain, Params));
        Object->SetObjectField(TEXT("maze"),
                               SerializeParamsForArchetype(ECaveGeneratorType::Maze, Params));
        Object->SetObjectField(TEXT("surface"),
                               SerializeParamsForArchetype(ECaveGeneratorType::SurfaceWorld, Params));
        Object->SetObjectField(TEXT("vertical_shaft"),
                               SerializeParamsForArchetype(ECaveGeneratorType::VerticalShafts, Params));
        Object->SetObjectField(TEXT("floating_island"),
                               SerializeParamsForArchetype(ECaveGeneratorType::FloatingIslands, Params));
        return Object;
    }

    bool DeserializeAllParams(const TSharedPtr<FJsonObject>& Object,
                              FVoxelStrateArchetypeParams& OutParams)
    {
        if (!Object.IsValid()) return false;
        TSharedPtr<FJsonObject> Tunnel, Slab, Maze, Surface, Vertical, Floating;
        if (!ReadJsonObject(Object, TEXT("tunnel_network"), Tunnel)
            || !ReadJsonObject(Object, TEXT("slab"), Slab)
            || !ReadJsonObject(Object, TEXT("maze"), Maze)
            || !ReadJsonObject(Object, TEXT("surface"), Surface)
            || !ReadJsonObject(Object, TEXT("vertical_shaft"), Vertical)
            || !ReadJsonObject(Object, TEXT("floating_island"), Floating))
        {
            return false;
        }
        OutParams = FVoxelStrateArchetypeParams();
        return DeserializeParamsForArchetype(ECaveGeneratorType::TunnelNetwork, Tunnel, OutParams)
            && DeserializeParamsForArchetype(ECaveGeneratorType::FlatPlain, Slab, OutParams)
            && DeserializeParamsForArchetype(ECaveGeneratorType::Maze, Maze, OutParams)
            && DeserializeParamsForArchetype(ECaveGeneratorType::SurfaceWorld, Surface, OutParams)
            && DeserializeParamsForArchetype(ECaveGeneratorType::VerticalShafts, Vertical, OutParams)
            && DeserializeParamsForArchetype(ECaveGeneratorType::FloatingIslands, Floating, OutParams);
    }

    TSharedPtr<FJsonObject> SerializeRecipeEntry(const FVoxelOpRecipeEntry& Entry)
    {
        TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
        Object->SetNumberField(TEXT("op_class"), static_cast<int32>(Entry.OpClass));
        Object->SetNumberField(TEXT("param_block"), static_cast<int32>(Entry.ParamBlock));
        return Object;
    }

    bool DeserializeRecipeEntry(const TSharedPtr<FJsonObject>& Object,
                                FVoxelOpRecipeEntry& OutEntry)
    {
        int32 OpClass = 0;
        int32 ParamBlock = 0;
        if (!ReadJsonInt32(Object, TEXT("op_class"), OpClass)
            || !ReadJsonInt32(Object, TEXT("param_block"), ParamBlock)
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

    TSharedPtr<FJsonObject> SerializeRecipe(const FVoxelOpStackRecipe& Recipe)
    {
        TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
        Object->SetNumberField(TEXT("root_polarity"), static_cast<int32>(Recipe.RootPolarity));
        Object->SetObjectField(TEXT("root"), SerializeRecipeEntry(Recipe.Root));
        Object->SetObjectField(TEXT("shape_source"), SerializeRecipeEntry(Recipe.ShapeSource));
        Object->SetObjectField(TEXT("conversion"), SerializeRecipeEntry(Recipe.Conversion));
        Object->SetNumberField(TEXT("structural_param_block"),
                               static_cast<int32>(Recipe.StructuralParamBlock));
        TArray<TSharedPtr<FJsonValue>> Modifiers;
        for (const FVoxelOpRecipeEntry& Entry : Recipe.Modifiers)
        {
            Modifiers.Add(MakeShared<FJsonValueObject>(SerializeRecipeEntry(Entry)));
        }
        Object->SetArrayField(TEXT("modifiers"), MoveTemp(Modifiers));
        return Object;
    }

    bool DeserializeRecipe(const TSharedPtr<FJsonObject>& Object,
                           FVoxelOpStackRecipe& OutRecipe)
    {
        if (!Object.IsValid()) return false;
        int32 RootPolarity = 0;
        int32 StructuralBlock = 0;
        TSharedPtr<FJsonObject> Root, Shape, Conversion;
        const TArray<TSharedPtr<FJsonValue>>* Modifiers = nullptr;
        if (!ReadJsonInt32(Object, TEXT("root_polarity"), RootPolarity)
            || !ReadJsonObject(Object, TEXT("root"), Root)
            || !ReadJsonObject(Object, TEXT("shape_source"), Shape)
            || !ReadJsonObject(Object, TEXT("conversion"), Conversion)
            || !ReadJsonInt32(Object, TEXT("structural_param_block"), StructuralBlock)
            || !ReadJsonArray(Object, TEXT("modifiers"), Modifiers)
            || RootPolarity < static_cast<int32>(EVoxelStrateRootPolarity::RockCarve)
            || RootPolarity > static_cast<int32>(EVoxelStrateRootPolarity::VoidFill)
            || StructuralBlock < static_cast<int32>(EVoxelStrateParamBlock::None)
            || StructuralBlock > static_cast<int32>(EVoxelStrateParamBlock::FloatingIsland)
            || Modifiers->Num() < 4 || Modifiers->Num() > 8
            || !DeserializeRecipeEntry(Root, OutRecipe.Root)
            || !DeserializeRecipeEntry(Shape, OutRecipe.ShapeSource)
            || !DeserializeRecipeEntry(Conversion, OutRecipe.Conversion))
        {
            return false;
        }
        OutRecipe.RootPolarity = static_cast<EVoxelStrateRootPolarity>(RootPolarity);
        OutRecipe.StructuralParamBlock = static_cast<EVoxelStrateParamBlock>(StructuralBlock);
        OutRecipe.Modifiers.Reset();
        for (const TSharedPtr<FJsonValue>& Value : *Modifiers)
        {
            if (!Value.IsValid() || Value->Type != EJson::Object) return false;
            FVoxelOpRecipeEntry& Entry = OutRecipe.Modifiers.AddDefaulted_GetRef();
            if (!DeserializeRecipeEntry(Value->AsObject(), Entry)) return false;
        }
        return true;
    }

    void SetMetricFloat(const TSharedPtr<FJsonObject>& Object,
                        const TCHAR* Name, float Value)
    {
        FScalarValue Scalar;
        Scalar.Kind = EScalarKind::Float;
        Scalar.Number = Value;
        Object->SetObjectField(Name, SerializeScalar(Scalar));
    }

    bool ReadMetricFloat(const TSharedPtr<FJsonObject>& Object,
                         const TCHAR* Name, float& OutValue)
    {
        FScalarValue Scalar;
        if (!ReadScalar(Object, Name, Scalar) || Scalar.Kind != EScalarKind::Float)
        {
            return false;
        }
        OutValue = static_cast<float>(Scalar.Number);
        return FMath::IsFinite(OutValue);
    }

    TSharedPtr<FJsonObject> SerializeMetrics(const FVoxelStrateMeasuredMetrics& Metrics)
    {
        TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
        Object->SetBoolField(TEXT("valid"), Metrics.bValid);
        Object->SetNumberField(TEXT("num_sampled"), static_cast<double>(Metrics.NumSampled));
        Object->SetNumberField(TEXT("num_air"), static_cast<double>(Metrics.NumAir));
        Object->SetNumberField(TEXT("num_solid"), static_cast<double>(Metrics.NumSolid));
        SetMetricFloat(Object, TEXT("air_fraction"), Metrics.AirFraction);
        Object->SetNumberField(TEXT("num_air_components"), Metrics.NumAirComponents);
        SetMetricFloat(Object, TEXT("largest_component_share"), Metrics.LargestComponentShare);
        Object->SetNumberField(TEXT("largest_component_cells"),
                               static_cast<double>(Metrics.LargestComponentCells));
        Object->SetNumberField(TEXT("num_components_at_least_1_pct"),
                               Metrics.NumComponentsAtLeast1Pct);
        SetMetricFloat(Object, TEXT("walkable_fraction"), Metrics.WalkableFraction);
        Object->SetNumberField(TEXT("walkable_floor_columns"),
                               static_cast<double>(Metrics.WalkableFloorColumns));
        SetMetricFloat(Object, TEXT("walkable_floor_area_fraction"),
                       Metrics.WalkableFloorAreaFraction);
        Object->SetNumberField(TEXT("num_walkable_surface_components"),
                               Metrics.NumWalkableSurfaceComponents);
        Object->SetNumberField(TEXT("largest_walkable_surface_columns"),
                               static_cast<double>(Metrics.LargestWalkableSurfaceColumns));
        SetMetricFloat(Object, TEXT("largest_walkable_surface_share"),
                       Metrics.LargestWalkableSurfaceShare);
        SetMetricFloat(Object, TEXT("median_feature_scale"), Metrics.MedianFeatureScale);
        Object->SetNumberField(TEXT("median_vertical_clearance"), Metrics.MedianVerticalClearance);
        Object->SetBoolField(TEXT("player_fit_resolved"), Metrics.bPlayerFitResolved);
        Object->SetStringField(TEXT("player_fit_refusal_reason"), Metrics.PlayerFitRefusalReason);
        Object->SetNumberField(TEXT("num_player_fit_cells"),
                               static_cast<double>(Metrics.NumPlayerFitCells));
        SetMetricFloat(Object, TEXT("player_fit_fraction"), Metrics.PlayerFitFraction);
        Object->SetNumberField(TEXT("num_traversable_components"),
                               Metrics.NumTraversableComponents);
        Object->SetNumberField(TEXT("largest_traversable_component_cells"),
                               static_cast<double>(Metrics.LargestTraversableComponentCells));
        SetMetricFloat(Object, TEXT("traversable_component_share"),
                       Metrics.TraversableComponentShare);
        SetMetricFloat(Object, TEXT("minimum_player_clearance_voxels"),
                       Metrics.MinimumPlayerClearanceVoxels);
        Object->SetNumberField(TEXT("resolved_margin_voxels"), Metrics.ResolvedMarginVoxels);
        Object->SetNumberField(TEXT("sampled_min_z"), Metrics.SampledMinZ);
        Object->SetNumberField(TEXT("sampled_max_z"), Metrics.SampledMaxZ);
        Object->SetNumberField(TEXT("sampled_num_x"), Metrics.SampledNumX);
        Object->SetNumberField(TEXT("sampled_num_y"), Metrics.SampledNumY);
        Object->SetNumberField(TEXT("sampled_num_z"), Metrics.SampledNumZ);
        SetMetricFloat(Object, TEXT("sampled_min_x"), Metrics.SampledMinX);
        SetMetricFloat(Object, TEXT("sampled_max_x"), Metrics.SampledMaxX);
        SetMetricFloat(Object, TEXT("sampled_min_y"), Metrics.SampledMinY);
        SetMetricFloat(Object, TEXT("sampled_max_y"), Metrics.SampledMaxY);
        TSharedPtr<FJsonObject> Point = MakeShared<FJsonObject>();
        SetMetricFloat(Point, TEXT("x"), static_cast<float>(Metrics.LargestComponentPoint.X));
        SetMetricFloat(Point, TEXT("y"), static_cast<float>(Metrics.LargestComponentPoint.Y));
        SetMetricFloat(Point, TEXT("z"), static_cast<float>(Metrics.LargestComponentPoint.Z));
        Object->SetObjectField(TEXT("largest_component_point"), Point);
        TArray<TSharedPtr<FJsonValue>> Components;
        for (const int64 Cells : Metrics.AirComponentCells)
        {
            Components.Add(MakeShared<FJsonValueNumber>(static_cast<double>(Cells)));
        }
        Object->SetArrayField(TEXT("air_component_cells"), MoveTemp(Components));
        return Object;
    }

    bool DeserializeMetrics(const TSharedPtr<FJsonObject>& Object,
                            FVoxelStrateMeasuredMetrics& OutMetrics)
    {
        if (!Object.IsValid()) return false;
        bool bValid = true;
        bValid = ReadJsonBool(Object, TEXT("valid"), OutMetrics.bValid) && bValid;
        bValid = ReadJsonInt64(Object, TEXT("num_sampled"), OutMetrics.NumSampled) && bValid;
        bValid = ReadJsonInt64(Object, TEXT("num_air"), OutMetrics.NumAir) && bValid;
        bValid = ReadJsonInt64(Object, TEXT("num_solid"), OutMetrics.NumSolid) && bValid;
        bValid = ReadMetricFloat(Object, TEXT("air_fraction"), OutMetrics.AirFraction) && bValid;
        bValid = ReadJsonInt32(Object, TEXT("num_air_components"), OutMetrics.NumAirComponents) && bValid;
        bValid = ReadMetricFloat(Object, TEXT("largest_component_share"), OutMetrics.LargestComponentShare) && bValid;
        bValid = ReadJsonInt64(Object, TEXT("largest_component_cells"), OutMetrics.LargestComponentCells) && bValid;
        bValid = ReadJsonInt32(Object, TEXT("num_components_at_least_1_pct"), OutMetrics.NumComponentsAtLeast1Pct) && bValid;
        bValid = ReadMetricFloat(Object, TEXT("walkable_fraction"), OutMetrics.WalkableFraction) && bValid;
        if (Object->HasField(TEXT("walkable_floor_columns")))
        {
            bValid = ReadJsonInt64(Object, TEXT("walkable_floor_columns"),
                                   OutMetrics.WalkableFloorColumns) && bValid;
            bValid = ReadMetricFloat(Object, TEXT("walkable_floor_area_fraction"),
                                     OutMetrics.WalkableFloorAreaFraction) && bValid;
            bValid = ReadJsonInt32(Object, TEXT("num_walkable_surface_components"),
                                   OutMetrics.NumWalkableSurfaceComponents) && bValid;
            bValid = ReadJsonInt64(Object, TEXT("largest_walkable_surface_columns"),
                                   OutMetrics.LargestWalkableSurfaceColumns) && bValid;
            bValid = ReadMetricFloat(Object, TEXT("largest_walkable_surface_share"),
                                     OutMetrics.LargestWalkableSurfaceShare) && bValid;
        }
        bValid = ReadMetricFloat(Object, TEXT("median_feature_scale"), OutMetrics.MedianFeatureScale) && bValid;
        bValid = ReadJsonInt32(Object, TEXT("median_vertical_clearance"), OutMetrics.MedianVerticalClearance) && bValid;
        if (Object->HasField(TEXT("player_fit_resolved")))
        {
            bValid = ReadJsonBool(Object, TEXT("player_fit_resolved"),
                                  OutMetrics.bPlayerFitResolved) && bValid;
            bValid = ReadJsonString(Object, TEXT("player_fit_refusal_reason"),
                                    OutMetrics.PlayerFitRefusalReason) && bValid;
            bValid = ReadJsonInt64(Object, TEXT("num_player_fit_cells"),
                                   OutMetrics.NumPlayerFitCells) && bValid;
            bValid = ReadMetricFloat(Object, TEXT("player_fit_fraction"),
                                     OutMetrics.PlayerFitFraction) && bValid;
            bValid = ReadJsonInt32(Object, TEXT("num_traversable_components"),
                                   OutMetrics.NumTraversableComponents) && bValid;
            bValid = ReadJsonInt64(Object, TEXT("largest_traversable_component_cells"),
                                   OutMetrics.LargestTraversableComponentCells) && bValid;
            bValid = ReadMetricFloat(Object, TEXT("traversable_component_share"),
                                     OutMetrics.TraversableComponentShare) && bValid;
            bValid = ReadMetricFloat(Object, TEXT("minimum_player_clearance_voxels"),
                                     OutMetrics.MinimumPlayerClearanceVoxels) && bValid;
        }
        bValid = ReadJsonInt32(Object, TEXT("resolved_margin_voxels"), OutMetrics.ResolvedMarginVoxels) && bValid;
        bValid = ReadJsonInt32(Object, TEXT("sampled_min_z"), OutMetrics.SampledMinZ) && bValid;
        bValid = ReadJsonInt32(Object, TEXT("sampled_max_z"), OutMetrics.SampledMaxZ) && bValid;
        bValid = ReadJsonInt32(Object, TEXT("sampled_num_x"), OutMetrics.SampledNumX) && bValid;
        bValid = ReadJsonInt32(Object, TEXT("sampled_num_y"), OutMetrics.SampledNumY) && bValid;
        bValid = ReadJsonInt32(Object, TEXT("sampled_num_z"), OutMetrics.SampledNumZ) && bValid;
        bValid = ReadMetricFloat(Object, TEXT("sampled_min_x"), OutMetrics.SampledMinX) && bValid;
        bValid = ReadMetricFloat(Object, TEXT("sampled_max_x"), OutMetrics.SampledMaxX) && bValid;
        bValid = ReadMetricFloat(Object, TEXT("sampled_min_y"), OutMetrics.SampledMinY) && bValid;
        bValid = ReadMetricFloat(Object, TEXT("sampled_max_y"), OutMetrics.SampledMaxY) && bValid;
        TSharedPtr<FJsonObject> Point;
        bValid = ReadJsonObject(Object, TEXT("largest_component_point"), Point) && bValid;
        if (Point.IsValid())
        {
            float X = 0.0f;
            float Y = 0.0f;
            float Z = 0.0f;
            bValid = ReadMetricFloat(Point, TEXT("x"), X) && bValid;
            bValid = ReadMetricFloat(Point, TEXT("y"), Y) && bValid;
            bValid = ReadMetricFloat(Point, TEXT("z"), Z) && bValid;
            OutMetrics.LargestComponentPoint = FVector(X, Y, Z);
        }
        const TArray<TSharedPtr<FJsonValue>>* Components = nullptr;
        bValid = ReadJsonArray(Object, TEXT("air_component_cells"), Components) && bValid;
        OutMetrics.AirComponentCells.Reset();
        if (Components != nullptr)
        {
            for (const TSharedPtr<FJsonValue>& Value : *Components)
            {
                if (!Value.IsValid() || Value->Type != EJson::Number)
                {
                    bValid = false;
                    continue;
                }
                const double Number = Value->AsNumber();
                const int64 Cells = static_cast<int64>(Number);
                if (!FMath::IsFinite(Number) || Number < 0.0
                    || FMath::FloorToDouble(Number) != Number
                    || static_cast<double>(Cells) != Number)
                {
                    bValid = false;
                    continue;
                }
                OutMetrics.AirComponentCells.Add(Cells);
            }
        }
        return bValid;
    }

    const TCHAR* ConnectivityName(EVoxelConnectivityResult Result)
    {
        switch (Result)
        {
        case EVoxelConnectivityResult::Connected: return TEXT("Connected");
        case EVoxelConnectivityResult::NotConnectedAtThisResolution: return TEXT("NotConnectedAtThisResolution");
        case EVoxelConnectivityResult::StartCellSolid: return TEXT("StartCellSolid");
        case EVoxelConnectivityResult::GoalCellSolid: return TEXT("GoalCellSolid");
        case EVoxelConnectivityResult::StartCellNotPlayerFit: return TEXT("StartCellNotPlayerFit");
        case EVoxelConnectivityResult::GoalCellNotPlayerFit: return TEXT("GoalCellNotPlayerFit");
        case EVoxelConnectivityResult::OutOfWindow: return TEXT("OutOfWindow");
        case EVoxelConnectivityResult::CoarseLiedBudgetExhausted: return TEXT("CoarseLiedBudgetExhausted");
        }
        return TEXT("Unknown");
    }

    bool ParseConnectivityName(const FString& Name, EVoxelConnectivityResult& OutResult)
    {
        const EVoxelConnectivityResult Values[] = {
            EVoxelConnectivityResult::Connected,
            EVoxelConnectivityResult::NotConnectedAtThisResolution,
            EVoxelConnectivityResult::StartCellSolid,
            EVoxelConnectivityResult::GoalCellSolid,
            EVoxelConnectivityResult::StartCellNotPlayerFit,
            EVoxelConnectivityResult::GoalCellNotPlayerFit,
            EVoxelConnectivityResult::OutOfWindow,
            EVoxelConnectivityResult::CoarseLiedBudgetExhausted,
        };
        for (const EVoxelConnectivityResult Value : Values)
        {
            if (Name == ConnectivityName(Value))
            {
                OutResult = Value;
                return true;
            }
        }
        return false;
    }

    const TCHAR* ArchetypeName(ECaveGeneratorType Archetype)
    {
        return VF_GetStrateArchetypeName(Archetype);
    }

    bool ParseArchetype(const FString& Name, ECaveGeneratorType& OutArchetype)
    {
        const ECaveGeneratorType Values[] = {
            ECaveGeneratorType::TunnelNetwork,
            ECaveGeneratorType::FlatPlain,
            ECaveGeneratorType::CrystalChamber,
            ECaveGeneratorType::Maze,
            ECaveGeneratorType::SurfaceWorld,
            ECaveGeneratorType::VerticalShafts,
            ECaveGeneratorType::FloatingIslands,
            ECaveGeneratorType::Underwater,
        };
        for (const ECaveGeneratorType Value : Values)
        {
            if (Name == ArchetypeName(Value))
            {
                OutArchetype = Value;
                return true;
            }
        }
        return false;
    }

    TSharedPtr<FJsonObject> SerializePolicy(const FVoxelSeasonSelectionPolicy& Policy)
    {
        TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
        Object->SetNumberField(TEXT("grounded_fraction"), Policy.GroundedFraction);
        Object->SetNumberField(TEXT("outlier_fraction"), Policy.OutlierFraction);
        Object->SetNumberField(TEXT("adjacent_similarity_distance_threshold"),
                               Policy.AdjacentSimilarityDistanceThreshold);
        Object->SetNumberField(TEXT("adjacent_similarity_penalty_weight"),
                               Policy.AdjacentSimilarityPenaltyWeight);
        Object->SetNumberField(TEXT("new_archetype_bonus"), Policy.NewArchetypeBonus);
        Object->SetNumberField(TEXT("new_recipe_bonus"), Policy.NewRecipeBonus);
        Object->SetNumberField(TEXT("boss_slot_interval"), Policy.BossSlotInterval);
        Object->SetNumberField(TEXT("minimum_largest_component_share"),
                               Policy.MinimumLargestComponentShare);
        Object->SetNumberField(TEXT("feature_scale_normalization_voxels"),
                               Policy.FeatureScaleNormalizationVoxels);
        Object->SetNumberField(TEXT("clearance_normalization_voxels"),
                               Policy.ClearanceNormalizationVoxels);
        return Object;
    }

    bool ReadPolicy(const TSharedPtr<FJsonObject>& Object,
                    FVoxelSeasonSelectionPolicy& OutPolicy)
    {
        if (!Object.IsValid()) return false;
        double Number = 0.0;
        bool bValid = true;
        if (ReadJsonNumber(Object, TEXT("grounded_fraction"), Number)) OutPolicy.GroundedFraction = static_cast<float>(Number); else bValid = false;
        if (ReadJsonNumber(Object, TEXT("outlier_fraction"), Number)) OutPolicy.OutlierFraction = static_cast<float>(Number); else bValid = false;
        if (ReadJsonNumber(Object, TEXT("adjacent_similarity_distance_threshold"), Number)) OutPolicy.AdjacentSimilarityDistanceThreshold = Number; else bValid = false;
        if (ReadJsonNumber(Object, TEXT("adjacent_similarity_penalty_weight"), Number)) OutPolicy.AdjacentSimilarityPenaltyWeight = Number; else bValid = false;
        if (ReadJsonNumber(Object, TEXT("new_archetype_bonus"), Number)) OutPolicy.NewArchetypeBonus = Number; else bValid = false;
        if (ReadJsonNumber(Object, TEXT("new_recipe_bonus"), Number)) OutPolicy.NewRecipeBonus = Number; else bValid = false;
        if (ReadJsonInt32(Object, TEXT("boss_slot_interval"), OutPolicy.BossSlotInterval) == false) bValid = false;
        if (ReadJsonNumber(Object, TEXT("minimum_largest_component_share"), Number)) OutPolicy.MinimumLargestComponentShare = static_cast<float>(Number); else bValid = false;
        if (ReadJsonNumber(Object, TEXT("feature_scale_normalization_voxels"), Number)) OutPolicy.FeatureScaleNormalizationVoxels = static_cast<float>(Number); else bValid = false;
        if (ReadJsonNumber(Object, TEXT("clearance_normalization_voxels"), Number)) OutPolicy.ClearanceNormalizationVoxels = static_cast<float>(Number); else bValid = false;
        return bValid;
    }

    void SetCommonManifestFields(const TSharedPtr<FJsonObject>& Root,
                                 const FVoxelSeasonManifest& Manifest)
    {
        Root->SetStringField(TEXT("format"), TEXT("VoxelForgeSeasonManifest"));
        Root->SetBoolField(TEXT("valid"), Manifest.bValid);
        Root->SetNumberField(TEXT("schema_version"), Manifest.SchemaVersion);
        Root->SetNumberField(TEXT("season"), Manifest.Season);
        Root->SetNumberField(TEXT("seed"), Manifest.Seed);
        Root->SetNumberField(TEXT("input_corpus_hash"), static_cast<double>(Manifest.InputCorpusHash));
        Root->SetNumberField(TEXT("candidate_count"), Manifest.CandidateCount);
        Root->SetNumberField(TEXT("survivor_count"), Manifest.SurvivorCount);
        Root->SetNumberField(TEXT("selected_count"), Manifest.SelectedCount);
        Root->SetNumberField(TEXT("rejected_count"), Manifest.RejectedCount);
        Root->SetNumberField(TEXT("grounded_selected_count"), Manifest.GroundedSelectedCount);
        Root->SetNumberField(TEXT("outlier_selected_count"), Manifest.OutlierSelectedCount);
        Root->SetNumberField(TEXT("fixed_selected_count"), Manifest.FixedSelectedCount);
        Root->SetNumberField(TEXT("total_strates"), Manifest.TotalStrates);
        Root->SetNumberField(TEXT("inter_strate_gap_chunks"), Manifest.InterStrateGapChunks);
        SetMetricFloat(Root, TEXT("origin_spine_radius"), Manifest.OriginSpineRadius);
        SetMetricFloat(Root, TEXT("world_radius_voxels"), Manifest.WorldRadiusVoxels);
        Root->SetObjectField(TEXT("selection_policy"), SerializePolicy(Manifest.SelectionPolicy));
        Root->SetStringField(TEXT("policy_note"), Manifest.PolicyNote);
    }

    TSharedPtr<FJsonObject> SerializeRegionManifest(
        const FVoxelStrateRegionManifest& Regions)
    {
        TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
        Object->SetBoolField(TEXT("valid"), Regions.bValid);
        Object->SetStringField(TEXT("failure_reason"), Regions.FailureReason);
        Object->SetNumberField(TEXT("seed"), Regions.Seed);
        Object->SetNumberField(TEXT("strate_index"), Regions.StrateIndex);
        Object->SetNumberField(TEXT("region_count"), Regions.RegionCount);
        Object->SetNumberField(TEXT("partition_seed"),
                               static_cast<double>(Regions.PartitionSeed));
        SetMetricFloat(Object, TEXT("lattice_cell_size"), Regions.LatticeCellSize);
        SetMetricFloat(Object, TEXT("blend_width"), Regions.BlendWidth);
        Object->SetBoolField(TEXT("has_global_structural_params"),
                             Regions.bHasGlobalStructuralParams);
        Object->SetNumberField(TEXT("structural_param_block"),
                               static_cast<int32>(Regions.StructuralParamBlock));
        SetMetricFloat(Object, TEXT("strate_top_world_z"), Regions.StrateTopWorldZ);
        SetMetricFloat(Object, TEXT("strate_bottom_world_z"), Regions.StrateBottomWorldZ);
        SetMetricFloat(Object, TEXT("boundary_seal_thickness"),
                       Regions.BoundarySealThickness);
        SetMetricFloat(Object, TEXT("base_density"), Regions.BaseDensity);

        TArray<TSharedPtr<FJsonValue>> RegionValues;
        for (const FVoxelStrateRegion& Region : Regions.Regions)
        {
            TSharedPtr<FJsonObject> RegionObject = MakeShared<FJsonObject>();
            RegionObject->SetNumberField(TEXT("region_index"), Region.RegionIndex);
            RegionObject->SetNumberField(TEXT("seed"), Region.Seed);
            RegionObject->SetStringField(TEXT("archetype"), ArchetypeName(Region.Archetype));
            RegionObject->SetNumberField(TEXT("archetype_id"),
                                         static_cast<int32>(Region.Archetype));
            RegionObject->SetBoolField(TEXT("uses_recipe"), Region.bUsesRecipe);
            RegionObject->SetObjectField(TEXT("recipe"), SerializeRecipe(Region.Recipe));
            RegionObject->SetObjectField(TEXT("parameters"),
                                         SerializeAllParams(Region.ArchetypeParams));
            RegionValues.Add(MakeShared<FJsonValueObject>(MoveTemp(RegionObject)));
        }
        Object->SetArrayField(TEXT("regions"), MoveTemp(RegionValues));
        return Object;
    }

    bool ReadJsonUInt32(const TSharedPtr<FJsonObject>& Object,
                        const TCHAR* Name, uint32& OutValue)
    {
        double Number = 0.0;
        if (!ReadJsonNumber(Object, Name, Number)
            || !FMath::IsFinite(Number)
            || Number < 0.0 || Number > 4294967295.0
            || FMath::FloorToDouble(Number) != Number)
        {
            return false;
        }
        OutValue = static_cast<uint32>(Number);
        return true;
    }

    bool DeserializeRegionManifest(const TSharedPtr<FJsonObject>& Object,
                                   FVoxelStrateRegionManifest& OutRegions)
    {
        if (!Object.IsValid()) return false;
        OutRegions = FVoxelStrateRegionManifest();
        bool JsonValid = false;
        int32 StructuralBlock = 0;
        bool bFieldsValid = ReadJsonBool(Object, TEXT("valid"), JsonValid)
            && ReadJsonString(Object, TEXT("failure_reason"), OutRegions.FailureReason)
            && ReadJsonInt32(Object, TEXT("seed"), OutRegions.Seed)
            && ReadJsonInt32(Object, TEXT("strate_index"), OutRegions.StrateIndex)
            && ReadJsonInt32(Object, TEXT("region_count"), OutRegions.RegionCount)
            && ReadJsonUInt32(Object, TEXT("partition_seed"), OutRegions.PartitionSeed)
            && ReadMetricFloat(Object, TEXT("lattice_cell_size"), OutRegions.LatticeCellSize)
            && ReadMetricFloat(Object, TEXT("blend_width"), OutRegions.BlendWidth)
            && ReadJsonBool(Object, TEXT("has_global_structural_params"),
                            OutRegions.bHasGlobalStructuralParams)
            && ReadJsonInt32(Object, TEXT("structural_param_block"), StructuralBlock)
            && ReadMetricFloat(Object, TEXT("strate_top_world_z"), OutRegions.StrateTopWorldZ)
            && ReadMetricFloat(Object, TEXT("strate_bottom_world_z"),
                               OutRegions.StrateBottomWorldZ)
            && ReadMetricFloat(Object, TEXT("boundary_seal_thickness"),
                               OutRegions.BoundarySealThickness)
            && ReadMetricFloat(Object, TEXT("base_density"), OutRegions.BaseDensity);
        if (StructuralBlock < static_cast<int32>(EVoxelStrateParamBlock::None)
            || StructuralBlock > static_cast<int32>(EVoxelStrateParamBlock::FloatingIsland))
        {
            bFieldsValid = false;
        }
        OutRegions.StructuralParamBlock = static_cast<EVoxelStrateParamBlock>(StructuralBlock);

        const TArray<TSharedPtr<FJsonValue>>* RegionValues = nullptr;
        bFieldsValid = ReadJsonArray(Object, TEXT("regions"), RegionValues) && bFieldsValid;
        if (RegionValues == nullptr
            || OutRegions.RegionCount < 1 || OutRegions.RegionCount > 3
            || RegionValues->Num() != OutRegions.RegionCount)
        {
            bFieldsValid = false;
        }

        OutRegions.Regions.Reset();
        if (RegionValues != nullptr)
        {
            OutRegions.Regions.Reserve(RegionValues->Num());
            for (int32 Index = 0; Index < RegionValues->Num(); ++Index)
            {
                const TSharedPtr<FJsonValue>& Value = (*RegionValues)[Index];
                if (!Value.IsValid() || Value->Type != EJson::Object)
                {
                    bFieldsValid = false;
                    continue;
                }
                const TSharedPtr<FJsonObject> RegionObject = Value->AsObject();
                FVoxelStrateRegion& Region = OutRegions.Regions.AddDefaulted_GetRef();
                FString ArchetypeText;
                int32 ArchetypeId = -1;
                TSharedPtr<FJsonObject> Recipe, Params;
                bFieldsValid = ReadJsonInt32(RegionObject, TEXT("region_index"),
                                             Region.RegionIndex) && bFieldsValid;
                bFieldsValid = ReadJsonInt32(RegionObject, TEXT("seed"), Region.Seed)
                    && bFieldsValid;
                bFieldsValid = ReadJsonString(RegionObject, TEXT("archetype"), ArchetypeText)
                    && bFieldsValid;
                bFieldsValid = ReadJsonInt32(RegionObject, TEXT("archetype_id"), ArchetypeId)
                    && bFieldsValid;
                bFieldsValid = ReadJsonBool(RegionObject, TEXT("uses_recipe"),
                                            Region.bUsesRecipe) && bFieldsValid;
                bFieldsValid = ReadJsonObject(RegionObject, TEXT("recipe"), Recipe)
                    && bFieldsValid;
                bFieldsValid = ReadJsonObject(RegionObject, TEXT("parameters"), Params)
                    && bFieldsValid;
                bFieldsValid = ParseArchetype(ArchetypeText, Region.Archetype)
                    && bFieldsValid;
                bFieldsValid = Region.RegionIndex == Index && bFieldsValid;
                bFieldsValid = ArchetypeId == static_cast<int32>(Region.Archetype)
                    && bFieldsValid;
                if (Region.bUsesRecipe)
                {
                    bFieldsValid = DeserializeRecipe(Recipe, Region.Recipe) && bFieldsValid;
                }
                bFieldsValid = DeserializeAllParams(Params, Region.ArchetypeParams)
                    && bFieldsValid;
            }
        }

        OutRegions.bValid = JsonValid;
        return bFieldsValid && OutRegions.IsValid();
    }

    TSharedPtr<FJsonObject> SerializeStrate(const FVoxelSeasonStrate& Strate)
    {
        TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
        Object->SetNumberField(TEXT("depth_index"), Strate.DepthIndex);
        Object->SetNumberField(TEXT("candidate_index"), Strate.CandidateIndex);
        Object->SetNumberField(TEXT("seed"), Strate.Seed);
        Object->SetNumberField(TEXT("height_in_chunks"), Strate.HeightInChunks);
        Object->SetNumberField(TEXT("top_world_z"), Strate.TopWorldZ);
        Object->SetNumberField(TEXT("bottom_world_z"), Strate.BottomWorldZ);
        Object->SetStringField(TEXT("archetype"), ArchetypeName(Strate.Archetype));
        Object->SetNumberField(TEXT("archetype_id"), static_cast<int32>(Strate.Archetype));
        Object->SetBoolField(TEXT("uses_recipe"), Strate.bUsesRecipe);
        Object->SetBoolField(TEXT("uses_regions"), Strate.bUsesRegions);
        Object->SetObjectField(TEXT("recipe"), SerializeRecipe(Strate.Recipe));
        Object->SetObjectField(TEXT("parameters"), SerializeAllParams(Strate.Params));
        // Keep old/single-region artifacts compact and on the exact legacy rebuild path.  A
        // multi-region strate carries the complete parent manifest because that is its runtime
        // identity; an invalid default Regions object must not make fixed legacy entries unloadable.
        if (Strate.bUsesRegions)
        {
            Object->SetObjectField(TEXT("regions"), SerializeRegionManifest(Strate.Regions));
        }
        Object->SetObjectField(TEXT("measured_metrics"), SerializeMetrics(Strate.Metrics));
        Object->SetNumberField(TEXT("distance_from_corpus_centroid"), Strate.DistanceFromCorpusCentroid);
        Object->SetStringField(TEXT("selection_reason"), Strate.SelectionReason);
        Object->SetStringField(TEXT("source_definition_path"), Strate.SourceDefinitionPath);
        Object->SetBoolField(TEXT("grounded"), Strate.bGrounded);
        Object->SetBoolField(TEXT("outlier"), Strate.bOutlier);
        Object->SetBoolField(TEXT("boss_slot"), Strate.bBossSlot);
        Object->SetBoolField(TEXT("fixed"), Strate.bFixed);
        Object->SetBoolField(TEXT("passed_non_vacuous"), Strate.bPassedNonVacuous);
        Object->SetBoolField(TEXT("passed_largest_component"), Strate.bPassedLargestComponent);
        Object->SetBoolField(TEXT("passed_primordial_law"), Strate.bPassedPrimordialLaw);
        Object->SetStringField(TEXT("primordial_law_result"), ConnectivityName(Strate.PrimordialLawResult));
        return Object;
    }

    bool DeserializeStrate(const TSharedPtr<FJsonObject>& Object,
                           FVoxelSeasonStrate& OutStrate)
    {
        if (!Object.IsValid()) return false;
        FString ArchetypeText, Reason, SourcePath, LawText;
        int32 ArchetypeId = -1;
        bool bValid = ReadJsonInt32(Object, TEXT("depth_index"), OutStrate.DepthIndex)
            && ReadJsonInt32(Object, TEXT("candidate_index"), OutStrate.CandidateIndex)
            && ReadJsonInt32(Object, TEXT("seed"), OutStrate.Seed)
            && ReadJsonInt32(Object, TEXT("height_in_chunks"), OutStrate.HeightInChunks)
            && ReadJsonInt32(Object, TEXT("top_world_z"), OutStrate.TopWorldZ)
            && ReadJsonInt32(Object, TEXT("bottom_world_z"), OutStrate.BottomWorldZ)
            && ReadJsonString(Object, TEXT("archetype"), ArchetypeText)
            && ReadJsonInt32(Object, TEXT("archetype_id"), ArchetypeId)
            && ReadJsonBool(Object, TEXT("uses_recipe"), OutStrate.bUsesRecipe)
            && ReadJsonString(Object, TEXT("selection_reason"), Reason)
            && ReadJsonString(Object, TEXT("source_definition_path"), SourcePath)
            && ReadJsonBool(Object, TEXT("grounded"), OutStrate.bGrounded)
            && ReadJsonBool(Object, TEXT("outlier"), OutStrate.bOutlier)
            && ReadJsonBool(Object, TEXT("boss_slot"), OutStrate.bBossSlot)
            && ReadJsonBool(Object, TEXT("fixed"), OutStrate.bFixed)
            && ReadJsonBool(Object, TEXT("passed_non_vacuous"), OutStrate.bPassedNonVacuous)
            && ReadJsonBool(Object, TEXT("passed_largest_component"), OutStrate.bPassedLargestComponent)
            && ReadJsonBool(Object, TEXT("passed_primordial_law"), OutStrate.bPassedPrimordialLaw)
            && ReadJsonString(Object, TEXT("primordial_law_result"), LawText);
        double Distance = 0.0;
        bValid = ReadJsonNumber(Object, TEXT("distance_from_corpus_centroid"), Distance) && bValid;
        OutStrate.DistanceFromCorpusCentroid = Distance;
        OutStrate.SelectionReason = Reason;
        OutStrate.SourceDefinitionPath = SourcePath;
        TSharedPtr<FJsonObject> Recipe, Params, Metrics, RegionsObject;
        bValid = ReadJsonObject(Object, TEXT("recipe"), Recipe) && bValid;
        bValid = ReadJsonObject(Object, TEXT("parameters"), Params) && bValid;
        bValid = ReadJsonObject(Object, TEXT("measured_metrics"), Metrics) && bValid;
        const TSharedPtr<FJsonValue>* RegionsValue = Object->Values.Find(TEXT("regions"));
        if (RegionsValue != nullptr)
        {
            bValid = ReadJsonObject(Object, TEXT("regions"), RegionsObject) && bValid;
            if (RegionsObject.IsValid())
            {
                bValid = DeserializeRegionManifest(RegionsObject, OutStrate.Regions)
                    && bValid;
            }
        }
        const TSharedPtr<FJsonValue>* UsesRegionsValue =
            Object->Values.Find(TEXT("uses_regions"));
        if (UsesRegionsValue != nullptr)
        {
            bValid = ReadJsonBool(Object, TEXT("uses_regions"), OutStrate.bUsesRegions)
                && bValid;
        }
        else
        {
            // Schema-1 manifests predating lateral regions simply take the old path.  A region
            // object is optional for backward compatibility, and never implicitly changes that
            // path without the explicit opt-in bit.
            OutStrate.bUsesRegions = false;
        }
        bValid = ParseArchetype(ArchetypeText, OutStrate.Archetype) && bValid;
        bValid = ArchetypeId == static_cast<int32>(OutStrate.Archetype) && bValid;
        bValid = ParseConnectivityName(LawText, OutStrate.PrimordialLawResult) && bValid;
        if (OutStrate.bUsesRecipe)
        {
            bValid = DeserializeRecipe(Recipe, OutStrate.Recipe) && bValid;
        }
        bValid = DeserializeAllParams(Params, OutStrate.Params) && bValid;
        bValid = DeserializeMetrics(Metrics, OutStrate.Metrics) && bValid;
        if (OutStrate.bUsesRegions
            && (!OutStrate.Regions.IsValid() || OutStrate.Regions.RegionCount <= 1))
        {
            bValid = false;
        }
        return bValid;
    }

}

namespace
{
    FString SerializeVoxelSeasonManifestInternal(
        const FVoxelSeasonManifest& Manifest, bool bIncludeContentHash)
    {
        using namespace VoxelSeasonManifestPrivate;
        TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
        SetCommonManifestFields(Root, Manifest);

        TArray<TSharedPtr<FJsonValue>> Strates;
        for (const FVoxelSeasonStrate& Strate : Manifest.Strates)
        {
            Strates.Add(MakeShared<FJsonValueObject>(SerializeStrate(Strate)));
        }
        Root->SetArrayField(TEXT("strates"), MoveTemp(Strates));

        TArray<TSharedPtr<FJsonValue>> Rejections;
        for (const FVoxelSeasonRejectionCount& Rejection : Manifest.RejectionCounts)
        {
            TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
            Object->SetStringField(TEXT("reason"), Rejection.Reason);
            Object->SetNumberField(TEXT("count"), Rejection.Count);
            Rejections.Add(MakeShared<FJsonValueObject>(Object));
        }
        Root->SetArrayField(TEXT("rejection_counts"), MoveTemp(Rejections));

        if (!Manifest.CandidateAudit.IsEmpty())
        {
            TArray<TSharedPtr<FJsonValue>> Audit;
            for (const FVoxelSeasonCandidateAudit& Row : Manifest.CandidateAudit)
            {
                TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
                Object->SetNumberField(TEXT("candidate_index"), Row.CandidateIndex);
                Object->SetNumberField(TEXT("seed"), Row.Seed);
                Object->SetStringField(TEXT("archetype"), ArchetypeName(Row.Archetype));
                Object->SetNumberField(TEXT("recipe_hash"), static_cast<double>(Row.RecipeHash));
                Object->SetNumberField(TEXT("distance_from_corpus_centroid"), Row.DistanceFromCorpusCentroid);
                Object->SetBoolField(TEXT("passed_hard_gates"), Row.bPassedHardGates);
                Object->SetBoolField(TEXT("selected"), Row.bSelected);
                Object->SetStringField(TEXT("rejection_reason"), Row.RejectionReason);
                Audit.Add(MakeShared<FJsonValueObject>(Object));
            }
            Root->SetArrayField(TEXT("candidate_audit"), MoveTemp(Audit));
        }

        if (bIncludeContentHash)
        {
            const FString Payload = SerializeVoxelSeasonManifestInternal(Manifest, false);
            if (Payload.IsEmpty()) return FString();
            FTCHARToUTF8 Utf8(*Payload);
            uint8 Digest[FSHA1::DigestSize];
            FSHA1::HashBuffer(Utf8.Get(), Utf8.Length(), Digest);
            Root->SetStringField(TEXT("content_hash"), BytesToHex(Digest, FSHA1::DigestSize));
        }

        FString JsonText;
        TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
            TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&JsonText);
        if (!FJsonSerializer::Serialize(Root.ToSharedRef(), Writer) || !Writer->Close())
        {
            return FString();
        }
        return JsonText;
    }
}

FString VF_ComputeVoxelSeasonManifestContentHash(const FVoxelSeasonManifest& Manifest)
{
    const FString Payload = SerializeVoxelSeasonManifestInternal(Manifest, false);
    if (Payload.IsEmpty()) return FString();
    FTCHARToUTF8 Utf8(*Payload);
    uint8 Digest[FSHA1::DigestSize];
    FSHA1::HashBuffer(Utf8.Get(), Utf8.Length(), Digest);
    return BytesToHex(Digest, FSHA1::DigestSize);
}

FString VF_SerializeVoxelSeasonManifest(const FVoxelSeasonManifest& Manifest)
{
    return SerializeVoxelSeasonManifestInternal(Manifest, true);
}

bool VF_SaveVoxelSeasonManifest(const FString& ManifestPath,
                                const FVoxelSeasonManifest& Manifest,
                                FString& OutReport)
{
    if (ManifestPath.IsEmpty())
    {
        OutReport = TEXT("season manifest path is empty");
        return false;
    }
    const FString JsonText = VF_SerializeVoxelSeasonManifest(Manifest);
    if (JsonText.IsEmpty())
    {
        OutReport = TEXT("could not serialize season manifest");
        return false;
    }
    const FString Directory = FPaths::GetPath(ManifestPath);
    if (!Directory.IsEmpty() && !IFileManager::Get().MakeDirectory(*Directory, true))
    {
        OutReport = FString::Printf(TEXT("could not create manifest directory %s"), *Directory);
        return false;
    }
    if (!FFileHelper::SaveStringToFile(JsonText, *ManifestPath,
                                       FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
    {
        OutReport = FString::Printf(TEXT("could not write season manifest %s"), *ManifestPath);
        return false;
    }
    OutReport = FString::Printf(TEXT("saved season manifest to %s"), *ManifestPath);
    return true;
}

bool VF_DeserializeVoxelSeasonManifest(const FString& ManifestJson,
                                       FVoxelSeasonManifest& OutManifest,
                                       FString& OutReport)
{
    using namespace VoxelSeasonManifestPrivate;
    OutManifest = FVoxelSeasonManifest();
    OutReport.Reset();
    if (ManifestJson.IsEmpty())
    {
        OutReport = TEXT("season manifest JSON is empty");
        OutManifest.Error = OutReport;
        return false;
    }

    TSharedPtr<FJsonObject> Root;
    const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ManifestJson);
    if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
    {
        OutReport = TEXT("season manifest JSON is malformed");
        OutManifest.Error = OutReport;
        return false;
    }

    FString Format;
    int32 SchemaVersion = 0;
    bool bValid = false;
    if (!ReadJsonString(Root, TEXT("format"), Format)
        || Format != TEXT("VoxelForgeSeasonManifest")
        || !ReadJsonBool(Root, TEXT("valid"), bValid)
        || !ReadJsonInt32(Root, TEXT("schema_version"), SchemaVersion))
    {
        OutReport = TEXT("season manifest header is missing or malformed");
        OutManifest.Error = OutReport;
        return false;
    }
    if (SchemaVersion != FVoxelSeasonManifest::CurrentSchemaVersion)
    {
        OutReport = FString::Printf(TEXT("unsupported season manifest schema %d (expected %d)"),
                                     SchemaVersion, FVoxelSeasonManifest::CurrentSchemaVersion);
        OutManifest.Error = OutReport;
        return false;
    }

    bool bFieldsValid = true;
    OutManifest.bValid = bValid;
    OutManifest.SchemaVersion = SchemaVersion;
    bFieldsValid = ReadJsonString(Root, TEXT("content_hash"), OutManifest.ContentHash)
        && bFieldsValid;
    bFieldsValid = ReadJsonInt32(Root, TEXT("season"), OutManifest.Season) && bFieldsValid;
    bFieldsValid = ReadJsonInt32(Root, TEXT("seed"), OutManifest.Seed) && bFieldsValid;
    double CorpusHash = 0.0;
    bFieldsValid = ReadJsonNumber(Root, TEXT("input_corpus_hash"), CorpusHash) && bFieldsValid;
    if (CorpusHash < 0.0 || CorpusHash > 4294967295.0
        || FMath::FloorToDouble(CorpusHash) != CorpusHash)
    {
        bFieldsValid = false;
    }
    OutManifest.InputCorpusHash = static_cast<uint32>(CorpusHash);
    bFieldsValid = ReadJsonInt32(Root, TEXT("candidate_count"), OutManifest.CandidateCount) && bFieldsValid;
    bFieldsValid = ReadJsonInt32(Root, TEXT("survivor_count"), OutManifest.SurvivorCount) && bFieldsValid;
    bFieldsValid = ReadJsonInt32(Root, TEXT("selected_count"), OutManifest.SelectedCount) && bFieldsValid;
    bFieldsValid = ReadJsonInt32(Root, TEXT("rejected_count"), OutManifest.RejectedCount) && bFieldsValid;
    bFieldsValid = ReadJsonInt32(Root, TEXT("grounded_selected_count"), OutManifest.GroundedSelectedCount) && bFieldsValid;
    bFieldsValid = ReadJsonInt32(Root, TEXT("outlier_selected_count"), OutManifest.OutlierSelectedCount) && bFieldsValid;
    bFieldsValid = ReadJsonInt32(Root, TEXT("fixed_selected_count"), OutManifest.FixedSelectedCount) && bFieldsValid;
    bFieldsValid = ReadJsonInt32(Root, TEXT("total_strates"), OutManifest.TotalStrates) && bFieldsValid;
    bFieldsValid = ReadJsonInt32(Root, TEXT("inter_strate_gap_chunks"), OutManifest.InterStrateGapChunks) && bFieldsValid;
    bFieldsValid = ReadMetricFloat(Root, TEXT("origin_spine_radius"), OutManifest.OriginSpineRadius) && bFieldsValid;
    bFieldsValid = ReadMetricFloat(Root, TEXT("world_radius_voxels"), OutManifest.WorldRadiusVoxels) && bFieldsValid;
    TSharedPtr<FJsonObject> Policy;
    bFieldsValid = ReadJsonObject(Root, TEXT("selection_policy"), Policy) && bFieldsValid;
    if (Policy.IsValid())
    {
        bFieldsValid = ReadPolicy(Policy, OutManifest.SelectionPolicy) && bFieldsValid;
    }
    bFieldsValid = ReadJsonString(Root, TEXT("policy_note"), OutManifest.PolicyNote) && bFieldsValid;

    const TArray<TSharedPtr<FJsonValue>>* StrateValues = nullptr;
    const TArray<TSharedPtr<FJsonValue>>* RejectionValues = nullptr;
    bFieldsValid = ReadJsonArray(Root, TEXT("strates"), StrateValues) && bFieldsValid;
    bFieldsValid = ReadJsonArray(Root, TEXT("rejection_counts"), RejectionValues) && bFieldsValid;
    if (StrateValues != nullptr)
    {
        OutManifest.Strates.Reset();
        OutManifest.Strates.Reserve(StrateValues->Num());
        for (const TSharedPtr<FJsonValue>& Value : *StrateValues)
        {
            if (!Value.IsValid() || Value->Type != EJson::Object)
            {
                bFieldsValid = false;
                continue;
            }
            FVoxelSeasonStrate& Strate = OutManifest.Strates.AddDefaulted_GetRef();
            if (!DeserializeStrate(Value->AsObject(), Strate))
            {
                bFieldsValid = false;
            }
        }
    }
    if (RejectionValues != nullptr)
    {
        OutManifest.RejectionCounts.Reset();
        OutManifest.RejectionCounts.Reserve(RejectionValues->Num());
        for (const TSharedPtr<FJsonValue>& Value : *RejectionValues)
        {
            if (!Value.IsValid() || Value->Type != EJson::Object)
            {
                bFieldsValid = false;
                continue;
            }
            TSharedPtr<FJsonObject> Object = Value->AsObject();
            FVoxelSeasonRejectionCount& Row = OutManifest.RejectionCounts.AddDefaulted_GetRef();
            bFieldsValid = ReadJsonString(Object, TEXT("reason"), Row.Reason) && bFieldsValid;
            bFieldsValid = ReadJsonInt32(Object, TEXT("count"), Row.Count) && bFieldsValid;
        }
    }

    const TArray<TSharedPtr<FJsonValue>>* AuditValues = nullptr;
    if (ReadJsonArray(Root, TEXT("candidate_audit"), AuditValues))
    {
        OutManifest.CandidateAudit.Reset();
        OutManifest.CandidateAudit.Reserve(AuditValues->Num());
        for (const TSharedPtr<FJsonValue>& Value : *AuditValues)
        {
            if (!Value.IsValid() || Value->Type != EJson::Object)
            {
                bFieldsValid = false;
                continue;
            }
            TSharedPtr<FJsonObject> Object = Value->AsObject();
            FVoxelSeasonCandidateAudit& Row = OutManifest.CandidateAudit.AddDefaulted_GetRef();
            FString ArchetypeText;
            double Distance = 0.0;
            bFieldsValid = ReadJsonInt32(Object, TEXT("candidate_index"), Row.CandidateIndex) && bFieldsValid;
            bFieldsValid = ReadJsonInt32(Object, TEXT("seed"), Row.Seed) && bFieldsValid;
            bFieldsValid = ReadJsonString(Object, TEXT("archetype"), ArchetypeText) && bFieldsValid;
            double RecipeHash = 0.0;
            bFieldsValid = ReadJsonNumber(Object, TEXT("recipe_hash"), RecipeHash) && bFieldsValid;
            bFieldsValid = RecipeHash >= 0.0 && RecipeHash <= 4294967295.0
                && FMath::FloorToDouble(RecipeHash) == RecipeHash && bFieldsValid;
            Row.RecipeHash = static_cast<uint32>(RecipeHash);
            bFieldsValid = ReadJsonNumber(Object, TEXT("distance_from_corpus_centroid"), Distance) && bFieldsValid;
            Row.DistanceFromCorpusCentroid = Distance;
            bFieldsValid = ReadJsonBool(Object, TEXT("passed_hard_gates"), Row.bPassedHardGates) && bFieldsValid;
            bFieldsValid = ReadJsonBool(Object, TEXT("selected"), Row.bSelected) && bFieldsValid;
            bFieldsValid = ReadJsonString(Object, TEXT("rejection_reason"), Row.RejectionReason) && bFieldsValid;
            bFieldsValid = ParseArchetype(ArchetypeText, Row.Archetype) && bFieldsValid;
        }
    }

    if (OutManifest.Strates.Num() != OutManifest.SelectedCount
        || OutManifest.SelectedCount <= 0
        || OutManifest.WorldRadiusVoxels != 0.0f)
    {
        bFieldsValid = false;
    }
    for (int32 Index = 0; Index < OutManifest.Strates.Num(); ++Index)
    {
        const FVoxelSeasonStrate& Strate = OutManifest.Strates[Index];
        const float StoredTop = static_cast<float>(Strate.TopWorldZ);
        const float StoredBottom = static_cast<float>(Strate.BottomWorldZ);
        if (Strate.DepthIndex != Index || Strate.bUsesRegions || !Strate.bPassedPrimordialLaw
            || Strate.PrimordialLawResult != EVoxelConnectivityResult::Connected)
        {
            bFieldsValid = false;
            break;
        }
        if (Strate.bUsesRegions
            && (!Strate.Regions.IsValid()
                || Strate.Regions.RegionCount <= 1
                || Strate.Regions.Seed != Strate.Seed
                || Strate.Regions.StrateIndex != Strate.DepthIndex
                || Strate.Regions.PartitionSeed != VF_GetStrateRegionPartitionSeed(
                    Strate.Seed, Strate.DepthIndex)
                || !Strate.Regions.bHasGlobalStructuralParams
                || FMemory::Memcmp(&Strate.Regions.StrateTopWorldZ,
                                   &StoredTop, sizeof(float)) != 0
                || FMemory::Memcmp(&Strate.Regions.StrateBottomWorldZ,
                                   &StoredBottom, sizeof(float)) != 0)
        )
        {
            bFieldsValid = false;
            break;
        }
    }
    if (!bFieldsValid)
    {
        OutReport = TEXT("season manifest contains invalid fields");
        OutManifest.Error = OutReport;
        OutManifest.bValid = false;
        return false;
    }
    const FString ComputedHash = VF_ComputeVoxelSeasonManifestContentHash(OutManifest);
    if (ComputedHash.IsEmpty()
        || !ComputedHash.Equals(OutManifest.ContentHash, ESearchCase::IgnoreCase))
    {
        OutReport = FString::Printf(
            TEXT("season manifest content hash mismatch (stored %s, computed %s)"),
            *OutManifest.ContentHash, *ComputedHash);
        OutManifest.Error = OutReport;
        OutManifest.bValid = false;
        return false;
    }
    OutManifest.SerializedJson = ManifestJson;
    OutReport = FString::Printf(TEXT("loaded season manifest hash %s"), *OutManifest.ContentHash);
    return OutManifest.IsUsable();
}

bool VF_LoadVoxelSeasonManifest(const FString& ManifestPath,
                                FVoxelSeasonManifest& OutManifest,
                                FString& OutReport)
{
    FString JsonText;
    if (ManifestPath.IsEmpty())
    {
        OutReport = TEXT("season manifest path is empty");
        OutManifest = FVoxelSeasonManifest();
        OutManifest.Error = OutReport;
        return false;
    }
    if (!FFileHelper::LoadFileToString(JsonText, *ManifestPath))
    {
        OutReport = FString::Printf(TEXT("could not read season manifest %s"), *ManifestPath);
        OutManifest = FVoxelSeasonManifest();
        OutManifest.Error = OutReport;
        return false;
    }
    if (!VF_DeserializeVoxelSeasonManifest(JsonText, OutManifest, OutReport))
    {
        OutReport = FString::Printf(TEXT("season manifest %s: %s"), *ManifestPath, *OutReport);
        OutManifest.Error = OutReport;
        return false;
    }
    OutManifest.ManifestPath = ManifestPath;
    OutReport = FString::Printf(TEXT("loaded season manifest from %s (hash %s)"),
                                *ManifestPath, *OutManifest.ContentHash);
    return true;
}

namespace VoxelSeasonRuntimePrivate
{
    float ActiveTop(const FVoxelStrateArchetypeParams& Params, ECaveGeneratorType Archetype)
    {
        switch (Archetype)
        {
        case ECaveGeneratorType::TunnelNetwork:
        case ECaveGeneratorType::Underwater:      return Params.TunnelNetworkParams.StrateTopWorldZ;
        case ECaveGeneratorType::FlatPlain:
        case ECaveGeneratorType::CrystalChamber:  return Params.SlabParams.StrateTopWorldZ;
        case ECaveGeneratorType::Maze:            return Params.MazeParams.StrateTopWorldZ;
        case ECaveGeneratorType::SurfaceWorld:    return Params.SurfaceParams.StrateTopWorldZ;
        case ECaveGeneratorType::VerticalShafts:  return Params.VerticalShaftParams.StrateTopWorldZ;
        case ECaveGeneratorType::FloatingIslands: return Params.FloatingIslandParams.StrateTopWorldZ;
        }
        return 0.0f;
    }

    float ActiveBottom(const FVoxelStrateArchetypeParams& Params, ECaveGeneratorType Archetype)
    {
        switch (Archetype)
        {
        case ECaveGeneratorType::TunnelNetwork:
        case ECaveGeneratorType::Underwater:      return Params.TunnelNetworkParams.StrateBottomWorldZ;
        case ECaveGeneratorType::FlatPlain:
        case ECaveGeneratorType::CrystalChamber:  return Params.SlabParams.StrateBottomWorldZ;
        case ECaveGeneratorType::Maze:            return Params.MazeParams.StrateBottomWorldZ;
        case ECaveGeneratorType::SurfaceWorld:    return Params.SurfaceParams.StrateBottomWorldZ;
        case ECaveGeneratorType::VerticalShafts:  return Params.VerticalShaftParams.StrateBottomWorldZ;
        case ECaveGeneratorType::FloatingIslands: return Params.FloatingIslandParams.StrateBottomWorldZ;
        }
        return 0.0f;
    }

    bool SameFloatBits(float A, float B)
    {
        return FMemory::Memcmp(&A, &B, sizeof(float)) == 0;
    }
}

#if WITH_EDITOR
namespace VoxelSeasonCompositionPrivate
{
    float ActiveTop(const FVoxelStrateArchetypeParams& Params, ECaveGeneratorType Archetype);
    float ActiveBottom(const FVoxelStrateArchetypeParams& Params, ECaveGeneratorType Archetype);
    float ActiveSeal(const FVoxelStrateArchetypeParams& Params, ECaveGeneratorType Archetype);
    void SetRuntimeBounds(FVoxelStrateArchetypeParams& Params, float TopWorldZ, float BottomWorldZ);
    void SetRegionRuntimeBounds(FVoxelStrateRegionManifest& Regions,
                                int32 Seed, int32 StrateIndex,
                                float TopWorldZ, float BottomWorldZ);
    const TCHAR* RejectionReasonForLaw(EVoxelConnectivityResult Result);

    // These caps are policy/allocator guards, not gameplay limits. They keep a malformed build-box
    // request from turning the candidate audit or Z-layout arithmetic into an unbounded allocation.
    constexpr int32 MaxCandidateCount = 4096;
    constexpr int32 MaxSelectedStrateCount = 256;
    constexpr int32 MaxStrateHeightInChunks = 4096;

#if WITH_EDITOR
    struct FSeasonCandidateState
    {
        FVoxelStrateComposerCandidate Candidate;
        FVoxelStrateMeasuredMetrics Metrics;
        double DistanceFromCorpusCentroid = 0.0;
        bool bPassedHardGates = false;
        bool bSelected = false;
        bool bUnavailable = false;
        bool bOutlierPool = false;
        FString RejectionReason;
    };
#endif

    struct FSeasonEvaluation
    {
        bool bPassedNonVacuous = false;
        bool bPassedLargestComponent = false;
        bool bPassedPrimordialLaw = false;
        EVoxelConnectivityResult PrimordialLawResult = EVoxelConnectivityResult::OutOfWindow;
        FVoxelStrateMeasuredMetrics Metrics;
        FString FailureReason;
    };

    class FSeasonStackSampler final : public IVoxelStrateDensitySampler
    {
    public:
        explicit FSeasonStackSampler(const FVoxelOpStack& InStack)
            : Stack(InStack)
        {
        }

        float SampleDensity(float WorldX, float WorldY, float WorldZ) const override
        {
            // FVoxelOpStack::EvalMC is the established MC-facing convention: negative solid,
            // positive air. No generation path is called here; this class exists only for the
            // offline measurement API.
            return Stack.EvalMC(WorldX, WorldY, WorldZ);
        }

    private:
        const FVoxelOpStack& Stack;
    };

    bool IsFinitePolicy(const FVoxelSeasonSelectionPolicy& Policy)
    {
        const float FractionSum = Policy.GroundedFraction + Policy.OutlierFraction;
        return FMath::IsFinite(Policy.GroundedFraction)
            && FMath::IsFinite(Policy.OutlierFraction)
            && Policy.GroundedFraction >= 0.0f
            && Policy.OutlierFraction >= 0.0f
            && FMath::IsFinite(FractionSum) && FractionSum > 0.0f
            && FMath::IsFinite(Policy.AdjacentSimilarityDistanceThreshold)
            && Policy.AdjacentSimilarityDistanceThreshold >= 0.0
            && FMath::IsFinite(Policy.AdjacentSimilarityPenaltyWeight)
            && Policy.AdjacentSimilarityPenaltyWeight >= 0.0
            && FMath::IsFinite(Policy.NewArchetypeBonus)
            && FMath::IsFinite(Policy.NewRecipeBonus)
            && Policy.BossSlotInterval >= 0
            && FMath::IsFinite(Policy.MinimumLargestComponentShare)
            && Policy.MinimumLargestComponentShare >= 0.0f
            && Policy.MinimumLargestComponentShare <= 1.0f
            && FMath::IsFinite(Policy.FeatureScaleNormalizationVoxels)
            && Policy.FeatureScaleNormalizationVoxels > 0.0f
            && FMath::IsFinite(Policy.ClearanceNormalizationVoxels)
            && Policy.ClearanceNormalizationVoxels > 0.0f;
    }

    bool IsValidMeasureSettings(const FVoxelStrateMeasureSettings& Settings)
    {
        return Settings.SampleStep > 0
            && Settings.RadiusInVoxels > 0
            && Settings.MaxCells > 0
            && Settings.MaxRouteRetries >= 0
            && Settings.HeadroomCells >= 0
            && Settings.InteriorMarginVoxels >= -1
            && FMath::IsFinite(Settings.CenterXY.X)
            && FMath::IsFinite(Settings.CenterXY.Y)
            && FMath::IsFinite(Settings.CoverMarginVoxels)
            && Settings.CoverMarginVoxels >= 0.0f
            && FMath::IsFinite(Settings.PlayerCapsuleRadiusVoxels)
            && Settings.PlayerCapsuleRadiusVoxels > 0.0f
            && FMath::IsFinite(Settings.PlayerCapsuleHalfHeightVoxels)
            && Settings.PlayerCapsuleHalfHeightVoxels > 0.0f
            && FMath::IsFinite(Settings.PlayerMaxStepHeightMeters)
            && Settings.PlayerMaxStepHeightMeters >= 0.0f
            && FMath::IsFinite(Settings.PlayerWalkableFloorAngleDegrees)
            && Settings.PlayerWalkableFloorAngleDegrees >= 0.0f
            && Settings.PlayerWalkableFloorAngleDegrees <= 90.0f
            && FMath::IsFinite(Settings.PlayerSupportPatchMinCoverageFraction)
            && Settings.PlayerSupportPatchMinCoverageFraction > 0.0f
            && Settings.PlayerSupportPatchMinCoverageFraction <= 1.0f;
    }

    bool IsBossSlot(int32 DepthIndex, int32 TotalStrates, int32 Interval)
    {
        // Slot zero is the surface entry, and the terminal slot is deliberately never a boss.
        return Interval > 0 && DepthIndex > 0 && DepthIndex < TotalStrates - 1
            && ((DepthIndex + 1) % Interval) == 0;
    }

    uint32 SeasonAttemptHash(int32 SeasonSeed, int32 CandidateIndex)
    {
        // The attempt index is deliberately folded into the hash. Reordering candidate work on
        // the build box therefore cannot accidentally turn an attempt into mutable RNG state.
        uint32 Hash = VoxelHash::Mix(static_cast<uint32>(SeasonSeed) ^ 0x53454153u);
        Hash = VoxelHash::Mix(Hash ^ (static_cast<uint32>(CandidateIndex) * 0x9E3779B9u));
        return VoxelHash::Mix(Hash ^ 0x54494552u);
    }

    FString PercentageText(float Fraction)
    {
        return FString::Printf(TEXT("%.0f%%"), static_cast<double>(Fraction * 100.0f));
    }

    void AddRejection(TMap<FString, int32>& Counts, const FString& Reason)
    {
        const FString Key = Reason.IsEmpty() ? TEXT("unspecified") : Reason;
        Counts.FindOrAdd(Key) += 1;
    }

    void ExportRejectionCounts(const TMap<FString, int32>& Counts,
                               TArray<FVoxelSeasonRejectionCount>& OutCounts)
    {
        TArray<FString> Reasons;
        Counts.GetKeys(Reasons);
        Reasons.Sort([](const FString& A, const FString& B) { return A < B; });
        OutCounts.Reset();
        for (const FString& Reason : Reasons)
        {
            FVoxelSeasonRejectionCount& Row = OutCounts.AddDefaulted_GetRef();
            Row.Reason = Reason;
            Row.Count = Counts.FindChecked(Reason);
        }
    }

    void CopyDefinitionParams(const UVoxelStrateDefinition& Definition,
                              FVoxelSeasonFixedStrate& OutFixed)
    {
        OutFixed = FVoxelSeasonFixedStrate();
        OutFixed.HeightInChunks = Definition.StrateHeightInChunks;
        OutFixed.Archetype = Definition.GeneratorType;
        OutFixed.SourceDefinitionPath = Definition.GetPathName();
        OutFixed.bUsesRecipe = false;
        OutFixed.Params.TunnelNetworkParams = Definition.GenerationParams;
        OutFixed.Params.SlabParams = Definition.SlabParams;
        OutFixed.Params.MazeParams = Definition.MazeParams;
        OutFixed.Params.SurfaceParams = Definition.SurfaceParams;
        OutFixed.Params.VerticalShaftParams = Definition.VerticalShaftParams;
        OutFixed.Params.FloatingIslandParams = Definition.FloatingIslandParams;
    }

    bool ResolveFixedStrates(const FVoxelSeasonCompositionSettings& Settings,
                             int32 SeasonSeed,
                             TMap<int32, FVoxelSeasonFixedStrate>& OutFixed,
                             FString& OutError)
    {
        OutFixed = Settings.FixedStrates;
        if (Settings.WorldSettings == nullptr)
        {
            return true;
        }

        for (const TPair<int32, TSoftObjectPtr<UVoxelStrateDefinition>>& Pair
             : Settings.WorldSettings->FixedStrates)
        {
            if (OutFixed.Contains(Pair.Key))
            {
                continue;
            }
            UVoxelStrateDefinition* Definition = Pair.Value.LoadSynchronous();
            if (Definition == nullptr)
            {
                OutError = FString::Printf(TEXT("fixed strate slot %d could not load definition %s"),
                                            Pair.Key, *Pair.Value.ToSoftObjectPath().ToString());
                return false;
            }
            FVoxelSeasonFixedStrate Fixed;
            CopyDefinitionParams(*Definition, Fixed);
            Fixed.Seed = SeasonSeed;
            if (Fixed.SourceDefinitionPath.IsEmpty())
            {
                Fixed.SourceDefinitionPath = Pair.Value.ToSoftObjectPath().ToString();
            }
            OutFixed.Add(Pair.Key, MoveTemp(Fixed));
        }
        return true;
    }

#if WITH_EDITOR
    void MakeCandidateStrate(const FVoxelStrateComposerCandidate& Candidate,
                             int32 CandidateIndex,
                             int32 HeightInChunks,
                             int32 TopWorldZ,
                             int32 BottomWorldZ,
                             FVoxelSeasonStrate& OutStrate)
    {
        OutStrate = FVoxelSeasonStrate();
        OutStrate.DepthIndex = INDEX_NONE;
        OutStrate.CandidateIndex = CandidateIndex;
        OutStrate.Seed = Candidate.Seed;
        OutStrate.HeightInChunks = HeightInChunks;
        OutStrate.TopWorldZ = TopWorldZ;
        OutStrate.BottomWorldZ = BottomWorldZ;
        OutStrate.Archetype = Candidate.Archetype;
        OutStrate.Recipe = Candidate.Recipe;
        OutStrate.Params = Candidate.ArchetypeParams;
        OutStrate.Regions = Candidate.Regions;
        OutStrate.bUsesRecipe = true;
        // Lateral composition remains a laboratory feature until its cross-seam law is perfect.
        // The season artifact must therefore stay on the proved single-stack runtime contract.
        OutStrate.bUsesRegions = VF_LateralRegionsAreShippable()
            && Candidate.Regions.RegionCount > 1;
        SetRuntimeBounds(OutStrate.Params, static_cast<float>(TopWorldZ),
                         static_cast<float>(BottomWorldZ));
        if (OutStrate.Regions.IsValid())
        {
            SetRegionRuntimeBounds(OutStrate.Regions, Candidate.Seed, CandidateIndex,
                                   static_cast<float>(TopWorldZ),
                                   static_cast<float>(BottomWorldZ));
        }
    }
#endif

    void MakeFixedStrate(const FVoxelSeasonFixedStrate& Fixed,
                         int32 DepthIndex,
                         int32 TopWorldZ,
                         int32 BottomWorldZ,
                         FVoxelSeasonStrate& OutStrate)
    {
        OutStrate = FVoxelSeasonStrate();
        OutStrate.DepthIndex = DepthIndex;
        OutStrate.CandidateIndex = INDEX_NONE;
        OutStrate.Seed = Fixed.Seed;
        OutStrate.HeightInChunks = Fixed.HeightInChunks;
        OutStrate.TopWorldZ = TopWorldZ;
        OutStrate.BottomWorldZ = BottomWorldZ;
        OutStrate.Archetype = Fixed.Archetype;
        OutStrate.Recipe = Fixed.Recipe;
        OutStrate.Params = Fixed.Params;
        OutStrate.Regions = Fixed.Regions;
        OutStrate.SourceDefinitionPath = Fixed.SourceDefinitionPath;
        OutStrate.bFixed = true;
        OutStrate.bUsesRecipe = Fixed.bUsesRecipe;
        OutStrate.bUsesRegions = Fixed.bUsesRegions && Fixed.Regions.IsValid();
        SetRuntimeBounds(OutStrate.Params, static_cast<float>(TopWorldZ),
                         static_cast<float>(BottomWorldZ));
        if (OutStrate.bUsesRegions)
        {
            SetRegionRuntimeBounds(OutStrate.Regions, Fixed.Seed, DepthIndex,
                                   static_cast<float>(TopWorldZ),
                                   static_cast<float>(BottomWorldZ));
        }
    }

    bool EvaluateStrate(const FVoxelSeasonStrate& Strate,
                        const FVoxelSeasonCompositionSettings& Settings,
                        FSeasonEvaluation& OutEvaluation,
                        FVoxelStrateSampleGrid* OutGrid = nullptr,
                        FVoxelStrateMetrics* OutRawMetrics = nullptr)
    {
        OutEvaluation = FSeasonEvaluation();
        if (OutGrid != nullptr)
        {
            *OutGrid = FVoxelStrateSampleGrid();
        }
        if (OutRawMetrics != nullptr)
        {
            *OutRawMetrics = FVoxelStrateMetrics();
        }

        FVoxelOpStack Stack;
        FVoxelOpContext Context;
        FString BuildError;
        bool bBuilt = false;
        if (Strate.bUsesRegions)
        {
            bBuilt = VF_BuildStrateRegionStack(
                Strate.Regions, Settings.OriginSpineRadius, nullptr,
                Stack, Context, &BuildError);
        }
        else if (Strate.bUsesRecipe)
        {
            bBuilt = VF_BuildStackFromRecipe(Strate.Recipe, Strate.Params, Strate.Seed,
                                              Settings.OriginSpineRadius, nullptr, Stack,
                                              Context, &BuildError);
        }
        else
        {
#if WITH_EDITOR
            const float Seal = ActiveSeal(Strate.Params, Strate.Archetype);
            bBuilt = VF_BuildNativeStrateStackForCandidate(
                Strate.Archetype, Strate.Params, Strate.Seed, Settings.OriginSpineRadius,
                0.0f, Seal, nullptr, Stack, Context);
            if (!bBuilt)
            {
                BuildError = TEXT("native fixed strate stack could not be materialised");
            }
#else
            BuildError = TEXT("native fixed strate validation is editor-only");
#endif
        }
        if (!bBuilt)
        {
            OutEvaluation.FailureReason = BuildError.IsEmpty()
                ? TEXT("recipe/native stack build failed") : BuildError;
            return false;
        }
        if (Context.WorldRadiusVoxels != 0.0f)
        {
            OutEvaluation.FailureReason = TEXT("WorldRadiusVoxels was not zero during composition");
            return false;
        }

        const float Seal = Context.EdgeSealThickness;
        if (!FMath::IsFinite(Seal) || Seal < 0.0f)
        {
            OutEvaluation.FailureReason = TEXT("boundary seal thickness is invalid");
            return false;
        }
        Stack.PrepareChunk(Context);
        FSeasonStackSampler Sampler(Stack);
        FVoxelStrateMetrics RawMetrics = VF_MeasureStrateWithSampler(
            Sampler, Strate.BottomWorldZ, Strate.TopWorldZ, Seal,
            Settings.MeasureSettings, OutGrid);
        if (OutRawMetrics != nullptr)
        {
            *OutRawMetrics = RawMetrics;
        }
        if (!RawMetrics.bValid)
        {
            OutEvaluation.FailureReason = RawMetrics.RefusalReason.IsEmpty()
                ? TEXT("measurement failed")
                : FString::Printf(TEXT("measurement failed: %s"), *RawMetrics.RefusalReason);
            return false;
        }

        OutEvaluation.bPassedNonVacuous = RawMetrics.NumSampled > 0
            && RawMetrics.NumAir > 0 && RawMetrics.NumSolid > 0;
        if (!OutEvaluation.bPassedNonVacuous)
        {
            OutEvaluation.FailureReason = TEXT("vacuous");
            return false;
        }

        OutEvaluation.bPassedLargestComponent =
            RawMetrics.LargestComponentShare >= Settings.SelectionPolicy.MinimumLargestComponentShare;
        if (!OutEvaluation.bPassedLargestComponent)
        {
            OutEvaluation.FailureReason = FString::Printf(
                TEXT("fragmented (largest component %.6f < %.6f)"),
                RawMetrics.LargestComponentShare,
                Settings.SelectionPolicy.MinimumLargestComponentShare);
            return false;
        }

        const float InteriorBottom = static_cast<float>(Strate.BottomWorldZ)
            + static_cast<float>(RawMetrics.ResolvedMarginVoxels);
        const float InteriorTop = static_cast<float>(Strate.TopWorldZ)
            - static_cast<float>(RawMetrics.ResolvedMarginVoxels);
        if (!FMath::IsFinite(InteriorBottom) || !FMath::IsFinite(InteriorTop)
            || InteriorTop - InteriorBottom <= 1.0f)
        {
            OutEvaluation.FailureReason = TEXT("primordial law failed: no two interior endpoint cells");
            return false;
        }

        FVoxelStrateMeasureSettings PlayerFitSettings = Settings.PlayerFitMeasureSettings;
        // The season primordial law has always used the (0,0) spine as its arrival and departure
        // line. Keep the fine ROI centred on that same physical route; centring it on the coarse
        // largest-air representative can exclude the law endpoints and turn a valid route into
        // an arbitrary OutOfWindow result. The player-fit pass still has its independent fine
        // resolution and bounded radius.
        PlayerFitSettings.CenterXY = FVector2D::ZeroVector;
        PlayerFitSettings.CoverPointA.Reset();
        PlayerFitSettings.CoverPointB.Reset();
        FVoxelStrateMetrics PlayerFitMetrics;
        const FVoxelConnectivityDiagnostics PlayerFitLaw =
            VF_DiagnosePlayerFitConnectivityWithSampler(
                Sampler, Strate.BottomWorldZ, Strate.TopWorldZ, Seal,
                FVector(0.0f, 0.0f, InteriorBottom + 0.5f),
                FVector(0.0f, 0.0f, InteriorTop - 0.5f),
                PlayerFitSettings, &PlayerFitMetrics);
        // The legacy fields remain the bounded coarse measurement. The player fields come from
        // the separate one-voxel ROI that also backed the restricted route query.
        RawMetrics.bPlayerFitResolved = PlayerFitMetrics.bPlayerFitResolved;
        RawMetrics.PlayerFitRefusalReason = PlayerFitMetrics.PlayerFitRefusalReason;
        RawMetrics.NumPlayerFitCells = PlayerFitMetrics.NumPlayerFitCells;
        RawMetrics.PlayerFitFraction = PlayerFitMetrics.PlayerFitFraction;
        RawMetrics.NumTraversableComponents = PlayerFitMetrics.NumTraversableComponents;
        RawMetrics.LargestTraversableComponentCells =
            PlayerFitMetrics.LargestTraversableComponentCells;
        RawMetrics.TraversableComponentShare = PlayerFitMetrics.TraversableComponentShare;
        RawMetrics.MinimumPlayerClearanceVoxels =
            PlayerFitMetrics.MinimumPlayerClearanceVoxels;
        if (OutRawMetrics != nullptr)
        {
            *OutRawMetrics = RawMetrics;
        }

        OutEvaluation.PrimordialLawResult = PlayerFitLaw.Result;
        const bool bPlayerFitExists = PlayerFitMetrics.bValid
            && PlayerFitMetrics.bPlayerFitResolved
            && PlayerFitMetrics.NumPlayerFitCells > 0
            && PlayerFitMetrics.NumTraversableComponents > 0
            && PlayerFitMetrics.TraversableComponentShare > 0.0f;
        OutEvaluation.bPassedPrimordialLaw = bPlayerFitExists
            && PlayerFitLaw.bValid
            && PlayerFitLaw.Result == EVoxelConnectivityResult::Connected;
        if (!OutEvaluation.bPassedPrimordialLaw)
        {
            OutEvaluation.FailureReason = FString::Printf(
                TEXT("player-fit primordial law failed: %s%s"),
                RejectionReasonForLaw(PlayerFitLaw.Result),
                bPlayerFitExists ? TEXT("") : TEXT(" (no player-fitting floor volume)"));
            return false;
        }

        OutEvaluation.Metrics = VF_SummarizeStrateMetrics(RawMetrics);
        if (!OutEvaluation.Metrics.IsUsable())
        {
            OutEvaluation.FailureReason = TEXT("measurement summary was not usable");
            return false;
        }
        return true;
    }

    FVoxelStratePromotionPolicy MakeMetricDistancePolicy(
        const FVoxelSeasonSelectionPolicy& Policy)
    {
        FVoxelStratePromotionPolicy Result;
        Result.FeatureScaleNormalizationVoxels = Policy.FeatureScaleNormalizationVoxels;
        Result.ClearanceNormalizationVoxels = Policy.ClearanceNormalizationVoxels;
        return Result;
    }

    bool IsBetterScore(double Score, int32 CandidateIndex,
                      double BestScore, int32 BestCandidateIndex)
    {
        constexpr double TieEpsilon = 1.0e-12;
        if (BestCandidateIndex == INDEX_NONE)
        {
            return true;
        }
        if (Score > BestScore + TieEpsilon)
        {
            return true;
        }
        return FMath::Abs(Score - BestScore) <= TieEpsilon
            && CandidateIndex < BestCandidateIndex;
    }
    float ActiveTop(const FVoxelStrateArchetypeParams& Params, ECaveGeneratorType Archetype)
    {
        switch (Archetype)
        {
        case ECaveGeneratorType::TunnelNetwork:
        case ECaveGeneratorType::Underwater:
            return Params.TunnelNetworkParams.StrateTopWorldZ;
        case ECaveGeneratorType::FlatPlain:
        case ECaveGeneratorType::CrystalChamber:
            return Params.SlabParams.StrateTopWorldZ;
        case ECaveGeneratorType::Maze:
            return Params.MazeParams.StrateTopWorldZ;
        case ECaveGeneratorType::SurfaceWorld:
            return Params.SurfaceParams.StrateTopWorldZ;
        case ECaveGeneratorType::VerticalShafts:
            return Params.VerticalShaftParams.StrateTopWorldZ;
        case ECaveGeneratorType::FloatingIslands:
            return Params.FloatingIslandParams.StrateTopWorldZ;
        default:
            return 0.0f;
        }
    }

    float ActiveBottom(const FVoxelStrateArchetypeParams& Params, ECaveGeneratorType Archetype)
    {
        switch (Archetype)
        {
        case ECaveGeneratorType::TunnelNetwork:
        case ECaveGeneratorType::Underwater:
            return Params.TunnelNetworkParams.StrateBottomWorldZ;
        case ECaveGeneratorType::FlatPlain:
        case ECaveGeneratorType::CrystalChamber:
            return Params.SlabParams.StrateBottomWorldZ;
        case ECaveGeneratorType::Maze:
            return Params.MazeParams.StrateBottomWorldZ;
        case ECaveGeneratorType::SurfaceWorld:
            return Params.SurfaceParams.StrateBottomWorldZ;
        case ECaveGeneratorType::VerticalShafts:
            return Params.VerticalShaftParams.StrateBottomWorldZ;
        case ECaveGeneratorType::FloatingIslands:
            return Params.FloatingIslandParams.StrateBottomWorldZ;
        default:
            return 0.0f;
        }
    }

    float ActiveSeal(const FVoxelStrateArchetypeParams& Params, ECaveGeneratorType Archetype)
    {
        switch (Archetype)
        {
        case ECaveGeneratorType::TunnelNetwork:
        case ECaveGeneratorType::Underwater:
            return Params.TunnelNetworkParams.BoundarySealThickness;
        case ECaveGeneratorType::FlatPlain:
        case ECaveGeneratorType::CrystalChamber:
            return Params.SlabParams.BoundarySealThickness;
        case ECaveGeneratorType::Maze:
            return Params.MazeParams.BoundarySealThickness;
        case ECaveGeneratorType::SurfaceWorld:
            return Params.SurfaceParams.BoundarySealThickness;
        case ECaveGeneratorType::VerticalShafts:
            return Params.VerticalShaftParams.BoundarySealThickness;
        case ECaveGeneratorType::FloatingIslands:
            return Params.FloatingIslandParams.BoundarySealThickness;
        default:
            return 0.0f;
        }
    }

    void SetRuntimeBounds(FVoxelStrateArchetypeParams& Params, float TopWorldZ, float BottomWorldZ)
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

    void SetRegionRuntimeBounds(FVoxelStrateRegionManifest& Regions,
                                int32 Seed, int32 StrateIndex,
                                float TopWorldZ, float BottomWorldZ)
    {
        VF_RekeyStrateRegionManifest(Regions, Seed, StrateIndex);
        Regions.StrateTopWorldZ = TopWorldZ;
        Regions.StrateBottomWorldZ = BottomWorldZ;
        Regions.bHasGlobalStructuralParams = true;
        for (FVoxelStrateRegion& Region : Regions.Regions)
        {
            VF_SetStrateArchetypeRuntimeBounds(Region.ArchetypeParams,
                                               TopWorldZ, BottomWorldZ);
        }
    }

    bool SameFloatBits(float A, float B)
    {
        return FMemory::Memcmp(&A, &B, sizeof(float)) == 0;
    }

    const TCHAR* RejectionReasonForLaw(EVoxelConnectivityResult Result)
    {
        return VoxelSeasonManifestPrivate::ConnectivityName(Result);
    }
}
#endif // WITH_EDITOR — composition, measurement and review helpers never ship

namespace VoxelSeasonCompositionPrivate
{
#if WITH_EDITOR
    bool WriteReviewPage(const FVoxelSeasonManifest& Manifest,
                         const FVoxelSeasonCompositionSettings& Settings,
                         const FString& ReviewDirectory,
                         FString& OutReviewPath,
                         FString& OutError);
#endif
}

#if WITH_EDITOR

FVoxelSeasonManifest VF_ComposeSeason(int32 SeasonSeed,
                                      const FVoxelSeasonCompositionSettings& Settings)
{
    using namespace VoxelSeasonCompositionPrivate;
    FVoxelSeasonManifest Manifest;
    Manifest.Season = Settings.SeasonNumber != 0
        ? Settings.SeasonNumber
        : (Settings.WorldSettings != nullptr ? Settings.WorldSettings->CurrentSeason : 0);
    Manifest.Seed = SeasonSeed;
    Manifest.SchemaVersion = FVoxelSeasonManifest::CurrentSchemaVersion;
    Manifest.SelectionPolicy = Settings.SelectionPolicy;
    Manifest.PolicyNote = TEXT(
        "PROVISIONAL POLICY: grounded/outlier ratio, adjacency threshold/weight, variety bonuses, "
        "boss cadence, and metric normalizers are review knobs. This is not a settled quality score. "
        "Jahni reviews the selected descent page and may veto the season before publish.");

    auto Fail = [&](const FString& Reason) -> FVoxelSeasonManifest
    {
        Manifest.bValid = false;
        Manifest.Error = Reason;
        return Manifest;
    };

    if (Settings.WorldSettings != nullptr
        && Settings.WorldSettings->WorldRadiusVoxels != 0.0f)
    {
        return Fail(TEXT("WorldRadiusVoxels must remain exactly 0 for season composition."));
    }
    if (Settings.WorldRadiusVoxels != 0.0f)
    {
        return Fail(TEXT("WorldRadiusVoxels must remain exactly 0 for season composition."));
    }
    if (Settings.CandidateCount <= 0 || Settings.CandidateCount > MaxCandidateCount)
    {
        return Fail(FString::Printf(TEXT("CandidateCount must be in [1,%d]."), MaxCandidateCount));
    }
    if (Settings.SelectedStrateCount <= 0
        || Settings.SelectedStrateCount > MaxSelectedStrateCount)
    {
        return Fail(FString::Printf(TEXT("SelectedStrateCount must be in [1,%d]."),
                                    MaxSelectedStrateCount));
    }
    if (Settings.StrateHeightInChunks <= 0
        || Settings.StrateHeightInChunks > MaxStrateHeightInChunks)
    {
        return Fail(FString::Printf(TEXT("StrateHeightInChunks must be in [1,%d]."),
                                    MaxStrateHeightInChunks));
    }
    if (Settings.InterStrateGapChunks < 0
        || Settings.InterStrateGapChunks > MaxStrateHeightInChunks)
    {
        return Fail(TEXT("InterStrateGapChunks is outside the bounded composition range."));
    }
    if (!FMath::IsFinite(Settings.OriginSpineRadius) || Settings.OriginSpineRadius < 0.0f)
    {
        return Fail(TEXT("OriginSpineRadius must be finite and non-negative."));
    }
    if (!IsFinitePolicy(Settings.SelectionPolicy))
    {
        return Fail(TEXT("SelectionPolicy contains a non-finite or out-of-range knob."));
    }
    if (!IsValidMeasureSettings(Settings.MeasureSettings))
    {
        return Fail(TEXT("MeasureSettings are invalid for a bounded offline pass."));
    }
    if (!IsValidMeasureSettings(Settings.PlayerFitMeasureSettings))
    {
        return Fail(TEXT("PlayerFitMeasureSettings are invalid for a bounded offline pass."));
    }

    TMap<int32, FVoxelSeasonFixedStrate> FixedStrates;
    FString FixedError;
    if (!ResolveFixedStrates(Settings, SeasonSeed, FixedStrates, FixedError))
    {
        return Fail(FixedError);
    }
    for (const TPair<int32, FVoxelSeasonFixedStrate>& Pair : FixedStrates)
    {
        if (Pair.Key < 0 || Pair.Key >= Settings.SelectedStrateCount)
        {
            return Fail(FString::Printf(TEXT("FixedStrates index %d is outside [0,%d)."),
                                        Pair.Key, Settings.SelectedStrateCount));
        }
        if (!VoxelSeasonManifestPrivate::IsSupportedArchetype(Pair.Value.Archetype))
        {
            return Fail(FString::Printf(TEXT("FixedStrates index %d has an unsupported archetype."),
                                        Pair.Key));
        }
        if (Pair.Value.HeightInChunks <= 0
            || Pair.Value.HeightInChunks > MaxStrateHeightInChunks)
        {
            return Fail(FString::Printf(TEXT("FixedStrates index %d has an invalid height."),
                                        Pair.Key));
        }
    }

    FVoxelStrateCorpus OwnedCorpus;
    const FVoxelStrateCorpus* Corpus = Settings.Corpus;
    if (Corpus == nullptr)
    {
        FString CorpusReport;
        if (!OwnedCorpus.LoadFromAssetRegistry(CorpusReport) || !OwnedCorpus.IsValid())
        {
            return Fail(FString::Printf(TEXT("could not load the strate corpus: %s"),
                                        *CorpusReport));
        }
        Corpus = &OwnedCorpus;
    }
    if (Corpus == nullptr || !Corpus->IsValid())
    {
        return Fail(TEXT("season composition requires a valid strate corpus."));
    }

    Manifest.InputCorpusHash = Corpus->GetContentsHash();
    Manifest.CandidateCount = Settings.CandidateCount;
    Manifest.TotalStrates = Settings.SelectedStrateCount;
    Manifest.InterStrateGapChunks = Settings.InterStrateGapChunks;
    Manifest.OriginSpineRadius = Settings.OriginSpineRadius;
    Manifest.WorldRadiusVoxels = 0.0f;

    TArray<FSeasonCandidateState> Candidates;
    Candidates.Reserve(Settings.CandidateCount);
    for (int32 CandidateIndex = 0; CandidateIndex < Settings.CandidateCount; ++CandidateIndex)
    {
        FSeasonCandidateState& State = Candidates.AddDefaulted_GetRef();
        const int32 CandidateSeed = static_cast<int32>(
            SeasonAttemptHash(SeasonSeed, CandidateIndex));
        State.Candidate = VF_RollStrateCandidate(
            *Corpus, CandidateSeed, CandidateIndex, true);
        if (!State.Candidate.bValid)
        {
            State.RejectionReason = State.Candidate.FailureReason.IsEmpty()
                ? TEXT("candidate roll failed") : State.Candidate.FailureReason;
            continue;
        }

        State.DistanceFromCorpusCentroid = VF_DistanceFromStrateCorpusCentroid(
            *Corpus, State.Candidate.ArchetypeParams, State.Candidate.Archetype);
        if (!FMath::IsFinite(State.DistanceFromCorpusCentroid))
        {
            State.DistanceFromCorpusCentroid = 0.0;
        }

        FVoxelSeasonStrate Probe;
        const int32 ProbeTopWorldZ = 32;
        const int32 ProbeBottomWorldZ = ProbeTopWorldZ
            - Settings.StrateHeightInChunks * 32;
        MakeCandidateStrate(State.Candidate, CandidateIndex,
                            Settings.StrateHeightInChunks, ProbeTopWorldZ,
                            ProbeBottomWorldZ, Probe);
        FSeasonEvaluation Evaluation;
        if (EvaluateStrate(Probe, Settings, Evaluation))
        {
            State.Metrics = Evaluation.Metrics;
            State.bPassedHardGates = true;
        }
        else
        {
            State.RejectionReason = Evaluation.FailureReason;
        }
    }

    TArray<int32> SurvivorIndices;
    SurvivorIndices.Reserve(Candidates.Num());
    for (int32 Index = 0; Index < Candidates.Num(); ++Index)
    {
        if (Candidates[Index].bPassedHardGates)
        {
            SurvivorIndices.Add(Index);
        }
    }
    Manifest.SurvivorCount = SurvivorIndices.Num();

    const int32 GeneratedSlotCount = Settings.SelectedStrateCount - FixedStrates.Num();
    if (GeneratedSlotCount < 0)
    {
        return Fail(TEXT("FixedStrates contains more slots than the requested season."));
    }
    if (SurvivorIndices.Num() < GeneratedSlotCount)
    {
        return Fail(FString::Printf(
            TEXT("only %d candidates survived the hard gates, but %d generated slots are required"),
            SurvivorIndices.Num(), GeneratedSlotCount));
    }

    const float FractionSum = Settings.SelectionPolicy.GroundedFraction
        + Settings.SelectionPolicy.OutlierFraction;
    const int32 TargetOutlierCount = FMath::Clamp(
        FMath::RoundToInt(static_cast<float>(GeneratedSlotCount)
            * Settings.SelectionPolicy.OutlierFraction / FractionSum),
        0, GeneratedSlotCount);
    SurvivorIndices.Sort([&Candidates](int32 A, int32 B)
    {
        const double ADistance = Candidates[A].DistanceFromCorpusCentroid;
        const double BDistance = Candidates[B].DistanceFromCorpusCentroid;
        if (ADistance != BDistance)
        {
            return ADistance > BDistance;
        }
        return Candidates[A].Candidate.Index < Candidates[B].Candidate.Index;
    });
    for (int32 Index = 0; Index < FMath::Min(TargetOutlierCount, SurvivorIndices.Num()); ++Index)
    {
        Candidates[SurvivorIndices[Index]].bOutlierPool = true;
    }

    const FVoxelStratePromotionPolicy MetricDistancePolicy =
        MakeMetricDistancePolicy(Settings.SelectionPolicy);
    TSet<int32> UsedArchetypes;
    TSet<uint32> UsedRecipes;
    FVoxelStrateMeasuredMetrics PreviousMetrics;
    bool bHavePreviousMetrics = false;
    int64 CurrentTopChunk = 0;
    int32 GeneratedOrdinal = 0;
    int32 SelectedGeneratedCount = 0;
    int32 AssignedOutlierCount = 0;
    int32 FixedSelectedCount = 0;

    auto GetLayoutBounds = [&](int32 HeightInChunks,
                               int32& OutTopWorldZ, int32& OutBottomWorldZ,
                               int64& OutBottomChunk) -> bool
    {
        if (HeightInChunks <= 0) return false;
        const int64 TopChunk = CurrentTopChunk;
        OutBottomChunk = TopChunk - static_cast<int64>(HeightInChunks) + 1;
        const int64 TopWorld = (TopChunk + 1) * 32;
        const int64 BottomWorld = OutBottomChunk * 32;
        if (TopWorld < static_cast<int64>(TNumericLimits<int32>::Lowest())
            || TopWorld > static_cast<int64>(TNumericLimits<int32>::Max())
            || BottomWorld < static_cast<int64>(TNumericLimits<int32>::Lowest())
            || BottomWorld > static_cast<int64>(TNumericLimits<int32>::Max()))
        {
            return false;
        }
        OutTopWorldZ = static_cast<int32>(TopWorld);
        OutBottomWorldZ = static_cast<int32>(BottomWorld);
        return OutTopWorldZ > OutBottomWorldZ;
    };

    for (int32 DepthIndex = 0; DepthIndex < Settings.SelectedStrateCount; ++DepthIndex)
    {
        const FVoxelSeasonFixedStrate* Fixed = FixedStrates.Find(DepthIndex);
        const int32 HeightInChunks = Fixed != nullptr
            ? Fixed->HeightInChunks : Settings.StrateHeightInChunks;
        int32 TopWorldZ = 0;
        int32 BottomWorldZ = 0;
        int64 BottomChunk = 0;
        if (!GetLayoutBounds(HeightInChunks, TopWorldZ, BottomWorldZ, BottomChunk))
        {
            return Fail(FString::Printf(TEXT("season Z layout overflowed at slot %d"), DepthIndex));
        }

        if (Fixed != nullptr)
        {
            FVoxelSeasonStrate Strate;
            MakeFixedStrate(*Fixed, DepthIndex, TopWorldZ, BottomWorldZ, Strate);
            FSeasonEvaluation Evaluation;
            if (!EvaluateStrate(Strate, Settings, Evaluation))
            {
                return Fail(FString::Printf(TEXT("fixed slot %d failed validation: %s"),
                                            DepthIndex, *Evaluation.FailureReason));
            }
            Strate.Metrics = Evaluation.Metrics;
            Strate.bPassedNonVacuous = Evaluation.bPassedNonVacuous;
            Strate.bPassedLargestComponent = Evaluation.bPassedLargestComponent;
            Strate.bPassedPrimordialLaw = Evaluation.bPassedPrimordialLaw;
            Strate.PrimordialLawResult = Evaluation.PrimordialLawResult;
            Strate.DistanceFromCorpusCentroid = VF_DistanceFromStrateCorpusCentroid(
                *Corpus, Strate.Params, Strate.Archetype);
            Strate.bBossSlot = IsBossSlot(DepthIndex, Settings.SelectedStrateCount,
                                          Settings.SelectionPolicy.BossSlotInterval);
            Strate.SelectionReason = TEXT("fixed: preserved from the absolute FixedStrates slot");
            if (Strate.bBossSlot)
            {
                Strate.SelectionReason += TEXT("; boss slot at the provisional five-slot cadence");
            }
            Manifest.Strates.Add(MoveTemp(Strate));
            ++FixedSelectedCount;
            UsedArchetypes.Add(static_cast<int32>(Fixed->Archetype));
            if (Fixed->bUsesRecipe)
            {
                UsedRecipes.Add(VF_HashStrateStructureRecipe(Fixed->Recipe));
            }
            PreviousMetrics = Manifest.Strates.Last().Metrics;
            bHavePreviousMetrics = true;
        }
        else
        {
            const int32 RemainingGeneratedSlots = GeneratedSlotCount - GeneratedOrdinal;
            const int32 DesiredOutliersSoFar = FMath::RoundToInt(
                static_cast<float>(GeneratedOrdinal + 1) * TargetOutlierCount
                    / FMath::Max(1, GeneratedSlotCount));
            const bool bWantOutlier = DesiredOutliersSoFar >
                AssignedOutlierCount;

            int32 BestStateIndex = INDEX_NONE;
            double BestScore = -TNumericLimits<double>::Max();
            bool bUsedClassFallback = false;
            auto FindBest = [&](bool bRequireOutlierClass) -> bool
            {
                bool bFound = false;
                for (int32 StateIndex = 0; StateIndex < Candidates.Num(); ++StateIndex)
                {
                    FSeasonCandidateState& State = Candidates[StateIndex];
                    if (!State.bPassedHardGates || State.bSelected || State.bUnavailable
                        || (bRequireOutlierClass && State.bOutlierPool != bWantOutlier))
                    {
                        continue;
                    }
                    const bool bNewArchetype = !UsedArchetypes.Contains(
                        static_cast<int32>(State.Candidate.Archetype));
                    const uint32 RecipeHash = VF_HashStrateStructureRecipe(State.Candidate.Recipe);
                    const bool bNewRecipe = !UsedRecipes.Contains(RecipeHash);
                    const bool bActualOutlier = State.bOutlierPool;
                    double Score = bActualOutlier
                        ? State.DistanceFromCorpusCentroid
                        : -State.DistanceFromCorpusCentroid;
                    if (bHavePreviousMetrics && State.Metrics.IsUsable())
                    {
                        const double AdjacentDistance = VF_NormalizedMeasuredMetricDistance(
                            PreviousMetrics, State.Metrics, MetricDistancePolicy);
                        if (FMath::IsFinite(AdjacentDistance)
                            && AdjacentDistance < Settings.SelectionPolicy.AdjacentSimilarityDistanceThreshold)
                        {
                            Score -= (Settings.SelectionPolicy.AdjacentSimilarityDistanceThreshold
                                      - AdjacentDistance)
                                * Settings.SelectionPolicy.AdjacentSimilarityPenaltyWeight;
                        }
                    }
                    if (bNewArchetype)
                    {
                        Score += Settings.SelectionPolicy.NewArchetypeBonus;
                    }
                    if (bNewRecipe)
                    {
                        Score += Settings.SelectionPolicy.NewRecipeBonus;
                    }
                    if (!FMath::IsFinite(Score))
                    {
                        continue;
                    }
                    if (!bFound || IsBetterScore(Score, State.Candidate.Index,
                                                 BestScore,
                                                 Candidates[BestStateIndex].Candidate.Index))
                    {
                        BestStateIndex = StateIndex;
                        BestScore = Score;
                        bFound = true;
                    }
                }
                return bFound;
            };

            if (!FindBest(true))
            {
                bUsedClassFallback = FindBest(false);
            }
            if (BestStateIndex == INDEX_NONE)
            {
                return Fail(FString::Printf(
                    TEXT("no validated candidate remains for season slot %d (%d generated slots remain)"),
                    DepthIndex, RemainingGeneratedSlots));
            }

            bool bPlaced = false;
            while (BestStateIndex != INDEX_NONE)
            {
                FSeasonCandidateState& State = Candidates[BestStateIndex];
                FVoxelSeasonStrate Strate;
                MakeCandidateStrate(State.Candidate, State.Candidate.Index,
                                    Settings.StrateHeightInChunks, TopWorldZ,
                                    BottomWorldZ, Strate);
                Strate.DepthIndex = DepthIndex;
                if (Strate.bUsesRegions)
                {
                    // Candidate probes use the attempt index only to make their provisional
                    // measurements independent.  The persisted world is keyed by its absolute
                    // strate slot, so rekey before the final placement gate and measurement.
                    SetRegionRuntimeBounds(Strate.Regions, Strate.Seed, DepthIndex,
                                           static_cast<float>(TopWorldZ),
                                           static_cast<float>(BottomWorldZ));
                }
                FSeasonEvaluation Evaluation;
                if (!EvaluateStrate(Strate, Settings, Evaluation))
                {
                    State.bUnavailable = true;
                    State.RejectionReason = FString::Printf(
                        TEXT("final placement at depth %d failed: %s"),
                        DepthIndex, *Evaluation.FailureReason);
                    BestStateIndex = INDEX_NONE;
                    if (!FindBest(true))
                    {
                        bUsedClassFallback = FindBest(false);
                    }
                    continue;
                }

                const bool bActualOutlier = State.bOutlierPool;
                const bool bNewArchetype = !UsedArchetypes.Contains(
                    static_cast<int32>(State.Candidate.Archetype));
                const uint32 RecipeHash = VF_HashStrateStructureRecipe(State.Candidate.Recipe);
                const bool bNewRecipe = !UsedRecipes.Contains(RecipeHash);
                Strate.Metrics = Evaluation.Metrics;
                Strate.DistanceFromCorpusCentroid = State.DistanceFromCorpusCentroid;
                Strate.bGrounded = !bActualOutlier;
                Strate.bOutlier = bActualOutlier;
                Strate.bBossSlot = IsBossSlot(DepthIndex, Settings.SelectedStrateCount,
                                              Settings.SelectionPolicy.BossSlotInterval);
                Strate.bPassedNonVacuous = Evaluation.bPassedNonVacuous;
                Strate.bPassedLargestComponent = Evaluation.bPassedLargestComponent;
                Strate.bPassedPrimordialLaw = Evaluation.bPassedPrimordialLaw;
                Strate.PrimordialLawResult = Evaluation.PrimordialLawResult;
                const FString ClassName = bActualOutlier ? TEXT("outlier") : TEXT("grounded");
                const FString DistanceDirection = bActualOutlier
                    ? TEXT("farthest surviving corpus-centroid distance")
                    : TEXT("nearest surviving corpus-centroid distance");
                Strate.SelectionReason = FString::Printf(
                    TEXT("%s (%s grounded / %s outlier placeholder): %s; "
                         "adjacency penalty and variety tie-breakers"),
                    *ClassName,
                    *PercentageText(Settings.SelectionPolicy.GroundedFraction / FractionSum),
                    *PercentageText(Settings.SelectionPolicy.OutlierFraction / FractionSum),
                    *DistanceDirection);
                if (bUsedClassFallback)
                {
                    Strate.SelectionReason += TEXT("; requested ratio class was exhausted");
                }
                if (bNewArchetype || bNewRecipe)
                {
                    Strate.SelectionReason += TEXT("; unseen ");
                    if (bNewArchetype) Strate.SelectionReason += TEXT("archetype");
                    if (bNewArchetype && bNewRecipe) Strate.SelectionReason += TEXT("/");
                    if (bNewRecipe) Strate.SelectionReason += TEXT("recipe");
                    Strate.SelectionReason += TEXT(" bonus applied");
                }
                if (Strate.bBossSlot)
                {
                    Strate.SelectionReason += TEXT("; boss slot at the provisional five-slot cadence");
                }

                State.Metrics = Evaluation.Metrics;
                State.bSelected = true;
                State.RejectionReason.Reset();
                Manifest.Strates.Add(MoveTemp(Strate));
                UsedArchetypes.Add(static_cast<int32>(State.Candidate.Archetype));
                UsedRecipes.Add(RecipeHash);
                PreviousMetrics = Manifest.Strates.Last().Metrics;
                bHavePreviousMetrics = true;
                ++GeneratedOrdinal;
                ++SelectedGeneratedCount;
                if (bActualOutlier)
                {
                    ++AssignedOutlierCount;
                }
                bPlaced = true;
                break;
            }
            if (!bPlaced)
            {
                return Fail(FString::Printf(TEXT("all remaining candidates failed final placement at slot %d"),
                                            DepthIndex));
            }
        }

        CurrentTopChunk = BottomChunk - 1 - static_cast<int64>(Settings.InterStrateGapChunks);
    }

    if (Manifest.Strates.Num() != Settings.SelectedStrateCount
        || SelectedGeneratedCount != GeneratedSlotCount
        || FixedSelectedCount != FixedStrates.Num())
    {
        return Fail(TEXT("season selector did not fill every absolute slot exactly once"));
    }

    TMap<FString, int32> RejectionCounts;
    if (Settings.bIncludeCandidateAudit)
    {
        Manifest.CandidateAudit.Reserve(Candidates.Num());
    }
    for (const FSeasonCandidateState& State : Candidates)
    {
        const FString Reason = State.bSelected
            ? FString()
            : (State.RejectionReason.IsEmpty()
                ? TEXT("not selected by provisional policy") : State.RejectionReason);
        if (!State.bSelected)
        {
            AddRejection(RejectionCounts, Reason);
        }
        if (Settings.bIncludeCandidateAudit)
        {
            FVoxelSeasonCandidateAudit& Row = Manifest.CandidateAudit.AddDefaulted_GetRef();
            Row.CandidateIndex = State.Candidate.Index;
            Row.Seed = State.Candidate.Seed;
            Row.Archetype = State.Candidate.Archetype;
            Row.RecipeHash = State.Candidate.bValid
                ? VF_HashStrateStructureRecipe(State.Candidate.Recipe) : 0;
            Row.DistanceFromCorpusCentroid = State.DistanceFromCorpusCentroid;
            Row.bPassedHardGates = State.bPassedHardGates;
            Row.bSelected = State.bSelected;
            Row.RejectionReason = Reason;
        }
    }
    ExportRejectionCounts(RejectionCounts, Manifest.RejectionCounts);
    Manifest.SelectedCount = Manifest.Strates.Num();
    Manifest.GroundedSelectedCount = 0;
    Manifest.OutlierSelectedCount = 0;
    Manifest.FixedSelectedCount = 0;
    for (const FVoxelSeasonStrate& Strate : Manifest.Strates)
    {
        if (Strate.bGrounded) ++Manifest.GroundedSelectedCount;
        if (Strate.bOutlier) ++Manifest.OutlierSelectedCount;
        if (Strate.bFixed) ++Manifest.FixedSelectedCount;
        if (!Strate.bPassedNonVacuous || !Strate.bPassedLargestComponent
            || !Strate.bPassedPrimordialLaw
            || Strate.PrimordialLawResult != EVoxelConnectivityResult::Connected)
        {
            return Fail(FString::Printf(TEXT("selected slot %d failed the final primordial-law assertion"),
                                        Strate.DepthIndex));
        }
    }
    Manifest.RejectedCount = FMath::Max(0, Manifest.CandidateCount - SelectedGeneratedCount);

    if (Settings.bWriteArtifacts)
    {
        const FString BaseDirectory = Settings.OutputDirectory.IsEmpty()
            ? FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("VoxelForge"), TEXT("Seasons"))
            : Settings.OutputDirectory;
        const FString SeasonDirectory = FPaths::Combine(
            BaseDirectory,
            FString::Printf(TEXT("season_%04d_seed_%d"), Manifest.Season, Manifest.Seed));
        Manifest.ManifestPath = FPaths::Combine(SeasonDirectory, TEXT("season_manifest.json"));
        if (Settings.bWriteReviewPage)
        {
            const FString ReviewDirectory = FPaths::Combine(SeasonDirectory, TEXT("review"));
            if (!WriteReviewPage(Manifest, Settings, ReviewDirectory,
                                 Manifest.ReviewPagePath, Manifest.Error))
            {
                return Fail(Manifest.Error.IsEmpty()
                    ? TEXT("could not write selected-season review page") : Manifest.Error);
            }
        }
    }

    Manifest.bValid = true;
    Manifest.Error.Reset();
    Manifest.ContentHash = VF_ComputeVoxelSeasonManifestContentHash(Manifest);
    if (Manifest.ContentHash.IsEmpty())
    {
        return Fail(TEXT("could not hash composed season manifest"));
    }
    Manifest.SerializedJson = VF_SerializeVoxelSeasonManifest(Manifest);
    if (Manifest.SerializedJson.IsEmpty())
    {
        return Fail(TEXT("could not serialize composed season manifest"));
    }
    if (Settings.bWriteArtifacts)
    {
        FString SaveReport;
        if (!VF_SaveVoxelSeasonManifest(Manifest.ManifestPath, Manifest, SaveReport))
        {
            return Fail(SaveReport);
        }
    }
    return Manifest;
}

#endif

#if WITH_EDITOR
FVoxelSeasonManifest VF_ComposeSeason(int32 SeasonSeed, const UVoxelSettings* WorldSettings)
{
    FVoxelSeasonCompositionSettings Settings;
    Settings.WorldSettings = WorldSettings;
    if (WorldSettings != nullptr)
    {
        Settings.SelectedStrateCount = WorldSettings->TotalStrates;
        Settings.InterStrateGapChunks = WorldSettings->InterStrateGapChunks;
        Settings.OriginSpineRadius = WorldSettings->OriginSpineRadius;
        Settings.WorldRadiusVoxels = WorldSettings->WorldRadiusVoxels;
        Settings.SeasonNumber = WorldSettings->CurrentSeason;
    }
    return VF_ComposeSeason(SeasonSeed, Settings);
}
#endif

bool VF_BuildSeasonStrateStack(const FVoxelSeasonStrate& Strate,
                               float SpineRadius,
                               const UVoxelStrateManager* StrateManager,
                               FVoxelOpStack& OutStack,
                               FVoxelOpContext& OutContext,
                               FString* OutError)
{
    using namespace VoxelSeasonRuntimePrivate;
    auto Fail = [&](const FString& Reason) -> bool
    {
        if (OutError != nullptr)
        {
            *OutError = Reason;
        }
        return false;
    };
    if (OutError != nullptr)
    {
        OutError->Reset();
    }
    if (!VoxelSeasonManifestPrivate::IsSupportedArchetype(Strate.Archetype)
        || Strate.HeightInChunks <= 0
        || !FMath::IsFinite(SpineRadius) || SpineRadius < 0.0f
        || Strate.TopWorldZ <= Strate.BottomWorldZ)
    {
        return Fail(TEXT("Manifest strate has invalid identity, height, bounds, or spine radius."));
    }

    const float StoredTop = static_cast<float>(Strate.TopWorldZ);
    const float StoredBottom = static_cast<float>(Strate.BottomWorldZ);
    if (Strate.bUsesRegions)
    {
        if (!Strate.Regions.IsValid()
            || Strate.Regions.RegionCount <= 1
            || Strate.Regions.Seed != Strate.Seed
            || Strate.Regions.StrateIndex != Strate.DepthIndex
            || Strate.Regions.PartitionSeed != VF_GetStrateRegionPartitionSeed(
                Strate.Seed, Strate.DepthIndex)
            || !Strate.Regions.bHasGlobalStructuralParams
            || !SameFloatBits(Strate.Regions.StrateTopWorldZ, StoredTop)
            || !SameFloatBits(Strate.Regions.StrateBottomWorldZ, StoredBottom))
        {
            return Fail(TEXT("Manifest lateral region description is invalid or not keyed to its slot."));
        }
    }
    if (!SameFloatBits(ActiveTop(Strate.Params, Strate.Archetype), StoredTop)
        || !SameFloatBits(ActiveBottom(Strate.Params, Strate.Archetype), StoredBottom))
    {
        return Fail(TEXT("Manifest bounds do not match the active parameter vector."));
    }

    bool bBuilt = false;
    if (Strate.bUsesRegions)
    {
#if WITH_EDITOR
        bBuilt = VF_BuildStrateRegionStack(Strate.Regions, SpineRadius, StrateManager,
                                            OutStack, OutContext, OutError);
#else
        return Fail(TEXT("Lateral region manifests are not shippable."));
#endif
    }
    else if (Strate.bUsesRecipe)
    {
        bBuilt = VF_BuildStackFromRecipe(Strate.Recipe, Strate.Params, Strate.Seed,
                                          SpineRadius, StrateManager, OutStack, OutContext,
                                          OutError);
    }
    else
    {
#if WITH_EDITOR
        const float Seal = VoxelSeasonCompositionPrivate::ActiveSeal(
            Strate.Params, Strate.Archetype);
        bBuilt = VF_BuildNativeStrateStackForCandidate(
            Strate.Archetype, Strate.Params, Strate.Seed, SpineRadius,
            0.0f, Seal, StrateManager, OutStack, OutContext);
        if (!bBuilt && OutError != nullptr)
        {
            *OutError = TEXT("Native manifest strate could not be materialised in the editor build.");
        }
#else
        bBuilt = false;
        if (OutError != nullptr)
        {
            *OutError = TEXT("Native fixed-strate rebuild is editor/build-box only.");
        }
#endif
    }
    if (!bBuilt)
    {
        return false;
    }
    if (OutContext.WorldRadiusVoxels != 0.0f)
    {
        return Fail(TEXT("Manifest rebuild changed the required WorldRadiusVoxels=0 invariant."));
    }
    return true;
}

bool VF_RebuildVoxelSeasonStrate(const FVoxelSeasonStrate& Strate,
                                 float SpineRadius,
                                 const UVoxelStrateManager* StrateManager,
                                 FVoxelOpStack& OutStack,
                                 FVoxelOpContext& OutContext,
                                 FString* OutError)
{
    return VF_BuildSeasonStrateStack(Strate, SpineRadius, StrateManager,
                                     OutStack, OutContext, OutError);
}

#if WITH_EDITOR
namespace VoxelSeasonCompositionPrivate
{
    bool WriteReviewPage(const FVoxelSeasonManifest& Manifest,
                         const FVoxelSeasonCompositionSettings& Settings,
                         const FString& ReviewDirectory,
                         FString& OutReviewPath,
                         FString& OutError)
    {
        OutReviewPath.Reset();
        OutError.Reset();
        if (!IFileManager::Get().MakeDirectory(*ReviewDirectory, true))
        {
            OutError = FString::Printf(TEXT("could not create season review directory %s"),
                                        *ReviewDirectory);
            return false;
        }

        TArray<FVoxelStratePreviewCandidate> Cards;
        Cards.Reserve(Manifest.Strates.Num());
        FVoxelStratePreviewWindow CommonWindow;
        bool bHaveWindow = false;
        for (const FVoxelSeasonStrate& Strate : Manifest.Strates)
        {
            FVoxelStrateSampleGrid Grid;
            FVoxelStrateMetrics RawMetrics;
            FSeasonEvaluation Evaluation;
            if (!EvaluateStrate(Strate, Settings, Evaluation, &Grid, &RawMetrics))
            {
                OutError = FString::Printf(TEXT("selected slot %d failed its review re-measure: %s"),
                                            Strate.DepthIndex, *Evaluation.FailureReason);
                return false;
            }
            if (!Grid.IsValid())
            {
                OutError = FString::Printf(TEXT("selected slot %d produced no review grid"),
                                            Strate.DepthIndex);
                return false;
            }

            const FString RecipeString = Strate.bUsesRecipe
                ? VF_FormatStrateStructureRecipe(Strate.Recipe)
                : FString::Printf(TEXT("native/%s"), VF_GetStrateArchetypeName(Strate.Archetype));
            const FString ArrivalVerdict = FString::Printf(
                TEXT("%s%s"),
                VoxelSeasonManifestPrivate::ConnectivityName(Evaluation.PrimordialLawResult),
                (Evaluation.bPassedPrimordialLaw ? TEXT("") : TEXT(" (failed)")));
            FVoxelStratePreviewCandidate& Card = Cards.AddDefaulted_GetRef();
            if (!VF_WriteStratePreviewCandidate(
                    ReviewDirectory, Strate.DepthIndex, RecipeString, Grid, RawMetrics,
                    Settings.MeasureSettings.HeadroomCells,
                    Strate.DistanceFromCorpusCentroid, ArrivalVerdict, false, TEXT(""),
                    Card, OutError))
            {
                return false;
            }
            Card.bSeasonOrder = true;
            Card.DepthIndex = Strate.DepthIndex;
            Card.SelectionReason = Strate.SelectionReason;
            Card.bBossSlot = Strate.bBossSlot;
            if (!bHaveWindow)
            {
                CommonWindow = Card.Window;
                bHaveWindow = true;
            }
        }

        if (!bHaveWindow || !CommonWindow.IsValid())
        {
            OutError = TEXT("selected season has no valid preview measurement window");
            return false;
        }
        const FString RunTitle = FString::Printf(
            TEXT("VoxelForge Season %d — seed %d — descent review"), Manifest.Season, Manifest.Seed);
        if (!VF_WriteStratePreviewIndex(ReviewDirectory, RunTitle, CommonWindow,
                                        Cards, OutReviewPath, OutError))
        {
            return false;
        }
        return true;
    }
}
#endif // WITH_EDITOR — review rendering never ships
