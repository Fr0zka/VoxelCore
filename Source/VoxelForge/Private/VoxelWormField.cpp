// VoxelWormField.cpp
// Shared deterministic worm-field evaluation and the measured lattice experiment.

#include "VoxelWormField.h"

#include "VoxelCaveMorphology.h" // VoxelHash::SeedOffset
#include "VoxelGenerator.h"       // per-worker canonical tile window
#include "VoxelNoise.h"
#include "VoxelTypes.h"           // VOXEL_NOISE_SCALE

#include "HAL/IConsoleManager.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

#include <atomic>
#include <cfloat>
#include <cstring>

namespace
{
    // The selected production mode is resolved once, before a generation stack is allowed to
    // observe it, so all workers in a process use one world definition. Mode 0 remains the exact
    // legacy control for measurements and compatibility investigations.
    int32 GVoxelForgeWormLatticeStep = 0;
    std::atomic<int32> GResolvedWormLatticeStep(-1);
    int32 GVoxelForgeWormNoiseMode = 1;
    std::atomic<int32> GResolvedWormNoiseMode(-1);

    // The value field has the same threshold/strength contract as legacy Perlin.  This small
    // amplitude retune keeps the authored TunnelNetwork player-fit margin after replacing the
    // gradient evaluation with the cheaper scalar hash.  It is a code default, not a CVar, so a
    // client cannot silently choose a different world.
    constexpr float CheapWormNoiseAmplitude = 1.0f;

    FAutoConsoleVariableRef CVarVoxelForgeWormLatticeStep(
        TEXT("voxel.WormLatticeStep"),
        GVoxelForgeWormLatticeStep,
        TEXT("WORLD-CHANGING development experiment. 0=exact, 2/4=coarse worm lattice with "
             "trilinear interpolation. All multiplayer peers and regenerated worlds must use "
             "the same value. Default 0 until measured."),
        ECVF_Default);

    FAutoConsoleVariableRef CVarVoxelForgeWormNoiseMode(
        TEXT("voxel.WormNoiseMode"),
        GVoxelForgeWormNoiseMode,
        TEXT("WORLD-CHANGING development experiment. 0=legacy gradient Perlin, 1=cheap "
             "deterministic value-noise worm fields. All multiplayer peers and regenerated "
             "worlds must use the same value. Default 1 (the measured production mode)."),
        ECVF_Default);

    int32 ResolveLatticeStep()
    {
        int32 Existing = GResolvedWormLatticeStep.load(std::memory_order_acquire);
        if (Existing >= 0)
        {
            return Existing;
        }

        int32 CommandLineValue = GVoxelForgeWormLatticeStep;
        FParse::Value(
            FCommandLine::Get(), TEXT("voxel.WormLatticeStep="), CommandLineValue);
        const int32 Resolved = CommandLineValue == 2 || CommandLineValue == 4
            ? CommandLineValue : 0;
        int32 Expected = -1;
        if (!GResolvedWormLatticeStep.compare_exchange_strong(
                Expected, Resolved, std::memory_order_release, std::memory_order_acquire))
        {
            return Expected;
        }
        GVoxelForgeWormLatticeStep = Resolved;
        return Resolved;
    }

    int32 ResolveNoiseMode()
    {
        int32 Existing = GResolvedWormNoiseMode.load(std::memory_order_acquire);
        if (Existing >= 0)
        {
            return Existing;
        }

        int32 CommandLineValue = GVoxelForgeWormNoiseMode;
        FParse::Value(
            FCommandLine::Get(), TEXT("voxel.WormNoiseMode="), CommandLineValue);
        const int32 Resolved = CommandLineValue == 1 ? 1 : 0;
        int32 Expected = -1;
        if (!GResolvedWormNoiseMode.compare_exchange_strong(
                Expected, Resolved, std::memory_order_release, std::memory_order_acquire))
        {
            return Expected;
        }
        GVoxelForgeWormNoiseMode = Resolved;
        return Resolved;
    }

