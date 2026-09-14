// VoxelStrateManager.cpp
// Runtime strate layout generation and queries.

#include "VoxelStrateManager.h"
#include "CoreGlobals.h"  // GIsAutomationTesting — the opt-in diagnostic stays quiet under tests
#include "VoxelSettings.h"
#include "VoxelSeasonAsset.h"
#include "VoxelTypes.h"  // For CHUNK_SIZE, VOXEL_SIZE, WorldToChunkCoord
#include "VoxelCaveMorphology.h"  // For VoxelSDF and VoxelHash
#include "VoxelDensityPrimitives.h"  // Shared passage carve polarity/strength
#include "VoxelDensityProfile.h"  // Opt-in targeted per-voxel attribution
#include "VoxelStartupTrace.h"
#include "VoxelTerrainOpDefinition.h"  // For UVoxelTerrainOpDefinition::ApplyTo
#include "VoxelBiomeDefinition.h"  // For UVoxelBiomeDefinition (biome context flatten)
#include "VoxelWormField.h"  // World-changing worm evaluator mode/lattice identity
#include "HAL/IConsoleManager.h"
#include "Misc/CommandLine.h"
#include "Misc/Crc.h"
#include "Misc/Parse.h"
#include "UObject/UObjectGlobals.h"

#include <atomic>

namespace
{
    std::atomic<uint64> GNextStrateManagerLifetimeId { 0 };

    // Exact A/B switch for the repeated passage-floor projection item.  The other field-
    // preserving rewrites in this round are deliberately unconditional; this switch isolates
    // the projection reuse without changing any density arithmetic.
    int32 GVoxelForgeFloorRound1PassageProjectionCache = 1;
    FAutoConsoleVariableRef CVarVoxelForgeFloorRound1PassageProjectionCache(
        TEXT("voxel.FloorRound1PassageProjectionCache"),
        GVoxelForgeFloorRound1PassageProjectionCache,
        TEXT("Reuse exact passage floor projections within one density sample; 0 disables the A/B cache."));

    // This is a measurement override only. The asset-owned bEnableWorms field remains the real
    // per-strate contract; this process-wide switch exists so the game harness can measure the
    // cost of enabled versus disabled worms without editing an owner's asset. It is resolved once
    // before generation params are published so workers cannot disagree mid-world.
    int32 GVoxelForgeWormsForceOff = 0;
    std::atomic<int32> GResolvedWormsForceOff(-1);
    FAutoConsoleVariableRef CVarVoxelForgeWormsForceOff(
        TEXT("voxel.WormsForceOff"),
        GVoxelForgeWormsForceOff,
        TEXT("WORLD-CHANGING DEVELOPMENT-ONLY measurement override. 0=asset behavior, 1=force "
             "all worm strengths to zero. Never vary this between multiplayer peers or world "
             "regenerations; it is not the per-strate off switch."),
        ECVF_Default);

    bool VF_WormsForceOff()
    {
        int32 Existing = GResolvedWormsForceOff.load(std::memory_order_acquire);
        if (Existing >= 0)
        {
            return Existing != 0;
        }

        int32 CommandLineValue = GVoxelForgeWormsForceOff;
        FParse::Value(
            FCommandLine::Get(), TEXT("voxel.WormsForceOff="), CommandLineValue);
        const int32 Resolved = CommandLineValue != 0 ? 1 : 0;
        int32 Expected = -1;
        if (!GResolvedWormsForceOff.compare_exchange_strong(
                Expected, Resolved, std::memory_order_release, std::memory_order_acquire))
        {
            return Expected != 0;
        }
        GVoxelForgeWormsForceOff = Resolved;
        return Resolved != 0;
    }

    struct FRuntimeRoughnessOverrides
    {
        bool bSurfaceRoughness = false;
        float SurfaceRoughness = 0.0f;
        bool bFrequency = false;
        float Frequency = 0.0f;
        bool bNoiseType = false;
        EVoxelNoiseType NoiseType = EVoxelNoiseType::FBM;
    };

    const FRuntimeRoughnessOverrides& VF_GetRuntimeRoughnessOverrides()
    {
        // These are diagnostic command-line inputs, captured once per process.  Keeping the
        // override at the params boundary makes every chunk, classifier, and mesher see the same
        // immutable values without putting a command-line parse in the voxel hot loop.
        static const FRuntimeRoughnessOverrides Overrides = []()
        {
            FRuntimeRoughnessOverrides Result;
            const FString CommandLine(FCommandLine::Get());

            Result.bSurfaceRoughness = FParse::Value(
                *CommandLine, TEXT("voxel.surfaceroughness="), Result.SurfaceRoughness);
            if (Result.bSurfaceRoughness
                && (!VoxelMath::IsFinite(Result.SurfaceRoughness) || Result.SurfaceRoughness < 0.0f))
            {
                Result.bSurfaceRoughness = false;
            }

            Result.bFrequency = FParse::Value(
                *CommandLine, TEXT("voxel.roughnessfrequency="), Result.Frequency);
            if (Result.bFrequency
                && (!VoxelMath::IsFinite(Result.Frequency) || Result.Frequency <= 0.0f))
            {
                Result.bFrequency = false;
            }

            FString NoiseTypeText;
            if (FParse::Value(
                    *CommandLine, TEXT("voxel.roughnesstype="), NoiseTypeText))
            {
                NoiseTypeText.TrimStartAndEndInline();
                NoiseTypeText.ToLowerInline();
                NoiseTypeText.ReplaceInline(TEXT("_"), TEXT(""));
                NoiseTypeText.ReplaceInline(TEXT("-"), TEXT(""));
                if (NoiseTypeText == TEXT("fbm") || NoiseTypeText == TEXT("fractal"))
                {
                    Result.NoiseType = EVoxelNoiseType::FBM;
                    Result.bNoiseType = true;
                }
                else if (NoiseTypeText == TEXT("ridged") || NoiseTypeText == TEXT("ridge"))
                {
                    Result.NoiseType = EVoxelNoiseType::Ridged;
                    Result.bNoiseType = true;
                }
                else if (NoiseTypeText == TEXT("mixed"))
                {
                    Result.NoiseType = EVoxelNoiseType::Mixed;
                    Result.bNoiseType = true;
                }
                else if (NoiseTypeText == TEXT("cellular") || NoiseTypeText == TEXT("worley"))
                {
                    Result.NoiseType = EVoxelNoiseType::Cellular;
                    Result.bNoiseType = true;
                }
            }

            if (Result.bSurfaceRoughness || Result.bFrequency || Result.bNoiseType)
            {
                UE_LOG(LogTemp, Display,
                    TEXT("[StrateManager] Runtime roughness override: strength=%s%.3f frequency=%s%.6f type=%s"),
                    Result.bSurfaceRoughness ? TEXT("") : TEXT("(asset) "),
                    Result.SurfaceRoughness,
                    Result.bFrequency ? TEXT("") : TEXT("(asset) "),
                    Result.Frequency,
                    Result.bNoiseType
                        ? (Result.NoiseType == EVoxelNoiseType::FBM ? TEXT("FBM")
                            : Result.NoiseType == EVoxelNoiseType::Ridged ? TEXT("Ridged")
                            : Result.NoiseType == EVoxelNoiseType::Mixed ? TEXT("Mixed")
                            : TEXT("Cellular"))
                        : TEXT("(asset)"));
            }
            return Result;
        }();
        return Overrides;
    }

    static FStrateGenerationParams VF_ApplyRuntimeRoughnessOverrides(
        FStrateGenerationParams Params)
    {
        const FRuntimeRoughnessOverrides& Overrides = VF_GetRuntimeRoughnessOverrides();
        if (Overrides.bSurfaceRoughness)
        {
            Params.SurfaceRoughness = Overrides.SurfaceRoughness;
        }
        if (Overrides.bFrequency)
        {
            Params.RoughnessFrequency = Overrides.Frequency;
        }
        if (Overrides.bNoiseType)
        {
            Params.RoughnessNoiseType = Overrides.NoiseType;
        }
        return Params;
    }
}

UVoxelStrateManager::UVoxelStrateManager()
    : CacheLifetimeId(GNextStrateManagerLifetimeId.fetch_add(1, std::memory_order_relaxed) + 1)
{
}

#if WITH_EDITOR
#include "VoxelStrateComposer.h"

struct FVoxelStrateComposerSlotOverride
{
    int32 CandidateSeed = 0;
    ECaveGeneratorType Archetype = ECaveGeneratorType::TunnelNetwork;
    FVoxelStrateArchetypeParams Params;
    bool bUseRecipe = false;
    FVoxelOpStackRecipe Recipe;
    bool bUseRegions = false;
    FVoxelStrateRegionManifest Regions;
};

