// Pinned contract for VoxelMath::DetSin/DetCos/DetSinCos.

#include "Misc/AutomationTest.h"
#include "VoxelTypes.h"

#include <math.h>

namespace
{
    struct FDetSinCosGolden
    {
        float Input;
        uint32 SinBits;
        uint32 CosBits;
    };

    uint32 FloatBits(float Value)
    {
        uint32 Bits = 0;
        FMemory::Memcpy(&Bits, &Value, sizeof(Bits));
        return Bits;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FVoxelForgeDetSinCosTest,
    "VoxelForge.Determinism.DetSinCos",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVoxelForgeDetSinCosTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    const float Pi = 3.1415926535897932384626433832795f;
    const float TwoPi = 6.283185307179586476925286766559f;
    // The expected values are deliberately bit patterns rather than tolerances.  This corpus is
    // the pinned implementation contract; update it only with an intentional world re-tune.
    static const FDetSinCosGolden Corpus[] =
    {
        { 0.0f,          0x00000000u, 0x3F800000u },
        {-0.0f,          0x80000000u, 0x3F800000u },
        { Pi,             0xB3BBBD2Eu, 0xBF800000u },
        {-Pi,             0x33BBBD2Eu, 0xBF800000u },
        { TwoPi,          0x343BBD2Eu, 0x3F800000u },
        {-TwoPi,          0xB43BBD2Eu, 0x3F800000u },
        { 0.001f,         0x3A83126Du, 0x3F7FFFF8u },
        {-0.001f,         0xBA83126Du, 0x3F7FFFF8u },
        { 123456.789f,    0xBF7FA83Du, 0x3D53E806u },
        {-123456.789f,    0x3F7FA83Du, 0x3D53E806u },
        { 1000000.25f,    0xBDDBDDEBu, 0x3F7E853Du },
        {-1000000.25f,   0x3DDBDDEBu, 0x3F7E853Du },
    };

    for (const FDetSinCosGolden& Sample : Corpus)
    {
        float ActualSin = 0.0f;
        float ActualCos = 0.0f;
        VoxelMath::DetSinCos(ActualSin, ActualCos, Sample.Input);
        AddInfo(FString::Printf(
            TEXT("DetSinCos input %.9g -> sin=0x%08X cos=0x%08X"),
            Sample.Input, FloatBits(ActualSin), FloatBits(ActualCos)));
        TestEqual(
            FString::Printf(TEXT("sin bits for %.9g"), Sample.Input),
            FloatBits(ActualSin), Sample.SinBits);
        TestEqual(
            FString::Printf(TEXT("cos bits for %.9g"), Sample.Input),
            FloatBits(ActualCos), Sample.CosBits);
        TestEqual(
            FString::Printf(TEXT("DetSin agrees with paired sin for %.9g"), Sample.Input),
            FloatBits(VoxelMath::DetSin(Sample.Input)), FloatBits(ActualSin));
        TestEqual(
            FString::Printf(TEXT("DetCos agrees with paired cos for %.9g"), Sample.Input),
            FloatBits(VoxelMath::DetCos(Sample.Input)), FloatBits(ActualCos));
    }

    // The canonical export measured |argument| <= 6.19255877.  Sweep a small margin around that
    // envelope; the large corpus above separately exercises the double range reduction.
    constexpr float MeasuredWorldArgumentMax = 6.25f;
    constexpr int32 SweepSamples = 200001;
    float MaxSinError = 0.0f;
    float MaxCosError = 0.0f;
    for (int32 Index = 0; Index < SweepSamples; ++Index)
    {
        const float Alpha = -MeasuredWorldArgumentMax
            + (2.0f * MeasuredWorldArgumentMax)
                * static_cast<float>(Index) / static_cast<float>(SweepSamples - 1);
        float ActualSin = 0.0f;
        float ActualCos = 0.0f;
        VoxelMath::DetSinCos(ActualSin, ActualCos, Alpha);
        MaxSinError = FMath::Max(MaxSinError, FMath::Abs(ActualSin - ::sinf(Alpha)));
        MaxCosError = FMath::Max(MaxCosError, FMath::Abs(ActualCos - ::cosf(Alpha)));
    }
    AddInfo(FString::Printf(
        TEXT("DetSinCos max abs error vs sinf/cosf over [-%.1f,+%.1f], %d samples: "
             "sin %.9g, cos %.9g"),
        MeasuredWorldArgumentMax, MeasuredWorldArgumentMax, SweepSamples,
        MaxSinError, MaxCosError));
    TestTrue(TEXT("DetSin max error over measured world range <= 2e-6"), MaxSinError <= 2.0e-6f);
    TestTrue(TEXT("DetCos max error over measured world range <= 2e-6"), MaxCosError <= 2.0e-6f);

    return true;
}