    FORCEINLINE FVector3f MakeWormNoiseBase(
        float WorldX,
        float WorldY,
        float WorldZ,
        const VoxelWormField::FParameters& Parameters)
    {
        const float EffectiveZ = (Parameters.VerticalScale != 1.0f
                                  && Parameters.VerticalScale > 0.0f)
            ? WorldZ / Parameters.VerticalScale : WorldZ;
        const float WormZFrequency = Parameters.Frequency * Parameters.HorizontalBias;
        return FVector3f(
            WorldX * Parameters.Frequency
                + VoxelHash::SeedOffset(Parameters.Seed, 1.0f),
            WorldY * Parameters.Frequency
                + VoxelHash::SeedOffset(Parameters.Seed, 1.7f),
            EffectiveZ * WormZFrequency
                + VoxelHash::SeedOffset(Parameters.Seed, 2.3f));
    }

    FORCEINLINE float EvaluateN1Field(
        float WorldX,
        float WorldY,
        float WorldZ,
        const VoxelWormField::FParameters& Parameters,
        int32 NoiseMode,
        float NoiseScale)
    {
        const FVector3f NoiseBase = MakeWormNoiseBase(
            WorldX, WorldY, WorldZ, Parameters);
        return FMath::Abs(
            (NoiseMode == 0
                ? VoxelNoise::Perlin3D(NoiseBase)
                : VoxelNoise::ValueNoise3D(NoiseBase, Parameters.Seed, 0x4D3A2F19u))
            * NoiseScale);
    }

    FORCEINLINE float EvaluateN2Field(
        float WorldX,
        float WorldY,
        float WorldZ,
        const VoxelWormField::FParameters& Parameters,
        int32 NoiseMode,
        float NoiseScale)
    {
        const FVector3f NoiseBase = MakeWormNoiseBase(
            WorldX, WorldY, WorldZ, Parameters);
        const FVector3f N2Position = NoiseBase + FVector3f(137.0f, 259.0f, 431.0f);
        return FMath::Abs(
            (NoiseMode == 0
                ? VoxelNoise::Perlin3D(N2Position)
                : VoxelNoise::ValueNoise3D(N2Position, Parameters.Seed, 0xA7C15E2Du))
            * NoiseScale);
    }

    FORCEINLINE float EvaluateExactField(
        float WorldX,
        float WorldY,
        float WorldZ,
        float Threshold,
        const VoxelWormField::FParameters& Parameters)
    {
        const int32 NoiseMode = ResolveNoiseMode();
        const float NoiseScale = VOXEL_NOISE_SCALE
            * (NoiseMode == 1 ? CheapWormNoiseAmplitude : 1.0f);
        const float N1 = EvaluateN1Field(
            WorldX, WorldY, WorldZ, Parameters, NoiseMode, NoiseScale);

        // N2 is non-negative, so this preserves the old exact evaluator's short-circuit.
        if (N1 >= Threshold)
        {
            return N1;
        }
        return N1 + EvaluateN2Field(
            WorldX, WorldY, WorldZ, Parameters, NoiseMode, NoiseScale);
    }

    struct FLatticeNode
    {
        int32 X = 0;
        int32 Y = 0;
        int32 Z = 0;
        float Value = 0.0f;
        bool bValid = false;
    };

    // The shared density grid is traversed z/y/x. Retaining two adjacent lattice planes is
    // necessary for the z transition; 2048 entries evicted the previous plane and made the
    // experiment slower than exact evaluation.  This is fixed storage, not a heap allocation, and
    // remains deterministic on every worker.
    constexpr uint32 LatticeCacheSize = 32768;
    static_assert((LatticeCacheSize & (LatticeCacheSize - 1u)) == 0,
        "The lattice cache size must remain a power of two for the mask lookup.");
    constexpr int32 TileLatticeAxis = 32;
    static_assert(static_cast<uint32>(TileLatticeAxis * TileLatticeAxis * TileLatticeAxis)
                      == LatticeCacheSize,
        "The direct tile lattice must use the fixed cache storage exactly.");