static void VF_SetComposerRuntimeBounds(
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
#endif

// Fractal Brownian Motion (layered Perlin) along a 1D parameter, ~[-1,1].
// Independent octaves at increasing frequency / decreasing amplitude give an organic,
// non-repeating wander — the key to a worm that SQUIRMS instead of zig-zagging (1D) or
// orbiting (single-octave 2-channel = a spiral). Each axis samples this with its own seed.
static float PassageFBM(float X, float Seed)
{
    float Total = 0.0f, Amp = 1.0f, Freq = 1.0f, MaxV = 0.0f;
    for (int32 O = 0; O < 4; ++O)
    {
        Total += FMath::PerlinNoise3D(FVector(X * Freq + Seed, Seed * 1.7f + O * 13.0f, O * 5.0f)) * Amp;
        MaxV += Amp;
        Amp  *= 0.5f;
        Freq *= 2.0f;
    }
    return (MaxV > 0.0f) ? (Total / MaxV) : 0.0f;
}

namespace
{
    // One projection result per passage is enough for the current voxel sample.  The surrounding
    // passage cache already moves with (manager, layout, chunk); a generation invalidates old flags
    // before any caller can observe them.  Keeping the entries indexed by passage avoids the TMap
    // rehash path that this hot loop used to carry, while a collision-free hit remains exact.
    struct FPassageFloorProjectionCacheEntry
    {
        // The sample key lives once in FPassageEvaluationCache.  A monotonically increasing
        // generation makes an entry's old flags inert without clearing its float payload.
        // The payload is only read after the corresponding valid flag is set.
        uint64 SampleGeneration = 0;
        uint8 ComputedFlags = 0;
        bool bNativeValid = false;
        float NativeFloorZ = 0.0f;
        float NativeSupportRadius = 0.0f;
        bool bGenericValid = false;
        float GenericFloorZ = 0.0f;
        float GenericSupportRadius = 0.0f;
        bool bWalkableAir = false;

        enum : uint8
        {
            NativeComputed = 1u << 0,
            GenericComputed = 1u << 1,
            WalkableAirComputed = 1u << 2
        };

        FORCEINLINE bool Has(uint8 Flag) const
        {
            return (ComputedFlags & Flag) != 0;
        }

        FORCEINLINE void SetComputed(uint8 Flag)
        {
            ComputedFlags |= Flag;
        }
    };

    /**
     * One shared passage cache for the tube SDF and landing-floor fill.
     *
     * The source query and all landing dimensions are resolved by GeneratePassages.  This cache
     * only narrows the immutable passage array once per (manager, version, chunk); neither the
     * player-fit stencil nor a topology search can leak into the voxel loop.
     */
    struct FPassageEvaluationCache
    {
        const UVoxelStrateManager* Owner = nullptr;
        uint64 OwnerLifetimeId = 0;
        FIntVector Chunk = FIntVector(INT32_MAX, INT32_MAX, INT32_MAX);
        uint32 Version = 0xFFFFFFFFu;
        TArray<int32> Nearby;
        TArray<FPassageFloorProjectionCacheEntry> FloorProjections;
        bool bFloorProjectionSampleValid = false;
        double FloorProjectionX = 0.0;
        double FloorProjectionY = 0.0;
        double FloorProjectionZ = 0.0;
        uint64 FloorProjectionGeneration = 0;
    };

    FPassageEvaluationCache& VF_GetPassageEvaluationCache()
    {
        thread_local FPassageEvaluationCache Cache;
        return Cache;
    }

    const TArray<int32>& VF_GetNearbyPassages(
        const UVoxelStrateManager* Manager,
        const FIntVector& ChunkCoord)
    {
        FPassageEvaluationCache& Cache = VF_GetPassageEvaluationCache();
        const uint32 Version = Manager ? Manager->GetLayoutVersion() : 0u;
        const uint64 LifetimeId = Manager ? Manager->GetCacheLifetimeId() : 0;
        if (Cache.Owner == Manager && Cache.OwnerLifetimeId == LifetimeId
            && Cache.Chunk == ChunkCoord && Cache.Version == Version)
        {
            return Cache.Nearby;
        }

        Cache.Owner = Manager;
        Cache.OwnerLifetimeId = LifetimeId;
        Cache.Chunk = ChunkCoord;
        Cache.Version = Version;
        Cache.Nearby.Reset();
        Cache.FloorProjections.Reset();
        Cache.bFloorProjectionSampleValid = false;
        if (!Manager) return Cache.Nearby;

        const FVector ChunkCenter(
            (ChunkCoord.X + 0.5f) * (float)CHUNK_SIZE,
            (ChunkCoord.Y + 0.5f) * (float)CHUNK_SIZE,
            (ChunkCoord.Z + 0.5f) * (float)CHUNK_SIZE);
        const float ChunkRadius = (float)CHUNK_SIZE * 0.8660254f + 3.0f;
        const TArray<FVoxelPassage>& Passages = Manager->GetPassages();
        Cache.FloorProjections.SetNum(Passages.Num());
        for (int32 PassageIndex = 0; PassageIndex < Passages.Num(); ++PassageIndex)
        {
            const FVoxelPassage& Passage = Passages[PassageIndex];
            const float Reach = Passage.BoundRadius + ChunkRadius;
            if (FVector::DistSquared(ChunkCenter, Passage.BoundCenter) <= Reach * Reach)
            {
                Cache.Nearby.Add(PassageIndex);
            }
        }
        return Cache.Nearby;
    }

    FPassageFloorProjectionCacheEntry* VF_GetPassageFloorProjectionCacheEntry(
        const UVoxelStrateManager* Manager,
        const FVector& Position,
        int32 PassageIndex)
    {
        if (GVoxelForgeFloorRound1PassageProjectionCache == 0
            || Manager == nullptr || PassageIndex < 0)
        {
            return nullptr;
        }

        FPassageEvaluationCache& Cache = VF_GetPassageEvaluationCache();
        if (Cache.Owner != Manager
            || Cache.OwnerLifetimeId != Manager->GetCacheLifetimeId()
            || Cache.Version != Manager->GetLayoutVersion()
            || PassageIndex >= Cache.FloorProjections.Num())
        {
            return nullptr;
        }

        if (!Cache.bFloorProjectionSampleValid
            || Cache.FloorProjectionX != Position.X
            || Cache.FloorProjectionY != Position.Y
            || Cache.FloorProjectionZ != Position.Z)
        {
            Cache.bFloorProjectionSampleValid = true;
            Cache.FloorProjectionX = Position.X;
            Cache.FloorProjectionY = Position.Y;
            Cache.FloorProjectionZ = Position.Z;
            ++Cache.FloorProjectionGeneration;
            if (Cache.FloorProjectionGeneration == 0)
            {
                // Keep generation zero reserved for never-initialized entries.  This is
                // unreachable in a practical worker lifetime, but preserves the invariant if a
                // process survives long enough to wrap the counter.
                for (FPassageFloorProjectionCacheEntry& Candidate : Cache.FloorProjections)
                {
                    Candidate.SampleGeneration = 0;
                }
                ++Cache.FloorProjectionGeneration;
            }
        }

        FPassageFloorProjectionCacheEntry* Entry =
            Cache.FloorProjections.GetData() + PassageIndex;
        if (Entry->SampleGeneration != Cache.FloorProjectionGeneration)
        {
            Entry->SampleGeneration = Cache.FloorProjectionGeneration;
            Entry->ComputedFlags = 0;
        }
        return Entry;
    }

    // Passage relief is a function of the immutable passage descriptor and the actual XY column.
    // Keep the cache fixed-size and thread-local, like the morphology floor cache: a long export
    // cannot retain one entry per tile, and collisions only lose a hit.  Fractional normal
    // samples intentionally fall through to the exact same two-octave calculation.
    struct FVoxelPassageReliefCacheEntry
    {
        const UVoxelStrateManager* Owner = nullptr;
        uint64 OwnerLifetimeId = 0;
        uint32 LayoutVersion = 0;
        int32 PassageIndex = INDEX_NONE;
        int32 X = 0;
        int32 Y = 0;
        float Noise = 0.0f;
        bool bValid = false;
    };

    struct FVoxelPassageReliefCache
    {
        static constexpr int32 Capacity = 16384;
        static_assert((Capacity & (Capacity - 1)) == 0,
            "passage relief cache must be a power of two");
        FVoxelPassageReliefCacheEntry Entries[Capacity];

        static uint32 HashKey(
            const UVoxelStrateManager* Owner, uint64 LifetimeId, uint32 Version,
            int32 PassageIndex, int32 X, int32 Y)
        {
            uint32 Hash = static_cast<uint32>(reinterpret_cast<UPTRINT>(Owner));
            Hash ^= static_cast<uint32>(reinterpret_cast<UPTRINT>(Owner) >> 32);
            const auto Combine = [&Hash](uint32 Value)
            {
                Hash ^= Value + 0x9E3779B9u + (Hash << 6) + (Hash >> 2);
            };
            Combine(static_cast<uint32>(LifetimeId));
            Combine(static_cast<uint32>(LifetimeId >> 32));
            Combine(Version);
            Combine(static_cast<uint32>(PassageIndex));
            Combine(static_cast<uint32>(X));
            Combine(static_cast<uint32>(Y));
            return VoxelHash::Mix(Hash);
        }
    };

    FVoxelPassageReliefCache& VF_GetPassageReliefCache()
    {
        thread_local FVoxelPassageReliefCache Cache;
        return Cache;
    }

    FORCEINLINE float VF_ComputePassageReliefNoise(
        double X, double Y, uint32 FloorSeed, float Frequency)
    {
        const float SF = static_cast<float>(FloorSeed) * 0.00001f;
        float Noise = FMath::PerlinNoise2D(
            FVector2D(
                static_cast<float>(X) * Frequency + SF,
                static_cast<float>(Y) * Frequency + SF * 1.7f)) * 0.65f;
        Noise += FMath::PerlinNoise2D(
            FVector2D(
                static_cast<float>(X) * Frequency * 2.3f + SF * 3.1f,
                static_cast<float>(Y) * Frequency * 2.3f + SF * 5.3f)) * 0.35f;
        return Noise;
    }

    FORCEINLINE float VF_PassageReliefNoise(
        const UVoxelStrateManager* Manager, int32 PassageIndex,
        const FVoxelPassage& Passage, double X, double Y)
    {
        if (!(Passage.NativeFloorReliefStrength > 0.0f)
            || !VoxelMath::IsFinite(Passage.NativeFloorReliefStrength)
            || !VoxelMath::IsFinite(Passage.NativeFloorReliefFrequency)
            || !(FMath::Abs(Passage.NativeFloorReliefFrequency) > KINDA_SMALL_NUMBER))
        {
            return 0.0f;
        }

        const bool bInteger = VoxelMath::IsFinite(X) && VoxelMath::IsFinite(Y)
            && FMath::FloorToDouble(X) == X && FMath::FloorToDouble(Y) == Y
            && X >= static_cast<double>(MIN_int32) && X <= static_cast<double>(MAX_int32)
            && Y >= static_cast<double>(MIN_int32) && Y <= static_cast<double>(MAX_int32);
        if (!bInteger || Manager == nullptr || PassageIndex == INDEX_NONE)
        {
            return VF_ComputePassageReliefNoise(
                X, Y, Passage.NativeFloorSeed, Passage.NativeFloorReliefFrequency);
        }

        const int32 IX = FMath::FloorToInt(X);
        const int32 IY = FMath::FloorToInt(Y);
        FVoxelPassageReliefCache& Cache = VF_GetPassageReliefCache();
        const uint64 LifetimeId = Manager->GetCacheLifetimeId();
        const uint32 Version = Manager->GetLayoutVersion();
        FVoxelPassageReliefCacheEntry& Entry = Cache.Entries[
            FVoxelPassageReliefCache::HashKey(
                Manager, LifetimeId, Version, PassageIndex, IX, IY)
            & (FVoxelPassageReliefCache::Capacity - 1)];
        if (Entry.bValid
            && Entry.Owner == Manager
            && Entry.OwnerLifetimeId == LifetimeId
            && Entry.LayoutVersion == Version
            && Entry.PassageIndex == PassageIndex
            && Entry.X == IX && Entry.Y == IY)
        {
            return Entry.Noise;
        }

        const float Noise = VF_ComputePassageReliefNoise(
            X, Y, Passage.NativeFloorSeed, Passage.NativeFloorReliefFrequency);
        Entry.Owner = Manager;
        Entry.OwnerLifetimeId = LifetimeId;
        Entry.LayoutVersion = Version;
        Entry.PassageIndex = PassageIndex;
        Entry.X = IX;
        Entry.Y = IY;
        Entry.Noise = Noise;
        Entry.bValid = true;
        return Noise;
    }
}

namespace
{
    // The graph-tunnel relief proof is intentionally repeated here instead of sharing a loose
    // "noise amplitude" constant.  The passage uses the same weighted two-octave field and the
    // same conservative partial-derivative bound; a zero scale is the fail-closed result.
    constexpr float VF_PassagePerlin2DPartialAbsBound = 8.5f;
    constexpr float VF_PassageMaxReliefScale = 0.20f;

    static float VF_BuildPassageReliefScale(
        const FVector& A, const FVector& B,
        float BaseGradient, int32 SegmentIndex, int32 NumSegments,
        float Strength, float Frequency)
    {
        if (!(Strength > 0.0f)
            || !VoxelMath::IsFinite(Strength)
            || !VoxelMath::IsFinite(Frequency)
            || !(FMath::Abs(Frequency) > KINDA_SMALL_NUMBER)
            || !VoxelMath::IsFinite(BaseGradient)
            || BaseGradient < 0.0f)
        {
            return 0.0f;
        }

        const float AvailableGradient =
            VoxelPassageGeometry::PlayerWalkableFloorMaxGradient - BaseGradient;
        if (!(AvailableGradient > 0.0f))
        {
            return 0.0f;
        }

        const float Amplitude = FMath::Abs(Strength) * VOXEL_NOISE_SCALE;
        const float OctaveFrequencyWeight = 0.65f + 0.35f * 2.3f;
        const float NoiseGradientBound =
            1.4142135623730951f * VF_PassagePerlin2DPartialAbsBound
            * OctaveFrequencyWeight * FMath::Abs(Frequency) * Amplitude;
        const int32 LandingEnvelopeCount = (SegmentIndex == 0 ? 1 : 0)
            + (SegmentIndex + 1 == NumSegments ? 1 : 0);
        const float HorizontalRun = FVector2D(
            static_cast<float>(B.X - A.X),
            static_cast<float>(B.Y - A.Y)).Size();
        const float EnvelopeDerivativeBound = LandingEnvelopeCount > 0
            ? (1.5f * static_cast<float>(LandingEnvelopeCount)
                / VoxelPassageGeometry::WalkableTunnelLandingApronVoxels)
                * Amplitude
            : 0.0f;
        const float ReliefGradientBound =
            NoiseGradientBound + EnvelopeDerivativeBound;
        if (!(ReliefGradientBound > KINDA_SMALL_NUMBER)
            || !VoxelMath::IsFinite(ReliefGradientBound)
            || !(HorizontalRun > KINDA_SMALL_NUMBER))
        {
            return 0.0f;
        }

        return FMath::Clamp(
            FMath::Min(
                VF_PassageMaxReliefScale,
                AvailableGradient / ReliefGradientBound),
            0.0f, VF_PassageMaxReliefScale);
    }

    static bool VF_ProjectNativePassageFloorUncached(
        const FVoxelPassage& Passage, const FVector& Position,
        float& OutFloorZ, float& OutSupportRadius,
        const UVoxelStrateManager* Manager = nullptr,
        int32 PassageIndex = INDEX_NONE)
    {
        const int32 ControlPointCount = Passage.ControlPoints.Num();
        if (!Passage.bNativeFloorEnabled
            || ControlPointCount < 2
            || Passage.ControlRadii.Num() != ControlPointCount
            || Passage.NativeFloorProfileZ.Num() != ControlPointCount)
        {
            return false;
        }

        const FVector* ControlPointData = Passage.ControlPoints.GetData();
        const float* ControlRadiusData = Passage.ControlRadii.GetData();
        const float* NativeFloorProfileData = Passage.NativeFloorProfileZ.GetData();
        const double QueryX = static_cast<double>(static_cast<float>(Position.X));
        const double QueryY = static_cast<double>(static_cast<float>(Position.Y));
        float BestDistanceSquared = FLT_MAX;
        int32 BestSegment = INDEX_NONE;
        float BestT = 0.0f;
        float BestSupportRadius = 0.0f;
        for (int32 SegmentIndex = 0;
             SegmentIndex + 1 < ControlPointCount;
             ++SegmentIndex)
        {
            const FVector& A = ControlPointData[SegmentIndex];
            const FVector& B = ControlPointData[SegmentIndex + 1];
            const double AXY_X = static_cast<double>(static_cast<float>(A.X));
            const double AXY_Y = static_cast<double>(static_cast<float>(A.Y));
            const double DeltaX = static_cast<double>(static_cast<float>(B.X - A.X));
            const double DeltaY = static_cast<double>(static_cast<float>(B.Y - A.Y));
            const float LengthSquared = static_cast<float>(
                DeltaX * DeltaX + DeltaY * DeltaY);
            if (!(LengthSquared > KINDA_SMALL_NUMBER))
            {
                continue;
            }

            const double Dot = (QueryX - AXY_X) * DeltaX
                + (QueryY - AXY_Y) * DeltaY;
            const float T = FMath::Clamp(
                Dot / LengthSquared,
                0.0f, 1.0f);
            const double ClosestX = AXY_X + DeltaX * T;
            const double ClosestY = AXY_Y + DeltaY * T;
            const double DistanceX = QueryX - ClosestX;
            const double DistanceY = QueryY - ClosestY;
            const float DistanceSquared = static_cast<float>(
                DistanceX * DistanceX + DistanceY * DistanceY);
            if (DistanceSquared >= BestDistanceSquared)
            {
                continue;
            }

            BestDistanceSquared = DistanceSquared;
            BestSegment = SegmentIndex;
            BestT = T;
            BestSupportRadius = FMath::Max(
                FMath::Min(
                    FMath::Abs(ControlRadiusData[SegmentIndex]),
                    FMath::Abs(ControlRadiusData[SegmentIndex + 1])) - 0.5f,
                VoxelPassageGeometry::PlayerRadiusVoxels);
        }

        if (BestSegment == INDEX_NONE
            || BestDistanceSquared > FMath::Square(BestSupportRadius))
        {
            return false;
        }

        const float ClampedT = FMath::Clamp(BestT, 0.0f, 1.0f);
        OutFloorZ = FMath::Lerp(
            NativeFloorProfileData[BestSegment],
            NativeFloorProfileData[BestSegment + 1],
            ClampedT);

        float ReliefScale = 0.0f;
        const int32 ReliefScaleCount = Passage.NativeFloorReliefScales.Num();
        if (BestSegment >= 0 && BestSegment < ReliefScaleCount)
        {
            ReliefScale = Passage.NativeFloorReliefScales.GetData()[BestSegment];
        }
        float Envelope = 1.0f;
        const FVector& BestA = ControlPointData[BestSegment];
        const FVector& BestB = ControlPointData[BestSegment + 1];
        const double BestDeltaX = static_cast<double>(static_cast<float>(
            BestB.X - BestA.X));
        const double BestDeltaY = static_cast<double>(static_cast<float>(
            BestB.Y - BestA.Y));
        const float HorizontalRun = static_cast<float>(FMath::Sqrt(
            BestDeltaX * BestDeltaX + BestDeltaY * BestDeltaY));
        if (BestSegment == 0)
        {
            Envelope *= SmoothStep01(FMath::Clamp(
                (ClampedT * HorizontalRun)
                    / VoxelPassageGeometry::WalkableTunnelLandingApronVoxels,
                0.0f, 1.0f));
        }
        if (BestSegment + 1 == ControlPointCount - 1)
        {
            Envelope *= SmoothStep01(FMath::Clamp(
                ((1.0f - ClampedT) * HorizontalRun)
                    / VoxelPassageGeometry::WalkableTunnelLandingApronVoxels,
                0.0f, 1.0f));
        }
        OutFloorZ += VF_PassageReliefNoise(
            Manager, PassageIndex, Passage,
            static_cast<double>(Position.X), static_cast<double>(Position.Y))
            * VOXEL_NOISE_SCALE * Passage.NativeFloorReliefStrength
            * Envelope * ReliefScale;
        OutSupportRadius = BestSupportRadius;
        return VoxelMath::IsFinite(OutFloorZ)
            && VoxelMath::IsFinite(OutSupportRadius)
            && OutSupportRadius > 0.0f;
    }

    static bool VF_ProjectNativePassageFloor(
        const FVoxelPassage& Passage, const FVector& Position,
        float& OutFloorZ, float& OutSupportRadius,
        const UVoxelStrateManager* Manager = nullptr,
        int32 PassageIndex = INDEX_NONE)
    {
        FPassageFloorProjectionCacheEntry* Entry =
            VF_GetPassageFloorProjectionCacheEntry(Manager, Position, PassageIndex);
        if (Entry == nullptr)
        {
            return VF_ProjectNativePassageFloorUncached(
                Passage, Position, OutFloorZ, OutSupportRadius,
                Manager, PassageIndex);
        }
        if (!Entry->Has(FPassageFloorProjectionCacheEntry::NativeComputed))
        {
            Entry->bNativeValid = VF_ProjectNativePassageFloorUncached(
                Passage, Position, Entry->NativeFloorZ,
                Entry->NativeSupportRadius, Manager, PassageIndex);
            Entry->SetComputed(FPassageFloorProjectionCacheEntry::NativeComputed);
        }
        if (Entry->bNativeValid)
        {
            OutFloorZ = Entry->NativeFloorZ;
            OutSupportRadius = Entry->NativeSupportRadius;
        }
        return Entry->bNativeValid;
    }

    static bool VF_ProjectPassageFloor(
        const FVoxelPassage& Passage, const FVector& Position,
        float& OutFloorZ, float& OutSupportRadius,
        const UVoxelStrateManager* Manager = nullptr,
        int32 PassageIndex = INDEX_NONE)
    {
        FPassageFloorProjectionCacheEntry* Entry =
            VF_GetPassageFloorProjectionCacheEntry(Manager, Position, PassageIndex);
        if (Entry != nullptr
            && Entry->Has(FPassageFloorProjectionCacheEntry::GenericComputed))
        {
            if (Entry->bGenericValid)
            {
                OutFloorZ = Entry->GenericFloorZ;
                OutSupportRadius = Entry->GenericSupportRadius;
            }
            return Entry->bGenericValid;
        }

        float FloorZ = 0.0f;
        float SupportRadius = 0.0f;
        bool bValid = VF_ProjectNativePassageFloor(
            Passage, Position, FloorZ, SupportRadius,
            Manager, PassageIndex);
        if (!bValid && Passage.bWalkableTunnelContract)
        {
            bValid = VoxelPassageGeometry::ProjectWalkableTunnelFloor(
                Passage.ControlPoints, Passage.ControlRadii, Position,
                FloorZ, SupportRadius);
        }

        if (Entry != nullptr)
        {
            Entry->bGenericValid = bValid;
            if (bValid)
            {
                Entry->GenericFloorZ = FloorZ;
                Entry->GenericSupportRadius = SupportRadius;
            }
            Entry->SetComputed(FPassageFloorProjectionCacheEntry::GenericComputed);
        }
        if (bValid)
        {
            OutFloorZ = FloorZ;
            OutSupportRadius = SupportRadius;
        }
        return bValid;
    }
}

static void VF_AuthorNativePassageFloor(
    FVoxelPassage& Passage,
    const UVoxelStrateDefinition* UpperDefinition,
    const UVoxelStrateDefinition* LowerDefinition,
    uint32 PassageSeed)
{
    Passage.bNativeFloorEnabled = false;
    Passage.NativeFloorProfileZ.Reset();
    Passage.NativeFloorReliefScales.Reset();
    Passage.NativeFloorReliefStrength = 0.0f;
    Passage.NativeFloorReliefFrequency = 0.015f;
    Passage.NativeFloorSeed = 0;

    if (!Passage.bWalkableTunnelContract
        || Passage.ControlPoints.Num() < 2
        || Passage.ControlRadii.Num() != Passage.ControlPoints.Num())
    {
        return;
    }

    // Inter-strate passages can connect to non-cave archetypes.  Keep the native D profile for
    // the walkable contract in all cases, but only borrow relief from a cave strate that actually
    // owns the same room-floor field.  A malformed/unknown parameter source fails closed to a
    // flat floor rather than inventing a second relief system.
    const FStrateGenerationParams* ReliefParams = nullptr;
    if (UpperDefinition != nullptr
        && (UpperDefinition->GeneratorType == ECaveGeneratorType::TunnelNetwork
            || UpperDefinition->GeneratorType == ECaveGeneratorType::Underwater))
    {
        ReliefParams = &UpperDefinition->GenerationParams;
    }
    else if (LowerDefinition != nullptr
        && (LowerDefinition->GeneratorType == ECaveGeneratorType::TunnelNetwork
            || LowerDefinition->GeneratorType == ECaveGeneratorType::Underwater))
    {
        ReliefParams = &LowerDefinition->GenerationParams;
    }
    if (ReliefParams != nullptr)
    {
        Passage.NativeFloorReliefStrength = FMath::Max(
            ReliefParams->FloorReliefStrength, 0.0f);
        Passage.NativeFloorReliefFrequency = ReliefParams->FloorReliefFrequency;
    }
    Passage.NativeFloorSeed = VoxelHash::Mix(PassageSeed ^ 0xD00DF10Eu);
    Passage.NativeFloorProfileZ.SetNum(Passage.ControlPoints.Num());
    for (int32 PointIndex = 0;
         PointIndex < Passage.ControlPoints.Num();
         ++PointIndex)
    {
        const float Radius = FMath::Abs(Passage.ControlRadii[PointIndex]);
        const float FloorZ = VoxelPassageGeometry::TunnelFloorZ(
            Passage.ControlPoints[PointIndex], Radius);
        if (!VoxelMath::IsFinite(FloorZ))
        {
            Passage.NativeFloorProfileZ.Reset();
            return;
        }
        Passage.NativeFloorProfileZ[PointIndex] = FloorZ;
    }

    const int32 NumSegments = Passage.ControlPoints.Num() - 1;
    Passage.NativeFloorReliefScales.SetNumZeroed(NumSegments);
    for (int32 SegmentIndex = 0;
         SegmentIndex < NumSegments;
         ++SegmentIndex)
    {
        const FVector& A = Passage.ControlPoints[SegmentIndex];
        const FVector& B = Passage.ControlPoints[SegmentIndex + 1];
        const float HorizontalRun = FVector2D(
            static_cast<float>(B.X - A.X),
            static_cast<float>(B.Y - A.Y)).Size();
        const float BaseGradient = HorizontalRun > KINDA_SMALL_NUMBER
            ? FMath::Abs(
                Passage.NativeFloorProfileZ[SegmentIndex + 1]
                    - Passage.NativeFloorProfileZ[SegmentIndex]) / HorizontalRun
            : FLT_MAX;
        Passage.NativeFloorReliefScales[SegmentIndex] =
            VF_BuildPassageReliefScale(
                A, B, BaseGradient, SegmentIndex, NumSegments,
                Passage.NativeFloorReliefStrength,
                Passage.NativeFloorReliefFrequency);
    }
    Passage.bNativeFloorEnabled =
        Passage.NativeFloorProfileZ.Num() == Passage.ControlPoints.Num();
}

static float VF_BoundarySealThicknessForDefinition(
    const UVoxelStrateDefinition& Definition)
{
    switch (Definition.GeneratorType)
    {
    case ECaveGeneratorType::TunnelNetwork:
    case ECaveGeneratorType::Underwater:
        return Definition.GenerationParams.BoundarySealThickness;
    case ECaveGeneratorType::FlatPlain:
    case ECaveGeneratorType::CrystalChamber:
        return Definition.SlabParams.BoundarySealThickness;
    case ECaveGeneratorType::Maze:
        return Definition.MazeParams.BoundarySealThickness;
    case ECaveGeneratorType::SurfaceWorld:
        return Definition.SurfaceParams.BoundarySealThickness;
    case ECaveGeneratorType::VerticalShafts:
        return Definition.VerticalShaftParams.BoundarySealThickness;
    case ECaveGeneratorType::FloatingIslands:
        return Definition.FloatingIslandParams.BoundarySealThickness;
    default:
        return 0.0f;
    }
}

/**
 * The default inter-strate tunnel is a carved tube plus an explicit support slab.  The tube's
 * rounded SDF guarantees clearance, but it cannot guarantee a standable surface over a source
 * field or a disturbance that was solid before the passage post.  Project the query into the
 * nearest horizontal control segment and return the conservative floor carried by that segment.
 * This is a fixed-size arithmetic loop over the already-built descriptor; it performs no source
 * search, allocation, or topology work.
 */
static bool VF_IsWalkableTunnelFloor(
    const FVoxelPassage& Passage,
    const FVector& Position,
    const UVoxelStrateManager* Manager = nullptr,
    int32 PassageIndex = INDEX_NONE)
{
    if (!Passage.bWalkableTunnelContract
        || Passage.ControlPoints.Num() < 2
        || Passage.ControlRadii.Num() != Passage.ControlPoints.Num())
    {
        return false;
    }

    float FloorZ = 0.0f;
    float SupportRadius = 0.0f;
    if (!VF_ProjectPassageFloor(
            Passage, Position, FloorZ, SupportRadius, Manager, PassageIndex))
    {
        return false;
    }

    return Position.Z <= FloorZ + KINDA_SMALL_NUMBER
        && Position.Z > FloorZ - VoxelPassageGeometry::LandingFloorThicknessVoxels;
}

static bool VF_IsWalkableTunnelAir(
    const FVoxelPassage& Passage,
    const FVector& Position,
    const UVoxelStrateManager* Manager = nullptr,
    int32 PassageIndex = INDEX_NONE,
    float* OutFloorZ = nullptr,
    float* OutSupportRadius = nullptr)
{
    if (!Passage.bWalkableTunnelContract
        || Passage.ControlPoints.Num() < 2
        || Passage.ControlRadii.Num() != Passage.ControlPoints.Num())
    {
        return false;
    }

    FPassageFloorProjectionCacheEntry* Entry =
        VF_GetPassageFloorProjectionCacheEntry(Manager, Position, PassageIndex);
    if (Entry != nullptr
        && Entry->Has(FPassageFloorProjectionCacheEntry::WalkableAirComputed))
    {
        if (Entry->bWalkableAir)
        {
            if (OutFloorZ != nullptr)
            {
                *OutFloorZ = Entry->GenericFloorZ;
            }
            if (OutSupportRadius != nullptr)
            {
                *OutSupportRadius = Entry->GenericSupportRadius;
            }
        }
        return Entry->bWalkableAir;
    }

    float FloorZ = 0.0f;
    float SupportRadius = 0.0f;
    if (!VF_ProjectPassageFloor(
            Passage, Position, FloorZ, SupportRadius, Manager, PassageIndex))
    {
        return false;
    }

    // The carved tube is wider than this guaranteed core.  The core is deliberately expressed as
    // a vertical prism over the projected floor: it gives the final-density player stencil a
    // stable air volume even when a source archetype or a disturbance would otherwise refill the
    // shallow part of the rounded SDF.  The floor writer below owns the closed lower face.
    const float AirHeight = 2.0f * SupportRadius;
    const bool bWalkableAir = Position.Z > FloorZ
        + VoxelPassageGeometry::WalkableTunnelFloorAirClearanceVoxels
        && Position.Z < FloorZ + AirHeight - KINDA_SMALL_NUMBER;
    if (Entry != nullptr)
    {
        Entry->bWalkableAir = bWalkableAir;
        Entry->SetComputed(FPassageFloorProjectionCacheEntry::WalkableAirComputed);
    }
    if (bWalkableAir)
    {
        if (OutFloorZ != nullptr)
        {
            *OutFloorZ = FloorZ;
        }
        if (OutSupportRadius != nullptr)
        {
            *OutSupportRadius = SupportRadius;
        }
    }
    return bWalkableAir;
}

static bool VF_FindWalkableTunnelAir(
    const UVoxelStrateManager* Manager,
    const FVector& Position,
    int32& OutPassageIndex,
    float& OutFloorZ,
    float& OutSupportRadius)
{
    OutPassageIndex = INDEX_NONE;
    OutFloorZ = 0.0f;
    OutSupportRadius = 0.0f;
    if (Manager == nullptr)
    {
        return false;
    }

    const FIntVector ChunkCoord(
        FMath::FloorToInt(Position.X / (float)CHUNK_SIZE),
        FMath::FloorToInt(Position.Y / (float)CHUNK_SIZE),
        FMath::FloorToInt(Position.Z / (float)CHUNK_SIZE));
    const TArray<int32>& Nearby = VF_GetNearbyPassages(Manager, ChunkCoord);
    const TArray<FVoxelPassage>& Passages = Manager->GetPassages();
    const FVoxelPassage* PassageData = Passages.GetData();
    for (const int32 PassageIndex : Nearby)
    {
        if (!Passages.IsValidIndex(PassageIndex))
        {
            continue;
        }
        const FVoxelPassage& Passage = PassageData[PassageIndex];
        if (FVector::DistSquared(Position, Passage.BoundCenter) > Passage.BoundRadiusSq
            || !VF_IsWalkableTunnelAir(
                Passage, Position, Manager, PassageIndex,
                &OutFloorZ, &OutSupportRadius))
        {
            continue;
        }
        OutPassageIndex = PassageIndex;
        return true;
    }
    return false;
}

static bool VF_IsAnyPassageFloorAt(
    const UVoxelStrateManager* Manager,
    const FVector& Position)
{
    if (Manager == nullptr)
    {
        return false;
    }
    const FIntVector ChunkCoord(
        FMath::FloorToInt(Position.X / (float)CHUNK_SIZE),
        FMath::FloorToInt(Position.Y / (float)CHUNK_SIZE),
        FMath::FloorToInt(Position.Z / (float)CHUNK_SIZE));
    const TArray<int32>& Nearby = VF_GetNearbyPassages(Manager, ChunkCoord);
    const TArray<FVoxelPassage>& Passages = Manager->GetPassages();
    const FVoxelPassage* PassageData = Passages.GetData();
    for (const int32 PassageIndex : Nearby)
    {
        if (!Passages.IsValidIndex(PassageIndex))
        {
            continue;
        }
        const FVoxelPassage& Passage = PassageData[PassageIndex];
        if (!VoxelPassageGeometry::VerticalShaftConnectorAirMarker()
            && (VF_IsPassageLandingFloor(Position, Passage.UpperLanding)
            || VF_IsPassageLandingFloor(Position, Passage.LowerLanding)
            || (Manager->ArePassageSupportFloorWritesEnabledForDiagnostics()
                && VF_IsWalkableTunnelFloor(Passage, Position, Manager, PassageIndex))))
        {
            return true;
        }
    }
    return false;
}

bool UVoxelStrateManager::Initialize(UVoxelSettings* Settings, int32 WorldSeed)
{
    VoxelForgeStartupTrace::FStageScope StartupTraceStage(TEXT("StrateManager.Initialize"));
    if (!Settings)
    {
        UE_LOG(LogTemp, Error, TEXT("[StrateManager] No settings provided!"));
        return false;
    }

    FVoxelSeasonManifest SeasonManifest;
    bool bUseSeason = false;
    if (!Settings->Season.IsNull())
    {
        UVoxelSeasonAsset* SeasonAsset = Settings->Season.LoadSynchronous();
        FString SeasonReport;
        if (SeasonAsset == nullptr
            || !SeasonAsset->LoadManifest(SeasonManifest, SeasonReport))
        {
            UE_LOG(LogTemp, Error,
                TEXT("[StrateManager] Season asset is assigned but unusable: %s"),
                SeasonAsset ? *SeasonReport : *Settings->Season.ToString());
            return false; // Fail closed: falling back to the authored pool would diverge peers.
        }
        bUseSeason = true;
        WorldSeed = SeasonManifest.Seed;
    }

    StrateLayout.Empty();
    SeasonStrates.Empty();
    ActiveSeasonContentHash.Reset();
#if WITH_EDITOR
    // A full layout rebuild discards any temporary walk-through candidate. The world calls this
    // under FScopedGenerationPause, so no worker can observe the map while it is being cleared.
    ComposerOverrides.Reset();
#endif

    const int32 TotalStrates = bUseSeason
        ? SeasonManifest.Strates.Num() : Settings->TotalStrates;
    const int32 LayoutGapChunks = bUseSeason
        ? SeasonManifest.InterStrateGapChunks
        : FMath::Max(0, Settings->InterStrateGapChunks);

    //=========================================================================
    // STEP 1: Build shuffled pool (seed-based randomization)
    //=========================================================================
    // Copy the pool and shuffle it deterministically using the world seed.
    // Fixed strates are excluded from the shuffle — they always use their
    // assigned definition regardless of seed.

    using FLoadedPoolEntry = TPair<FString, UVoxelStrateDefinition*>;
    TArray<FLoadedPoolEntry> ShuffledPool;
    if (!bUseSeason)
    {
        for (const TSoftObjectPtr<UVoxelStrateDefinition>& SoftPtr : Settings->StratePool)
        {
            // Load the asset (synchronous for now — could be async later)
            UVoxelStrateDefinition* Def = SoftPtr.LoadSynchronous();
            if (Def)
            {
                ShuffledPool.Emplace(SoftPtr.ToString(), Def);
            }
        }
    }

    // Sort by the soft asset path before shuffling. Pointer addresses depend on load order and
    // would make the layout machine-dependent. The pool is a set for layout purposes: editor
    // reordering must not change the input sequence seen by Fisher-Yates.
    // Trier par chemin de soft asset avant le shuffle. Les adresses de pointeurs dépendent de
    // l'ordre de chargement et rendraient le layout dépendant de la machine. Le pool est un set
    // pour le layout : réordonner l'asset dans l'éditeur ne doit pas changer l'entrée de Fisher-Yates.
    ShuffledPool.Sort([](const FLoadedPoolEntry& A, const FLoadedPoolEntry& B)
    {
        return FCString::Strcmp(*A.Key, *B.Key) < 0;
    });

    // Seed-based shuffle using Fisher-Yates
    // FRandomStream gives us deterministic random numbers from a seed
    FRandomStream Rng(WorldSeed);
    for (int32 i = ShuffledPool.Num() - 1; i > 0; i--)
    {
        int32 j = Rng.RandRange(0, i);
        ShuffledPool.Swap(i, j);
    }

    //=========================================================================
    // STEP 2: Assign definitions to each strate slot
    //=========================================================================
    // Walk through strate indices 0..TotalStrates-1.
    // Fixed strates use their pinned definition.
    // Random strates cycle through the shuffled pool.

    int32 PoolCursor = 0;  // Current position in the shuffled pool

    // Pre-load fixed strate definitions
    TMap<int32, UVoxelStrateDefinition*> LoadedFixed;
    if (!bUseSeason)
    {
        for (auto& Pair : Settings->FixedStrates)
        {
            UVoxelStrateDefinition* Def = Pair.Value.LoadSynchronous();
            if (Def)
            {
                LoadedFixed.Add(Pair.Key, Def);
            }
        }
    }

    // Current Z position (in chunks). Starts at 0 and goes downward (negative).
    int32 CurrentTopZ = 0;

    for (int32 i = 0; i < TotalStrates; i++)
    {
        FStrateSlot Slot;
        Slot.StrateIndex = i;

        if (bUseSeason)
        {
            const FVoxelSeasonStrate& SeasonStrate = SeasonManifest.Strates[i];
            if (!SeasonStrate.SourceDefinitionPath.IsEmpty())
            {
                TSoftObjectPtr<UVoxelStrateDefinition> SourceDefinition(
                    FSoftObjectPath(SeasonStrate.SourceDefinitionPath));
                UVoxelStrateDefinition* LoadedDefinition = SourceDefinition.LoadSynchronous();
                if (LoadedDefinition == nullptr)
                {
                    UE_LOG(LogTemp, Error,
                        TEXT("[StrateManager] Season slot %d could not load authored definition '%s'."),
                        i, *SeasonStrate.SourceDefinitionPath);
                    StrateLayout.Empty();
                    SeasonStrates.Empty();
                    return false;
                }
                // Keep the authored content/passage/visual bag without mutating the cooked asset,
                // then make the manifest's density identity authoritative below.
                Slot.Definition = DuplicateObject<UVoxelStrateDefinition>(LoadedDefinition, this);
            }
            else
            {
                // The current density manifest has no wider content record. Keep all consumers
                // supplied with a stable definition object, but do not guess an authored theme.
                Slot.Definition = NewObject<UVoxelStrateDefinition>(this, NAME_None, RF_Transient);
                Slot.Definition->StrateName = FText::FromString(
                    FString::Printf(TEXT("Season %d Strate %d"), SeasonManifest.Season, i));
                Slot.Definition->TransitionType = EVoxelStrateTransition::Hard;
                // Passage configuration is not part of schema v2. Inventing the UObject default
                // here would add an unreviewed tunnel to every generated boundary and make the
                // runtime field differ from the field that passed composition. The origin spine
                // remains the guaranteed connection until a later schema explicitly stores these.
                Slot.Definition->PassageConfig.Connections = 0;
            }

            Slot.Definition->GeneratorType = SeasonStrate.Archetype;
            Slot.Definition->bUseOperatorStack = SeasonStrate.bUsesRecipe;
            Slot.Definition->StrateHeightInChunks = SeasonStrate.HeightInChunks;
            Slot.Definition->GenerationParams = SeasonStrate.Params.TunnelNetworkParams;
            Slot.Definition->SlabParams = SeasonStrate.Params.SlabParams;
            Slot.Definition->MazeParams = SeasonStrate.Params.MazeParams;
            Slot.Definition->SurfaceParams = SeasonStrate.Params.SurfaceParams;
            Slot.Definition->VerticalShaftParams = SeasonStrate.Params.VerticalShaftParams;
            Slot.Definition->FloatingIslandParams = SeasonStrate.Params.FloatingIslandParams;

            Slot.HeightInChunks = SeasonStrate.HeightInChunks;
            Slot.TopChunkZ = SeasonStrate.TopWorldZ / CHUNK_SIZE - 1;
            Slot.BottomChunkZ = SeasonStrate.BottomWorldZ / CHUNK_SIZE;
            if (SeasonStrate.TopWorldZ % CHUNK_SIZE != 0
                || SeasonStrate.BottomWorldZ % CHUNK_SIZE != 0
                || Slot.TopChunkZ != CurrentTopZ
                || Slot.BottomChunkZ != CurrentTopZ - (Slot.HeightInChunks - 1))
            {
                UE_LOG(LogTemp, Error,
                    TEXT("[StrateManager] Season slot %d bounds do not describe the declared stacked layout."), i);
                StrateLayout.Empty();
                SeasonStrates.Empty();
                return false;
            }
            SeasonStrates.Add(SeasonStrate);
        }
        else
        {
            // Pick definition: fixed or from pool
            UVoxelStrateDefinition** FixedDef = LoadedFixed.Find(i);
            if (FixedDef && *FixedDef)
            {
                Slot.Definition = *FixedDef;
            }
            else if (ShuffledPool.Num() > 0)
            {
                // Cycle through the pool (wraps around if more strates than pool entries)
                Slot.Definition = ShuffledPool[PoolCursor % ShuffledPool.Num()].Value;
                PoolCursor++;
            }
            else
            {
                UE_LOG(LogTemp, Warning, TEXT("[StrateManager] No strate definitions available for slot %d!"), i);
                continue;
            }

            // Compute Z range from definition's height
            Slot.HeightInChunks = Slot.Definition->StrateHeightInChunks;
            Slot.TopChunkZ = CurrentTopZ;
            Slot.BottomChunkZ = CurrentTopZ - (Slot.HeightInChunks - 1);
        }

        // Move the cursor down for the next strate, leaving a solid-bedrock gap of
        // InterStrateGapChunks chunks between this strate and the next.
        CurrentTopZ = Slot.BottomChunkZ - 1 - LayoutGapChunks;

        StrateLayout.Add(Slot);

        if (VoxelForgeStartupTrace::IsActive())
        {
            FString DefinitionPath = Slot.Definition ? Slot.Definition->GetPathName() : FString();
            DefinitionPath.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
            DefinitionPath.ReplaceInline(TEXT("\""), TEXT("\\\""));
            const UEnum* ArchetypeEnum = StaticEnum<ECaveGeneratorType>();
            const FString ArchetypeName = ArchetypeEnum
                ? ArchetypeEnum->GetNameStringByValue(static_cast<int64>(Slot.Definition->GeneratorType))
                : FString::Printf(TEXT("Value_%d"), static_cast<int32>(Slot.Definition->GeneratorType));
            VoxelForgeStartupTrace::RecordEvent(TEXT("strate_slot"), FString::Printf(
                TEXT("\"index\":%d,\"definition\":\"%s\",\"archetype\":\"%s\",\"top_chunk_z\":%d,"
                     "\"bottom_chunk_z\":%d,\"height_chunks\":%d,\"operator_stack\":%d"),
                Slot.StrateIndex, *DefinitionPath, *ArchetypeName,
                Slot.TopChunkZ, Slot.BottomChunkZ, Slot.HeightInChunks,
                Slot.Definition->bUseOperatorStack ? 1 : 0));
        }

        UE_LOG(LogTemp, Log, TEXT("[StrateManager] Strate %d: '%s' | Z chunks [%d to %d] | %d chunks tall"),
            i,
            *Slot.Definition->StrateName.ToString(),
            Slot.TopChunkZ,
            Slot.BottomChunkZ,
            Slot.HeightInChunks);
    }

    // Diagnostic de configuration, une seule fois par construction de layout. SurfaceWorld est
    // volontairement exclu : son chemin T1.d exact-lattice ne dépend pas de ce drapeau.
    // Configuration diagnostic once per layout build. SurfaceWorld is deliberately excluded:
    // its exact-lattice T1.d path does not depend on this flag.
    int32 NumCaveSlots = 0;
    int32 NumOperatorStackDisabledCaves = 0;
    for (const FStrateSlot& Slot : StrateLayout)
    {
        const ECaveGeneratorType SlotArchetype = bUseSeason
            ? SeasonStrates[Slot.StrateIndex].Archetype : Slot.Definition->GeneratorType;
        if (!Slot.Definition || SlotArchetype == ECaveGeneratorType::SurfaceWorld)
        {
            continue;
        }

        ++NumCaveSlots;
        const bool bUsesStack = bUseSeason
            ? SeasonStrates[Slot.StrateIndex].bUsesRecipe : Slot.Definition->bUseOperatorStack;
        if (!bUsesStack)
        {
            ++NumOperatorStackDisabledCaves;
        }
    }

    // ⚠️ WARNING EN ÉDITEUR/JEU, JAMAIS EN TEST. Les tests `Determinism.*` construisent
    // DÉLIBÉRÉMENT un monde non opt-in — c'est leur oracle de comparaison — et le framework
    // d'automatisation compte un Warning comme un échec. Un diagnostic ne doit pas casser la suite
    // qu'il est censé éclairer. Le message reste écrit UNE fois : seule la verbosité change.
    // Warning in editor/game where it is actionable, never in tests: the Determinism.* tests build
    // a non-opted-in world ON PURPOSE as their comparison oracle, and the automation framework
    // treats a Warning as a failure. One message, two verbosities.
    const bool bQuietDiagnostic = GIsAutomationTesting;

    if (NumOperatorStackDisabledCaves > 0)
    {
        const FString Summary = FString::Printf(
            TEXT("[StrateManager] Operator-stack opt-in: %d/%d cave layout slots have Use Operator Stack disabled. These slots cannot use operator-stack ClassifyBox/T1.d; enable the asset setting on the listed definitions if that is intended."),
            NumOperatorStackDisabledCaves, NumCaveSlots);

        if (bQuietDiagnostic) { UE_LOG(LogTemp, Verbose, TEXT("%s"), *Summary); }
        else                  { UE_LOG(LogTemp, Warning, TEXT("%s"), *Summary); }
    }
    else
    {
        UE_LOG(LogTemp, Log,
            TEXT("[StrateManager] Operator-stack opt-in: all %d cave layout slots have Use Operator Stack enabled."),
            NumCaveSlots);
    }

    for (const FStrateSlot& Slot : StrateLayout)
    {
        const bool bSeasonStack = bUseSeason
            && SeasonStrates[Slot.StrateIndex].bUsesRecipe;
        const ECaveGeneratorType SlotArchetype = bUseSeason
            ? SeasonStrates[Slot.StrateIndex].Archetype : Slot.Definition->GeneratorType;
        if (!Slot.Definition
            || SlotArchetype == ECaveGeneratorType::SurfaceWorld
            || (bUseSeason ? bSeasonStack : Slot.Definition->bUseOperatorStack))
        {
            continue;
        }

        const FString Line = FString::Printf(
            TEXT("[StrateManager]   cave slot=%d name='%s' Z chunks=[%d to %d] bUseOperatorStack=false"),
            Slot.StrateIndex,
            *Slot.Definition->StrateName.ToString(),
            Slot.TopChunkZ,
            Slot.BottomChunkZ);

        if (bQuietDiagnostic) { UE_LOG(LogTemp, Verbose, TEXT("%s"), *Line); }
        else                  { UE_LOG(LogTemp, Warning, TEXT("%s"), *Line); }
    }

    CachedSeed = WorldSeed;
    bOpenSurfaceEntry = Settings->bOpenSurfaceEntry;
    OriginSpineRadius = bUseSeason
        ? SeasonManifest.OriginSpineRadius : Settings->OriginSpineRadius;
    InterStrateGapChunks = LayoutGapChunks;
    if (bUseSeason)
    {
        ActiveSeasonContentHash = SeasonManifest.ContentHash;
    }
    // Passage shape/count is per-strate now (UVoxelStrateDefinition::PassageConfig).

    //=========================================================================
    // STEP 3: Load terrain operation assets
    //=========================================================================
    // Each strate definition references terrain ops as soft pointers.
    // We load them synchronously here so they're available during generation.
    // Without this, BuildParamsFromDefinition's Entry.Operation.Get() would
    // return null if the assets haven't been loaded yet.
    for (const FStrateSlot& Slot : StrateLayout)
    {
        if (!Slot.Definition) continue;
        if (bUseSeason && SeasonStrates[Slot.StrateIndex].bUsesRecipe) continue;

        for (const FStrateTerrainOpEntry& Entry : Slot.Definition->TerrainOperations)
        {
            if (!Entry.Operation.IsNull())
            {
                Entry.Operation.LoadSynchronous();
            }
        }
    }

    UE_LOG(LogTemp, Log, TEXT("[StrateManager] Initialized %d strates (seed=%d)"),
        StrateLayout.Num(), WorldSeed);

    VoxelForgeStartupTrace::RecordEvent(TEXT("strate_layout_ready"), FString::Printf(
        TEXT("\"strates\":%d,\"seed\":%d,\"inter_strate_gap_chunks\":%d,\"season\":%d"),
        StrateLayout.Num(), WorldSeed, InterStrateGapChunks, bUseSeason ? 1 : 0));

    // Generate passages between consecutive strates
    GeneratePassages();
    return bUseSeason ? IsUsingSeason() : true;
}

#if WITH_EDITOR
const FVoxelStrateComposerSlotOverride* UVoxelStrateManager::FindComposerOverride(
    int32 StrateIndex) const
{
    const TSharedPtr<FVoxelStrateComposerSlotOverride>* Found = ComposerOverrides.Find(StrateIndex);
    return Found != nullptr ? Found->Get() : nullptr;
}

bool UVoxelStrateManager::SetComposerOverrideForStrate(
    int32 StrateIndex, int32 CandidateSeed, ECaveGeneratorType Archetype,
    const FVoxelStrateArchetypeParams& Params, bool bUseRecipe,
    const FVoxelOpStackRecipe* Recipe, FString& OutError,
    const FVoxelStrateRegionManifest* InRegions)
{
    OutError.Reset();

    const FStrateSlot* TargetSlot = nullptr;
    for (const FStrateSlot& Slot : StrateLayout)
    {
        if (Slot.StrateIndex == StrateIndex)
        {
            TargetSlot = &Slot;
            break;
        }
    }
    if (TargetSlot == nullptr || TargetSlot->Definition == nullptr)
    {
        OutError = FString::Printf(TEXT("Strate index %d is not present in the live layout."), StrateIndex);
        return false;
    }

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
        break;
    default:
        OutError = TEXT("The composer returned an unsupported strate archetype.");
        return false;
    }

    if (bUseRecipe && Recipe == nullptr)
    {
        OutError = TEXT("A structure candidate did not provide a recipe.");
        return false;
    }

    TSharedPtr<FVoxelStrateComposerSlotOverride> Override =
        MakeShared<FVoxelStrateComposerSlotOverride>();
    Override->CandidateSeed = CandidateSeed;
    Override->Archetype = Archetype;
    Override->Params = Params;
    if ((VF_WormsForceOff() || !TargetSlot->Definition->bEnableWorms)
        && (Archetype == ECaveGeneratorType::TunnelNetwork
            || Archetype == ECaveGeneratorType::Underwater))
    {
        // The composer is allowed to roll a complete candidate, but a disabled asset must never
        // retain a worm-enabled candidate in its runtime override, even for code that inspects
        // the override before GetGenerationParams applies the final resolution gate.
        Override->Params.TunnelNetworkParams.WormStrength = 0.0f;
    }
    VF_SetComposerRuntimeBounds(
        Override->Params,
        (float)(TargetSlot->TopChunkZ + 1) * CHUNK_SIZE,
        (float)TargetSlot->BottomChunkZ * CHUNK_SIZE);
    Override->bUseRecipe = bUseRecipe;
    if (Recipe != nullptr)
    {
        Override->Recipe = *Recipe;
    }

    if (InRegions != nullptr && InRegions->RegionCount > 1)
    {
        if (!InRegions->IsValid())
        {
            OutError = InRegions->FailureReason.IsEmpty()
                ? TEXT("The composer returned an invalid lateral region manifest.")
                : InRegions->FailureReason;
            return false;
        }
        Override->bUseRegions = true;
        Override->Regions = *InRegions;
        VF_RekeyStrateRegionManifest(Override->Regions, CandidateSeed, StrateIndex);
        Override->Regions.StrateTopWorldZ = (float)(TargetSlot->TopChunkZ + 1) * CHUNK_SIZE;
        Override->Regions.StrateBottomWorldZ = (float)TargetSlot->BottomChunkZ * CHUNK_SIZE;
        VF_SetStrateArchetypeRuntimeBounds(Override->Params,
                                           Override->Regions.StrateTopWorldZ,
                                           Override->Regions.StrateBottomWorldZ);
        for (FVoxelStrateRegion& Region : Override->Regions.Regions)
        {
            VF_SetStrateArchetypeRuntimeBounds(Region.ArchetypeParams,
                                               Override->Regions.StrateTopWorldZ,
                                               Override->Regions.StrateBottomWorldZ);
        }
        Override->Regions.bHasGlobalStructuralParams = true;
    }

    // Only one slot is overridden at a time. Keeping this map small also makes the worker-side
    // copy-on-chunk-refetch cheap. Passage geometry is deliberately not regenerated: the layout
    // and its passages are unchanged, and this is the same manager state used by the offline
    // candidate measurement harness.
    ComposerOverrides.Reset();
    ComposerOverrides.Add(StrateIndex, MoveTemp(Override));

    // This is both the passage-shortlist invalidation counter and the cache key used by the
    // generator's per-chunk params/stack memos. The world has already paused all readers.
    ++PassagesVersion;
    return true;
}

bool UVoxelStrateManager::GetComposerOverrideForChunk(
    const FIntVector& ChunkCoord, int32& OutCandidateSeed,
    ECaveGeneratorType& OutArchetype, FVoxelStrateArchetypeParams& OutParams,
    bool& bOutUseRecipe, FVoxelOpStackRecipe& OutRecipe) const
{
    const int32 SlotIdx = FindSlotIndexForChunkZ(ChunkCoord.Z);
    if (SlotIdx < 0)
    {
        return false;
    }

    const FVoxelStrateComposerSlotOverride* Override =
        FindComposerOverride(StrateLayout[SlotIdx].StrateIndex);
    if (Override == nullptr)
    {
        return false;
    }

    OutCandidateSeed = Override->CandidateSeed;
    OutArchetype = Override->Archetype;
    OutParams = Override->Params;
    bOutUseRecipe = Override->bUseRecipe;
    OutRecipe = Override->Recipe;
    return true;
}

bool UVoxelStrateManager::GetComposerRegionOverrideForChunk(
    const FIntVector& ChunkCoord, FVoxelStrateRegionManifest& OutRegions) const
{
    const int32 SlotIdx = FindSlotIndexForChunkZ(ChunkCoord.Z);
    if (SlotIdx < 0)
    {
        return false;
    }

    const FVoxelStrateComposerSlotOverride* Override =
        FindComposerOverride(StrateLayout[SlotIdx].StrateIndex);
    if (Override == nullptr || !Override->bUseRegions)
    {
        return false;
    }
    OutRegions = Override->Regions;
    return true;
}

#endif

bool UVoxelStrateManager::GetRecipeForChunk(
    const FIntVector& ChunkCoord, int32& OutRecipeSeed,
    ECaveGeneratorType& OutArchetype, FVoxelStrateArchetypeParams& OutParams,
    FVoxelOpStackRecipe& OutRecipe) const
{
    const int32 SlotIdx = FindSlotIndexForChunkZ(ChunkCoord.Z);
    if (SlotIdx < 0) return false;

#if WITH_EDITOR
    if (const FVoxelStrateComposerSlotOverride* Override =
            FindComposerOverride(StrateLayout[SlotIdx].StrateIndex))
    {
        if (!Override->bUseRecipe || Override->bUseRegions) return false;
        OutRecipeSeed = Override->CandidateSeed;
        OutArchetype = Override->Archetype;
        OutParams = Override->Params;
        OutRecipe = Override->Recipe;
        return true;
    }
#endif
    if (SeasonStrates.IsValidIndex(SlotIdx))
    {
        const FVoxelSeasonStrate& Strate = SeasonStrates[SlotIdx];
        if (!Strate.bUsesRecipe || Strate.bUsesRegions) return false;
        OutRecipeSeed = Strate.Seed;
        OutArchetype = Strate.Archetype;
        OutParams = Strate.Params;
        OutRecipe = Strate.Recipe;
        return true;
    }
    return false;
}

