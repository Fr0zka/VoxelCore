// Soundness and empirical coverage for the worker-local worm N1 block proof.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "VoxelCaveMorphology.h"
#include "VoxelNoise.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FVoxelForgeWormBlockSkipTest,
    "VoxelForge.Determinism.WormBlockSkipSoundness",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

namespace
{
    constexpr float WormFrequency = 0.015f;
    constexpr float WormHorizontalBias = 3.0f;
    constexpr float WormThreshold = 0.06f;
    constexpr float WormVerticalScale = 1.0f;
    constexpr int32 TestCellsPerAxis = 32;
    constexpr int32 TestTileCount = 192;
    constexpr int32 TestRandomPairCount = 500000;

    FORCEINLINE uint32 NextTestBits(uint32& State)
    {
        State += 0x9E3779B9u;
        uint32 Z = State;
        Z = (Z ^ (Z >> 16)) * 0x7FEB352Du;
        Z = (Z ^ (Z >> 15)) * 0x846CA68Bu;
        return Z ^ (Z >> 16);
    }

    FORCEINLINE float NextTestUnit(uint32& State)
    {
        return static_cast<float>(NextTestBits(State)) / 4294967296.0f;
    }

    FORCEINLINE float ScaledN1(const FVector3f& Position)
    {
        return FMath::Abs(VoxelNoise::Perlin3D(Position) * VOXEL_NOISE_SCALE);
    }

    FORCEINLINE FVector3f MakeWormNoisePosition(
        int32 WorldX, int32 WorldY, int32 WorldZ, uint32 Seed)
    {
        float EffectiveZ = static_cast<float>(WorldZ);
        if (WormVerticalScale != 1.0f && WormVerticalScale > 0.0f)
        {
            EffectiveZ /= WormVerticalScale;
        }
        const float WormZFreq = WormFrequency * WormHorizontalBias;
        return FVector3f(
            static_cast<float>(WorldX) * WormFrequency
                + VoxelHash::SeedOffset(Seed, 1.0f),
            static_cast<float>(WorldY) * WormFrequency
                + VoxelHash::SeedOffset(Seed, 1.7f),
            EffectiveZ * WormZFreq + VoxelHash::SeedOffset(Seed, 2.3f));
    }

    FORCEINLINE double FadePrime(double T)
    {
        return 30.0 * T * T * (1.0 - T) * (1.0 - T);
    }

    FORCEINLINE double GradientSupport(double X, double Y, double Z)
    {
        const double AX = FMath::Abs(X);
        const double AY = FMath::Abs(Y);
        const double AZ = FMath::Abs(Z);
        return FMath::Max(FMath::Max(AX + AY, AX + AZ), AY + AZ);
    }

    // Supremum of the exact finite support relaxation used in the header derivation.  This is
    // intentionally independent of the hash: all eight corners may choose any member of the
    // actual GradDot set, so this is the field-wide proof envelope rather than an average sample.
    double SupportRelaxedXDerivative(double X, double Y, double Z)
    {
        const double D = FadePrime(X);
        const double SY = Y * Y * Y * (Y * (Y * 6.0 - 15.0) + 10.0);
        const double SZ = Z * Z * Z * (Z * (Z * 6.0 - 15.0) + 10.0);
        double Result = 0.0;
        for (int32 J = 0; J < 2; ++J)
        {
            for (int32 K = 0; K < 2; ++K)
            {
                const double Weight = (J != 0 ? SY : 1.0 - SY)
                    * (K != 0 ? SZ : 1.0 - SZ);
                const double YLocal = Y - static_cast<double>(J);
                const double ZLocal = Z - static_cast<double>(K);
                Result += Weight * (
                    GradientSupport(1.0 - X - D * X, -D * YLocal, -D * ZLocal)
                    + GradientSupport(X - D * (1.0 - X), D * YLocal, D * ZLocal));
            }
        }
        return Result;
    }
}

bool FVoxelForgeWormBlockSkipTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    const double FullL = VoxelNoise::WormN1NoiseSpaceLipschitzBound;
    const double HalfL = FullL * 0.5;
    TestTrue(TEXT("the production N1 Lipschitz bound is finite and positive"),
             FMath::IsFinite(FullL) && FullL > 0.0);
    TestTrue(TEXT("the actual 3D partial bound is 15/4"),
             FMath::IsNearlyEqual(VoxelNoise::Perlin3DPartialAbsBound, 3.75, 1.0e-12));
    TestTrue(TEXT("VOXEL_NOISE_SCALE is included in the N1 bound"),
             FMath::IsNearlyEqual(
                 FullL,
                 static_cast<double>(VOXEL_NOISE_SCALE)
                     * VoxelNoise::Perlin3DPartialAbsBound
                     * static_cast<double>(1.732050807568877293527446341505872L),
                 1.0e-12));

    // Check the finite support maximization densely, including corners and every cell face. The
    // closed-form case split is recorded beside the production constant; this grid is a regression
    // tripwire for changing GradDot/Fade/Lerp without updating that proof.
    double MaxRelaxedSupport = 0.0;
    FVector3f MaxRelaxedPoint = FVector3f::ZeroVector;
    constexpr int32 SupportGrid = 128;
    for (int32 IX = 0; IX <= SupportGrid; ++IX)
    {
        const double X = static_cast<double>(IX) / SupportGrid;
        for (int32 IY = 0; IY <= SupportGrid; ++IY)
        {
            const double Y = static_cast<double>(IY) / SupportGrid;
            for (int32 IZ = 0; IZ <= SupportGrid; ++IZ)
            {
                const double Z = static_cast<double>(IZ) / SupportGrid;
                const double Value = SupportRelaxedXDerivative(X, Y, Z);
                if (Value > MaxRelaxedSupport)
                {
                    MaxRelaxedSupport = Value;
                    MaxRelaxedPoint = FVector3f(
                        static_cast<float>(X), static_cast<float>(Y), static_cast<float>(Z));
                }
            }
        }
    }
    AddInfo(FString::Printf(
        TEXT("3D support relaxation max=%.12f at (%.6f,%.6f,%.6f), bound=%.12f; "
             "the exact case split gives 15/4 at (0.5,0.5,0.5)."),
        MaxRelaxedSupport, MaxRelaxedPoint.X, MaxRelaxedPoint.Y, MaxRelaxedPoint.Z,
        VoxelNoise::Perlin3DPartialAbsBound));
    TestTrue(TEXT("the dense support relaxation stays below the proven partial bound"),
             MaxRelaxedSupport <= VoxelNoise::Perlin3DPartialAbsBound + 1.0e-10);
    TestTrue(TEXT("the support relaxation reaches the stated supremum"),
             MaxRelaxedSupport >= VoxelNoise::Perlin3DPartialAbsBound - 1.0e-10);

    // Empirical attack: samples straddle lattice faces/corners at several scales, then a large
    // deterministic random-pair population probes ordinary cells and arbitrary directions.
    double MaxEmpiricalRatio = 0.0;
    FVector3f WorstA = FVector3f::ZeroVector;
    FVector3f WorstB = FVector3f::ZeroVector;
    uint64 PairCount = 0;
    auto ObservePair = [&](const FVector3f& A, const FVector3f& B)
    {
        const double DX = static_cast<double>(A.X) - static_cast<double>(B.X);
        const double DY = static_cast<double>(A.Y) - static_cast<double>(B.Y);
        const double DZ = static_cast<double>(A.Z) - static_cast<double>(B.Z);
        const double Distance = FMath::Sqrt(DX * DX + DY * DY + DZ * DZ);
        if (!(Distance > 0.0))
        {
            return;
        }
        ++PairCount;
        const double Ratio = FMath::Abs(
            static_cast<double>(ScaledN1(A)) - static_cast<double>(ScaledN1(B))) / Distance;
        if (Ratio > MaxEmpiricalRatio)
        {
            MaxEmpiricalRatio = Ratio;
            WorstA = A;
            WorstB = B;
        }
    };

    constexpr float FaceEpsilons[] = { 1.0e-5f, 1.0e-4f, 1.0e-3f, 1.0e-2f, 5.0e-2f };
    constexpr float FaceCoordinates[] = {
        1.0e-3f, 1.0e-2f, 0.1f, 0.25f, 0.5f, 0.75f, 0.9f, 0.99f, 0.999f };
    for (int32 X = -12; X < 12; ++X)
    {
        for (int32 Y = -12; Y < 12; ++Y)
        {
            for (int32 Z = -12; Z < 12; ++Z)
            {
                for (const float Epsilon : FaceEpsilons)
                {
                    for (const float V : FaceCoordinates)
                    {
                        for (const float W : FaceCoordinates)
                        {
                            ObservePair(
                                FVector3f(X + 1.0f - Epsilon, Y + V, Z + W),
                                FVector3f(X + 1.0f + Epsilon, Y + V, Z + W));
                            ObservePair(
                                FVector3f(X + V, Y + 1.0f - Epsilon, Z + W),
                                FVector3f(X + V, Y + 1.0f + Epsilon, Z + W));
                            ObservePair(
                                FVector3f(X + V, Y + W, Z + 1.0f - Epsilon),
                                FVector3f(X + V, Y + W, Z + 1.0f + Epsilon));
                        }
                    }
                    ObservePair(
                        FVector3f(X + 1.0f - Epsilon, Y + 1.0f - Epsilon, Z + 1.0f - Epsilon),
                        FVector3f(X + 1.0f + Epsilon, Y + 1.0f + Epsilon, Z + 1.0f + Epsilon));
                }
            }
        }
    }

    uint32 RandomState = 0xA5B3571Du;
    for (int32 Index = 0; Index < TestRandomPairCount; ++Index)
    {
        const FVector3f A(
            NextTestUnit(RandomState) * 128.0f - 64.0f,
            NextTestUnit(RandomState) * 128.0f - 64.0f,
            NextTestUnit(RandomState) * 128.0f - 64.0f);
        const FVector3f B(
            A.X + NextTestUnit(RandomState) * 0.5f - 0.25f,
            A.Y + NextTestUnit(RandomState) * 0.5f - 0.25f,
            A.Z + NextTestUnit(RandomState) * 0.5f - 0.25f);
        ObservePair(A, B);
    }
    AddInfo(FString::Printf(
        TEXT("Empirical scaled-N1 Lipschitz attack: pairs=%llu worst=%.9f, worst/L=%.9f, "
             "L=%.9f; worst A=(%.6f,%.6f,%.6f) B=(%.6f,%.6f,%.6f)."),
        static_cast<unsigned long long>(PairCount), MaxEmpiricalRatio,
        MaxEmpiricalRatio / FullL, FullL,
        WorstA.X, WorstA.Y, WorstA.Z, WorstB.X, WorstB.Y, WorstB.Z));
    TestTrue(TEXT("empirical near-face/corner/random ratios stay below L"),
             MaxEmpiricalRatio <= FullL + 0.01);

    // Brute-force the exact mesher lattice for a large deterministic set of tile contexts. Every
    // block admitted by the production proof is checked at every lattice point it covers.
    uint64 BlocksScanned = 0;
    uint64 ProductionProofs = 0;
    uint64 ProductionLatticeSamples = 0;
    uint64 ProductionViolations = 0;
    for (int32 TileIndex = 0; TileIndex < TestTileCount; ++TileIndex)
    {
        const FIntVector TileOrigin(
            (TileIndex % 17 - 8) * 64,
            ((TileIndex / 17) % 13 - 6) * 64,
            ((TileIndex / 221) % 9 - 4) * 64);
        const uint32 Seed = 1337u + static_cast<uint32>(TileIndex) * 0x9E3779B9u;

        for (int32 BlockZ = 0; BlockZ < 9; ++BlockZ)
        {
            for (int32 BlockY = 0; BlockY < 9; ++BlockY)
            {
                for (int32 BlockX = 0; BlockX < 9; ++BlockX)
                {
                    ++BlocksScanned;
                    const int32 MinX = BlockX * VoxelNoise::WormBlockSampleSide - 1;
                    const int32 MinY = BlockY * VoxelNoise::WormBlockSampleSide - 1;
                    const int32 MinZ = BlockZ * VoxelNoise::WormBlockSampleSide - 1;
                    const int32 MaxX = FMath::Min(
                        TestCellsPerAxis + 1,
                        MinX + VoxelNoise::WormBlockSampleSide - 1);
                    const int32 MaxY = FMath::Min(
                        TestCellsPerAxis + 1,
                        MinY + VoxelNoise::WormBlockSampleSide - 1);
                    const int32 MaxZ = FMath::Min(
                        TestCellsPerAxis + 1,
                        MinZ + VoxelNoise::WormBlockSampleSide - 1);
                    const FVector3f Q0 = MakeWormNoisePosition(
                        TileOrigin.X + MinX, TileOrigin.Y + MinY, TileOrigin.Z + MinZ, Seed);
                    const FVector3f Q1 = MakeWormNoisePosition(
                        TileOrigin.X + MaxX, TileOrigin.Y + MaxY, TileOrigin.Z + MaxZ, Seed);
                    const FVector3f QMin(
                        FMath::Min(Q0.X, Q1.X), FMath::Min(Q0.Y, Q1.Y), FMath::Min(Q0.Z, Q1.Z));
                    const FVector3f QMax(
                        FMath::Max(Q0.X, Q1.X), FMath::Max(Q0.Y, Q1.Y), FMath::Max(Q0.Z, Q1.Z));
                    const FVector3f QCenter(
                        static_cast<float>((static_cast<double>(QMin.X) + QMax.X) * 0.5),
                        static_cast<float>((static_cast<double>(QMin.Y) + QMax.Y) * 0.5),
                        static_cast<float>((static_cast<double>(QMin.Z) + QMax.Z) * 0.5));

                    if (!VoxelNoise::ProvesScaledAbsPerlin3DAbove(
                            QCenter, QMin, QMax, WormThreshold, FullL))
                    {
                        continue;
                    }
                    ++ProductionProofs;
                    for (int32 Z = MinZ; Z <= MaxZ; ++Z)
                    {
                        for (int32 Y = MinY; Y <= MaxY; ++Y)
                        {
                            for (int32 X = MinX; X <= MaxX; ++X)
                            {
                                ++ProductionLatticeSamples;
                                const float ExactN1 = ScaledN1(MakeWormNoisePosition(
                                    TileOrigin.X + X, TileOrigin.Y + Y, TileOrigin.Z + Z, Seed));
                                if (ExactN1 < WormThreshold)
                                {
                                    ++ProductionViolations;
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    AddInfo(FString::Printf(
        TEXT("Production block audit: tiles=%d blocks=%llu proofs=%llu lattice_samples=%llu "
             "violations=%llu, side=%d, threshold=%.6f."),
        TestTileCount, static_cast<unsigned long long>(BlocksScanned),
        static_cast<unsigned long long>(ProductionProofs),
        static_cast<unsigned long long>(ProductionLatticeSamples),
        static_cast<unsigned long long>(ProductionViolations),
        VoxelNoise::WormBlockSampleSide, WormThreshold));
    TestTrue(TEXT("the production-bound block audit found at least one proof"),
             ProductionProofs > 0);
    TestEqual(TEXT("every production-bound proof is sound on its exact lattice"),
              ProductionViolations, static_cast<uint64>(0));

    // Fail-capable negative control. This pair is from a real low-coordinate hash cell. Its exact
    // scaled-N1 slope is above L/2; the threshold is selected between the two exact values, so a
    // deliberately halved bound admits a proof that the second lattice point violates.
    const FVector3f WitnessCenter(1510.5478515625f, 2191.43701171875f, 7279.404296875f);
    const FVector3f WitnessPoint(1510.54931640625f, 2191.437255859375f, 7279.50390625f);
    const FVector3f WitnessMin(
        2.0f * WitnessCenter.X - WitnessPoint.X,
        2.0f * WitnessCenter.Y - WitnessPoint.Y,
        2.0f * WitnessCenter.Z - WitnessPoint.Z);
    const FVector3f WitnessMax = WitnessPoint;
    constexpr float WitnessThreshold = 0.015f;
    const float WitnessCenterN1 = ScaledN1(WitnessCenter);
    const float WitnessPointN1 = ScaledN1(WitnessPoint);
    const bool bHalfProof = VoxelNoise::ProvesScaledAbsPerlin3DAbove(
        WitnessCenter, WitnessMin, WitnessMax, WitnessThreshold, HalfL);
    const bool bFullProof = VoxelNoise::ProvesScaledAbsPerlin3DAbove(
        WitnessCenter, WitnessMin, WitnessMax, WitnessThreshold, FullL);
    AddInfo(FString::Printf(
        TEXT("Halved-L negative control: center_N1=%.9f point_N1=%.9f threshold=%.6f "
             "half_proof=%d full_proof=%d."),
        WitnessCenterN1, WitnessPointN1, WitnessThreshold,
        bHalfProof ? 1 : 0, bFullProof ? 1 : 0));
    TestTrue(TEXT("the deliberately halved L admits the negative-control block"), bHalfProof);
    TestFalse(TEXT("the full L rejects the negative-control block"), bFullProof);
    TestTrue(TEXT("the negative-control lattice point really violates N1 >= threshold"),
             WitnessPointN1 < WitnessThreshold);
    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