    struct FLatticeCache
    {
        uint32 Seed = 0;
        uint32 FrequencyBits = 0;
        uint32 HorizontalBiasBits = 0;
        uint32 VerticalScaleBits = 0;
        int32 Step = 0;
        bool bUseTileWindow = false;
        FIntVector TileBaseGrid = FIntVector::ZeroValue;
        int32 TileSampleStep = 0;
        int32 TileCellsPerAxis = 0;
        bool bContextValid = false;
        FLatticeNode Nodes[LatticeCacheSize];

        void SetContext(
            const VoxelWormField::FParameters& Parameters,
            int32 InStep,
            bool InUseTileWindow,
            const FIntVector& InTileOrigin,
            int32 InTileSampleStep,
            int32 InTileCellsPerAxis)
        {
            uint32 FrequencyBitsIn = 0;
            uint32 HorizontalBiasBitsIn = 0;
            uint32 VerticalScaleBitsIn = 0;
            FMemory::Memcpy(&FrequencyBitsIn, &Parameters.Frequency, sizeof(uint32));
            FMemory::Memcpy(&HorizontalBiasBitsIn, &Parameters.HorizontalBias, sizeof(uint32));
            FMemory::Memcpy(&VerticalScaleBitsIn, &Parameters.VerticalScale, sizeof(uint32));
            const double WormStepD = static_cast<double>(InStep);
            const FIntVector InTileBaseGrid(
                FMath::FloorToInt(static_cast<double>(InTileOrigin.X) / WormStepD) - 2,
                FMath::FloorToInt(static_cast<double>(InTileOrigin.Y) / WormStepD) - 2,
                FMath::FloorToInt(static_cast<double>(InTileOrigin.Z) / WormStepD) - 2);
            if (bContextValid
                && Seed == Parameters.Seed
                && FrequencyBits == FrequencyBitsIn
                && HorizontalBiasBits == HorizontalBiasBitsIn
                && VerticalScaleBits == VerticalScaleBitsIn
                && Step == InStep
                && bUseTileWindow == InUseTileWindow
                && TileBaseGrid == InTileBaseGrid
                && TileSampleStep == InTileSampleStep
                && TileCellsPerAxis == InTileCellsPerAxis)
            {
                return;
            }

            Seed = Parameters.Seed;
            FrequencyBits = FrequencyBitsIn;
            HorizontalBiasBits = HorizontalBiasBitsIn;
            VerticalScaleBits = VerticalScaleBitsIn;
            Step = InStep;
            bUseTileWindow = InUseTileWindow;
            TileBaseGrid = InTileBaseGrid;
            TileSampleStep = InTileSampleStep;
            TileCellsPerAxis = InTileCellsPerAxis;
            bContextValid = true;
            for (FLatticeNode& Node : Nodes)
            {
                Node.bValid = false;
            }
        }

        static uint32 Hash(int32 X, int32 Y, int32 Z)
        {
            uint32 H = static_cast<uint32>(X) * 0x9E3779B1u;
            H ^= static_cast<uint32>(Y) * 0x85EBCA77u;
            H ^= static_cast<uint32>(Z) * 0xC2B2AE3Du;
            H ^= H >> 16;
            H *= 0x7FEB352Du;
            H ^= H >> 15;
            return H;
        }