//=============================================================================
// PASSAGE GENERATION
//=============================================================================

void UVoxelStrateManager::GeneratePassages()
{
    VoxelForgeStartupTrace::FStageScope StartupTraceStage(TEXT("GeneratePassages"));
    Passages.Empty();

    if (StrateLayout.Num() < 1)
    {
        VoxelForgeStartupTrace::RecordEvent(TEXT("passages_ready"), TEXT("\"passages\":0"));
        return;
    }

    // Deterministic per-value hashes from the world seed. Every draw is keyed by its boundary
    // strate index, connection index, and a unique salt, so one passage cannot shift another.
    // Hachages déterministes par valeur depuis le seed du monde. Chaque tirage est indexé par la
    // strate de frontière, la connexion et un sel unique : un passage ne peut plus décaler l'autre.
    const uint32 PassageSeed = static_cast<uint32>(CachedSeed) ^ 0x50A55A6Eu;  // "PASSAGE"
    constexpr uint32 PassageSaltAngle      = 0xA1100001u;
    constexpr uint32 PassageSaltDistance   = 0xA1100002u;
    constexpr uint32 PassageSaltUpperReach = 0xA1100003u;
    constexpr uint32 PassageSaltLowerReach = 0xA1100004u;
    constexpr uint32 PassageSaltWormFreq   = 0xA1100005u;
    constexpr uint32 PassageSaltNoiseX     = 0xA1100006u;
    constexpr uint32 PassageSaltNoiseY     = 0xA1100007u;
    constexpr uint32 PassageSaltNoiseZ     = 0xA1100008u;
    constexpr uint32 PassageSaltPhase      = 0xA1100009u;
    constexpr uint32 PassageSaltBendFreq   = 0xA110000Au;
    constexpr uint32 PassageSaltUpperDoor  = 0xA110000Bu;
    constexpr uint32 PassageSaltLowerDoor  = 0xA110000Cu;

    int32 TotalPassages = 0;
    int32 NumAimedAtUpperPlayerFit = 0;
    int32 NumAimedAtLowerPlayerFit = 0;
    TSet<FString> NoQueryArchetypes;

    const auto ArchetypeName = [](ECaveGeneratorType Archetype)
    {
        if (const UEnum* ArchetypeEnum = StaticEnum<ECaveGeneratorType>())
        {
            return ArchetypeEnum->GetNameStringByValue(static_cast<int64>(Archetype));
        }
        return FString::Printf(TEXT("Value_%d"), static_cast<int32>(Archetype));
    };

    const auto MaxLateralSnapFor = [](const UVoxelStrateDefinition& Definition) -> float
    {
        switch (Definition.GeneratorType)
        {
        case ECaveGeneratorType::Maze:
            // One maze lattice cell is the largest deliberate correction. It keeps a shortcut
            // within the same local maze neighborhood and protects its configured spine-distance
            // distribution from silently becoming a long-range teleport.
            return FMath::Max(Definition.MazeParams.CellSize, 1.0f);

        case ECaveGeneratorType::VerticalShafts:
            // A density of 0.6 does not guarantee an occupied cell in the immediate grid
            // neighbourhood. Allow the nearest occupied shaft to be two grid cells away while
            // remaining a bounded local query; the landing query still returns a pose inside
            // that shaft's deterministic feature core, so this cannot create a non-topological
            // mouth.
            return FMath::Max(2.0f * Definition.VerticalShaftParams.ShaftSpacing, 1.0f);

        case ECaveGeneratorType::FloatingIslands:
            // One island grid spacing protects the intentional radial placement while still
            // allowing a passage to reach a neighboring blob rather than an arbitrary far island.
            return FMath::Max(Definition.FloatingIslandParams.IslandSpacing, 1.0f);

        case ECaveGeneratorType::TunnelNetwork:
        case ECaveGeneratorType::Underwater:
            // Une salle est une CIBLE ÉPARSE en XY, exactement comme un couloir de labyrinthe : le
            // budget doit donc être réel. Zéro ici était un bug — la requête trouvait la salle la
            // plus proche puis atterrissait à côté d'elle dans 92,8 % des cas (balayage d'un million
            // de seeds). Un espacement de salles est le voisinage local naturel.
            //
            // A room is an XY-SPARSE target, exactly like a maze corridor, so the budget must be
            // real. Zero here was the bug: the query found the nearest room and then landed beside
            // it 92.8% of the time (million-seed sweep). One room spacing is the natural local
            // neighbourhood, and it protects the configured spine-distance distribution.
            return FMath::Max(Definition.GenerationParams.RoomSpacing, 1.0f);

        case ECaveGeneratorType::FlatPlain:
        case ECaveGeneratorType::CrystalChamber:
        default:
            // Les slabs sont une bande de vide CONTINUE en XY : le XY demandé est déjà à
            // l'intérieur, donc aucun déplacement latéral n'est nécessaire. Zéro est correct ici.
            // A slab's void band is XY-CONTINUOUS, so the requested XY is already inside it and no
            // lateral movement is needed. Zero is correct here, unlike for rooms.
            return 0.0f;
        }
    };

    const auto BoundarySealThicknessFor = [](const UVoxelStrateDefinition& Definition) -> float
    {
        switch (Definition.GeneratorType)
        {
        case ECaveGeneratorType::TunnelNetwork:
        case ECaveGeneratorType::Underwater:
            return Definition.GenerationParams.BoundarySealThickness;

        case ECaveGeneratorType::FlatPlain:
        case ECaveGeneratorType::CrystalChamber:
            return Definition.SlabParams.BoundarySealThickness;

        case ECaveGeneratorType::Maze:
            return Definition.MazeParams.BoundarySealThickness;

        case ECaveGeneratorType::SurfaceWorld:
            return Definition.SurfaceParams.BoundarySealThickness;

        case ECaveGeneratorType::VerticalShafts:
            return Definition.VerticalShaftParams.BoundarySealThickness;

        case ECaveGeneratorType::FloatingIslands:
            return Definition.FloatingIslandParams.BoundarySealThickness;

        default:
            return 0.0f;
        }
    };

    //=========================================================================
    // INTER-STRATE PASSAGES: tunnels connecting consecutive strates.
    // Each passage is randomly assigned one of 5 types, which determines
    // its shape, radius, and control point layout.
    //=========================================================================
    for (int32 i = 0; i < StrateLayout.Num() - 1; i++)
    {
        const FStrateSlot& Upper = StrateLayout[i];
        const FStrateSlot& Lower = StrateLayout[i + 1];

        // This (upper) strate's PassageConfig controls the progression tunnel to the layer below.
        // Each strate already owns a finite (0,0) landing room; this passage is the connection
        // that opens the next room rather than a second vertical spine.
        const UVoxelStrateDefinition* UpperDef = Upper.Definition;
        if (!UpperDef) continue;
        const FStratePassageConfig& Cfg = UpperDef->PassageConfig;

        // Upper strate floor and lower strate ceiling (differ when there's a bedrock gap).
        const float UpperTopZ    = (float)(Upper.TopChunkZ + 1) * CHUNK_SIZE;
        const float UpperBottomZ = (float)(Upper.BottomChunkZ) * CHUNK_SIZE;
        const float LowerTopZ    = (float)(Lower.TopChunkZ + 1) * CHUNK_SIZE;
        const float UpperMax = (float)Upper.HeightInChunks * CHUNK_SIZE * 0.9f;
        const float LowerMax = (float)Lower.HeightInChunks * CHUNK_SIZE * 0.9f;

        const float DistLo = FMath::Min(Cfg.DistanceMin, Cfg.DistanceMax);
        const float DistHi = FMath::Max(Cfg.DistanceMin, Cfg.DistanceMax);
        // The origin room owns the future straight shaft. Keep the progression mouth outside its
        // carve/blend envelope, then choose a deterministic point in the authored annulus. If an
        // asset has an invalid inner radius, repair only that radius; the outer authored radius is
        // preserved unless it is too small to contain the required inner edge.
        const float OriginLandingHalfWidth =
            VoxelPassageGeometry::LandingHalfWidthForRadius(OriginSpineRadius);
        const float MinimumDescentDistance = FMath::Max(
            OriginLandingHalfWidth + VoxelPassageGeometry::LandingCarveBlendVoxels,
            VoxelPassageGeometry::MinimumTurnFloorWidthVoxels);
        const float PlacementLo = FMath::Max(DistLo, MinimumDescentDistance);
        const float PlacementHi = FMath::Max(DistHi, PlacementLo);

        const int32 Conns = FMath::Max(0, Cfg.Connections);
        for (int32 c = 0; c < Conns; c++)
        {
            const auto PassageRandom01 = [PassageSeed, i, c](uint32 Salt)
            {
                return VoxelHash::ToFloat01(VoxelHash::Cell(i, c, PassageSeed ^ Salt));
            };
            const auto PassageRandomRange = [&PassageRandom01](float Min, float Max, uint32 Salt)
            {
                return FMath::Lerp(Min, Max, PassageRandom01(Salt));
            };

            FVoxelPassage Passage;
            Passage.UpperStrateIndex = i;
            Passage.LowerStrateIndex = i + 1;
            // The authored style is also reflected in the legacy passage type.  In particular,
            // only Straight receives the walkable-tunnel contract; a future style roll cannot
            // silently inherit the default's player-fit promise.
            switch (Cfg.Style)
            {
            case EVoxelPassageStyle::Straight:
                Passage.PassageType = EVoxelPassageType::SlopedTunnel;
                break;
            case EVoxelPassageStyle::Spiral:
                Passage.PassageType = EVoxelPassageType::SpiralDescent;
                break;
            case EVoxelPassageStyle::Cascading:
                Passage.PassageType = EVoxelPassageType::CascadingDrops;
                break;
            case EVoxelPassageStyle::Worm:
            default:
                Passage.PassageType = EVoxelPassageType::CrackCrevice;
                break;
            }

            // PLACEMENT: random angle, distance from the (0,0) spine within a deterministic
            // annulus. It is deliberately not the spine: that room is reserved for the future
            // surface shaft, while this passage opens a separate room in the strate network.
            const float Angle = PassageRandom01(PassageSaltAngle) * (2.0f * PI);
            const float Distance = PassageRandomRange(PlacementLo, PlacementHi, PassageSaltDistance);
            const float PX = FMath::Cos(Angle) * Distance;
            const float PY = FMath::Sin(Angle) * Distance;

            ++TotalPassages;

            // Ask each mouth's own strate for a source-level landing point. These queries are pure
            // and safe during Initialize: they do not construct an operator stack or call back
            // into the manager. Cave room graphs use the queried strate's seed from
            // MakeStrateSeed; slab, maze, shaft, and island fields use the world seed consumed by
            // their source. The two queries are deliberately independent: a passage never reads
            // another passage's endpoint or any live layout state.
            FVector SuggestedUpperPoint = FVector::ZeroVector;
            bool bAimedUpperAtPlayerFitPoint = false;
            if (UpperDef)
            {
                const bool bUsesRoomSeed =
                    UpperDef->GeneratorType == ECaveGeneratorType::TunnelNetwork
                    || UpperDef->GeneratorType == ECaveGeneratorType::Underwater;
                const int32 UpperQuerySeed = bUsesRoomSeed
                    ? static_cast<int32>(VoxelCaveMorphology::MakeStrateSeed(
                        static_cast<uint32>(CachedSeed), Upper.StrateIndex))
                    : CachedSeed;

                bAimedUpperAtPlayerFitPoint = VF_SuggestLandingPoint(
                    UpperDef->GeneratorType,
                    UpperDef->GenerationParams,
                    UpperDef->SlabParams,
                    UpperDef->MazeParams,
                    UpperDef->VerticalShaftParams,
                    UpperDef->FloatingIslandParams,
                    UpperQuerySeed,
                    UpperTopZ,
                    UpperBottomZ,
                    PX,
                    PY,
                    MaxLateralSnapFor(*UpperDef),
                    SuggestedUpperPoint,
                    CachedSeed);

                if (bAimedUpperAtPlayerFitPoint)
                {
                    ++NumAimedAtUpperPlayerFit;
                }
                else
                {
                    NoQueryArchetypes.Add(ArchetypeName(UpperDef->GeneratorType));
                }
            }

            FVector SuggestedLowerPoint = FVector::ZeroVector;
            bool bAimedLowerAtPlayerFitPoint = false;
            const UVoxelStrateDefinition* LowerDef = Lower.Definition;
            if (LowerDef)
            {
                const bool bUsesRoomSeed =
                    LowerDef->GeneratorType == ECaveGeneratorType::TunnelNetwork
                    || LowerDef->GeneratorType == ECaveGeneratorType::Underwater;
                const int32 LowerQuerySeed = bUsesRoomSeed
                    ? static_cast<int32>(VoxelCaveMorphology::MakeStrateSeed(
                        static_cast<uint32>(CachedSeed), Lower.StrateIndex))
                    : CachedSeed;

                const float MaxLateralSnap = MaxLateralSnapFor(*LowerDef);
                bAimedLowerAtPlayerFitPoint = VF_SuggestLandingPoint(
                    LowerDef->GeneratorType,
                    LowerDef->GenerationParams,
                    LowerDef->SlabParams,
                    LowerDef->MazeParams,
                    LowerDef->VerticalShaftParams,
                    LowerDef->FloatingIslandParams,
                    LowerQuerySeed,
                    LowerTopZ,
                    (float)(Lower.BottomChunkZ) * CHUNK_SIZE,
                    PX,
                    PY,
                    MaxLateralSnap,
                    SuggestedLowerPoint,
                    CachedSeed);

                if (bAimedLowerAtPlayerFitPoint)
                {
                    ++NumAimedAtLowerPlayerFit;
                }
                else
                {
                    NoQueryArchetypes.Add(ArchetypeName(LowerDef->GeneratorType));
                }
            }

            // LENGTH: reach into each strate, capped to the interior.
            const float UpperReach = FMath::Min(
                PassageRandomRange(Cfg.ReachMin, Cfg.ReachMax, PassageSaltUpperReach), UpperMax);
            const float LowerReach = FMath::Min(
                PassageRandomRange(Cfg.ReachMin, Cfg.ReachMax, PassageSaltLowerReach), LowerMax);
            float TopZ = UpperBottomZ + UpperReach;
            float BottomZ = LowerTopZ - LowerReach;
            float UpperX = PX;
            float UpperY = PY;
            float LowerX = PX;
            float LowerY = PY;
            Passage.RequestedUpperPoint = FVector(PX, PY, TopZ);
            Passage.RequestedLowerPoint = FVector(PX, PY, BottomZ);

            if (bAimedUpperAtPlayerFitPoint)
            {
                const float UpperSealThickness = BoundarySealThicknessFor(*UpperDef);
                const float InnerBottomZ = UpperBottomZ + UpperSealThickness;
                const float InnerTopZ = UpperTopZ - UpperSealThickness;
                if (VoxelMath::IsFinite(SuggestedUpperPoint.X)
                    && VoxelMath::IsFinite(SuggestedUpperPoint.Y)
                    && VoxelMath::IsFinite(SuggestedUpperPoint.Z)
                    && VoxelMath::IsFinite(InnerBottomZ)
                    && VoxelMath::IsFinite(InnerTopZ)
                    && InnerBottomZ < InnerTopZ)
                {
                    // VF_SuggestLandingPoint already guarantees a strict interior answer. Keep a
                    // tiny margin in the final clamp so a future source query cannot land on a
                    // seal boundary through rounding.
                    UpperX = SuggestedUpperPoint.X;
                    UpperY = SuggestedUpperPoint.Y;
                    const float StrictMargin = FMath::Min(
                        KINDA_SMALL_NUMBER, (InnerTopZ - InnerBottomZ) * 0.25f);
                    TopZ = FMath::Clamp(
                        SuggestedUpperPoint.Z,
                        InnerBottomZ + StrictMargin,
                        InnerTopZ - StrictMargin);
                }
            }

            if (bAimedLowerAtPlayerFitPoint)
            {
                const float LowerSealThickness = BoundarySealThicknessFor(*LowerDef);
                const float LowerBottomZ = (float)(Lower.BottomChunkZ) * CHUNK_SIZE;
                const float InnerBottomZ = LowerBottomZ + LowerSealThickness;
                const float InnerTopZ = LowerTopZ - LowerSealThickness;
                if (VoxelMath::IsFinite(SuggestedLowerPoint.X)
                    && VoxelMath::IsFinite(SuggestedLowerPoint.Y)
                    && VoxelMath::IsFinite(SuggestedLowerPoint.Z)
                    && VoxelMath::IsFinite(InnerBottomZ)
                    && VoxelMath::IsFinite(InnerTopZ)
                    && InnerBottomZ < InnerTopZ)
                {
                    // VF_SuggestLandingPoint already guarantees a strict interior answer. Keep a
                    // tiny margin in the final clamp so a future source query cannot land on a
                    // seal boundary through rounding.
                    LowerX = SuggestedLowerPoint.X;
                    LowerY = SuggestedLowerPoint.Y;
                    const float StrictMargin = FMath::Min(
                        KINDA_SMALL_NUMBER, (InnerTopZ - InnerBottomZ) * 0.25f);
                    BottomZ = FMath::Clamp(
                        SuggestedLowerPoint.Z,
                        InnerBottomZ + StrictMargin,
                        InnerTopZ - StrictMargin);
                }
            }

            const int32 Segments = FMath::Clamp(Cfg.Segments, 1, 48);
            Passage.ControlPoints.Reset();
            Passage.ControlRadii.Reset();
            Passage.ControlPoints.Reserve(Segments + 1);
            Passage.ControlRadii.Reserve(Segments + 1);

            // WIDTH profile: mouth radius at the ends, mid radius in the centre (taper/bulge).
            auto RadiusAt = [&](float t) { return FMath::Lerp(Cfg.MouthRadius, Cfg.MidRadius, FMath::Sin(t * PI)); };

            // Per-passage shape seeds.
            const float WormFreq = PassageRandomRange(0.8f, 1.8f, PassageSaltWormFreq);   // (vertical wobble only)
            const float NSeedX = PassageRandomRange(0.0f, 500.0f, PassageSaltNoiseX);
            const float NSeedY = PassageRandomRange(0.0f, 500.0f, PassageSaltNoiseY);
            const float NSeedZ = PassageRandomRange(0.0f, 500.0f, PassageSaltNoiseZ);
            const float PhaseA = PassageRandom01(PassageSaltPhase) * (2.0f * PI);
            // Base fBM frequency for the worm's wander (octaves add finer detail on top).
            const float BendFreq = PassageRandomRange(1.5f, 2.5f, PassageSaltBendFreq);

            for (int32 s = 0; s <= Segments; s++)
            {
                const float T = (float)s / (float)Segments;
                float Z = FMath::Lerp(TopZ, BottomZ, T);
                const float Env = FMath::Sin(T * PI);  // 0 at both ends → mouths stay anchored
                float OX = 0.0f, OY = 0.0f;

                switch (Cfg.Style)
                {
                case EVoxelPassageStyle::Straight:
                    // No authored lateral offset. A snapped lower mouth can still make this
                    // segment slanted, so the endpoint interpolation below remains intentional.
                    break;

                case EVoxelPassageStyle::Spiral:
                {
                    const float Ang = PhaseA + T * Cfg.SpiralTurns * 2.0f * PI;
                    OX = FMath::Cos(Ang) * Cfg.SpiralRadius * Env;
                    OY = FMath::Sin(Ang) * Cfg.SpiralRadius * Env;
                    break;
                }

                case EVoxelPassageStyle::Cascading:
                {
                    // Switchback staircase: each tread offsets in a new deterministic direction.
                    const int32 Steps = FMath::Clamp(Cfg.CascadeSteps, 1, 16);
                    const int32 Idx = FMath::Min((int32)(T * Steps), Steps - 1);
                    const float SA = PhaseA + (float)Idx * 2.39996f;  // golden-angle spread
                    OX = FMath::Cos(SA) * Cfg.CascadeLedge * Env;
                    OY = FMath::Sin(SA) * Cfg.CascadeLedge * Env;
                    break;
                }

                case EVoxelPassageStyle::Worm:
                default:
                {
                    // SQUIRM: displace the descent independently on X and Y with multi-octave
                    // fBM (different seeds → uncorrelated). Independent fBM per axis is a true
                    // 2D organic wander — it curls and meanders "here and there" rather than
                    // oscillating along one line (zig-zag) or orbiting the axis (spiral).
                    // Flat-top envelope keeps full motion along the length but anchors the mouths.
                    const float WormEnv = FMath::Clamp(FMath::Sin(T * PI) * 3.0f, 0.0f, 1.0f);
                    OX = PassageFBM(T * BendFreq, NSeedX)         * VOXEL_NOISE_SCALE * Cfg.Wander * WormEnv;
                    OY = PassageFBM(T * BendFreq, NSeedY + 53.0f) * VOXEL_NOISE_SCALE * Cfg.Wander * WormEnv;
                    break;
                }
                }

                // Vertical wobble (all styles): dips/rises along the descent, anchored at ends.
                if (Cfg.VerticalWobble > 0.0f)
                {
                    const float NZ = FMath::PerlinNoise3D(FVector(T * WormFreq * 1.3f + NSeedZ, NSeedZ * 0.5f, 27.0f));
                    Z += NZ * VOXEL_NOISE_SCALE * Cfg.VerticalWobble * Env;
                }

                const float BaseX = FMath::Lerp(UpperX, LowerX, T);
                const float BaseY = FMath::Lerp(UpperY, LowerY, T);
                FVector ControlPoint(BaseX + OX, BaseY + OY, Z);
                // The style envelope is mathematically zero at a mouth, but evaluating sin(PI)
                // leaves a tiny float residue. Pin both endpoints so the full query result is the
                // actual aimed mouth, including any lateral snap, and never merely an approximation.
                if (s == 0)
                {
                    ControlPoint = FVector(UpperX, UpperY, TopZ);
                }
                else if (s == Segments)
                {
                    ControlPoint = FVector(LowerX, LowerY, BottomZ);
                }
                Passage.ControlPoints.Add(ControlPoint);
                Passage.ControlRadii.Add(RadiusAt(T));
            }

            // The old control-point endpoints were also the player's standing points. That made
            // a sloped/vertical tube open directly under the capsule. Keep the queried points as
            // explicit standing anchors, and move only the tube's first/last point to a doorway
            // tangent to the landing floor. The endpoint-to-anchor distinction is the geometry
            // that turns a mouth into a place.
            auto DoorDirectionFrom = [](const FVector& Delta, float Angle) -> FVector
            {
                FVector Direction(Delta.X, Delta.Y, 0.0f);
                if (!Direction.Normalize())
                {
                    Direction = FVector(FMath::Cos(Angle), FMath::Sin(Angle), 0.0f);
                    if (!Direction.Normalize())
                    {
                        Direction = FVector(1.0f, 0.0f, 0.0f);
                    }
                }
                return Direction;
            };

            const FVector UpperDoorDirection = DoorDirectionFrom(
                Passage.ControlPoints.Num() > 1
                    ? Passage.ControlPoints[1] - Passage.ControlPoints[0]
                    : FVector::ZeroVector,
                PassageRandom01(PassageSaltUpperDoor) * 2.0f * PI);
            const FVector LowerDoorDirection = DoorDirectionFrom(
                Passage.ControlPoints.Num() > 1
                    ? Passage.ControlPoints[Passage.ControlPoints.Num() - 2]
                        - Passage.ControlPoints.Last()
                    : FVector::ZeroVector,
                PassageRandom01(PassageSaltLowerDoor) * 2.0f * PI);

            // A source-fit answer is the landing itself. Keep it local to the requested room: the
            // (0,0) landing is reserved for the future straight shaft, so no radial connector is
            // synthesized here.
            Passage.UpperLanding = VF_BuildPassageLanding(
                FVector(UpperX, UpperY, TopZ),
                Cfg.MouthRadius,
                UpperDoorDirection,
                UpperTopZ,
                UpperBottomZ,
                UpperDef ? BoundarySealThicknessFor(*UpperDef) : 0.0f,
                bAimedUpperAtPlayerFitPoint);
            Passage.LowerLanding = VF_BuildPassageLanding(
                FVector(LowerX, LowerY, BottomZ),
                Cfg.MouthRadius,
                LowerDoorDirection,
                LowerTopZ,
                (float)(Lower.BottomChunkZ) * CHUNK_SIZE,
                LowerDef ? BoundarySealThicknessFor(*LowerDef) : 0.0f,
                bAimedLowerAtPlayerFitPoint);
            Passage.UpperPoint = Passage.UpperLanding.StandingPoint;
            Passage.LowerPoint = Passage.LowerLanding.StandingPoint;

            if (Cfg.Style == EVoxelPassageStyle::Straight
                && Passage.UpperLanding.HalfWidth > 0.0f
                && Passage.LowerLanding.HalfWidth > 0.0f)
            {
                // BASE CONNECTION: a walkable two-leg switchback.  A direct line between the
                // mouths would often be nearly vertical because both requests are placed at the
                // same XY radius.  Put the turn on the perpendicular bisector and give each leg
                // at least half of the horizontal run required by the named 15-degree law.
                // This is deliberately a construction rule, not a seed-dependent observation.
                const float SwitchbackAngle =
                    PassageRandom01(PassageSaltUpperDoor) * 2.0f * PI;
                FVector2D SwitchbackDirection(
                    FMath::Cos(SwitchbackAngle), FMath::Sin(SwitchbackAngle));
                if (!SwitchbackDirection.Normalize())
                {
                    SwitchbackDirection = FVector2D(1.0f, 0.0f);
                }

                const FVector DoorDirection(
                    SwitchbackDirection.X, SwitchbackDirection.Y, 0.0f);
                Passage.UpperLanding.DoorDirection = DoorDirection;
                // A real switchback turns around: the lower doorway faces back toward the upper
                // leg. Keeping both doors on the same side would make the two sloped legs retrace
                // one XY line at different heights, leaving the floor projection ambiguous.
                const FVector TunnelLowerDoorDirection = -DoorDirection;
                Passage.LowerLanding.DoorDirection = TunnelLowerDoorDirection;
                const float UpperDoorOffset = FMath::Max(
                    Passage.UpperLanding.HalfWidth - 1.0f, 0.0f);
                const float LowerDoorOffset = FMath::Max(
                    Passage.LowerLanding.HalfWidth - 1.0f, 0.0f);
                const float SafeMouthRadius = FMath::Max(
                    FMath::Abs(Cfg.MouthRadius), 1.0f);
                Passage.UpperLanding.DoorPoint =
                    Passage.UpperLanding.StandingPoint + DoorDirection * UpperDoorOffset;
                Passage.UpperLanding.DoorPoint.Z =
                    Passage.UpperLanding.FloorZ + SafeMouthRadius;
                Passage.LowerLanding.DoorPoint =
                    Passage.LowerLanding.StandingPoint
                        + TunnelLowerDoorDirection * LowerDoorOffset;
                Passage.LowerLanding.DoorPoint.Z =
                    Passage.LowerLanding.FloorZ + SafeMouthRadius;

                const FVector UpperDoor = Passage.UpperLanding.DoorPoint;
                const FVector LowerDoor = Passage.LowerLanding.DoorPoint;
                // Keep a short level apron beyond each room floor. The door anchor is one voxel
                // inside the room's support square; a diagonal door direction can therefore need
                // several voxels before it leaves that square and its four-voxel SDF blend.
                // The level apron must clear the room half-width, the tube radius, and the
                // smooth carve band before the sloped leg begins.  The final six-voxel transition
                // is measured from that clear point back toward the turn, so include it too.  For
                // the default 8-voxel half-width / 6-voxel mouth this is 17 voxels (4.25 m), not
                // the old 6-voxel minimum that left the sloped capsule inside the landing room.
                const float LandingApron = FMath::Max(
                    VoxelPassageGeometry::WalkableTunnelLandingApronVoxels,
                    VoxelPassageGeometry::WalkableTunnelTurnTransitionVoxels
                        + SafeMouthRadius
                        + VoxelPassageGeometry::LandingCarveBlendVoxels
                        + 1.0f);
                const FVector UpperApronEnd = UpperDoor
                    + DoorDirection * LandingApron;
                const FVector LowerApronBegin = LowerDoor
                    + TunnelLowerDoorDirection * LandingApron;
                const float VerticalDrop = FMath::Abs(UpperDoor.Z - LowerDoor.Z);
                const float UpperFloorZ = VoxelPassageGeometry::TunnelFloorZ(
                    UpperDoor, Cfg.MouthRadius);
                const float LowerFloorZ = VoxelPassageGeometry::TunnelFloorZ(
                    LowerDoor, Cfg.MouthRadius);
                const float MidFloorZ = 0.5f * (UpperFloorZ + LowerFloorZ);
                const FVector2D RampStartMid(
                    0.5f * (UpperApronEnd.X + LowerApronBegin.X),
                    0.5f * (UpperApronEnd.Y + LowerApronBegin.Y));
                // The turn offset must be perpendicular to the actual two-apron endpoint chord,
                // not merely perpendicular to the standing-point chord.  Opposite-facing doors
                // add their offsets to that chord; using the old direction could make the two
                // sloped legs nearly collinear and bring the lower leg back beside the upper door.
                FVector2D TurnDirection(
                    LowerApronBegin.Y - UpperApronEnd.Y,
                    -(LowerApronBegin.X - UpperApronEnd.X));
                if (!TurnDirection.Normalize())
                {
                    TurnDirection = FVector2D(-DoorDirection.Y, DoorDirection.X);
                    if (!TurnDirection.Normalize())
                    {
                        TurnDirection = FVector2D(0.0f, 1.0f);
                    }
                }
                // There are two sides on which the switchback can turn. Choose the side whose
                // two sloped legs initially move away from their own landing rooms; the old fixed
                // sign could send a slope back through a room floor before reaching open space.
                FVector2D UpperOutward(
                    UpperApronEnd.X - Passage.UpperLanding.StandingPoint.X,
                    UpperApronEnd.Y - Passage.UpperLanding.StandingPoint.Y);
                FVector2D LowerOutward(
                    LowerApronBegin.X - Passage.LowerLanding.StandingPoint.X,
                    LowerApronBegin.Y - Passage.LowerLanding.StandingPoint.Y);
                UpperOutward.Normalize();
                LowerOutward.Normalize();
                const float TurnOffsetForScore =
                    0.5f * VoxelPassageGeometry::RequiredHorizontalRunForFloorDrop(
                        UpperFloorZ - 0.5f * (UpperFloorZ + LowerFloorZ),
                        0.5f * (UpperFloorZ + LowerFloorZ) - LowerFloorZ)
                    + VoxelPassageGeometry::WalkableTunnelTurnTransitionVoxels;
                const FVector2D TurnMidPlus(
                    RampStartMid.X + TurnDirection.X * TurnOffsetForScore,
                    RampStartMid.Y + TurnDirection.Y * TurnOffsetForScore);
                const FVector2D TurnMidMinus(
                    RampStartMid.X - TurnDirection.X * TurnOffsetForScore,
                    RampStartMid.Y - TurnDirection.Y * TurnOffsetForScore);
                const auto OutwardTurnScore = [
                    &UpperOutward, &LowerOutward, &UpperApronEnd, &LowerApronBegin]
                    (const FVector2D& Candidate) -> float
                {
                    return FVector2D::DotProduct(
                               Candidate - FVector2D(
                                   UpperApronEnd.X, UpperApronEnd.Y), UpperOutward)
                        + FVector2D::DotProduct(
                               FVector2D(LowerApronBegin.X, LowerApronBegin.Y) - Candidate,
                               LowerOutward);
                };
                if (OutwardTurnScore(TurnMidMinus) > OutwardTurnScore(TurnMidPlus))
                {
                    TurnDirection *= -1.0f;
                }
                const float UpperFloorDrop = UpperFloorZ - MidFloorZ;
                const float LowerFloorDrop = MidFloorZ - LowerFloorZ;
                const float RequiredHorizontalRun =
                    VoxelPassageGeometry::RequiredHorizontalRunForFloorDrop(
                        UpperFloorDrop, LowerFloorDrop);
                // Move the level turn farther than half the required run. Each slope then loses
                // only the named transition length to its level segment and still retains the
                // full horizontal run required by the 15-degree floor law.
                const float TurnTransition =
                    VoxelPassageGeometry::WalkableTunnelTurnTransitionVoxels;
                const float TurnOffset =
                    0.5f * RequiredHorizontalRun + TurnTransition;
                const FVector2D FloorSafeDoorMid(
                    RampStartMid.X
                        + TurnDirection.X * TurnOffset,
                    RampStartMid.Y
                        + TurnDirection.Y * TurnOffset);
                FVector2D UpperSlopeDirection(
                    FloorSafeDoorMid.X - UpperApronEnd.X,
                    FloorSafeDoorMid.Y - UpperApronEnd.Y);
                if (!UpperSlopeDirection.Normalize())
                {
                    UpperSlopeDirection = FVector2D(
                        DoorDirection.X, DoorDirection.Y);
                }
                FVector2D LowerSlopeDirection(
                    LowerApronBegin.X - FloorSafeDoorMid.X,
                    LowerApronBegin.Y - FloorSafeDoorMid.Y);
                if (!LowerSlopeDirection.Normalize())
                {
                    LowerSlopeDirection = -UpperSlopeDirection;
                }
                const FVector2D UpperSlopeStartXY(
                    UpperApronEnd.X + UpperSlopeDirection.X * TurnTransition,
                    UpperApronEnd.Y + UpperSlopeDirection.Y * TurnTransition);
                const FVector2D LowerSlopeEndXY(
                    LowerApronBegin.X - LowerSlopeDirection.X * TurnTransition,
                    LowerApronBegin.Y - LowerSlopeDirection.Y * TurnTransition);
                const FVector UpperApronEndPoint(
                    UpperApronEnd.X, UpperApronEnd.Y,
                    UpperFloorZ + FMath::Abs(Cfg.MouthRadius));
                const FVector UpperSlopeStart(
                    UpperSlopeStartXY.X, UpperSlopeStartXY.Y,
                    UpperFloorZ + FMath::Abs(Cfg.MouthRadius));
                const FVector MidPoint(
                    FloorSafeDoorMid.X, FloorSafeDoorMid.Y,
                    MidFloorZ + FMath::Abs(Cfg.MidRadius));
                const FVector LowerSlopeEnd(
                    LowerSlopeEndXY.X, LowerSlopeEndXY.Y,
                    LowerFloorZ + FMath::Abs(Cfg.MouthRadius));
                const FVector LowerApronBeginPoint(
                    LowerApronBegin.X, LowerApronBegin.Y,
                    LowerFloorZ + FMath::Abs(Cfg.MouthRadius));

                Passage.ControlPoints.Reset(7);
                Passage.ControlRadii.Reset(7);
                Passage.ControlPoints.Add(UpperDoor);
                Passage.ControlPoints.Add(UpperApronEndPoint);
                Passage.ControlPoints.Add(UpperSlopeStart);
                Passage.ControlPoints.Add(MidPoint);
                Passage.ControlPoints.Add(LowerSlopeEnd);
                Passage.ControlPoints.Add(LowerApronBeginPoint);
                Passage.ControlPoints.Add(LowerDoor);
                Passage.ControlRadii.Add(Cfg.MouthRadius);
                Passage.ControlRadii.Add(Cfg.MouthRadius);
                Passage.ControlRadii.Add(Cfg.MouthRadius);
                Passage.ControlRadii.Add(Cfg.MidRadius);
                Passage.ControlRadii.Add(Cfg.MouthRadius);
                Passage.ControlRadii.Add(Cfg.MouthRadius);
                Passage.ControlRadii.Add(Cfg.MouthRadius);

                Passage.PassageType = EVoxelPassageType::SlopedTunnel;
                Passage.bWalkableTunnelContract = true;
                Passage.TunnelMaxGradientDegrees =
                    VoxelPassageGeometry::WalkableTunnelMaxGradientDegrees;
                Passage.TunnelVerticalDropVoxels = VerticalDrop;
                Passage.TunnelRequiredHorizontalRunVoxels = RequiredHorizontalRun;
                Passage.TunnelHorizontalPathLengthVoxels = 0.0f;
                for (int32 ControlIndex = 0;
                     ControlIndex + 1 < Passage.ControlPoints.Num();
                     ++ControlIndex)
                {
                    Passage.TunnelHorizontalPathLengthVoxels += FVector2D(
                        Passage.ControlPoints[ControlIndex + 1].X
                            - Passage.ControlPoints[ControlIndex].X,
                        Passage.ControlPoints[ControlIndex + 1].Y
                            - Passage.ControlPoints[ControlIndex].Y).Size();
                }
                Passage.TunnelMinimumClearWidthVoxels = 2.0f * FMath::Max(
                    FMath::Min(FMath::Abs(Cfg.MouthRadius), FMath::Abs(Cfg.MidRadius)),
                    0.0f);
                Passage.TunnelClearHeightVoxels = Passage.TunnelMinimumClearWidthVoxels;
            }
            else if (Passage.ControlPoints.Num() > 0)
            {
                Passage.ControlPoints[0] = Passage.UpperLanding.DoorPoint;
                Passage.ControlPoints.Last() = Passage.LowerLanding.DoorPoint;
            }
            Passage.Radius = 0.0f;
            for (const float ControlRadius : Passage.ControlRadii)
            {
                Passage.Radius = FMath::Max(Passage.Radius, ControlRadius);
            }
            // Fallback / bounds if an invalid authored width produced no positive profile.
            Passage.Radius = FMath::Max(Passage.Radius, FMath::Max(Cfg.MouthRadius, Cfg.MidRadius));

            // The passage now owns its D-section at construction time.  Both landing floors,
            // the complete control chain, and the room-derived relief source are known here;
            // no voxel query needs to decide whether this particular passage deserves a floor.
            VF_AuthorNativePassageFloor(
                Passage, UpperDef, LowerDef,
                PassageSeed ^ VoxelHash::Cell(i, c, PassageSeed));

            // Bounding sphere over the tube, both rooms, and both floor slabs (+ widest radius +
            // blend) for culling. Under-sizing this sphere would cull a real landing and leave a
            // sealed pocket, so the room's full box diagonal is included rather than treating the
            // standing anchor as a point.
            {
                FVector BoundsMin(FLT_MAX, FLT_MAX, FLT_MAX);
                FVector BoundsMax(-FLT_MAX, -FLT_MAX, -FLT_MAX);
                auto IncludePoint = [&BoundsMin, &BoundsMax](const FVector& Point, float Pad)
                {
                    BoundsMin.X = FMath::Min(BoundsMin.X, Point.X - Pad);
                    BoundsMin.Y = FMath::Min(BoundsMin.Y, Point.Y - Pad);
                    BoundsMin.Z = FMath::Min(BoundsMin.Z, Point.Z - Pad);
                    BoundsMax.X = FMath::Max(BoundsMax.X, Point.X + Pad);
                    BoundsMax.Y = FMath::Max(BoundsMax.Y, Point.Y + Pad);
                    BoundsMax.Z = FMath::Max(BoundsMax.Z, Point.Z + Pad);
                };
                for (const FVector& CP : Passage.ControlPoints)
                {
                    IncludePoint(CP, Passage.Radius + 4.0f);
                }
                const auto IncludeLanding = [&IncludePoint](const FVoxelPassageLanding& Landing)
                {
                    const float Height = FMath::Max(Landing.CeilingZ - Landing.FloorZ, 0.0f);
                    const FVector RoomCenter(
                        Landing.StandingPoint.X,
                        Landing.StandingPoint.Y,
                        (Landing.FloorZ + Landing.CeilingZ) * 0.5f);
                    const float RoomRadius = FMath::Sqrt(
                        2.0f * FMath::Square(Landing.HalfWidth)
                        + 0.25f * FMath::Square(Height))
                        + Landing.FloorThickness + 4.0f;
                    IncludePoint(RoomCenter, RoomRadius);
                };
                IncludeLanding(Passage.UpperLanding);
                IncludeLanding(Passage.LowerLanding);

                const FVector Center = (BoundsMin + BoundsMax) * 0.5f;
                float MaxDistSq = 0.0f;
                MaxDistSq = FMath::Max(MaxDistSq, (float)FVector::DistSquared(Center, BoundsMin));
                MaxDistSq = FMath::Max(MaxDistSq, (float)FVector::DistSquared(Center, BoundsMax));
                const float R = FMath::Sqrt(MaxDistSq);
                Passage.BoundCenter = Center;
                Passage.BoundRadius = R;
                Passage.BoundRadiusSq = R * R;
            }

            Passages.Add(Passage);
        }
    }

    TArray<FString> SortedNoQueryArchetypes;
    for (const FString& Archetype : NoQueryArchetypes)
    {
        SortedNoQueryArchetypes.Add(Archetype);
    }
    SortedNoQueryArchetypes.Sort();
    FString NoQueryList = TEXT("none");
    if (SortedNoQueryArchetypes.Num() > 0)
    {
        NoQueryList = FString::Join(SortedNoQueryArchetypes, TEXT(", "));
    }

    UE_LOG(LogTemp, Log,
        TEXT("[StrateManager] Passage landings: upper %d/%d and lower %d/%d source-fit; %d/%d mouth queries fell back to random reach (archetypes with no query: %s)."),
        NumAimedAtUpperPlayerFit,
        TotalPassages,
        NumAimedAtLowerPlayerFit,
        TotalPassages,
        (TotalPassages * 2) - NumAimedAtUpperPlayerFit - NumAimedAtLowerPlayerFit,
        TotalPassages * 2,
        *NoQueryList);

    //=========================================================================
    // SURFACE ENTRY — the one optional above-ground opening.
    // It opens the top seal and stops at the ceiling of strate 0's origin landing room. It does
    // not continue down the room or into lower strates: every lower seal remains closed until the
    // corresponding progression passage is opened.
    //=========================================================================
    if (bOpenSurfaceEntry && OriginSpineRadius > 0.0f && StrateLayout.Num() > 0)
    {
        const FStrateSlot& Top = StrateLayout[0];
        const float TopZ = (float)(Top.TopChunkZ + 1) * CHUNK_SIZE;
        const float BottomZ = (float)Top.BottomChunkZ * CHUNK_SIZE;
        const float TopSeal = Top.Definition
            ? BoundarySealThicknessFor(*Top.Definition) : 0.0f;
        const VoxelPassageGeometry::FOriginLandingGeometry OriginLanding =
            VoxelPassageGeometry::BuildOriginLandingGeometry(
                TopZ, BottomZ, TopSeal, OriginSpineRadius);
        if (!OriginLanding.bValid)
        {
            UE_LOG(LogTemp, Warning,
                TEXT("[StrateManager] Surface entry skipped: top origin landing has no seal-safe room interval."));
        }
        else
        {
            FVoxelPassage Entry;
            Entry.UpperStrateIndex = 0;
            Entry.LowerStrateIndex = 0;
            // Keep the legacy type: this is the explicitly opted-in above-ground vertical opening,
            // not the default inter-strate descent style.
            Entry.PassageType = EVoxelPassageType::VerticalShaft;
            // Match the reserved future shaft envelope exactly. The entry is only opened through
            // the top seal into the finite landing room; lower strate seals remain untouched.
            Entry.Radius = FMath::Max(OriginSpineRadius, 1.0f);
            Entry.UpperPoint = FVector(0.0f, 0.0f, TopZ + CHUNK_SIZE);
            // The capsule's lower tangent meets the room ceiling; it never bores through the
            // room's floor or creates an origin column in strate 0.
            Entry.LowerPoint = FVector(
                0.0f, 0.0f, OriginLanding.CeilingZ + Entry.Radius);
            Entry.ControlPoints.Reset(2);
            Entry.ControlPoints.Add(Entry.UpperPoint);
            Entry.ControlPoints.Add(Entry.LowerPoint);
            Entry.ControlRadii.Reset(2);
            Entry.ControlRadii.Add(Entry.Radius);
            Entry.ControlRadii.Add(Entry.Radius);
            Entry.TunnelClearHeightVoxels = 2.0f * Entry.Radius;
            Entry.TunnelMinimumClearWidthVoxels = 2.0f * Entry.Radius;
            {
                const FVector C = (Entry.UpperPoint + Entry.LowerPoint) * 0.5f;
                const float R = (float)FVector::Dist(C, Entry.UpperPoint) + Entry.Radius + 4.0f;
                Entry.BoundCenter = C;
                Entry.BoundRadius = R;
                Entry.BoundRadiusSq = R * R;
            }
            Passages.Add(Entry);

            UE_LOG(LogTemp, Log,
                TEXT("[StrateManager] Surface entry opening at (0,0) topZ=%.0f roomFloor=%.1f roomCeiling=%.1f R=%.1f"),
                TopZ, OriginLanding.FloorZ, OriginLanding.CeilingZ, Entry.Radius);
        }
    }

    // Invalidate any thread_local per-chunk passage shortlists (see EvaluateModifierSDF).
    ++PassagesVersion;

    VoxelForgeStartupTrace::RecordEvent(TEXT("passages_ready"), FString::Printf(
        TEXT("\"passages\":%d,\"passages_version\":%u,\"upper_source_fit\":%d,\"lower_source_fit\":%d"),
        Passages.Num(), PassagesVersion, NumAimedAtUpperPlayerFit, NumAimedAtLowerPlayerFit));
}

