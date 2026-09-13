// Bit-exact equivalence coverage for the generation-path finite predicate.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "VoxelTypes.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FVoxelForgeIsFiniteTest,
    "VoxelForge.Math.IsFiniteExact",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

namespace
{
    template <typename TValue, typename TBits>
    TValue ValueFromBits(TBits Bits)
    {
        TValue Value;
        FMemory::Memcpy(&Value, &Bits, sizeof(Value));
        return Value;
    }

    FORCEINLINE uint64 NextHashedBits(uint64& State)
    {
        State += 0x9E3779B97F4A7C15ull;
        uint64 Z = State;
        Z = (Z ^ (Z >> 30)) * 0xBF58476D1CE4E5B9ull;
        Z = (Z ^ (Z >> 27)) * 0x94D049BB133111EBull;
        return Z ^ (Z >> 31);
    }

    struct FFloatPattern
    {
        const TCHAR* Name;
        uint32 Bits;
    };

    struct FDoublePattern
    {
        const TCHAR* Name;
        uint64 Bits;
    };
}

bool FVoxelForgeIsFiniteTest::RunTest(const FString& Parameters)
{
    int32 FloatMismatches = 0;
    int32 DoubleMismatches = 0;

    auto CheckFloat = [&](const TCHAR* Name, uint32 Bits)
    {
        const float Value = ValueFromBits<float>(Bits);
        const bool Fast = VoxelMath::IsFiniteFast(Value);
        const bool Reference = FMath::IsFinite(Value);
        if (Fast != Reference)
        {
            ++FloatMismatches;
            if (FloatMismatches <= 8)
            {
                AddError(FString::Printf(
                    TEXT("%s: float bits 0x%08X produced fast=%s, FMath=%s"),
                    Name, Bits,
                    Fast ? TEXT("true") : TEXT("false"),
                    Reference ? TEXT("true") : TEXT("false")));
            }
        }
    };

    auto CheckDouble = [&](const TCHAR* Name, uint64 Bits)
    {
        const double Value = ValueFromBits<double>(Bits);
        const bool Fast = VoxelMath::IsFiniteFast(Value);
        const bool Reference = FMath::IsFinite(Value);
        if (Fast != Reference)
        {
            ++DoubleMismatches;
            if (DoubleMismatches <= 8)
            {
                AddError(FString::Printf(
                    TEXT("%s: double bits 0x%08X%08X produced fast=%s, FMath=%s"),
                    Name,
                    static_cast<uint32>(Bits >> 32), static_cast<uint32>(Bits),
                    Fast ? TEXT("true") : TEXT("false"),
                    Reference ? TEXT("true") : TEXT("false")));
            }
        }
    };

    // Explicit classes make a NaN regression fail even if the random stream happens not to hit a
    // NaN. In particular, a helper that only checks the sign/exponent incorrectly must fail here.
    const FFloatPattern FloatPatterns[] =
    {
        { TEXT("+0"),                 0x00000000u },
        { TEXT("-0"),                 0x80000000u },
        { TEXT("+min subnormal"),     0x00000001u },
        { TEXT("-min subnormal"),     0x80000001u },
        { TEXT("+max subnormal"),     0x007FFFFFu },
        { TEXT("-max subnormal"),     0x807FFFFFu },
        { TEXT("+min normal"),        0x00800000u },
        { TEXT("-min normal"),        0x80800000u },
        { TEXT("+max normal"),        0x7F7FFFFFu },
        { TEXT("-max normal"),        0xFF7FFFFFu },
        { TEXT("+infinity"),          0x7F800000u },
        { TEXT("-infinity"),          0xFF800000u },
        { TEXT("+quiet NaN payload 1"), 0x7FC00001u },
        { TEXT("-quiet NaN payload 1"), 0xFFC00001u },
        { TEXT("+quiet NaN payload mixed"), 0x7FC12345u },
        { TEXT("-quiet NaN payload mixed"), 0xFFC12345u },
        { TEXT("+signalling NaN payload 1"), 0x7F800001u },
        { TEXT("-signalling NaN payload 1"), 0xFF800001u },
        { TEXT("+signalling NaN payload mixed"), 0x7FA00001u },
        { TEXT("-signalling NaN payload mixed"), 0xFFA00001u },
        { TEXT("+signalling NaN max payload"), 0x7FBFFFFFu },
        { TEXT("-signalling NaN max payload"), 0xFFBFFFFFu },
    };
    for (const FFloatPattern& Pattern : FloatPatterns)
    {
        CheckFloat(Pattern.Name, Pattern.Bits);
    }

    const FDoublePattern DoublePatterns[] =
    {
        { TEXT("+0"),                 0x0000000000000000ull },
        { TEXT("-0"),                 0x8000000000000000ull },
        { TEXT("+min subnormal"),     0x0000000000000001ull },
        { TEXT("-min subnormal"),     0x8000000000000001ull },
        { TEXT("+max subnormal"),     0x000FFFFFFFFFFFFFull },
        { TEXT("-max subnormal"),     0x800FFFFFFFFFFFFFull },
        { TEXT("+min normal"),        0x0010000000000000ull },
        { TEXT("-min normal"),        0x8010000000000000ull },
        { TEXT("+max normal"),        0x7FEFFFFFFFFFFFFFull },
        { TEXT("-max normal"),        0xFFEFFFFFFFFFFFFFull },
        { TEXT("+infinity"),          0x7FF0000000000000ull },
        { TEXT("-infinity"),          0xFFF0000000000000ull },
        { TEXT("+quiet NaN payload 1"), 0x7FF8000000000001ull },
        { TEXT("-quiet NaN payload 1"), 0xFFF8000000000001ull },
        { TEXT("+quiet NaN payload mixed"), 0x7FF8123456789ABCull },
        { TEXT("-quiet NaN payload mixed"), 0xFFF8123456789ABCull },
        { TEXT("+signalling NaN payload 1"), 0x7FF0000000000001ull },
        { TEXT("-signalling NaN payload 1"), 0xFFF0000000000001ull },
        { TEXT("+signalling NaN payload mixed"), 0x7FF4000000000001ull },
        { TEXT("-signalling NaN payload mixed"), 0xFFF4000000000001ull },
        { TEXT("+signalling NaN max payload"), 0x7FF7FFFFFFFFFFFFull },
        { TEXT("-signalling NaN max payload"), 0xFFF7FFFFFFFFFFFFull },
    };
    for (const FDoublePattern& Pattern : DoublePatterns)
    {
        CheckDouble(Pattern.Name, Pattern.Bits);
    }

    // Deterministic hash-generated raw bit patterns exercise ordinary values and special values
    // without converting through floating point.  The loop counts are intentionally large enough
    // to make this a broad bit-pattern check rather than a handful of representative numbers.
    constexpr uint32 NumRandomFloatPatterns = 2000000u;
    constexpr uint32 NumRandomDoublePatterns = 2000000u;
    uint64 FloatState = 0x123456789ABCDEF0ull;
    for (uint32 Index = 0; Index < NumRandomFloatPatterns; ++Index)
    {
        CheckFloat(TEXT("hashed random"), static_cast<uint32>(NextHashedBits(FloatState)));
    }

    uint64 DoubleState = 0x0FEDCBA987654321ull;
    for (uint32 Index = 0; Index < NumRandomDoublePatterns; ++Index)
    {
        CheckDouble(TEXT("hashed random"), NextHashedBits(DoubleState));
    }

    AddInfo(FString::Printf(
        TEXT("Compared %d explicit + %u hashed float patterns and %d explicit + %u hashed double "
             "patterns; float mismatches=%d, double mismatches=%d."),
        UE_ARRAY_COUNT(FloatPatterns), NumRandomFloatPatterns,
        UE_ARRAY_COUNT(DoublePatterns), NumRandomDoublePatterns,
        FloatMismatches, DoubleMismatches));

    TestEqual(TEXT("float finite predicate has no mismatches"), FloatMismatches, 0);
    TestEqual(TEXT("double finite predicate has no mismatches"), DoubleMismatches, 0);
    return FloatMismatches == 0 && DoubleMismatches == 0;
}

#endif // WITH_DEV_AUTOMATION_TESTS