        float Get(int32 X, int32 Y, int32 Z,
                  const VoxelWormField::FParameters& Parameters,
                  int32 NoiseMode,
                  float NoiseScale)
        {
            const auto EvaluateNode = [this, &Parameters, NoiseMode, NoiseScale](FLatticeNode& Node,
                                                            int32 NodeX, int32 NodeY,
                                                            int32 NodeZ) -> float
            {
                if (Node.bValid && Node.X == NodeX && Node.Y == NodeY && Node.Z == NodeZ)
                {
                    return Node.Value;
                }
                Node.X = NodeX;
                Node.Y = NodeY;
                Node.Z = NodeZ;
                const double StepD = static_cast<double>(Step);
                const float WorldX = static_cast<float>(static_cast<double>(NodeX) * StepD);
                const float WorldY = static_cast<float>(static_cast<double>(NodeY) * StepD);
                const float WorldZ = static_cast<float>(static_cast<double>(NodeZ) * StepD);
                // The lattice stores N1 only. The requested voxel uses this interpolated N1 for
                // the threshold decision; N2 remains an exact point query only in the carve
                // region. This removes the first noise call on the hot above-threshold majority
                // without making the second noise unconditional.
                Node.Value = EvaluateN1Field(
                    WorldX, WorldY, WorldZ, Parameters, NoiseMode, NoiseScale);
                Node.bValid = true;
                return Node.Value;
            };

            if (bUseTileWindow)
            {
                const int32 LocalX = X - TileBaseGrid.X;
                const int32 LocalY = Y - TileBaseGrid.Y;
                const int32 LocalZ = Z - TileBaseGrid.Z;
                if (LocalX >= 0 && LocalX < TileLatticeAxis
                    && LocalY >= 0 && LocalY < TileLatticeAxis
                    && LocalZ >= 0 && LocalZ < TileLatticeAxis)
                {
                    FLatticeNode& Node = Nodes[
                        (LocalZ * TileLatticeAxis + LocalY) * TileLatticeAxis + LocalX];
                    return EvaluateNode(Node, X, Y, Z);
                }

                // A malformed or unusually large tile window must not alter the field's finite
                // fallback semantics. It is outside the fixed direct table, so evaluate N1 once
                // for this sample rather than attempting an unbounded allocation.
                const double StepD = static_cast<double>(Step);
                return EvaluateN1Field(
                    static_cast<float>(static_cast<double>(X) * StepD),
                    static_cast<float>(static_cast<double>(Y) * StepD),
                    static_cast<float>(static_cast<double>(Z) * StepD),
                    Parameters, NoiseMode, NoiseScale);
            }

            const uint32 Start = Hash(X, Y, Z) & (LatticeCacheSize - 1u);
            for (uint32 Probe = 0; Probe < LatticeCacheSize; ++Probe)
            {
                FLatticeNode& Node = Nodes[(Start + Probe) & (LatticeCacheSize - 1u)];
                if (Node.bValid)
                {
                    if (Node.X == X && Node.Y == Y && Node.Z == Z)
                    {
                        return Node.Value;
                    }
                    continue;
                }

                Node.X = X;
                Node.Y = Y;
                Node.Z = Z;
                return EvaluateNode(Node, X, Y, Z);
            }

            // A saturated cache is exceptionally unlikely for one density block. Falling back to
            // a direct N1 evaluation keeps the result bounded if a caller exceeds it.
            const double StepD = static_cast<double>(Step);
            return EvaluateN1Field(
                static_cast<float>(static_cast<double>(X) * StepD),
                static_cast<float>(static_cast<double>(Y) * StepD),
                static_cast<float>(static_cast<double>(Z) * StepD),
                Parameters, NoiseMode, NoiseScale);
        }
    };

    thread_local FLatticeCache GLatticeCache;

    FORCEINLINE bool CanUseLattice(float WorldX, float WorldY, float WorldZ, int32 Step)
    {
        if (Step <= 1 || !FMath::IsFinite(WorldX) || !FMath::IsFinite(WorldY)
            || !FMath::IsFinite(WorldZ))
        {
            return false;
        }

        // The integer grid indices must survive the conversion back to a finite float world
        // coordinate. Normal VoxelForge worlds are far inside this envelope; pathological input
        // takes the exact path rather than changing overflow behaviour.
        constexpr double MaxGridCoordinate = 1000000000.0;
        return FMath::Abs(static_cast<double>(WorldX) / static_cast<double>(Step))
                   <= MaxGridCoordinate
            && FMath::Abs(static_cast<double>(WorldY) / static_cast<double>(Step))
                   <= MaxGridCoordinate
            && FMath::Abs(static_cast<double>(WorldZ) / static_cast<double>(Step))
                   <= MaxGridCoordinate;
    }

    FORCEINLINE float Lerp(float A, float B, float Alpha)
    {
        return A + (B - A) * Alpha;
    }