//=============================================================================
// MODIFIER SDF (inter-strate passages)
//=============================================================================

float UVoxelStrateManager::EvaluateModifierSDF(float WorldX, float WorldY, float WorldZ) const
{
    const FIntVector ChunkCoord(
        FMath::FloorToInt(WorldX / (float)CHUNK_SIZE),
        FMath::FloorToInt(WorldY / (float)CHUNK_SIZE),
        FMath::FloorToInt(WorldZ / (float)CHUNK_SIZE));

    // The shortlist is rebuilt once for a (manager, version, chunk) and is shared by the tube,
    // landing, and floor paths. No source-fit stencil or topology search is allowed below it.
    const TArray<int32>& Nearby = VF_GetNearbyPassages(this, ChunkCoord);
    if (VoxelDensityProfile::AreCountersEnabled())
    {
        VoxelDensityProfile::AddCounter(
            VoxelDensityProfile::ECounter::PassageCandidates,
            static_cast<uint64>(Nearby.Num()));
    }

    if (Nearby.Num() == 0) return FLT_MAX;   // no passage near this chunk → no carve

    float MinSDF = FLT_MAX;
    const float BlendK = 3.0f;  // Smooth blend for passage junctions
    const FVoxelPassage* PassageData = Passages.GetData();

    //=========================================================================
    // PASSAGES — tapered capsule chains between strates (per-strate PassageConfig).
    // Each passage is a control-point chain with per-point radii; the (0,0) surface
    // entry is a simple straight tube. A bounding-sphere reject skips far passages.
    //=========================================================================
    const FVector Pos(WorldX, WorldY, WorldZ);
    for (int32 PIdx : Nearby)
    {
        const FVoxelPassage& P = PassageData[PIdx];
        // BOUNDING-SPHERE REJECT: skip passages this voxel can't possibly be inside.
        // EvaluateModifierSDF runs PER VOXEL and used to evaluate every passage's full
        // capsule chain unconditionally — the dominant lag source once passages became
        // 12-segment worms. Now far passages cost a single squared-distance compare.
        if (FVector::DistSquared(Pos, P.BoundCenter) > P.BoundRadiusSq) continue;
        if (VoxelDensityProfile::AreCountersEnabled())
        {
            VoxelDensityProfile::AddCounter(
                VoxelDensityProfile::ECounter::PassageEvaluated);
        }

        const int32 ControlPointCount = P.ControlPoints.Num();
        if (ControlPointCount >= 2)
        {
            // Tapered capsule chain along the control points. ControlRadii (if present)
            // gives the per-point width so the tunnel can flare at the mouths and pinch
            // in the middle; otherwise the uniform Radius is used.
            const bool bTaper = (P.ControlRadii.Num() == P.ControlPoints.Num());
            const FVector* ControlPointData = P.ControlPoints.GetData();
            const float* ControlRadiusData = P.ControlRadii.GetData();
            float PassageSDF = FLT_MAX;
            for (int32 j = 0; j < ControlPointCount - 1; j++)
            {
                const float rA = bTaper ? ControlRadiusData[j]     : P.Radius;
                const float rB = bTaper ? ControlRadiusData[j + 1] : P.Radius;
                const float SegSDF = VoxelSDF::TaperedCapsule(
                    Pos, ControlPointData[j], ControlPointData[j + 1], rA, rB);
                PassageSDF = VoxelSDF::SmoothMin(PassageSDF, SegSDF, BlendK);
            }

            // A walkable inter-strate passage is a D-shaped primitive, not a capsule followed
            // by a slab.  The profile and its bounded relief were authored in GeneratePassages;
            // projecting once here selects the nearest immutable floor segment for this sample.
            // Unknown/malformed descriptors deliberately retain the bare capsule and the legacy
            // diagnostic backstop remains available.
            if (P.bNativeFloorEnabled)
            {
                float NativeFloorZ = 0.0f;
                float NativeSupportRadius = 0.0f;
                if (VF_ProjectNativePassageFloor(
                        P, Pos, NativeFloorZ, NativeSupportRadius,
                        this, PIdx))
                {
                    PassageSDF = VoxelSDF::SmoothMax(
                        PassageSDF,
                        NativeFloorZ - static_cast<float>(Pos.Z),
                        BlendK * 0.35f);
                }
            }
            MinSDF = VoxelSDF::SmoothMin(MinSDF, PassageSDF, BlendK);
        }
        else
        {
            // Fallback: straight uniform tube Upper→Lower (e.g. the (0,0) surface entry).
            const float PassageSDF = VoxelSDF::Capsule(Pos, P.UpperPoint, P.LowerPoint, P.Radius);
            MinSDF = VoxelSDF::SmoothMin(MinSDF, PassageSDF, BlendK);
        }

        // A landing is a real room with a hard flat-floor half-space, not a sphere around the
        // tube endpoint. It is the same fixed SDF geometry selected during GeneratePassages; it
        // never performs a source query here and does not synthesize a radial root connector.
        const float UpperLandingSDF = VF_EvaluatePassageLandingSDF(Pos, P.UpperLanding);
        const float LowerLandingSDF = VF_EvaluatePassageLandingSDF(Pos, P.LowerLanding);
        MinSDF = VoxelSDF::SmoothMin(MinSDF, UpperLandingSDF, BlendK);
        MinSDF = VoxelSDF::SmoothMin(MinSDF, LowerLandingSDF, BlendK);
    }

    return MinSDF;
}

void UVoxelStrateManager::ApplyPassageModifier(
    float& Density, float WorldX, float WorldY, float WorldZ,
    float BaseDensity, float SealThickness) const
{
    VoxelDensityProfile::FScopedTimer ProfileTimer(
        VoxelDensityProfile::EBucket::PassageModifier);
    const float ModSDF = EvaluateModifierSDF(WorldX, WorldY, WorldZ);
    VF_ApplyPassageCarving(Density, ModSDF, BaseDensity, SealThickness);
    ApplyPassageLandingAir(Density, WorldX, WorldY, WorldZ, BaseDensity, SealThickness);

    const FIntVector ChunkCoord(
        FMath::FloorToInt(WorldX / (float)CHUNK_SIZE),
        FMath::FloorToInt(WorldY / (float)CHUNK_SIZE),
        FMath::FloorToInt(WorldZ / (float)CHUNK_SIZE));
    const TArray<int32>& Nearby = VF_GetNearbyPassages(this, ChunkCoord);
    const FVoxelPassage* PassageData = Passages.GetData();
    const FVector Position(WorldX, WorldY, WorldZ);
    for (const int32 PassageIndex : Nearby)
    {
        const FVoxelPassage& Passage = PassageData[PassageIndex];
        if (!VoxelPassageGeometry::VerticalShaftConnectorAirMarker()
            && (VF_IsPassageLandingFloor(Position, Passage.UpperLanding)
            || VF_IsPassageLandingFloor(Position, Passage.LowerLanding)
            // A valid native profile owns its D-floor in the shape and in the final MC
            // composition.  Keep the old support slab only as a fail-closed fallback for a
            // malformed/non-native descriptor; it must not patch a floor the shape already owns.
            || (bPassageSupportFloorWritesEnabled
                && !Passage.bNativeFloorEnabled
                && VF_IsWalkableTunnelFloor(Passage, Position, this, PassageIndex))))
        {
            // This is the one bidirectional part of PassageCarveOp: a floor is a proved solid
            // support slab. It is deliberately applied after the air carve so a tube can never
            // tunnel through the floor and leave the player over a void.
            Density = FMath::Max(Density, BaseDensity);
            break;
        }
    }

    // A neighbouring passage may own a floor at this XY while this point is in the air core of
    // the current walkable tunnel.  Reassert tunnel air after all floor posts so overlap cannot
    // turn a valid route into a solid plug.  At the tunnel's own floor the air predicate is false,
    // so the support plane remains solid.
    ApplyPassageTunnelAir(Density, WorldX, WorldY, WorldZ, BaseDensity, SealThickness);
}

void UVoxelStrateManager::ApplyPassageCarvingOnly(
    float& Density, float WorldX, float WorldY, float WorldZ,
    float BaseDensity, float SealThickness) const
{
    VoxelDensityProfile::FScopedTimer ProfileTimer(
        VoxelDensityProfile::EBucket::PassageModifier);
    const float ModSDF = EvaluateModifierSDF(WorldX, WorldY, WorldZ);
    VF_ApplyPassageCarving(Density, ModSDF, BaseDensity, SealThickness);
}

void UVoxelStrateManager::ApplyPassageLandingAir(
    float& Density, float WorldX, float WorldY, float WorldZ,
    float BaseDensity, float SealThickness) const
{
    VoxelDensityProfile::FScopedTimer ProfileTimer(
        VoxelDensityProfile::EBucket::PassageLandingAir);
    const FIntVector ChunkCoord(
        FMath::FloorToInt(WorldX / (float)CHUNK_SIZE),
        FMath::FloorToInt(WorldY / (float)CHUNK_SIZE),
        FMath::FloorToInt(WorldZ / (float)CHUNK_SIZE));
    const TArray<int32>& Nearby = VF_GetNearbyPassages(this, ChunkCoord);
    if (Nearby.Num() == 0) return;

    const FVoxelPassage* PassageData = Passages.GetData();
    const FVector Position(WorldX, WorldY, WorldZ);
    // A smooth room blend may overlap the end of a ramp.  The analytic support plane owns that
    // overlap; otherwise landing air can erase the tunnel's final-density floor after the tunnel
    // air post has deliberately stopped above it.
    if (VF_IsAnyPassageFloorAt(this, Position))
    {
        return;
    }
    float MinLandingSDF = FLT_MAX;
    for (const int32 PassageIndex : Nearby)
    {
        const FVoxelPassage& Passage = PassageData[PassageIndex];
        if (FVector::DistSquared(Position, Passage.BoundCenter) > Passage.BoundRadiusSq)
        {
            continue;
        }
        const FVoxelPassageLanding* Landings[] = {
            &Passage.UpperLanding, &Passage.LowerLanding };
        for (const FVoxelPassageLanding* Landing : Landings)
        {
            const float LandingSDF = VF_EvaluatePassageLandingSDF(Position, *Landing);
            if (LandingSDF < MinLandingSDF)
            {
                MinLandingSDF = LandingSDF;
            }
        }
    }

    if (MinLandingSDF == FLT_MAX) return;

    // Keep the same smooth interior blend used by the passage op. This landing-specific writer is
    // idempotent because the MC-facing post may run after the structural op already saw the same
    // voxel. The landing's support floor is restored by the floor writer after this MC-facing pass.
    VF_ApplyPassageLandingCarving(Density, MinLandingSDF, BaseDensity, SealThickness);
}

void UVoxelStrateManager::ApplyPassageLandingAirMC(
    float& Density, float WorldX, float WorldY, float WorldZ,
    float BaseDensity, float SealThickness) const
{
    // ApplyDisturbances is deliberately an MC-space post-process and can add a bridge or ridge
    // on top of the structural passage. Reassert only landing air here; the support slab is
    // restored by ApplyPassageLandingFloorMC immediately afterwards. This stays on the same
    // thread-local passage shortlist as the hot voxel path and performs no source/topology work.
    float InternalDensity = -Density;
    ApplyPassageLandingAir(
        InternalDensity, WorldX, WorldY, WorldZ, BaseDensity, SealThickness);
    Density = -InternalDensity;
}

void UVoxelStrateManager::ApplyPassageTunnelAir(
    float& Density, float WorldX, float WorldY, float WorldZ,
    float BaseDensity, float SealThickness) const
{
    VoxelDensityProfile::FScopedTimer ProfileTimer(
        VoxelDensityProfile::EBucket::PassageTunnelAir);
    const FVector Position(WorldX, WorldY, WorldZ);
    int32 PassageIndex = INDEX_NONE;
    float FloorZ = 0.0f;
    float SupportRadius = 0.0f;
    const bool bFoundWalkableAir = VF_FindWalkableTunnelAir(
        this, Position, PassageIndex, FloorZ, SupportRadius);
    if (bFoundWalkableAir)
    {
        const float AirTarget = -(BaseDensity * 2.0f + SealThickness + 4.0f);
        Density = FMath::Min(Density, AirTarget);
    }
}

void UVoxelStrateManager::ApplyPassageTunnelAirMC(
    float& Density, float WorldX, float WorldY, float WorldZ,
    float BaseDensity, float SealThickness) const
{
    // The disturbance layer uses MC polarity (negative = solid), so reuse the exact internal
    // tunnel-air operation rather than maintaining a second polarity-specific formula.
    float InternalDensity = -Density;
    ApplyPassageTunnelAir(
        InternalDensity, WorldX, WorldY, WorldZ, BaseDensity, SealThickness);
    Density = -InternalDensity;
}

void UVoxelStrateManager::ApplyPassageLandingFloorMC(
    float& Density, float WorldX, float WorldY, float WorldZ, float BaseDensity) const
{
    VoxelDensityProfile::FScopedTimer ProfileTimer(
        VoxelDensityProfile::EBucket::PassageLandingFloor);
    const FIntVector ChunkCoord(
        FMath::FloorToInt(WorldX / (float)CHUNK_SIZE),
        FMath::FloorToInt(WorldY / (float)CHUNK_SIZE),
        FMath::FloorToInt(WorldZ / (float)CHUNK_SIZE));
    const TArray<int32>& Nearby = VF_GetNearbyPassages(this, ChunkCoord);
    const FVoxelPassage* PassageData = Passages.GetData();
    const FVector Position(WorldX, WorldY, WorldZ);
    for (const int32 PassageIndex : Nearby)
    {
        const FVoxelPassage& Passage = PassageData[PassageIndex];
        if (!VoxelPassageGeometry::VerticalShaftConnectorAirMarker()
            && (VF_IsPassageLandingFloor(Position, Passage.UpperLanding)
                || VF_IsPassageLandingFloor(Position, Passage.LowerLanding)
                || (bPassageSupportFloorWritesEnabled
                    && !Passage.bNativeFloorEnabled
                    && VF_IsWalkableTunnelFloor(Passage, Position, this, PassageIndex))))
        {
            // Result is in MC convention here (negative = solid). This reassertion is the
            // structural floor backstop after the optional MC-space disturbance layer.
            Density = FMath::Min(Density, -BaseDensity);
            break;
        }
    }
}

static bool VF_IsPassageRoomFloor(
    const FVector& Position, const FVoxelPassageLanding& Landing)
{
    if (!VoxelMath::IsFinite(Position.X) || !VoxelMath::IsFinite(Position.Y)
        || !VoxelMath::IsFinite(Position.Z) || !VoxelMath::IsFinite(Landing.FloorZ)
        || !VoxelMath::IsFinite(Landing.FloorThickness)
        || Landing.FloorThickness <= 0.0f || Landing.HalfWidth <= 0.0f)
    {
        return false;
    }
    return Position.Z <= Landing.FloorZ + KINDA_SMALL_NUMBER
        && Position.Z > Landing.FloorZ - Landing.FloorThickness
        && FMath::Abs(Position.X - Landing.StandingPoint.X)
            <= FMath::Max(Landing.HalfWidth - 1.0f, 0.0f)
        && FMath::Abs(Position.Y - Landing.StandingPoint.Y)
            <= FMath::Max(Landing.HalfWidth - 1.0f, 0.0f);
}

void UVoxelStrateManager::ApplyPassageStructuralPostsMC(
    float& Density, float WorldX, float WorldY, float WorldZ,
    float BaseDensity, float SealThickness,
    bool bProtectAuthoredTunnelFloor) const
{
    VoxelDensityProfile::FScopedTimer ProfileTimer(
        VoxelDensityProfile::EBucket::PassageStructuralPosts);

    const FIntVector ChunkCoord(
        FMath::FloorToInt(WorldX / (float)CHUNK_SIZE),
        FMath::FloorToInt(WorldY / (float)CHUNK_SIZE),
        FMath::FloorToInt(WorldZ / (float)CHUNK_SIZE));
    const TArray<int32>& Nearby = VF_GetNearbyPassages(this, ChunkCoord);
    if (Nearby.Num() == 0) return;

    const FVoxelPassage* PassageData = Passages.GetData();
    const FVector Position(WorldX, WorldY, WorldZ);
    const bool bSuppressFloor =
        VoxelPassageGeometry::VerticalShaftConnectorAirMarker();
    bool bAnyPassageFloor = false;
    bool bLegacyTunnelSupportFloor = false;
    bool bAnyRoomFloor = false;
    bool bWalkableAir = false;
    float MinLandingSDF = FLT_MAX;

    // The four old MC backstops all walked the same immutable per-chunk shortlist. Gather their
    // predicates in one pass; the writes below retain the old order (landing air, landing floor,
    // tunnel air, room floor), including the vertical-shaft floor suppression marker.
    for (const int32 PassageIndex : Nearby)
    {
        if (!Passages.IsValidIndex(PassageIndex)) continue;
        const FVoxelPassage& Passage = PassageData[PassageIndex];

        const bool bTunnelSupportFloor =
            bPassageSupportFloorWritesEnabled
            && !Passage.bNativeFloorEnabled
            && VF_IsWalkableTunnelFloor(Passage, Position, this, PassageIndex);
        if (!bSuppressFloor
            && (VF_IsPassageLandingFloor(Position, Passage.UpperLanding)
                || VF_IsPassageLandingFloor(Position, Passage.LowerLanding)
                || bTunnelSupportFloor))
        {
            bAnyPassageFloor = true;
        }
        if (!bSuppressFloor && bTunnelSupportFloor)
        {
            bLegacyTunnelSupportFloor = true;
        }

        if (!bSuppressFloor
            && (VF_IsPassageRoomFloor(Position, Passage.UpperLanding)
                || VF_IsPassageRoomFloor(Position, Passage.LowerLanding)))
        {
            bAnyRoomFloor = true;
        }

        if (!bWalkableAir
            && FVector::DistSquared(Position, Passage.BoundCenter) <= Passage.BoundRadiusSq
            && VF_IsWalkableTunnelAir(Passage, Position, this, PassageIndex))
        {
            bWalkableAir = true;
        }

        if (FVector::DistSquared(Position, Passage.BoundCenter) <= Passage.BoundRadiusSq)
        {
            const FVoxelPassageLanding* Landings[] = {
                &Passage.UpperLanding, &Passage.LowerLanding };
            for (const FVoxelPassageLanding* Landing : Landings)
            {
                MinLandingSDF = FMath::Min(
                    MinLandingSDF,
                    VF_EvaluatePassageLandingSDF(Position, *Landing));
            }
        }
    }

    // A graph tunnel floor is authored by the room-graph source with its complete clearance
    // budget. A neighbouring inter-strate landing must not reopen that finite support band; its
    // carve remains valid everywhere else. Legacy callers leave this false and retain the old
    // passage-owned behavior.
    if (!bProtectAuthoredTunnelFloor
        && !bAnyPassageFloor && MinLandingSDF < FLT_MAX)
    {
        float InternalDensity = -Density;
        VF_ApplyPassageLandingCarving(
            InternalDensity, MinLandingSDF, BaseDensity, SealThickness);
        Density = -InternalDensity;
    }

    if (bAnyPassageFloor)
    {
        Density = FMath::Min(Density, -BaseDensity);
    }

    if (bWalkableAir && !bProtectAuthoredTunnelFloor)
    {
        Density = FMath::Max(
            Density, BaseDensity * 2.0f + SealThickness + 4.0f);
    }

    if (bAnyRoomFloor)
    {
        Density = FMath::Min(Density, -BaseDensity);
    }

    if (bLegacyTunnelSupportFloor && VoxelDensityProfile::AreCountersEnabled())
    {
        VoxelDensityProfile::AddCounter(
            VoxelDensityProfile::ECounter::PassageSupportFloorBackstopFires);
    }
}

void UVoxelStrateManager::ApplyPassageNativeFloorMC(
    float& Density, float WorldX, float WorldY, float WorldZ,
    float BaseDensity) const
{
    const FIntVector ChunkCoord(
        FMath::FloorToInt(WorldX / (float)CHUNK_SIZE),
        FMath::FloorToInt(WorldY / (float)CHUNK_SIZE),
        FMath::FloorToInt(WorldZ / (float)CHUNK_SIZE));
    const TArray<int32>& Nearby = VF_GetNearbyPassages(this, ChunkCoord);
    if (Nearby.Num() == 0
        || VoxelPassageGeometry::VerticalShaftConnectorAirMarker())
    {
        return;
    }

    const FVoxelPassage* PassageData = Passages.GetData();
    const FVector Position(WorldX, WorldY, WorldZ);
    for (const int32 PassageIndex : Nearby)
    {
        if (!Passages.IsValidIndex(PassageIndex))
        {
            continue;
        }
        const FVoxelPassage& Passage = PassageData[PassageIndex];
        if (!Passage.bNativeFloorEnabled)
        {
            continue;
        }

        float FloorZ = 0.0f;
        float SupportRadius = 0.0f;
        if (!VF_ProjectNativePassageFloor(
                Passage, Position, FloorZ, SupportRadius,
                this, PassageIndex))
        {
            continue;
        }
        if (Position.Z <= FloorZ + KINDA_SMALL_NUMBER
            && Position.Z > FloorZ - VoxelPassageGeometry::LandingFloorThicknessVoxels)
        {
            // The old support slab is now disabled in the A/B run.  This final write is the
            // D-shaped primitive's floor ownership, derived from its build-time profile; it is
            // intentionally not controlled by bPassageSupportFloorWritesEnabled.
            Density = FMath::Min(Density, -FMath::Max(BaseDensity, 1.0f));
            if (VoxelDensityProfile::AreCountersEnabled())
            {
                VoxelDensityProfile::AddCounter(
                    VoxelDensityProfile::ECounter::PassageNativeFloorCompositions);
            }
            break;
        }
    }
}

void UVoxelStrateManager::ApplyPassageLandingRoomFloorMC(
    float& Density, float WorldX, float WorldY, float WorldZ,
    float BaseDensity) const
{
    VoxelDensityProfile::FScopedTimer ProfileTimer(
        VoxelDensityProfile::EBucket::PassageLandingRoomFloor);
    const FIntVector ChunkCoord(
        FMath::FloorToInt(WorldX / (float)CHUNK_SIZE),
        FMath::FloorToInt(WorldY / (float)CHUNK_SIZE),
        FMath::FloorToInt(WorldZ / (float)CHUNK_SIZE));
    const TArray<int32>& Nearby = VF_GetNearbyPassages(this, ChunkCoord);
    const FVoxelPassage* PassageData = Passages.GetData();
    const FVector Position(WorldX, WorldY, WorldZ);
    for (const int32 PassageIndex : Nearby)
    {
        const FVoxelPassage& Passage = PassageData[PassageIndex];
        if (!VoxelPassageGeometry::VerticalShaftConnectorAirMarker()
            && (VF_IsPassageRoomFloor(Position, Passage.UpperLanding)
                || VF_IsPassageRoomFloor(Position, Passage.LowerLanding)))
        {
            Density = FMath::Min(Density, -BaseDensity);
            break;
        }
    }
}

bool UVoxelStrateManager::AnyPassageNearBox(const FVector& MinVoxel, const FVector& MaxVoxel) const
{
    // Le carve d'un passage atteint ModSDF < PASSAGE_BLEND_RADIUS (4, VoxelGenerator.cpp) au-delà de
    // sa surface ; BoundRadius inclut déjà rayon + blend, on re-pad par sécurité (conservatif).
    constexpr float CarvePad = 4.0f;
    for (const FVoxelPassage& P : Passages)
    {
        // Point de la boîte le plus proche du centre de la sphère → test sphère/AABB.
        const FVector C(
            FMath::Clamp(P.BoundCenter.X, MinVoxel.X, MaxVoxel.X),
            FMath::Clamp(P.BoundCenter.Y, MinVoxel.Y, MaxVoxel.Y),
            FMath::Clamp(P.BoundCenter.Z, MinVoxel.Z, MaxVoxel.Z));
        const float Reach = P.BoundRadius + CarvePad;
        if (FVector::DistSquared(C, P.BoundCenter) <= Reach * Reach)
        {
            return true;
        }
    }
    return false;
}

bool UVoxelStrateManager::AnyPassageLandingFloorNearBox(
    const FVector& MinVoxel, const FVector& MaxVoxel) const
{
    // A conservative AABB test is enough for the floor's only solid write. False positives cost
    // a tile; a false negative could classify an air tile uniformly and remove the player's floor.
    for (const FVoxelPassage& Passage : Passages)
    {
        const FVoxelPassageLanding* Landings[] = {
            &Passage.UpperLanding, &Passage.LowerLanding };
        for (const FVoxelPassageLanding* Landing : Landings)
        {
            if (Landing->HalfWidth <= 0.0f || Landing->FloorThickness <= 0.0f)
            {
                continue;
            }
            const float Pad = 1.0f;
            const float FloorMinX = Landing->StandingPoint.X - Landing->HalfWidth - Pad;
            const float FloorMaxX = Landing->StandingPoint.X + Landing->HalfWidth + Pad;
            const float FloorMinY = Landing->StandingPoint.Y - Landing->HalfWidth - Pad;
            const float FloorMaxY = Landing->StandingPoint.Y + Landing->HalfWidth + Pad;
            const float FloorMinZ = Landing->FloorZ - Landing->FloorThickness - Pad;
            const float FloorMaxZ = Landing->FloorZ + Pad;
            if (FloorMaxX >= MinVoxel.X && FloorMinX <= MaxVoxel.X
                && FloorMaxY >= MinVoxel.Y && FloorMinY <= MaxVoxel.Y
                && FloorMaxZ >= MinVoxel.Z && FloorMinZ <= MaxVoxel.Z)
            {
                return true;
            }

        }

        // The default tube also owns a three-voxel support band. Its conservative segment AABB
        // keeps a classifier from proving AllAir over the floor and then dropping that support
        // during meshing. False positives are intentional; false negatives would be a hole.
        if (Passage.bWalkableTunnelContract
            && Passage.ControlPoints.Num() >= 2
            && Passage.ControlRadii.Num() == Passage.ControlPoints.Num())
        {
            constexpr float Pad = 1.0f;
            for (int32 SegmentIndex = 0;
                 SegmentIndex + 1 < Passage.ControlPoints.Num();
                 ++SegmentIndex)
            {
                const FVector& A = Passage.ControlPoints[SegmentIndex];
                const FVector& B = Passage.ControlPoints[SegmentIndex + 1];
                const float SupportRadius = FMath::Max(
                    FMath::Min(
                        FMath::Abs(Passage.ControlRadii[SegmentIndex]),
                        FMath::Abs(Passage.ControlRadii[SegmentIndex + 1])) - 0.5f,
                    VoxelPassageGeometry::PlayerRadiusVoxels);
                const float FloorMinZ = FMath::Min(
                    VoxelPassageGeometry::TunnelFloorZ(
                        A, Passage.ControlRadii[SegmentIndex]),
                    VoxelPassageGeometry::TunnelFloorZ(
                        B, Passage.ControlRadii[SegmentIndex + 1]))
                    - VoxelPassageGeometry::LandingFloorThicknessVoxels - Pad;
                const float FloorMaxZ = FMath::Max(
                    VoxelPassageGeometry::TunnelFloorZ(
                        A, Passage.ControlRadii[SegmentIndex]),
                    VoxelPassageGeometry::TunnelFloorZ(
                        B, Passage.ControlRadii[SegmentIndex + 1])) + Pad;
                const float FloorMinX = FMath::Min(A.X, B.X) - SupportRadius - Pad;
                const float FloorMaxX = FMath::Max(A.X, B.X) + SupportRadius + Pad;
                const float FloorMinY = FMath::Min(A.Y, B.Y) - SupportRadius - Pad;
                const float FloorMaxY = FMath::Max(A.Y, B.Y) + SupportRadius + Pad;
                if (FloorMaxX >= MinVoxel.X && FloorMinX <= MaxVoxel.X
                    && FloorMaxY >= MinVoxel.Y && FloorMinY <= MaxVoxel.Y
                    && FloorMaxZ >= MinVoxel.Z && FloorMinZ <= MaxVoxel.Z)
                {
                    return true;
                }
            }
        }
    }
    return false;
}

namespace
{
    static bool VF_LatticeAxisRange(
        float Min, float Max, float Origin, int32 Step,
        int32& OutFirst, int32& OutLast)
    {
        if (Step <= 0 || !VoxelMath::IsFinite(Min) || !VoxelMath::IsFinite(Max)
            || !VoxelMath::IsFinite(Origin) || Min > Max)
        {
            return false;
        }
        const float InvStep = 1.0f / static_cast<float>(Step);
        OutFirst = FMath::CeilToInt((Min - Origin) * InvStep - 1.0e-4f);
        OutLast  = FMath::FloorToInt((Max - Origin) * InvStep + 1.0e-4f);
        return OutFirst <= OutLast;
    }

    struct FVoxelPassageModifierDomain
    {
        FVector Min = FVector(FLT_MAX, FLT_MAX, FLT_MAX);
        FVector Max = FVector(-FLT_MAX, -FLT_MAX, -FLT_MAX);
    };

    static bool VF_LatticeBoxTouchesAABB(
        const FBox& Box, const FIntVector& Origin, int32 Step,
        float MinX, float MaxX, float MinY, float MaxY, float MinZ, float MaxZ);

    static bool VF_BuildPassageModifierDomains(
        const TArray<FVoxelPassage>& Passages,
        TArray<FVoxelPassageModifierDomain, TInlineAllocator<128>>& Domains)
    {
        constexpr int32 MaxControlPointsForBound = 4096;
        Domains.Reset();

        auto AddDomain = [&](const FVector& Min, const FVector& Max) -> bool
        {
            if (!VoxelMath::IsFinite(Min.X) || !VoxelMath::IsFinite(Min.Y)
                || !VoxelMath::IsFinite(Min.Z) || !VoxelMath::IsFinite(Max.X)
                || !VoxelMath::IsFinite(Max.Y) || !VoxelMath::IsFinite(Max.Z)
                || Min.X > Max.X || Min.Y > Max.Y || Min.Z > Max.Z)
            {
                return false;
            }
            Domains.Add({Min, Max});
            return true;
        };

        for (const FVoxelPassage& Passage : Passages)
        {
            if (!VoxelMath::IsFinite(Passage.BoundCenter.X)
                || !VoxelMath::IsFinite(Passage.BoundCenter.Y)
                || !VoxelMath::IsFinite(Passage.BoundCenter.Z)
                || !VoxelMath::IsFinite(Passage.BoundRadius)
                || !VoxelMath::IsFinite(Passage.BoundRadiusSq)
                || Passage.BoundRadius < 0.0f || Passage.BoundRadiusSq < 0.0f)
            {
                // Invalid generated geometry is an unknown, not an empty passage set.
                return false;
            }

            if (Passage.ControlPoints.Num() >= 2)
            {
                if (Passage.ControlPoints.Num() > MaxControlPointsForBound)
                {
                    return false;
                }
                const bool bTaper = Passage.ControlRadii.Num() == Passage.ControlPoints.Num();
                if (!bTaper && !VoxelMath::IsFinite(Passage.Radius))
                {
                    return false;
                }
                for (int32 PointIndex = 0;
                     PointIndex < Passage.ControlPoints.Num(); ++PointIndex)
                {
                    const FVector& Point = Passage.ControlPoints[PointIndex];
                    if (!VoxelMath::IsFinite(Point.X) || !VoxelMath::IsFinite(Point.Y)
                        || !VoxelMath::IsFinite(Point.Z))
                    {
                        return false;
                    }
                    if (bTaper && !VoxelMath::IsFinite(Passage.ControlRadii[PointIndex]))
                    {
                        return false;
                    }
                }

                for (int32 SegmentIndex = 0;
                     SegmentIndex + 1 < Passage.ControlPoints.Num(); ++SegmentIndex)
                {
                    const FVector& A = Passage.ControlPoints[SegmentIndex];
                    const FVector& B = Passage.ControlPoints[SegmentIndex + 1];
                    const float RadiusA = bTaper
                        ? Passage.ControlRadii[SegmentIndex] : Passage.Radius;
                    const float RadiusB = bTaper
                        ? Passage.ControlRadii[SegmentIndex + 1] : Passage.Radius;
                    const float Radius = FMath::Max(FMath::Abs(RadiusA), FMath::Abs(RadiusB));
                    if (!VoxelMath::IsFinite(Radius))
                    {
                        return false;
                    }
                    if (!AddDomain(
                            FVector(FMath::Min(A.X, B.X) - Radius,
                                    FMath::Min(A.Y, B.Y) - Radius,
                                    FMath::Min(A.Z, B.Z) - Radius),
                            FVector(FMath::Max(A.X, B.X) + Radius,
                                    FMath::Max(A.Y, B.Y) + Radius,
                                    FMath::Max(A.Z, B.Z) + Radius)))
                    {
                        return false;
                    }
                }
            }
            else
            {
                if (!VoxelMath::IsFinite(Passage.UpperPoint.X)
                    || !VoxelMath::IsFinite(Passage.UpperPoint.Y)
                    || !VoxelMath::IsFinite(Passage.UpperPoint.Z)
                    || !VoxelMath::IsFinite(Passage.LowerPoint.X)
                    || !VoxelMath::IsFinite(Passage.LowerPoint.Y)
                    || !VoxelMath::IsFinite(Passage.LowerPoint.Z)
                    || !VoxelMath::IsFinite(Passage.Radius))
                {
                    return false;
                }
                const float Radius = FMath::Abs(Passage.Radius);
                if (!AddDomain(
                        FVector(FMath::Min(Passage.UpperPoint.X, Passage.LowerPoint.X) - Radius,
                                FMath::Min(Passage.UpperPoint.Y, Passage.LowerPoint.Y) - Radius,
                                FMath::Min(Passage.UpperPoint.Z, Passage.LowerPoint.Z) - Radius),
                        FVector(FMath::Max(Passage.UpperPoint.X, Passage.LowerPoint.X) + Radius,
                                FMath::Max(Passage.UpperPoint.Y, Passage.LowerPoint.Y) + Radius,
                                FMath::Max(Passage.UpperPoint.Z, Passage.LowerPoint.Z) + Radius)))
                {
                    return false;
                }
            }

            const FVoxelPassageLanding* Landings[] = {
                &Passage.UpperLanding, &Passage.LowerLanding };
            for (const FVoxelPassageLanding* Landing : Landings)
            {
                if (!VoxelMath::IsFinite(Landing->StandingPoint.X)
                    || !VoxelMath::IsFinite(Landing->StandingPoint.Y)
                    || !VoxelMath::IsFinite(Landing->FloorZ)
                    || !VoxelMath::IsFinite(Landing->CeilingZ)
                    || !VoxelMath::IsFinite(Landing->HalfWidth))
                {
                    // The canonical evaluator returns FLT_MAX for this landing. Treating it as
                    // unknown is safer for future evaluator changes and only gives up a skip.
                    return false;
                }
                if (Landing->HalfWidth <= 0.0f
                    || Landing->CeilingZ <= Landing->FloorZ)
                {
                    continue;
                }
                constexpr float RoomRounding = 1.5f;
                const float Height = Landing->CeilingZ - Landing->FloorZ;
                const float HalfX = FMath::Max(Landing->HalfWidth - RoomRounding, 0.25f);
                const float HalfZ = FMath::Max(Height * 0.5f - RoomRounding, 0.25f);
                const float CenterZ = (Landing->FloorZ + Landing->CeilingZ) * 0.5f;
                if (!AddDomain(
                        FVector(Landing->StandingPoint.X - HalfX - RoomRounding,
                                Landing->StandingPoint.Y - HalfX - RoomRounding,
                                FMath::Min(Landing->FloorZ,
                                           CenterZ - HalfZ - RoomRounding)),
                        FVector(Landing->StandingPoint.X + HalfX + RoomRounding,
                                Landing->StandingPoint.Y + HalfX + RoomRounding,
                                FMath::Max(Landing->CeilingZ,
                                           CenterZ + HalfZ + RoomRounding))))
                {
                    return false;
                }
            }
        }
        return true;
    }

    static bool VF_AnyPassageModifierDomainTouchesLattice(
        const TArray<FVoxelPassage>& Passages,
        const FBox& VoxelBox, const FIntVector& LatticeOrigin, int32 Step)
    {
        if (Step <= 0 || !VoxelBox.IsValid)
        {
            // An invalid query must never manufacture an identity proof.
            return true;
        }

        // EvaluateModifierSDF smooth-mins every finite primitive in passage order. The polynomial
        // smooth-min can dip below the smallest raw term by at most K/6 per combine (K=3 here).
        // Build a conservative AABB for each actual tube/landing primitive, then pad the union by
        // the worst finite dip. This is materially tighter than Passage.BoundRadius for a long,
        // thin inter-strate route, while still being a pure reject gate: a miss proves only that
        // the modifier cannot be < 4 at any MC lattice point. Malformed data remains Unknown.
        constexpr float CarveThreshold = 4.0f;
        constexpr float SmoothMinK = 3.0f;
        TArray<FVoxelPassageModifierDomain, TInlineAllocator<128>> Domains;
        if (!VF_BuildPassageModifierDomains(Passages, Domains))
        {
            return true;
        }
        if (Domains.Num() == 0)
        {
            return false;
        }
        const int64 DomainCount = Domains.Num();
        const float SmoothDip = static_cast<float>(DomainCount - 1)
            * (SmoothMinK / 6.0f);
        const float Padding = CarveThreshold + SmoothDip;
        if (!VoxelMath::IsFinite(Padding) || Padding < 0.0f)
        {
            return true;
        }
        for (const FVoxelPassageModifierDomain& Domain : Domains)
        {
            if (VF_LatticeBoxTouchesAABB(
                    VoxelBox, LatticeOrigin, Step,
                    Domain.Min.X - Padding, Domain.Max.X + Padding,
                    Domain.Min.Y - Padding, Domain.Max.Y + Padding,
                    Domain.Min.Z - Padding, Domain.Max.Z + Padding))
            {
                return true;
            }
        }
        return false;
    }

    static FORCEINLINE float VF_DistanceToSegmentLowerBound(
        const FVector& Position, const FVector& A, const FVector& B, float Radius)
    {
        const FVector AB = B - A;
        const FVector AP = Position - A;
        const float LenSq = FVector::DotProduct(AB, AB);
        const float T = LenSq > KINDA_SMALL_NUMBER
            ? FMath::Clamp(FVector::DotProduct(AP, AB) / LenSq, 0.0f, 1.0f)
            : 0.0f;
        const FVector Closest = A + AB * T;
        return FVector::Dist(Position, Closest) - Radius;
    }

    static bool VF_PointMayHavePassageCarve(
        const TArray<FVoxelPassage>& Passages,
        const FVector& Position)
    {
        if (!VoxelMath::IsFinite(Position.X) || !VoxelMath::IsFinite(Position.Y)
            || !VoxelMath::IsFinite(Position.Z))
        {
            return true;
        }

        constexpr float BlendK = 3.0f;
        constexpr float CarveThreshold = 4.0f;
        constexpr float NumericalSafetyMargin = 1.0e-3f;
        float ModifierLowerBound = FLT_MAX;
        for (const FVoxelPassage& Passage : Passages)
        {
            if (!VoxelMath::IsFinite(Passage.BoundCenter.X)
                || !VoxelMath::IsFinite(Passage.BoundCenter.Y)
                || !VoxelMath::IsFinite(Passage.BoundCenter.Z)
                || !VoxelMath::IsFinite(Passage.BoundRadius)
                || !VoxelMath::IsFinite(Passage.BoundRadiusSq)
                || Passage.BoundRadius < 0.0f || Passage.BoundRadiusSq < 0.0f)
            {
                return true;
            }
            if (FVector::DistSquared(Position, Passage.BoundCenter) > Passage.BoundRadiusSq)
            {
                continue;
            }

            float PassageLowerBound = FLT_MAX;
            if (Passage.ControlPoints.Num() >= 2)
            {
                if (Passage.ControlPoints.Num() > 4096)
                {
                    return true;
                }
                const bool bTaper = Passage.ControlRadii.Num() == Passage.ControlPoints.Num();
                if (!bTaper && !VoxelMath::IsFinite(Passage.Radius))
                {
                    return true;
                }
                for (int32 PointIndex = 0;
                     PointIndex < Passage.ControlPoints.Num(); ++PointIndex)
                {
                    const FVector& Point = Passage.ControlPoints[PointIndex];
                    if (!VoxelMath::IsFinite(Point.X) || !VoxelMath::IsFinite(Point.Y)
                        || !VoxelMath::IsFinite(Point.Z))
                    {
                        return true;
                    }
                    if (bTaper && !VoxelMath::IsFinite(Passage.ControlRadii[PointIndex]))
                    {
                        return true;
                    }
                }
                for (int32 SegmentIndex = 0;
                     SegmentIndex + 1 < Passage.ControlPoints.Num(); ++SegmentIndex)
                {
                    const FVector& A = Passage.ControlPoints[SegmentIndex];
                    const FVector& B = Passage.ControlPoints[SegmentIndex + 1];
                    const float RadiusA = bTaper
                        ? Passage.ControlRadii[SegmentIndex] : Passage.Radius;
                    const float RadiusB = bTaper
                        ? Passage.ControlRadii[SegmentIndex + 1] : Passage.Radius;
                    const float Radius = FMath::Max(FMath::Abs(RadiusA), FMath::Abs(RadiusB));
                    const float SegmentLowerBound = VF_DistanceToSegmentLowerBound(
                        Position, A, B, Radius);
                    if (!VoxelMath::IsFinite(SegmentLowerBound))
                    {
                        return true;
                    }
                    PassageLowerBound = VoxelSDF::SmoothMin(
                        PassageLowerBound, SegmentLowerBound, BlendK);
                }
            }
            else
            {
                if (!VoxelMath::IsFinite(Passage.UpperPoint.X)
                    || !VoxelMath::IsFinite(Passage.UpperPoint.Y)
                    || !VoxelMath::IsFinite(Passage.UpperPoint.Z)
                    || !VoxelMath::IsFinite(Passage.LowerPoint.X)
                    || !VoxelMath::IsFinite(Passage.LowerPoint.Y)
                    || !VoxelMath::IsFinite(Passage.LowerPoint.Z)
                    || !VoxelMath::IsFinite(Passage.Radius))
                {
                    return true;
                }
                PassageLowerBound = VF_DistanceToSegmentLowerBound(
                    Position, Passage.UpperPoint, Passage.LowerPoint,
                    FMath::Abs(Passage.Radius));
                if (!VoxelMath::IsFinite(PassageLowerBound))
                {
                    return true;
                }
            }

            const FVoxelPassageLanding* Landings[] = {
                &Passage.UpperLanding, &Passage.LowerLanding };
            for (const FVoxelPassageLanding* Landing : Landings)
            {
                if (!VoxelMath::IsFinite(Landing->StandingPoint.X)
                    || !VoxelMath::IsFinite(Landing->StandingPoint.Y)
                    || !VoxelMath::IsFinite(Landing->FloorZ)
                    || !VoxelMath::IsFinite(Landing->CeilingZ)
                    || !VoxelMath::IsFinite(Landing->HalfWidth))
                {
                    return true;
                }
                if (Landing->HalfWidth <= 0.0f
                    || Landing->CeilingZ <= Landing->FloorZ)
                {
                    continue;
                }
                const float LandingSDF = VF_EvaluatePassageLandingSDF(Position, *Landing);
                if (!VoxelMath::IsFinite(LandingSDF))
                {
                    return true;
                }
                PassageLowerBound = VoxelSDF::SmoothMin(
                    PassageLowerBound, LandingSDF, BlendK);
            }

            ModifierLowerBound = VoxelSDF::SmoothMin(
                ModifierLowerBound, PassageLowerBound, BlendK);
        }
        return !VoxelMath::IsFinite(ModifierLowerBound)
            || ModifierLowerBound < CarveThreshold + NumericalSafetyMargin;
    }

    static float VF_DistanceBetweenAABBs(
        const FVector& MinA, const FVector& MaxA,
        const FVector& MinB, const FVector& MaxB)
    {
        const float DX = MaxA.X < MinB.X ? MinB.X - MaxA.X
            : MaxB.X < MinA.X ? MinA.X - MaxB.X : 0.0f;
        const float DY = MaxA.Y < MinB.Y ? MinB.Y - MaxA.Y
            : MaxB.Y < MinA.Y ? MinA.Y - MaxB.Y : 0.0f;
        const float DZ = MaxA.Z < MinB.Z ? MinB.Z - MaxA.Z
            : MaxB.Z < MinA.Z ? MinA.Z - MaxB.Z : 0.0f;
        return FMath::Sqrt(DX * DX + DY * DY + DZ * DZ);
    }

    // Exact minimum distance between a finite segment and an axis-aligned box. The squared
    // distance to a box is piecewise quadratic along the segment; the only breakpoints are where
    // one segment coordinate crosses one of the six box planes. This is a small, allocation-free
    // lower-bound primitive for the passage proof, not a change to the runtime SDF evaluator.
    static bool VF_SegmentDistanceToBox(
        const FVector& A, const FVector& B, const FBox& Box, float& OutDistance)
    {
        if (!VoxelMath::IsFinite(A.X) || !VoxelMath::IsFinite(A.Y) || !VoxelMath::IsFinite(A.Z)
            || !VoxelMath::IsFinite(B.X) || !VoxelMath::IsFinite(B.Y) || !VoxelMath::IsFinite(B.Z)
            || !Box.IsValid)
        {
            return false;
        }

        const FVector D = B - A;
        float Breaks[8] = { 0.0f, 1.0f };
        int32 BreakCount = 2;
        const float AValues[3] = { A.X, A.Y, A.Z };
        const float DValues[3] = { D.X, D.Y, D.Z };
        const float BoxMinValues[3] = {
            (float)Box.Min.X, (float)Box.Min.Y, (float)Box.Min.Z };
        const float BoxMaxValues[3] = {
            (float)Box.Max.X, (float)Box.Max.Y, (float)Box.Max.Z };
        for (int32 Axis = 0; Axis < 3; ++Axis)
        {
            // This is a lower-bound proof.  A nonzero direction, however small, can cross a
            // box plane and reduce the segment distance; treating it as stationary would make
            // the bound too high and could incorrectly skip geometry.
            if (DValues[Axis] == 0.0f)
            {
                continue;
            }
            const float TMin = (BoxMinValues[Axis] - AValues[Axis]) / DValues[Axis];
            const float TMax = (BoxMaxValues[Axis] - AValues[Axis]) / DValues[Axis];
            if (TMin > 0.0f && TMin < 1.0f && BreakCount < UE_ARRAY_COUNT(Breaks))
            {
                Breaks[BreakCount++] = TMin;
            }
            if (TMax > 0.0f && TMax < 1.0f && BreakCount < UE_ARRAY_COUNT(Breaks))
            {
                Breaks[BreakCount++] = TMax;
            }
        }
        for (int32 I = 1; I < BreakCount; ++I)
        {
            const float Value = Breaks[I];
            int32 J = I - 1;
            while (J >= 0 && Breaks[J] > Value)
            {
                Breaks[J + 1] = Breaks[J];
                --J;
            }
            Breaks[J + 1] = Value;
        }

        float MinimumDistanceSquared = FLT_MAX;
        for (int32 Interval = 0; Interval + 1 < BreakCount; ++Interval)
        {
            const float T0 = Breaks[Interval];
            const float T1 = Breaks[Interval + 1];
            const float TM = 0.5f * (T0 + T1);
            float QuadraticA = 0.0f;
            float QuadraticB = 0.0f;
            float QuadraticC = 0.0f;
            for (int32 Axis = 0; Axis < 3; ++Axis)
            {
                const float Mid = AValues[Axis] + DValues[Axis] * TM;
                float Offset = 0.0f;
                if (Mid < BoxMinValues[Axis])
                {
                    Offset = AValues[Axis] - BoxMinValues[Axis];
                }
                else if (Mid > BoxMaxValues[Axis])
                {
                    Offset = AValues[Axis] - BoxMaxValues[Axis];
                }
                else
                {
                    continue;
                }
                QuadraticA += DValues[Axis] * DValues[Axis];
                QuadraticB += 2.0f * Offset * DValues[Axis];
                QuadraticC += Offset * Offset;
            }
            float T = T0;
            if (QuadraticA > 0.0f)
            {
                T = FMath::Clamp(
                    -QuadraticB / (2.0f * QuadraticA), T0, T1);
            }
            const float DistanceSquared = FMath::Max(
                0.0f,
                (QuadraticA * T + QuadraticB) * T + QuadraticC);
            MinimumDistanceSquared = FMath::Min(
                MinimumDistanceSquared, DistanceSquared);
        }
        if (!VoxelMath::IsFinite(MinimumDistanceSquared))
        {
            return false;
        }
        OutDistance = FMath::Sqrt(MinimumDistanceSquared);
        return VoxelMath::IsFinite(OutDistance);
    }

    static bool VF_LandingLowerBoundOverBox(
        const FVoxelPassageLanding& Landing, const FBox& Box, float& OutLowerBound)
    {
        if (!VoxelMath::IsFinite(Landing.StandingPoint.X)
            || !VoxelMath::IsFinite(Landing.StandingPoint.Y)
            || !VoxelMath::IsFinite(Landing.FloorZ)
            || !VoxelMath::IsFinite(Landing.CeilingZ)
            || !VoxelMath::IsFinite(Landing.HalfWidth)
            || !Box.IsValid)
        {
            return false;
        }
        if (Landing.HalfWidth <= 0.0f || Landing.CeilingZ <= Landing.FloorZ)
        {
            OutLowerBound = FLT_MAX;
            return true;
        }

        constexpr float RoomRounding = 1.5f;
        const float Height = Landing.CeilingZ - Landing.FloorZ;
        const FVector Center(
            Landing.StandingPoint.X, Landing.StandingPoint.Y,
            (Landing.FloorZ + Landing.CeilingZ) * 0.5f);
        const FVector HalfExtent(
            FMath::Max(Landing.HalfWidth - RoomRounding, 0.25f),
            FMath::Max(Landing.HalfWidth - RoomRounding, 0.25f),
            FMath::Max(Height * 0.5f - RoomRounding, 0.25f));
        const FVector OuterMin = Center - HalfExtent - FVector(RoomRounding);
        const FVector OuterMax = Center + HalfExtent + FVector(RoomRounding);
        const FVector BoxMin((float)Box.Min.X, (float)Box.Min.Y, (float)Box.Min.Z);
        const FVector BoxMax((float)Box.Max.X, (float)Box.Max.Y, (float)Box.Max.Z);
        const float OuterDistance = VF_DistanceBetweenAABBs(
            BoxMin, BoxMax, OuterMin, OuterMax);
        const float RoundedBoxLower = OuterDistance > 0.0f
            ? OuterDistance
            : -FMath::Max(HalfExtent.X,
                FMath::Max(HalfExtent.Y, HalfExtent.Z)) - RoomRounding;
        OutLowerBound = FMath::Max(
            RoundedBoxLower, Landing.FloorZ - (float)Box.Max.Z);
        return VoxelMath::IsFinite(OutLowerBound);
    }

    static bool VF_PassageModifierLowerBoundOverBox(
        const TArray<FVoxelPassage>& Passages,
        const FBox& Box, float& OutLowerBound)
    {
        if (!Box.IsValid)
        {
            return false;
        }

        constexpr float BlendK = 3.0f;
        float ModifierLowerBound = FLT_MAX;
        const FVector BoxMin((float)Box.Min.X, (float)Box.Min.Y, (float)Box.Min.Z);
        const FVector BoxMax((float)Box.Max.X, (float)Box.Max.Y, (float)Box.Max.Z);
        for (const FVoxelPassage& Passage : Passages)
        {
            if (!VoxelMath::IsFinite(Passage.BoundCenter.X)
                || !VoxelMath::IsFinite(Passage.BoundCenter.Y)
                || !VoxelMath::IsFinite(Passage.BoundCenter.Z)
                || !VoxelMath::IsFinite(Passage.BoundRadius)
                || !VoxelMath::IsFinite(Passage.BoundRadiusSq)
                || Passage.BoundRadius < 0.0f || Passage.BoundRadiusSq < 0.0f)
            {
                return false;
            }
            const FVector ClosestPoint(
                FMath::Clamp(Passage.BoundCenter.X, BoxMin.X, BoxMax.X),
                FMath::Clamp(Passage.BoundCenter.Y, BoxMin.Y, BoxMax.Y),
                FMath::Clamp(Passage.BoundCenter.Z, BoxMin.Z, BoxMax.Z));
            if (FVector::DistSquared(ClosestPoint, Passage.BoundCenter)
                > Passage.BoundRadiusSq)
            {
                continue;
            }

            float PassageLowerBound = FLT_MAX;
            if (Passage.ControlPoints.Num() >= 2)
            {
                if (Passage.ControlPoints.Num() > 4096)
                {
                    return false;
                }
                const bool bTaper = Passage.ControlRadii.Num() == Passage.ControlPoints.Num();
                if (!bTaper && !VoxelMath::IsFinite(Passage.Radius))
                {
                    return false;
                }
                for (int32 PointIndex = 0;
                     PointIndex < Passage.ControlPoints.Num(); ++PointIndex)
                {
                    const FVector& Point = Passage.ControlPoints[PointIndex];
                    if (!VoxelMath::IsFinite(Point.X) || !VoxelMath::IsFinite(Point.Y)
                        || !VoxelMath::IsFinite(Point.Z)
                        || (bTaper && !VoxelMath::IsFinite(Passage.ControlRadii[PointIndex])))
                    {
                        return false;
                    }
                }
                for (int32 SegmentIndex = 0;
                     SegmentIndex + 1 < Passage.ControlPoints.Num(); ++SegmentIndex)
                {
                    const float RadiusA = bTaper
                        ? Passage.ControlRadii[SegmentIndex] : Passage.Radius;
                    const float RadiusB = bTaper
                        ? Passage.ControlRadii[SegmentIndex + 1] : Passage.Radius;
                    const float Radius = FMath::Max(FMath::Abs(RadiusA), FMath::Abs(RadiusB));
                    float SegmentDistance = 0.0f;
                    if (!VoxelMath::IsFinite(Radius)
                        || !VF_SegmentDistanceToBox(
                            Passage.ControlPoints[SegmentIndex],
                            Passage.ControlPoints[SegmentIndex + 1], Box, SegmentDistance))
                    {
                        return false;
                    }
                    PassageLowerBound = VoxelSDF::SmoothMin(
                        PassageLowerBound, SegmentDistance - Radius, BlendK);
                }
            }
            else
            {
                if (!VoxelMath::IsFinite(Passage.UpperPoint.X)
                    || !VoxelMath::IsFinite(Passage.UpperPoint.Y)
                    || !VoxelMath::IsFinite(Passage.UpperPoint.Z)
                    || !VoxelMath::IsFinite(Passage.LowerPoint.X)
                    || !VoxelMath::IsFinite(Passage.LowerPoint.Y)
                    || !VoxelMath::IsFinite(Passage.LowerPoint.Z)
                    || !VoxelMath::IsFinite(Passage.Radius))
                {
                    return false;
                }
                float SegmentDistance = 0.0f;
                if (!VF_SegmentDistanceToBox(
                        Passage.UpperPoint, Passage.LowerPoint, Box, SegmentDistance))
                {
                    return false;
                }
                PassageLowerBound = SegmentDistance - FMath::Abs(Passage.Radius);
            }

            const FVoxelPassageLanding* Landings[] = {
                &Passage.UpperLanding, &Passage.LowerLanding };
            for (const FVoxelPassageLanding* Landing : Landings)
            {
                float LandingLowerBound = FLT_MAX;
                if (!VF_LandingLowerBoundOverBox(
                        *Landing, Box, LandingLowerBound))
                {
                    return false;
                }
                PassageLowerBound = VoxelSDF::SmoothMin(
                    PassageLowerBound, LandingLowerBound, BlendK);
            }
            ModifierLowerBound = VoxelSDF::SmoothMin(
                ModifierLowerBound, PassageLowerBound, BlendK);
        }
        OutLowerBound = ModifierLowerBound;
        return VoxelMath::IsFinite(OutLowerBound);
    }