    float EvaluateLattice(
        float WorldX,
        float WorldY,
        float WorldZ,
        const VoxelWormField::FParameters& Parameters,
        int32 Step,
        float NoiseScale)
    {
        const int32 NoiseMode = ResolveNoiseMode();
        const bool bUseTileWindow = VoxelGenLOD::TileCellsPerAxis > 0
            && VoxelGenLOD::TileCellsPerAxis <= CHUNK_SIZE
            && VoxelGenLOD::SampleStep > 0;
        GLatticeCache.SetContext(
            Parameters,
            Step,
            bUseTileWindow,
            VoxelGenLOD::TileOriginVoxels,
            VoxelGenLOD::SampleStep,
            VoxelGenLOD::TileCellsPerAxis);

        const double StepD = static_cast<double>(Step);
        const double GX = static_cast<double>(WorldX) / StepD;
        const double GY = static_cast<double>(WorldY) / StepD;
        const double GZ = static_cast<double>(WorldZ) / StepD;
        const int32 X0 = FMath::FloorToInt(GX);
        const int32 Y0 = FMath::FloorToInt(GY);
        const int32 Z0 = FMath::FloorToInt(GZ);
        const float Tx = static_cast<float>(GX - static_cast<double>(X0));
        const float Ty = static_cast<float>(GY - static_cast<double>(Y0));
        const float Tz = static_cast<float>(GZ - static_cast<double>(Z0));

        const float V000 = GLatticeCache.Get(X0, Y0, Z0, Parameters, NoiseMode, NoiseScale);
        const float V100 = GLatticeCache.Get(X0 + 1, Y0, Z0, Parameters, NoiseMode, NoiseScale);
        const float V010 = GLatticeCache.Get(X0, Y0 + 1, Z0, Parameters, NoiseMode, NoiseScale);
        const float V110 = GLatticeCache.Get(X0 + 1, Y0 + 1, Z0, Parameters, NoiseMode, NoiseScale);
        const float V001 = GLatticeCache.Get(X0, Y0, Z0 + 1, Parameters, NoiseMode, NoiseScale);
        const float V101 = GLatticeCache.Get(X0 + 1, Y0, Z0 + 1, Parameters, NoiseMode, NoiseScale);
        const float V011 = GLatticeCache.Get(X0, Y0 + 1, Z0 + 1, Parameters, NoiseMode, NoiseScale);
        const float V111 = GLatticeCache.Get(X0 + 1, Y0 + 1, Z0 + 1, Parameters, NoiseMode, NoiseScale);

        const float X00 = Lerp(V000, V100, Tx);
        const float X10 = Lerp(V010, V110, Tx);
        const float X01 = Lerp(V001, V101, Tx);
        const float X11 = Lerp(V011, V111, Tx);
        return Lerp(Lerp(X00, X10, Ty), Lerp(X01, X11, Ty), Tz);
    }
}

namespace VoxelWormField
{
    int32 GetLatticeStep()
    {
        return ResolveLatticeStep();
    }

    int32 GetNoiseMode()
    {
        return ResolveNoiseMode();
    }

    float EvaluateExact(
        float WorldX,
        float WorldY,
        float WorldZ,
        float Threshold,
        const FParameters& Parameters)
    {
        return EvaluateExactField(WorldX, WorldY, WorldZ, Threshold, Parameters);
    }

    float Evaluate(
        float WorldX,
        float WorldY,
        float WorldZ,
        float Threshold,
        const FParameters& Parameters,
        int32 LatticeStep)
    {
        const int32 NoiseMode = ResolveNoiseMode();
        const bool bUseLattice = CanUseLattice(WorldX, WorldY, WorldZ, LatticeStep);
        const float NoiseScale = VOXEL_NOISE_SCALE
            * (NoiseMode == 1 ? CheapWormNoiseAmplitude : 1.0f);
        const float N1 = bUseLattice
            ? EvaluateLattice(WorldX, WorldY, WorldZ, Parameters, LatticeStep, NoiseScale)
            : EvaluateN1Field(WorldX, WorldY, WorldZ, Parameters, NoiseMode, NoiseScale);
        if (N1 >= Threshold)
        {
            return N1;
        }
        return N1 + EvaluateN2Field(
            WorldX, WorldY, WorldZ, Parameters, NoiseMode, NoiseScale);
    }
}