    // MaxPassageCarveFactorNearLattice is queried once for the root and once for every refined
    // child.  The child lattice is a strict subset of the root lattice, so retaining the exact
    // carve factor at each root point is a sound memo: a child reads the same values the old
    // implementation would have recomputed, in the same deterministic order.  The cache is
    // worker-local because classifier calls can run concurrently and no mutable state may be
    // published through the immutable strate manager.
    struct FPassageLatticeBoundCache
    {
        const UVoxelStrateManager* Owner = nullptr;
        uint64 OwnerLifetimeId = 0;
        uint32 Version = 0xFFFFFFFFu;
        FIntVector Origin = FIntVector::ZeroValue;
        int32 Step = 0;
        int32 FirstX = 0, LastX = -1;
        int32 FirstY = 0, LastY = -1;
        int32 FirstZ = 0, LastZ = -1;
        bool bValid = false;
        bool bNoCandidate = false;
        TArray<float> Factors;

        void Reset()
        {
            Owner = nullptr;
            OwnerLifetimeId = 0;
            Version = 0xFFFFFFFFu;
            Origin = FIntVector::ZeroValue;
            Step = 0;
            FirstX = 0; LastX = -1;
            FirstY = 0; LastY = -1;
            FirstZ = 0; LastZ = -1;
            bValid = false;
            bNoCandidate = false;
            Factors.Reset();
        }

        void Begin(const UVoxelStrateManager* InOwner, uint64 InLifetimeId,
                   uint32 InVersion, const FIntVector& InOrigin, int32 InStep,
                   int32 InFirstX, int32 InLastX,
                   int32 InFirstY, int32 InLastY,
                   int32 InFirstZ, int32 InLastZ,
                   bool bInNoCandidate)
        {
            Owner = InOwner;
            OwnerLifetimeId = InLifetimeId;
            Version = InVersion;
            Origin = InOrigin;
            Step = InStep;
            FirstX = InFirstX; LastX = InLastX;
            FirstY = InFirstY; LastY = InLastY;
            FirstZ = InFirstZ; LastZ = InLastZ;
            bValid = true;
            bNoCandidate = bInNoCandidate;
            Factors.Reset();
        }

        bool Contains(const UVoxelStrateManager* InOwner, uint64 InLifetimeId,
                      uint32 InVersion, const FIntVector& InOrigin, int32 InStep,
                      int32 InFirstX, int32 InLastX,
                      int32 InFirstY, int32 InLastY,
                      int32 InFirstZ, int32 InLastZ) const
        {
            return bValid && Owner == InOwner && OwnerLifetimeId == InLifetimeId
                && Version == InVersion && Origin == InOrigin && Step == InStep
                && InFirstX >= FirstX && InLastX <= LastX
                && InFirstY >= FirstY && InLastY <= LastY
                && InFirstZ >= FirstZ && InLastZ <= LastZ;
        }

        int32 Index(int32 IX, int32 IY, int32 IZ) const
        {
            const int32 DimX = LastX - FirstX + 1;
            const int32 DimY = LastY - FirstY + 1;
            return ((IZ - FirstZ) * DimY + (IY - FirstY)) * DimX + (IX - FirstX);
        }

        float MaxIn(const int32 InFirstX, const int32 InLastX,
                    const int32 InFirstY, const int32 InLastY,
                    const int32 InFirstZ, const int32 InLastZ) const
        {
            if (bNoCandidate) { return 0.0f; }

            float Result = 0.0f;
            for (int32 IZ = InFirstZ; IZ <= InLastZ; ++IZ)
            {
                for (int32 IY = InFirstY; IY <= InLastY; ++IY)
                {
                    for (int32 IX = InFirstX; IX <= InLastX; ++IX)
                    {
                        Result = FMath::Max(Result, Factors[Index(IX, IY, IZ)]);
                        if (Result >= 1.0f)
                        {
                            return Result;
                        }
                    }
                }
            }
            return Result;
        }
    };

    FPassageLatticeBoundCache& VF_GetPassageLatticeBoundCache()
    {
        thread_local FPassageLatticeBoundCache Cache;
        return Cache;
    }

    static bool VF_LatticeBoxTouchesAABB(
        const FBox& Box, const FIntVector& Origin, int32 Step,
        float MinX, float MaxX, float MinY, float MaxY, float MinZ, float MaxZ)
    {
        return VoxelPassageGeometry::LatticeAxisHasSampleInInterval(
                   FMath::Max((float)Box.Min.X, MinX),
                   FMath::Min((float)Box.Max.X, MaxX),
                   (float)Origin.X, Step)
            && VoxelPassageGeometry::LatticeAxisHasSampleInInterval(
                   FMath::Max((float)Box.Min.Y, MinY),
                   FMath::Min((float)Box.Max.Y, MaxY),
                   (float)Origin.Y, Step)
            && VoxelPassageGeometry::LatticeAxisHasSampleInInterval(
                   FMath::Max((float)Box.Min.Z, MinZ),
                   FMath::Min((float)Box.Max.Z, MaxZ),
                   (float)Origin.Z, Step);
    }
}

bool UVoxelStrateManager::AnyPassageNearLattice(
    const FBox& VoxelBox, const FIntVector& LatticeOrigin, int32 Step) const
{
    // This query feeds FPassageCarveOp::EffectOverBox, so a spatial candidate is not enough:
    // returning true kills the AllSolid hypothesis even when the exact MC samples all have a
    // zero carve factor.  That was the LOD0 false-Mixed path. Keep the cheap domain test as the
    // common reject, then use the same exact lattice factor that MaxCarveOverBox consumes. The
    // manager cache makes the second call in the fold free for the same box.
    if (!VF_AnyPassageModifierDomainTouchesLattice(
            Passages, VoxelBox, LatticeOrigin, Step))
    {
        return false;
    }

    const float MaxFactor = MaxPassageCarveFactorNearLattice(
        VoxelBox, LatticeOrigin, Step);
    // Invalid input deliberately returns 1.0 from the exact helper. A non-finite result is also
    // retained as a candidate rather than becoming an identity proof.
    return !VoxelMath::IsFinite(MaxFactor) || MaxFactor > 0.0f;
}

bool UVoxelStrateManager::AnyPassageAirPostNearLattice(
    const FBox& VoxelBox, const FIntVector& LatticeOrigin, int32 Step,
    float BaseDensity, float SealThickness) const
{
    // FPassageCarveOp already evaluates the complete passage modifier in the stack.  The only
    // passage work still outside that proof is the post-disturbance landing/tunnel-air backstop.
    // Do not use the enclosing passage sphere as its candidate: that sphere is deliberately much
    // wider than a walkable core and was the false-positive that made LOD0 all-solid tiles recurse.
    if (Step <= 0 || !VoxelBox.IsValid
        || VoxelPassageGeometry::VerticalShaftConnectorAirMarker())
    {
        return true;
    }
    if (!VoxelMath::IsFinite(BaseDensity) || !(BaseDensity > 0.0f)
        || !VoxelMath::IsFinite(SealThickness) || SealThickness < 0.0f)
    {
        return true;
    }

    int32 FirstX = 0, LastX = -1;
    int32 FirstY = 0, LastY = -1;
    int32 FirstZ = 0, LastZ = -1;
    if (!VF_LatticeAxisRange(
            VoxelBox.Min.X, VoxelBox.Max.X, (float)LatticeOrigin.X, Step, FirstX, LastX)
        || !VF_LatticeAxisRange(
            VoxelBox.Min.Y, VoxelBox.Max.Y, (float)LatticeOrigin.Y, Step, FirstY, LastY)
        || !VF_LatticeAxisRange(
            VoxelBox.Min.Z, VoxelBox.Max.Z, (float)LatticeOrigin.Z, Step, FirstZ, LastZ))
    {
        return true;
    }

    const int64 DimX = (int64)LastX - FirstX + 1;
    const int64 DimY = (int64)LastY - FirstY + 1;
    const int64 DimZ = (int64)LastZ - FirstZ + 1;
    const int64 SampleCount = DimX * DimY * DimZ;
    // This query is a proof helper, not a second unbounded density grid.  An unusual external
    // caller gets Unknown/candidate and therefore keeps the normal mesher path.
    if (SampleCount <= 0 || SampleCount > 65536)
    {
        return true;
    }

    // The tunnel-air post is a vertical prism over the nearest horizontal control segment.  Its
    // existence on a lattice does not require a 3-D walk: project each XY lattice point once,
    // then ask whether the resulting floor interval contains a Z lattice sample.  The interval is
    // deliberately closed at both ends here, so strict predicate boundaries can only create a
    // false candidate; they can never hide a real air sample.
    constexpr int32 MaxTunnelControlPointsForQuery = 4096;
    for (const FVoxelPassage& Passage : Passages)
    {
        if (!Passage.bWalkableTunnelContract)
        {
            continue;
        }
        if (Passage.ControlPoints.Num() < 2
            || Passage.ControlRadii.Num() != Passage.ControlPoints.Num()
            || Passage.ControlPoints.Num() > MaxTunnelControlPointsForQuery)
        {
            return true;
        }
        for (int32 PointIndex = 0;
             PointIndex < Passage.ControlPoints.Num(); ++PointIndex)
        {
            const FVector& Point = Passage.ControlPoints[PointIndex];
            if (!VoxelMath::IsFinite(Point.X) || !VoxelMath::IsFinite(Point.Y)
                || !VoxelMath::IsFinite(Point.Z)
                || !VoxelMath::IsFinite(Passage.ControlRadii[PointIndex]))
            {
                return true;
            }
        }

        for (int32 IY = FirstY; IY <= LastY; ++IY)
        {
            const float Y = (float)LatticeOrigin.Y + (float)(IY * Step);
            for (int32 IX = FirstX; IX <= LastX; ++IX)
            {
                const FVector Position(
                    (float)LatticeOrigin.X + (float)(IX * Step), Y, 0.0f);
                float FloorZ = 0.0f;
                float SupportRadius = 0.0f;
                if (!VoxelPassageGeometry::ProjectWalkableTunnelFloor(
                        Passage.ControlPoints, Passage.ControlRadii, Position,
                        FloorZ, SupportRadius))
                {
                    continue;
                }
                const float AirMinZ = FMath::Max(
                    (float)VoxelBox.Min.Z,
                    FloorZ + VoxelPassageGeometry::WalkableTunnelFloorAirClearanceVoxels);
                const float AirMaxZ = FMath::Min(
                    (float)VoxelBox.Max.Z,
                    FloorZ + 2.0f * SupportRadius);
                if (VoxelPassageGeometry::LatticeAxisHasSampleInInterval(
                        AirMinZ, AirMaxZ,
                        (float)LatticeOrigin.Z, Step))
                {
                    return true;
                }
            }
        }
    }

    constexpr float LandingBlendRadius = 4.0f;
    TArray<int32, TInlineAllocator<32>> Candidates;
    struct FPostPotentialDomain
    {
        int32 PassageIndex = INDEX_NONE;
        FVector Min = FVector(FLT_MAX, FLT_MAX, FLT_MAX);
        FVector Max = FVector(-FLT_MAX, -FLT_MAX, -FLT_MAX);
    };
    TArray<FPostPotentialDomain, TInlineAllocator<64>> PotentialDomains;
    float PotentialMinX = FLT_MAX;
    float PotentialMinY = FLT_MAX;
    float PotentialMinZ = FLT_MAX;
    float PotentialMaxX = -FLT_MAX;
    float PotentialMaxY = -FLT_MAX;
    float PotentialMaxZ = -FLT_MAX;

    // BoundCenter/BoundRadius encloses the whole passage, which is intentionally much larger
    // than the post writers. Using that sphere here made every LOD0 child along a long route
    // walk its complete 17^3 lattice even when the route's landing/tunnel-air AABBs were many
    // chunks away. Build a conservative union of the actual post domains instead. A point
    // outside these AABBs cannot satisfy either VF_EvaluatePassageLandingSDF or
    // VF_IsWalkableTunnelAir; the exact predicates below still decide every point inside.
    auto AddPotentialAABB = [&](int32 PassageIndex,
                                float MinX, float MaxX, float MinY, float MaxY,
                                float MinZ, float MaxZ, bool& bTouches) -> bool
    {
        if (!VoxelMath::IsFinite(MinX) || !VoxelMath::IsFinite(MaxX)
            || !VoxelMath::IsFinite(MinY) || !VoxelMath::IsFinite(MaxY)
            || !VoxelMath::IsFinite(MinZ) || !VoxelMath::IsFinite(MaxZ)
            || MinX > MaxX || MinY > MaxY || MinZ > MaxZ)
        {
            return false;
        }
        if (VF_LatticeBoxTouchesAABB(
                VoxelBox, LatticeOrigin, Step,
                MinX, MaxX, MinY, MaxY, MinZ, MaxZ))
        {
            bTouches = true;
            PotentialMinX = FMath::Min(PotentialMinX, MinX);
            PotentialMinY = FMath::Min(PotentialMinY, MinY);
            PotentialMinZ = FMath::Min(PotentialMinZ, MinZ);
            PotentialMaxX = FMath::Max(PotentialMaxX, MaxX);
            PotentialMaxY = FMath::Max(PotentialMaxY, MaxY);
            PotentialMaxZ = FMath::Max(PotentialMaxZ, MaxZ);
            PotentialDomains.Add({
                PassageIndex,
                FVector(MinX, MinY, MinZ), FVector(MaxX, MaxY, MaxZ) });
        }
        return true;
    };

    for (int32 PassageIndex = 0; PassageIndex < Passages.Num(); ++PassageIndex)
    {
        const FVoxelPassage& Passage = Passages[PassageIndex];
        if (!VoxelMath::IsFinite(Passage.BoundCenter.X)
            || !VoxelMath::IsFinite(Passage.BoundCenter.Y)
            || !VoxelMath::IsFinite(Passage.BoundCenter.Z)
            || !VoxelMath::IsFinite(Passage.BoundRadius)
            || !VoxelMath::IsFinite(Passage.BoundRadiusSq)
            || Passage.BoundRadius < 0.0f || Passage.BoundRadiusSq < 0.0f)
        {
            return true;
        }

        bool bPassageTouches = false;
        const FVoxelPassageLanding* Landings[] = {
            &Passage.UpperLanding, &Passage.LowerLanding };
        for (const FVoxelPassageLanding* Landing : Landings)
        {
            // A passage such as the optional surface entry has no landing post and therefore
            // leaves this descriptor at its zero-valued default. VF_EvaluatePassageLandingSDF
            // treats that descriptor as FLT_MAX (the writer is a known no-op), so it must not
            // turn the whole tile into an Unknown/candidate. FloorThickness is intentionally
            // not part of this validity check: the landing-air writer reads the SDF fields even
            // when a separate floor descriptor is degenerate.
            const bool bLandingSdfValid =
                VoxelMath::IsFinite(Landing->StandingPoint.X)
                && VoxelMath::IsFinite(Landing->StandingPoint.Y)
                && VoxelMath::IsFinite(Landing->FloorZ)
                && VoxelMath::IsFinite(Landing->CeilingZ)
                && VoxelMath::IsFinite(Landing->HalfWidth)
                && Landing->HalfWidth > 0.0f
                && Landing->CeilingZ > Landing->FloorZ;
            if (bLandingSdfValid)
            {
                const float HalfExtent = Landing->HalfWidth + LandingBlendRadius;
                if (!AddPotentialAABB(
                        PassageIndex,
                        Landing->StandingPoint.X - HalfExtent,
                        Landing->StandingPoint.X + HalfExtent,
                        Landing->StandingPoint.Y - HalfExtent,
                        Landing->StandingPoint.Y + HalfExtent,
                        Landing->FloorZ - LandingBlendRadius,
                        Landing->CeilingZ + LandingBlendRadius,
                        bPassageTouches))
                {
                    return true;
                }
            }
        }

        if (bPassageTouches)
        {
            Candidates.Add(PassageIndex);
        }
    }
    if (Candidates.Num() == 0)
    {
        return false;
    }

    // The union above is conservative but can be much smaller than the queried box. Restrict the
    // exact walk to its lattice intersection; a false positive still pays the exact predicates,
    // while a far-away long-passage sphere no longer does.
    if (!VF_LatticeAxisRange(
            FMath::Max((float)VoxelBox.Min.X, PotentialMinX),
            FMath::Min((float)VoxelBox.Max.X, PotentialMaxX),
            (float)LatticeOrigin.X, Step, FirstX, LastX)
        || !VF_LatticeAxisRange(
            FMath::Max((float)VoxelBox.Min.Y, PotentialMinY),
            FMath::Min((float)VoxelBox.Max.Y, PotentialMaxY),
            (float)LatticeOrigin.Y, Step, FirstY, LastY)
        || !VF_LatticeAxisRange(
            FMath::Max((float)VoxelBox.Min.Z, PotentialMinZ),
            FMath::Min((float)VoxelBox.Max.Z, PotentialMaxZ),
            (float)LatticeOrigin.Z, Step, FirstZ, LastZ))
    {
        return false;
    }

    const int64 PotentialSampleCount = ((int64)LastX - FirstX + 1)
        * ((int64)LastY - FirstY + 1)
        * ((int64)LastZ - FirstZ + 1);
    if (PotentialSampleCount <= 0 || PotentialSampleCount > 65536)
    {
        return true;
    }

    const float AirTarget = -(BaseDensity * 2.0f + SealThickness + 4.0f);
    if (!VoxelMath::IsFinite(AirTarget))
    {
        return true;
    }
    for (int32 IZ = FirstZ; IZ <= LastZ; ++IZ)
    {
        const float Z = (float)LatticeOrigin.Z + (float)(IZ * Step);
        for (int32 IY = FirstY; IY <= LastY; ++IY)
        {
            const float Y = (float)LatticeOrigin.Y + (float)(IY * Step);
            for (int32 IX = FirstX; IX <= LastX; ++IX)
            {
                const FVector Position(
                    (float)LatticeOrigin.X + (float)(IX * Step), Y, Z);
                float MinLandingSDF = FLT_MAX;

                for (const int32 PassageIndex : Candidates)
                {
                    const FVoxelPassage& Passage = Passages[PassageIndex];
                    bool bInPotentialDomain = false;
                    for (const FPostPotentialDomain& Domain : PotentialDomains)
                    {
                        if (Domain.PassageIndex == PassageIndex
                            && Position.X >= Domain.Min.X && Position.X <= Domain.Max.X
                            && Position.Y >= Domain.Min.Y && Position.Y <= Domain.Max.Y
                            && Position.Z >= Domain.Min.Z && Position.Z <= Domain.Max.Z)
                        {
                            bInPotentialDomain = true;
                            break;
                        }
                    }
                    if (!bInPotentialDomain)
                    {
                        continue;
                    }
                    const float DistanceSquared =
                        FVector::DistSquared(Position, Passage.BoundCenter);
                    if (DistanceSquared > Passage.BoundRadiusSq)
                    {
                        continue;
                    }

                    const float UpperSDF = VF_EvaluatePassageLandingSDF(
                        Position, Passage.UpperLanding);
                    const float LowerSDF = VF_EvaluatePassageLandingSDF(
                        Position, Passage.LowerLanding);
                    if (!VoxelMath::IsFinite(UpperSDF) || !VoxelMath::IsFinite(LowerSDF))
                    {
                        // A malformed landing is an unknown post, never an identity proof.
                        return true;
                    }
                    MinLandingSDF = FMath::Min(
                        MinLandingSDF, FMath::Min(UpperSDF, LowerSDF));
                }

                // The floor writer can only suppress a landing-air post, so omitting that
                // exclusion here is deliberately conservative: it can retain a final-field
                // candidate, but can never prove an empty tile that owns a support floor.
                // The exact same SDF threshold as the post is sufficient for the current-density
                // interpolation: SDF < 4 is exactly its activation domain.
                if (MinLandingSDF < LandingBlendRadius)
                {
                    float CarveFactor = FMath::Clamp(
                        (LandingBlendRadius - MinLandingSDF)
                            / (LandingBlendRadius * 2.0f),
                        0.0f, 1.0f);
                    CarveFactor = SmoothStep01(CarveFactor);
                    const float LandingThreshold = FMath::Lerp(
                        BaseDensity, AirTarget, CarveFactor);
                    if (!VoxelMath::IsFinite(LandingThreshold)
                        || LandingThreshold <= 0.0f)
                    {
                        // Zero is not solid under the mesher's sign convention, so it is part of
                        // the final-field risk just like a strictly air threshold.
                        return true;
                    }
                }
            }
        }
    }
    return false;
}

float UVoxelStrateManager::MaxPassageCarveFactorNearLattice(
    const FBox& VoxelBox, const FIntVector& LatticeOrigin, int32 Step) const
{
    // The passage bound is only a cheap candidate test.  It encloses an entire multi-segment
    // passage, so using it as the final lattice proof turns a long tunnel into a false positive
    // for every tile inside its enclosing sphere.  That is particularly damaging at LOD0: the
    // classifier then falls through to a full density grid even when every sampled point is rock.
    //
    // Once a bound reaches this lattice, ask the same exact modifier SDF used by
    // ApplyPassageCarvingOnly at the actual MC samples.  This is still a proof, not a sampling
    // shortcut: the operator is evaluated at precisely the points the mesher will evaluate, and
    // a non-finite result remains conservative.  The common no-candidate path stays O(P), while a
    // false-positive bound pays one exact lattice walk and can then be classified as Identity.
    if (Step <= 0 || !VoxelBox.IsValid)
    {
        return 1.0f;
    }

    int32 FirstX = 0, LastX = -1;
    int32 FirstY = 0, LastY = -1;
    int32 FirstZ = 0, LastZ = -1;
    if (!VF_LatticeAxisRange(
            VoxelBox.Min.X, VoxelBox.Max.X, (float)LatticeOrigin.X, Step, FirstX, LastX)
        || !VF_LatticeAxisRange(
            VoxelBox.Min.Y, VoxelBox.Max.Y, (float)LatticeOrigin.Y, Step, FirstY, LastY)
        || !VF_LatticeAxisRange(
            VoxelBox.Min.Z, VoxelBox.Max.Z, (float)LatticeOrigin.Z, Step, FirstZ, LastZ))
    {
        return 1.0f;
    }

    FPassageLatticeBoundCache& Cache = VF_GetPassageLatticeBoundCache();
    const uint64 LifetimeId = GetCacheLifetimeId();
    const uint32 Version = GetLayoutVersion();
    if (Cache.Contains(
            this, LifetimeId, Version, LatticeOrigin, Step,
            FirstX, LastX, FirstY, LastY, FirstZ, LastZ))
    {
        return Cache.MaxIn(FirstX, LastX, FirstY, LastY, FirstZ, LastZ);
    }

    constexpr float PassageCarveThreshold = 4.0f;
    constexpr float SmoothMinK = 3.0f;
    float ModifierBoxLowerBound = 0.0f;
    if (VF_PassageModifierLowerBoundOverBox(
            Passages, VoxelBox, ModifierBoxLowerBound)
        && ModifierBoxLowerBound >= PassageCarveThreshold + 1.0e-3f)
    {
        Cache.Begin(
            this, LifetimeId, Version, LatticeOrigin, Step,
            FirstX, LastX, FirstY, LastY, FirstZ, LastZ,
            /*bInNoCandidate*/ true);
        return 0.0f;
    }
    TArray<FVoxelPassageModifierDomain, TInlineAllocator<128>> ModifierDomains;
    bool bKnownModifierDomains = VF_BuildPassageModifierDomains(
        Passages, ModifierDomains);
    float ModifierDomainPadding = FLT_MAX;
    bool bModifierDomainTouchesLattice = true;
    if (bKnownModifierDomains)
    {
        if (ModifierDomains.Num() == 0)
        {
            bModifierDomainTouchesLattice = false;
        }
        else
        {
            const float SmoothDip = static_cast<float>(ModifierDomains.Num() - 1)
                * (SmoothMinK / 6.0f);
            const float Padding = PassageCarveThreshold + SmoothDip;
            ModifierDomainPadding = Padding;
            if (!VoxelMath::IsFinite(Padding) || Padding < 0.0f)
            {
                bKnownModifierDomains = false;
            }
            else
            {
                bModifierDomainTouchesLattice = false;
                for (const FVoxelPassageModifierDomain& Domain : ModifierDomains)
                {
                    if (VF_LatticeBoxTouchesAABB(
                            VoxelBox, LatticeOrigin, Step,
                            Domain.Min.X - Padding, Domain.Max.X + Padding,
                            Domain.Min.Y - Padding, Domain.Max.Y + Padding,
                            Domain.Min.Z - Padding, Domain.Max.Z + Padding))
                    {
                        bModifierDomainTouchesLattice = true;
                        break;
                    }
                }
            }
        }
    }

    if (bKnownModifierDomains && !bModifierDomainTouchesLattice)
    {
        Cache.Begin(
            this, LifetimeId, Version, LatticeOrigin, Step,
            FirstX, LastX, FirstY, LastY, FirstZ, LastZ,
            /*bInNoCandidate*/ true);
        return 0.0f;
    }

    auto CarveFactorFromSDF = [](float ModifierSDF)
    {
        if (!(ModifierSDF < PassageCarveThreshold))
        {
            return 0.0f;
        }
        float CarveFactor = FMath::Clamp(
            (PassageCarveThreshold - ModifierSDF)
                / (PassageCarveThreshold * 2.0f),
            0.0f, 1.0f);
        return SmoothStep01(CarveFactor);
    };

    // A domain hit is still much wider than the tapered capsule itself.  Before paying for the
    // full modifier evaluator, use a point-level lower-bound fold over the actual primitive
    // domains.  It preserves the saturating SmoothMin behavior, so a far segment contributes no
    // artificial global K/6 penalty.  This keeps the proof exact while avoiding a 33^3 walk
    // through every segment for a tile that merely grazes a passage's old sphere.
    const auto MayEvaluatePoint = [&](const FVector& Position) -> bool
    {
        if (!bKnownModifierDomains)
        {
            return true;
        }
        bool bInsideModifierDomain = false;
        for (const FVoxelPassageModifierDomain& Domain : ModifierDomains)
        {
            if (Position.X >= Domain.Min.X - ModifierDomainPadding
                && Position.X <= Domain.Max.X + ModifierDomainPadding
                && Position.Y >= Domain.Min.Y - ModifierDomainPadding
                && Position.Y <= Domain.Max.Y + ModifierDomainPadding
                && Position.Z >= Domain.Min.Z - ModifierDomainPadding
                && Position.Z <= Domain.Max.Z + ModifierDomainPadding)
            {
                bInsideModifierDomain = true;
                break;
            }
        }
        return bInsideModifierDomain
            && VF_PointMayHavePassageCarve(Passages, Position);
    };

    // Do not sweep the complete 33^3 lattice merely because a continuous passage domain enters
    // the tile. Recursively split the sample index box and use the exact segment/box lower bound
    // above to discard whole groups. A leaf still evaluates only the original MC vertices; this is
    // a proof acceleration, not a lower-resolution sample.
    const int64 FullSampleCount =
        (static_cast<int64>(LastX) - FirstX + 1)
        * (static_cast<int64>(LastY) - FirstY + 1)
        * (static_cast<int64>(LastZ) - FirstZ + 1);
    constexpr int64 MaxRecursiveLatticeSamples = 65536;
    if (bKnownModifierDomains && FullSampleCount > 0
        && FullSampleCount <= MaxRecursiveLatticeSamples)
    {
        Cache.Begin(
            this, LifetimeId, Version, LatticeOrigin, Step,
            FirstX, LastX, FirstY, LastY, FirstZ, LastZ,
            /*bInNoCandidate*/ false);
        Cache.Factors.SetNumZeroed(static_cast<int32>(FullSampleCount));

        float MaxCarveFactor = 0.0f;
        auto EvaluatePoint = [&](int32 IX, int32 IY, int32 IZ)
        {
            const float X = (float)LatticeOrigin.X + (float)(IX * Step);
            const float Y = (float)LatticeOrigin.Y + (float)(IY * Step);
            const float Z = (float)LatticeOrigin.Z + (float)(IZ * Step);
            const FVector Position(X, Y, Z);
            if (!MayEvaluatePoint(Position))
            {
                return;
            }
            const float ModifierSDF = EvaluateModifierSDF(X, Y, Z);
            const float CarveFactor = VoxelMath::IsFinite(ModifierSDF)
                ? CarveFactorFromSDF(ModifierSDF) : 1.0f;
            Cache.Factors[Cache.Index(IX, IY, IZ)] = CarveFactor;
            MaxCarveFactor = FMath::Max(MaxCarveFactor, CarveFactor);
        };
        auto Visit = [&](auto&& Self,
                         int32 IX0, int32 IX1,
                         int32 IY0, int32 IY1,
                         int32 IZ0, int32 IZ1) -> void
        {
            const FVector NodeMin(
                (float)LatticeOrigin.X + (float)(IX0 * Step),
                (float)LatticeOrigin.Y + (float)(IY0 * Step),
                (float)LatticeOrigin.Z + (float)(IZ0 * Step));
            const FVector NodeMax(
                (float)LatticeOrigin.X + (float)(IX1 * Step),
                (float)LatticeOrigin.Y + (float)(IY1 * Step),
                (float)LatticeOrigin.Z + (float)(IZ1 * Step));
            const FBox NodeBox(NodeMin, NodeMax);
            float NodeLowerBound = 0.0f;
            if (VF_PassageModifierLowerBoundOverBox(
                    Passages, NodeBox, NodeLowerBound)
                && NodeLowerBound >= PassageCarveThreshold + 1.0e-3f)
            {
                return;
            }

            const int64 NodeSampleCount =
                (static_cast<int64>(IX1) - IX0 + 1)
                * (static_cast<int64>(IY1) - IY0 + 1)
                * (static_cast<int64>(IZ1) - IZ0 + 1);
            if (NodeSampleCount <= 64)
            {
                for (int32 IZ = IZ0; IZ <= IZ1; ++IZ)
                for (int32 IY = IY0; IY <= IY1; ++IY)
                for (int32 IX = IX0; IX <= IX1; ++IX)
                {
                    EvaluatePoint(IX, IY, IZ);
                }
                return;
            }

            const int32 SpanX = IX1 - IX0;
            const int32 SpanY = IY1 - IY0;
            const int32 SpanZ = IZ1 - IZ0;
            if (SpanX >= SpanY && SpanX >= SpanZ && SpanX > 0)
            {
                const int32 Mid = IX0 + SpanX / 2;
                Self(Self, IX0, Mid, IY0, IY1, IZ0, IZ1);
                Self(Self, Mid + 1, IX1, IY0, IY1, IZ0, IZ1);
            }
            else if (SpanY >= SpanZ && SpanY > 0)
            {
                const int32 Mid = IY0 + SpanY / 2;
                Self(Self, IX0, IX1, IY0, Mid, IZ0, IZ1);
                Self(Self, IX0, IX1, Mid + 1, IY1, IZ0, IZ1);
            }
            else if (SpanZ > 0)
            {
                const int32 Mid = IZ0 + SpanZ / 2;
                Self(Self, IX0, IX1, IY0, IY1, IZ0, Mid);
                Self(Self, IX0, IX1, IY0, IY1, Mid + 1, IZ1);
            }
            else
            {
                EvaluatePoint(IX0, IY0, IZ0);
            }
        };
        Visit(Visit, FirstX, LastX, FirstY, LastY, FirstZ, LastZ);
        return MaxCarveFactor;
    }

    int32 EvaluationFirstX = FirstX, EvaluationLastX = LastX;
    int32 EvaluationFirstY = FirstY, EvaluationLastY = LastY;
    int32 EvaluationFirstZ = FirstZ, EvaluationLastZ = LastZ;
    if (bKnownModifierDomains)
    {
        float CandidateMinX = FLT_MAX;
        float CandidateMinY = FLT_MAX;
        float CandidateMinZ = FLT_MAX;
        float CandidateMaxX = -FLT_MAX;
        float CandidateMaxY = -FLT_MAX;
        float CandidateMaxZ = -FLT_MAX;
        for (const FVoxelPassageModifierDomain& Domain : ModifierDomains)
        {
            CandidateMinX = FMath::Min(CandidateMinX,
                                       Domain.Min.X - ModifierDomainPadding);
            CandidateMinY = FMath::Min(CandidateMinY,
                                       Domain.Min.Y - ModifierDomainPadding);
            CandidateMinZ = FMath::Min(CandidateMinZ,
                                       Domain.Min.Z - ModifierDomainPadding);
            CandidateMaxX = FMath::Max(CandidateMaxX,
                                       Domain.Max.X + ModifierDomainPadding);
            CandidateMaxY = FMath::Max(CandidateMaxY,
                                       Domain.Max.Y + ModifierDomainPadding);
            CandidateMaxZ = FMath::Max(CandidateMaxZ,
                                       Domain.Max.Z + ModifierDomainPadding);
        }
        if (!VoxelMath::IsFinite(CandidateMinX) || !VoxelMath::IsFinite(CandidateMinY)
            || !VoxelMath::IsFinite(CandidateMinZ) || !VoxelMath::IsFinite(CandidateMaxX)
            || !VoxelMath::IsFinite(CandidateMaxY) || !VoxelMath::IsFinite(CandidateMaxZ))
        {
            bKnownModifierDomains = false;
        }
        else if (!VF_LatticeAxisRange(
                     FMath::Max((float)VoxelBox.Min.X, CandidateMinX),
                     FMath::Min((float)VoxelBox.Max.X, CandidateMaxX),
                     (float)LatticeOrigin.X, Step,
                     EvaluationFirstX, EvaluationLastX)
            || !VF_LatticeAxisRange(
                   FMath::Max((float)VoxelBox.Min.Y, CandidateMinY),
                   FMath::Min((float)VoxelBox.Max.Y, CandidateMaxY),
                   (float)LatticeOrigin.Y, Step,
                   EvaluationFirstY, EvaluationLastY)
            || !VF_LatticeAxisRange(
                   FMath::Max((float)VoxelBox.Min.Z, CandidateMinZ),
                   FMath::Min((float)VoxelBox.Max.Z, CandidateMaxZ),
                   (float)LatticeOrigin.Z, Step,
                   EvaluationFirstZ, EvaluationLastZ))
        {
            Cache.Begin(
                this, LifetimeId, Version, LatticeOrigin, Step,
                FirstX, LastX, FirstY, LastY, FirstZ, LastZ,
                /*bInNoCandidate*/ true);
            return 0.0f;
        }
    }

    const int64 DimX = static_cast<int64>(LastX) - FirstX + 1;
    const int64 DimY = static_cast<int64>(LastY) - FirstY + 1;
    const int64 DimZ = static_cast<int64>(LastZ) - FirstZ + 1;
    const int64 SampleCount = DimX * DimY * DimZ;
    constexpr int64 MaxCachedLatticeSamples = 65536;
    if (SampleCount > 0 && SampleCount <= MaxCachedLatticeSamples)
    {
        Cache.Begin(
            this, LifetimeId, Version, LatticeOrigin, Step,
            FirstX, LastX, FirstY, LastY, FirstZ, LastZ,
            /*bInNoCandidate*/ false);
        if (bKnownModifierDomains)
        {
            Cache.Factors.SetNumZeroed(static_cast<int32>(SampleCount));
        }
        else
        {
            Cache.Factors.SetNumUninitialized(static_cast<int32>(SampleCount));
        }

        float MaxCarveFactor = 0.0f;
        for (int32 IZ = EvaluationFirstZ; IZ <= EvaluationLastZ; ++IZ)
        {
            const float Z = (float)LatticeOrigin.Z + (float)(IZ * Step);
            for (int32 IY = EvaluationFirstY; IY <= EvaluationLastY; ++IY)
            {
                const float Y = (float)LatticeOrigin.Y + (float)(IY * Step);
                for (int32 IX = EvaluationFirstX; IX <= EvaluationLastX; ++IX)
                {
                    const float X = (float)LatticeOrigin.X + (float)(IX * Step);
                    const FVector Position(X, Y, Z);
                    float CarveFactor = 0.0f;
                    if (MayEvaluatePoint(Position))
                    {
                        const float ModifierSDF = EvaluateModifierSDF(X, Y, Z);
                        CarveFactor = VoxelMath::IsFinite(ModifierSDF)
                            ? CarveFactorFromSDF(ModifierSDF) : 1.0f;
                    }
                    Cache.Factors[Cache.Index(IX, IY, IZ)] = CarveFactor;
                    MaxCarveFactor = FMath::Max(MaxCarveFactor, CarveFactor);
                    if (MaxCarveFactor >= 1.0f)
                    {
                        // The remaining values are still filled below. Descendant queries need
                        // the complete exact lattice, not merely this query's maximum.
                        for (int32 RemainingZ = IZ; RemainingZ <= LastZ; ++RemainingZ)
                        {
                            const float RemainingWorldZ =
                                (float)LatticeOrigin.Z + (float)(RemainingZ * Step);
                            const int32 StartY = RemainingZ == IZ ? IY : FirstY;
                            for (int32 RemainingY = StartY; RemainingY <= LastY; ++RemainingY)
                            {
                                const float RemainingWorldY =
                                    (float)LatticeOrigin.Y + (float)(RemainingY * Step);
                                const int32 StartX =
                                    (RemainingZ == IZ && RemainingY == IY) ? IX + 1 : FirstX;
                                for (int32 RemainingX = StartX; RemainingX <= LastX; ++RemainingX)
                                {
                                    const float RemainingWorldX =
                                        (float)LatticeOrigin.X + (float)(RemainingX * Step);
                                    const FVector RemainingPosition(
                                        RemainingWorldX, RemainingWorldY, RemainingWorldZ);
                                    float RemainingFactor = 0.0f;
                                    if (MayEvaluatePoint(RemainingPosition))
                                    {
                                        const float RemainingSDF = EvaluateModifierSDF(
                                            RemainingWorldX, RemainingWorldY, RemainingWorldZ);
                                        RemainingFactor = VoxelMath::IsFinite(RemainingSDF)
                                            ? CarveFactorFromSDF(RemainingSDF) : 1.0f;
                                    }
                                    Cache.Factors[Cache.Index(RemainingX, RemainingY, RemainingZ)] =
                                        RemainingFactor;
                                }
                            }
                        }
                        return MaxCarveFactor;
                    }
                }
            }
        }
        return MaxCarveFactor;
    }

    // Very large or unusual external queries do not enter the bounded worker memo. Keep the
    // original exact walk for them; refusing the cache must never change the proof.
    Cache.Reset();
    float MaxCarveFactor = 0.0f;
    for (int32 IZ = EvaluationFirstZ; IZ <= EvaluationLastZ; ++IZ)
    {
        const float Z = (float)LatticeOrigin.Z + (float)(IZ * Step);
        for (int32 IY = EvaluationFirstY; IY <= EvaluationLastY; ++IY)
        {
            const float Y = (float)LatticeOrigin.Y + (float)(IY * Step);
            for (int32 IX = EvaluationFirstX; IX <= EvaluationLastX; ++IX)
            {
                const float X = (float)LatticeOrigin.X + (float)(IX * Step);
                const FVector Position(X, Y, Z);
                if (!MayEvaluatePoint(Position))
                {
                    continue;
                }
                const float ModifierSDF = EvaluateModifierSDF(X, Y, Z);
                if (!VoxelMath::IsFinite(ModifierSDF))
                {
                    return 1.0f;
                }
                MaxCarveFactor = FMath::Max(
                    MaxCarveFactor, CarveFactorFromSDF(ModifierSDF));
                if (MaxCarveFactor >= 1.0f)
                {
                    return MaxCarveFactor;
                }
            }
        }
    }
    return MaxCarveFactor;
}

bool UVoxelStrateManager::AnyPassageLandingFloorNearLattice(
    const FBox& VoxelBox, const FIntVector& LatticeOrigin, int32 Step) const
{
    if (Step <= 0 || !VoxelBox.IsValid)
    {
        return true;
    }
    for (const FVoxelPassage& Passage : Passages)
    {
        const FVoxelPassageLanding* Landings[] = {
            &Passage.UpperLanding, &Passage.LowerLanding };
        for (const FVoxelPassageLanding* Landing : Landings)
        {
            if (!VoxelMath::IsFinite(Landing->StandingPoint.X)
                || !VoxelMath::IsFinite(Landing->StandingPoint.Y)
                || !VoxelMath::IsFinite(Landing->FloorZ)
                || !VoxelMath::IsFinite(Landing->HalfWidth)
                || !VoxelMath::IsFinite(Landing->FloorThickness))
            {
                return true;
            }
            if (Landing->HalfWidth <= 0.0f || Landing->FloorThickness <= 0.0f)
            {
                continue;
            }
            constexpr float Pad = 1.0f;
            if (VF_LatticeBoxTouchesAABB(
                    VoxelBox, LatticeOrigin, Step,
                    Landing->StandingPoint.X - Landing->HalfWidth - Pad,
                    Landing->StandingPoint.X + Landing->HalfWidth + Pad,
                    Landing->StandingPoint.Y - Landing->HalfWidth - Pad,
                    Landing->StandingPoint.Y + Landing->HalfWidth + Pad,
                    Landing->FloorZ - Landing->FloorThickness - Pad,
                    Landing->FloorZ + Pad))
            {
                return true;
            }
        }

        if (Passage.bWalkableTunnelContract)
        {
            if (Passage.ControlPoints.Num() < 2
                || Passage.ControlRadii.Num() != Passage.ControlPoints.Num())
            {
                return true;
            }
            constexpr float Pad = 1.0f;
            for (int32 SegmentIndex = 0;
                 SegmentIndex + 1 < Passage.ControlPoints.Num(); ++SegmentIndex)
            {
                const FVector& A = Passage.ControlPoints[SegmentIndex];
                const FVector& B = Passage.ControlPoints[SegmentIndex + 1];
                if (!VoxelMath::IsFinite(A.X) || !VoxelMath::IsFinite(A.Y)
                    || !VoxelMath::IsFinite(A.Z) || !VoxelMath::IsFinite(B.X)
                    || !VoxelMath::IsFinite(B.Y) || !VoxelMath::IsFinite(B.Z)
                    || !VoxelMath::IsFinite(Passage.ControlRadii[SegmentIndex])
                    || !VoxelMath::IsFinite(Passage.ControlRadii[SegmentIndex + 1]))
                {
                    return true;
                }
                const float SupportRadius = FMath::Max(
                    FMath::Min(
                        FMath::Abs(Passage.ControlRadii[SegmentIndex]),
                        FMath::Abs(Passage.ControlRadii[SegmentIndex + 1])) - 0.5f,
                    VoxelPassageGeometry::PlayerRadiusVoxels);
                const float FloorA = VoxelPassageGeometry::TunnelFloorZ(
                    A, Passage.ControlRadii[SegmentIndex]);
                const float FloorB = VoxelPassageGeometry::TunnelFloorZ(
                    B, Passage.ControlRadii[SegmentIndex + 1]);
                if (VF_LatticeBoxTouchesAABB(
                        VoxelBox, LatticeOrigin, Step,
                        FMath::Min(A.X, B.X) - SupportRadius - Pad,
                        FMath::Max(A.X, B.X) + SupportRadius + Pad,
                        FMath::Min(A.Y, B.Y) - SupportRadius - Pad,
                        FMath::Max(A.Y, B.Y) + SupportRadius + Pad,
                        FMath::Min(FloorA, FloorB)
                            - VoxelPassageGeometry::LandingFloorThicknessVoxels - Pad,
                        FMath::Max(FloorA, FloorB) + Pad))
                {
                    return true;
                }
            }
        }
    }
    return false;
}

bool UVoxelStrateManager::AnyLandingFloorAtLattice(
    const FBox& VoxelBox, const FIntVector& LatticeOrigin, int32 Step) const
{
    // This query is used only after the stack has proved AllAir.  The broad floor guards above
    // are intentionally padded for sound interval folding, but that padding turns a long tunnel
    // or landing AABB into a false final-field candidate.  Check the actual writer predicates on
    // the same MC lattice before asking GetDensityAt for a certificate.  Unknown inputs remain
    // candidates; this function is never allowed to manufacture an empty proof.
    if (Step <= 0 || !VoxelBox.IsValid
        || VoxelPassageGeometry::VerticalShaftConnectorAirMarker())
    {
        return true;
    }

    const bool bBroadPassageFloor = AnyPassageLandingFloorNearLattice(
        VoxelBox, LatticeOrigin, Step);
    const bool bBroadOriginFloor = AnyOriginLandingFloorNearLattice(
        VoxelBox, LatticeOrigin, Step);
    if (!bBroadPassageFloor && !bBroadOriginFloor)
    {
        return false;
    }

    auto GetRange = [&](float Min, float Max, int32& OutFirst, int32& OutLast) -> bool
    {
        return VF_LatticeAxisRange(
            Min, Max, static_cast<float>(LatticeOrigin.X), Step, OutFirst, OutLast);
    };
    auto GetYRange = [&](float Min, float Max, int32& OutFirst, int32& OutLast) -> bool
    {
        return VF_LatticeAxisRange(
            Min, Max, static_cast<float>(LatticeOrigin.Y), Step, OutFirst, OutLast);
    };
    auto GetZRange = [&](float Min, float Max, int32& OutFirst, int32& OutLast) -> bool
    {
        return VF_LatticeAxisRange(
            Min, Max, static_cast<float>(LatticeOrigin.Z), Step, OutFirst, OutLast);
    };

    auto HasAnyLandingSample = [&](const FVoxelPassageLanding& Landing) -> bool
    {
        if (!VoxelMath::IsFinite(Landing.StandingPoint.X)
            || !VoxelMath::IsFinite(Landing.StandingPoint.Y)
            || !VoxelMath::IsFinite(Landing.FloorZ)
            || !VoxelMath::IsFinite(Landing.HalfWidth)
            || !VoxelMath::IsFinite(Landing.FloorThickness)
            || Landing.HalfWidth <= 0.0f || Landing.FloorThickness <= 0.0f)
        {
            // This function gates an AllAir proof. Invalid authored floor data is an unknown
            // structural writer, not evidence that the writer is absent.
            return true;
        }
        const float HalfWidth = FMath::Max(Landing.HalfWidth - 1.0f, 0.0f);
        int32 IX0 = 0, IX1 = -1, IY0 = 0, IY1 = -1, IZ0 = 0, IZ1 = -1;
        return VF_LatticeAxisRange(
                   FMath::Max((float)VoxelBox.Min.X,
                              Landing.StandingPoint.X - HalfWidth),
                   FMath::Min((float)VoxelBox.Max.X,
                              Landing.StandingPoint.X + HalfWidth),
                   (float)LatticeOrigin.X, Step, IX0, IX1)
            && VF_LatticeAxisRange(
                   FMath::Max((float)VoxelBox.Min.Y,
                              Landing.StandingPoint.Y - HalfWidth),
                   FMath::Min((float)VoxelBox.Max.Y,
                              Landing.StandingPoint.Y + HalfWidth),
                   (float)LatticeOrigin.Y, Step, IY0, IY1)
            && VF_LatticeAxisRange(
                   FMath::Max((float)VoxelBox.Min.Z,
                              Landing.FloorZ - Landing.FloorThickness),
                   FMath::Min((float)VoxelBox.Max.Z,
                              Landing.FloorZ + KINDA_SMALL_NUMBER),
                   (float)LatticeOrigin.Z, Step, IZ0, IZ1);
    };

    if (bBroadPassageFloor)
    {
        for (const FVoxelPassage& Passage : Passages)
        {
            if (HasAnyLandingSample(Passage.UpperLanding)
                || HasAnyLandingSample(Passage.LowerLanding))
            {
                return true;
            }

            if (!Passage.bWalkableTunnelContract)
            {
                continue;
            }
            if (Passage.ControlPoints.Num() < 2
                || Passage.ControlRadii.Num() != Passage.ControlPoints.Num())
            {
                return true;
            }

            constexpr float Pad = 1.0f;
            for (int32 SegmentIndex = 0;
                 SegmentIndex + 1 < Passage.ControlPoints.Num(); ++SegmentIndex)
            {
                const FVector& A = Passage.ControlPoints[SegmentIndex];
                const FVector& B = Passage.ControlPoints[SegmentIndex + 1];
                if (!VoxelMath::IsFinite(A.X) || !VoxelMath::IsFinite(A.Y)
                    || !VoxelMath::IsFinite(A.Z) || !VoxelMath::IsFinite(B.X)
                    || !VoxelMath::IsFinite(B.Y) || !VoxelMath::IsFinite(B.Z)
                    || !VoxelMath::IsFinite(Passage.ControlRadii[SegmentIndex])
                    || !VoxelMath::IsFinite(Passage.ControlRadii[SegmentIndex + 1]))
                {
                    return true;
                }
                const float SupportRadius = FMath::Max(
                    FMath::Min(
                        FMath::Abs(Passage.ControlRadii[SegmentIndex]),
                        FMath::Abs(Passage.ControlRadii[SegmentIndex + 1])) - 0.5f,
                    VoxelPassageGeometry::PlayerRadiusVoxels);
                const float FloorA = VoxelPassageGeometry::TunnelFloorZ(
                    A, Passage.ControlRadii[SegmentIndex]);
                const float FloorB = VoxelPassageGeometry::TunnelFloorZ(
                    B, Passage.ControlRadii[SegmentIndex + 1]);
                const float MinX = FMath::Min(A.X, B.X) - SupportRadius - Pad;
                const float MaxX = FMath::Max(A.X, B.X) + SupportRadius + Pad;
                const float MinY = FMath::Min(A.Y, B.Y) - SupportRadius - Pad;
                const float MaxY = FMath::Max(A.Y, B.Y) + SupportRadius + Pad;
                const float MinZ = FMath::Min(FloorA, FloorB)
                    - VoxelPassageGeometry::LandingFloorThicknessVoxels - Pad;
                const float MaxZ = FMath::Max(FloorA, FloorB) + Pad;
                if (!VoxelMath::IsFinite(MinX) || !VoxelMath::IsFinite(MaxX)
                    || !VoxelMath::IsFinite(MinY) || !VoxelMath::IsFinite(MaxY)
                    || !VoxelMath::IsFinite(MinZ) || !VoxelMath::IsFinite(MaxZ))
                {
                    return true;
                }

                int32 IX0 = 0, IX1 = -1, IY0 = 0, IY1 = -1, IZ0 = 0, IZ1 = -1;
                if (!GetRange(FMath::Max((float)VoxelBox.Min.X, MinX),
                              FMath::Min((float)VoxelBox.Max.X, MaxX), IX0, IX1)
                    || !GetYRange(FMath::Max((float)VoxelBox.Min.Y, MinY),
                                  FMath::Min((float)VoxelBox.Max.Y, MaxY), IY0, IY1)
                    || !GetZRange(FMath::Max((float)VoxelBox.Min.Z, MinZ),
                                  FMath::Min((float)VoxelBox.Max.Z, MaxZ), IZ0, IZ1))
                {
                    continue;
                }

                for (int32 IZ = IZ0; IZ <= IZ1; ++IZ)
                {
                    const float Z = (float)LatticeOrigin.Z + (float)(IZ * Step);
                    for (int32 IY = IY0; IY <= IY1; ++IY)
                    {
                        const float Y = (float)LatticeOrigin.Y + (float)(IY * Step);
                        for (int32 IX = IX0; IX <= IX1; ++IX)
                        {
                            const FVector Position(
                                (float)LatticeOrigin.X + (float)(IX * Step), Y, Z);
                            if (VF_IsWalkableTunnelFloor(Passage, Position))
                            {
                                return true;
                            }
                        }
                    }
                }
            }
        }
    }

    if (bBroadOriginFloor)
    {
        for (const FStrateSlot& Slot : StrateLayout)
        {
            if (Slot.Definition == nullptr) continue;
            const float TopZ = (static_cast<float>(Slot.TopChunkZ) + 1.0f) * CHUNK_SIZE;
            const float BottomZ = static_cast<float>(Slot.BottomChunkZ) * CHUNK_SIZE;
            const float Seal = VF_BoundarySealThicknessForDefinition(*Slot.Definition);
            const VoxelPassageGeometry::FOriginLandingGeometry Geometry =
                VoxelPassageGeometry::BuildOriginLandingGeometry(
                    TopZ, BottomZ, Seal, OriginSpineRadius);
            if (!Geometry.bValid)
            {
                continue;
            }
            const float HalfWidth = FMath::Max(Geometry.HalfWidth - 1.0f, 0.0f);
            int32 IX0 = 0, IX1 = -1, IY0 = 0, IY1 = -1, IZ0 = 0, IZ1 = -1;
            if (VF_LatticeAxisRange(
                    FMath::Max((float)VoxelBox.Min.X, -HalfWidth - 0.0f),
                    FMath::Min((float)VoxelBox.Max.X, HalfWidth + 0.0f),
                    (float)LatticeOrigin.X, Step, IX0, IX1)
                && VF_LatticeAxisRange(
                    FMath::Max((float)VoxelBox.Min.Y, -HalfWidth - 0.0f),
                    FMath::Min((float)VoxelBox.Max.Y, HalfWidth + 0.0f),
                    (float)LatticeOrigin.Y, Step, IY0, IY1)
                && VF_LatticeAxisRange(
                    FMath::Max((float)VoxelBox.Min.Z,
                               Geometry.FloorZ - Geometry.FloorThickness),
                    FMath::Min((float)VoxelBox.Max.Z,
                               Geometry.FloorZ + KINDA_SMALL_NUMBER),
                    (float)LatticeOrigin.Z, Step, IZ0, IZ1))
            {
                return true;
            }
        }
    }
    return false;
}

bool UVoxelStrateManager::AnyOriginLandingNearBox(
    const FVector& MinVoxel, const FVector& MaxVoxel) const
{
    if (OriginSpineRadius <= 0.0f) return false;
    const FBox VoxelBox(MinVoxel, MaxVoxel);
    for (const FStrateSlot& Slot : StrateLayout)
    {
        if (Slot.Definition == nullptr) continue;
        const float TopZ = (static_cast<float>(Slot.TopChunkZ) + 1.0f) * CHUNK_SIZE;
        const float BottomZ = static_cast<float>(Slot.BottomChunkZ) * CHUNK_SIZE;
        if (VoxelPassageGeometry::OriginLandingRoomTouchesBox(
                VoxelBox, TopZ, BottomZ,
                VF_BoundarySealThicknessForDefinition(*Slot.Definition),
                OriginSpineRadius))
        {
            return true;
        }
    }
    return false;
}

bool UVoxelStrateManager::AnyOriginLandingFloorNearBox(
    const FVector& MinVoxel, const FVector& MaxVoxel) const
{
    if (OriginSpineRadius <= 0.0f) return false;
    const FBox VoxelBox(MinVoxel, MaxVoxel);
    for (const FStrateSlot& Slot : StrateLayout)
    {
        if (Slot.Definition == nullptr) continue;
        const float TopZ = (static_cast<float>(Slot.TopChunkZ) + 1.0f) * CHUNK_SIZE;
        const float BottomZ = static_cast<float>(Slot.BottomChunkZ) * CHUNK_SIZE;
        if (VoxelPassageGeometry::OriginLandingFloorTouchesBox(
                VoxelBox, TopZ, BottomZ,
                VF_BoundarySealThicknessForDefinition(*Slot.Definition),
                OriginSpineRadius))
        {
            return true;
        }
    }
    return false;
}

bool UVoxelStrateManager::AnyOriginLandingNearLattice(
    const FBox& VoxelBox, const FIntVector& LatticeOrigin, int32 Step) const
{
    if (OriginSpineRadius <= 0.0f) return false;
    for (const FStrateSlot& Slot : StrateLayout)
    {
        if (Slot.Definition == nullptr) continue;
        const float TopZ = (static_cast<float>(Slot.TopChunkZ) + 1.0f) * CHUNK_SIZE;
        const float BottomZ = static_cast<float>(Slot.BottomChunkZ) * CHUNK_SIZE;
        const float Seal = VF_BoundarySealThicknessForDefinition(*Slot.Definition);
        if (VoxelPassageGeometry::OriginLandingRoomTouchesLattice(
                VoxelBox, LatticeOrigin, Step, TopZ, BottomZ, Seal, OriginSpineRadius))
        {
            return true;
        }
    }
    return false;
}

bool UVoxelStrateManager::AnyOriginLandingAirNearLattice(
    const FBox& VoxelBox, const FIntVector& LatticeOrigin, int32 Step,
    float BaseDensity, float SealThickness) const
{
    if (Step <= 0 || !VoxelBox.IsValid || OriginSpineRadius <= 0.0f)
    {
        return true;
    }
    if (!VoxelMath::IsFinite(BaseDensity) || !(BaseDensity > 0.0f)
        || !VoxelMath::IsFinite(SealThickness) || SealThickness < 0.0f)
    {
        return true;
    }
    const float AirTarget = -(BaseDensity * 2.0f + SealThickness + 4.0f);
    if (!VoxelMath::IsFinite(AirTarget))
    {
        return true;
    }

    for (const FStrateSlot& Slot : StrateLayout)
    {
        if (Slot.Definition == nullptr)
        {
            continue;
        }
        const float TopZ = (static_cast<float>(Slot.TopChunkZ) + 1.0f) * CHUNK_SIZE;
        const float BottomZ = static_cast<float>(Slot.BottomChunkZ) * CHUNK_SIZE;
        const float Seal = VF_BoundarySealThicknessForDefinition(*Slot.Definition);
        const VoxelPassageGeometry::FOriginLandingGeometry Geometry =
            VoxelPassageGeometry::BuildOriginLandingGeometry(
                TopZ, BottomZ, Seal, OriginSpineRadius);
        if (!Geometry.bValid)
        {
            continue;
        }
        if (!VoxelPassageGeometry::OriginLandingRoomTouchesLattice(
                VoxelBox, LatticeOrigin, Step, TopZ, BottomZ, Seal, OriginSpineRadius))
        {
            continue;
        }

        int32 FirstX = 0, LastX = -1;
        int32 FirstY = 0, LastY = -1;
        int32 FirstZ = 0, LastZ = -1;
        if (!VF_LatticeAxisRange(
                VoxelBox.Min.X, VoxelBox.Max.X, (float)LatticeOrigin.X, Step, FirstX, LastX)
            || !VF_LatticeAxisRange(
                VoxelBox.Min.Y, VoxelBox.Max.Y, (float)LatticeOrigin.Y, Step, FirstY, LastY)
            || !VF_LatticeAxisRange(
                VoxelBox.Min.Z, VoxelBox.Max.Z, (float)LatticeOrigin.Z, Step, FirstZ, LastZ))
        {
            return true;
        }
        const int64 SampleCount = ((int64)LastX - FirstX + 1)
            * ((int64)LastY - FirstY + 1)
            * ((int64)LastZ - FirstZ + 1);
        if (SampleCount <= 0 || SampleCount > 65536)
        {
            return true;
        }

        for (int32 IZ = FirstZ; IZ <= LastZ; ++IZ)
        for (int32 IY = FirstY; IY <= LastY; ++IY)
        for (int32 IX = FirstX; IX <= LastX; ++IX)
        {
            const FVector Position(
                (float)LatticeOrigin.X + (float)(IX * Step),
                (float)LatticeOrigin.Y + (float)(IY * Step),
                (float)LatticeOrigin.Z + (float)(IZ * Step));
            const float RoomSDF = VoxelPassageGeometry::OriginLandingRoomSDF(
                Position, Geometry);
            if (!VoxelMath::IsFinite(RoomSDF))
            {
                return true;
            }
            if (RoomSDF < VoxelPassageGeometry::LandingCarveBlendVoxels)
            {
                float CarveFactor = FMath::Clamp(
                    (VoxelPassageGeometry::LandingCarveBlendVoxels - RoomSDF)
                        / (VoxelPassageGeometry::LandingCarveBlendVoxels * 2.0f),
                    0.0f, 1.0f);
                CarveFactor = SmoothStep01(CarveFactor);
                const float LandingThreshold = FMath::Lerp(
                    BaseDensity, AirTarget, CarveFactor);
                if (!VoxelMath::IsFinite(LandingThreshold)
                    || LandingThreshold <= 0.0f)
                {
                    return true;
                }
            }
        }
    }
    return false;
}

bool UVoxelStrateManager::AnyOriginLandingFloorNearLattice(
    const FBox& VoxelBox, const FIntVector& LatticeOrigin, int32 Step) const
{
    if (OriginSpineRadius <= 0.0f) return false;
    for (const FStrateSlot& Slot : StrateLayout)
    {
        if (Slot.Definition == nullptr) continue;
        const float TopZ = (static_cast<float>(Slot.TopChunkZ) + 1.0f) * CHUNK_SIZE;
        const float BottomZ = static_cast<float>(Slot.BottomChunkZ) * CHUNK_SIZE;
        const float Seal = VF_BoundarySealThicknessForDefinition(*Slot.Definition);
        if (VoxelPassageGeometry::OriginLandingFloorTouchesLattice(
                VoxelBox, LatticeOrigin, Step, TopZ, BottomZ, Seal, OriginSpineRadius))
        {
            return true;
        }
    }
    return false;
}

//=============================================================================
// QUERIES
//=============================================================================

int32 UVoxelStrateManager::FindSlotIndexForChunkZ(int32 ChunkZ) const
{
    // Linear search through strate layout.
    // With ~10-20 strates this is fine. If we ever have hundreds,
    // switch to binary search (layout is sorted by Z).
    for (int32 i = 0; i < StrateLayout.Num(); i++)
    {
        const FStrateSlot& Slot = StrateLayout[i];
        if (ChunkZ <= Slot.TopChunkZ && ChunkZ >= Slot.BottomChunkZ)
        {
            return i;
        }
    }
    return -1;
}

UVoxelStrateDefinition* UVoxelStrateManager::GetStrateAt(float WorldZ) const
{
    // Convert world Z to chunk Z coordinate
    int32 ChunkZ = FMath::FloorToInt((WorldZ / VOXEL_SIZE) / CHUNK_SIZE);
    int32 SlotIdx = FindSlotIndexForChunkZ(ChunkZ);
    if (SlotIdx >= 0)
    {
        return StrateLayout[SlotIdx].Definition;
    }
    return nullptr;
}

int32 UVoxelStrateManager::GetStrateIndex(float WorldZ) const
{
    int32 ChunkZ = FMath::FloorToInt((WorldZ / VOXEL_SIZE) / CHUNK_SIZE);
    int32 SlotIdx = FindSlotIndexForChunkZ(ChunkZ);
    if (SlotIdx >= 0)
    {
        return StrateLayout[SlotIdx].StrateIndex;
    }
    return -1;
}

UVoxelStrateDefinition* UVoxelStrateManager::GetStrateForChunk(const FIntVector& ChunkCoord) const
{
    int32 SlotIdx = FindSlotIndexForChunkZ(ChunkCoord.Z);
    if (SlotIdx >= 0)
    {
        return StrateLayout[SlotIdx].Definition;
    }
    return nullptr;
}

bool UVoxelStrateManager::GetStrateChunkZBounds(int32 ChunkZ, int32& OutTopChunkZ, int32& OutBottomChunkZ) const
{
    // Strate-aware vertical streaming. Returns the chunk-Z span of the strate containing
    // ChunkZ; false if ChunkZ is in the inter-strate gap (or outside the layout) — there the
    // caller leaves the vertical view UNCLAMPED, since the gap is a brief see-both-sides
    // descent transition. TopChunkZ > BottomChunkZ (Z decreases downward).
    const int32 SlotIdx = FindSlotIndexForChunkZ(ChunkZ);
    if (SlotIdx < 0)
    {
        return false;
    }
    OutTopChunkZ    = StrateLayout[SlotIdx].TopChunkZ;
    OutBottomChunkZ = StrateLayout[SlotIdx].BottomChunkZ;
    return true;
}

ECaveGeneratorType UVoxelStrateManager::GetGeneratorTypeForChunk(const FIntVector& ChunkCoord) const
{
    // Look up which slot this chunk falls into.
    // If outside all strates (above or below), default to TunnelNetwork —
    // the fallback density path will produce solid rock anyway.
    int32 SlotIdx = FindSlotIndexForChunkZ(ChunkCoord.Z);
    if (SlotIdx < 0 || !StrateLayout[SlotIdx].Definition)
    {
        return ECaveGeneratorType::TunnelNetwork;
    }

#if WITH_EDITOR
    if (const FVoxelStrateComposerSlotOverride* Override =
        FindComposerOverride(StrateLayout[SlotIdx].StrateIndex))
    {
        return Override->Archetype;
    }
#endif

    if (SeasonStrates.IsValidIndex(SlotIdx))
    {
        return SeasonStrates[SlotIdx].Archetype;
    }

    return StrateLayout[SlotIdx].Definition->GeneratorType;
}

bool UVoxelStrateManager::UsesOperatorStackForChunk(const FIntVector& ChunkCoord) const
{
    const int32 SlotIdx = FindSlotIndexForChunkZ(ChunkCoord.Z);
    if (SlotIdx < 0 || !StrateLayout[SlotIdx].Definition) { return false; }

    if (SeasonStrates.IsValidIndex(SlotIdx))
    {
        // A recipe is an explicit stack opt-in. Native fixed entries retain the authored/switch
        // route while still reading their season parameter vector.
        return SeasonStrates[SlotIdx].bUsesRecipe;
    }

#if WITH_EDITOR
    if (FindComposerOverride(StrateLayout[SlotIdx].StrateIndex) != nullptr)
    {
        // Temporary composer candidates are measured through the stack path. Do not inherit an
        // unrelated asset flag from the slot being replaced.
        return true;
    }
#endif

    const UVoxelStrateDefinition* Def = StrateLayout[SlotIdx].Definition;
    if (!Def->bUseOperatorStack) { return false; }

    // LA LISTE DES ARCHÉTYPES PORTÉS — le seul endroit où elle est écrite. Un archétype non porté
    // ignore le drapeau et retombe sur le `switch`, pour qu'on puisse cocher la case sur n'importe
    // quelle strate sans rien casser en attendant son portage.
    // THE PORTED-ARCHETYPE LIST, written down exactly once. An unported archetype ignores the flag
    // and falls back to the switch, so the box can be ticked anywhere without breaking anything.
    switch (Def->GeneratorType)
    {
    case ECaveGeneratorType::Maze:            return true;   // Phase 1
    case ECaveGeneratorType::FlatPlain:                      // Phase 2 — les deux partagent
    case ECaveGeneratorType::CrystalChamber:  return true;   //   UNE seule pile (BuildSlabStack)

    case ECaveGeneratorType::SurfaceWorld:
        // ✅ La garde « pas de biomes » est TOMBÉE (étape 2c) : le combiner `Mask` existe, donc une
        // strate à biomes mélange bien ses hauteurs comme le chemin d'origine. Les trois archétypes
        // du dessus plus celui-ci font 5 des 8 portés.
        // The no-biome guard is GONE: the Mask combiner exists, so a biome strate blends its heights
        // exactly as the original path does.
        return true;

    case ECaveGeneratorType::VerticalShafts:  return true;   // Phase 2 — 3 ops repris de Maze tels quels

    case ECaveGeneratorType::FloatingIslands:
        // Phase 2 — la pile qui tourne à l'ENVERS : source de VIDE + fill, au lieu de source de ROC
        // + carve, avec les MÊMES opérateurs au signe près.
        return true;

    case ECaveGeneratorType::Underwater:
        // ⚠️ AUCUNE PILE À ELLE : `Underwater` EST `TunnelNetwork` plus un drapeau d'eau consommé
        // côté rendu. `GetDensityAt` les met dans le même `case`, et `WaterLevelRelative` n'est lu
        // que par `GetWaterLevel*` de ce manager — jamais par la densité (vérifié, pas supposé).
    case ECaveGeneratorType::TunnelNetwork:
        // Phase 2, LE DERNIER, et le plus gros : ~1080 lignes portées en trois étapes (squelette
        // SDF → douze modificateurs de détail → override d'op par salle), 19 opérateurs, dont
        // `FRoomGraphSource` qui **APPELLE** `BuildChunkCache`/`EvaluateSDFCached` au lieu de les
        // transcrire — c'est là que vit la discipline d'invariance de fenêtre d'ARCHITECTURE §8.4,
        // et en forker une copie aurait été le pire résultat possible de ce refactor.
        //
        // **8 SUR 8.** Le `switch` d'archétypes a désormais un jumeau en pile d'opérateurs, opt-in
        // par strate, chacun vérifié par un test d'équivalence bit à bit contre sa fonction
        // d'origine. Ce qui n'est PAS fait : `ClassifyTile` n'utilise toujours pas `ClassifyBox`.
        return true;

    default:                                  return false;
    }
}

bool UVoxelStrateManager::IsGapChunk(const FIntVector& ChunkCoord) const
{
    if (StrateLayout.Num() == 0) return false;

    // Above the top strate or below the bottom strate = open air, NOT a gap.
    const int32 StackTop    = StrateLayout[0].TopChunkZ;
    const int32 StackBottom = StrateLayout.Last().BottomChunkZ;
    if (ChunkCoord.Z > StackTop || ChunkCoord.Z < StackBottom) return false;

    // Inside the stack's Z span but not in any strate slot → it's a bedrock gap.
    return FindSlotIndexForChunkZ(ChunkCoord.Z) < 0;
}

FSlabGenerationParams UVoxelStrateManager::GetSlabParamsForChunk(const FIntVector& ChunkCoord) const
{
    // Fallback: empty params with BaseDensity < 0 → all-air outside strate range.
    int32 SlotIdx = FindSlotIndexForChunkZ(ChunkCoord.Z);
    if (SlotIdx < 0 || !StrateLayout[SlotIdx].Definition)
    {
        FSlabGenerationParams Empty;
        Empty.BaseDensity = -1.0f;
        return Empty;
    }

    const FStrateSlot& Slot = StrateLayout[SlotIdx];

    // Copy the designer-authored slab params from the strate definition, unless an editor
    // composer candidate owns this slot.
    FSlabGenerationParams Result;
    if (SeasonStrates.IsValidIndex(SlotIdx))
    {
        Result = SeasonStrates[SlotIdx].Params.SlabParams;
    }
#if WITH_EDITOR
    else if (const FVoxelStrateComposerSlotOverride* Override =
        FindComposerOverride(Slot.StrateIndex))
    {
        Result = (Override->Archetype == ECaveGeneratorType::FlatPlain
                  || Override->Archetype == ECaveGeneratorType::CrystalChamber)
            ? Override->Params.SlabParams
            : Slot.Definition->SlabParams;
    }
    else
#endif
    {
        Result = Slot.Definition->SlabParams;
    }

    // Fill in the runtime Z bounds (voxel coordinates, same convention as
    // FStrateGenerationParams::StrateTopWorldZ / StrateBottomWorldZ).
    // TopChunkZ+1 because the top chunk's CEILING is at (TopChunkZ+1)*CHUNK_SIZE.
    Result.StrateTopWorldZ    = (float)(Slot.TopChunkZ + 1) * CHUNK_SIZE;
    Result.StrateBottomWorldZ = (float)(Slot.BottomChunkZ)  * CHUNK_SIZE;

    return Result;
}

//=============================================================================
// PER-ARCHETYPE PARAM GETTERS
//=============================================================================
// Each mirrors GetSlabParamsForChunk: copy designer params, fill runtime Z bounds.
// No cross-boundary blending — archetypes meet at Hard boundaries.

FMazeGenerationParams UVoxelStrateManager::GetMazeParamsForChunk(const FIntVector& ChunkCoord) const
{
    const int32 SlotIdx = FindSlotIndexForChunkZ(ChunkCoord.Z);
    if (SlotIdx < 0 || !StrateLayout[SlotIdx].Definition)
    {
        FMazeGenerationParams Empty;
        Empty.BaseDensity = -1.0f;
        return Empty;
    }

    const FStrateSlot& Slot = StrateLayout[SlotIdx];
    FMazeGenerationParams Result = SeasonStrates.IsValidIndex(SlotIdx)
        ? SeasonStrates[SlotIdx].Params.MazeParams : Slot.Definition->MazeParams;
#if WITH_EDITOR
    if (const FVoxelStrateComposerSlotOverride* Override = FindComposerOverride(Slot.StrateIndex))
    {
        if (Override->Archetype == ECaveGeneratorType::Maze)
        {
            Result = Override->Params.MazeParams;
        }
    }
#endif
    Result.StrateTopWorldZ = (float)(Slot.TopChunkZ + 1) * CHUNK_SIZE;
    Result.StrateBottomWorldZ = (float)Slot.BottomChunkZ * CHUNK_SIZE;
    return Result;
}

FSurfaceGenerationParams UVoxelStrateManager::GetSurfaceParamsForChunk(const FIntVector& ChunkCoord) const
{
    const int32 SlotIdx = FindSlotIndexForChunkZ(ChunkCoord.Z);
    if (SlotIdx < 0 || !StrateLayout[SlotIdx].Definition)
    {
        FSurfaceGenerationParams Empty;
        Empty.BaseDensity = -1.0f;
        return Empty;
    }

    const FStrateSlot& Slot = StrateLayout[SlotIdx];
    FSurfaceGenerationParams Result = SeasonStrates.IsValidIndex(SlotIdx)
        ? SeasonStrates[SlotIdx].Params.SurfaceParams : Slot.Definition->SurfaceParams;
#if WITH_EDITOR
    if (const FVoxelStrateComposerSlotOverride* Override = FindComposerOverride(Slot.StrateIndex))
    {
        if (Override->Archetype == ECaveGeneratorType::SurfaceWorld)
        {
            Result = Override->Params.SurfaceParams;
        }
    }
#endif
    Result.StrateTopWorldZ = (float)(Slot.TopChunkZ + 1) * CHUNK_SIZE;
    Result.StrateBottomWorldZ = (float)Slot.BottomChunkZ * CHUNK_SIZE;
    return Result;
}

FVerticalShaftParams UVoxelStrateManager::GetVerticalShaftParamsForChunk(const FIntVector& ChunkCoord) const
{
    const int32 SlotIdx = FindSlotIndexForChunkZ(ChunkCoord.Z);
    if (SlotIdx < 0 || !StrateLayout[SlotIdx].Definition)
    {
        FVerticalShaftParams Empty;
        Empty.BaseDensity = -1.0f;
        return Empty;
    }

    const FStrateSlot& Slot = StrateLayout[SlotIdx];
    FVerticalShaftParams Result = SeasonStrates.IsValidIndex(SlotIdx)
        ? SeasonStrates[SlotIdx].Params.VerticalShaftParams : Slot.Definition->VerticalShaftParams;
#if WITH_EDITOR
    if (const FVoxelStrateComposerSlotOverride* Override = FindComposerOverride(Slot.StrateIndex))
    {
        if (Override->Archetype == ECaveGeneratorType::VerticalShafts)
        {
            Result = Override->Params.VerticalShaftParams;
        }
    }
#endif
    Result.StrateTopWorldZ = (float)(Slot.TopChunkZ + 1) * CHUNK_SIZE;
    Result.StrateBottomWorldZ = (float)Slot.BottomChunkZ * CHUNK_SIZE;
    return Result;
}

FFloatingIslandParams UVoxelStrateManager::GetFloatingIslandParamsForChunk(const FIntVector& ChunkCoord) const
{
    const int32 SlotIdx = FindSlotIndexForChunkZ(ChunkCoord.Z);
    if (SlotIdx < 0 || !StrateLayout[SlotIdx].Definition)
    {
        FFloatingIslandParams Empty;
        Empty.BaseDensity = -1.0f;
        return Empty;
    }

    const FStrateSlot& Slot = StrateLayout[SlotIdx];
    FFloatingIslandParams Result = SeasonStrates.IsValidIndex(SlotIdx)
        ? SeasonStrates[SlotIdx].Params.FloatingIslandParams : Slot.Definition->FloatingIslandParams;
#if WITH_EDITOR
    if (const FVoxelStrateComposerSlotOverride* Override = FindComposerOverride(Slot.StrateIndex))
    {
        if (Override->Archetype == ECaveGeneratorType::FloatingIslands)
        {
            Result = Override->Params.FloatingIslandParams;
        }
    }
#endif
    Result.StrateTopWorldZ = (float)(Slot.TopChunkZ + 1) * CHUNK_SIZE;
    Result.StrateBottomWorldZ = (float)Slot.BottomChunkZ * CHUNK_SIZE;
    return Result;
}

FBiomeContext UVoxelStrateManager::GetBiomeContextForChunk(const FIntVector& ChunkCoord) const
{
    FBiomeContext Out;

    const int32 SlotIdx = FindSlotIndexForChunkZ(ChunkCoord.Z);
    if (SlotIdx < 0 || !StrateLayout[SlotIdx].Definition) return Out;

    const UVoxelStrateDefinition* Def = StrateLayout[SlotIdx].Definition;
    if (Def->Biomes.Num() == 0) return Out;   // biomes disabled for this strate

    Out.Map = Def->BiomeMapParams;
    Out.Biomes.Reserve(Def->Biomes.Num());
    for (int32 i = 0; i < Def->Biomes.Num(); ++i)
    {
        const UVoxelBiomeDefinition* B = Def->Biomes[i];
        if (!B) continue;   // skip null entries (keep original index for content lookup)

        FBiomeResolved R;
        R.Index       = i;
        R.ReliefMin   = B->ReliefMin;   R.ReliefMax   = B->ReliefMax;
        R.MoistureMin = B->MoistureMin; R.MoistureMax = B->MoistureMax;
        R.DebugColor  = B->DebugColor.ToFColor(true);
        R.MaterialPaletteIndex = B->MaterialPaletteIndex;
        Out.Biomes.Add(R);
    }
    return Out;
}

bool UVoxelStrateManager::GetStrateUnrealZRange(float WorldZ, float& OutTopZ, float& OutBottomZ) const
{
    const int32 ChunkZ = FMath::FloorToInt((WorldZ / VOXEL_SIZE) / CHUNK_SIZE);
    const int32 SlotIdx = FindSlotIndexForChunkZ(ChunkZ);
    if (SlotIdx < 0) return false;

    const FStrateSlot& Slot = StrateLayout[SlotIdx];
    // Voxel-space Z bounds → Unreal units. Ceiling = top chunk's upper edge.
    OutTopZ    = (float)(Slot.TopChunkZ + 1) * CHUNK_SIZE * VOXEL_SIZE;
    OutBottomZ = (float)(Slot.BottomChunkZ)  * CHUNK_SIZE * VOXEL_SIZE;
    return true;
}

FStrateDisturbanceParams UVoxelStrateManager::GetDisturbanceParamsForChunk(const FIntVector& ChunkCoord) const
{
    int32 SlotIdx = FindSlotIndexForChunkZ(ChunkCoord.Z);
    if (SlotIdx < 0 || !StrateLayout[SlotIdx].Definition)
    {
        return FStrateDisturbanceParams();  // all features disabled
    }
    const FStrateSlot& Slot = StrateLayout[SlotIdx];
    FStrateDisturbanceParams Result = Slot.Definition->Disturbances;
    Result.StrateTopWorldZ    = (float)(Slot.TopChunkZ + 1) * CHUNK_SIZE;
    Result.StrateBottomWorldZ = (float)(Slot.BottomChunkZ)  * CHUNK_SIZE;
    return Result;
}

float UVoxelStrateManager::GetWaterLevelWorldZForChunk(const FIntVector& ChunkCoord) const
{
    int32 SlotIdx = FindSlotIndexForChunkZ(ChunkCoord.Z);
    if (SlotIdx < 0 || !StrateLayout[SlotIdx].Definition) return -FLT_MAX;

    const FStrateSlot& Slot = StrateLayout[SlotIdx];
    const UVoxelStrateDefinition* Def = Slot.Definition;
    if (!Def->bHasWater) return -FLT_MAX;

    // Pull the relative level from whichever archetype owns water.
    float Rel = 0.0f;
    const ECaveGeneratorType Archetype = SeasonStrates.IsValidIndex(SlotIdx)
        ? SeasonStrates[SlotIdx].Archetype : Def->GeneratorType;
    switch (Archetype)
    {
    case ECaveGeneratorType::SurfaceWorld:
        Rel = SeasonStrates.IsValidIndex(SlotIdx)
            ? SeasonStrates[SlotIdx].Params.SurfaceParams.WaterLevelRelative
            : Def->SurfaceParams.WaterLevelRelative;
        break;
    default:
        Rel = SeasonStrates.IsValidIndex(SlotIdx)
            ? SeasonStrates[SlotIdx].Params.TunnelNetworkParams.WaterLevelRelative
            : Def->GenerationParams.WaterLevelRelative;
        break;
    }
    if (Rel <= 0.0f) return -FLT_MAX;

    const float BottomZ = (float)(Slot.BottomChunkZ)  * CHUNK_SIZE;
    const float TopZ    = (float)(Slot.TopChunkZ + 1) * CHUNK_SIZE;
    return FMath::Lerp(BottomZ, TopZ, FMath::Clamp(Rel, 0.0f, 1.0f));
}

FStrateGenerationParams UVoxelStrateManager::GetGenerationParams(const FIntVector& ChunkCoord) const
{
    int32 SlotIdx = FindSlotIndexForChunkZ(ChunkCoord.Z);

    // If outside all strates, return negative density → guaranteed air.
    // BaseDensity must be < 0 because IsoLevel is 0.0 and density >= IsoLevel = solid.
    if (SlotIdx < 0)
    {
        FStrateGenerationParams Empty;
        Empty.BaseDensity = -1.0f;   // Negative → air after negation
        Empty.WormStrength = 0.0f;
        Empty.RoomDensity = 0.0f;    // No rooms outside strates
        return Empty;
    }

    const FStrateSlot& Slot = StrateLayout[SlotIdx];

    // This is the one final resolution gate for the asset-owned worm switch. It is deliberately
    // applied after composer/season replacements and after boundary blending: a disabled current
    // strate can never be re-enabled by a rolled candidate or by an enabled neighbour.
    const auto ResolveFinalParams = [&Slot](FStrateGenerationParams Result)
    {
        if (VF_WormsForceOff()
            || (Slot.Definition != nullptr && !Slot.Definition->bEnableWorms))
        {
            Result.WormStrength = 0.0f;
        }
        return VF_ApplyRuntimeRoughnessOverrides(Result);
    };

#if WITH_EDITOR
    if (const FVoxelStrateComposerSlotOverride* Override =
        FindComposerOverride(Slot.StrateIndex))
    {
        FStrateGenerationParams Result = Override->Params.TunnelNetworkParams;
        Result.StrateTopWorldZ = (float)(Slot.TopChunkZ + 1) * CHUNK_SIZE;
        Result.StrateBottomWorldZ = (float)Slot.BottomChunkZ * CHUNK_SIZE;
        // A candidate is a hard replacement of this slot. Do not blend its rolled vector with an
        // authored neighbour at a boundary; this is also how the offline fixture measures it.
        return ResolveFinalParams(Result);
    }
#endif

    if (SeasonStrates.IsValidIndex(SlotIdx))
    {
        FStrateGenerationParams Result = SeasonStrates[SlotIdx].Params.TunnelNetworkParams;
        Result.StrateTopWorldZ = (float)(Slot.TopChunkZ + 1) * CHUNK_SIZE;
        Result.StrateBottomWorldZ = (float)Slot.BottomChunkZ * CHUNK_SIZE;
        // Season vectors are already final, measured slot records. Vertical structural posts own
        // their boundary; blending them with another selected recipe would describe neither one.
        return ResolveFinalParams(Result);
    }

    FStrateGenerationParams BaseParams = BuildParamsFromDefinition(Slot.Definition);

    //=========================================================================
    // SET STRATE BOUNDARY Z VALUES
    //=========================================================================
    // The density function needs to know the strate's Z range (in voxel coords)
    // to seal the top and bottom with solid rock. This prevents caves from
    // carving through strate boundaries.
    //
    // TopChunkZ=0, CHUNK_SIZE=32: top of the strate = chunk 0's top edge = voxel Z=32
    // BottomChunkZ=-3: bottom of the strate = chunk -3's bottom edge = voxel Z=-3*32 = -96
    BaseParams.StrateTopWorldZ = (float)(Slot.TopChunkZ + 1) * CHUNK_SIZE;
    BaseParams.StrateBottomWorldZ = (float)(Slot.BottomChunkZ) * CHUNK_SIZE;

    //=========================================================================
    // BOUNDARY BLENDING (transition-type aware)
    //=========================================================================
    // If this chunk is near a strate boundary, apply the appropriate transition
    // based on the UPPER strate's TransitionType setting.
    //
    // Three transition styles:
    //
    //   GRADIENT (default):
    //     Classic linear lerp of all params across BlendChunks. Smooth,
    //     invisible boundary. Cave shape morphs gradually from one strate
    //     to the next over several chunks.
    //
    //   HARD:
    //     No blending at all — params switch instantly at the boundary.
    //     The abrupt change in density, room size, roughness, etc. creates
    //     a natural cliff, ledge, or visible material discontinuity.
    //     BlendChunks is ignored (effectively 0).
    //
    //   INTERLEAVED:
    //     3D Perlin noise warps the effective boundary Z position per XY column.
    //     Some columns transition early (fingers of the lower strate reach UP),
    //     others late (fingers of the upper strate reach DOWN). The Z frequency
    //     is intentionally low so the fingers are horizontal — wide, flat
    //     intrusions rather than vertical spikes.
    //
    // WHICH STRATE'S TRANSITION TYPE IS USED:
    // At the bottom boundary of strate N (between N and N+1), we use
    // strate N's (the upper strate's) TransitionType. This is consistent:
    // each strate definition controls what happens at its lower edge.
    // At the top boundary of strate N (between N-1 and N), we use
    // strate N-1's TransitionType (the strate above controls its lower edge).

    //---------------------------------------------------------------------
    // CHECK BOTTOM BOUNDARY (transitioning to strate below)
    //---------------------------------------------------------------------
    // DistFromBottom = how many chunks above the bottom edge of this strate.
    // When 0, we're right at the boundary. When == BlendChunks, we're at
    // the outer edge of the transition zone.
    int32 DistFromBottom = ChunkCoord.Z - Slot.BottomChunkZ;

    if (SlotIdx + 1 < StrateLayout.Num())
    {
        // The upper strate (this one) controls the transition type at its lower edge
        const EVoxelStrateTransition TransType = Slot.Definition->TransitionType;

        // Per-definition blend distance (overrides the manager's default BlendChunks)
        const int32 EffectiveBlend = Slot.Definition->TransitionBlendChunks;

        // Prepare the neighbor's params (only used for Gradient and Interleaved)
        const FStrateSlot& BelowSlot = StrateLayout[SlotIdx + 1];

        switch (TransType)
        {
        case EVoxelStrateTransition::Hard:
        {
            // HARD TRANSITION: No blending. The current strate's params apply
            // all the way to the boundary with zero transition zone.
            // The abrupt param change (different densities, room sizes, etc.)
            // creates a natural cliff or ledge — no special density boost needed.
            // We simply skip blending and fall through to the "return BaseParams" below.
            break;
        }

        case EVoxelStrateTransition::Gradient:
        {
            // GRADIENT TRANSITION: Classic smooth lerp across the blend zone.
            // Alpha goes from 0 (at the outer edge of the zone) to 1 (right at boundary).
            if (DistFromBottom < EffectiveBlend)
            {
                FStrateGenerationParams BelowParams = BuildParamsFromDefinition(BelowSlot.Definition);
                BelowParams.StrateTopWorldZ = (float)(BelowSlot.TopChunkZ + 1) * CHUNK_SIZE;
                BelowParams.StrateBottomWorldZ = (float)(BelowSlot.BottomChunkZ) * CHUNK_SIZE;

                // Linear alpha: 0 at EffectiveBlend chunks away, 1 at the boundary
                float Alpha = 1.0f - ((float)DistFromBottom / (float)EffectiveBlend);
                Alpha = FMath::Clamp(Alpha, 0.0f, 1.0f);

                return ResolveFinalParams(
                    FStrateGenerationParams::Lerp(BaseParams, BelowParams, Alpha));
            }
            break;
        }

        case EVoxelStrateTransition::Interleaved:
        {
            // INTERLEAVED TRANSITION: 3D noise warps the effective boundary Z.
            //
            // Instead of a flat boundary plane, the boundary becomes a wavy 3D surface.
            // For each XY position, a Perlin noise sample offsets the boundary Z by
            // up to ±2 chunks. Where the noise pushes the boundary UP, the lower strate's
            // params appear earlier (its "fingers" reach into the upper strate). Where
            // the noise pushes DOWN, the upper strate's params persist longer.
            //
            // The Z frequency is intentionally 3x lower than XY frequency so the fingers
            // are horizontal slabs rather than vertical spikes — this matches how real
            // geological intrusions look (wide, flat, layered).
            //
            // WarpAmplitude of 2.0 means the boundary can shift ±2 chunks from its
            // true position. Combined with EffectiveBlend for the transition width,
            // we need to check a wider zone: EffectiveBlend + WarpAmplitude.
            const float WarpAmplitude = 2.0f;  // Max boundary offset in chunks
            const int32 CheckRange = EffectiveBlend + FMath::CeilToInt(WarpAmplitude);

            if (DistFromBottom < CheckRange)
            {
                FStrateGenerationParams BelowParams = BuildParamsFromDefinition(BelowSlot.Definition);
                BelowParams.StrateTopWorldZ = (float)(BelowSlot.TopChunkZ + 1) * CHUNK_SIZE;
                BelowParams.StrateBottomWorldZ = (float)(BelowSlot.BottomChunkZ) * CHUNK_SIZE;

                // Sample 3D Perlin noise to warp the boundary position.
                // XY frequency 0.15 gives medium-scale variation (~6-7 chunks per cycle).
                // Z frequency 0.05 gives slow vertical change — horizontal finger shapes.
                // CachedSeed offsets ensure each world has unique finger patterns.
                float WarpNoise = FMath::PerlinNoise3D(FVector(
                    ChunkCoord.X * 0.15f + CachedSeed * 0.01f,
                    ChunkCoord.Y * 0.15f + CachedSeed * 0.017f,
                    ChunkCoord.Z * 0.05f  // Lower Z frequency for horizontal "fingers"
                )) * VOXEL_NOISE_SCALE;

                // Offset the distance from boundary by the noise * amplitude.
                // Positive noise → boundary pushed up → lower strate appears earlier.
                // Negative noise → boundary pushed down → upper strate persists longer.
                float WarpedDist = (float)DistFromBottom + WarpNoise * WarpAmplitude;

                // Compute alpha from the warped distance (same formula as Gradient,
                // but using the noise-displaced distance instead of the true distance)
                float Alpha = 1.0f - FMath::Clamp(WarpedDist / (float)EffectiveBlend, 0.0f, 1.0f);

                // Only blend if alpha > 0 (we're inside the warped transition zone)
                if (Alpha > 0.0f)
                {
                    return ResolveFinalParams(
                        FStrateGenerationParams::Lerp(BaseParams, BelowParams, Alpha));
                }
            }
            break;
        }
        }
    }

    //---------------------------------------------------------------------
    // CHECK TOP BOUNDARY (transitioning to strate above)
    //---------------------------------------------------------------------
    // Mirror logic: the ABOVE strate's TransitionType controls its lower edge,
    // which is this strate's upper edge. So we read from StrateLayout[SlotIdx-1].
    int32 DistFromTop = Slot.TopChunkZ - ChunkCoord.Z;

    if (SlotIdx > 0)
    {
        // The strate ABOVE controls the transition at its lower edge (= our upper edge)
        const FStrateSlot& AboveSlot = StrateLayout[SlotIdx - 1];
        const EVoxelStrateTransition TransType = AboveSlot.Definition->TransitionType;
        const int32 EffectiveBlend = AboveSlot.Definition->TransitionBlendChunks;

        switch (TransType)
        {
        case EVoxelStrateTransition::Hard:
        {
            // No blending — fall through to return BaseParams
            break;
        }

        case EVoxelStrateTransition::Gradient:
        {
            if (DistFromTop < EffectiveBlend)
            {
                FStrateGenerationParams AboveParams = BuildParamsFromDefinition(AboveSlot.Definition);
                AboveParams.StrateTopWorldZ = (float)(AboveSlot.TopChunkZ + 1) * CHUNK_SIZE;
                AboveParams.StrateBottomWorldZ = (float)(AboveSlot.BottomChunkZ) * CHUNK_SIZE;

                float Alpha = 1.0f - ((float)DistFromTop / (float)EffectiveBlend);
                Alpha = FMath::Clamp(Alpha, 0.0f, 1.0f);

                return ResolveFinalParams(
                    FStrateGenerationParams::Lerp(BaseParams, AboveParams, Alpha));
            }
            break;
        }

        case EVoxelStrateTransition::Interleaved:
        {
            const float WarpAmplitude = 2.0f;
            const int32 CheckRange = EffectiveBlend + FMath::CeilToInt(WarpAmplitude);

            if (DistFromTop < CheckRange)
            {
                FStrateGenerationParams AboveParams = BuildParamsFromDefinition(AboveSlot.Definition);
                AboveParams.StrateTopWorldZ = (float)(AboveSlot.TopChunkZ + 1) * CHUNK_SIZE;
                AboveParams.StrateBottomWorldZ = (float)(AboveSlot.BottomChunkZ) * CHUNK_SIZE;

                // Same noise function but with a different seed offset to avoid
                // symmetry between top and bottom boundaries of adjacent strates
                float WarpNoise = FMath::PerlinNoise3D(FVector(
                    ChunkCoord.X * 0.15f + CachedSeed * 0.013f,
                    ChunkCoord.Y * 0.15f + CachedSeed * 0.023f,
                    ChunkCoord.Z * 0.05f
                )) * VOXEL_NOISE_SCALE;

                float WarpedDist = (float)DistFromTop + WarpNoise * WarpAmplitude;
                float Alpha = 1.0f - FMath::Clamp(WarpedDist / (float)EffectiveBlend, 0.0f, 1.0f);

                if (Alpha > 0.0f)
                {
                    return ResolveFinalParams(
                        FStrateGenerationParams::Lerp(BaseParams, AboveParams, Alpha));
                }
            }
            break;
        }
        }
    }

    // Not near any boundary (or Hard transition) — use this strate's params directly
    return ResolveFinalParams(BaseParams);
}

//=============================================================================
// BUILD PARAMS FROM DEFINITION
//=============================================================================
// Returns the definition's base GenerationParams (cave shape, SDF, roughness, etc.).
//
// NOTE: Terrain op fields (TerraceStepHeight, ColumnDensity, etc.) are no longer
// merged here. They default to 0 (disabled) in the base params, and are applied
// per-room during BuildChunkCache() via FCachedRoom::RoomOp — each room hash-rolls
// one op from the strate's probability pool (FStrateTerrainOpEntry::Probability).
//
// Assets are still pre-loaded in Initialize() so BuildChunkCache can resolve
// soft pointers (Entry.Operation.Get()) without a disk read during generation.

FStrateGenerationParams UVoxelStrateManager::BuildParamsFromDefinition(const UVoxelStrateDefinition* Definition)
{
    if (!Definition) return FStrateGenerationParams();

    // Base params only — terrain op fields stay 0 until per-room assignment. Resolve the asset
    // switch here as well as at the final return so a disabled neighbour cannot leak worm strength
    // back through a boundary blend.
    FStrateGenerationParams Result = Definition->GenerationParams;
    if (VF_WormsForceOff() || !Definition->bEnableWorms)
    {
        Result.WormStrength = 0.0f;
    }
    return Result;
}

uint64 UVoxelStrateManager::GetGenerationParamsFingerprint() const
{
    // The diagnostic command-line override changes density inputs without changing an asset.  Do
    // not let a verdict from the unoverridden layout survive into an overridden session (or vice
    // versa); a zero fingerprint conservatively disables persistence for that session.
    const FRuntimeRoughnessOverrides& RoughnessOverrides = VF_GetRuntimeRoughnessOverrides();
    if (RoughnessOverrides.bSurfaceRoughness
        || RoughnessOverrides.bFrequency
        || RoughnessOverrides.bNoiseType)
    {
        return 0;
    }

    // The session cache is deliberately conservative.  These inputs are valid density inputs but
    // are not represented by the fixed-size hash below, so returning zero disables verdict reuse
    // instead of pretending that a partial key is complete.
    if (IsUsingSeason())
    {
        return 0;
    }
#if WITH_EDITOR
    if (ComposerOverrides.Num() > 0)
    {
        return 0;
    }
#endif

    uint32 A = 0x9E3779B9u;
    uint32 B = 0x85EBCA6Bu;
    auto HashBytes = [&A, &B](const void* Data, int32 Size)
    {
        A = FCrc::MemCrc32(Data, Size, A);
        B = FCrc::MemCrc32(Data, Size, B ^ 0xA511E9B3u);
    };
    auto HashValue = [&HashBytes](const auto& Value)
    {
        HashBytes(&Value, sizeof(Value));
    };

    HashValue(CachedSeed);
    HashValue(bOpenSurfaceEntry);
    HashValue(OriginSpineRadius);
    HashValue(InterStrateGapChunks);
    const int32 LayoutCount = StrateLayout.Num();
    HashValue(LayoutCount);

    for (const FStrateSlot& Slot : StrateLayout)
    {
        if (Slot.Definition == nullptr)
        {
            return 0;
        }

        // BuildChunkCache resolves these arrays into the room cache.  Hashing UObject pointers or
        // asset names would not prove the asset contents, so dynamic operation/biome layouts
        // conservatively opt out.  The normal synthetic/runtime profile has neither array.
        if (Slot.Definition->TerrainOperations.Num() > 0
            || Slot.Definition->Biomes.Num() > 0)
        {
            return 0;
        }

        HashValue(Slot.StrateIndex);
        HashValue(Slot.TopChunkZ);
        HashValue(Slot.BottomChunkZ);
        HashValue(Slot.HeightInChunks);
        HashValue(Slot.Definition->GeneratorType);
        HashValue(Slot.Definition->bUseOperatorStack);
        HashValue(Slot.Definition->bEnableWorms);
        HashValue(VF_WormsForceOff());
        // The selected evaluator is a world input even though it is deliberately not part of
        // FStrateGenerationParams (the fixed 83-field memo key). Keep persistent/verdict keys
        // from crossing an explicit WormNoiseMode/WormLatticeStep experiment.
        HashValue(VoxelWormField::GetNoiseMode());
        HashValue(VoxelWormField::GetLatticeStep());
        HashValue(Slot.Definition->StrateHeightInChunks);
        HashValue(Slot.Definition->TransitionType);
        HashValue(Slot.Definition->TransitionBlendChunks);
        HashValue(Slot.Definition->GenerationParams);
        HashValue(Slot.Definition->SlabParams);
        HashValue(Slot.Definition->MazeParams);
        HashValue(Slot.Definition->SurfaceParams);
        HashValue(Slot.Definition->VerticalShaftParams);
        HashValue(Slot.Definition->FloatingIslandParams);
        HashValue(Slot.Definition->PassageConfig);
        HashValue(Slot.Definition->bHasWater);
        HashValue(Slot.Definition->BiomeMapParams);
    }

    const uint64 Result = (static_cast<uint64>(B) << 32) | static_cast<uint64>(A);
    return Result != 0 ? Result : 1;
}
