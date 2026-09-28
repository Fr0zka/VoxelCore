// VoxelCaveMorphology.cpp
// Hash-based room/tunnel generation and SDF evaluation.
//
// TWO-PHASE EVALUATION:
// Phase 1 — BuildChunkCache: collects rooms, builds backbone, resolves tunnels.
//           Called ONCE per chunk (~1 time per 32³ = 32,768 voxels).
// Phase 2 — EvaluateSDFCached: evaluates room/tunnel SDFs with distance culling.
//           Called PER VOXEL using the cached data.
//
// FEATURES:
// - Origin room: guaranteed finite landing room at (0,0) per strate — reserved for the spine
// - Hash-based rooms: ellipsoid, rounded box, and capsule shapes
// - Tunnels: tapered capsules with deterministic wandering control-point chains
// - Horizontal bias: tunnels prefer horizontal connections
// - Endpoint Z offset: tunnels enter rooms at different heights
// - Per-room/tunnel distance culling: skip SDFs that can't affect this voxel
//
// CROSS-CHUNK DETERMINISM (the invariant that prevents seams):
// The room/tunnel GRAPH must reconstruct IDENTICALLY in every chunk whose voxels a
// tunnel touches. Room geometry is a pure hash of its cell, so that half is trivially
// identical. The fragile half is the EXISTENCE decision (the nearest-neighbor
// backbone), which historically depended on the per-chunk window of collected rooms.
// We now separate two regions (see BuildChunkCache):
//   - COLLECT region (wide): every room whose NN-candidate set could influence a
//     tunnel touching this chunk. Connectivity is decided over THIS set, so the
//     decision is window-invariant.
//   - STORE region (tight): only rooms/tunnels that can actually reach a voxel in
//     this chunk are kept for the per-voxel loop, so hot-path cost stays low.

#include "VoxelCaveMorphology.h"
#include "VoxelDensityProfile.h"
#include "VoxelDensityPrimitives.h"
#include "VoxelDensityAblation.h"
#include "VoxelStrateMeasure.h"
#include "VoxelTypes.h"          // Pour VOXEL_NOISE_SCALE, SmoothStep01
#include "VoxelStrateTypes.h"
#include "VoxelNoise.h"           // Pure FBM used by the slab landing query
#include "VoxelPassageGeometry.h"
#include "VoxelTerrainOpDefinition.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

#include <atomic>

namespace
{
    using FVFPlayerDensitySampler = TFunctionRef<float(float, float, float)>;

    // Density coordinates and cached cull radii are float-valued.  FVector is double-valued in
    // UE5, so using FVector::DistSquared here needlessly widens every candidate reject.  Keep the
    // cull in the same build-fixed float domain as the density query; the exact field output is
    // intentionally not required to match a double-precision round-trip.
    FORCEINLINE float VF_FloatDistSquared(
        float X, float Y, float Z, const FVector& Center)
    {
        const float DX = X - static_cast<float>(Center.X);
        const float DY = Y - static_cast<float>(Center.Y);
        const float DZ = Z - static_cast<float>(Center.Z);
        return DX * DX + DY * DY + DZ * DZ;
    }

    FORCEINLINE float VF_DistanceSquaredToAabb(
        float X, float Y, float Z,
        const FVector& Min,
        const FVector& Max)
    {
        const float DX = X < static_cast<float>(Min.X)
            ? static_cast<float>(Min.X) - X
            : (X > static_cast<float>(Max.X) ? X - static_cast<float>(Max.X) : 0.0f);
        const float DY = Y < static_cast<float>(Min.Y)
            ? static_cast<float>(Min.Y) - Y
            : (Y > static_cast<float>(Max.Y) ? Y - static_cast<float>(Max.Y) : 0.0f);
        const float DZ = Z < static_cast<float>(Min.Z)
            ? static_cast<float>(Min.Z) - Z
            : (Z > static_cast<float>(Max.Z) ? Z - static_cast<float>(Max.Z) : 0.0f);
        return DX * DX + DY * DY + DZ * DZ;
    }

    struct FVFTunnelShapeEvaluation
    {
        float SDF = FLT_MAX;
        bool bHasSweptFloor = false;
        float SweptFloorZ = -FLT_MAX;
        float SweptFloorRadius = 0.0f;
    };

    // FMath::PerlinNoise2D is the same two-octave field used by room floors.  The derivative bound
    // below is deliberately loose: each Grad2 component is in [-1, 1], the interpolation
    // difference is bounded by four, and SmoothCurve' <= 1.875.  Keeping this bound conservative
    // is what lets the corridor use the authored relief without turning the walkability promise
    // into a sampling accident.
    constexpr float VF_Perlin2DPartialAbsBound = 8.5f;
    constexpr float VF_MaxTunnelFloorReliefScale = 0.20f;

    // The weighted two-octave field is bounded by one in exact arithmetic.  Leave a margin for
    // the implementation's interpolation/rounding before using it as a proof bound.  This bound
    // is only used to skip a floor evaluation when the existing SmoothMax is provably the identity;
    // it never changes a value that could affect the isosurface.
    constexpr float VF_FloorReliefNoiseAbsBound = 1.01f;
    // A room owns a short, build-authored mouth apron.  The full room radius is intentionally
    // not used here: doing so lets a centre-to-centre tunnel floor overwrite the room floor far
    // inside the chamber and recreates the broad raised shelf this hand-off is meant to remove.
    constexpr float VF_TunnelMouthBlendRadiusVoxels =
        VoxelPassageGeometry::WalkableTunnelLandingApronVoxels;
    // The boundary probe describes where the *centreline* may leave the room.  Keep one complete
    // landing-carve band plus the player radius behind that probe so the tapered end-cap and the
    // fitted capsule can overlap the room on the sampled lattice.  This is a finite mouth apron,
    // not a centre-to-centre tunnel extension.
    constexpr float VF_TunnelMouthPlayerFitOverlapVoxels =
        VoxelPassageGeometry::LandingCarveBlendVoxels
        + VoxelPassageGeometry::PlayerRadiusVoxels;

    static float VF_TunnelMouthInset(float TunnelRadius, float BlendRadius)
    {
        return FMath::Max(
            0.5f,
            FMath::Abs(TunnelRadius) * 0.5f
                + FMath::Max(FMath::Abs(BlendRadius) * 0.25f, 0.0f)
                + VF_TunnelMouthPlayerFitOverlapVoxels);
    }

#if !UE_BUILD_SHIPPING
    static TAutoConsoleVariable<int32> CVarVoxelForgeTunnelMouthTrim(
        TEXT("voxel.TunnelMouthTrim"),
        1,
        TEXT("Development-only room-wall tunnel-mouth trim; 1 makes the room own the mouth by default."),
        ECVF_Default);
#endif

    enum class EVFFloorReliefFeature : uint8
    {
        RoomDouble,
        TunnelFloat,
        LandingFloat,
        LandingDouble
    };

    struct FVFFloorReliefColumnKey
    {
        bool bInteger = false;
        int32 X = 0;
        int32 Y = 0;
    };

    FORCEINLINE FVFFloorReliefColumnKey VF_MakeFloorReliefColumnKey(
        double X, double Y)
    {
        FVFFloorReliefColumnKey Key;
        const bool bIntegerX = VoxelMath::IsFinite(X)
            && X >= static_cast<double>(MIN_int32)
            && X <= static_cast<double>(MAX_int32)
            && FMath::FloorToDouble(X) == X;
        const bool bIntegerY = VoxelMath::IsFinite(Y)
            && Y >= static_cast<double>(MIN_int32)
            && Y <= static_cast<double>(MAX_int32)
            && FMath::FloorToDouble(Y) == Y;
        if (bIntegerX && bIntegerY)
        {
            Key.bInteger = true;
            Key.X = FMath::FloorToInt(X);
            Key.Y = FMath::FloorToInt(Y);
        }
        return Key;
    }

    // Relief is a function of the actual SDF-domain X/Y, not necessarily the original world X/Y:
    // CaveWarp is a 3D warp and therefore makes those coordinates vary with Z.  A cache keyed by
    // the actual domain is exact in both cases: it hits on genuine repeated columns (the world
    // structural path and unwarped queries), and safely falls through to the original calculation
    // when a warped sample is not the same column.  The table is direct-mapped and fixed-size so
    // it cannot become another retained per-tile allocation. Collisions only lose a hit.
    struct FVFFloorReliefCacheEntry
    {
        const void* CacheIdentity = nullptr;
        EVFFloorReliefFeature Feature = EVFFloorReliefFeature::RoomDouble;
        int32 FeatureIndex = INDEX_NONE;
        int32 X = 0;
        int32 Y = 0;
        uint32 FloorSeed = 0;
        float Frequency = 0.0f;
        float Noise = 0.0f;
        bool bValid = false;
    };

    struct FVFFloorReliefCache
    {
        // About 1.25 MiB on the current x64 layout, bounded independently of chunk/region size.
        static constexpr int32 Capacity = 32768;
        static_assert((Capacity & (Capacity - 1)) == 0, "floor relief cache must be a power of two");
        FVFFloorReliefCacheEntry Entries[Capacity];

        FORCEINLINE static uint32 HashKey(
            const void* CacheIdentity,
            EVFFloorReliefFeature Feature,
            int32 FeatureIndex,
            int32 X,
            int32 Y,
            uint32 FloorSeed,
            float Frequency)
        {
            const UPTRINT PointerBits = reinterpret_cast<UPTRINT>(CacheIdentity);
            uint32 Hash = static_cast<uint32>(PointerBits)
                ^ static_cast<uint32>(PointerBits >> 32);
            auto Combine = [&Hash](uint32 Value)
            {
                Hash ^= Value + 0x9e3779b9u + (Hash << 6) + (Hash >> 2);
            };
            Combine(static_cast<uint32>(Feature));
            Combine(static_cast<uint32>(FeatureIndex));
            Combine(static_cast<uint32>(X));
            Combine(static_cast<uint32>(Y));
            Combine(FloorSeed);
            Combine(GetTypeHash(Frequency));
            return VoxelHash::Mix(Hash);
        }

        FORCEINLINE float GetOrCompute(
            double X, double Y,
            uint32 FloorSeed, float Frequency,
            const void* CacheIdentity,
            EVFFloorReliefFeature Feature,
            int32 FeatureIndex,
            const FVFFloorReliefColumnKey& Column,
            float (*Compute)(double, double, uint32, float))
        {
            if (!Column.bInteger)
            {
                return Compute(X, Y, FloorSeed, Frequency);
            }

            FVFFloorReliefCacheEntry& Entry = Entries[HashKey(
                CacheIdentity, Feature, FeatureIndex,
                Column.X, Column.Y, FloorSeed, Frequency)
                & (Capacity - 1)];
            if (Entry.bValid
                && Entry.CacheIdentity == CacheIdentity
                && Entry.Feature == Feature
                && Entry.FeatureIndex == FeatureIndex
                && Entry.X == Column.X
                && Entry.Y == Column.Y
                && Entry.FloorSeed == FloorSeed
                && Entry.Frequency == Frequency)
            {
                return Entry.Noise;
            }

            const float Noise = Compute(X, Y, FloorSeed, Frequency);
            Entry.CacheIdentity = CacheIdentity;
            Entry.Feature = Feature;
            Entry.FeatureIndex = FeatureIndex;
            Entry.X = Column.X;
            Entry.Y = Column.Y;
            Entry.FloorSeed = FloorSeed;
            Entry.Frequency = Frequency;
            Entry.Noise = Noise;
            Entry.bValid = true;
            return Noise;
        }
    };

    static thread_local FVFFloorReliefCache GFloorReliefCache;

    FORCEINLINE float VF_ComputeFloorReliefNoiseDouble(
        double X, double Y, uint32 FloorSeed, float Frequency)
    {
        const float SF = static_cast<float>(FloorSeed) * 0.00001f;
        float Noise = FMath::PerlinNoise2D(
            FVector2D(X * Frequency + SF, Y * Frequency + SF * 1.7f)) * 0.65f;
        Noise += FMath::PerlinNoise2D(
            FVector2D(X * Frequency * 2.3f + SF * 3.1f,
                       Y * Frequency * 2.3f + SF * 5.3f)) * 0.35f;
        return Noise;
    }

    FORCEINLINE float VF_ComputeFloorReliefNoiseFloat(
        double X, double Y, uint32 FloorSeed, float Frequency)
    {
        const float FX = static_cast<float>(X);
        const float FY = static_cast<float>(Y);
        const float SF = static_cast<float>(FloorSeed) * 0.00001f;
        float Noise = FMath::PerlinNoise2D(
            FVector2D(FX * Frequency + SF, FY * Frequency + SF * 1.7f)) * 0.65f;
        Noise += FMath::PerlinNoise2D(
            FVector2D(FX * Frequency * 2.3f + SF * 3.1f,
                       FY * Frequency * 2.3f + SF * 5.3f)) * 0.35f;
        return Noise;
    }

    FORCEINLINE float VF_FloorReliefNoiseDouble(
        double X, double Y, uint32 FloorSeed, float Frequency,
        const void* CacheIdentity,
        EVFFloorReliefFeature Feature,
        int32 FeatureIndex,
        const FVFFloorReliefColumnKey& Column)
    {
        return GFloorReliefCache.GetOrCompute(
            X, Y, FloorSeed, Frequency, CacheIdentity, Feature, FeatureIndex,
            Column,
            &VF_ComputeFloorReliefNoiseDouble);
    }

    FORCEINLINE float VF_FloorReliefNoiseFloat(
        float X, float Y, uint32 FloorSeed, float Frequency,
        const void* CacheIdentity,
        EVFFloorReliefFeature Feature,
        int32 FeatureIndex,
        const FVFFloorReliefColumnKey& Column)
    {
        return GFloorReliefCache.GetOrCompute(
            static_cast<double>(X), static_cast<double>(Y),
            FloorSeed, Frequency, CacheIdentity, Feature, FeatureIndex,
            Column,
            &VF_ComputeFloorReliefNoiseFloat);
    }

    FORCEINLINE bool VF_GetFloorReliefBound(
        float Strength, float Envelope, float ReliefScale, float Frequency,
        float& OutBound)
    {
        OutBound = 0.0f;
        if (!(Strength > 0.0f)
            || !VoxelMath::IsFinite(Strength)
            || !VoxelMath::IsFinite(Frequency)
            || !(Envelope > 0.0f)
            || !VoxelMath::IsFinite(Envelope)
            || !(ReliefScale > 0.0f)
            || !VoxelMath::IsFinite(ReliefScale))
        {
            return false;
        }

        OutBound = FMath::Abs(Strength) * VOXEL_NOISE_SCALE
            * VF_FloorReliefNoiseAbsBound * Envelope * ReliefScale;
        return VoxelMath::IsFinite(OutBound) && OutBound >= 0.0f;
    }

    // Keep corridor relief on the exact two-octave field already used by room floors.  The
    // endpoint fade is not a second noise system: it is the landing apron that lets the swept
    // corridor meet the room's own floor without a seed-dependent vertical lip.
    FORCEINLINE float VF_TunnelFloorRelief(
        float X, float Y, const FCachedTunnel& Tunnel,
        float Envelope, float ReliefScale,
        const void* CacheIdentity, int32 TunnelIndex,
        const FVFFloorReliefColumnKey& Column)
    {
        if (!(Tunnel.FloorReliefStrength > 0.0f)
            || !VoxelMath::IsFinite(Tunnel.FloorReliefStrength)
            || !VoxelMath::IsFinite(Tunnel.FloorReliefFrequency)
            || !(Envelope > 0.0f)
            || !(ReliefScale > 0.0f))
        {
            return 0.0f;
        }

        const float Noise = VF_FloorReliefNoiseFloat(
            X, Y, Tunnel.FloorSeed, Tunnel.FloorReliefFrequency,
            CacheIdentity, EVFFloorReliefFeature::TunnelFloat, TunnelIndex,
            Column);
        return Noise * VOXEL_NOISE_SCALE * Tunnel.FloorReliefStrength
            * Envelope * ReliefScale;
    }

    FORCEINLINE float VF_TunnelFloorProfileT(
        const FVector& A, float RadiusA,
        const FVector& B, float RadiusB,
        float T)
    {
        // Legacy/cache fallback is deliberately continuous.  A malformed or old cache may no
        // longer manufacture a staircase from a per-sample step count; safe callers either use
        // the authored profile or retain the smooth tapered-capsule floor.
        return FMath::Clamp(T, 0.0f, 1.0f);
    }

    FORCEINLINE float VF_TunnelFloorReliefScale(
        const FVector& A, float RadiusA,
        const FVector& B, float RadiusB,
        int32 SegmentIndex, int32 NumSegments,
        const FCachedTunnel& Tunnel)
    {
        if (!(Tunnel.FloorReliefStrength > 0.0f)
            || !VoxelMath::IsFinite(Tunnel.FloorReliefStrength)
            || !VoxelMath::IsFinite(Tunnel.FloorReliefFrequency))
        {
            return 0.0f;
        }

        const float HorizontalRun = FVector2D(
            static_cast<float>(B.X - A.X), static_cast<float>(B.Y - A.Y)).Size();
        if (!(HorizontalRun > KINDA_SMALL_NUMBER))
        {
            return 0.0f;
        }

        const float FloorDelta = FMath::Abs(
            VoxelPassageGeometry::TunnelFloorZ(B, FMath::Abs(RadiusB))
            - VoxelPassageGeometry::TunnelFloorZ(A, FMath::Abs(RadiusA)));
        const float BaseGradient = FloorDelta / HorizontalRun;
        const float GentleThreshold = VoxelMath::IsFinite(
                Tunnel.TunnelFloorGentleSlopeGradient)
            ? FMath::Max(Tunnel.TunnelFloorGentleSlopeGradient, 0.0f)
            : VoxelPassageGeometry::PlayerWalkableFloorMaxGradient;
        const float AvailableGradient = GentleThreshold - BaseGradient;
        if (!(AvailableGradient > 0.0f))
        {
            return 0.0f;
        }

        const float Frequency = FMath::Abs(Tunnel.FloorReliefFrequency);
        const float Amplitude = FMath::Abs(Tunnel.FloorReliefStrength)
            * VOXEL_NOISE_SCALE;
        const float OctaveFrequencyWeight = 0.65f + 0.35f * 2.3f;
        const float NoiseGradientBound =
            1.4142135623730951f * VF_Perlin2DPartialAbsBound
            * OctaveFrequencyWeight * Frequency * Amplitude;
        const int32 LandingEnvelopeCount = (SegmentIndex == 0 ? 1 : 0)
            + (SegmentIndex + 1 == NumSegments ? 1 : 0);
        const float EnvelopeDerivativeBound = LandingEnvelopeCount > 0
            ? (1.5f * static_cast<float>(LandingEnvelopeCount)
                / VoxelPassageGeometry::WalkableTunnelLandingApronVoxels)
                * Amplitude
            : 0.0f;
        const float ReliefGradientBound = NoiseGradientBound
            + EnvelopeDerivativeBound;
        if (!(ReliefGradientBound > KINDA_SMALL_NUMBER)
            || !VoxelMath::IsFinite(ReliefGradientBound))
        {
            return 0.0f;
        }

        return FMath::Clamp(
            FMath::Min(
                VF_MaxTunnelFloorReliefScale,
                AvailableGradient / ReliefGradientBound),
            0.0f, VF_MaxTunnelFloorReliefScale);
    }

    FORCEINLINE float VF_TunnelFloorReliefScaleForGradient(
        const FVector& A, const FVector& B,
        int32 SegmentIndex, int32 NumSegments,
        const FCachedTunnel& Tunnel,
        float BaseGradient, bool bTerraced)
    {
        if (bTerraced
            || !(Tunnel.FloorReliefStrength > 0.0f)
            || !VoxelMath::IsFinite(Tunnel.FloorReliefStrength)
            || !VoxelMath::IsFinite(Tunnel.FloorReliefFrequency))
        {
            return 0.0f;
        }

        const float HorizontalRun = FVector2D(
            static_cast<float>(B.X - A.X), static_cast<float>(B.Y - A.Y)).Size();
        if (!(HorizontalRun > KINDA_SMALL_NUMBER)
            || !VoxelMath::IsFinite(BaseGradient)
            || BaseGradient < 0.0f)
        {
            return 0.0f;
        }

        const float GentleThreshold = VoxelMath::IsFinite(
                Tunnel.TunnelFloorGentleSlopeGradient)
            ? FMath::Max(Tunnel.TunnelFloorGentleSlopeGradient, 0.0f)
            : VoxelPassageGeometry::PlayerWalkableFloorMaxGradient;
        const float AvailableGradient = GentleThreshold - BaseGradient;
        if (!(AvailableGradient > 0.0f))
        {
            return 0.0f;
        }

        const float Frequency = FMath::Abs(Tunnel.FloorReliefFrequency);
        const float Amplitude = FMath::Abs(Tunnel.FloorReliefStrength)
            * VOXEL_NOISE_SCALE;
        const float OctaveFrequencyWeight = 0.65f + 0.35f * 2.3f;
        const float NoiseGradientBound =
            1.4142135623730951f * VF_Perlin2DPartialAbsBound
            * OctaveFrequencyWeight * Frequency * Amplitude;
        const int32 LandingEnvelopeCount = (SegmentIndex == 0 ? 1 : 0)
            + (SegmentIndex + 1 == NumSegments ? 1 : 0);
        const float EnvelopeDerivativeBound = LandingEnvelopeCount > 0
            ? (1.5f * static_cast<float>(LandingEnvelopeCount)
                / VoxelPassageGeometry::WalkableTunnelLandingApronVoxels)
                * Amplitude
            : 0.0f;
        const float ReliefGradientBound = NoiseGradientBound
            + EnvelopeDerivativeBound;
        if (!(ReliefGradientBound > KINDA_SMALL_NUMBER)
            || !VoxelMath::IsFinite(ReliefGradientBound))
        {
            return 0.0f;
        }

        return FMath::Clamp(
            FMath::Min(
                VF_MaxTunnelFloorReliefScale,
                AvailableGradient / ReliefGradientBound),
            0.0f, VF_MaxTunnelFloorReliefScale);
    }

    static void VF_BuildTunnelFloorProfile(
        const TArray<FVector>& ControlPoints,
        const TArray<float>& ControlRadii,
        const FCachedTunnel& Tunnel,
        const TArray<int32>* FloorLedgeLevels,
        TArray<FTunnelFloorSegmentProfile>& OutProfiles)
    {
        OutProfiles.Reset();
        if (ControlPoints.Num() < 2
            || ControlRadii.Num() != ControlPoints.Num())
        {
            return;
        }

        const int32 NumSegments = ControlPoints.Num() - 1;
        OutProfiles.SetNum(NumSegments);
        TArray<float, TInlineAllocator<16>> FloorAtControl;
        FloorAtControl.SetNum(ControlPoints.Num());
        for (int32 Index = 0; Index < ControlPoints.Num(); ++Index)
        {
            FloorAtControl[Index] = VoxelPassageGeometry::TunnelFloorZ(
                ControlPoints[Index], FMath::Abs(ControlRadii[Index]));
        }

        const int32 MaxLedges = FMath::Clamp(Tunnel.TunnelFloorMaxLedges, 1, 4096);
        const int32 AuthoredLedgeCount = FMath::Clamp(
            Tunnel.TunnelFloorAuthoredLedgeCount, 1,
            FMath::Min(MaxLedges, FMath::Max(1, NumSegments)));
        const bool bHasLedgeLevels = Tunnel.bDramaticLedge
            && AuthoredLedgeCount > 0
            && FloorLedgeLevels != nullptr
            && FloorLedgeLevels->Num() == ControlPoints.Num();
        const float TotalFloorDelta = FloorAtControl.Last() - FloorAtControl[0];

        for (int32 SegmentIndex = 0; SegmentIndex < NumSegments; ++SegmentIndex)
        {
            FTunnelFloorSegmentProfile& Profile = OutProfiles[SegmentIndex];
            const FVector& A = ControlPoints[SegmentIndex];
            const FVector& B = ControlPoints[SegmentIndex + 1];
            const float FloorA = FloorAtControl[SegmentIndex];
            const float FloorB = FloorAtControl[SegmentIndex + 1];
            const float HorizontalRun = FVector2D(
                static_cast<float>(B.X - A.X),
                static_cast<float>(B.Y - A.Y)).Size();
            Profile.StartFloorZ = FloorA;
            Profile.EndFloorZ = FloorB;
            Profile.NaturalStartFloorZ = FloorA;
            Profile.NaturalEndFloorZ = FloorB;
            Profile.bLedgeTransition = false;

            if (bHasLedgeLevels)
            {
                // The graph builder has already selected the few boundaries. The profile records
                // one explicit vertical transition there; it never quantises samples inside the
                // segment, so a steep edge cannot turn into a staircase.
                const int32 StartLevel = FMath::Clamp(
                    (*FloorLedgeLevels)[SegmentIndex], 0, AuthoredLedgeCount);
                const int32 EndLevel = FMath::Clamp(
                    (*FloorLedgeLevels)[SegmentIndex + 1], 0, AuthoredLedgeCount);
                Profile.StartFloorZ = FloorAtControl[0]
                    + TotalFloorDelta * static_cast<float>(StartLevel)
                        / static_cast<float>(AuthoredLedgeCount);
                Profile.EndFloorZ = FloorAtControl[0]
                    + TotalFloorDelta * static_cast<float>(EndLevel)
                        / static_cast<float>(AuthoredLedgeCount);
                Profile.bLedgeTransition = EndLevel != StartLevel;
            }

            const bool bTransition = Profile.bLedgeTransition
                && !FMath::IsNearlyEqual(Profile.StartFloorZ, Profile.EndFloorZ);
            const float ProfileGradient = (!bTransition && HorizontalRun > KINDA_SMALL_NUMBER)
                ? FMath::Abs(Profile.EndFloorZ - Profile.StartFloorZ) / HorizontalRun
                : 0.0f;
            Profile.ReliefScale = VF_TunnelFloorReliefScaleForGradient(
                A, B, SegmentIndex, NumSegments, Tunnel,
                ProfileGradient, bTransition);
        }

        if (VoxelDensityProfile::AreCountersEnabled())
        {
            uint64 AuthoredLedges = 0;
            uint64 LedgesBelow2 = 0;
            uint64 Ledges2To4 = 0;
            uint64 Ledges4To8 = 0;
            uint64 Ledges8Plus = 0;
            for (const FTunnelFloorSegmentProfile& Profile : OutProfiles)
            {
                if (Profile.bLedgeTransition)
                {
                    ++AuthoredLedges;
                    const float LedgeHeight = FMath::Abs(
                        Profile.EndFloorZ - Profile.StartFloorZ);
                    if (LedgeHeight < 2.0f) { ++LedgesBelow2; }
                    else if (LedgeHeight < 4.0f) { ++Ledges2To4; }
                    else if (LedgeHeight < 8.0f) { ++Ledges4To8; }
                    else { ++Ledges8Plus; }
                }
            }
            VoxelDensityProfile::AddCounter(
                VoxelDensityProfile::ECounter::TunnelFloorProfileBuilds);
            VoxelDensityProfile::AddCounter(
                VoxelDensityProfile::ECounter::TunnelFloorProfileSegments,
                static_cast<uint64>(OutProfiles.Num()));
            VoxelDensityProfile::AddCounter(
                VoxelDensityProfile::ECounter::TunnelFloorProfileLedges,
                AuthoredLedges);
            VoxelDensityProfile::AddCounter(
                VoxelDensityProfile::ECounter::TunnelFloorProfileLedgesBelow2Voxels,
                LedgesBelow2);
            VoxelDensityProfile::AddCounter(
                VoxelDensityProfile::ECounter::TunnelFloorProfileLedges2To4Voxels,
                Ledges2To4);
            VoxelDensityProfile::AddCounter(
                VoxelDensityProfile::ECounter::TunnelFloorProfileLedges4To8Voxels,
                Ledges4To8);
            VoxelDensityProfile::AddCounter(
                VoxelDensityProfile::ECounter::TunnelFloorProfileLedges8PlusVoxels,
                Ledges8Plus);
        }
    }

    struct FVFRoomFloorOwnership
    {
        bool bValid = false;
        // The room-floor blend is intentionally short, but the tunnel capsule and the fitted
        // player envelope can still overlap the room beyond that blend.  This separate hand-off
        // bit prevents the graph tunnel's lower-side polarity from turning that overlap into a
        // wall without extending the room floor itself into the corridor.
        bool bSuppressTunnelBottom = false;
        float FloorZ = 0.0f;
        float Weight = 0.0f;
        uint32 RoomHash = 0;
    };

    static FVFRoomFloorOwnership VF_FindTunnelMouthOwnership(
        const FVector& Position,
        const TArray<FVector>& ControlPoints,
        const TArray<float>& ControlRadii,
        float MouthBlendRadiusA,
        float MouthBlendRadiusB,
        float MouthFloorZA,
        float MouthFloorZB,
        uint32 FloorRoomHashA,
        uint32 FloorRoomHashB,
        float SDFBlendRadius)
    {
        FVFRoomFloorOwnership Result;
        const int32 ControlPointCount = ControlPoints.Num();
        if (ControlPointCount < 2
            || ControlRadii.Num() != ControlPointCount)
        {
            return Result;
        }

        const FVector* ControlPointData = ControlPoints.GetData();
        const float* ControlRadiusData = ControlRadii.GetData();

        const float Fade = FMath::Max(
            FMath::Max(SDFBlendRadius, 0.0f) * 0.35f,
            1.0f);
        float BestScore = FLT_MAX;
        for (int32 Endpoint = 0; Endpoint < 2; ++Endpoint)
        {
            const int32 ControlIndex = Endpoint == 0
                ? 0 : ControlPointCount - 1;
            const FVector& Mouth = ControlPointData[ControlIndex];
            const float DX = static_cast<float>(Position.X - Mouth.X);
            const float DY = static_cast<float>(Position.Y - Mouth.Y);
            const float Distance = FMath::Sqrt(DX * DX + DY * DY);
            const float BlendRadius = FMath::Max(
                Endpoint == 0 ? MouthBlendRadiusA : MouthBlendRadiusB,
                0.0f);
            const float EndpointRadius = FMath::Abs(ControlRadiusData[ControlIndex]);
            const float BottomHandoffRadius = BlendRadius
                + VF_TunnelMouthPlayerFitOverlapVoxels
                + EndpointRadius;
            const float RelativeDistance = Distance - BlendRadius;
            const float Weight = RelativeDistance <= 0.0f
                ? 1.0f
                : (RelativeDistance < Fade
                    ? 1.0f - SmoothStep01(RelativeDistance / Fade)
                    : 0.0f);
            const bool bInBottomHandoff = VoxelMath::IsFinite(BottomHandoffRadius)
                && Distance <= BottomHandoffRadius;
            if (!(Weight > 0.0f) && !bInBottomHandoff)
            {
                continue;
            }

            // Prefer the nearest normalized mouth. Endpoint order is the deterministic tie-break
            // when two large rooms overlap the same tunnel entrance.
            const float Score = Distance / FMath::Max(BlendRadius, 1.0f);
            if (Score >= BestScore)
            {
                continue;
            }
            const float DefaultFloorZ = static_cast<float>(Mouth.Z)
                - FMath::Abs(ControlRadiusData[ControlIndex]);
            const float StoredFloorZ = Endpoint == 0 ? MouthFloorZA : MouthFloorZB;
            const float FloorZ = StoredFloorZ > -FLT_MAX
                && VoxelMath::IsFinite(StoredFloorZ)
                ? StoredFloorZ
                : DefaultFloorZ;
            if (!VoxelMath::IsFinite(FloorZ))
            {
                continue;
            }
            BestScore = Score;
            Result.bValid = Weight > 0.0f;
            Result.bSuppressTunnelBottom = bInBottomHandoff;
            Result.FloorZ = FloorZ;
            Result.Weight = Weight;
            Result.RoomHash = Endpoint == 0 ? FloorRoomHashA : FloorRoomHashB;
        }
        return Result;
    }

    static FVFRoomFloorOwnership VF_FindSDFTunnelMouthOwnership(
        const FVector& Position,
        const FCachedTunnel& Tunnel,
        float SDFBlendRadius)
    {
        if (!Tunnel.bHasFloorRoomOwnership)
        {
            return FVFRoomFloorOwnership();
        }
        return VF_FindTunnelMouthOwnership(
            Position, Tunnel.ControlPoints, Tunnel.ControlRadii,
            Tunnel.SDFMouthBlendRadiusA, Tunnel.SDFMouthBlendRadiusB,
            Tunnel.SDFMouthFloorZA, Tunnel.SDFMouthFloorZB,
            Tunnel.FloorRoomHashA, Tunnel.FloorRoomHashB, SDFBlendRadius);
    }

    static FVFRoomFloorOwnership VF_FindWorldTunnelMouthOwnership(
        const FVector& Position,
        const FCachedTunnel& Tunnel,
        float SDFBlendRadius)
    {
        if (!Tunnel.bHasFloorRoomOwnership)
        {
            return FVFRoomFloorOwnership();
        }
        return VF_FindTunnelMouthOwnership(
            Position, Tunnel.WorldControlPoints, Tunnel.WorldControlRadii,
            Tunnel.WorldMouthBlendRadiusA, Tunnel.WorldMouthBlendRadiusB,
            Tunnel.WorldMouthFloorZA, Tunnel.WorldMouthFloorZB,
            Tunnel.FloorRoomHashA, Tunnel.FloorRoomHashB, SDFBlendRadius);
    }

    // The mouth hand-off is finite.  Most tunnel samples are in open corridor space, so reject
    // them with two squared-distance comparisons before entering the exact sqrt/SmoothStep query.
    // The extra voxel is a conservative rounding pad; it cannot exclude a point accepted by
    // VF_FindTunnelMouthOwnership.  Returning true for malformed/non-finite cache data keeps the
    // legacy exact query as the safety fallback.
    static bool VF_MayHaveTunnelMouthOwnership(
        const FVector& Position,
        const FCachedTunnel& Tunnel,
        bool bWorldChain,
        float SDFBlendRadius)
    {
        if (!Tunnel.bHasFloorRoomOwnership)
        {
            return false;
        }

        const TArray<FVector>& ControlPoints = bWorldChain
            ? Tunnel.WorldControlPoints : Tunnel.ControlPoints;
        const TArray<float>& ControlRadii = bWorldChain
            ? Tunnel.WorldControlRadii : Tunnel.ControlRadii;
        const int32 ControlPointCount = ControlPoints.Num();
        if (ControlPointCount < 2 || ControlRadii.Num() != ControlPointCount)
        {
            return false;
        }

        const float Fade = FMath::Max(
            FMath::Max(SDFBlendRadius, 0.0f) * 0.35f,
            1.0f);
        if (!VoxelMath::IsFinite(Fade)
            || !VoxelMath::IsFinite(Position.X)
            || !VoxelMath::IsFinite(Position.Y))
        {
            return true;
        }

        const float BlendRadiusA = FMath::Max(
            bWorldChain ? Tunnel.WorldMouthBlendRadiusA : Tunnel.SDFMouthBlendRadiusA,
            0.0f);
        const float BlendRadiusB = FMath::Max(
            bWorldChain ? Tunnel.WorldMouthBlendRadiusB : Tunnel.SDFMouthBlendRadiusB,
            0.0f);
        const float BlendRadii[2] = {BlendRadiusA, BlendRadiusB};
        for (int32 Endpoint = 0; Endpoint < 2; ++Endpoint)
        {
            const int32 ControlIndex = Endpoint == 0 ? 0 : ControlPointCount - 1;
            const FVector& Mouth = ControlPoints[ControlIndex];
            const float EndpointRadius = FMath::Abs(ControlRadii[ControlIndex]);
            const float BlendRadius = BlendRadii[Endpoint];
            const float FadeReach = BlendRadius + Fade;
            const float HandoffReach = BlendRadius
                + VF_TunnelMouthPlayerFitOverlapVoxels + EndpointRadius;
            const float Reach = FMath::Max(FadeReach, HandoffReach) + 1.0f;
            if (!VoxelMath::IsFinite(Mouth.X)
                || !VoxelMath::IsFinite(Mouth.Y)
                || !VoxelMath::IsFinite(EndpointRadius)
                || !VoxelMath::IsFinite(BlendRadius)
                || !VoxelMath::IsFinite(Reach))
            {
                return true;
            }

            const float DX = static_cast<float>(Position.X - Mouth.X);
            const float DY = static_cast<float>(Position.Y - Mouth.Y);
            const float DistanceSquared = DX * DX + DY * DY;
            if (!VoxelMath::IsFinite(DistanceSquared)
                || DistanceSquared <= FMath::Square(Reach))
            {
                return true;
            }
        }
        return false;
    }

    struct FVFTunnelFloorTerms
    {
        float BaseFloorZ = 0.0f;
        float ReliefEnvelope = 0.0f;
        float ReliefScale = 0.0f;
    };

    FORCEINLINE FVFTunnelFloorTerms VF_TunnelFloorTerms(
        const FVector& A, float RadiusA,
        const FVector& B, float RadiusB,
        float T, int32 SegmentIndex, int32 NumSegments,
        const FCachedTunnel& Tunnel,
        const FTunnelFloorSegmentProfile* FloorProfile,
        const FVFRoomFloorOwnership* RoomFloorOwnership)
    {
        FVFTunnelFloorTerms Terms;
        if (FloorProfile != nullptr)
        {
            const float ClampedT = FMath::Clamp(T, 0.0f, 1.0f);
            const float ProfileT = FloorProfile->bLedgeTransition
                ? (ClampedT >= 1.0f
                    ? 1.0f
                    : 0.0f)
                : ClampedT;
            Terms.BaseFloorZ = FMath::Lerp(
                FloorProfile->StartFloorZ, FloorProfile->EndFloorZ, ProfileT);
            Terms.ReliefScale = FloorProfile->ReliefScale;
        }
        else
        {
            if (VoxelDensityProfile::AreCountersEnabled())
            {
                VoxelDensityProfile::AddCounter(
                    VoxelDensityProfile::ECounter::TunnelFloorProfileFallbacks);
            }
            const float ProfileT = VF_TunnelFloorProfileT(
                A, RadiusA, B, RadiusB, T);
            Terms.BaseFloorZ = FMath::Lerp(
                VoxelPassageGeometry::TunnelFloorZ(A, FMath::Abs(RadiusA)),
                VoxelPassageGeometry::TunnelFloorZ(B, FMath::Abs(RadiusB)),
                ProfileT);
            Terms.ReliefScale = VF_TunnelFloorReliefScale(
                A, RadiusA, B, RadiusB, SegmentIndex, NumSegments, Tunnel);
        }

        // The room is the owner of its own floor volume. A tunnel capsule penetrates past the
        // mouth into that room; without this reconciliation its floor cut can leave a raised or
        // lowered apron in the room. Blend only across the SDF mouth band so the tunnel remains
        // unchanged in open corridor space.
        const bool bOwnsThisTunnelMouth = RoomFloorOwnership != nullptr
            && RoomFloorOwnership->bValid
            && Tunnel.bHasFloorRoomOwnership
            && (RoomFloorOwnership->RoomHash == Tunnel.FloorRoomHashA
                || RoomFloorOwnership->RoomHash == Tunnel.FloorRoomHashB);
        if (bOwnsThisTunnelMouth)
        {
            // The room is A and the corridor is B: inside the finite mouth hand-off, A owns the
            // floor regardless of which endpoint happened to be higher.  The old one-sided Min
            // only removed a raised tunnel apron; a lower tunnel still won the SmoothMin union
            // and left a lowered shelf in the chamber.  Interpolate to the room floor in the
            // authored apron so both representations agree at the room boundary.
            Terms.BaseFloorZ = FMath::Lerp(
                Terms.BaseFloorZ, RoomFloorOwnership->FloorZ,
                FMath::Clamp(RoomFloorOwnership->Weight, 0.0f, 1.0f));
            // The room's floor relief is already included in FloorZ. Fade the tunnel relief out
            // with the ownership blend, so the transition cannot stack two independent floors.
            Terms.ReliefScale *= 1.0f - FMath::Clamp(
                RoomFloorOwnership->Weight, 0.0f, 1.0f);
        }

        const double DeltaX = static_cast<double>(static_cast<float>(B.X - A.X));
        const double DeltaY = static_cast<double>(static_cast<float>(B.Y - A.Y));
        const float HorizontalRun = static_cast<float>(FMath::Sqrt(
            DeltaX * DeltaX + DeltaY * DeltaY));
        Terms.ReliefEnvelope = 1.0f;
        if (SegmentIndex == 0)
        {
            Terms.ReliefEnvelope *= SmoothStep01(FMath::Clamp(
                (T * HorizontalRun)
                    / VoxelPassageGeometry::WalkableTunnelLandingApronVoxels,
                0.0f, 1.0f));
        }
        if (SegmentIndex + 1 == NumSegments)
        {
            Terms.ReliefEnvelope *= SmoothStep01(FMath::Clamp(
                ((1.0f - T) * HorizontalRun)
                    / VoxelPassageGeometry::WalkableTunnelLandingApronVoxels,
                0.0f, 1.0f));
        }
        return Terms;
    }

    FORCEINLINE float VF_TunnelFloorFromTerms(
        const FVector& Position,
        const FCachedTunnel& Tunnel,
        const FVFTunnelFloorTerms& Terms,
        const void* CacheIdentity, int32 TunnelIndex,
        bool bEvaluateRelief,
        const FVFFloorReliefColumnKey& Column)
    {
        float FloorZ = Terms.BaseFloorZ;
        if (bEvaluateRelief)
        {
            FloorZ += VF_TunnelFloorRelief(
                static_cast<float>(Position.X), static_cast<float>(Position.Y),
                Tunnel, Terms.ReliefEnvelope, Terms.ReliefScale,
                CacheIdentity, TunnelIndex, Column);
        }
        return FloorZ;
    }

    FORCEINLINE bool VF_ProjectTunnelSegmentXY(
        const FVector& Position, const FVector& A, const FVector& B,
        float& OutT, float& OutDistanceSquared)
    {
        const double AXY_X = static_cast<double>(static_cast<float>(A.X));
        const double AXY_Y = static_cast<double>(static_cast<float>(A.Y));
        const double DeltaX = static_cast<double>(static_cast<float>(B.X - A.X));
        const double DeltaY = static_cast<double>(static_cast<float>(B.Y - A.Y));
        const float LengthSquared = static_cast<float>(
            DeltaX * DeltaX + DeltaY * DeltaY);
        if (LengthSquared <= KINDA_SMALL_NUMBER)
        {
            OutT = 0.0f;
            OutDistanceSquared = FLT_MAX;
            return false;
        }

        const double QueryX = static_cast<double>(static_cast<float>(Position.X));
        const double QueryY = static_cast<double>(static_cast<float>(Position.Y));
        const double Dot = (QueryX - AXY_X) * DeltaX
            + (QueryY - AXY_Y) * DeltaY;
        OutT = FMath::Clamp(Dot / LengthSquared, 0.0f, 1.0f);
        const double ClosestX = AXY_X + DeltaX * OutT;
        const double ClosestY = AXY_Y + DeltaY * OutT;
        const double DistanceX = QueryX - ClosestX;
        const double DistanceY = QueryY - ClosestY;
        OutDistanceSquared = static_cast<float>(
            DistanceX * DistanceX + DistanceY * DistanceY);
        return true;
    }

    static FVFTunnelShapeEvaluation VF_EvaluateSweptTunnelChain(
        const FVector& Position,
        const TArray<FVector>& ControlPoints,
        const TArray<float>& ControlRadii,
        const FCachedTunnel& Tunnel,
        float SDFBlendRadius,
        bool bApplyFloorCut,
        const void* CacheIdentity, int32 TunnelIndex,
        const FVFFloorReliefColumnKey& ReliefColumn,
        const TArray<FTunnelFloorSegmentProfile>* FloorProfiles,
        const FVFRoomFloorOwnership* RoomFloorOwnership)
    {
        FVFTunnelShapeEvaluation Result;
        const int32 ControlPointCount = ControlPoints.Num();
        if (ControlPointCount < 2
            || ControlRadii.Num() != ControlPointCount)
        {
            return Result;
        }

        const int32 NumSegments = ControlPointCount - 1;
        const FVector* ControlPointData = ControlPoints.GetData();
        const float* ControlRadiusData = ControlRadii.GetData();
        const FTunnelFloorSegmentProfile* FloorProfileData = nullptr;
        if (FloorProfiles != nullptr && FloorProfiles->Num() == NumSegments)
        {
            FloorProfileData = FloorProfiles->GetData();
        }
        const float FloorBlend = FMath::Max(SDFBlendRadius, 0.0f) * 0.35f;
        for (int32 SegmentIndex = 0;
             SegmentIndex < NumSegments;
             ++SegmentIndex)
        {
            const FVector& A = ControlPointData[SegmentIndex];
            const FVector& B = ControlPointData[SegmentIndex + 1];
            const float RadiusA = ControlRadiusData[SegmentIndex];
            const float RadiusB = ControlRadiusData[SegmentIndex + 1];
            float T = 0.0f;
            float HorizontalDistanceSquared = FLT_MAX;
            const bool bHasHorizontalProjection = VF_ProjectTunnelSegmentXY(
                Position, A, B, T, HorizontalDistanceSquared);
            if (!bHasHorizontalProjection)
            {
                // A vertical/degenerate XY segment has no finite floor projection.  Retain the
                // capsule for air, but never invent a support surface there.
                Result.SDF = FMath::Min(
                    Result.SDF,
                    VoxelSDF::TaperedCapsule(Position, A, B, RadiusA, RadiusB));
                continue;
            }

            const FTunnelFloorSegmentProfile* Profile = FloorProfileData != nullptr
                ? FloorProfileData + SegmentIndex : nullptr;
            const FVFTunnelFloorTerms FloorTerms = VF_TunnelFloorTerms(
                A, RadiusA, B, RadiusB,
                T, SegmentIndex, NumSegments, Tunnel, Profile,
                RoomFloorOwnership);
            const float NaturalFloorZ = Profile != nullptr
                ? FMath::Lerp(
                    Profile->NaturalStartFloorZ,
                    Profile->NaturalEndFloorZ,
                    FMath::Clamp(T, 0.0f, 1.0f))
                : FMath::Lerp(
                    VoxelPassageGeometry::TunnelFloorZ(A, FMath::Abs(RadiusA)),
                    VoxelPassageGeometry::TunnelFloorZ(B, FMath::Abs(RadiusB)),
                    FMath::Clamp(T, 0.0f, 1.0f));
            const float SupportRadius = FMath::Max(
                FMath::Min(FMath::Abs(RadiusA), FMath::Abs(RadiusB)) - 0.5f,
                VoxelPassageGeometry::PlayerRadiusVoxels);
            float ReliefBound = 0.0f;
            const bool bHasReliefBound = VF_GetFloorReliefBound(
                Tunnel.FloorReliefStrength,
                FloorTerms.ReliefEnvelope,
                FloorTerms.ReliefScale,
                Tunnel.FloorReliefFrequency,
                ReliefBound);

            // The corridor is authored as one shape: translate this tapered capsule so its
            // bottom follows the immutable floor profile.  A zero shift is deliberately kept on
            // the original call, preserving the natural path exactly.  The provisional shift
            // excludes relief only long enough to decide whether the bounded relief can matter;
            // the final shift below includes the actual cached column value.
            // Every complete build-time profile owns the floor.  A route that was deliberately
            // wound, or that contains an explicit ledge transition, also moves the swept curve
            // with that authored floor.  A direct smooth ramp keeps the established natural arch
            // and lets the SmoothMax floor cut provide the walkable D-floor; this avoids changing
            // headroom in every gentle tunnel merely because a profile is now always present.
            const bool bUseAuthoredProfile = bApplyFloorCut
                && Profile != nullptr
                && (Tunnel.bFloorRouteWasWound || Tunnel.bDramaticLedge);
            const bool bAnchorCapsule = bUseAuthoredProfile
                && VoxelMath::IsFinite(NaturalFloorZ);
            const auto EvaluateAnchoredShape = [
                &Position, &A, &B, RadiusA, RadiusB,
                NaturalFloorZ, bAnchorCapsule]
                (float FloorZ) -> float
            {
                const float Shift = FloorZ - NaturalFloorZ;
                if (!bAnchorCapsule
                    || !VoxelMath::IsFinite(Shift)
                    || FMath::Abs(Shift) <= KINDA_SMALL_NUMBER)
                {
                    return VoxelSDF::TaperedCapsule(
                        Position, A, B, RadiusA, RadiusB);
                }
                const FVector Offset(0.0f, 0.0f, Shift);
                return VoxelSDF::TaperedCapsule(
                    Position, A + Offset, B + Offset, RadiusA, RadiusB);
            };
            float SegmentSDF = EvaluateAnchoredShape(FloorTerms.BaseFloorZ);

            bool bEvaluateRelief = bApplyFloorCut;
            if (bEvaluateRelief && bHasReliefBound)
            {
                // If the capsule is already above the highest possible floor by at least the
                // SmoothMax radius, the cut is exactly the identity. The support test below is a
                // separate obligation: a floor that does not alter SDF at this sample may still
                // be the support band queried by the post stack.
                const bool bFloorCannotAffect = SegmentSDF >=
                    FloorTerms.BaseFloorZ + ReliefBound
                    - static_cast<float>(Position.Z) + FloorBlend;
                const bool bCannotBeSupport =
                    SegmentSDF > 0.0f
                    || HorizontalDistanceSquared > FMath::Square(SupportRadius)
                    || Position.Z < FloorTerms.BaseFloorZ - ReliefBound
                        - VoxelPassageGeometry::LandingFloorThicknessVoxels
                    || Position.Z > FloorTerms.BaseFloorZ + ReliefBound
                        + VoxelPassageGeometry::WalkableTunnelFloorAirClearanceVoxels;
                bEvaluateRelief = !bFloorCannotAffect || !bCannotBeSupport;
            }
            if (bApplyFloorCut && RoomFloorOwnership != nullptr
                && RoomFloorOwnership->bValid
                && Tunnel.bHasFloorRoomOwnership
                && (RoomFloorOwnership->RoomHash == Tunnel.FloorRoomHashA
                    || RoomFloorOwnership->RoomHash == Tunnel.FloorRoomHashB)
                && RoomFloorOwnership->Weight > 0.0f)
            {
                // Room ownership can move the floor even when the uncapped tunnel floor is
                // outside its own relief bound. Keep the exact room relief in that mouth band.
                bEvaluateRelief = true;
            }

            const float FloorZ = VF_TunnelFloorFromTerms(
                Position, Tunnel, FloorTerms,
                CacheIdentity, TunnelIndex, bEvaluateRelief, ReliefColumn);
            // Relief belongs to the authored floor as well as to its SmoothMax cut.  Re-evaluate
            // the translated arch only when the column relief actually changed its anchor; most
            // samples retain the cheap provisional result.
            if (bAnchorCapsule
                && VoxelMath::IsFinite(FloorZ)
                && FMath::Abs(FloorZ - FloorTerms.BaseFloorZ) > KINDA_SMALL_NUMBER)
            {
                SegmentSDF = EvaluateAnchoredShape(FloorZ);
            }
            const float SegmentResultSDF = bApplyFloorCut
                ? VoxelSDF::SmoothMax(
                    SegmentSDF, FloorZ - static_cast<float>(Position.Z), FloorBlend)
                : SegmentSDF;
            if (SegmentResultSDF < Result.SDF)
            {
                Result.SDF = SegmentResultSDF;
                // This is shape metadata, not a second floor field.  It records which side of
                // the winning swept SDF is its own bottom so the final MC hand-off can restore
                // that side when a room's generic air field would otherwise erase it.
                Result.bHasSweptFloor = bApplyFloorCut
                    && VoxelMath::IsFinite(FloorZ)
                    && VoxelMath::IsFinite(SupportRadius)
                    && VoxelMath::IsFinite(HorizontalDistanceSquared)
                    && HorizontalDistanceSquared <= FMath::Square(SupportRadius)
                    // The room owns the finite mouth apron.  Do not let the tunnel's bottom
                    // polarity turn that overlap into a wall; the corridor interior remains
                    // owned by this swept shape.
                    && !(RoomFloorOwnership != nullptr
                        && RoomFloorOwnership->bSuppressTunnelBottom);
                Result.SweptFloorZ = Result.bHasSweptFloor ? FloorZ : -FLT_MAX;
                Result.SweptFloorRadius = Result.bHasSweptFloor ? SupportRadius : 0.0f;
            }

        }
        return Result;
    }

    static FVFTunnelShapeEvaluation VF_EvaluateSweptTunnel(
        const FVector& Position,
        const FCachedTunnel& Tunnel,
        bool bWorldChain,
        float SDFBlendRadius,
        bool bApplyFloorCut,
        const void* CacheIdentity, int32 TunnelIndex,
        const FVFRoomFloorOwnership* RoomFloorOwnership = nullptr)
    {
        const FVFFloorReliefColumnKey ReliefColumn =
            VF_MakeFloorReliefColumnKey(
                static_cast<float>(Position.X), static_cast<float>(Position.Y));
        const TArray<FVector>& ShapeControlPoints = bWorldChain
            ? Tunnel.WorldControlPoints : Tunnel.ControlPoints;
        const TArray<float>& ShapeControlRadii = bWorldChain
            ? Tunnel.WorldControlRadii : Tunnel.ControlRadii;
        if (ShapeControlPoints.Num() >= 2
            && ShapeControlRadii.Num() == ShapeControlPoints.Num())
        {
            return VF_EvaluateSweptTunnelChain(
                Position, ShapeControlPoints, ShapeControlRadii,
                Tunnel, SDFBlendRadius, bApplyFloorCut,
                CacheIdentity, TunnelIndex, ReliefColumn,
                bWorldChain ? &Tunnel.WorldFloorProfiles : &Tunnel.FloorProfiles,
                RoomFloorOwnership);
        }

        if (Tunnel.bHasMidpoint)
        {
            TArray<FVector> Points;
            TArray<float> Radii;
            Points.Add(Tunnel.EndpointA);
            Points.Add(Tunnel.Midpoint);
            Points.Add(Tunnel.EndpointB);
            Radii.Add(Tunnel.RadiusA);
            Radii.Add(Tunnel.RadiusMid);
            Radii.Add(Tunnel.RadiusB);
            // The fallback descriptor is only retained for cooked/old cache compatibility.  The
            // temporary arrays stay inline and do not allocate on the normal chain path.
            return VF_EvaluateSweptTunnelChain(
                Position, Points, Radii,
                Tunnel, SDFBlendRadius, bApplyFloorCut,
                CacheIdentity, TunnelIndex, ReliefColumn,
                bWorldChain ? &Tunnel.WorldFloorProfiles : &Tunnel.FloorProfiles,
                RoomFloorOwnership);
        }

        TArray<FVector> Points;
        TArray<float> Radii;
        Points.Add(Tunnel.EndpointA);
        Points.Add(Tunnel.EndpointB);
        Radii.Add(Tunnel.RadiusA);
        Radii.Add(Tunnel.RadiusB);
        return VF_EvaluateSweptTunnelChain(
            Position, Points, Radii,
            Tunnel, SDFBlendRadius, bApplyFloorCut,
            CacheIdentity, TunnelIndex, ReliefColumn,
            bWorldChain ? &Tunnel.WorldFloorProfiles : &Tunnel.FloorProfiles,
            RoomFloorOwnership);
    }

}

void FChunkSDFSpatialIndex::Reset()
{
    MinChunkX = 0;
    MinChunkY = 0;
    NumCellsX = 0;
    NumCellsY = 0;
    bValid = false;
    CellOffsets.Reset();
    ItemIndices.Reset();
}

void FChunkSDFSpatialIndex::Release()
{
    MinChunkX = 0;
    MinChunkY = 0;
    NumCellsX = 0;
    NumCellsY = 0;
    bValid = false;
    CellOffsets.Empty();
    ItemIndices.Empty();
}

SIZE_T FChunkSDFSpatialIndex::GetAllocatedSize() const
{
    return static_cast<SIZE_T>(CellOffsets.GetAllocatedSize())
        + static_cast<SIZE_T>(ItemIndices.GetAllocatedSize());
}

bool FChunkSDFSpatialIndex::GetRange(
    float WorldX, float WorldY, int32& OutBegin, int32& OutEnd) const
{
    OutBegin = 0;
    OutEnd = 0;
    if (!bValid
        || !VoxelMath::IsFinite(WorldX) || !VoxelMath::IsFinite(WorldY)
        || NumCellsX <= 0 || NumCellsY <= 0)
    {
        return false;
    }

    const double ChunkXDouble = FMath::FloorToDouble(
        static_cast<double>(WorldX) / static_cast<double>(CHUNK_SIZE));
    const double ChunkYDouble = FMath::FloorToDouble(
        static_cast<double>(WorldY) / static_cast<double>(CHUNK_SIZE));
    if (ChunkXDouble < static_cast<double>(MIN_int32)
        || ChunkXDouble > static_cast<double>(MAX_int32)
        || ChunkYDouble < static_cast<double>(MIN_int32)
        || ChunkYDouble > static_cast<double>(MAX_int32))
    {
        return false;
    }

    const int64 LocalX = static_cast<int64>(ChunkXDouble) - static_cast<int64>(MinChunkX);
    const int64 LocalY = static_cast<int64>(ChunkYDouble) - static_cast<int64>(MinChunkY);
    if (LocalX < 0 || LocalX >= static_cast<int64>(NumCellsX)
        || LocalY < 0 || LocalY >= static_cast<int64>(NumCellsY))
    {
        // A point outside the indexed rectangle is uncommon for the requesting tile, but the
        // exact fallback is safer than assuming the index covers a malformed/legacy query.
        return false;
    }

    const int64 Slot = LocalY * static_cast<int64>(NumCellsX) + LocalX;
    if (Slot < 0 || Slot + 1 >= static_cast<int64>(CellOffsets.Num()))
    {
        return false;
    }

    const int32* CellOffsetData = CellOffsets.GetData();
    OutBegin = CellOffsetData[static_cast<int32>(Slot)];
    OutEnd = CellOffsetData[static_cast<int32>(Slot + 1)];
    return OutBegin >= 0 && OutEnd >= OutBegin && OutEnd <= ItemIndices.Num();
}

namespace
{
    struct FVFSpatialBounds
    {
        float MinX = 0.0f;
        float MinY = 0.0f;
        float MaxX = 0.0f;
        float MaxY = 0.0f;
    };

    static bool VF_SetSpatialBounds(
        float CenterX, float CenterY, float Radius, FVFSpatialBounds& OutBounds)
    {
        if (!VoxelMath::IsFinite(CenterX) || !VoxelMath::IsFinite(CenterY)
            || !VoxelMath::IsFinite(Radius) || Radius < 0.0f)
        {
            return false;
        }
        OutBounds.MinX = CenterX - Radius;
        OutBounds.MinY = CenterY - Radius;
        OutBounds.MaxX = CenterX + Radius;
        OutBounds.MaxY = CenterY + Radius;
        return VoxelMath::IsFinite(OutBounds.MinX) && VoxelMath::IsFinite(OutBounds.MinY)
            && VoxelMath::IsFinite(OutBounds.MaxX) && VoxelMath::IsFinite(OutBounds.MaxY)
            && OutBounds.MinX <= OutBounds.MaxX && OutBounds.MinY <= OutBounds.MaxY;
    }

    static bool VF_SetSpatialAabb(
        float MinX, float MinY, float MaxX, float MaxY,
        float Radius, FVFSpatialBounds& OutBounds)
    {
        if (!VoxelMath::IsFinite(MinX) || !VoxelMath::IsFinite(MinY)
            || !VoxelMath::IsFinite(MaxX) || !VoxelMath::IsFinite(MaxY)
            || !VoxelMath::IsFinite(Radius) || Radius < 0.0f
            || MinX > MaxX || MinY > MaxY)
        {
            return false;
        }
        OutBounds.MinX = MinX - Radius;
        OutBounds.MinY = MinY - Radius;
        OutBounds.MaxX = MaxX + Radius;
        OutBounds.MaxY = MaxY + Radius;
        return VoxelMath::IsFinite(OutBounds.MinX) && VoxelMath::IsFinite(OutBounds.MinY)
            && VoxelMath::IsFinite(OutBounds.MaxX) && VoxelMath::IsFinite(OutBounds.MaxY)
            && OutBounds.MinX <= OutBounds.MaxX && OutBounds.MinY <= OutBounds.MaxY;
    }

    static bool VF_IncludeSpatialBounds(
        FVFSpatialBounds& InOut, const FVFSpatialBounds& Other, bool& bHasBounds)
    {
        if (!bHasBounds)
        {
            InOut = Other;
            bHasBounds = true;
            return true;
        }
        InOut.MinX = FMath::Min(InOut.MinX, Other.MinX);
        InOut.MinY = FMath::Min(InOut.MinY, Other.MinY);
        InOut.MaxX = FMath::Max(InOut.MaxX, Other.MaxX);
        InOut.MaxY = FMath::Max(InOut.MaxY, Other.MaxY);
        return true;
    }

    template <typename FGetBounds>
    static void VF_BuildSpatialIndex(
        FChunkSDFSpatialIndex& OutIndex, int32 ItemCount, FGetBounds&& GetBounds)
    {
        OutIndex.Reset();
        if (ItemCount <= 0)
        {
            return;
        }

        // Never turn a malformed authored bound into a giant allocation.  A failed build is
        // deliberately indistinguishable from the unindexed linear scan to the density evaluator.
        constexpr int64 MaxBuckets = 1024 * 1024;
        constexpr int64 MaxReferences = 4 * 1024 * 1024;
        TArray<int32> ItemMinX;
        TArray<int32> ItemMinY;
        TArray<int32> ItemMaxX;
        TArray<int32> ItemMaxY;
        ItemMinX.SetNumUninitialized(ItemCount);
        ItemMinY.SetNumUninitialized(ItemCount);
        ItemMaxX.SetNumUninitialized(ItemCount);
        ItemMaxY.SetNumUninitialized(ItemCount);

        FVFSpatialBounds AllBounds;
        bool bHasBounds = false;
        int32 GlobalMinX = MAX_int32;
        int32 GlobalMinY = MAX_int32;
        int32 GlobalMaxX = MIN_int32;
        int32 GlobalMaxY = MIN_int32;
        int64 TotalReferences = 0;
        for (int32 ItemIndex = 0; ItemIndex < ItemCount; ++ItemIndex)
        {
            FVFSpatialBounds Bounds;
            if (!GetBounds(ItemIndex, Bounds))
            {
                return;
            }

            const double MinChunkXDouble = FMath::FloorToDouble(
                static_cast<double>(Bounds.MinX) / static_cast<double>(CHUNK_SIZE));
            const double MinChunkYDouble = FMath::FloorToDouble(
                static_cast<double>(Bounds.MinY) / static_cast<double>(CHUNK_SIZE));
            const double MaxChunkXDouble = FMath::FloorToDouble(
                static_cast<double>(Bounds.MaxX) / static_cast<double>(CHUNK_SIZE));
            const double MaxChunkYDouble = FMath::FloorToDouble(
                static_cast<double>(Bounds.MaxY) / static_cast<double>(CHUNK_SIZE));
            if (MinChunkXDouble < static_cast<double>(MIN_int32)
                || MinChunkXDouble > static_cast<double>(MAX_int32)
                || MinChunkYDouble < static_cast<double>(MIN_int32)
                || MinChunkYDouble > static_cast<double>(MAX_int32)
                || MaxChunkXDouble < static_cast<double>(MIN_int32)
                || MaxChunkXDouble > static_cast<double>(MAX_int32)
                || MaxChunkYDouble < static_cast<double>(MIN_int32)
                || MaxChunkYDouble > static_cast<double>(MAX_int32))
            {
                return;
            }

            const int32 MinX = static_cast<int32>(MinChunkXDouble);
            const int32 MinY = static_cast<int32>(MinChunkYDouble);
            const int32 MaxX = static_cast<int32>(MaxChunkXDouble);
            const int32 MaxY = static_cast<int32>(MaxChunkYDouble);
            if (MinX > MaxX || MinY > MaxY)
            {
                return;
            }

            ItemMinX[ItemIndex] = MinX;
            ItemMinY[ItemIndex] = MinY;
            ItemMaxX[ItemIndex] = MaxX;
            ItemMaxY[ItemIndex] = MaxY;
            GlobalMinX = FMath::Min(GlobalMinX, MinX);
            GlobalMinY = FMath::Min(GlobalMinY, MinY);
            GlobalMaxX = FMath::Max(GlobalMaxX, MaxX);
            GlobalMaxY = FMath::Max(GlobalMaxY, MaxY);
            TotalReferences += (static_cast<int64>(MaxX) - MinX + 1)
                * (static_cast<int64>(MaxY) - MinY + 1);
            if (TotalReferences > MaxReferences)
            {
                return;
            }
            VF_IncludeSpatialBounds(AllBounds, Bounds, bHasBounds);
        }

        const int64 NumCellsX64 = static_cast<int64>(GlobalMaxX) - GlobalMinX + 1;
        const int64 NumCellsY64 = static_cast<int64>(GlobalMaxY) - GlobalMinY + 1;
        if (!bHasBounds || NumCellsX64 <= 0 || NumCellsY64 <= 0
            || NumCellsX64 > MAX_int32 || NumCellsY64 > MAX_int32
            || NumCellsX64 * NumCellsY64 > MaxBuckets)
        {
            return;
        }

        OutIndex.MinChunkX = GlobalMinX;
        OutIndex.MinChunkY = GlobalMinY;
        OutIndex.NumCellsX = static_cast<int32>(NumCellsX64);
        OutIndex.NumCellsY = static_cast<int32>(NumCellsY64);
        const int64 BucketCount = NumCellsX64 * NumCellsY64;
        OutIndex.CellOffsets.SetNumZeroed(static_cast<int32>(BucketCount + 1));

        for (int32 ItemIndex = 0; ItemIndex < ItemCount; ++ItemIndex)
        {
            for (int32 Y = ItemMinY[ItemIndex]; Y <= ItemMaxY[ItemIndex]; ++Y)
            {
                const int64 Row = static_cast<int64>(Y - OutIndex.MinChunkY)
                    * static_cast<int64>(OutIndex.NumCellsX);
                for (int32 X = ItemMinX[ItemIndex]; X <= ItemMaxX[ItemIndex]; ++X)
                {
                    const int64 Slot = Row + static_cast<int64>(X - OutIndex.MinChunkX);
                    ++OutIndex.CellOffsets[static_cast<int32>(Slot + 1)];
                }
            }
        }
        for (int32 Slot = 0; Slot < static_cast<int32>(BucketCount); ++Slot)
        {
            OutIndex.CellOffsets[Slot + 1] += OutIndex.CellOffsets[Slot];
        }

        OutIndex.ItemIndices.SetNumUninitialized(static_cast<int32>(TotalReferences));
        TArray<int32> WriteOffsets = OutIndex.CellOffsets;
        for (int32 ItemIndex = 0; ItemIndex < ItemCount; ++ItemIndex)
        {
            for (int32 Y = ItemMinY[ItemIndex]; Y <= ItemMaxY[ItemIndex]; ++Y)
            {
                const int64 Row = static_cast<int64>(Y - OutIndex.MinChunkY)
                    * static_cast<int64>(OutIndex.NumCellsX);
                for (int32 X = ItemMinX[ItemIndex]; X <= ItemMaxX[ItemIndex]; ++X)
                {
                    const int64 Slot = Row + static_cast<int64>(X - OutIndex.MinChunkX);
                    OutIndex.ItemIndices[WriteOffsets[static_cast<int32>(Slot)]++] = ItemIndex;
                }
            }
        }
        OutIndex.bValid = true;
    }

    template <typename FVisit>
    static int32 VF_ForEachSpatialCandidate(
        const FChunkSDFSpatialIndex& Index, int32 ItemCount,
        float WorldX, float WorldY, FVisit&& Visit,
        bool bUseSpatialIndex = true)
    {
        int32 Begin = 0;
        int32 End = 0;
        int32 CandidateCount = 0;
        if (bUseSpatialIndex && Index.GetRange(WorldX, WorldY, Begin, End))
        {
            const int32* ItemIndexData = Index.ItemIndices.GetData();
            for (int32 Cursor = Begin; Cursor < End; ++Cursor)
            {
                const int32 ItemIndex = ItemIndexData[Cursor];
                if (ItemIndex >= 0 && ItemIndex < ItemCount)
                {
                    ++CandidateCount;
                    Visit(ItemIndex);
                }
            }
            return CandidateCount;
        }

        for (int32 ItemIndex = 0; ItemIndex < ItemCount; ++ItemIndex)
        {
            ++CandidateCount;
            Visit(ItemIndex);
        }
        return CandidateCount;
    }

    static bool VF_GetRoomSpatialBounds(int32, const FCachedRoom& Room, FVFSpatialBounds& OutBounds)
    {
        return VF_SetSpatialBounds(
            static_cast<float>(Room.Center.X), static_cast<float>(Room.Center.Y),
            FMath::Sqrt(FMath::Max(Room.CullRadiusSq, 0.0f)), OutBounds);
    }

    static float VF_CachedRoomShapeSDF(
        const FCachedRoom& Room, const FVector& Position)
    {
        switch (Room.ShapeType)
        {
        case 1:
            return VoxelSDF::RoundedBox(
                Position, Room.Center, Room.ShapeA, Room.ShapeR);
        case 2:
            return VoxelSDF::Capsule(
                Position, Room.ShapeA, Room.ShapeB, Room.ShapeR);
        default:
            return VoxelSDF::Ellipsoid(Position, Room.Center, Room.ShapeA);
        }
    }

    static bool VF_IsInsideCachedRoomShape(
        const FVector& Position,
        const FChunkSDFCache& Cache,
        bool bUseSpatialIndex)
    {
        bool bInside = false;
        auto VisitRoom = [&](int32 RoomIndex)
        {
            if (bInside || !Cache.Rooms.IsValidIndex(RoomIndex))
            {
                return;
            }
            const FCachedRoom& Room = Cache.Rooms[RoomIndex];
            if (VF_FloatDistSquared(
                    static_cast<float>(Position.X),
                    static_cast<float>(Position.Y),
                    static_cast<float>(Position.Z),
                    Room.Center) > Room.CullRadiusSq)
            {
                return;
            }
            bInside = VF_CachedRoomShapeSDF(Room, Position) < 0.0f;
        };
        VF_ForEachSpatialCandidate(
            Cache.RoomSpatialIndex, Cache.Rooms.Num(),
            static_cast<float>(Position.X), static_cast<float>(Position.Y),
            VisitRoom, bUseSpatialIndex);
        return bInside;
    }

    static bool VF_GetJoinSpatialBounds(
        int32, const FCachedRoomFloorJoin& Join, FVFSpatialBounds& OutBounds)
    {
        return VF_SetSpatialBounds(
            static_cast<float>(Join.BoundCenter.X), static_cast<float>(Join.BoundCenter.Y),
            FMath::Sqrt(FMath::Max(Join.BoundRadiusSq, 0.0f)), OutBounds);
    }

    static bool VF_GetTunnelSpatialBounds(
        int32, const FCachedTunnel& Tunnel, FVFSpatialBounds& OutBounds)
    {
        bool bHasBounds = false;
        auto IncludeCenterRadius = [&](float X, float Y, float Radius) -> bool
        {
            FVFSpatialBounds Bounds;
            if (!VF_SetSpatialBounds(X, Y, Radius, Bounds))
            {
                return false;
            }
            return VF_IncludeSpatialBounds(OutBounds, Bounds, bHasBounds);
        };
        auto IncludeAabbRadius = [&](const FVector& Min, const FVector& Max, float Radius) -> bool
        {
            FVFSpatialBounds Bounds;
            if (!VF_SetSpatialAabb(
                    static_cast<float>(Min.X), static_cast<float>(Min.Y),
                    static_cast<float>(Max.X), static_cast<float>(Max.Y),
                    FMath::Max(Radius, 0.0f), Bounds))
            {
                return false;
            }
            return VF_IncludeSpatialBounds(OutBounds, Bounds, bHasBounds);
        };

        if (!IncludeCenterRadius(
                static_cast<float>(Tunnel.BoundCenter.X),
                static_cast<float>(Tunnel.BoundCenter.Y),
                FMath::Sqrt(FMath::Max(Tunnel.BoundRadiusSq, 0.0f)))
            || !IncludeCenterRadius(
                static_cast<float>(Tunnel.WorldBoundCenter.X),
                static_cast<float>(Tunnel.WorldBoundCenter.Y),
                FMath::Sqrt(FMath::Max(Tunnel.WorldBoundRadiusSq, 0.0f)))
            || !IncludeAabbRadius(
                Tunnel.SDFCenterlineMin, Tunnel.SDFCenterlineMax,
                Tunnel.SDFInfluenceRadius)
            || !IncludeAabbRadius(
                Tunnel.WorldCenterlineMin, Tunnel.WorldCenterlineMax,
                Tunnel.WorldInfluenceRadius))
        {
            return false;
        }
        return bHasBounds;
    }

    static bool VF_GetPitSpatialBounds(
        int32, const FCachedPit& Pit, FVFSpatialBounds& OutBounds)
    {
        return VF_SetSpatialBounds(
            Pit.CenterX, Pit.CenterY,
            FMath::Sqrt(FMath::Max(Pit.BoundXYRadiusSq, 0.0f)), OutBounds);
    }

    static bool VF_GetChimneySpatialBounds(
        int32, const FCachedChimney& Chimney, FVFSpatialBounds& OutBounds)
    {
        return VF_SetSpatialBounds(
            Chimney.CenterX, Chimney.CenterY,
            FMath::Sqrt(FMath::Max(Chimney.BoundXYRadiusSq, 0.0f)), OutBounds);
    }

    static bool VF_GetColumnSpatialBounds(
        int32, const FCachedColumn& Column, FVFSpatialBounds& OutBounds)
    {
        return VF_SetSpatialBounds(
            Column.CenterX, Column.CenterY,
            FMath::Sqrt(FMath::Max(Column.BoundXYRadiusSq, 0.0f)), OutBounds);
    }
}

void FChunkSDFCache::Reset()
{
    // Keep the allocations: classifier windows can alternate between empty and
    // populated regions on adjacent requests.
    Rooms.Reset();
    RoomFloorJoins.Reset();
    for (FCachedTunnel& Tunnel : Tunnels)
    {
        Tunnel.FloorLedgeLevels.Reset();
        Tunnel.FloorProfiles.Reset();
        Tunnel.WorldFloorProfiles.Reset();
    }
    Tunnels.Reset();
    Pits.Reset();
    Chimneys.Reset();
    Columns.Reset();
    RoomSpatialIndex.Reset();
    RoomFloorJoinSpatialIndex.Reset();
    TunnelSpatialIndex.Reset();
    PitSpatialIndex.Reset();
    ChimneySpatialIndex.Reset();
    ColumnSpatialIndex.Reset();
    SDFBlendRadius = 0.0f;
}

void FChunkSDFCache::Release()
{
    Rooms.Empty();
    RoomFloorJoins.Empty();
    Tunnels.Empty();
    Pits.Empty();
    Chimneys.Empty();
    Columns.Empty();
    RoomSpatialIndex.Release();
    RoomFloorJoinSpatialIndex.Release();
    TunnelSpatialIndex.Release();
    PitSpatialIndex.Release();
    ChimneySpatialIndex.Release();
    ColumnSpatialIndex.Release();
}

SIZE_T FChunkSDFCache::GetAllocatedSize() const
{
    return static_cast<SIZE_T>(GetAllocatedSizeBreakdown().TotalBytes());
}

VoxelDensityProfile::FCacheMemoryBreakdown FChunkSDFCache::GetAllocatedSizeBreakdown() const
{
    VoxelDensityProfile::FCacheMemoryBreakdown Breakdown;
    auto ArrayBytes = [](const auto& Array) -> uint64
    {
        return static_cast<uint64>(Array.GetAllocatedSize());
    };

    Breakdown.RoomsBytes = ArrayBytes(Rooms);
    Breakdown.RoomFloorJoinsBytes = ArrayBytes(RoomFloorJoins);
    Breakdown.TunnelsBytes = ArrayBytes(Tunnels);
    Breakdown.PitsBytes = ArrayBytes(Pits);
    Breakdown.ChimneysBytes = ArrayBytes(Chimneys);
    Breakdown.ColumnsBytes = ArrayBytes(Columns);
    Breakdown.SpatialIndexBytes = static_cast<uint64>(
        RoomSpatialIndex.GetAllocatedSize()
        + RoomFloorJoinSpatialIndex.GetAllocatedSize()
        + TunnelSpatialIndex.GetAllocatedSize()
        + PitSpatialIndex.GetAllocatedSize()
        + ChimneySpatialIndex.GetAllocatedSize()
        + ColumnSpatialIndex.GetAllocatedSize());
    for (const FCachedTunnel& Tunnel : Tunnels)
    {
        Breakdown.TunnelsBytes += ArrayBytes(Tunnel.ControlPoints);
        Breakdown.TunnelsBytes += ArrayBytes(Tunnel.ControlRadii);
        Breakdown.TunnelsBytes += ArrayBytes(Tunnel.WorldControlPoints);
        Breakdown.TunnelsBytes += ArrayBytes(Tunnel.WorldControlRadii);
        Breakdown.TunnelsBytes += ArrayBytes(Tunnel.FloorLedgeLevels);
        Breakdown.TunnelsBytes += ArrayBytes(Tunnel.FloorProfiles);
        Breakdown.TunnelsBytes += ArrayBytes(Tunnel.WorldFloorProfiles);
    }

    for (const FCachedRoom& Room : Rooms)
    {
        Breakdown.RoomsBytes += ArrayBytes(Room.MouthRises);
    }

    return Breakdown;
}

//=============================================================================
// INTERNAL: Full room data used during cache building only.
// The CellX/CellY fields are needed for tunnel pair hashing but NOT for
// per-voxel SDF evaluation, so they don't go into the cached FCachedRoom.
//=============================================================================
struct FBuildRoom
{
    FVector Center;         // World position of the room center
    float RadiusXY;         // Horizontal radius
    float RadiusZ;          // Vertical radius
    int32 CellX, CellY;    // Grid cell (needed for pair hash during tunnel decisions)
    uint32 Hash;            // Cell hash (for shape selection + tunnel property derivation)
    bool bIsOrigin;         // True for the origin room at (0,0)
    bool bStore;            // True if this room can affect a voxel in THIS chunk
                            // (collected for connectivity decisions either way).
    FVector PlayerFitPoint = FVector::ZeroVector;
    bool bHasPlayerFitPoint = false;
    bool bPlayerFitAttempted = false;
};

struct FBuildRoomMouthRise
{
    int32 RoomIndex = INDEX_NONE;
    FVector Mouth = FVector::ZeroVector;
    float TargetFloorZ = 0.0f;
    float BlendRadius = 0.0f;
    float Strength = 0.0f;
};

struct FVFRoomGraphReach
{
    float DirectRoomReach = 0.0f;
    // Complete XY reach of a room body plus any authored terrain operation that can be selected
    // for that room. This is also used by the STORE cull; leaving it out would drop a room whose
    // pit/chimney/detail override extends beyond the base SDF body.
    float TerrainOpReach = 0.0f;
    float FloorReliefEnvelope = 0.0f;
    float BlendEnvelope = 0.0f;
    float MaxTunnelLength = 0.0f;
    float PairAabbReach = 0.0f;
    float PairZReach = 0.0f;
    float CollectMargin = 0.0f;
};

static float VF_RoomShapeReachUpperBound(float RadiusXY, float RadiusZ)
{
    const float RXY = FMath::Abs(RadiusXY);
    const float RZ = FMath::Abs(RadiusZ);
    const float Ellipsoid = FMath::Max(RXY, RZ);
    const FVector BoxExtent(
        RXY * 0.8f + RXY * 0.25f,
        RXY * 0.8f + RXY * 0.25f,
        RZ * 0.8f + RXY * 0.25f);
    const float RoundedBox = BoxExtent.Size();
    const float Capsule = RXY * 0.7f + FMath::Min(RXY * 0.6f, RZ);
    return FMath::Max3(Ellipsoid, RoundedBox, Capsule);
}

// Terrain operations are authored outside FStrateGenerationParams and are selected per room at
// cache-build time. The tile-window proof must therefore include both the transported params and
// every operation that the pool can select. The limit is deliberately finite: an authored feature
// beyond it uses the legacy worker window, while the fixed-margin proof remains valid for all
// assets that stay inside this envelope.
static constexpr float VF_MaxProvenTerrainOpExtent = 256.0f;

static bool VF_ComputeTerrainOpEnvelope(
    const FStrateGenerationParams& Params,
    const TArray<FStrateTerrainOpEntry>* TerrainOps,
    float BaseRoomReach,
    float AllRoomRadiusEnvelope,
    float BlendEnvelope,
    float& OutReach)
{
    float MaxReach = BaseRoomReach;
    float MaxAuthoredExtent = 0.0f;
    bool bHasActiveTerrainOperation = false;

    auto RecordAuthoredExtent = [&](float Extent) -> bool
    {
        if (!VoxelMath::IsFinite(Extent) || Extent < 0.0f)
        {
            return false;
        }
        bHasActiveTerrainOperation = true;
        MaxAuthoredExtent = FMath::Max(MaxAuthoredExtent, Extent);
        return MaxAuthoredExtent <= VF_MaxProvenTerrainOpExtent;
    };
    auto RecordReach = [&](float Reach) -> bool
    {
        if (!VoxelMath::IsFinite(Reach) || Reach < 0.0f)
        {
            return false;
        }
        MaxReach = FMath::Max(MaxReach, Reach);
        return VoxelMath::IsFinite(MaxReach);
    };
    auto RecordDetail = [&](float EffectRange) -> bool
    {
        return RecordAuthoredExtent(EffectRange)
            && RecordReach(BaseRoomReach + EffectRange);
    };
    auto RecordStructural = [&](float EffectExtent, float FeatureReach) -> bool
    {
        return RecordAuthoredExtent(EffectExtent)
            && RecordReach(FeatureReach);
    };

    auto AccumulateParams = [&](const FStrateGenerationParams& OpParams) -> bool
    {
        const float OperationValues[] = {
            OpParams.TerraceStepHeight,
            OpParams.TerraceHardness,
            OpParams.TerraceNoiseDisplacement,
            OpParams.LayerLineSpacing,
            OpParams.LayerLineDepth,
            OpParams.OverhangStrength,
            OpParams.OverhangDepth,
            OpParams.OverhangFrequency,
            OpParams.RibbingSpacing,
            OpParams.RibbingDepth,
            OpParams.CliffStrength,
            OpParams.ScallopStrength,
            OpParams.ScallopFrequency,
            OpParams.ArchDensity,
            OpParams.ArchMinRadius,
            OpParams.ArchMaxRadius,
            OpParams.ColumnDensity,
            OpParams.ColumnMinRadius,
            OpParams.ColumnMaxRadius,
            OpParams.PitDensity,
            OpParams.PitMinRadius,
            OpParams.PitMaxRadius,
            OpParams.PitDepth,
            OpParams.ChimneyDensity,
            OpParams.ChimneyMinRadius,
            OpParams.ChimneyMaxRadius,
            OpParams.ChimneyHeight,
            OpParams.DomeDensity,
            OpParams.DomeMinRadius,
            OpParams.DomeMaxRadius,
            OpParams.DomeHeightRatio,
            OpParams.PinchDensity,
            OpParams.PinchStrength,
            OpParams.PinchLength};
        for (const float Value : OperationValues)
        {
            if (!VoxelMath::IsFinite(Value))
            {
                return false;
            }
        }

        if (OpParams.TerraceStepHeight > 0.0f)
        {
            if (OpParams.TerraceHardness < 0.0f || OpParams.TerraceHardness > 1.0f
                || OpParams.TerraceNoiseDisplacement < 0.0f)
            {
                return false;
            }
            if (!RecordDetail(
                    OpParams.TerraceStepHeight * 3.0f
                    + OpParams.TerraceNoiseDisplacement + 4.0f))
            {
                return false;
            }
        }
        if (OpParams.LayerLineSpacing > 0.0f)
        {
            if (OpParams.LayerLineDepth < 0.0f
                || !RecordDetail(OpParams.LayerLineSpacing
                    + OpParams.LayerLineDepth + 4.0f))
            {
                return false;
            }
        }
        if (OpParams.RibbingSpacing > 0.0f)
        {
            if (OpParams.RibbingDepth < 0.0f
                || !RecordDetail(OpParams.RibbingSpacing
                    + OpParams.RibbingDepth + 4.0f))
            {
                return false;
            }
        }
        if (OpParams.CliffStrength > 0.0f)
        {
            if (!RecordDetail(OpParams.CliffStrength + 8.0f))
            {
                return false;
            }
        }
        if (OpParams.ScallopStrength > 0.0f)
        {
            if (OpParams.ScallopFrequency <= 0.0f
                || !RecordDetail(
                    4.0f / OpParams.ScallopFrequency
                    + OpParams.ScallopStrength + 4.0f))
            {
                return false;
            }
        }
        if (OpParams.OverhangStrength > 0.0f)
        {
            if (OpParams.OverhangDepth < 0.0f
                || OpParams.OverhangFrequency <= 0.0f
                || !RecordDetail(OpParams.OverhangDepth * 2.0f + 4.0f))
            {
                return false;
            }
        }
        if (OpParams.ArchDensity > 0.0f)
        {
            if (OpParams.ArchMinRadius < 0.0f || OpParams.ArchMaxRadius < 0.0f)
            {
                return false;
            }
            const float Radius = FMath::Max(OpParams.ArchMinRadius, OpParams.ArchMaxRadius);
            if (!RecordStructural(
                    Radius + 4.0f,
                    AllRoomRadiusEnvelope * 1.2f + Radius + BlendEnvelope * 3.0f + 4.0f))
            {
                return false;
            }
        }
        if (OpParams.ColumnDensity > 0.0f)
        {
            if (OpParams.ColumnMinRadius < 0.0f || OpParams.ColumnMaxRadius < 0.0f)
            {
                return false;
            }
            const float Radius = FMath::Max(OpParams.ColumnMinRadius, OpParams.ColumnMaxRadius);
            if (!RecordStructural(
                    Radius + 6.0f,
                    AllRoomRadiusEnvelope * 0.75f + Radius + 6.0f))
            {
                return false;
            }
        }
        if (OpParams.PitDensity > 0.0f)
        {
            if (OpParams.PitMinRadius < 0.0f || OpParams.PitMaxRadius < 0.0f
                || OpParams.PitDepth < 0.0f)
            {
                return false;
            }
            const float Radius = FMath::Max(OpParams.PitMinRadius, OpParams.PitMaxRadius);
            if (!RecordStructural(
                    2.0f * Radius + OpParams.PitDepth + BlendEnvelope + 4.0f,
                    AllRoomRadiusEnvelope * 0.6f + 2.0f * Radius
                        + OpParams.PitDepth + BlendEnvelope + 4.0f))
            {
                return false;
            }
        }
        if (OpParams.ChimneyDensity > 0.0f)
        {
            if (OpParams.ChimneyMinRadius < 0.0f || OpParams.ChimneyMaxRadius < 0.0f
                || OpParams.ChimneyHeight < 0.0f)
            {
                return false;
            }
            const float Radius = FMath::Max(
                OpParams.ChimneyMinRadius, OpParams.ChimneyMaxRadius);
            if (!RecordStructural(
                    2.0f * Radius + OpParams.ChimneyHeight + BlendEnvelope + 4.0f,
                    AllRoomRadiusEnvelope * 0.6f + 2.0f * Radius
                        + OpParams.ChimneyHeight + BlendEnvelope + 4.0f))
            {
                return false;
            }
        }
        if (OpParams.DomeDensity > 0.0f)
        {
            if (OpParams.DomeMinRadius < 0.0f || OpParams.DomeMaxRadius < 0.0f
                || OpParams.DomeHeightRatio < 0.0f)
            {
                return false;
            }
            const float Radius = FMath::Max(OpParams.DomeMinRadius, OpParams.DomeMaxRadius);
            if (!RecordStructural(
                    Radius * (1.0f + OpParams.DomeHeightRatio) + 4.0f,
                    AllRoomRadiusEnvelope * 0.4f + Radius
                        * (1.0f + OpParams.DomeHeightRatio) + 4.0f))
            {
                return false;
            }
        }
        if (OpParams.PinchDensity > 0.0f)
        {
            if (OpParams.PinchStrength < 0.0f || OpParams.PinchLength < 0.0f)
            {
                return false;
            }
            const float Extent = FMath::Max(OpParams.PinchStrength, OpParams.PinchLength);
            if (!RecordStructural(
                    Extent + 5.0f,
                    AllRoomRadiusEnvelope * 0.85f + Extent + 5.0f))
            {
                return false;
            }
        }
        return true;
    };

    if (!AccumulateParams(Params))
    {
        return false;
    }

    if (TerrainOps != nullptr)
    {
        for (const FStrateTerrainOpEntry& Entry : *TerrainOps)
        {
            if (!VoxelMath::IsFinite(Entry.Probability)
                || Entry.Probability < 0.0f || Entry.Probability > 1.0f
                || !VoxelMath::IsFinite(Entry.Weight)
                || Entry.Weight < 0.0f || Entry.Weight > 3.0f)
            {
                return false;
            }
            if (Entry.Probability <= 0.0f || Entry.Weight <= 0.0f)
            {
                continue;
            }
            if (Entry.Operation.IsNull())
            {
                continue;
            }
            const UVoxelTerrainOpDefinition* Operation = Entry.Operation.Get();
            if (Operation == nullptr)
            {
                // A referenced but unloaded asset is not a proof of absence. Keep the legacy
                // window until the manager has a concrete definition to inspect.
                return false;
            }
            switch (Operation->Type)
            {
            case EVoxelTerrainOpType::Terrace:
            case EVoxelTerrainOpType::LayerLines:
            case EVoxelTerrainOpType::Ribbing:
            case EVoxelTerrainOpType::Cliff:
            case EVoxelTerrainOpType::Scallop:
            case EVoxelTerrainOpType::Overhang:
            case EVoxelTerrainOpType::Arch:
            case EVoxelTerrainOpType::Column:
            case EVoxelTerrainOpType::Pit:
            case EVoxelTerrainOpType::Chimney:
            case EVoxelTerrainOpType::Dome:
            case EVoxelTerrainOpType::Pinch:
                break;
            default:
                return false;
            }

            FStrateGenerationParams OperationParams{};
            Operation->ApplyTo(OperationParams, Entry.Weight);
            if (!AccumulateParams(OperationParams))
            {
                return false;
            }
        }
    }

    // Keep the field at zero when the strate has no active terrain operation.  The caller already
    // has the base room envelope; using that same value as a second global room radius would make
    // the legacy/no-op path store extra rooms and could change the canonical field.  Active pools
    // publish the wider envelope so the tile proof and STORE cull see pits/chimneys/detail reach.
    OutReach = bHasActiveTerrainOperation ? MaxReach : 0.0f;
    return VoxelMath::IsFinite(OutReach)
        && VoxelMath::IsFinite(MaxAuthoredExtent)
        && MaxAuthoredExtent <= VF_MaxProvenTerrainOpExtent;
}

static bool VF_ComputeRoomGraphReach(
    const FStrateGenerationParams& Params,
    const TArray<FStrateTerrainOpEntry>* TerrainOps,
    FVFRoomGraphReach& OutReach)
{
    const float RelevantParams[] = {
        Params.RoomDensity,
        Params.RoomSpacing,
        Params.MinRoomRadius,
        Params.MaxRoomRadius,
        Params.RoomHeightRatio,
        Params.RoomFloorCutMin,
        Params.RoomFloorCutMax,
        Params.FloorReliefStrength,
        Params.FloorReliefFrequency,
        Params.RoomMouthRiseStrength,
        Params.RoomMouthRiseBlendVoxels,
        Params.OriginRoomRadius,
        Params.TunnelMinRadius,
        Params.TunnelMaxRadius,
        Params.TunnelDensity,
        Params.MaxTunnelLength,
        Params.TunnelWarpStrength,
        Params.TunnelHorizontalBias,
        Params.TunnelEndpointZOffset,
        Params.TunnelFloorTerraceStepHeight,
        Params.TunnelFloorMaxLedgeHeight,
        Params.TunnelFloorGentleSlopeThreshold,
        Params.SDFBlendRadius,
        Params.CaveWarpStrength,
        Params.CaveWarpFrequency,
        Params.VerticalScale,
        Params.BoundarySealThickness,
        Params.StrateTopWorldZ,
        Params.StrateBottomWorldZ};
    for (const float Value : RelevantParams)
    {
        if (!VoxelMath::IsFinite(Value))
        {
            return false;
        }
    }

    if (Params.RoomSpacing <= 0.0f
        || Params.RoomDensity < 0.0f
        || Params.MinRoomRadius < 0.0f
        || Params.MaxRoomRadius < 0.0f
        || Params.RoomHeightRatio < 0.0f
        || Params.OriginRoomRadius < 0.0f
        || Params.TunnelMinRadius < 0.0f
        || Params.TunnelMaxRadius < 0.0f
        || Params.SDFBlendRadius < 0.0f
        || Params.TunnelFloorTerraceStepHeight < 0.0f
        || Params.TunnelFloorMaxLedgeHeight < 0.0f
        || Params.TunnelFloorGentleSlopeThreshold < 0.0f
        || Params.TunnelFloorGentleSlopeThreshold
            > VoxelPassageGeometry::TunnelFloorGentleSlopeMaximumDegrees
        || FMath::Abs(Params.FloorReliefStrength) > 1000000.0f
        || FMath::Abs(Params.CaveWarpStrength) > 1000000.0f
        || FMath::Abs(Params.RoomFloorCutMin) > 1000000.0f
        || FMath::Abs(Params.RoomFloorCutMax) > 1000000.0f
        || FMath::Max(Params.MaxTunnelLength, 0.0f) > 1000000.0f)
    {
        return false;
    }

    // The cache's room centres and the world-space tunnel chain have separate
    // Z conventions when VerticalScale != 1. A proof that ignores that
    // conversion could under-estimate the chain length used for its lateral
    // wander. Let the normal cache path handle those authored variants.
    if (Params.VerticalScale > 0.0f
        && FMath::Abs(Params.VerticalScale - 1.0f) > 1.0e-6f)
    {
        return false;
    }

    const float RoomRadiusEnvelope = FMath::Max(
        FMath::Abs(Params.MinRoomRadius),
        FMath::Abs(Params.MaxRoomRadius));
    const float AllRoomRadiusEnvelope = FMath::Max(
        RoomRadiusEnvelope, FMath::Abs(Params.OriginRoomRadius));
    const float AllRoomHeightEnvelope = AllRoomRadiusEnvelope
        * FMath::Abs(Params.RoomHeightRatio);
    const float FloorReliefEnvelope = FMath::Abs(Params.FloorReliefStrength)
        * VOXEL_NOISE_SCALE * 1.5f;
    const float BlendEnvelope = FMath::Max(Params.SDFBlendRadius, 0.0f);
    const float TunnelRadiusEnvelope = FMath::Max(
        0.5f,
        FMath::Max(
            FMath::Abs(Params.TunnelMinRadius),
            FMath::Abs(Params.TunnelMaxRadius)) * 1.18f);
    const float MaxTunnelLength = FMath::Max(
        Params.MaxTunnelLength, 0.0f);
    const float BaseDirectRoomReach = VF_RoomShapeReachUpperBound(
        AllRoomRadiusEnvelope, AllRoomHeightEnvelope)
        + FloorReliefEnvelope + BlendEnvelope * 3.0f;
    float TerrainOpReach = BaseDirectRoomReach;
    if (!VF_ComputeTerrainOpEnvelope(
            Params, TerrainOps, BaseDirectRoomReach,
            AllRoomRadiusEnvelope, BlendEnvelope, TerrainOpReach))
    {
        return false;
    }
    const float DirectRoomReach = FMath::Max(BaseDirectRoomReach, TerrainOpReach);
    const float TunnelInfluence = FMath::Abs(Params.TunnelWarpStrength)
        + TunnelRadiusEnvelope + BlendEnvelope * 3.0f;
    const float MaxInfluence = FMath::Max(DirectRoomReach, TunnelInfluence);

    // A fitted tunnel mouth may move by one bounded cave-warp displacement in
    // each XY axis. The chain may also grow a deterministic switchback when a
    // direct room-floor route exceeds the gentle-slope limit. Bound the
    // endpoint's possible Z correction too, because it controls the required
    // route length even though the preflight is XY-only.
    const float CaveWarpRaw = FMath::Abs(Params.CaveWarpStrength)
        * VOXEL_NOISE_SCALE;
    const float CaveWarpEnvelope = CaveWarpRaw * 1.5f;
    const float EndpointXY = FMath::Sqrt(2.0f)
        * CaveWarpEnvelope;
    const float FloorCutEnvelope = FMath::Max(
        1.0f,
        FMath::Max(
            FMath::Abs(Params.RoomFloorCutMin),
            FMath::Abs(Params.RoomFloorCutMax)));
    const float ReliefSearch = static_cast<float>(FMath::Clamp(
        FMath::CeilToInt(FMath::Abs(Params.FloorReliefStrength)
            * VOXEL_NOISE_SCALE + CaveWarpRaw) + 8,
        8,
        128));
    const float MaxStepHeight = FVoxelPlayerCapsuleConstants::MaxStepHeightMeters
        / FVoxelPlayerCapsuleConstants::VoxelSizeMeters;
    const float EndpointVertical = AllRoomHeightEnvelope * FloorCutEnvelope
        + FloorReliefEnvelope + CaveWarpEnvelope + ReliefSearch
        + MaxStepHeight + TunnelRadiusEnvelope + 2.0f;
    const float EndpointOffset = FMath::Sqrt(
        EndpointXY * EndpointXY
        + EndpointVertical * EndpointVertical);
    const float TunnelLengthBound = MaxTunnelLength
        + 2.0f * EndpointOffset;
    const float WorldFloorPad = FMath::Max(
        VoxelPassageGeometry::LandingFloorThicknessVoxels,
        BlendEnvelope);
    const float TunnelTubeReach = TunnelRadiusEnvelope
        + WorldFloorPad + BlendEnvelope * 3.0f
        + CaveWarpEnvelope + 2.0f;
    // A steep room-floor delta is now solved by a deterministic lateral route rather than by
    // quantising the floor into small risers.  Bound the route's possible lateral amplitude with
    // the same finite endpoint envelope used above.  This is intentionally conservative: the
    // actual chain uses a handful of switchback waves and never exceeds this reach.
    const float GentleSlopeThreshold = VoxelPassageGeometry::TunnelFloorGradientFromDegrees(
        Params.TunnelFloorGentleSlopeThreshold);
    const float MaxFloorDeltaEnvelope = 2.0f * EndpointVertical;
    const float RequiredRouteRunEnvelope = FMath::Min(
        VoxelPassageGeometry::TunnelFloorMaximumRouteLengthVoxels,
        MaxFloorDeltaEnvelope / FMath::Max(
            GentleSlopeThreshold, KINDA_SMALL_NUMBER));
    const float OrganicLateralEnvelope = FMath::Min(
        FMath::Abs(Params.TunnelWarpStrength), TunnelLengthBound * 0.25f);
    const float RouteLateralEnvelope = FMath::Max(
        8.0f,
        OrganicLateralEnvelope + RequiredRouteRunEnvelope);
    const float PairAabbReach = EndpointXY
        + RouteLateralEnvelope
        + FMath::Max(MaxInfluence, TunnelTubeReach);
    const float PairZReach = EndpointVertical + TunnelTubeReach;
    // A stored feature can depend on a nearest-neighbour candidate two graph hops away: A's
    // candidate B can select C, and both decisions affect the edge emitted near the search box.
    // PairAabbReach is the complete endpoint/chain/tube envelope; using it here closes the old
    // proof gap where the collect margin only covered the smaller direct influence estimate.
    const float CollectMargin = 2.0f * MaxTunnelLength
        + FMath::Max(DirectRoomReach, PairAabbReach);

    if (!VoxelMath::IsFinite(DirectRoomReach)
        || !VoxelMath::IsFinite(PairAabbReach)
        || !VoxelMath::IsFinite(PairZReach)
        || !VoxelMath::IsFinite(CollectMargin)
        || DirectRoomReach < 0.0f
        || PairAabbReach < 0.0f
        || PairZReach < 0.0f
        || CollectMargin < 0.0f)
    {
        return false;
    }

    OutReach.DirectRoomReach = DirectRoomReach;
    OutReach.TerrainOpReach = TerrainOpReach;
    OutReach.FloorReliefEnvelope = FloorReliefEnvelope;
    OutReach.BlendEnvelope = BlendEnvelope;
    OutReach.MaxTunnelLength = MaxTunnelLength;
    OutReach.PairAabbReach = PairAabbReach;
    OutReach.PairZReach = PairZReach;
    OutReach.CollectMargin = CollectMargin;
    return true;
}

template <typename AllocatorType>
static bool VF_MayHaveRoomGraphFeature(
    const TArray<FBuildRoom, AllocatorType>& Rooms,
    float SearchMinX, float SearchMinY,
    float SearchMaxX, float SearchMaxY,
    const FStrateGenerationParams& Params,
    uint32 StrateSeed,
    const FVFRoomGraphReach& Reach,
    bool bUseZ = false,
    float SearchMinZ = 0.0f,
    float SearchMaxZ = 0.0f)
{
    auto AabbDistanceSquared = [&](
        float X, float Y, float MinX, float MinY,
        float MaxX, float MaxY) -> float
    {
        const float DX = X < MinX ? MinX - X
            : (X > MaxX ? X - MaxX : 0.0f);
        const float DY = Y < MinY ? MinY - Y
            : (Y > MaxY ? Y - MaxY : 0.0f);
        return DX * DX + DY * DY;
    };

    for (const FBuildRoom& Room : Rooms)
    {
        if (!VoxelMath::IsFinite(Room.Center.X)
            || !VoxelMath::IsFinite(Room.Center.Y)
            || !VoxelMath::IsFinite(Room.Center.Z)
            || !VoxelMath::IsFinite(Room.RadiusXY)
            || !VoxelMath::IsFinite(Room.RadiusZ))
        {
            return true;
        }
        const float BaseRoomReach = VF_RoomShapeReachUpperBound(
            Room.RadiusXY, Room.RadiusZ)
            + Reach.FloorReliefEnvelope + Reach.BlendEnvelope * 3.0f;
        const float RoomReach = FMath::Max(BaseRoomReach, Reach.TerrainOpReach);
        if (!VoxelMath::IsFinite(RoomReach) || RoomReach < 0.0f)
        {
            return true;
        }
        if (bUseZ
            && (static_cast<float>(Room.Center.Z) + RoomReach < SearchMinZ
                || static_cast<float>(Room.Center.Z) - RoomReach > SearchMaxZ))
        {
            continue;
        }
        const float DistanceSquared = AabbDistanceSquared(
            static_cast<float>(Room.Center.X),
            static_cast<float>(Room.Center.Y),
            SearchMinX - RoomReach,
            SearchMinY - RoomReach,
            SearchMaxX + RoomReach,
            SearchMaxY + RoomReach);
        if (!VoxelMath::IsFinite(DistanceSquared))
        {
            return true;
        }
        if (DistanceSquared <= KINDA_SMALL_NUMBER)
        {
            return true;
        }
    }

    if (Reach.MaxTunnelLength <= 0.0f || Rooms.Num() < 2)
    {
        return false;
    }

    // A pair whose broad centerline envelope can reach the query is enough to
    // keep the full graph path. This deliberately treats every geometrically
    // eligible pair as potential; the exact backbone/random decision remains
    // owned by BuildChunkCache. It keeps this preflight linear in the common
    // feature-present case and avoids player-fit work in the feature-free case.
    for (int32 First = 0; First < Rooms.Num(); ++First)
    {
        for (int32 Second = First + 1; Second < Rooms.Num(); ++Second)
        {
            const FVector& FirstCenter = Rooms[First].Center;
            const FVector& SecondCenter = Rooms[Second].Center;
            if (bUseZ
                && (FMath::Max(
                        static_cast<float>(FirstCenter.Z),
                        static_cast<float>(SecondCenter.Z))
                        + Reach.PairZReach < SearchMinZ
                    || FMath::Min(
                        static_cast<float>(FirstCenter.Z),
                        static_cast<float>(SecondCenter.Z))
                        - Reach.PairZReach > SearchMaxZ))
            {
                continue;
            }
            const float EuclideanDistance = static_cast<float>(
                FVector::Dist(FirstCenter, SecondCenter));
            if (!VoxelMath::IsFinite(EuclideanDistance))
            {
                return true;
            }
            if (EuclideanDistance > Reach.MaxTunnelLength)
            {
                continue;
            }
            const float PairMinX = FMath::Min(
                static_cast<float>(FirstCenter.X),
                static_cast<float>(SecondCenter.X))
                - Reach.PairAabbReach;
            const float PairMinY = FMath::Min(
                static_cast<float>(FirstCenter.Y),
                static_cast<float>(SecondCenter.Y))
                - Reach.PairAabbReach;
            const float PairMaxX = FMath::Max(
                static_cast<float>(FirstCenter.X),
                static_cast<float>(SecondCenter.X))
                + Reach.PairAabbReach;
            const float PairMaxY = FMath::Max(
                static_cast<float>(FirstCenter.Y),
                static_cast<float>(SecondCenter.Y))
                + Reach.PairAabbReach;
            if (!VoxelMath::IsFinite(PairMinX) || !VoxelMath::IsFinite(PairMinY)
                || !VoxelMath::IsFinite(PairMaxX) || !VoxelMath::IsFinite(PairMaxY))
            {
                return true;
            }
            if (PairMaxX >= SearchMinX && PairMaxY >= SearchMinY
                && PairMinX <= SearchMaxX && PairMinY <= SearchMaxY)
            {
                return true;
            }
        }
    }
    return false;

#if 0
    const float MaxTunnelLengthSquared =
        Reach.MaxTunnelLength * Reach.MaxTunnelLength;
    if (!VoxelMath::IsFinite(MaxTunnelLengthSquared))
    {
        return true;
    }

    TArray<int32, TInlineAllocator<128>> NearestNeighbor;
    NearestNeighbor.SetNumUninitialized(Rooms.Num());
    const bool bFlowToOrigin = Params.bTunnelsFlowTowardOrigin;
    auto OriginKeySquared = [&](int32 Index) -> float
    {
        const FVector& Center = Rooms[Index].Center;
        return static_cast<float>(
            Center.X * Center.X + Center.Y * Center.Y);
    };
    auto LinkMetric = [&](int32 First, int32 Second) -> float
    {
        const float Distance = static_cast<float>(
            FVector::Dist(Rooms[First].Center, Rooms[Second].Center));
        const float VerticalSeparation = FMath::Abs(
            static_cast<float>(
                Rooms[First].Center.Z - Rooms[Second].Center.Z));
        return Distance
            + VerticalSeparation * Params.TunnelHorizontalBias * 5.0f;
    };
    auto PickNeighbor = [&](int32 Index) -> int32
    {
        const float MyKeySquared = OriginKeySquared(Index);
        float BestInward = FLT_MAX;
        int32 BestInwardIndex = -1;
        float BestAny = FLT_MAX;
        int32 BestAnyIndex = -1;
        for (int32 Other = 0; Other < Rooms.Num(); ++Other)
        {
            if (Other == Index)
            {
                continue;
            }
            const float DistanceSquared = static_cast<float>(
                FVector::DistSquared(
                    Rooms[Index].Center, Rooms[Other].Center));
            if (!VoxelMath::IsFinite(DistanceSquared))
            {
                return -2;
            }
            if (DistanceSquared > MaxTunnelLengthSquared)
            {
                continue;
            }
            const float Metric = LinkMetric(Index, Other);
            if (!VoxelMath::IsFinite(Metric))
            {
                return -2;
            }
            if (Metric < BestAny)
            {
                BestAny = Metric;
                BestAnyIndex = Other;
            }
            if (bFlowToOrigin
                && OriginKeySquared(Other) < MyKeySquared
                && Metric < BestInward)
            {
                BestInward = Metric;
                BestInwardIndex = Other;
            }
        }
        return (bFlowToOrigin && BestInwardIndex != -1)
            ? BestInwardIndex : BestAnyIndex;
    };
    for (int32 Index = 0; Index < Rooms.Num(); ++Index)
    {
        NearestNeighbor[Index] = PickNeighbor(Index);
        if (NearestNeighbor[Index] == -2)
        {
            return true;
        }
    }

    for (int32 First = 0; First < Rooms.Num(); ++First)
    {
        for (int32 Second = First + 1; Second < Rooms.Num(); ++Second)
        {
            const float EuclideanDistance = static_cast<float>(
                FVector::Dist(Rooms[First].Center, Rooms[Second].Center));
            if (!VoxelMath::IsFinite(EuclideanDistance))
            {
                return true;
            }
            bool bBackbone = NearestNeighbor[First] == Second
                || NearestNeighbor[Second] == First;
            float CheckDistance = EuclideanDistance;
            if (!bBackbone && Params.TunnelHorizontalBias > 0.0f)
            {
                const float VerticalSeparation = FMath::Abs(
                    static_cast<float>(
                        Rooms[First].Center.Z - Rooms[Second].Center.Z));
                CheckDistance += VerticalSeparation
                    * Params.TunnelHorizontalBias * 5.0f;
            }
            if (!VoxelMath::IsFinite(CheckDistance)
                || CheckDistance > Reach.MaxTunnelLength)
            {
                continue;
            }
            if (!bBackbone)
            {
                if (Params.TunnelDensity <= 0.0f)
                {
                    continue;
                }
                const uint32 PairHash = VoxelHash::Pair(
                    Rooms[First].CellX, Rooms[First].CellY,
                    Rooms[Second].CellX, Rooms[Second].CellY,
                    StrateSeed);
                if (VoxelHash::ToFloat01(PairHash) >= Params.TunnelDensity)
                {
                    continue;
                }
            }

            const float PairMinX = FMath::Min(
                static_cast<float>(Rooms[First].Center.X),
                static_cast<float>(Rooms[Second].Center.X))
                - Reach.PairAabbReach;
            const float PairMinY = FMath::Min(
                static_cast<float>(Rooms[First].Center.Y),
                static_cast<float>(Rooms[Second].Center.Y))
                - Reach.PairAabbReach;
            const float PairMaxX = FMath::Max(
                static_cast<float>(Rooms[First].Center.X),
                static_cast<float>(Rooms[Second].Center.X))
                + Reach.PairAabbReach;
            const float PairMaxY = FMath::Max(
                static_cast<float>(Rooms[First].Center.Y),
                static_cast<float>(Rooms[Second].Center.Y))
                + Reach.PairAabbReach;
            if (!VoxelMath::IsFinite(PairMinX) || !VoxelMath::IsFinite(PairMinY)
                || !VoxelMath::IsFinite(PairMaxX) || !VoxelMath::IsFinite(PairMaxY))
            {
                return true;
            }
            if (PairMaxX >= SearchMinX && PairMaxY >= SearchMinY
                && PairMinX <= SearchMaxX && PairMinY <= SearchMaxY)
            {
                // A backbone link is considered potential even if the origin
                // connection cap later downgrades it. That only sacrifices a
                // skip; it can never turn a real tunnel into an empty proof.
                return true;
            }
        }
    }
    return false;
#endif
}

//=============================================================================
// INTERNAL: Shared hash-placement skeleton for the per-room baked features
// (pits / chimneys / columns). Squelette commun de placement par hash — les
// trois boucles de bake étaient des copies quasi identiques de ce motif.
//
// Rolls EXACTLY the hash chain the hand-written loops used (bit-identical):
//   H  = Mix(RoomHash ^ (SaltBase + i * SaltStep))  → density gate
//   H2 = Mix(H ^ Salt2)                             → XY offset (X: H2, Y: Mix(H2))
//   H3 = Mix(H2 ^ Salt3)                            → radius lerp [MinRadius, MaxRadius]
// Type-specific work (Z anchor, flare, bounds, struct fill) lives in the Emit
// lambda; it receives H3 so pits/chimneys can chain their 4th hash from it.
//=============================================================================
template <typename FEmit>
static void BakeRoomFeature(
    const FCachedRoom& CR,
    int32 MaxCount, float Density,
    uint32 SaltBase, uint32 SaltStep, uint32 Salt2, uint32 Salt3,
    float XYScale, float MinRadius, float MaxRadius,
    FEmit&& Emit)   // Emit(X, Y, Radius, H3)
{
    if (Density <= 0.0f) return;

    for (int32 i = 0; i < MaxCount; i++)
    {
        const uint32 H = VoxelHash::Mix(CR.Hash ^ (SaltBase + (uint32)i * SaltStep));
        if (VoxelHash::ToFloat01(H) > Density) continue;

        const uint32 H2 = VoxelHash::Mix(H ^ Salt2);
        const uint32 H3 = VoxelHash::Mix(H2 ^ Salt3);

        const float X = CR.Center.X + VoxelHash::ToFloatSigned(H2) * CR.RadiusXY * XYScale;
        const float Y = CR.Center.Y + VoxelHash::ToFloatSigned(VoxelHash::Mix(H2)) * CR.RadiusXY * XYScale;
        const float R = FMath::Lerp(MinRadius, MaxRadius, VoxelHash::ToFloat01(H3));

        Emit(X, Y, R, H3);
    }
}

    static void VF_FinalizeTunnelBroadPhaseBounds(FCachedTunnel& Tunnel, float BlendK)
    {
        const float BlendPad = FMath::Max(BlendK, 0.0f);
        // Detail modifiers run while CaveSDF is below 3*K, so the source
        // broad phase must cover that full downstream reach.
        const float SDFDetailPad = BlendPad * 3.0f;
        const float WorldFloorPad = FMath::Max(
            VoxelPassageGeometry::LandingFloorThicknessVoxels, BlendPad);

        if (Tunnel.ControlPoints.Num() >= 2
            && Tunnel.ControlRadii.Num() == Tunnel.ControlPoints.Num())
        {
            Tunnel.SDFCenterlineMin = Tunnel.ControlPoints[0];
            Tunnel.SDFCenterlineMax = Tunnel.ControlPoints[0];
            float MaxRadius = 0.0f;
            for (int32 Index = 0; Index < Tunnel.ControlPoints.Num(); ++Index)
            {
                const FVector& Point = Tunnel.ControlPoints[Index];
                Tunnel.SDFCenterlineMin.X = FMath::Min(Tunnel.SDFCenterlineMin.X, Point.X);
                Tunnel.SDFCenterlineMin.Y = FMath::Min(Tunnel.SDFCenterlineMin.Y, Point.Y);
                Tunnel.SDFCenterlineMin.Z = FMath::Min(Tunnel.SDFCenterlineMin.Z, Point.Z);
                Tunnel.SDFCenterlineMax.X = FMath::Max(Tunnel.SDFCenterlineMax.X, Point.X);
                Tunnel.SDFCenterlineMax.Y = FMath::Max(Tunnel.SDFCenterlineMax.Y, Point.Y);
                Tunnel.SDFCenterlineMax.Z = FMath::Max(Tunnel.SDFCenterlineMax.Z, Point.Z);
                MaxRadius = FMath::Max(MaxRadius, FMath::Abs(Tunnel.ControlRadii[Index]));
            }
            Tunnel.SDFInfluenceRadius = MaxRadius + SDFDetailPad;
        }
        else
        {
            Tunnel.SDFCenterlineMin = Tunnel.EndpointA;
            Tunnel.SDFCenterlineMax = Tunnel.EndpointA;
            Tunnel.SDFCenterlineMin.X = FMath::Min(Tunnel.SDFCenterlineMin.X, Tunnel.EndpointB.X);
            Tunnel.SDFCenterlineMin.Y = FMath::Min(Tunnel.SDFCenterlineMin.Y, Tunnel.EndpointB.Y);
            Tunnel.SDFCenterlineMin.Z = FMath::Min(Tunnel.SDFCenterlineMin.Z, Tunnel.EndpointB.Z);
            Tunnel.SDFCenterlineMax.X = FMath::Max(Tunnel.SDFCenterlineMax.X, Tunnel.EndpointB.X);
            Tunnel.SDFCenterlineMax.Y = FMath::Max(Tunnel.SDFCenterlineMax.Y, Tunnel.EndpointB.Y);
            Tunnel.SDFCenterlineMax.Z = FMath::Max(Tunnel.SDFCenterlineMax.Z, Tunnel.EndpointB.Z);
            float MaxRadius = FMath::Max(
                FMath::Abs(Tunnel.RadiusA), FMath::Abs(Tunnel.RadiusB));
            if (Tunnel.bHasMidpoint)
            {
                Tunnel.SDFCenterlineMin.X = FMath::Min(Tunnel.SDFCenterlineMin.X, Tunnel.Midpoint.X);
                Tunnel.SDFCenterlineMin.Y = FMath::Min(Tunnel.SDFCenterlineMin.Y, Tunnel.Midpoint.Y);
                Tunnel.SDFCenterlineMin.Z = FMath::Min(Tunnel.SDFCenterlineMin.Z, Tunnel.Midpoint.Z);
                Tunnel.SDFCenterlineMax.X = FMath::Max(Tunnel.SDFCenterlineMax.X, Tunnel.Midpoint.X);
                Tunnel.SDFCenterlineMax.Y = FMath::Max(Tunnel.SDFCenterlineMax.Y, Tunnel.Midpoint.Y);
                Tunnel.SDFCenterlineMax.Z = FMath::Max(Tunnel.SDFCenterlineMax.Z, Tunnel.Midpoint.Z);
                MaxRadius = FMath::Max(MaxRadius, FMath::Abs(Tunnel.RadiusMid));
            }
            Tunnel.SDFInfluenceRadius = MaxRadius + SDFDetailPad;
        }

        if (Tunnel.WorldControlPoints.Num() >= 2
            && Tunnel.WorldControlRadii.Num() == Tunnel.WorldControlPoints.Num())
        {
            Tunnel.WorldCenterlineMin = Tunnel.WorldControlPoints[0];
            Tunnel.WorldCenterlineMax = Tunnel.WorldControlPoints[0];
            float MaxRadius = 0.0f;
            for (int32 Index = 0; Index < Tunnel.WorldControlPoints.Num(); ++Index)
            {
                const FVector& Point = Tunnel.WorldControlPoints[Index];
                Tunnel.WorldCenterlineMin.X = FMath::Min(Tunnel.WorldCenterlineMin.X, Point.X);
                Tunnel.WorldCenterlineMin.Y = FMath::Min(Tunnel.WorldCenterlineMin.Y, Point.Y);
                Tunnel.WorldCenterlineMin.Z = FMath::Min(Tunnel.WorldCenterlineMin.Z, Point.Z);
                Tunnel.WorldCenterlineMax.X = FMath::Max(Tunnel.WorldCenterlineMax.X, Point.X);
                Tunnel.WorldCenterlineMax.Y = FMath::Max(Tunnel.WorldCenterlineMax.Y, Point.Y);
                Tunnel.WorldCenterlineMax.Z = FMath::Max(Tunnel.WorldCenterlineMax.Z, Point.Z);
                MaxRadius = FMath::Max(MaxRadius, FMath::Abs(Tunnel.WorldControlRadii[Index]));
            }
            Tunnel.WorldInfluenceRadius = MaxRadius + WorldFloorPad;
        }
        else
        {
            Tunnel.WorldCenterlineMin = Tunnel.SDFCenterlineMin;
            Tunnel.WorldCenterlineMax = Tunnel.SDFCenterlineMax;
            Tunnel.WorldInfluenceRadius = Tunnel.SDFInfluenceRadius
                - SDFDetailPad + WorldFloorPad;
        }
    }

    static void VF_FinalizeCacheBroadPhaseBounds(
        FChunkSDFCache& Cache,
        float BlendK)
    {
        for (int32 Index = 0; Index < Cache.Tunnels.Num(); ++Index)
        {
            FCachedTunnel& Tunnel = Cache.Tunnels[Index];
            VF_FinalizeTunnelBroadPhaseBounds(Tunnel, BlendK);
        }

        VF_BuildSpatialIndex(
            Cache.RoomSpatialIndex, Cache.Rooms.Num(),
            [&Cache](int32 Index, FVFSpatialBounds& Bounds)
            {
                return VF_GetRoomSpatialBounds(Index, Cache.Rooms[Index], Bounds);
            });
        VF_BuildSpatialIndex(
            Cache.RoomFloorJoinSpatialIndex, Cache.RoomFloorJoins.Num(),
            [&Cache](int32 Index, FVFSpatialBounds& Bounds)
            {
                return VF_GetJoinSpatialBounds(Index, Cache.RoomFloorJoins[Index], Bounds);
            });
        VF_BuildSpatialIndex(
            Cache.TunnelSpatialIndex, Cache.Tunnels.Num(),
            [&Cache](int32 Index, FVFSpatialBounds& Bounds)
            {
                return VF_GetTunnelSpatialBounds(Index, Cache.Tunnels[Index], Bounds);
            });
        VF_BuildSpatialIndex(
            Cache.PitSpatialIndex, Cache.Pits.Num(),
            [&Cache](int32 Index, FVFSpatialBounds& Bounds)
            {
                return VF_GetPitSpatialBounds(Index, Cache.Pits[Index], Bounds);
            });
        VF_BuildSpatialIndex(
            Cache.ChimneySpatialIndex, Cache.Chimneys.Num(),
            [&Cache](int32 Index, FVFSpatialBounds& Bounds)
            {
                return VF_GetChimneySpatialBounds(Index, Cache.Chimneys[Index], Bounds);
            });
        VF_BuildSpatialIndex(
            Cache.ColumnSpatialIndex, Cache.Columns.Num(),
            [&Cache](int32 Index, FVFSpatialBounds& Bounds)
            {
                return VF_GetColumnSpatialBounds(Index, Cache.Columns[Index], Bounds);
            });
    }
namespace
{
    // Runtime landing queries use the same pinned capsule dimensions and support rules as the
    // player-fit measurement, but they cannot call the editor-only flood fill.  Keep this local
    // evaluator deliberately source-only: the callback is pure density math supplied by the
    // archetype query, never a generator, manager, cache, or layout lookup.
    struct FVFPlayerCapsuleStencilRow
    {
        int32 RelativeZ = 0;
        TArray<FIntPoint> HorizontalOffsets;
    };

    bool VF_IsPointInsidePlayerCapsule(
        float RelativeZ,
        int32 OffsetX,
        int32 OffsetY,
        float Radius,
        float HalfHeight)
    {
        const float AxisHalfLength = FMath::Max(0.0f, HalfHeight - Radius);
        const float DistanceToAxis = FMath::Max(
            FMath::Abs(RelativeZ) - AxisHalfLength, 0.0f);
        const float DistanceSquared = static_cast<float>(OffsetX * OffsetX + OffsetY * OffsetY)
            + DistanceToAxis * DistanceToAxis;
        return DistanceSquared <= Radius * Radius + KINDA_SMALL_NUMBER;
    }

    bool VF_BuildPlayerFitStencil(
        const FVoxelStrateMeasureSettings& Settings,
        TArray<FVFPlayerCapsuleStencilRow>& OutRows,
        TArray<FIntPoint>& OutSupportOffsets,
        int32& OutCentreSupportIndex,
        int32& OutRequiredSupportCount)
    {
        OutRows.Reset();
        OutSupportOffsets.Reset();
        OutCentreSupportIndex = INDEX_NONE;
        OutRequiredSupportCount = 0;

        if (!VoxelMath::IsFinite(Settings.PlayerCapsuleRadiusVoxels)
            || Settings.PlayerCapsuleRadiusVoxels <= 0.0f
            || !VoxelMath::IsFinite(Settings.PlayerCapsuleHalfHeightVoxels)
            || Settings.PlayerCapsuleHalfHeightVoxels <= 0.0f
            || !VoxelMath::IsFinite(Settings.PlayerMaxStepHeightMeters)
            || Settings.PlayerMaxStepHeightMeters < 0.0f
            || !VoxelMath::IsFinite(Settings.PlayerWalkableFloorAngleDegrees)
            || Settings.PlayerWalkableFloorAngleDegrees < 0.0f
            || Settings.PlayerWalkableFloorAngleDegrees > 90.0f
            || !VoxelMath::IsFinite(Settings.PlayerSupportPatchMinCoverageFraction)
            || Settings.PlayerSupportPatchMinCoverageFraction <= 0.0f
            || Settings.PlayerSupportPatchMinCoverageFraction > 1.0f)
        {
            return false;
        }

        const int32 HeightCells = FMath::CeilToInt(
            2.0f * Settings.PlayerCapsuleHalfHeightVoxels);
        const int32 MaxHorizontalOffset = FMath::CeilToInt(
            Settings.PlayerCapsuleRadiusVoxels);
        if (HeightCells <= 0 || HeightCells > 4096
            || MaxHorizontalOffset < 0 || MaxHorizontalOffset > 4096)
        {
            return false;
        }

        int64 NumStencilOffsets = 0;
        for (int32 RelativeZ = 0; RelativeZ < HeightCells; ++RelativeZ)
        {
            FVFPlayerCapsuleStencilRow& Row = OutRows.AddDefaulted_GetRef();
            Row.RelativeZ = RelativeZ;
            const float CapsuleRelativeZ = static_cast<float>(RelativeZ) + 0.5f
                - Settings.PlayerCapsuleHalfHeightVoxels;
            for (int32 OffsetY = -MaxHorizontalOffset;
                 OffsetY <= MaxHorizontalOffset;
                 ++OffsetY)
            {
                for (int32 OffsetX = -MaxHorizontalOffset;
                     OffsetX <= MaxHorizontalOffset;
                     ++OffsetX)
                {
                    if (!VF_IsPointInsidePlayerCapsule(
                            CapsuleRelativeZ, OffsetX, OffsetY,
                            Settings.PlayerCapsuleRadiusVoxels,
                            Settings.PlayerCapsuleHalfHeightVoxels))
                    {
                        continue;
                    }
                    if (NumStencilOffsets >= (1ll << 20))
                    {
                        return false;
                    }
                    Row.HorizontalOffsets.Add(FIntPoint(OffsetX, OffsetY));
                    ++NumStencilOffsets;
                }
            }

            // Preserve the top partial row exactly as the measurement does.  A row with no
            // rounded-cap offsets still needs a centre sample for full-height clearance.
            if (Row.HorizontalOffsets.IsEmpty())
            {
                Row.HorizontalOffsets.Add(FIntPoint::ZeroValue);
            }
        }

        const int32 MaxSupportOffset = FMath::CeilToInt(
            Settings.PlayerCapsuleRadiusVoxels);
        for (int32 OffsetY = -MaxSupportOffset;
             OffsetY <= MaxSupportOffset;
             ++OffsetY)
        {
            for (int32 OffsetX = -MaxSupportOffset;
                 OffsetX <= MaxSupportOffset;
                 ++OffsetX)
            {
                if (static_cast<float>(OffsetX * OffsetX + OffsetY * OffsetY)
                    > FMath::Square(Settings.PlayerCapsuleRadiusVoxels)
                        + KINDA_SMALL_NUMBER)
                {
                    continue;
                }
                if (OffsetX == 0 && OffsetY == 0)
                {
                    OutCentreSupportIndex = OutSupportOffsets.Num();
                }
                OutSupportOffsets.Add(FIntPoint(OffsetX, OffsetY));
            }
        }

        if (OutRows.IsEmpty() || OutSupportOffsets.IsEmpty()
            || OutCentreSupportIndex == INDEX_NONE)
        {
            return false;
        }
        OutRequiredSupportCount = FMath::Clamp(
            FMath::CeilToInt(
                Settings.PlayerSupportPatchMinCoverageFraction
                    * static_cast<float>(OutSupportOffsets.Num())),
            1,
            OutSupportOffsets.Num());
        return true;
    }

    bool VF_GetPlayerFitInteriorBounds(
        const FVoxelStrateMeasureSettings& Settings,
        float StrateTopZ,
        float StrateBottomZ,
        float BoundarySealThickness,
        float& OutInnerTop,
        float& OutInnerBottom)
    {
        if (!VoxelMath::IsFinite(StrateTopZ)
            || !VoxelMath::IsFinite(StrateBottomZ)
            || !VoxelMath::IsFinite(BoundarySealThickness)
            || BoundarySealThickness < 0.0f
            || StrateTopZ <= StrateBottomZ)
        {
            return false;
        }

        int32 InteriorMarginVoxels = Settings.InteriorMarginVoxels;
        if (InteriorMarginVoxels < 0)
        {
            const double DerivedMargin = 2.0
                * static_cast<double>(BoundarySealThickness);
            if (!VoxelMath::IsFinite(DerivedMargin)
                || DerivedMargin > static_cast<double>(INT32_MAX))
            {
                return false;
            }
            InteriorMarginVoxels = FMath::CeilToInt(static_cast<float>(DerivedMargin));
        }
        if (InteriorMarginVoxels < 0 || InteriorMarginVoxels > 4096)
        {
            return false;
        }

        OutInnerBottom = StrateBottomZ + static_cast<float>(InteriorMarginVoxels);
        OutInnerTop = StrateTopZ - static_cast<float>(InteriorMarginVoxels);
        return VoxelMath::IsFinite(OutInnerTop)
            && VoxelMath::IsFinite(OutInnerBottom)
            && OutInnerTop > OutInnerBottom;
    }

    bool VF_ValidatePlayerFitPose(
        const FVoxelStrateMeasureSettings& Settings,
        const FVector& CandidateFeetPoint,
        float StrateTopZ,
        float StrateBottomZ,
        float BoundarySealThickness,
        FVFPlayerDensitySampler SampleDensity,
        FVector& OutPoint)
    {
        OutPoint = FVector::ZeroVector;
        if (!VoxelMath::IsFinite(CandidateFeetPoint.X)
            || !VoxelMath::IsFinite(CandidateFeetPoint.Y)
            || !VoxelMath::IsFinite(CandidateFeetPoint.Z)
            || !VoxelMath::IsFinite(StrateTopZ)
            || !VoxelMath::IsFinite(StrateBottomZ)
            || !VoxelMath::IsFinite(BoundarySealThickness)
            || BoundarySealThickness < 0.0f
            || StrateTopZ <= StrateBottomZ)
        {
            return false;
        }

        TArray<FVFPlayerCapsuleStencilRow> StencilRows;
        TArray<FIntPoint> SupportOffsets;
        int32 CentreSupportIndex = INDEX_NONE;
        int32 RequiredSupportCount = 0;
        if (!VF_BuildPlayerFitStencil(
                Settings, StencilRows, SupportOffsets,
                CentreSupportIndex, RequiredSupportCount))
        {
            return false;
        }

        const double MaxStepHeightVoxelsReal =
            static_cast<double>(Settings.PlayerMaxStepHeightMeters)
            / static_cast<double>(FVoxelPlayerCapsuleConstants::VoxelSizeMeters);
        if (!VoxelMath::IsFinite(MaxStepHeightVoxelsReal)
            || MaxStepHeightVoxelsReal < 0.0
            || MaxStepHeightVoxelsReal > 4096.0)
        {
            return false;
        }
        const int32 MaxDownwardSearchCells = FMath::CeilToInt(
            static_cast<float>(MaxStepHeightVoxelsReal));
        const float MaxStepHeightVoxels = static_cast<float>(MaxStepHeightVoxelsReal);
        const float MinimumWalkableNormalZ = VoxelMath::DetCos(FMath::DegreesToRadians(
            Settings.PlayerWalkableFloorAngleDegrees));

        TArray<float> SupportHeights;
        SupportHeights.SetNumUninitialized(SupportOffsets.Num());
        TArray<uint8> bSupported;
        bSupported.Init(0u, SupportOffsets.Num());
        int32 NumSupported = 0;
        for (int32 SupportIndex = 0; SupportIndex < SupportOffsets.Num(); ++SupportIndex)
        {
            const FIntPoint& Offset = SupportOffsets[SupportIndex];
            for (int32 DownwardCell = 0;
                 DownwardCell <= MaxDownwardSearchCells;
                 ++DownwardCell)
            {
                const float UpperZ = CandidateFeetPoint.Z
                    - static_cast<float>(DownwardCell);
                const float LowerZ = UpperZ - 1.0f;
                const float LowerDensity = SampleDensity(
                    CandidateFeetPoint.X + static_cast<float>(Offset.X),
                    CandidateFeetPoint.Y + static_cast<float>(Offset.Y),
                    LowerZ);
                const float UpperDensity = SampleDensity(
                    CandidateFeetPoint.X + static_cast<float>(Offset.X),
                    CandidateFeetPoint.Y + static_cast<float>(Offset.Y),
                    UpperZ);
                if (!VoxelMath::IsFinite(LowerDensity)
                    || !VoxelMath::IsFinite(UpperDensity)
                    || !(LowerDensity <= 0.0f)
                    || !(UpperDensity > 0.0f))
                {
                    continue;
                }

                const float Denominator = UpperDensity - LowerDensity;
                if (!VoxelMath::IsFinite(Denominator) || Denominator <= 0.0f)
                {
                    continue;
                }
                const float Fraction = FMath::Clamp(
                    -LowerDensity / Denominator, 0.0f, 1.0f);
                const float SurfaceHeight = LowerZ + Fraction;
                const float CandidateBottom = CandidateFeetPoint.Z - 0.5f;
                if (!VoxelMath::IsFinite(SurfaceHeight)
                    || SurfaceHeight > CandidateBottom + KINDA_SMALL_NUMBER
                    || SurfaceHeight < CandidateBottom - MaxStepHeightVoxels
                        - KINDA_SMALL_NUMBER)
                {
                    continue;
                }

                bSupported[SupportIndex] = 1u;
                SupportHeights[SupportIndex] = SurfaceHeight;
                ++NumSupported;
                break;
            }
        }

        if (NumSupported < RequiredSupportCount
            || bSupported[CentreSupportIndex] == 0u)
        {
            return false;
        }

        float SupportHeight = -FLT_MAX;
        int32 SupportCount = 0;
        for (int32 Index = 0; Index < SupportOffsets.Num(); ++Index)
        {
            if (bSupported[Index] == 0u) continue;
            ++SupportCount;
            SupportHeight = FMath::Max(SupportHeight, SupportHeights[Index]);
        }
        if (SupportCount < RequiredSupportCount || !VoxelMath::IsFinite(SupportHeight))
        {
            return false;
        }

        float MaximumGradient = 0.0f;
        for (int32 First = 0; First < SupportOffsets.Num(); ++First)
        {
            if (bSupported[First] == 0u) continue;
            for (int32 Second = First + 1; Second < SupportOffsets.Num(); ++Second)
            {
                if (bSupported[Second] == 0u) continue;
                const float DX = static_cast<float>(
                    SupportOffsets[Second].X - SupportOffsets[First].X);
                const float DY = static_cast<float>(
                    SupportOffsets[Second].Y - SupportOffsets[First].Y);
                const float HorizontalDistance = FMath::Sqrt(DX * DX + DY * DY);
                if (HorizontalDistance <= KINDA_SMALL_NUMBER) continue;
                MaximumGradient = FMath::Max(
                    MaximumGradient,
                    FMath::Abs(SupportHeights[Second] - SupportHeights[First])
                        / HorizontalDistance);
            }
        }
        const float SupportNormalZ = 1.0f / FMath::Sqrt(
            1.0f + MaximumGradient * MaximumGradient);
        if (!VoxelMath::IsFinite(SupportNormalZ)
            || SupportNormalZ + KINDA_SMALL_NUMBER < MinimumWalkableNormalZ)
        {
            return false;
        }

        // Re-evaluate every occupied stencil sample at the chosen support, so an overhang cannot
        // masquerade as a floor. The returned point is the free centre immediately above that
        // resolved support, which is the stable standing/footing point used by passage mouths.
        for (const FVFPlayerCapsuleStencilRow& Row : StencilRows)
        {
            const float SampleZ = SupportHeight
                + static_cast<float>(Row.RelativeZ) + 0.5f;
            for (const FIntPoint& Offset : Row.HorizontalOffsets)
            {
                const float Density = SampleDensity(
                    CandidateFeetPoint.X + static_cast<float>(Offset.X),
                    CandidateFeetPoint.Y + static_cast<float>(Offset.Y),
                    SampleZ);
                if (!VoxelMath::IsFinite(Density) || !(Density > 0.0f))
                {
                    return false;
                }
            }
        }

        float InnerTop = 0.0f;
        float InnerBottom = 0.0f;
        if (!VF_GetPlayerFitInteriorBounds(
                Settings, StrateTopZ, StrateBottomZ, BoundarySealThickness,
                InnerTop, InnerBottom))
        {
            return false;
        }
        const float OutputZ = SupportHeight + 0.5f;
        const float OccupiedTop = SupportHeight
            + 2.0f * Settings.PlayerCapsuleHalfHeightVoxels;
        if (!VoxelMath::IsFinite(InnerBottom) || !VoxelMath::IsFinite(InnerTop)
            || !VoxelMath::IsFinite(OutputZ) || !VoxelMath::IsFinite(OccupiedTop)
            || OutputZ <= InnerBottom
            || OccupiedTop >= InnerTop)
        {
            return false;
        }

        OutPoint = FVector(CandidateFeetPoint.X, CandidateFeetPoint.Y, OutputZ);
        return !OutPoint.ContainsNaN()
            && VoxelMath::IsFinite(OutPoint.X)
            && VoxelMath::IsFinite(OutPoint.Y)
            && VoxelMath::IsFinite(OutPoint.Z);
    }

    struct FVFRoomLandingSite
    {
        FVector Center = FVector::ZeroVector;
        float RadiusXY = 0.0f;
        float RadiusZ = 0.0f;
        uint32 Hash = 0;
        bool bOrigin = false;
    };

    // The mouth searches ask the same room question dozens of times while only the query point
    // changes.  Keep the room constants in a small build-local descriptor so the search pays the
    // hash/shape/trig/floor setup once per endpoint, exactly as the per-voxel cache already does
    // for FCachedRoom.  This is deliberately separate from FCachedRoom: BuildRoomMouth runs before
    // the emitted cache exists.
    struct FVFPreparedRoomLanding
    {
        FVector Center = FVector::ZeroVector;
        FVector ShapeA = FVector::ZeroVector;
        FVector ShapeB = FVector::ZeroVector;
        float ShapeR = 0.0f;
        uint8 ShapeType = 0;

        bool bHasFloorCut = false;
        float FloorCutZ = -FLT_MAX;
        float FloorBlend = 0.0f;
        float FloorReliefStrength = 0.0f;
        float FloorReliefFrequency = 0.0f;
        uint32 FloorSeed = 0;
        int32 FloorFeatureIndex = INDEX_NONE;
        float FloorReliefBound = 0.0f;
        bool bHasFloorReliefBound = false;
    };

    static bool VF_PrepareRoomLanding(
        const FVFRoomLandingSite& Site,
        const FStrateGenerationParams& Params,
        FVFPreparedRoomLanding& OutQuery)
    {
        if (!VoxelMath::IsFinite(Site.Center.X)
            || !VoxelMath::IsFinite(Site.Center.Y)
            || !VoxelMath::IsFinite(Site.Center.Z)
            || !VoxelMath::IsFinite(Site.RadiusXY)
            || !VoxelMath::IsFinite(Site.RadiusZ)
            || !VoxelMath::IsFinite(Params.RoomShapeVariety)
            || !VoxelMath::IsFinite(Params.RoomFloorCutMin)
            || !VoxelMath::IsFinite(Params.RoomFloorCutMax)
            || !VoxelMath::IsFinite(Params.SDFBlendRadius)
            || !VoxelMath::IsFinite(Params.FloorReliefStrength)
            || !VoxelMath::IsFinite(Params.FloorReliefFrequency))
        {
            return false;
        }

        OutQuery = FVFPreparedRoomLanding();
        OutQuery.Center = Site.Center;

        const uint32 ShapeHash = VoxelHash::Mix(Site.Hash ^ 0xDEADBEEFu);
        const float ShapeRoll = Site.bOrigin ? 0.0f : VoxelHash::ToFloat01(ShapeHash);
        const float BoxThreshold = 1.0f - Params.RoomShapeVariety * 0.5f;
        const float CapsuleThreshold = 1.0f - Params.RoomShapeVariety * 0.2f;
        if (ShapeRoll >= BoxThreshold && ShapeRoll < CapsuleThreshold)
        {
            OutQuery.ShapeType = 1;
            OutQuery.ShapeA = FVector(
                Site.RadiusXY * 0.8f,
                Site.RadiusXY * 0.8f,
                Site.RadiusZ * 0.8f);
            OutQuery.ShapeR = Site.RadiusXY * 0.25f;
        }
        else if (ShapeRoll >= CapsuleThreshold)
        {
            OutQuery.ShapeType = 2;
            const float DirectionAngle = VoxelHash::ToFloat01(
                VoxelHash::Mix(Site.Hash ^ 0xCAFEBABEu)) * 2.0f * PI;
            const float StretchDistance = Site.RadiusXY * 0.7f;
            float SinDirection = 0.0f, CosDirection = 0.0f;
            VoxelMath::DetSinCos(SinDirection, CosDirection, DirectionAngle);
            const FVector Direction(CosDirection, SinDirection, 0.0f);
            OutQuery.ShapeA = Site.Center + Direction * StretchDistance;
            OutQuery.ShapeB = Site.Center - Direction * StretchDistance;
            OutQuery.ShapeR = FMath::Min(Site.RadiusXY * 0.6f, Site.RadiusZ);
        }
        else
        {
            OutQuery.ShapeType = 0;
            OutQuery.ShapeA = FVector(Site.RadiusXY, Site.RadiusXY, Site.RadiusZ);
        }

        const float FloorRoll = VoxelHash::ToFloat01(
            VoxelHash::Mix(Site.Hash ^ 0xF100F2u));
        const float FloorCut = FMath::Lerp(
            FMath::Min(Params.RoomFloorCutMin, Params.RoomFloorCutMax),
            FMath::Max(Params.RoomFloorCutMin, Params.RoomFloorCutMax),
            FloorRoll);
        if (FloorCut < 1.0f)
        {
            OutQuery.bHasFloorCut = true;
            OutQuery.FloorCutZ = Site.Center.Z - Site.RadiusZ * FloorCut;
            OutQuery.FloorBlend = Params.SDFBlendRadius * 0.35f;
            OutQuery.FloorReliefStrength = Params.FloorReliefStrength;
            OutQuery.FloorReliefFrequency = Params.FloorReliefFrequency;
            OutQuery.FloorSeed = VoxelHash::Mix(Site.Hash ^ 0xF100F1u);
            OutQuery.FloorFeatureIndex = static_cast<int32>(Site.Hash);
            OutQuery.bHasFloorReliefBound = VF_GetFloorReliefBound(
                OutQuery.FloorReliefStrength, 1.0f, 1.0f,
                OutQuery.FloorReliefFrequency, OutQuery.FloorReliefBound);
        }
        return true;
    }

    FORCEINLINE float VF_EvaluatePreparedRoomShapeSDF(
        const FVFPreparedRoomLanding& Query, const FVector& Position)
    {
        switch (Query.ShapeType)
        {
        case 1:
            return VoxelSDF::RoundedBox(
                Position, Query.Center, Query.ShapeA, Query.ShapeR);
        case 2:
            return VoxelSDF::Capsule(
                Position, Query.ShapeA, Query.ShapeB, Query.ShapeR);
        default:
            return VoxelSDF::Ellipsoid(
                Position, Query.Center, Query.ShapeA);
        }
    }

    FVector VF_ApplyCaveWarp(
        const FVector& WorldPoint,
        const FStrateGenerationParams& Params,
        uint32 Seed)
    {
        if (VoxelDensityAblation::IsCaveWarpOff())
        {
            return WorldPoint;
        }
        float EffectiveZ = WorldPoint.Z;
        if (Params.VerticalScale > 0.0f && Params.VerticalScale != 1.0f)
        {
            EffectiveZ = WorldPoint.Z / Params.VerticalScale;
        }

        FVector Warped(WorldPoint.X, WorldPoint.Y, EffectiveZ);
        if (Params.CaveWarpStrength <= 0.0f)
        {
            return Warped;
        }

        const float Frequency = Params.CaveWarpFrequency;
        const float Strength = Params.CaveWarpStrength;
        float WarpX, WarpY, WarpZ;
        VoxelNoise::Perlin3D_x3(
            WorldPoint.X * Frequency + VoxelHash::SeedOffset(Seed, 0.37f),
            WorldPoint.Y * Frequency + 1.3f,
            EffectiveZ * Frequency + 5.7f,
            WorldPoint.X * Frequency + 7.1f,
            WorldPoint.Y * Frequency + VoxelHash::SeedOffset(Seed, 0.59f),
            EffectiveZ * Frequency + 2.3f,
            WorldPoint.X * Frequency + 11.3f,
            WorldPoint.Y * Frequency + 9.7f,
            EffectiveZ * Frequency + VoxelHash::SeedOffset(Seed, 0.41f),
            WarpX, WarpY, WarpZ);
        Warped.X += WarpX * VOXEL_NOISE_SCALE * Strength;
        Warped.Y += WarpY * VOXEL_NOISE_SCALE * Strength;
        Warped.Z += WarpZ * VOXEL_NOISE_SCALE * Strength;
        return Warped;
    }

    FVector VF_UnwarpCavePoint(
        const FVector& TargetSDFPoint,
        const FStrateGenerationParams& Params,
        uint32 Seed)
    {
        FVector WorldPoint = TargetSDFPoint;
        const float ZScale = Params.VerticalScale > 0.0f
            ? Params.VerticalScale : 1.0f;
        if (ZScale != 1.0f)
        {
            WorldPoint.Z *= ZScale;
        }
        if (Params.CaveWarpStrength <= 0.0f)
        {
            return WorldPoint;
        }

        // The default warp is a contraction at its authored frequency. A fixed eight-step
        // correction is enough to put a world landing back on the cached SDF point while keeping
        // the construction path allocation-free and deterministic.
        for (int32 Iteration = 0; Iteration < 8; ++Iteration)
        {
            const FVector Mapped = VF_ApplyCaveWarp(WorldPoint, Params, Seed);
            const FVector Error = TargetSDFPoint - Mapped;
            if (!Error.ContainsNaN()
                && VoxelMath::IsFinite(Error.X)
                && VoxelMath::IsFinite(Error.Y)
                && VoxelMath::IsFinite(Error.Z))
            {
                WorldPoint.X += Error.X;
                WorldPoint.Y += Error.Y;
                WorldPoint.Z += Error.Z * ZScale;
            }
        }
        return WorldPoint;
    }

    float VF_RoomFloorZFromParts(
        const FVector& RoomCenter,
        float RoomRadiusXY,
        float RoomRadiusZ,
        uint32 RoomHash,
        uint32 WarpSeed,
        const FStrateGenerationParams& Params)
    {
        const float Roll = VoxelHash::ToFloat01(VoxelHash::Mix(RoomHash ^ 0xF100F2u));
        const float FloorCut = FMath::Lerp(
            FMath::Min(Params.RoomFloorCutMin, Params.RoomFloorCutMax),
            FMath::Max(Params.RoomFloorCutMin, Params.RoomFloorCutMax),
            Roll);
        float FloorZ = FloorCut < 1.0f
            ? RoomCenter.Z - RoomRadiusZ * FloorCut
            : RoomCenter.Z - RoomRadiusZ;
        if (FloorCut < 1.0f && Params.FloorReliefStrength > 0.0f)
        {
            // Match the evaluator's domain-warped XY input at the room centre. The local landing
            // query still searches a bounded vertical band for the support patch around this
            // estimate because the capsule covers neighbouring XY samples too.
            const float EffectiveZ = Params.VerticalScale > 0.0f
                ? RoomCenter.Z / Params.VerticalScale : RoomCenter.Z;
            float SampleX = RoomCenter.X;
            float SampleY = RoomCenter.Y;
            if (Params.CaveWarpStrength > 0.0f)
            {
                const float Frequency = Params.CaveWarpFrequency;
                const float Strength = Params.CaveWarpStrength;
                SampleX += VoxelNoise::Perlin3D(
                    RoomCenter.X * Frequency + VoxelHash::SeedOffset(WarpSeed, 0.37f),
                    RoomCenter.Y * Frequency + 1.3f,
                    EffectiveZ * Frequency + 5.7f)
                    * VOXEL_NOISE_SCALE * Strength;
                SampleY += VoxelNoise::Perlin3D(
                    RoomCenter.X * Frequency + 7.1f,
                    RoomCenter.Y * Frequency + VoxelHash::SeedOffset(WarpSeed, 0.59f),
                    EffectiveZ * Frequency + 2.3f)
                    * VOXEL_NOISE_SCALE * Strength;
            }

            const float Frequency = Params.FloorReliefFrequency;
            const uint32 FloorSeed = VoxelHash::Mix(RoomHash ^ 0xF100F1u);
            const FVFFloorReliefColumnKey ReliefColumn =
                VF_MakeFloorReliefColumnKey(SampleX, SampleY);
            const float Noise = VF_FloorReliefNoiseFloat(
                SampleX, SampleY, FloorSeed, Frequency,
                nullptr, EVFFloorReliefFeature::LandingFloat,
                static_cast<int32>(RoomHash), ReliefColumn);
            FloorZ += Noise * VOXEL_NOISE_SCALE * Params.FloorReliefStrength;
        }
        // Keep this in the same (unwarped SDF) coordinate space as FCachedRoom::FloorCutZ. The
        // production caller warps the query position before EvaluateSDFCached, and the exact fit
        // predicate below samples that same warped field. This value is only a bounded search
        // anchor; it must not pre-apply the Z warp or the room would be shifted twice.
        return FloorZ;
    }

    float VF_RoomFloorZ(
        const FVFRoomLandingSite& Site,
        uint32 WarpSeed,
        const FStrateGenerationParams& Params)
    {
        return VF_RoomFloorZFromParts(
            Site.Center, Site.RadiusXY, Site.RadiusZ, Site.Hash, WarpSeed, Params);
    }

    float VF_EvaluateRoomLandingDensity(
        const FVFRoomLandingSite& Site,
        const FStrateGenerationParams& Params,
        uint32 Seed,
        float StrateTopZ,
        float StrateBottomZ,
        float WorldX,
        float WorldY,
        float WorldZ)
    {
        const FVector Position = VF_ApplyCaveWarp(
            FVector(WorldX, WorldY, WorldZ), Params, static_cast<uint32>(Seed));

        const uint32 ShapeHash = VoxelHash::Mix(Site.Hash ^ 0xDEADBEEFu);
        const float ShapeRoll = Site.bOrigin ? 0.0f : VoxelHash::ToFloat01(ShapeHash);
        const float BoxThreshold = 1.0f - Params.RoomShapeVariety * 0.5f;
        const float CapsuleThreshold = 1.0f - Params.RoomShapeVariety * 0.2f;
        float RoomSDF = 0.0f;
        if (ShapeRoll >= BoxThreshold && ShapeRoll < CapsuleThreshold)
        {
            RoomSDF = VoxelSDF::RoundedBox(
                Position,
                Site.Center,
                FVector(Site.RadiusXY * 0.8f, Site.RadiusXY * 0.8f, Site.RadiusZ * 0.8f),
                Site.RadiusXY * 0.25f);
        }
        else if (ShapeRoll >= CapsuleThreshold)
        {
            const float DirectionAngle = VoxelHash::ToFloat01(
                VoxelHash::Mix(Site.Hash ^ 0xCAFEBABEu)) * 2.0f * PI;
            const float StretchDistance = Site.RadiusXY * 0.7f;
            float SinDirection = 0.0f, CosDirection = 0.0f;
            VoxelMath::DetSinCos(SinDirection, CosDirection, DirectionAngle);
            const FVector Direction(CosDirection, SinDirection, 0.0f);
            RoomSDF = VoxelSDF::Capsule(
                Position,
                Site.Center + Direction * StretchDistance,
                Site.Center - Direction * StretchDistance,
                FMath::Min(Site.RadiusXY * 0.6f, Site.RadiusZ));
        }
        else
        {
            RoomSDF = VoxelSDF::Ellipsoid(
                Position, Site.Center,
                FVector(Site.RadiusXY, Site.RadiusXY, Site.RadiusZ));
        }

        const float FloorCut = FMath::Lerp(
            FMath::Min(Params.RoomFloorCutMin, Params.RoomFloorCutMax),
            FMath::Max(Params.RoomFloorCutMin, Params.RoomFloorCutMax),
            VoxelHash::ToFloat01(VoxelHash::Mix(Site.Hash ^ 0xF100F2u)));
        if (FloorCut < 1.0f)
        {
            float FloorZ = Site.Center.Z - Site.RadiusZ * FloorCut;
            bool bFloorCutIsIdentityForDensity = false;
            if (Params.FloorReliefStrength > 0.0f)
            {
                const float Frequency = Params.FloorReliefFrequency;
                const uint32 FloorSeed = VoxelHash::Mix(Site.Hash ^ 0xF100F1u);
                float ReliefBound = 0.0f;
                const float FloorBlend = Params.SDFBlendRadius * 0.35f;
                const float BaseFloorSDF = static_cast<float>(
                    FloorZ - Position.Z);
                const float BaseCutSDF = VoxelSDF::SmoothMax(
                    RoomSDF, BaseFloorSDF, FloorBlend);
                if (VF_GetFloorReliefBound(
                        Params.FloorReliefStrength, 1.0f, 1.0f,
                        Frequency, ReliefBound)
                    && FMath::Abs(BaseCutSDF) > ReliefBound)
                {
                    // This helper exposes only the sign of the room density. SmoothMax is
                    // one-Lipschitz in its floor argument, so the relief cannot change that
                    // sign when the unrelieved result is farther from zero than its bound.
                    RoomSDF = BaseCutSDF;
                    bFloorCutIsIdentityForDensity = true;
                }
                else
                {
                    const FVFFloorReliefColumnKey ReliefColumn =
                        VF_MakeFloorReliefColumnKey(Position.X, Position.Y);
                    const float Noise = VF_FloorReliefNoiseDouble(
                        Position.X, Position.Y,
                        FloorSeed, Frequency,
                        nullptr, EVFFloorReliefFeature::LandingDouble,
                        static_cast<int32>(Site.Hash), ReliefColumn);
                    FloorZ += Noise * VOXEL_NOISE_SCALE * Params.FloorReliefStrength;
                }
            }
            if (!bFloorCutIsIdentityForDensity)
            {
                RoomSDF = VoxelSDF::SmoothMax(
                    RoomSDF, FloorZ - Position.Z, Params.SDFBlendRadius * 0.35f);
            }
        }

        float InternalDensity = RoomSDF < 0.0f ? -1.0f : 1.0f;
        VF_ApplyBoundarySeal(
            InternalDensity, WorldZ, StrateTopZ, StrateBottomZ,
            Params.BoundarySealThickness, 1.0f);
        // Query-facing density uses the same MC polarity as the measurement: positive is air.
        return -InternalDensity;
    }

    static float VF_EvaluatePreparedRoomLandingDensity(
        const FVFPreparedRoomLanding& Query,
        const FStrateGenerationParams& Params,
        uint32 Seed,
        float StrateTopZ,
        float StrateBottomZ,
        float WorldX,
        float WorldY,
        float WorldZ)
    {
        const FVector Position = VF_ApplyCaveWarp(
            FVector(WorldX, WorldY, WorldZ), Params, Seed);
        float RoomSDF = VF_EvaluatePreparedRoomShapeSDF(Query, Position);

        if (Query.bHasFloorCut)
        {
            bool bFloorCutIsIdentityForDensity = false;
            if (Query.FloorReliefStrength > 0.0f)
            {
                const float BaseFloorSDF = Query.FloorCutZ - Position.Z;
                const float BaseCutSDF = VoxelSDF::SmoothMax(
                    RoomSDF, BaseFloorSDF, Query.FloorBlend);
                if (Query.bHasFloorReliefBound
                    && FMath::Abs(BaseCutSDF) > Query.FloorReliefBound)
                {
                    // SmoothMax is one-Lipschitz in its floor argument. The same bound used by
                    // the scalar reference proves that relief cannot change this sign, so avoid
                    // the noise call while retaining the exact sign decision.
                    RoomSDF = BaseCutSDF;
                    bFloorCutIsIdentityForDensity = true;
                }
                else
                {
                    const FVFFloorReliefColumnKey ReliefColumn =
                        VF_MakeFloorReliefColumnKey(Position.X, Position.Y);
                    const float Noise = VF_FloorReliefNoiseDouble(
                        Position.X, Position.Y,
                        Query.FloorSeed, Query.FloorReliefFrequency,
                        nullptr, EVFFloorReliefFeature::LandingDouble,
                        Query.FloorFeatureIndex, ReliefColumn);
                    const float FloorZ = Query.FloorCutZ
                        + Noise * VOXEL_NOISE_SCALE * Query.FloorReliefStrength;
                    RoomSDF = VoxelSDF::SmoothMax(
                        RoomSDF, FloorZ - Position.Z, Query.FloorBlend);
                    bFloorCutIsIdentityForDensity = true;
                }
            }
            if (!bFloorCutIsIdentityForDensity)
            {
                RoomSDF = VoxelSDF::SmoothMax(
                    RoomSDF, Query.FloorCutZ - Position.Z, Query.FloorBlend);
            }
        }

        float InternalDensity = RoomSDF < 0.0f ? -1.0f : 1.0f;
        VF_ApplyBoundarySeal(
            InternalDensity, WorldZ, StrateTopZ, StrateBottomZ,
            Params.BoundarySealThickness, 1.0f);
        return -InternalDensity;
    }

    static bool VF_FindRoomFloorCrossingWorld(
        const FVFRoomLandingSite& Site,
        const FStrateGenerationParams& Params,
        uint32 Seed,
        float StrateTopZ,
        float StrateBottomZ,
        const FVector& WorldMouthXY,
        float& OutFloorZ)
    {
        OutFloorZ = -FLT_MAX;
        if (!VoxelMath::IsFinite(WorldMouthXY.X)
            || !VoxelMath::IsFinite(WorldMouthXY.Y)
            || !VoxelMath::IsFinite(Site.Center.Z)
            || !VoxelMath::IsFinite(Site.RadiusZ))
        {
            return false;
        }

        // BuildRoomMouth receives a world-space point from the boundary search. Find the
        // crossing in that same space instead of reusing the SDF-space floor estimate. This is
        // deliberately a vertical room query: the room owns the floor height at the mouth, and
        // the tunnel endpoint is then placed one tunnel radius above that exact crossing.
        const float ReliefBound = FMath::Abs(Params.FloorReliefStrength)
            * VOXEL_NOISE_SCALE * 1.5f;
        const float MinZ = Site.Center.Z - FMath::Abs(Site.RadiusZ)
            - ReliefBound - 16.0f;
        const float MaxZ = Site.Center.Z + FMath::Abs(Site.RadiusZ)
            + ReliefBound + 16.0f;
        constexpr float SearchStep = 1.0f;
        if (!VoxelMath::IsFinite(MinZ) || !VoxelMath::IsFinite(MaxZ) || MaxZ <= MinZ)
        {
            return false;
        }

        FVFPreparedRoomLanding PreparedRoom;
        const bool bPreparedRoom = VF_PrepareRoomLanding(
            Site, Params, PreparedRoom);
        auto Evaluate = [&](float Z) -> float
        {
            return bPreparedRoom
                ? VF_EvaluatePreparedRoomLandingDensity(
                    PreparedRoom, Params, Seed, StrateTopZ, StrateBottomZ,
                    WorldMouthXY.X, WorldMouthXY.Y, Z)
                : VF_EvaluateRoomLandingDensity(
                    Site, Params, Seed, StrateTopZ, StrateBottomZ,
                    WorldMouthXY.X, WorldMouthXY.Y, Z);
        };

        float Low = MinZ;
        float LowValue = Evaluate(Low);
        if (!VoxelMath::IsFinite(LowValue))
        {
            return false;
        }

        // Once the lower and upper ends bracket the room, the authored room/floor field is
        // monotone through this narrow vertical landing band for the finite parameter envelope
        // used by the generator.  Find the one-voxel bucket with a logarithmic search, then run
        // the exact bisection in that bucket.  The latter is kept byte-for-byte in its arithmetic
        // and iteration count so the endpoint is identical to the one-voxel walk below.
        // If the bracket is not monotone (or the local bucket check cannot reproduce the crossing),
        // fall back to that one-voxel walk; malformed/unsupported inputs therefore keep its
        // result.
        auto BisectBracket = [&](float BracketLow, float BracketHigh,
                                 float BracketLowValue) -> bool
        {
            for (int32 Iteration = 0; Iteration < 24; ++Iteration)
            {
                const float Mid = (BracketLow + BracketHigh) * 0.5f;
                const float MidValue = Evaluate(Mid);
                if (!VoxelMath::IsFinite(MidValue))
                {
                    return false;
                }
                if (FMath::Abs(MidValue) <= 1.0e-5f)
                {
                    BracketLow = Mid;
                    BracketHigh = Mid;
                    break;
                }
                if (BracketLowValue <= 0.0f && MidValue < 0.0f)
                {
                    BracketLow = Mid;
                    BracketLowValue = MidValue;
                }
                else
                {
                    BracketHigh = Mid;
                }
            }
            OutFloorZ = (BracketLow + BracketHigh) * 0.5f;
            return VoxelMath::IsFinite(OutFloorZ);
        };

        const int32 Steps = FMath::CeilToInt((MaxZ - MinZ) / SearchStep);
        const float MaxValue = Evaluate(MaxZ);
        if (VoxelMath::IsFinite(MaxValue)
            && LowValue <= 0.0f
            && MaxValue >= 0.0f
            && Steps > 0)
        {
            float BracketLow = Low;
            float BracketHigh = MaxZ;
            float BracketLowValue = LowValue;
            for (int32 Iteration = 0; Iteration < 12; ++Iteration)
            {
                const float Mid = (BracketLow + BracketHigh) * 0.5f;
                const float MidValue = Evaluate(Mid);
                if (!VoxelMath::IsFinite(MidValue))
                {
                    BracketLow = Low;
                    BracketHigh = Low;
                    break;
                }
                if (BracketLowValue <= 0.0f && MidValue < 0.0f)
                {
                    BracketLow = Mid;
                    BracketLowValue = MidValue;
                }
                else
                {
                    BracketHigh = Mid;
                }
            }

            if (BracketHigh > BracketLow)
            {
                const float ApproximateCrossing = (BracketLow + BracketHigh) * 0.5f;
                const float RelativeCrossing =
                    (ApproximateCrossing - MinZ) / SearchStep;
                const int32 GuessIndex = FMath::Clamp(
                    FMath::FloorToInt(RelativeCrossing), 0, Steps - 1);
                for (int32 Offset = -2; Offset <= 2; ++Offset)
                {
                    const int32 CandidateIndex = GuessIndex + Offset;
                    if (CandidateIndex < 0 || CandidateIndex >= Steps)
                    {
                        continue;
                    }
                    const float CandidateLow = MinZ
                        + static_cast<float>(CandidateIndex) * SearchStep;
                    const float CandidateHigh = FMath::Min(
                        MaxZ, MinZ
                            + static_cast<float>(CandidateIndex + 1) * SearchStep);
                    const float CandidateLowValue = Evaluate(CandidateLow);
                    const float CandidateHighValue = Evaluate(CandidateHigh);
                    if (VoxelMath::IsFinite(CandidateLowValue)
                        && VoxelMath::IsFinite(CandidateHighValue)
                        && CandidateLowValue <= 0.0f
                        && CandidateHighValue >= 0.0f)
                    {
                        return BisectBracket(
                            CandidateLow, CandidateHigh, CandidateLowValue);
                    }
                }
            }
        }

        for (int32 StepIndex = 1; StepIndex <= Steps; ++StepIndex)
        {
            const float High = FMath::Min(
                MaxZ, MinZ + static_cast<float>(StepIndex) * SearchStep);
            const float HighValue = Evaluate(High);
            if (!VoxelMath::IsFinite(HighValue))
            {
                Low = High;
                LowValue = HighValue;
                continue;
            }
            if (FMath::Abs(LowValue) <= KINDA_SMALL_NUMBER)
            {
                OutFloorZ = Low;
                return true;
            }
            if (LowValue <= 0.0f && HighValue >= 0.0f)
            {
                return BisectBracket(Low, High, LowValue);
            }
            Low = High;
            LowValue = HighValue;
        }
        return false;
    }

    static bool VF_FindRoomMouthBoundary(
        const FVFRoomLandingSite& Site,
        const FStrateGenerationParams& Params,
        uint32 Seed,
        float StrateTopZ,
        float StrateBottomZ,
        const FVector& Anchor,
        const FVector& Direction,
        float TunnelRadius,
        float BlendRadius,
        FVector& OutMouthXY,
        float& OutBoundaryDistance)
    {
        FVector DirectionXY(Direction.X, Direction.Y, 0.0f);
        if (!DirectionXY.Normalize()
            || !VoxelMath::IsFinite(Anchor.X)
            || !VoxelMath::IsFinite(Anchor.Y)
            || !VoxelMath::IsFinite(Site.Center.Z))
        {
            return false;
        }

        // Probe both the room mid-height and the tunnel-centre height. The latter is important
        // for vertically compressed/ellipsoidal rooms: their wall can be closer at the floor
        // than at mid-height. Both probes use the exact hashed room shape and cave warp used by
        // density evaluation. Positive landing density means air/inside.
        const float RoomFloorZ = VF_RoomFloorZFromParts(
            Site.Center, Site.RadiusXY, Site.RadiusZ, Site.Hash, Seed, Params);
        const float LowerProbeZ = VoxelMath::IsFinite(RoomFloorZ)
            ? RoomFloorZ + FMath::Abs(TunnelRadius)
            : Site.Center.Z;
        const float UpperProbeZ = Site.Center.Z;
        FVFPreparedRoomLanding PreparedRoom;
        const bool bPreparedRoom = VF_PrepareRoomLanding(
            Site, Params, PreparedRoom);
        const auto IsInside = [&](float Distance) -> bool
        {
            const FVector Probe = Anchor + DirectionXY * Distance;
            const float LowerDensity = bPreparedRoom
                ? VF_EvaluatePreparedRoomLandingDensity(
                    PreparedRoom, Params, Seed, StrateTopZ, StrateBottomZ,
                    static_cast<float>(Probe.X), static_cast<float>(Probe.Y), LowerProbeZ)
                : VF_EvaluateRoomLandingDensity(
                    Site, Params, Seed, StrateTopZ, StrateBottomZ,
                    static_cast<float>(Probe.X), static_cast<float>(Probe.Y), LowerProbeZ);
            const float UpperDensity = bPreparedRoom
                ? VF_EvaluatePreparedRoomLandingDensity(
                    PreparedRoom, Params, Seed, StrateTopZ, StrateBottomZ,
                    static_cast<float>(Probe.X), static_cast<float>(Probe.Y), UpperProbeZ)
                : VF_EvaluateRoomLandingDensity(
                    Site, Params, Seed, StrateTopZ, StrateBottomZ,
                    static_cast<float>(Probe.X), static_cast<float>(Probe.Y), UpperProbeZ);
            return VoxelMath::IsFinite(LowerDensity)
                && LowerDensity > 0.0f
                && VoxelMath::IsFinite(UpperDensity)
                && UpperDensity > 0.0f;
        };

        if (!IsInside(0.0f))
        {
            return false;
        }

        const float ShapeReach = VF_RoomShapeReachUpperBound(
            Site.RadiusXY, Site.RadiusZ);
        const float WarpReach = FMath::Abs(Params.CaveWarpStrength)
            * VOXEL_NOISE_SCALE * 2.0f;
        float High = ShapeReach + WarpReach
            + FMath::Max(8.0f, FMath::Abs(TunnelRadius) + FMath::Abs(BlendRadius));
        if (!VoxelMath::IsFinite(High) || High <= 0.0f)
        {
            return false;
        }

        bool bOutside = !IsInside(High);
        for (int32 Expansion = 0; !bOutside && Expansion < 4; ++Expansion)
        {
            High = FMath::Min(High * 2.0f, 4096.0f);
            bOutside = !IsInside(High);
        }
        if (!bOutside)
        {
            return false;
        }

        float Low = 0.0f;
        for (int32 Iteration = 0; Iteration < 24; ++Iteration)
        {
            const float Mid = (Low + High) * 0.5f;
            if (IsInside(Mid))
            {
                Low = Mid;
            }
            else
            {
                High = Mid;
            }
        }

        const float MouthInset = VF_TunnelMouthInset(TunnelRadius, BlendRadius);
        OutBoundaryDistance = Low;
        const float MouthDistance = FMath::Max(0.0f, Low - MouthInset);
        OutMouthXY = Anchor + DirectionXY * MouthDistance;
        OutMouthXY.Z = Site.Center.Z;
        return VoxelMath::IsFinite(OutMouthXY.X)
            && VoxelMath::IsFinite(OutMouthXY.Y)
            && VoxelMath::IsFinite(OutBoundaryDistance);
    }

    bool VF_FindPlayerFitPointForRoom(
        const FStrateGenerationParams& Params,
        const FVFRoomLandingSite& Site,
        float StrateTopZ,
        float StrateBottomZ,
        uint32 WarpSeed,
        FVector& OutPoint)
    {
        OutPoint = FVector::ZeroVector;
        const float FloorZ = VF_RoomFloorZ(Site, WarpSeed, Params);
        if (!VoxelMath::IsFinite(FloorZ))
        {
            return false;
        }

        const float MaxStepHeightVoxels =
            FVoxelPlayerCapsuleConstants::MaxStepHeightMeters
                / FVoxelPlayerCapsuleConstants::VoxelSizeMeters;
        const float FloorReliefBound = FMath::Abs(Params.FloorReliefStrength)
            * VOXEL_NOISE_SCALE;
        const float CaveWarpBound = FMath::Abs(Params.CaveWarpStrength)
            * VOXEL_NOISE_SCALE;
        const int32 ReliefSearch = FMath::Clamp(
            FMath::CeilToInt(FloorReliefBound + CaveWarpBound) + 8,
            8,
            128);
        FVoxelStrateMeasureSettings FitSettings;
        const auto SampleRoom = [&](float X, float Y, float Z)
        {
            return VF_EvaluateRoomLandingDensity(
                Site, Params, WarpSeed,
                StrateTopZ, StrateBottomZ, X, Y, Z);
        };

        const int32 LocalExtent = 0;
        TArray<FIntPoint, TInlineAllocator<128>> LocalProbes;
        for (int32 LocalY = -LocalExtent; LocalY <= LocalExtent; LocalY += 2)
        {
            for (int32 LocalX = -LocalExtent; LocalX <= LocalExtent; LocalX += 2)
            {
                if (static_cast<float>(LocalX * LocalX + LocalY * LocalY)
                    > FMath::Square(Site.RadiusXY * 0.75f))
                {
                    continue;
                }
                LocalProbes.Add(FIntPoint(LocalX, LocalY));
            }
        }
        if ((LocalExtent & 1) != 0)
        {
            LocalProbes.Add(FIntPoint::ZeroValue);
        }
        LocalProbes.Sort([](const FIntPoint& A, const FIntPoint& B)
        {
            const int32 ADistanceSq = A.X * A.X + A.Y * A.Y;
            const int32 BDistanceSq = B.X * B.X + B.Y * B.Y;
            if (ADistanceSq != BDistanceSq)
            {
                return ADistanceSq < BDistanceSq;
            }
            return A.X != B.X ? A.X < B.X : A.Y < B.Y;
        });

        for (const FIntPoint& LocalProbe : LocalProbes)
        {
            const FVector CandidateFeetBase = VF_UnwarpCavePoint(
                FVector(
                    Site.Center.X + static_cast<float>(LocalProbe.X),
                    Site.Center.Y + static_cast<float>(LocalProbe.Y),
                    FloorZ + MaxStepHeightVoxels + 0.5f),
                Params,
                WarpSeed);
            for (int32 OffsetIndex = 0;
                 OffsetIndex <= ReliefSearch * 2;
                 ++OffsetIndex)
            {
                const int32 SignedOffset = OffsetIndex == 0
                    ? 0
                    : ((OffsetIndex & 1) != 0
                        ? -((OffsetIndex + 1) / 2)
                        : OffsetIndex / 2);
                if (FMath::Abs(SignedOffset) > ReliefSearch)
                {
                    continue;
                }
                const FVector CandidateFeet = CandidateFeetBase
                    + FVector(0.0f, 0.0f, static_cast<float>(SignedOffset));
                if (VF_ValidatePlayerFitPose(
                        FitSettings,
                        CandidateFeet,
                        StrateTopZ,
                        StrateBottomZ,
                        Params.BoundarySealThickness,
                        SampleRoom,
                        OutPoint))
                {
                    return true;
                }
            }
        }
        return false;
    }

    // The room-fit result is pure with respect to the complete parameter value, room site, Z
    // bounds, and warp seed. Keep the complete key explicit instead of hashing the USTRUCT's
    // object representation: the latter would include padding and would become unsafe if a
    // pointer/TArray field were ever added.
#define VF_PLAYER_FIT_PARAM_COUNT_LERP(Name) +1
#define VF_PLAYER_FIT_PARAM_COUNT_SNAP(Name) +1
    static constexpr int32 VF_PlayerFitParamWordCount =
        0 VF_STRATE_PARAM_FIELDS(
            VF_PLAYER_FIT_PARAM_COUNT_LERP,
            VF_PLAYER_FIT_PARAM_COUNT_SNAP);
#undef VF_PLAYER_FIT_PARAM_COUNT_LERP
#undef VF_PLAYER_FIT_PARAM_COUNT_SNAP

    static constexpr int32 VF_PlayerFitVectorWordCount =
        static_cast<int32>(sizeof(FVector::FReal) / sizeof(uint32));
    static_assert(
        VF_PlayerFitVectorWordCount == 1 || VF_PlayerFitVectorWordCount == 2,
        "player-fit memo expects float or double FVector components");
    static constexpr int32 VF_PlayerFitExtraKeyWordCount =
        3 * VF_PlayerFitVectorWordCount + 8;
    static constexpr int32 VF_PlayerFitKeyWordCount =
        VF_PlayerFitParamWordCount + VF_PlayerFitExtraKeyWordCount;
    static constexpr uint32 VF_PlayerFitMemoKeyVersion = 0x00010001u;

    struct FVFPlayerFitMemoKey
    {
        uint32 Words[VF_PlayerFitKeyWordCount] = {};
    };

    template <typename TKey, typename T>
    FORCEINLINE void VF_AppendPlayerFitMemoBits(
        TKey& Key,
        int32& InOutWordIndex,
        const T& Value)
    {
        static_assert(
            sizeof(T) <= sizeof(uint64),
            "player-fit memo key fields must be scalar values no wider than uint64");
        constexpr int32 WordCount =
            static_cast<int32>((sizeof(T) + sizeof(uint32) - 1) / sizeof(uint32));
        for (int32 WordIndex = 0; WordIndex < WordCount; ++WordIndex)
        {
            Key.Words[InOutWordIndex + WordIndex] = 0u;
        }
        FMemory::Memcpy(
            Key.Words + InOutWordIndex,
            &Value,
            sizeof(T));
        InOutWordIndex += WordCount;
    }

    static FVFPlayerFitMemoKey VF_MakePlayerFitMemoKey(
        const FStrateGenerationParams& Params,
        const FVFRoomLandingSite& Site,
        float StrateTopZ,
        float StrateBottomZ,
        uint32 WarpSeed)
    {
        FVFPlayerFitMemoKey Key;
        int32 WordIndex = 0;

#define VF_PLAYER_FIT_KEY_APPEND_LERP(Name) \
        VF_AppendPlayerFitMemoBits(Key, WordIndex, Params.Name);
#define VF_PLAYER_FIT_KEY_APPEND_SNAP(Name) \
        VF_AppendPlayerFitMemoBits(Key, WordIndex, Params.Name);
        VF_STRATE_PARAM_FIELDS(
            VF_PLAYER_FIT_KEY_APPEND_LERP,
            VF_PLAYER_FIT_KEY_APPEND_SNAP)
#undef VF_PLAYER_FIT_KEY_APPEND_LERP
#undef VF_PLAYER_FIT_KEY_APPEND_SNAP

        // FVector is double-valued in the UE5 build used by this plugin. Append each component
        // as raw words so the site key preserves its exact input bits on either float/double build.
        VF_AppendPlayerFitMemoBits(Key, WordIndex, Site.Center.X);
        VF_AppendPlayerFitMemoBits(Key, WordIndex, Site.Center.Y);
        VF_AppendPlayerFitMemoBits(Key, WordIndex, Site.Center.Z);
        VF_AppendPlayerFitMemoBits(Key, WordIndex, Site.RadiusXY);
        VF_AppendPlayerFitMemoBits(Key, WordIndex, Site.RadiusZ);
        VF_AppendPlayerFitMemoBits(Key, WordIndex, Site.Hash);
        VF_AppendPlayerFitMemoBits(Key, WordIndex, Site.bOrigin);
        VF_AppendPlayerFitMemoBits(Key, WordIndex, StrateTopZ);
        VF_AppendPlayerFitMemoBits(Key, WordIndex, StrateBottomZ);
        VF_AppendPlayerFitMemoBits(Key, WordIndex, WarpSeed);
        VF_AppendPlayerFitMemoBits(Key, WordIndex, VF_PlayerFitMemoKeyVersion);
        check(WordIndex == VF_PlayerFitKeyWordCount);
        return Key;
    }

    FORCEINLINE uint32 VF_HashPlayerFitMemoKey(const FVFPlayerFitMemoKey& Key)
    {
        uint32 Hash = 0xD14C4B7Du;
        for (int32 WordIndex = 0; WordIndex < VF_PlayerFitKeyWordCount; ++WordIndex)
        {
            Hash ^= Key.Words[WordIndex]
                + 0x9e3779b9u
                + (Hash << 6)
                + (Hash >> 2);
        }
        return VoxelHash::Mix(Hash);
    }

    // A slot has an atomic reader/writer state, but its key/result payload stays ordinary data.
    // Readers increment the low counter before touching the payload; writers set the high bit,
    // wait for existing readers, replace the payload, then publish the valid bit. This avoids
    // a seqlock data race and avoids the ABA window of a finite sequence number.
    struct FVFPlayerFitMemoSlot
    {
        static constexpr uint32 ReaderMask = (1u << 30) - 1u;
        static constexpr uint32 ValidBit = 1u << 30;
        static constexpr uint32 WriterBit = 1u << 31;

        std::atomic<uint32> State;
        std::atomic<uint32> KeyHash;
        FVFPlayerFitMemoKey Key;
        FVector Result = FVector::ZeroVector;
        bool bFound = false;

        FVFPlayerFitMemoSlot()
            : State(0u)
            , KeyHash(0u)
        {
        }

        bool TryAcquireRead()
        {
            uint32 Current = State.load(std::memory_order_acquire);
            for (;;)
            {
                if ((Current & ValidBit) == 0u
                    || (Current & WriterBit) != 0u
                    || (Current & ReaderMask) == ReaderMask)
                {
                    return false;
                }
                if (State.compare_exchange_weak(
                        Current,
                        Current + 1u,
                        std::memory_order_acquire,
                        std::memory_order_relaxed))
                {
                    return true;
                }
            }
        }

        void ReleaseRead()
        {
            State.fetch_sub(1u, std::memory_order_release);
        }

        bool AcquireWrite()
        {
            uint32 Current = State.load(std::memory_order_acquire);
            for (;;)
            {
                if ((Current & WriterBit) != 0u)
                {
                    FPlatformProcess::YieldThread();
                    Current = State.load(std::memory_order_acquire);
                    continue;
                }

                const bool bWasValid = (Current & ValidBit) != 0u;
                if (State.compare_exchange_weak(
                        Current,
                        Current | WriterBit,
                        std::memory_order_acq_rel,
                        std::memory_order_acquire))
                {
                    while ((State.load(std::memory_order_acquire) & ReaderMask) != 0u)
                    {
                        FPlatformProcess::YieldThread();
                    }
                    return bWasValid;
                }
            }
        }

    };

    struct FVFPlayerFitMemoSetLock
    {
        std::atomic<bool> bLocked;

        FVFPlayerFitMemoSetLock()
            : bLocked(false)
        {
        }

        void Lock()
        {
            bool bExpected = false;
            while (!bLocked.compare_exchange_weak(
                bExpected,
                true,
                std::memory_order_acquire,
                std::memory_order_relaxed))
            {
                bExpected = false;
                FPlatformProcess::YieldThread();
            }
        }

        void Unlock()
        {
            bLocked.store(false, std::memory_order_release);
        }
    };

    struct FVFPlayerFitMemo
    {
        static constexpr int32 NumSets = 4096;
        static constexpr int32 NumWays = 4;
        static constexpr int32 Capacity = NumSets * NumWays;
        static_assert((NumSets & (NumSets - 1)) == 0, "player-fit memo sets must be a power of two");
        static_assert((NumWays & (NumWays - 1)) == 0, "player-fit memo ways must be a power of two");

        FVFPlayerFitMemoSlot Slots[Capacity];
        FVFPlayerFitMemoSetLock SetLocks[NumSets];
        std::atomic<uint32> ReplacementCursor[NumSets];

        std::atomic<uint64> Requests{0};
        std::atomic<uint64> Hits{0};
        std::atomic<uint64> Misses{0};
        std::atomic<uint64> Searches{0};
        std::atomic<uint64> Evictions{0};
        std::atomic<uint64> Bypasses{0};
        std::atomic<uint64> OccupiedEntries{0};

        FVFPlayerFitMemo()
        {
            for (int32 SetIndex = 0; SetIndex < NumSets; ++SetIndex)
            {
                ReplacementCursor[SetIndex].store(0u, std::memory_order_relaxed);
            }
        }

        static FORCEINLINE int32 SlotIndex(uint32 KeyHash, int32 Way)
        {
            const int32 SetIndex = static_cast<int32>(KeyHash & (NumSets - 1));
            return SetIndex * NumWays + Way;
        }

        bool TryFind(
            const FVFPlayerFitMemoKey& InKey,
            uint32 InKeyHash,
            FVector& OutPoint,
            bool& OutFound)
        {
            for (int32 Way = 0; Way < NumWays; ++Way)
            {
                FVFPlayerFitMemoSlot& Slot = Slots[SlotIndex(InKeyHash, Way)];
                // A stale hash can only cause a miss. Exact key comparison below is still
                // mandatory before returning the cached result.
                if (Slot.KeyHash.load(std::memory_order_relaxed) != InKeyHash
                    || !Slot.TryAcquireRead())
                {
                    continue;
                }

                bool bMatches = true;
                for (int32 WordIndex = 0;
                     WordIndex < VF_PlayerFitKeyWordCount;
                     ++WordIndex)
                {
                    if (Slot.Key.Words[WordIndex] != InKey.Words[WordIndex])
                    {
                        bMatches = false;
                        break;
                    }
                }

                if (bMatches)
                {
                    OutPoint = Slot.Result;
                    OutFound = Slot.bFound;
                    Slot.ReleaseRead();
                    return true;
                }
                Slot.ReleaseRead();
            }
            return false;
        }

        void Publish(
            int32 SetIndex,
            const FVFPlayerFitMemoKey& InKey,
            uint32 InKeyHash,
            bool bFound,
            const FVector& Point)
        {
            int32 SelectedWay = INDEX_NONE;
            for (int32 Way = 0; Way < NumWays; ++Way)
            {
                const FVFPlayerFitMemoSlot& Slot = Slots[SlotIndex(InKeyHash, Way)];
                if ((Slot.State.load(std::memory_order_acquire)
                        & FVFPlayerFitMemoSlot::ValidBit) == 0u)
                {
                    SelectedWay = Way;
                    break;
                }
            }
            if (SelectedWay == INDEX_NONE)
            {
                SelectedWay = static_cast<int32>(
                    ReplacementCursor[SetIndex].fetch_add(
                        1u, std::memory_order_relaxed)
                    & (NumWays - 1));
            }

            FVFPlayerFitMemoSlot& Slot = Slots[SlotIndex(InKeyHash, SelectedWay)];
            const bool bWasValid = Slot.AcquireWrite();
            Slot.Key = InKey;
            Slot.Result = Point;
            Slot.bFound = bFound;
            Slot.KeyHash.store(InKeyHash, std::memory_order_relaxed);
            Slot.State.store(FVFPlayerFitMemoSlot::ValidBit, std::memory_order_release);
            if (bWasValid)
            {
                Evictions.fetch_add(1u, std::memory_order_relaxed);
            }
            else
            {
                OccupiedEntries.fetch_add(1u, std::memory_order_relaxed);
            }
        }

        template <typename ComputeFn>
        bool FindOrCompute(
            const FVFPlayerFitMemoKey& InKey,
            FVector& OutPoint,
            ComputeFn&& Compute)
        {
            const uint32 KeyHash = VF_HashPlayerFitMemoKey(InKey);
            Requests.fetch_add(1u, std::memory_order_relaxed);

            bool bFound = false;
            FVector CachedPoint = FVector::ZeroVector;
            if (TryFind(InKey, KeyHash, CachedPoint, bFound))
            {
                OutPoint = CachedPoint;
                Hits.fetch_add(1u, std::memory_order_relaxed);
                return bFound;
            }

            const int32 SetIndex = static_cast<int32>(KeyHash & (NumSets - 1));
            SetLocks[SetIndex].Lock();
            if (TryFind(InKey, KeyHash, CachedPoint, bFound))
            {
                SetLocks[SetIndex].Unlock();
                OutPoint = CachedPoint;
                Hits.fetch_add(1u, std::memory_order_relaxed);
                return bFound;
            }

            FVector ComputedPoint = FVector::ZeroVector;
            bFound = Compute(ComputedPoint);
            Searches.fetch_add(1u, std::memory_order_relaxed);
            Misses.fetch_add(1u, std::memory_order_relaxed);
            Publish(SetIndex, InKey, KeyHash, bFound, ComputedPoint);
            SetLocks[SetIndex].Unlock();

            OutPoint = ComputedPoint;
            return bFound;
        }

        void ResetStats()
        {
            Requests.store(0u, std::memory_order_relaxed);
            Hits.store(0u, std::memory_order_relaxed);
            Misses.store(0u, std::memory_order_relaxed);
            Searches.store(0u, std::memory_order_relaxed);
            Evictions.store(0u, std::memory_order_relaxed);
            Bypasses.store(0u, std::memory_order_relaxed);
        }
    };

    // Room-mouth construction is pure for the same generation parameters, room site, approach
    // direction, and tunnel radius.  BuildChunkCache is intentionally rebuilt for every touched
    // tile, so doing the warped wall/floor search in every cache threw away the same result over
    // and over again.  Keep a small process-wide set-associative memo, using the same guarded
    // payload protocol as the player-fit memo above.  This changes no field arithmetic; it only
    // moves the deterministic search to the first cache that needs a given mouth.
    static constexpr int32 VF_RoomMouthKeyWordCount =
        VF_PlayerFitParamWordCount
        + 3 * VF_PlayerFitVectorWordCount
        + 2 + 1 + 1
        + 2 * VF_PlayerFitVectorWordCount
        + 2 * VF_PlayerFitVectorWordCount
        + 4;
    static constexpr uint32 VF_RoomMouthMemoKeyVersion = 0x00010001u;

    struct FVFRoomMouthMemoKey
    {
        uint32 Words[VF_RoomMouthKeyWordCount] = {};
    };

    static FVFRoomMouthMemoKey VF_MakeRoomMouthMemoKey(
        const FStrateGenerationParams& Params,
        const FVFRoomLandingSite& Site,
        const FVector& Anchor,
        const FVector& Direction,
        float TunnelRadius,
        float BlendRadius,
        uint32 WarpSeed)
    {
        FVFRoomMouthMemoKey Key;
        int32 WordIndex = 0;

#define VF_ROOM_MOUTH_KEY_APPEND_LERP(Name) \
        VF_AppendPlayerFitMemoBits(Key, WordIndex, Params.Name);
#define VF_ROOM_MOUTH_KEY_APPEND_SNAP(Name) \
        VF_AppendPlayerFitMemoBits(Key, WordIndex, Params.Name);
        VF_STRATE_PARAM_FIELDS(
            VF_ROOM_MOUTH_KEY_APPEND_LERP,
            VF_ROOM_MOUTH_KEY_APPEND_SNAP)
#undef VF_ROOM_MOUTH_KEY_APPEND_LERP
#undef VF_ROOM_MOUTH_KEY_APPEND_SNAP

        VF_AppendPlayerFitMemoBits(Key, WordIndex, Site.Center.X);
        VF_AppendPlayerFitMemoBits(Key, WordIndex, Site.Center.Y);
        VF_AppendPlayerFitMemoBits(Key, WordIndex, Site.Center.Z);
        VF_AppendPlayerFitMemoBits(Key, WordIndex, Site.RadiusXY);
        VF_AppendPlayerFitMemoBits(Key, WordIndex, Site.RadiusZ);
        VF_AppendPlayerFitMemoBits(Key, WordIndex, Site.Hash);
        VF_AppendPlayerFitMemoBits(Key, WordIndex, Site.bOrigin);
        VF_AppendPlayerFitMemoBits(Key, WordIndex, Anchor.X);
        VF_AppendPlayerFitMemoBits(Key, WordIndex, Anchor.Y);
        VF_AppendPlayerFitMemoBits(Key, WordIndex, Direction.X);
        VF_AppendPlayerFitMemoBits(Key, WordIndex, Direction.Y);
        VF_AppendPlayerFitMemoBits(Key, WordIndex, TunnelRadius);
        VF_AppendPlayerFitMemoBits(Key, WordIndex, BlendRadius);
        VF_AppendPlayerFitMemoBits(Key, WordIndex, WarpSeed);
        VF_AppendPlayerFitMemoBits(Key, WordIndex, VF_RoomMouthMemoKeyVersion);
        check(WordIndex == VF_RoomMouthKeyWordCount);
        return Key;
    }

    FORCEINLINE uint32 VF_HashRoomMouthMemoKey(
        const FVFRoomMouthMemoKey& Key)
    {
        uint32 Hash = 0x6D6F7574u;
        for (int32 WordIndex = 0;
             WordIndex < VF_RoomMouthKeyWordCount;
             ++WordIndex)
        {
            Hash ^= Key.Words[WordIndex]
                + 0x9e3779b9u
                + (Hash << 6)
                + (Hash >> 2);
        }
        return VoxelHash::Mix(Hash);
    }

    struct FVFRoomMouthMemoSlot
    {
        static constexpr uint32 ReaderMask = (1u << 30) - 1u;
        static constexpr uint32 ValidBit = 1u << 30;
        static constexpr uint32 WriterBit = 1u << 31;

        std::atomic<uint32> State;
        std::atomic<uint32> KeyHash;
        FVFRoomMouthMemoKey Key;
        FVector Result = FVector::ZeroVector;
        bool bFound = false;

        FVFRoomMouthMemoSlot()
            : State(0u)
            , KeyHash(0u)
        {
        }

        bool TryAcquireRead()
        {
            uint32 Current = State.load(std::memory_order_acquire);
            for (;;)
            {
                if ((Current & ValidBit) == 0u
                    || (Current & WriterBit) != 0u
                    || (Current & ReaderMask) == ReaderMask)
                {
                    return false;
                }
                if (State.compare_exchange_weak(
                        Current,
                        Current + 1u,
                        std::memory_order_acquire,
                        std::memory_order_relaxed))
                {
                    return true;
                }
            }
        }

        void ReleaseRead()
        {
            State.fetch_sub(1u, std::memory_order_release);
        }

        bool AcquireWrite()
        {
            uint32 Current = State.load(std::memory_order_acquire);
            for (;;)
            {
                if ((Current & WriterBit) != 0u)
                {
                    FPlatformProcess::YieldThread();
                    Current = State.load(std::memory_order_acquire);
                    continue;
                }

                const bool bWasValid = (Current & ValidBit) != 0u;
                if (State.compare_exchange_weak(
                        Current,
                        Current | WriterBit,
                        std::memory_order_acq_rel,
                        std::memory_order_acquire))
                {
                    while ((State.load(std::memory_order_acquire) & ReaderMask) != 0u)
                    {
                        FPlatformProcess::YieldThread();
                    }
                    return bWasValid;
                }
            }
        }
    };

    struct FVFRoomMouthMemo
    {
        static constexpr int32 NumSets = 4096;
        static constexpr int32 NumWays = 4;
        static constexpr int32 Capacity = NumSets * NumWays;
        static_assert((NumSets & (NumSets - 1)) == 0,
            "room-mouth memo sets must be a power of two");
        static_assert((NumWays & (NumWays - 1)) == 0,
            "room-mouth memo ways must be a power of two");

        FVFRoomMouthMemoSlot Slots[Capacity];
        FVFPlayerFitMemoSetLock SetLocks[NumSets];
        std::atomic<uint32> ReplacementCursor[NumSets];

        std::atomic<uint64> Requests{0};
        std::atomic<uint64> Hits{0};
        std::atomic<uint64> Misses{0};
        std::atomic<uint64> Searches{0};
        std::atomic<uint64> Evictions{0};
        std::atomic<uint64> OccupiedEntries{0};

        FVFRoomMouthMemo()
        {
            for (int32 SetIndex = 0; SetIndex < NumSets; ++SetIndex)
            {
                ReplacementCursor[SetIndex].store(0u, std::memory_order_relaxed);
            }
        }

        static FORCEINLINE int32 SlotIndex(uint32 KeyHash, int32 Way)
        {
            const int32 SetIndex = static_cast<int32>(KeyHash & (NumSets - 1));
            return SetIndex * NumWays + Way;
        }

        bool TryFind(
            const FVFRoomMouthMemoKey& InKey,
            uint32 InKeyHash,
            FVector& OutPoint,
            bool& OutFound)
        {
            for (int32 Way = 0; Way < NumWays; ++Way)
            {
                FVFRoomMouthMemoSlot& Slot = Slots[SlotIndex(InKeyHash, Way)];
                if (Slot.KeyHash.load(std::memory_order_relaxed) != InKeyHash
                    || !Slot.TryAcquireRead())
                {
                    continue;
                }

                bool bMatches = true;
                for (int32 WordIndex = 0;
                     WordIndex < VF_RoomMouthKeyWordCount;
                     ++WordIndex)
                {
                    if (Slot.Key.Words[WordIndex] != InKey.Words[WordIndex])
                    {
                        bMatches = false;
                        break;
                    }
                }

                if (bMatches)
                {
                    OutPoint = Slot.Result;
                    OutFound = Slot.bFound;
                    Slot.ReleaseRead();
                    return true;
                }
                Slot.ReleaseRead();
            }
            return false;
        }

        void Publish(
            int32 SetIndex,
            const FVFRoomMouthMemoKey& InKey,
            uint32 InKeyHash,
            bool bFound,
            const FVector& Point)
        {
            int32 SelectedWay = INDEX_NONE;
            for (int32 Way = 0; Way < NumWays; ++Way)
            {
                const FVFRoomMouthMemoSlot& Slot = Slots[SlotIndex(InKeyHash, Way)];
                if ((Slot.State.load(std::memory_order_acquire)
                        & FVFRoomMouthMemoSlot::ValidBit) == 0u)
                {
                    SelectedWay = Way;
                    break;
                }
            }
            if (SelectedWay == INDEX_NONE)
            {
                SelectedWay = static_cast<int32>(
                    ReplacementCursor[SetIndex].fetch_add(
                        1u, std::memory_order_relaxed)
                    & (NumWays - 1));
            }

            FVFRoomMouthMemoSlot& Slot = Slots[SlotIndex(InKeyHash, SelectedWay)];
            const bool bWasValid = Slot.AcquireWrite();
            Slot.Key = InKey;
            Slot.Result = Point;
            Slot.bFound = bFound;
            Slot.KeyHash.store(InKeyHash, std::memory_order_relaxed);
            Slot.State.store(FVFRoomMouthMemoSlot::ValidBit, std::memory_order_release);
            if (bWasValid)
            {
                Evictions.fetch_add(1u, std::memory_order_relaxed);
            }
            else
            {
                OccupiedEntries.fetch_add(1u, std::memory_order_relaxed);
            }
        }

        template <typename ComputeFn>
        bool FindOrCompute(
            const FVFRoomMouthMemoKey& InKey,
            FVector& OutPoint,
            ComputeFn&& Compute)
        {
            const uint32 KeyHash = VF_HashRoomMouthMemoKey(InKey);
            Requests.fetch_add(1u, std::memory_order_relaxed);

            bool bFound = false;
            FVector CachedPoint = FVector::ZeroVector;
            if (TryFind(InKey, KeyHash, CachedPoint, bFound))
            {
                OutPoint = CachedPoint;
                Hits.fetch_add(1u, std::memory_order_relaxed);
                return bFound;
            }

            const int32 SetIndex = static_cast<int32>(KeyHash & (NumSets - 1));
            SetLocks[SetIndex].Lock();
            if (TryFind(InKey, KeyHash, CachedPoint, bFound))
            {
                SetLocks[SetIndex].Unlock();
                OutPoint = CachedPoint;
                Hits.fetch_add(1u, std::memory_order_relaxed);
                return bFound;
            }

            FVector ComputedPoint = FVector::ZeroVector;
            bFound = Compute(ComputedPoint);
            Searches.fetch_add(1u, std::memory_order_relaxed);
            Misses.fetch_add(1u, std::memory_order_relaxed);
            Publish(SetIndex, InKey, KeyHash, bFound, ComputedPoint);
            SetLocks[SetIndex].Unlock();

            OutPoint = ComputedPoint;
            return bFound;
        }
    };

    static FVFPlayerFitMemo GPlayerFitMemo;
    static FVFRoomMouthMemo GRoomMouthMemo;
    static TAutoConsoleVariable<int32> CVarVoxelForgePlayerFitMemo(
        TEXT("voxel.PlayerFitMemo"),
        1,
        TEXT("Memoize deterministic room player-fit points; 0 disables the memo."),
        ECVF_Default);
    bool VF_FindPlayerFitPointForRoomMemoized(
        const FStrateGenerationParams& Params,
        const FVFRoomLandingSite& Site,
        float StrateTopZ,
        float StrateBottomZ,
        uint32 WarpSeed,
        FVector& OutPoint)
    {
        if (CVarVoxelForgePlayerFitMemo.GetValueOnAnyThread() == 0)
        {
            GPlayerFitMemo.Bypasses.fetch_add(1u, std::memory_order_relaxed);
            return VF_FindPlayerFitPointForRoom(
                Params, Site, StrateTopZ, StrateBottomZ, WarpSeed, OutPoint);
        }

        const FVFPlayerFitMemoKey Key = VF_MakePlayerFitMemoKey(
            Params, Site, StrateTopZ, StrateBottomZ, WarpSeed);
        return GPlayerFitMemo.FindOrCompute(
            Key,
            OutPoint,
            [&](FVector& ComputedPoint)
            {
                return VF_FindPlayerFitPointForRoom(
                    Params,
                    Site,
                    StrateTopZ,
                    StrateBottomZ,
                    WarpSeed,
                    ComputedPoint);
            });
    }

    float VF_EvaluateSlabLandingDensity(
        const FSlabGenerationParams& Params,
        uint32 Seed,
        float WorldX,
        float WorldY,
        float WorldZ)
    {
        const float StrateHeight = Params.StrateTopWorldZ - Params.StrateBottomWorldZ;
        if (!(StrateHeight > 0.0f)) return -1.0f;

        const uint32 SeedU = Seed;
        const float FloorZ = Params.StrateBottomWorldZ
            + StrateHeight * Params.FloorRelativeHeight;
        float FloorNoise = 0.0f;
        if (Params.FloorRoughness > 0.0f)
        {
            FloorNoise = VoxelNoise::FBM(
                WorldX * Params.FloorRoughnessFrequency
                    + VoxelHash::SeedOffset(SeedU, 7.3f),
                WorldY * Params.FloorRoughnessFrequency
                    + VoxelHash::SeedOffset(SeedU, 11.1f),
                0.0f, 3) * VOXEL_NOISE_SCALE * Params.FloorRoughness;
        }
        const float FloorSurface = FloorZ + FloorNoise;

        const float CeilZ = Params.StrateBottomWorldZ
            + StrateHeight * Params.CeilingRelativeHeight;
        float CeilNoise = 0.0f;
        if (Params.CeilingRoughness > 0.0f)
        {
            const float RawNoise = VoxelNoise::FBM(
                WorldX * Params.CeilingRoughnessFrequency
                    + VoxelHash::SeedOffset(SeedU, 17.3f) + 1000.0f,
                WorldY * Params.CeilingRoughnessFrequency
                    + VoxelHash::SeedOffset(SeedU, 19.7f) + 2000.0f,
                3000.0f, 3) * VOXEL_NOISE_SCALE;
            CeilNoise = FMath::Abs(RawNoise) * Params.CeilingRoughness;
        }
        const float CeilSurface = FMath::Max(
            CeilZ - CeilNoise, FloorSurface + 2.0f);
        float InternalDensity = -FMath::Min(
            WorldZ - FloorSurface, CeilSurface - WorldZ);

        if (Params.ColumnDensity > 0.0f && Params.ColumnSpacing > 0.0f)
        {
            float ColumnSDF = FLT_MAX;
            const float Spacing = Params.ColumnSpacing;
            const int32 CellX = FMath::FloorToInt(WorldX / Spacing);
            const int32 CellY = FMath::FloorToInt(WorldY / Spacing);
            for (int32 DY = -1; DY <= 1; ++DY)
            {
                for (int32 DX = -1; DX <= 1; ++DX)
                {
                    const int32 ColumnX = CellX + DX;
                    const int32 ColumnY = CellY + DY;
                    const uint32 H = VoxelHash::Cell(
                        ColumnX, ColumnY, SeedU ^ 0xC01C01u);
                    if (VoxelHash::ToFloat01(H) > Params.ColumnDensity) continue;
                    const float JX = VoxelHash::ToFloat01(
                        VoxelHash::Mix(H ^ 0x12345678u));
                    const float JY = VoxelHash::ToFloat01(
                        VoxelHash::Mix(H ^ 0x9ABCDEF0u));
                    const float CentreX = (ColumnX + 0.15f + JX * 0.7f) * Spacing;
                    const float CentreY = (ColumnY + 0.15f + JY * 0.7f) * Spacing;
                    const float Radius = FMath::Lerp(
                        Params.ColumnMinRadius, Params.ColumnMaxRadius,
                        VoxelHash::ToFloat01(VoxelHash::Mix(H ^ 0xBEEFu)));
                    ColumnSDF = FMath::Min(
                        ColumnSDF,
                        FMath::Sqrt(FMath::Square(WorldX - CentreX)
                            + FMath::Square(WorldY - CentreY)) - Radius);
                }
            }
            if (ColumnSDF < 2.0f && ColumnSDF < FLT_MAX)
            {
                float Fill = FMath::Clamp(
                    (2.0f - ColumnSDF) / 4.0f, 0.0f, 1.0f);
                InternalDensity += SmoothStep01(Fill)
                    * Params.BaseDensity * 1.5f;
            }
        }

        VF_ApplyBoundarySeal(
            InternalDensity, WorldZ, Params.StrateTopWorldZ,
            Params.StrateBottomWorldZ, Params.BoundarySealThickness,
            Params.BaseDensity);
        return -InternalDensity;
    }

    float VF_EvaluateMazeLandingDensity(
        const FMazeGenerationParams& Params,
        uint32 Seed,
        float WorldX,
        float WorldY,
        float WorldZ)
    {
        const float CellSize = FMath::Max(Params.CellSize, 1.0f);
        const FVector Position(WorldX, WorldY, WorldZ);
        const uint32 SeedU = Seed ^ 0x4D617A65u;
        const int32 CellX = FMath::FloorToInt(WorldX / CellSize);
        const int32 CellY = FMath::FloorToInt(WorldY / CellSize);
        const int32 CellZ = FMath::FloorToInt(WorldZ / CellSize);
        auto NodeCenter = [CellSize](int32 X, int32 Y, int32 Z)
        {
            return FVector(
                (X + 0.5f) * CellSize,
                (Y + 0.5f) * CellSize,
                (Z + 0.5f) * CellSize);
        };
        const float Radius = FMath::Max(Params.CorridorRadius, 0.5f);
        float MazeSDF = FLT_MAX;
        for (int32 DZ = -1; DZ <= 0; ++DZ)
        {
            for (int32 DY = -1; DY <= 0; ++DY)
            {
                for (int32 DX = -1; DX <= 0; ++DX)
                {
                    const int32 X = CellX + DX;
                    const int32 Y = CellY + DY;
                    const int32 Z = CellZ + DZ;
                    const FVector A = NodeCenter(X, Y, Z);
                    if (VoxelMazeTopology::IsOpenEdge(
                            X, Y, Z, VoxelMazeTopology::EAxis::X,
                            SeedU, Params.BranchProbability, Params.Verticality))
                    {
                        MazeSDF = FMath::Min(
                            MazeSDF,
                            VoxelSDF::Capsule(
                                Position, A, NodeCenter(X + 1, Y, Z), Radius));
                    }
                    if (VoxelMazeTopology::IsOpenEdge(
                            X, Y, Z, VoxelMazeTopology::EAxis::Y,
                            SeedU, Params.BranchProbability, Params.Verticality))
                    {
                        MazeSDF = FMath::Min(
                            MazeSDF,
                            VoxelSDF::Capsule(
                                Position, A, NodeCenter(X, Y + 1, Z), Radius));
                    }
                    if (VoxelMazeTopology::IsOpenEdge(
                            X, Y, Z, VoxelMazeTopology::EAxis::Z,
                            SeedU, Params.BranchProbability, Params.Verticality))
                    {
                        MazeSDF = FMath::Min(
                            MazeSDF,
                            VoxelSDF::Capsule(
                                Position, A, NodeCenter(X, Y, Z + 1), Radius));
                    }
                }
            }
        }

        if (Params.SurfaceRoughness > 0.0f
            && MazeSDF < Radius + Params.SurfaceRoughness + 2.0f)
        {
            MazeSDF += VoxelNoise::FBM(
                WorldX * 0.12f, WorldY * 0.12f, WorldZ * 0.12f, 3)
                * VOXEL_NOISE_SCALE * Params.SurfaceRoughness;
        }

        float InternalDensity = Params.BaseDensity;
        if (MazeSDF < 2.0f)
        {
            float Carve = FMath::Clamp(
                (2.0f - MazeSDF) / 4.0f, 0.0f, 1.0f);
            InternalDensity -= SmoothStep01(Carve)
                * Params.BaseDensity * 2.0f;
        }
        VF_ApplyBoundarySeal(
            InternalDensity, WorldZ, Params.StrateTopWorldZ,
            Params.StrateBottomWorldZ, Params.BoundarySealThickness,
            Params.BaseDensity);
        return -InternalDensity;
    }

    float VF_EvaluateShaftLandingDensity(
        const FVerticalShaftParams& Params,
        uint32 Seed,
        float WorldX,
        float WorldY,
        float WorldZ)
    {
        const float Spacing = FMath::Max(Params.ShaftSpacing, 1.0f);
        const int32 BaseCellX = FMath::FloorToInt(WorldX / Spacing);
        const int32 BaseCellY = FMath::FloorToInt(WorldY / Spacing);
        const uint32 SeedU = Seed ^ 0x53686674u;
        float CaveSDF = FLT_MAX;
        FVector NearestCentre = FVector::ZeroVector;
        float NearestDistanceSq = FLT_MAX;
        float NearestRadius = 0.0f;
        for (int32 DY = -1; DY <= 1; ++DY)
        {
            for (int32 DX = -1; DX <= 1; ++DX)
            {
                const int32 CellX = BaseCellX + DX;
                const int32 CellY = BaseCellY + DY;
                const uint32 H = VoxelHash::Cell(CellX, CellY, SeedU);
                if (VoxelHash::ToFloat01(H) > Params.ShaftDensity) continue;
                const float JX = VoxelHash::ToFloat01(
                    VoxelHash::Mix(H ^ 0x12345678u));
                const float JY = VoxelHash::ToFloat01(
                    VoxelHash::Mix(H ^ 0x9ABCDEF0u));
                const float CentreX = (CellX + 0.15f + JX * 0.7f) * Spacing;
                const float CentreY = (CellY + 0.15f + JY * 0.7f) * Spacing;
                const float Radius = FMath::Lerp(
                    Params.ShaftMinRadius, Params.ShaftMaxRadius,
                    VoxelHash::ToFloat01(VoxelHash::Mix(H ^ 0xBEEFu)));
                const float DistanceSq = FMath::Square(WorldX - CentreX)
                    + FMath::Square(WorldY - CentreY);
                if (DistanceSq < NearestDistanceSq)
                {
                    NearestDistanceSq = DistanceSq;
                    NearestCentre = FVector(CentreX, CentreY, 0.0f);
                    NearestRadius = Radius;
                }
                CaveSDF = FMath::Min(
                    CaveSDF,
                    FMath::Sqrt(DistanceSq) - Radius);
            }
        }

        float InternalDensity = Params.BaseDensity;
        if (Params.SurfaceRoughness > 0.0f
            && CaveSDF < Params.SurfaceRoughness + 4.0f)
        {
            CaveSDF += VoxelNoise::FBM(
                WorldX * 0.1f, WorldY * 0.1f, WorldZ * 0.1f, 3)
                * VOXEL_NOISE_SCALE * Params.SurfaceRoughness;
        }
        if (CaveSDF < 2.0f)
        {
            float Carve = FMath::Clamp(
                (2.0f - CaveSDF) / 4.0f, 0.0f, 1.0f);
            InternalDensity -= SmoothStep01(Carve)
                * Params.BaseDensity * 2.0f;
        }

        if (Params.LedgeSpacing > 0.0f && Params.LedgeDepth > 0.0f
            && CaveSDF < 0.0f && NearestDistanceSq < FLT_MAX)
        {
            const float Phase = FMath::Frac(
                (WorldZ - Params.StrateBottomWorldZ) / Params.LedgeSpacing);
            const float BandT = FMath::Min(Phase, 1.0f - Phase)
                * Params.LedgeSpacing;
            if (BandT < Params.LedgeDepth
                && (WorldX - NearestCentre.X) + (WorldY - NearestCentre.Y) > 1.0e-3f)
            {
                const float Shelf = 1.0f - SmoothStep01(
                    BandT / Params.LedgeDepth);
                InternalDensity = FMath::Max(
                    InternalDensity, Shelf * Params.BaseDensity);
            }
        }
        VF_ApplyBoundarySeal(
            InternalDensity, WorldZ, Params.StrateTopWorldZ,
            Params.StrateBottomWorldZ, Params.BoundarySealThickness,
            Params.BaseDensity);
        return -InternalDensity;
    }

    // Find the nearest hash room's feature core without constructing a cache. The search is
    // deliberately bounded: an empty neighbourhood is an honest "no answer", not a guessed point.
    // The returned XY is the selected room centre; only a room core is a trustworthy sparse target.
    // Cherche le centre vertical de la salle hachée la plus proche sans construire de cache. La
    // recherche est bornée : un voisinage vide signifie "pas de réponse", jamais un point inventé.
    bool VF_FindNearestHashRoomLandingPoint(
        const FStrateGenerationParams& Params,
        int32 RoomSeed,
        int32 WarpSeed,
        float StrateTopZ,
        float StrateBottomZ,
        float WorldX,
        float WorldY,
        float MaxLateralSnap,
        FVector& OutPoint)
    {
        if (!VoxelMath::IsFinite(StrateTopZ) || !VoxelMath::IsFinite(StrateBottomZ)
            || !VoxelMath::IsFinite(WorldX) || !VoxelMath::IsFinite(WorldY)
            || !VoxelMath::IsFinite(MaxLateralSnap) || MaxLateralSnap < 0.0f
            || StrateTopZ <= StrateBottomZ)
        {
            return false;
        }

        const float CellSize = Params.RoomSpacing;
        const float RadiusEnvelope = FMath::Max(Params.MinRoomRadius, Params.MaxRoomRadius);
        if (!VoxelMath::IsFinite(CellSize) || !VoxelMath::IsFinite(Params.RoomDensity)
            || !VoxelMath::IsFinite(Params.MinRoomRadius) || !VoxelMath::IsFinite(Params.MaxRoomRadius)
            || !VoxelMath::IsFinite(Params.RoomHeightRatio)
            || !VoxelMath::IsFinite(Params.BoundarySealThickness)
            || !VoxelMath::IsFinite(Params.MaxTunnelLength)
            || CellSize <= 0.0f || Params.RoomDensity <= 0.0f
            || Params.MaxTunnelLength <= 0.0f
            || RadiusEnvelope <= 0.0f || Params.RoomHeightRatio <= 0.0f
            || Params.BoundarySealThickness < 0.0f)
        {
            return false;
        }

        // PLACEMENT CONTRACT: this envelope MUST remain identical to the room-center placement
        // code in VoxelCaveMorphology::BuildChunkCache below. The pure query deliberately does
        // not call that per-chunk cache builder during Initialize, so this is intentionally a
        // second copy.
        // VoxelForge.Determinism.PassageLandsInOpenSpace is the fixed ring test that catches drift.
        // The extra room-height buffer keeps the generated room body away from both seal bands.
        const float RoomZBuffer = RadiusEnvelope * Params.RoomHeightRatio;
        const float StrateMinZ = StrateBottomZ + Params.BoundarySealThickness + RoomZBuffer;
        const float StrateMaxZ = StrateTopZ - Params.BoundarySealThickness - RoomZBuffer;
        const float StrateRangeZ = StrateMaxZ - StrateMinZ;
        if (!VoxelMath::IsFinite(StrateRangeZ) || StrateRangeZ <= 0.0f)
        {
            return false;
        }

        // A passage is placed within the configured reach from the spine. Start the search a little
        // farther than that reach, then expand deterministic rings until the nearest candidate is
        // proven; cap pathological asset values so Initialize cannot become an unbounded grid scan.
        const float SearchDistance = FMath::Max(CellSize * 2.0f,
            FMath::Max(Params.MaxTunnelLength, 0.0f));
        const float SearchCells = SearchDistance / CellSize;
        const int32 InitialSearchRadius = FMath::Min(
            32,
            FMath::Max(2, FMath::CeilToInt(SearchCells) + 1));
        constexpr int32 MaxSearchRadius = 32;

        const int32 BaseCellX = FMath::FloorToInt(WorldX / CellSize);
        const int32 BaseCellY = FMath::FloorToInt(WorldY / CellSize);

        float BestDistSq = FLT_MAX;
        float BestZ = 0.0f;
        float BestRoomX = 0.0f;
        float BestRoomY = 0.0f;
        float BestRadiusXY = 0.0f;
        float BestRadiusZ = 0.0f;
        uint32 BestHash = 0u;
        int32 BestCellX = 0;
        int32 BestCellY = 0;
        bool bFound = false;

        struct FRoomCandidate
        {
            FVFRoomLandingSite Site;
            float DistSq = FLT_MAX;
            int32 CellX = 0;
            int32 CellY = 0;
        };
        TArray<FRoomCandidate, TInlineAllocator<64>> Candidates;

        auto ConsiderCell = [&](int32 CellX, int32 CellY)
        {
            // Keep this roll byte-for-byte aligned with BuildChunkCache's room placement.
            const uint32 CellHash = VoxelHash::Cell(CellX, CellY, (uint32)RoomSeed);
            if (VoxelHash::ToFloat01(CellHash) >= Params.RoomDensity) return;

            const float JitterX = VoxelHash::ToFloat01(VoxelHash::Mix(CellHash ^ 0x12345678u));
            const float JitterY = VoxelHash::ToFloat01(VoxelHash::Mix(CellHash ^ 0x9ABCDEF0u));
            const float JitterZ = VoxelHash::ToFloat01(VoxelHash::Mix(CellHash ^ 0x55AA55AAu));

            const float RoomX = (CellX + 0.15f + JitterX * 0.7f) * CellSize;
            const float RoomY = (CellY + 0.15f + JitterY * 0.7f) * CellSize;
            const float RoomZ = StrateMinZ + JitterZ * FMath::Max(StrateRangeZ, 1.0f);
            const float DX = RoomX - WorldX;
            const float DY = RoomY - WorldY;
            const float DistSq = DX * DX + DY * DY;

            FRoomCandidate Candidate;
            Candidate.Site = FVFRoomLandingSite{
                FVector(RoomX, RoomY, RoomZ),
                0.0f,
                0.0f,
                CellHash,
                false };
            const float CandidateSizeFactor = VoxelHash::ToFloat01(
                VoxelHash::Mix(CellHash ^ 0xFEDCBA98u));
            Candidate.Site.RadiusXY = FMath::Lerp(
                FMath::Min(Params.MinRoomRadius, Params.MaxRoomRadius),
                FMath::Max(Params.MinRoomRadius, Params.MaxRoomRadius),
                CandidateSizeFactor);
            Candidate.Site.RadiusZ = Candidate.Site.RadiusXY * Params.RoomHeightRatio;
            Candidate.DistSq = DistSq;
            Candidate.CellX = CellX;
            Candidate.CellY = CellY;
            Candidates.Add(Candidate);

            const bool bCloser = DistSq < BestDistSq;
            const bool bTie = DistSq == BestDistSq
                && (CellX < BestCellX || (CellX == BestCellX && CellY < BestCellY));
            if (!bCloser && !bTie) return;

            BestDistSq = DistSq;
            BestZ = RoomZ;
            BestRoomX = RoomX;
            BestRoomY = RoomY;
            BestRadiusXY = RadiusEnvelope;
            const float SizeFactor = VoxelHash::ToFloat01(
                VoxelHash::Mix(CellHash ^ 0xFEDCBA98u));
            BestRadiusXY = FMath::Lerp(
                FMath::Min(Params.MinRoomRadius, Params.MaxRoomRadius),
                FMath::Max(Params.MinRoomRadius, Params.MaxRoomRadius),
                SizeFactor);
            BestRadiusZ = BestRadiusXY * Params.RoomHeightRatio;
            BestHash = CellHash;
            BestCellX = CellX;
            BestCellY = CellY;
            bFound = true;
        };

        auto ScanRing = [&](int32 Radius)
        {
            for (int32 CellY = BaseCellY - Radius; CellY <= BaseCellY + Radius; ++CellY)
            {
                for (int32 CellX = BaseCellX - Radius; CellX <= BaseCellX + Radius; ++CellX)
                {
                    if (FMath::Max(FMath::Abs(CellX - BaseCellX), FMath::Abs(CellY - BaseCellY)) != Radius)
                    {
                        continue;
                    }
                    ConsiderCell(CellX, CellY);
                }
            }
        };

        // The nearest possible room centre in any cell outside this square is at least this far
        // from the query point (the placement jitter is bounded to [0.15, 0.85] cell).
        auto IsNearestProven = [&](int32 Radius)
        {
            if (!bFound) return false;
            const float OutsideDistance = ((float)Radius + 0.15f) * CellSize;
            return BestDistSq < OutsideDistance * OutsideDistance;
        };

        for (int32 CellY = BaseCellY - InitialSearchRadius;
             CellY <= BaseCellY + InitialSearchRadius;
             ++CellY)
        {
            for (int32 CellX = BaseCellX - InitialSearchRadius;
                 CellX <= BaseCellX + InitialSearchRadius;
                 ++CellX)
            {
                ConsiderCell(CellX, CellY);
            }
        }

        bool bNearestProven = IsNearestProven(InitialSearchRadius);
        for (int32 Radius = InitialSearchRadius + 1;
             Radius <= MaxSearchRadius && !bNearestProven;
             ++Radius)
        {
            ScanRing(Radius);
            bNearestProven = IsNearestProven(Radius);
        }

        if (!bFound || !bNearestProven) return false;

        // The placement envelope above normally makes this automatic. Keep the explicit check so
        // malformed but finite parameters never turn a boundary value into an open-point claim.
        const float InnerTop = StrateTopZ - Params.BoundarySealThickness;
        const float InnerBottom = StrateBottomZ + Params.BoundarySealThickness;
        if (!VoxelMath::IsFinite(BestZ) || BestZ <= InnerBottom || BestZ >= InnerTop)
        {
            return false;
        }

        // ⭐ Renvoyer le CENTRE de la salle, pas le XY demandé.
        //
        // La version précédente renvoyait FVector(WorldX, WorldY, BestZ) : le Z de la salle la plus
        // proche, au XY de l'APPELANT. Un balayage d'un million de seeds a montré que ce point tombe
        // HORS de la salle sélectionnée 92,8 % du temps — la requête trouvait une salle puis visait
        // à côté. Le centre est intérieur PAR CONSTRUCTION, et c'est aussi le point qui garde la
        // plus grande marge face au warp de production (qui déplace le champ d'environ 1,5 voxel :
        // décisif au bord d'une salle, négligeable en son centre).
        //
        // Return the ROOM CENTRE, not the requested XY. The previous version returned the nearest
        // room's Z at the CALLER's XY; a million-seed sweep put that point OUTSIDE the selected room
        // 92.8% of the time — it found a room and then aimed beside it. The centre is inside by
        // construction and holds the largest margin against the production cave warp (~1.5 voxels of
        // displacement: decisive at a room's edge, negligible at its centre).
        //
        // ⚠️ Le budget est borné volontairement. Les passages sont placés à une distance délibérée
        // de la spine (0,0) pour qu'un joueur perdu puisse les retrouver ; un snap non borné
        // dissoudrait cette propriété en silence. Hors budget ⇒ on décline, et l'appelant garde son
        // ancienne portée aléatoire.
        // The budget is bounded on purpose: passages sit at a deliberate distance from the (0,0)
        // spine so a lost player can find them, and an unbounded snap would dissolve that silently.
        // Over budget => decline, and the caller keeps its old random reach.
        const float MaxLateralSnapSq = MaxLateralSnap * MaxLateralSnap;
        const float MaxStepHeightVoxels =
            FVoxelPlayerCapsuleConstants::MaxStepHeightMeters
                / FVoxelPlayerCapsuleConstants::VoxelSizeMeters;
        // The room-centre estimate is analytic, but production evaluates the room after the
        // bounded XYZ cave warp.  A support stencil must be allowed to descend through that
        // displacement as well as the authored floor relief; otherwise a valid room can be
        // incorrectly downgraded to a random mouth whenever its shape bottom is more than one
        // player step below the nominal cut.
        const float FloorReliefBound = FMath::Abs(Params.FloorReliefStrength)
            * VOXEL_NOISE_SCALE;
        const float CaveWarpBound = FMath::Abs(Params.CaveWarpStrength)
            * VOXEL_NOISE_SCALE;
        const int32 ReliefSearch = FMath::Clamp(
            FMath::CeilToInt(FloorReliefBound + CaveWarpBound) + 8,
            8,
            128);
        FVoxelStrateMeasureSettings FitSettings;

        // The nearest room is preferred, but it is not allowed to turn a local relief/shape
        // quirk into a random landing. Test every deterministic candidate inside the lateral
        // budget, nearest first, and return only after the full source fit stencil succeeds.
        Candidates.Sort([](const FRoomCandidate& A, const FRoomCandidate& B)
        {
            if (A.DistSq != B.DistSq)
            {
                return A.DistSq < B.DistSq;
            }
            return A.CellX != B.CellX ? A.CellX < B.CellX : A.CellY < B.CellY;
        });

        for (const FRoomCandidate& Candidate : Candidates)
        {
            if (Candidate.DistSq > MaxLateralSnapSq)
            {
                continue;
            }

            const FVFRoomLandingSite& Site = Candidate.Site;
            const float FloorZ = VF_RoomFloorZ(Site, static_cast<uint32>(WarpSeed), Params);
            const auto SampleRoom = [&](float X, float Y, float Z)
            {
                return VF_EvaluateRoomLandingDensity(
                    Site, Params, static_cast<uint32>(WarpSeed),
                    StrateTopZ, StrateBottomZ, X, Y, Z);
            };
            // Cave warp can move the unwarped room centre close to a wall. Probe a deterministic
            // coarse disk around that centre, nearest first, while leaving the final decision to
            // the exact capsule/stencil test. The disk stays well inside the authored room so it
            // cannot silently turn an adjacent room into this room's landing.
            const int32 LocalExtent = FMath::Max(
                2,
                FMath::FloorToInt(FMath::Min(
                    Site.RadiusXY * 0.75f,
                    FMath::Max(CaveWarpBound + 4.0f, 8.0f))));
            TArray<FIntPoint, TInlineAllocator<128>> LocalProbes;
            for (int32 LocalY = -LocalExtent; LocalY <= LocalExtent; LocalY += 2)
            {
                for (int32 LocalX = -LocalExtent; LocalX <= LocalExtent; LocalX += 2)
                {
                    if (static_cast<float>(LocalX * LocalX + LocalY * LocalY)
                        > FMath::Square(Site.RadiusXY * 0.75f))
                    {
                        continue;
                    }
                    LocalProbes.Add(FIntPoint(LocalX, LocalY));
                }
            }
            if ((LocalExtent & 1) != 0)
            {
                LocalProbes.Add(FIntPoint::ZeroValue);
            }
            LocalProbes.Sort([](const FIntPoint& A, const FIntPoint& B)
            {
                const int32 ADistanceSq = A.X * A.X + A.Y * A.Y;
                const int32 BDistanceSq = B.X * B.X + B.Y * B.Y;
                if (ADistanceSq != BDistanceSq)
                {
                    return ADistanceSq < BDistanceSq;
                }
                return A.X != B.X ? A.X < B.X : A.Y < B.Y;
            });

            for (const FIntPoint& LocalProbe : LocalProbes)
            {
                const FVector CandidateFeetBase = VF_UnwarpCavePoint(
                    FVector(
                    Site.Center.X + static_cast<float>(LocalProbe.X),
                        Site.Center.Y + static_cast<float>(LocalProbe.Y),
                        FloorZ + MaxStepHeightVoxels + 0.5f),
                    Params,
                    static_cast<uint32>(WarpSeed));

                // The production room floor includes bounded hash relief and the remaining local
                // warp variation. Search a deterministic vertical band around the analytic anchor
                // so valid rooms are never downgraded merely because the centre estimate is off by
                // a few voxels.
                for (int32 OffsetIndex = 0;
                     OffsetIndex <= ReliefSearch * 2;
                     ++OffsetIndex)
                {
                    const int32 SignedOffset = OffsetIndex == 0
                        ? 0
                        : ((OffsetIndex & 1) != 0
                            ? -((OffsetIndex + 1) / 2)
                            : OffsetIndex / 2);
                    if (FMath::Abs(SignedOffset) > ReliefSearch)
                    {
                        continue;
                    }
                    const FVector CandidateFeet = CandidateFeetBase
                        + FVector(0.0f, 0.0f, static_cast<float>(SignedOffset));
                    if (VF_ValidatePlayerFitPose(
                            FitSettings,
                            CandidateFeet,
                            StrateTopZ,
                            StrateBottomZ,
                            Params.BoundarySealThickness,
                            SampleRoom,
                            OutPoint))
                    {
                        return true;
                    }
                }
            }
        }
        return false;
    }

    // The slab source is an XY height band. Evaluate the same two pure height fields as
    // FSlabVoidSource, then choose their midpoint after intersecting the seal-free interior.
    // La source slab est une bande de hauteurs XY : on recalcule les deux champs purs comme
    // FSlabVoidSource, puis on prend leur milieu après intersection avec l'intérieur sans seal.
    bool VF_SuggestSlabLandingPoint(
        const FSlabGenerationParams& Params,
        int32 Seed,
        float StrateTopZ,
        float StrateBottomZ,
        float WorldX,
        float WorldY,
        float MaxLateralSnap,
        FVector& OutPoint)
    {
        if (!VoxelMath::IsFinite(StrateTopZ) || !VoxelMath::IsFinite(StrateBottomZ)
            || !VoxelMath::IsFinite(WorldX) || !VoxelMath::IsFinite(WorldY)
            || !VoxelMath::IsFinite(MaxLateralSnap) || MaxLateralSnap < 0.0f
            || StrateTopZ <= StrateBottomZ)
        {
            return false;
        }

        if (!VoxelMath::IsFinite(Params.FloorRelativeHeight)
            || !VoxelMath::IsFinite(Params.CeilingRelativeHeight)
            || !VoxelMath::IsFinite(Params.FloorRoughness)
            || !VoxelMath::IsFinite(Params.FloorRoughnessFrequency)
            || !VoxelMath::IsFinite(Params.CeilingRoughness)
            || !VoxelMath::IsFinite(Params.CeilingRoughnessFrequency)
            || !VoxelMath::IsFinite(Params.ColumnDensity)
            || !VoxelMath::IsFinite(Params.ColumnMinRadius)
            || !VoxelMath::IsFinite(Params.ColumnMaxRadius)
            || !VoxelMath::IsFinite(Params.ColumnSpacing)
            || !VoxelMath::IsFinite(Params.BoundarySealThickness)
            || !VoxelMath::IsFinite(Params.BaseDensity)
            || Params.BoundarySealThickness < 0.0f)
        {
            return false;
        }

        const float StrateHeight = StrateTopZ - StrateBottomZ;
        const uint32 SeedU = (uint32)Seed;

        const float FloorZ = StrateBottomZ + StrateHeight * Params.FloorRelativeHeight;
        float FloorNoise = 0.0f;
        if (Params.FloorRoughness > 0.0f)
        {
            const float FF = Params.FloorRoughnessFrequency;
            FloorNoise = VoxelNoise::FBM(
                WorldX * FF + VoxelHash::SeedOffset(SeedU, 7.3f),
                WorldY * FF + VoxelHash::SeedOffset(SeedU, 11.1f),
                0.0f, 3
            ) * VOXEL_NOISE_SCALE * Params.FloorRoughness;
        }
        const float FloorSurface = FloorZ + FloorNoise;

        const float CeilZ = StrateBottomZ + StrateHeight * Params.CeilingRelativeHeight;
        float CeilNoise = 0.0f;
        if (Params.CeilingRoughness > 0.0f)
        {
            const float CF = Params.CeilingRoughnessFrequency;
            const float RawNoise = VoxelNoise::FBM(
                WorldX * CF + VoxelHash::SeedOffset(SeedU, 17.3f) + 1000.0f,
                WorldY * CF + VoxelHash::SeedOffset(SeedU, 19.7f) + 2000.0f,
                3000.0f, 3
            ) * VOXEL_NOISE_SCALE;
            CeilNoise = FMath::Abs(RawNoise) * Params.CeilingRoughness;
        }
        const float CeilSurface = FMath::Max(CeilZ - CeilNoise, FloorSurface + 2.0f);

        // VF_ApplyBoundarySeal's bands are [Top-Thickness, Top) and
        // (Bottom, Bottom+Thickness]. The midpoint below stays strictly inside both limits.
        const float InnerTop = StrateTopZ - Params.BoundarySealThickness;
        const float InnerBottom = StrateBottomZ + Params.BoundarySealThickness;
        const float OpenBottom = FMath::Max(FloorSurface, InnerBottom);
        const float OpenTop = FMath::Min(CeilSurface, InnerTop);
        if (!VoxelMath::IsFinite(OpenBottom) || !VoxelMath::IsFinite(OpenTop) || OpenTop <= OpenBottom)
        {
            return false;
        }

        // Slab columns are infinite-height. If one reaches this XY, no Z in the slab void is a
        // confident pre-passage landing point, so report "unanswerable" instead of guessing.
        if (Params.ColumnDensity > 0.0f && Params.ColumnSpacing > 0.0f)
        {
            const float Spacing = Params.ColumnSpacing;
            const int32 ColumnCellX = FMath::FloorToInt(WorldX / Spacing);
            const int32 ColumnCellY = FMath::FloorToInt(WorldY / Spacing);

            for (int32 DY = -1; DY <= 1; ++DY)
            {
                for (int32 DX = -1; DX <= 1; ++DX)
                {
                    const int32 CellX = ColumnCellX + DX;
                    const int32 CellY = ColumnCellY + DY;
                    const uint32 H = VoxelHash::Cell(CellX, CellY, SeedU ^ 0xC01C01u);
                    if (VoxelHash::ToFloat01(H) > Params.ColumnDensity) continue;

                    const float JX = VoxelHash::ToFloat01(VoxelHash::Mix(H ^ 0x12345678u));
                    const float JY = VoxelHash::ToFloat01(VoxelHash::Mix(H ^ 0x9ABCDEF0u));
                    const float ColumnX = (CellX + 0.15f + JX * 0.7f) * Spacing;
                    const float ColumnY = (CellY + 0.15f + JY * 0.7f) * Spacing;
                    const float Radius = FMath::Lerp(Params.ColumnMinRadius, Params.ColumnMaxRadius,
                        VoxelHash::ToFloat01(VoxelHash::Mix(H ^ 0xBEEFu)));
                    const float ColumnDX = WorldX - ColumnX;
                    const float ColumnDY = WorldY - ColumnY;
                    if (FMath::Sqrt(ColumnDX * ColumnDX + ColumnDY * ColumnDY) - Radius < 2.0f)
                    {
                        return false;
                    }
                }
            }
        }

        // The old query returned the centre of the void band.  That is open air, but it is not a
        // player pose: the body must stand on the floor.  Re-run the pure slab source through the
        // same support/capsule stencil used by the measurement and return the resolved feet row.
        FSlabGenerationParams SourceParams = Params;
        SourceParams.StrateTopWorldZ = StrateTopZ;
        SourceParams.StrateBottomWorldZ = StrateBottomZ;
        FVoxelStrateMeasureSettings FitSettings;
        const float MaxStepHeightVoxels =
            FVoxelPlayerCapsuleConstants::MaxStepHeightMeters
                / FVoxelPlayerCapsuleConstants::VoxelSizeMeters;
        const FVector CandidateFeet(
            WorldX, WorldY, OpenBottom + MaxStepHeightVoxels + 0.5f);
        const auto SampleSlab = [&](float X, float Y, float Z)
        {
            return VF_EvaluateSlabLandingDensity(
                SourceParams, static_cast<uint32>(Seed), X, Y, Z);
        };
        return VF_ValidatePlayerFitPose(
            FitSettings,
            CandidateFeet,
            StrateTopZ,
            StrateBottomZ,
            SourceParams.BoundarySealThickness,
            SampleSlab,
            OutPoint);
    }

    // Maze corridors are lattice edges. Only a horizontal edge is a useful landing source: a
    // vertical edge can prove air, but cannot prove a place to stand. The edge set and Z levels
    // below use the same origin-directed tree + loop contract as FLatticeCorridorSource, with no
    // cache.
    bool VF_SuggestMazeLandingPoint(
        const FMazeGenerationParams& Params,
        int32 Seed,
        float StrateTopZ,
        float StrateBottomZ,
        float WorldX,
        float WorldY,
        float MaxLateralSnap,
        FVector& OutPoint)
    {
        if (!VoxelMath::IsFinite(StrateTopZ) || !VoxelMath::IsFinite(StrateBottomZ)
            || !VoxelMath::IsFinite(WorldX) || !VoxelMath::IsFinite(WorldY)
            || !VoxelMath::IsFinite(MaxLateralSnap) || MaxLateralSnap < 0.0f
            || StrateTopZ <= StrateBottomZ)
        {
            return false;
        }

        if (!VoxelMath::IsFinite(Params.CellSize)
            || !VoxelMath::IsFinite(Params.CorridorRadius)
            || !VoxelMath::IsFinite(Params.BranchProbability)
            || !VoxelMath::IsFinite(Params.Verticality)
            || !VoxelMath::IsFinite(Params.SurfaceRoughness)
            || !VoxelMath::IsFinite(Params.BoundarySealThickness)
            || !VoxelMath::IsFinite(Params.BaseDensity)
            || Params.CellSize <= 0.0f
            || Params.CorridorRadius <= 0.0f
            || Params.BranchProbability < 0.0f || Params.BranchProbability > 1.0f
            || Params.Verticality < 0.0f || Params.Verticality > 1.0f
            || Params.SurfaceRoughness < 0.0f
            || Params.BoundarySealThickness < 0.0f
            || Params.BaseDensity <= 0.0f)
        {
            return false;
        }

        const float CellSize = FMath::Max(Params.CellSize, 1.0f);
        const float Radius = FMath::Max(Params.CorridorRadius, 0.5f);
        // VoxelNoise::FBM has the proven absolute bound 1.5. This is the displacement that a
        // corridor radius must survive; using the nominal [-1,1] label here would certify a
        // landing inside a wall at the extremum.
        const float RoughnessBound = Params.SurfaceRoughness * VOXEL_NOISE_SCALE * 1.5f;

        // A strict interior of the tube is needed. At the MC zero surface the source's smooth
        // carve is only half applied, so stopping at the nominal radius would be an air guess.
        const float SafeRadius = Radius - RoughnessBound - 0.25f;
        if (!VoxelMath::IsFinite(SafeRadius) || SafeRadius <= 0.0f)
        {
            return false;
        }

        const float InnerBottom = StrateBottomZ + Params.BoundarySealThickness;
        const float InnerTop = StrateTopZ - Params.BoundarySealThickness;
        if (!VoxelMath::IsFinite(InnerBottom) || !VoxelMath::IsFinite(InnerTop)
            || InnerBottom >= InnerTop)
        {
            return false;
        }

        // A node centre must leave the corridor radius above the lower interior boundary. The
        // explicit checks in the loop keep ceil/floor edge cases strict after float rounding.
        const int32 MinNodeZ = FMath::CeilToInt(
            (InnerBottom + Radius) / CellSize - 0.5f);
        const int32 MaxNodeZ = FMath::FloorToInt(
            InnerTop / CellSize - 0.5f);
        constexpr int32 MaxNodeLevels = 256;
        if (MaxNodeZ < MinNodeZ || MaxNodeZ - MinNodeZ >= MaxNodeLevels)
        {
            return false;
        }

        const int32 BaseCellX = FMath::FloorToInt(WorldX / CellSize);
        const int32 BaseCellY = FMath::FloorToInt(WorldY / CellSize);
        const uint32 SeedU = (uint32)Seed ^ 0x4D617A65u;  // 'Maze'
        const float MaxSnapSq = MaxLateralSnap * MaxLateralSnap;

        float BestDistSq = FLT_MAX;
        float BestX = 0.0f;
        float BestY = 0.0f;
        float BestZ = 0.0f;
        int32 BestCellX = INT32_MAX;
        int32 BestCellY = INT32_MAX;
        int32 BestNodeZ = INT32_MAX;
        int32 BestAxis = INT32_MAX;
        bool bFound = false;

        auto ConsiderEdge = [&](int32 CellX, int32 CellY, int32 CellZ, int32 Axis)
        {
            const VoxelMazeTopology::EAxis EdgeAxis =
                static_cast<VoxelMazeTopology::EAxis>(Axis);
            if (!VoxelMazeTopology::IsOpenEdge(
                    CellX, CellY, CellZ, EdgeAxis, SeedU,
                    Params.BranchProbability, Params.Verticality))
            {
                return;
            }

            const float NodeX = (CellX + 0.5f) * CellSize;
            const float NodeY = (CellY + 0.5f) * CellSize;
            const float NodeZWorld = (CellZ + 0.5f) * CellSize;
            if (NodeZWorld - Radius <= InnerBottom || NodeZWorld >= InnerTop)
            {
                return;
            }

            float ClosestX = NodeX;
            float ClosestY = NodeY;
            if (Axis == 0)
            {
                ClosestX = FMath::Clamp(WorldX, NodeX, NodeX + CellSize);
            }
            else
            {
                ClosestY = FMath::Clamp(WorldY, NodeY, NodeY + CellSize);
            }

            const float DX = WorldX - ClosestX;
            const float DY = WorldY - ClosestY;
            const float DistSq = DX * DX + DY * DY;
            const bool bCloser = DistSq < BestDistSq;
            const bool bTie = DistSq == BestDistSq
                && (CellZ < BestNodeZ
                    || (CellZ == BestNodeZ
                        && (CellX < BestCellX
                            || (CellX == BestCellX
                                && (CellY < BestCellY
                                    || (CellY == BestCellY && Axis < BestAxis))))));
            if (!bCloser && !bTie) return;

            BestDistSq = DistSq;
            BestX = ClosestX;
            BestY = ClosestY;
            BestZ = NodeZWorld;
            BestCellX = CellX;
            BestCellY = CellY;
            BestNodeZ = CellZ;
            BestAxis = Axis;
            bFound = true;
        };

        // A candidate edge can be one cell beyond the requested point when the point is near a
        // cell boundary. The two-cell guard band covers the segment endpoint and the one-cell
        // lateral budget without turning this into an unbounded lattice scan.
        const int32 SearchRadius = FMath::CeilToInt(MaxLateralSnap / CellSize) + 2;
        for (int32 CellZ = MinNodeZ; CellZ <= MaxNodeZ; ++CellZ)
        {
            for (int32 CellY = BaseCellY - SearchRadius; CellY <= BaseCellY + SearchRadius; ++CellY)
            {
                for (int32 CellX = BaseCellX - SearchRadius; CellX <= BaseCellX + SearchRadius; ++CellX)
                {
                    ConsiderEdge(CellX, CellY, CellZ, 0);
                    ConsiderEdge(CellX, CellY, CellZ, 1);
                }
            }
        }

        if (!bFound || BestDistSq > MaxSnapSq || !VoxelMath::IsFinite(BestX)
            || !VoxelMath::IsFinite(BestY) || !VoxelMath::IsFinite(BestZ))
        {
            return false;
        }

        FMazeGenerationParams SourceParams = Params;
        SourceParams.StrateTopWorldZ = StrateTopZ;
        SourceParams.StrateBottomWorldZ = StrateBottomZ;
        FVoxelStrateMeasureSettings FitSettings;
        const float MaxStepHeightVoxels =
            FVoxelPlayerCapsuleConstants::MaxStepHeightMeters
                / FVoxelPlayerCapsuleConstants::VoxelSizeMeters;
        const auto SampleMaze = [&](float X, float Y, float Z)
        {
            return VF_EvaluateMazeLandingDensity(
                SourceParams, static_cast<uint32>(Seed), X, Y, Z);
        };

        // The nearest point on a lattice edge can be its spherical node cap, where the support
        // patch is narrower than the same edge's middle. Try a few deterministic points along
        // the selected horizontal edge, then a small vertical anchor band. Every trial remains
        // inside the original lateral budget and is checked by the exact player stencil.
        const float NodeX = (BestCellX + 0.5f) * CellSize;
        const float NodeY = (BestCellY + 0.5f) * CellSize;
        constexpr int32 EdgeSamples = 9;
        const int32 VerticalOffsets[] = { 0, 1, -1, 2, -2, 3, -3, 4, 5, 6 };
        for (int32 EdgeSample = 0; EdgeSample < EdgeSamples; ++EdgeSample)
        {
            const float T = static_cast<float>(EdgeSample) / (EdgeSamples - 1);
            const float CandidateX = BestAxis == 0 ? NodeX + T * CellSize : NodeX;
            const float CandidateY = BestAxis == 1 ? NodeY + T * CellSize : NodeY;
            const float LateralDX = CandidateX - WorldX;
            const float LateralDY = CandidateY - WorldY;
            if (LateralDX * LateralDX + LateralDY * LateralDY > MaxSnapSq)
            {
                continue;
            }

            for (const int32 VerticalOffset : VerticalOffsets)
            {
                const FVector CandidateFeet(
                    CandidateX,
                    CandidateY,
                    BestZ - Radius + RoughnessBound
                        + MaxStepHeightVoxels + 0.5f
                        + static_cast<float>(VerticalOffset));
                if (VF_ValidatePlayerFitPose(
                        FitSettings,
                        CandidateFeet,
                        StrateTopZ,
                        StrateBottomZ,
                        SourceParams.BoundarySealThickness,
                        SampleMaze,
                        OutPoint))
                {
                    return true;
                }
            }
        }
        return false;
    }

    // VerticalShafts has a guaranteed floor only when its lower boundary seal exists. Select a
    // point above that seal and outside every ledge band; an infinite shaft by itself is not a
    // standing point. The 3x3 roll is deliberately the same local source neighbourhood as the
    // production density function.
    bool VF_SuggestVerticalShaftLandingPoint(
        const FVerticalShaftParams& Params,
        int32 Seed,
        float StrateTopZ,
        float StrateBottomZ,
        float WorldX,
        float WorldY,
        float MaxLateralSnap,
        FVector& OutPoint)
    {
        if (!VoxelMath::IsFinite(StrateTopZ) || !VoxelMath::IsFinite(StrateBottomZ)
            || !VoxelMath::IsFinite(WorldX) || !VoxelMath::IsFinite(WorldY)
            || !VoxelMath::IsFinite(MaxLateralSnap) || MaxLateralSnap < 0.0f
            || StrateTopZ <= StrateBottomZ)
        {
            return false;
        }

        if (!VoxelMath::IsFinite(Params.ShaftSpacing)
            || !VoxelMath::IsFinite(Params.ShaftDensity)
            || !VoxelMath::IsFinite(Params.ShaftMinRadius)
            || !VoxelMath::IsFinite(Params.ShaftMaxRadius)
            || !VoxelMath::IsFinite(Params.CrossConnectChance)
            || !VoxelMath::IsFinite(Params.ConnectorRadius)
            || !VoxelMath::IsFinite(Params.LedgeSpacing)
            || !VoxelMath::IsFinite(Params.LedgeDepth)
            || !VoxelMath::IsFinite(Params.SurfaceRoughness)
            || !VoxelMath::IsFinite(Params.BoundarySealThickness)
            || !VoxelMath::IsFinite(Params.BaseDensity)
            || Params.ShaftSpacing <= 0.0f
            || Params.ShaftDensity <= 0.0f || Params.ShaftDensity > 1.0f
            || Params.ShaftMinRadius <= 0.0f
            || Params.ShaftMaxRadius < Params.ShaftMinRadius
            || Params.CrossConnectChance < 0.0f || Params.CrossConnectChance > 1.0f
            || Params.ConnectorRadius <= 0.0f
            || Params.LedgeSpacing < 0.0f || Params.LedgeDepth < 0.0f
            || Params.SurfaceRoughness < 0.0f
            || Params.BoundarySealThickness <= 0.0f
            || Params.BaseDensity <= 0.0f)
        {
            return false;
        }

        const float RoughnessBound = Params.SurfaceRoughness * VOXEL_NOISE_SCALE;
        const float ShaftInteriorMargin = RoughnessBound + 0.25f;
        const float Spacing = FMath::Max(Params.ShaftSpacing, 1.0f);
        const int32 BaseCellX = FMath::FloorToInt(WorldX / Spacing);
        const int32 BaseCellY = FMath::FloorToInt(WorldY / Spacing);
        const uint32 SeedU = (uint32)Seed ^ 0x53686674u;  // 'Shft'

        const float MaxSnapSq = MaxLateralSnap * MaxLateralSnap;
        const int32 SearchRadius = FMath::CeilToInt(
            (MaxLateralSnap + FMath::Max(Params.ShaftMaxRadius, 1.0f)) / Spacing) + 2;

        float BestDistSq = FLT_MAX;
        float BestAxisX = 0.0f;
        float BestAxisY = 0.0f;
        float BestSafeRadius = 0.0f;
        bool bFound = false;
        int32 BestCellX = INT32_MAX;
        int32 BestCellY = INT32_MAX;
        for (int32 DY = -SearchRadius; DY <= SearchRadius; ++DY)
        {
            for (int32 DX = -SearchRadius; DX <= SearchRadius; ++DX)
            {
                const int32 CellX = BaseCellX + DX;
                const int32 CellY = BaseCellY + DY;
                const uint32 H = VoxelHash::Cell(CellX, CellY, SeedU);
                if (VoxelHash::ToFloat01(H) > Params.ShaftDensity) continue;

                const float JX = VoxelHash::ToFloat01(VoxelHash::Mix(H ^ 0x12345678u));
                const float JY = VoxelHash::ToFloat01(VoxelHash::Mix(H ^ 0x9ABCDEF0u));
                const float ShaftX = (CellX + 0.15f + JX * 0.7f) * Spacing;
                const float ShaftY = (CellY + 0.15f + JY * 0.7f) * Spacing;
                const float Radius = FMath::Lerp(Params.ShaftMinRadius, Params.ShaftMaxRadius,
                    VoxelHash::ToFloat01(VoxelHash::Mix(H ^ 0xBEEFu)));
                const float SafeRadius = Radius - ShaftInteriorMargin;
                if (!VoxelMath::IsFinite(SafeRadius) || SafeRadius <= 0.0f)
                {
                    continue;
                }

                const float DXWorld = WorldX - ShaftX;
                const float DYWorld = WorldY - ShaftY;
                const float Distance = FMath::Sqrt(DXWorld * DXWorld + DYWorld * DYWorld);
                const float SnapDistance = FMath::Max(Distance - SafeRadius, 0.0f);
                const float DistSq = SnapDistance * SnapDistance;
                const bool bCloser = DistSq < BestDistSq;
                const bool bTie = DistSq == BestDistSq
                    && (CellX < BestCellX || (CellX == BestCellX && CellY < BestCellY));
                if (!bCloser && !bTie)
                {
                    continue;
                }

                // A VerticalShafts landing must identify the topology, not merely an open point
                // inside its radius. The safe-radius distance still chooses the nearest confident
                // site under the existing snap budget. The candidate offsets below stay inside
                // this same feature core while allowing the generated ledge to provide a real
                // walkable support patch; the exact shaft axis is an open cylinder and has no
                // support surface by itself.
                BestAxisX = ShaftX;
                BestAxisY = ShaftY;
                BestSafeRadius = SafeRadius;
                BestDistSq = DistSq;
                BestCellX = CellX;
                BestCellY = CellY;
                bFound = true;
            }
        }

        if (!bFound || BestDistSq > MaxSnapSq || !VoxelMath::IsFinite(BestAxisX)
            || !VoxelMath::IsFinite(BestAxisY))
        {
            return false;
        }

        FVerticalShaftParams SourceParams = Params;
        SourceParams.StrateTopWorldZ = StrateTopZ;
        SourceParams.StrateBottomWorldZ = StrateBottomZ;

        FVoxelStrateMeasureSettings FitSettings;
        float InnerTop = 0.0f;
        float InnerBottom = 0.0f;
        if (!VF_GetPlayerFitInteriorBounds(
                FitSettings, StrateTopZ, StrateBottomZ,
                Params.BoundarySealThickness, InnerTop, InnerBottom))
        {
            return false;
        }
        const auto SampleShaft = [&](float X, float Y, float Z)
        {
            return VF_EvaluateShaftLandingDensity(
                SourceParams, static_cast<uint32>(Seed), X, Y, Z);
        };

        // The query's default interior margin is the same 2x-seal margin used by the exact
        // player-fit measurement. This prevents a lower boundary seal from being reported as a
        // standing floor that the measured window intentionally excludes.
        const float FirstCandidateZ = InnerBottom + 0.5f;
        const float LastCandidateZ = InnerTop
            - 2.0f * FitSettings.PlayerCapsuleHalfHeightVoxels - 0.5f;
        if (!VoxelMath::IsFinite(FirstCandidateZ) || !VoxelMath::IsFinite(LastCandidateZ)
            || LastCandidateZ < FirstCandidateZ)
        {
            return false;
        }

        const int32 NumCandidateSteps = FMath::Min(
            4096,
            FMath::Max(0, FMath::CeilToInt(LastCandidateZ - FirstCandidateZ)));

        const FVector2D CandidateOffsets[] = {
            FVector2D(0.0f, 0.0f),
            FVector2D(0.20f, 0.20f),
            FVector2D(0.35f, 0.35f),
            FVector2D(0.50f, 0.15f),
            FVector2D(0.15f, 0.50f),
            FVector2D(0.55f, 0.35f),
            FVector2D(0.35f, 0.55f),
        };
        for (const FVector2D& NormalizedOffset : CandidateOffsets)
        {
            const float CandidateX = BestAxisX
                + NormalizedOffset.X * BestSafeRadius;
            const float CandidateY = BestAxisY
                + NormalizedOffset.Y * BestSafeRadius;
            const float OffsetSq = FMath::Square(CandidateX - BestAxisX)
                + FMath::Square(CandidateY - BestAxisY);
            const float LateralDX = CandidateX - WorldX;
            const float LateralDY = CandidateY - WorldY;
            const float LateralDistanceSq = FMath::Square(LateralDX)
                + FMath::Square(LateralDY);
            if (!VoxelMath::IsFinite(CandidateX) || !VoxelMath::IsFinite(CandidateY)
                || !VoxelMath::IsFinite(LateralDistanceSq)
                || OffsetSq > FMath::Square(BestSafeRadius)
                || LateralDistanceSq > MaxSnapSq + KINDA_SMALL_NUMBER)
            {
                continue;
            }

            for (int32 CandidateStep = 0;
                 CandidateStep <= NumCandidateSteps;
                 ++CandidateStep)
            {
                const FVector CandidateFeet(
                    CandidateX, CandidateY,
                    FirstCandidateZ + static_cast<float>(CandidateStep));
                if (VF_ValidatePlayerFitPose(
                        FitSettings,
                        CandidateFeet,
                        StrateTopZ,
                        StrateBottomZ,
                        Params.BoundarySealThickness,
                        SampleShaft,
                        OutPoint))
                {
                    return true;
                }
            }
        }
        return false;
    }

    struct FVFIslandSite
    {
        int32 CellX, CellY;
        float X, Y, Rxy, TopHalf, TopZ, BotZ, TaperEnd;
    };

    // Evaluate only the island source, in the same internal-density convention as
    // FIslandBlobSource (positive = solid, negative = void). This is intentionally a
    // local pure evaluator: it lets the query bracket the actual blob top instead of treating a
    // nominal island centre as footing. Full-octave noise is used because this query has no LOD
    // state by design and GeneratePassages runs before a tile's LOD is selected.
    float VF_EvaluateIslandInteriorDensity(
        const FFloatingIslandParams& Params,
        uint32 Seed,
        const FVFIslandSite* Islands,
        int32 NumIslands,
        float WorldX,
        float WorldY,
        float WorldZ)
    {
        const float WarpAmp = (Params.IslandMinRadius + Params.IslandMaxRadius) * 0.5f * 0.35f;
        const FVector WarpXPosition(
            WorldX * 0.04f + VoxelHash::SeedOffset(Seed, 0.0007f),
            WorldY * 0.04f,
            WorldZ * 0.012f);
        const FVector WarpYPosition(
            WorldX * 0.04f + 31.0f,
            WorldY * 0.04f + 7.0f,
            WorldZ * 0.012f);
        const float WX = WorldX + VoxelNoise::FBM(
            (float)WarpXPosition.X, (float)WarpXPosition.Y, (float)WarpXPosition.Z, 3)
            * VOXEL_NOISE_SCALE * WarpAmp;
        const float WY = WorldY + VoxelNoise::FBM(
            (float)WarpYPosition.X, (float)WarpYPosition.Y, (float)WarpYPosition.Z, 3)
            * VOXEL_NOISE_SCALE * WarpAmp;

        const float BlendK = FMath::Max(Params.SDFBlendRadius, 0.01f);
        const float Spacing = FMath::Max(Params.IslandSpacing, 1.0f);
        const int32 BaseCellX = FMath::FloorToInt(WorldX / Spacing);
        const int32 BaseCellY = FMath::FloorToInt(WorldY / Spacing);
        float IslandSDF = FLT_MAX;
        for (int32 IslandIndex = 0; IslandIndex < NumIslands; ++IslandIndex)
        {
            const FVFIslandSite& Island = Islands[IslandIndex];
            // Production FIslandBlobSource evaluates exactly this 3x3 cell neighbourhood
            // for the queried XY. The outer search may collect more candidates so a lateral snap
            // can be found, but those distant blobs must not influence the returned footing.
            if (FMath::Abs(Island.CellX - BaseCellX) > 1
                || FMath::Abs(Island.CellY - BaseCellY) > 1)
            {
                continue;
            }
            const float DX = WX - Island.X;
            const float DY = WY - Island.Y;
            const float DistXY = FMath::Sqrt(DX * DX + DY * DY);
            const float Height = FMath::Clamp(
                (WorldZ - Island.BotZ) / FMath::Max(Island.TopZ - Island.BotZ, 1.0f),
                0.0f, 1.0f);
            const float Taper = SmoothStep01(FMath::Clamp(
                Height / Island.TaperEnd, 0.0f, 1.0f));
            const float Envelope = Island.Rxy * Taper;

            float TopSurface = Island.TopZ;
            if (Params.TopFlatten < 1.0f)
            {
                const float Edge = FMath::Clamp(
                    DistXY / FMath::Max(Island.Rxy, 1.0f), 0.0f, 1.0f);
                TopSurface = Island.TopZ
                    - (1.0f - Params.TopFlatten) * Island.TopHalf * 2.0f * Edge * Edge;
            }

            const float SDF = FMath::Max(DistXY - Envelope, WorldZ - TopSurface);
            IslandSDF = VoxelSDF::SmoothMin(IslandSDF, SDF, BlendK);
        }

        if (Params.SurfaceRoughness > 0.0f
            && IslandSDF < Params.SurfaceRoughness + BlendK + 2.0f)
        {
            const FVector RoughnessPosition(WorldX * 0.08f, WorldY * 0.08f, WorldZ * 0.08f);
            IslandSDF += VoxelNoise::FBM(
                (float)RoughnessPosition.X, (float)RoughnessPosition.Y,
                (float)RoughnessPosition.Z, 4)
                * VOXEL_NOISE_SCALE * Params.SurfaceRoughness;
        }

        float Density = -Params.BaseDensity;
        if (IslandSDF < BlendK)
        {
            float Fill = FMath::Clamp(
                (BlendK - IslandSDF) / (BlendK * 2.0f), 0.0f, 1.0f);
            Fill = SmoothStep01(Fill);
            Density += Fill * Params.BaseDensity * 2.0f;
        }
        return Density;
    }

    bool VF_SuggestFloatingIslandLandingPoint(
        const FFloatingIslandParams& Params,
        int32 Seed,
        float StrateTopZ,
        float StrateBottomZ,
        float WorldX,
        float WorldY,
        float MaxLateralSnap,
        FVector& OutPoint)
    {
        if (!VoxelMath::IsFinite(StrateTopZ) || !VoxelMath::IsFinite(StrateBottomZ)
            || !VoxelMath::IsFinite(WorldX) || !VoxelMath::IsFinite(WorldY)
            || !VoxelMath::IsFinite(MaxLateralSnap) || MaxLateralSnap < 0.0f
            || StrateTopZ <= StrateBottomZ)
        {
            return false;
        }

        if (!VoxelMath::IsFinite(Params.IslandSpacing)
            || !VoxelMath::IsFinite(Params.IslandDensity)
            || !VoxelMath::IsFinite(Params.IslandMinRadius)
            || !VoxelMath::IsFinite(Params.IslandMaxRadius)
            || !VoxelMath::IsFinite(Params.ThicknessRatio)
            || !VoxelMath::IsFinite(Params.VerticalJitter)
            || !VoxelMath::IsFinite(Params.TopFlatten)
            || !VoxelMath::IsFinite(Params.SurfaceRoughness)
            || !VoxelMath::IsFinite(Params.SDFBlendRadius)
            || !VoxelMath::IsFinite(Params.BoundarySealThickness)
            || !VoxelMath::IsFinite(Params.BaseDensity)
            || Params.IslandSpacing <= 0.0f
            || Params.IslandDensity <= 0.0f || Params.IslandDensity > 1.0f
            || Params.IslandMinRadius <= 0.0f
            || Params.IslandMaxRadius < Params.IslandMinRadius
            || Params.ThicknessRatio < 0.0f
            || Params.VerticalJitter < 0.0f
            || Params.SurfaceRoughness < 0.0f
            || Params.BoundarySealThickness < 0.0f
            || Params.BaseDensity <= 0.0f)
        {
            return false;
        }

        const float H = StrateTopZ - StrateBottomZ;
        const float Spacing = FMath::Max(Params.IslandSpacing, 1.0f);
        const uint32 SeedU = (uint32)Seed ^ 0x49736C64u;  // 'Isld'
        const float MidZ = (StrateTopZ + StrateBottomZ) * 0.5f;

        const float WarpAmp = (Params.IslandMinRadius + Params.IslandMaxRadius) * 0.5f * 0.35f;
        const float WarpBound = WarpAmp * VOXEL_NOISE_SCALE;
        const float MaxIslandRadius = FMath::Max(Params.IslandMaxRadius, 1.0f);
        const float SearchReach = MaxLateralSnap + MaxIslandRadius + WarpBound + 2.0f;
        const int32 SearchRadius = FMath::CeilToInt(SearchReach / Spacing) + 2;
        const int32 BaseCellX = FMath::FloorToInt(WorldX / Spacing);
        const int32 BaseCellY = FMath::FloorToInt(WorldY / Spacing);

        TArray<FVFIslandSite, TInlineAllocator<64>> Islands;
        Islands.Reserve((2 * SearchRadius + 1) * (2 * SearchRadius + 1));
        float MinBotZ = FLT_MAX;
        float MaxTopZ = -FLT_MAX;
        for (int32 DY = -SearchRadius; DY <= SearchRadius; ++DY)
        {
            for (int32 DX = -SearchRadius; DX <= SearchRadius; ++DX)
            {
                const int32 CellX = BaseCellX + DX;
                const int32 CellY = BaseCellY + DY;
                const uint32 Hh = VoxelHash::Cell(CellX, CellY, SeedU);
                if (VoxelHash::ToFloat01(Hh) > Params.IslandDensity) continue;

                FVFIslandSite& Island = Islands.AddDefaulted_GetRef();
                Island.CellX = CellX;
                Island.CellY = CellY;
                const float JX = VoxelHash::ToFloat01(VoxelHash::Mix(Hh ^ 0x12345678u));
                const float JY = VoxelHash::ToFloat01(VoxelHash::Mix(Hh ^ 0x9ABCDEF0u));
                Island.X = (CellX + 0.15f + JX * 0.7f) * Spacing;
                Island.Y = (CellY + 0.15f + JY * 0.7f) * Spacing;
                Island.Rxy = FMath::Lerp(Params.IslandMinRadius, Params.IslandMaxRadius,
                    VoxelHash::ToFloat01(VoxelHash::Mix(Hh ^ 0x5A5Au)));
                Island.TopHalf = Island.Rxy * 0.20f;
                const float UnderDepth = Island.Rxy * FMath::Max(Params.ThicknessRatio, 0.25f);
                const float SpreadZ = FMath::Max(
                    H * 0.5f - FMath::Max(Island.TopHalf, UnderDepth)
                    - Params.BoundarySealThickness, 0.0f) * Params.VerticalJitter;
                const float CenterZ = MidZ
                    + VoxelHash::ToFloatSigned(VoxelHash::Mix(Hh ^ 0xB17Du)) * SpreadZ;
                Island.TopZ = CenterZ + Island.TopHalf;
                Island.BotZ = CenterZ - UnderDepth;
                Island.TaperEnd = FMath::Lerp(0.45f, 0.7f,
                    VoxelHash::ToFloat01(VoxelHash::Mix(Hh ^ 0x7A1Eu)));

                MinBotZ = FMath::Min(MinBotZ, Island.BotZ);
                MaxTopZ = FMath::Max(MaxTopZ, Island.TopZ);
            }
        }

        if (Islands.Num() == 0 || !VoxelMath::IsFinite(MinBotZ) || !VoxelMath::IsFinite(MaxTopZ))
        {
            return false;
        }

        const float RoughnessBound = Params.SurfaceRoughness * VOXEL_NOISE_SCALE;
        const float BlendK = FMath::Max(Params.SDFBlendRadius, 0.01f);
        const float SearchPad = RoughnessBound + BlendK + 3.0f;
        const float SearchTop = MaxTopZ + SearchPad;
        const float SearchBottom = MinBotZ - SearchPad;
        constexpr float ScanStep = 0.5f;
        constexpr int32 MaxVerticalSamples = 2048;
        if (!VoxelMath::IsFinite(SearchTop) || !VoxelMath::IsFinite(SearchBottom)
            || SearchTop <= SearchBottom
            || (SearchTop - SearchBottom) / ScanStep > (float)MaxVerticalSamples)
        {
            return false;
        }

        const float MaxSnapSq = MaxLateralSnap * MaxLateralSnap;
        float BestDistSq = FLT_MAX;
        FVector BestPoint = FVector::ZeroVector;
        int32 BestIslandIndex = INDEX_NONE;
        FVoxelStrateMeasureSettings FitSettings;

        auto FindLandingZ = [&](float CandidateX, float CandidateY, float& OutLandingZ) -> bool
        {
            float UpperZ = SearchTop;
            float UpperDensity = VF_EvaluateIslandInteriorDensity(
                Params, SeedU, Islands.GetData(), Islands.Num(), CandidateX, CandidateY, UpperZ);
            if (!VoxelMath::IsFinite(UpperDensity) || UpperDensity > 0.0f)
            {
                return false;
            }

            for (int32 Sample = 0; Sample < MaxVerticalSamples && UpperZ > SearchBottom; ++Sample)
            {
                const float LowerZ = FMath::Max(SearchBottom, UpperZ - ScanStep);
                const float LowerDensity = VF_EvaluateIslandInteriorDensity(
                    Params, SeedU, Islands.GetData(), Islands.Num(), CandidateX, CandidateY, LowerZ);
                if (!VoxelMath::IsFinite(LowerDensity)) return false;

                // Descending from guaranteed air, the first solid bracket is the highest actual
                // blob top at this XY. That is the footing surface; no island-centre guess is
                // involved.
                if (UpperDensity <= 0.0f && LowerDensity > 0.0f)
                {
                    float SolidZ = LowerZ;
                    float AirZ = UpperZ;
                    for (int32 Iteration = 0; Iteration < 12; ++Iteration)
                    {
                        const float Mid = (SolidZ + AirZ) * 0.5f;
                        const float MidDensity = VF_EvaluateIslandInteriorDensity(
                            Params, SeedU, Islands.GetData(), Islands.Num(), CandidateX, CandidateY, Mid);
                        if (!VoxelMath::IsFinite(MidDensity)) return false;
                        if (MidDensity > 0.0f) SolidZ = Mid;
                        else                   AirZ = Mid;
                    }

                    const float InnerBottom = StrateBottomZ + Params.BoundarySealThickness;
                    const float InnerTop = StrateTopZ - Params.BoundarySealThickness;
                    const float LandingZ = AirZ + 0.5f;
                    if (!VoxelMath::IsFinite(LandingZ)
                        || LandingZ <= InnerBottom || LandingZ >= InnerTop
                        || VF_EvaluateIslandInteriorDensity(
                            Params, SeedU, Islands.GetData(), Islands.Num(), CandidateX, CandidateY, LandingZ) > 0.0f
                        || VF_EvaluateIslandInteriorDensity(
                            Params, SeedU, Islands.GetData(), Islands.Num(), CandidateX, CandidateY, SolidZ) <= 0.0f
                        || VF_EvaluateIslandInteriorDensity(
                            Params, SeedU, Islands.GetData(), Islands.Num(), CandidateX, CandidateY, LandingZ - 1.0f) <= 0.0f)
                    {
                        return false;
                    }

                    OutLandingZ = LandingZ;
                    return VoxelMath::IsFinite(OutLandingZ);
                }

                UpperZ = LowerZ;
                UpperDensity = LowerDensity;
            }

            return false;
        };

        const auto SampleIsland = [&](float X, float Y, float Z)
        {
            // Island density uses the opposite convention internally (positive = solid).
            return -VF_EvaluateIslandInteriorDensity(
                Params, SeedU, Islands.GetData(), Islands.Num(), X, Y, Z);
        };
        const float MaxStepHeightVoxels =
            FVoxelPlayerCapsuleConstants::MaxStepHeightMeters
                / FVoxelPlayerCapsuleConstants::VoxelSizeMeters;
        const FIntPoint CoreOffsets[] = {
            FIntPoint(0, 0),
            FIntPoint(1, 0), FIntPoint(-1, 0),
            FIntPoint(0, 1), FIntPoint(0, -1),
            FIntPoint(2, 0), FIntPoint(-2, 0),
            FIntPoint(0, 2), FIntPoint(0, -2),
            FIntPoint(1, 1), FIntPoint(1, -1),
            FIntPoint(-1, 1), FIntPoint(-1, -1),
            FIntPoint(3, 0), FIntPoint(-3, 0),
            FIntPoint(0, 3), FIntPoint(0, -3)
        };
        const float FeetOffsets[] = { 0.0f, -1.0f, 1.0f, -2.0f, 2.0f, 3.0f };

        for (int32 IslandIndex = 0; IslandIndex < Islands.Num(); ++IslandIndex)
        {
            const FVFIslandSite& Island = Islands[IslandIndex];
            // Reserve a conservative interior disk. WarpBound protects the source's lobed frame;
            // the one-voxel margin keeps a returned top away from the nominal radial edge. If no
            // such disk is within MaxLateralSnap, declining is preferable to an arbitrary jump.
            const float SafeRadius = Island.Rxy - WarpBound - 1.0f;
            if (!VoxelMath::IsFinite(SafeRadius) || SafeRadius <= 0.0f) continue;

            const float DX = WorldX - Island.X;
            const float DY = WorldY - Island.Y;
            const float Distance = FMath::Sqrt(DX * DX + DY * DY);
            const float SnapDistance = FMath::Max(Distance - SafeRadius, 0.0f);
            const float DistSq = SnapDistance * SnapDistance;
            if (DistSq > MaxSnapSq) continue;

            float CandidateX = WorldX;
            float CandidateY = WorldY;
            // Prefer the feature core whenever it fits the lateral budget. A point near the
            // nominal blob rim can be open air yet fail the support patch; the deterministic
            // centre is the island's large, stable landing feature.
            if (Distance <= MaxLateralSnap)
            {
                CandidateX = Island.X;
                CandidateY = Island.Y;
            }
            else if (Distance > SafeRadius && Distance > KINDA_SMALL_NUMBER)
            {
                const float Scale = SafeRadius / Distance;
                CandidateX = Island.X + DX * Scale;
                CandidateY = Island.Y + DY * Scale;
            }

            const float CandidateDX = CandidateX - WorldX;
            const float CandidateDY = CandidateY - WorldY;
            const float CandidateDistSq =
                CandidateDX * CandidateDX + CandidateDY * CandidateDY;
            if (CandidateDistSq > MaxSnapSq)
            {
                continue;
            }

            // A top surface is only a landing site if the complete player stencil can occupy the
            // air above it and the support patch is walkable.  The core offsets are a bounded,
            // deterministic local search for a broad patch; they are not a component query.
            for (const FIntPoint& CoreOffset : CoreOffsets)
            {
                const float TrialX = CandidateX + static_cast<float>(CoreOffset.X);
                const float TrialY = CandidateY + static_cast<float>(CoreOffset.Y);
                const float TrialDX = TrialX - Island.X;
                const float TrialDY = TrialY - Island.Y;
                if (TrialDX * TrialDX + TrialDY * TrialDY
                        > FMath::Square(SafeRadius) + KINDA_SMALL_NUMBER)
                {
                    continue;
                }
                const float LateralDX = TrialX - WorldX;
                const float LateralDY = TrialY - WorldY;
                const float TrialDistSq = LateralDX * LateralDX + LateralDY * LateralDY;
                if (TrialDistSq > MaxSnapSq) continue;

                float CandidateZ = 0.0f;
                if (!FindLandingZ(TrialX, TrialY, CandidateZ)) continue;
                for (const float FeetOffset : FeetOffsets)
                {
                    FVector FitPoint = FVector::ZeroVector;
                    if (!VF_ValidatePlayerFitPose(
                            FitSettings,
                            FVector(TrialX, TrialY,
                                CandidateZ + FeetOffset + MaxStepHeightVoxels),
                            StrateTopZ,
                            StrateBottomZ,
                            Params.BoundarySealThickness,
                            SampleIsland,
                            FitPoint))
                    {
                        continue;
                    }

                    const bool bCloser = TrialDistSq < BestDistSq;
                    const bool bTie = TrialDistSq == BestDistSq
                        && IslandIndex < BestIslandIndex;
                    if (!bCloser && !bTie) continue;

                    BestDistSq = TrialDistSq;
                    BestPoint = FitPoint;
                    BestIslandIndex = IslandIndex;
                }
            }
        }

        if (BestIslandIndex == INDEX_NONE || BestDistSq > MaxSnapSq || BestPoint.ContainsNaN())
        {
            return false;
        }

        OutPoint = BestPoint;
        return VoxelMath::IsFinite(OutPoint.X) && VoxelMath::IsFinite(OutPoint.Y)
            && VoxelMath::IsFinite(OutPoint.Z);
    }
}

namespace
{
    constexpr float VF_LandingCarveSafetyMargin =
        VoxelPassageGeometry::SealSafetyMarginVoxels;
}

FVector VoxelCaveMorphology::ApplyCaveWarp(
    const FVector& WorldPoint,
    const FStrateGenerationParams& Params,
    uint32 Seed)
{
    return VF_ApplyCaveWarp(WorldPoint, Params, Seed);
}

void VoxelCaveMorphology::ConfigurePlayerFitMemoFromCommandLine()
{
    int32 CommandLineValue = CVarVoxelForgePlayerFitMemo.GetValueOnAnyThread();
    const bool bCommandLineValue = FParse::Value(
        FCommandLine::Get(),
        TEXT("voxel.PlayerFitMemo="),
        CommandLineValue);
    if (bCommandLineValue)
    {
        CVarVoxelForgePlayerFitMemo->Set(
            CommandLineValue != 0 ? 1 : 0,
            ECVF_SetByCommandline);
    }

    UE_LOG(LogTemp, Display,
        TEXT("[VoxelPlayerFitMemo] configured enabled=%d source=%s sets=%d ways=%d ")
             TEXT("capacity=%d key_words=%d entry_bytes=%llu table_bytes=%llu"),
        CVarVoxelForgePlayerFitMemo.GetValueOnAnyThread() != 0 ? 1 : 0,
        bCommandLineValue ? TEXT("commandline") : TEXT("cvar/default"),
        FVFPlayerFitMemo::NumSets,
        FVFPlayerFitMemo::NumWays,
        FVFPlayerFitMemo::Capacity,
        VF_PlayerFitKeyWordCount,
        static_cast<unsigned long long>(sizeof(FVFPlayerFitMemoSlot)),
        static_cast<unsigned long long>(sizeof(FVFPlayerFitMemo)));
}

void VoxelCaveMorphology::ResetPlayerFitMemoStats()
{
    GPlayerFitMemo.ResetStats();
}

void VoxelCaveMorphology::LogPlayerFitMemoStats()
{
    const uint64 Requests = GPlayerFitMemo.Requests.load(std::memory_order_relaxed);
    const uint64 Hits = GPlayerFitMemo.Hits.load(std::memory_order_relaxed);
    const uint64 Misses = GPlayerFitMemo.Misses.load(std::memory_order_relaxed);
    const double HitRate = Requests > 0
        ? static_cast<double>(Hits) / static_cast<double>(Requests)
        : 0.0;
    UE_LOG(LogTemp, Display,
        TEXT("[VoxelPlayerFitMemo] enabled=%d requests=%llu hits=%llu misses=%llu ")
             TEXT("hit_rate=%.6f searches=%llu bypasses=%llu entries=%llu/%d ")
             TEXT("bytes=%llu evictions=%llu"),
        CVarVoxelForgePlayerFitMemo.GetValueOnAnyThread() != 0 ? 1 : 0,
        static_cast<unsigned long long>(Requests),
        static_cast<unsigned long long>(Hits),
        static_cast<unsigned long long>(Misses),
        HitRate,
        static_cast<unsigned long long>(GPlayerFitMemo.Searches.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(GPlayerFitMemo.Bypasses.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(GPlayerFitMemo.OccupiedEntries.load(std::memory_order_relaxed)),
        FVFPlayerFitMemo::Capacity,
        static_cast<unsigned long long>(sizeof(FVFPlayerFitMemo)),
        static_cast<unsigned long long>(GPlayerFitMemo.Evictions.load(std::memory_order_relaxed)));

    UE_LOG(LogTemp, Display,
        TEXT("[VoxelRoomMouthMemo] requests=%llu hits=%llu misses=%llu searches=%llu "
             "entries=%llu/%d bytes=%llu evictions=%llu"),
        static_cast<unsigned long long>(GRoomMouthMemo.Requests.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(GRoomMouthMemo.Hits.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(GRoomMouthMemo.Misses.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(GRoomMouthMemo.Searches.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(GRoomMouthMemo.OccupiedEntries.load(std::memory_order_relaxed)),
        FVFRoomMouthMemo::Capacity,
        static_cast<unsigned long long>(sizeof(GRoomMouthMemo)),
        static_cast<unsigned long long>(GRoomMouthMemo.Evictions.load(std::memory_order_relaxed)));
}

FVoxelPassageLanding VF_BuildPassageLanding(
    const FVector& InStandingPoint,
    float MouthRadius,
    const FVector& InDoorDirection,
    float StrateTopZ,
    float StrateBottomZ,
    float BoundarySealThickness,
    bool bSourcePlayerFit)
{
    FVoxelPassageLanding Landing;

    // Body-derived dimensions at the authored 25 cm voxel scale are shared with the origin
    // landing.  In particular, the 0.68 m player diameter, 3 m turning patch, 1.76 m capsule
    // height and 1 m headroom become the common §6.5 formulas in VoxelPassageGeometry.
    const float SafeMouthRadius = FMath::Max(FMath::Abs(MouthRadius), 1.0f);
    const float DesiredHalfWidth = VoxelPassageGeometry::LandingHalfWidthForRadius(MouthRadius);
    const float DesiredHeight = VoxelPassageGeometry::LandingHeightForRadius(MouthRadius);
    const float SafeSeal = FMath::Max(BoundarySealThickness, 0.0f);

    FVector Direction(InDoorDirection.X, InDoorDirection.Y, 0.0f);
    if (!Direction.Normalize())
    {
        Direction = FVector(1.0f, 0.0f, 0.0f);
    }

    Landing.StandingPoint = InStandingPoint;
    Landing.FloorZ = InStandingPoint.Z - 0.5f;
    Landing.HalfWidth = DesiredHalfWidth;
    Landing.CeilingZ = Landing.FloorZ + DesiredHeight;
    Landing.FloorThickness = VoxelPassageGeometry::LandingFloorThicknessVoxels;
    Landing.DoorDirection = Direction;
    Landing.bSourcePlayerFit = bSourcePlayerFit;

    // Keep the complete room and its floor strictly inside the vertical seal. Normal authored
    // strates have far more room than this bound; the clamp is a fail-safe for a source pose near
    // a seal and never moves a normal interior pose.
    const float InnerBottom = StrateBottomZ + SafeSeal;
    const float InnerTop = StrateTopZ - SafeSeal;
    if (VoxelMath::IsFinite(InnerBottom) && VoxelMath::IsFinite(InnerTop)
        && InnerTop > InnerBottom)
    {
        // Passage carving is intentionally ordered after the vertical seal and fades over its
        // four-voxel blend radius. Keep both the underside of the support slab and the top of the
        // room beyond that radius from the seal, so the landing cannot use the passage exception
        // to open a strate boundary.
        const float MinFloor = InnerBottom + Landing.FloorThickness
            + VF_LandingCarveSafetyMargin;
        const float MaxFloor = InnerTop - DesiredHeight - VF_LandingCarveSafetyMargin;
        if (MaxFloor >= MinFloor)
        {
            Landing.FloorZ = FMath::Clamp(Landing.FloorZ, MinFloor, MaxFloor);
            Landing.StandingPoint.Z = Landing.FloorZ + 0.5f;
            Landing.CeilingZ = Landing.FloorZ + DesiredHeight;
        }
        else
        {
            // A degenerate strate cannot satisfy the full body/headroom contract. Fail closed
            // instead of emitting a partial chamber whose blend would approach a seal.
            Landing.HalfWidth = 0.0f;
            Landing.CeilingZ = Landing.FloorZ;
            Landing.bSourcePlayerFit = false;
            return Landing;
        }
    }

    // The tube centreline is tangent to the floor: the tube bottom is exactly FloorZ.  The door
    // sits one voxel inside the room so the room/tube smooth union cannot leave a lip.
    Landing.DoorPoint = Landing.StandingPoint
        + Direction * FMath::Max(0.0f, Landing.HalfWidth - 1.0f);
    Landing.DoorPoint.Z = Landing.FloorZ + SafeMouthRadius;

    return Landing;
}

float VF_EvaluatePassageLandingSDF(
    const FVector& Position,
    const FVoxelPassageLanding& Landing)
{
    if (!VoxelMath::IsFinite(Position.X) || !VoxelMath::IsFinite(Position.Y)
        || !VoxelMath::IsFinite(Position.Z)
        || !VoxelMath::IsFinite(Landing.StandingPoint.X)
        || !VoxelMath::IsFinite(Landing.StandingPoint.Y)
        || !VoxelMath::IsFinite(Landing.FloorZ)
        || !VoxelMath::IsFinite(Landing.CeilingZ)
        || !VoxelMath::IsFinite(Landing.HalfWidth)
        || Landing.HalfWidth <= 0.0f
        || Landing.CeilingZ <= Landing.FloorZ)
    {
        return FLT_MAX;
    }

    const FVector RoomCenter(
        Landing.StandingPoint.X,
        Landing.StandingPoint.Y,
        (Landing.FloorZ + Landing.CeilingZ) * 0.5f);
    constexpr float RoomRounding = 1.5f;
    const FVector RoomHalfExtent(
        FMath::Max(Landing.HalfWidth - RoomRounding, 0.25f),
        FMath::Max(Landing.HalfWidth - RoomRounding, 0.25f),
        FMath::Max((Landing.CeilingZ - Landing.FloorZ) * 0.5f - RoomRounding, 0.25f));

    // max(Room, FloorZ-Z) is the important distinction from a bore: below FloorZ is solid,
    // while every horizontal section above it has the same flat floor plane.
    float LandingSDF = FMath::Max(
        VoxelSDF::RoundedBox(Position, RoomCenter, RoomHalfExtent, RoomRounding),
        Landing.FloorZ - Position.Z);

    return LandingSDF;
}

bool VF_IsPassageLandingFloor(
    const FVector& Position,
    const FVoxelPassageLanding& Landing)
{
    if (!VoxelMath::IsFinite(Position.X) || !VoxelMath::IsFinite(Position.Y)
        || !VoxelMath::IsFinite(Position.Z) || !VoxelMath::IsFinite(Landing.FloorZ)
        || !VoxelMath::IsFinite(Landing.FloorThickness)
        || Landing.FloorThickness <= 0.0f || Landing.HalfWidth <= 0.0f)
    {
        return false;
    }

    const auto IsInFloorBand = [](float Z, float FloorZ, float Thickness)
    {
        // The analytic ramp and the sampled density use the same float interpolation, but a
        // caller can arrive at the mathematically identical plane through a different fused
        // multiply/add sequence. Include one machine epsilon on the upper side so the support
        // backstop cannot disappear at an exact floor sample.
        return Z <= FloorZ + KINDA_SMALL_NUMBER && Z > FloorZ - Thickness;
    };
    // The room floor is a true horizontal landing: it is the only floor that owns the full
    // three-metre turning patch.  The origin room owns its centre, including its support floor.
    if (IsInFloorBand(Position.Z, Landing.FloorZ, Landing.FloorThickness))
    {
        const float FloorHalfWidth = FMath::Max(Landing.HalfWidth - 1.0f, 0.0f);
        if (FMath::Abs(Position.X - Landing.StandingPoint.X) <= FloorHalfWidth
            && FMath::Abs(Position.Y - Landing.StandingPoint.Y) <= FloorHalfWidth)
        {
            return true;
        }
    }

    return false;
}

bool VF_SuggestLandingPoint(
    ECaveGeneratorType Archetype,
    const FStrateGenerationParams& CaveParams,
    const FSlabGenerationParams& SlabParams,
    int32 Seed,
    float StrateTopZ,
    float StrateBottomZ,
    float DesiredX,
    float DesiredY,
    float MaxLateralSnap,
    FVector& OutPoint,
    int32 WorldSeed)
{
    switch (Archetype)
    {
    case ECaveGeneratorType::TunnelNetwork:
    case ECaveGeneratorType::Underwater:
        return VF_FindNearestHashRoomLandingPoint(
            CaveParams, Seed,
            WorldSeed == MIN_int32 ? Seed : WorldSeed,
            StrateTopZ, StrateBottomZ,
                                                  DesiredX, DesiredY, MaxLateralSnap, OutPoint);

    case ECaveGeneratorType::FlatPlain:
    case ECaveGeneratorType::CrystalChamber:
        return VF_SuggestSlabLandingPoint(SlabParams, Seed, StrateTopZ, StrateBottomZ,
                                           DesiredX, DesiredY, MaxLateralSnap, OutPoint);

    default:
        // This overload intentionally covers only archetypes whose placement parameters are
        // already present in the two legacy structs. The complete overload below carries the
        // lattice/grid/blob parameters and answers those sources without guessing.
        return false;
    }
}

bool VF_SuggestLandingPoint(
    ECaveGeneratorType Archetype,
    const FStrateGenerationParams& CaveParams,
    const FSlabGenerationParams& SlabParams,
    const FMazeGenerationParams& MazeParams,
    const FVerticalShaftParams& VerticalShaftParams,
    const FFloatingIslandParams& FloatingIslandParams,
    int32 Seed,
    float StrateTopZ,
    float StrateBottomZ,
    float DesiredX,
    float DesiredY,
    float MaxLateralSnap,
    FVector& OutPoint,
    int32 WorldSeed)
{
    switch (Archetype)
    {
    case ECaveGeneratorType::TunnelNetwork:
    case ECaveGeneratorType::Underwater:
        return VF_FindNearestHashRoomLandingPoint(
            CaveParams, Seed,
            WorldSeed == MIN_int32 ? Seed : WorldSeed,
            StrateTopZ, StrateBottomZ,
                                                  DesiredX, DesiredY, MaxLateralSnap, OutPoint);

    case ECaveGeneratorType::FlatPlain:
    case ECaveGeneratorType::CrystalChamber:
        return VF_SuggestSlabLandingPoint(SlabParams, Seed, StrateTopZ, StrateBottomZ,
                                           DesiredX, DesiredY, MaxLateralSnap, OutPoint);

    case ECaveGeneratorType::Maze:
        return VF_SuggestMazeLandingPoint(MazeParams, Seed, StrateTopZ, StrateBottomZ,
                                          DesiredX, DesiredY, MaxLateralSnap, OutPoint);

    case ECaveGeneratorType::VerticalShafts:
        return VF_SuggestVerticalShaftLandingPoint(VerticalShaftParams, Seed,
                                                   StrateTopZ, StrateBottomZ,
                                                   DesiredX, DesiredY, MaxLateralSnap, OutPoint);

    case ECaveGeneratorType::FloatingIslands:
        return VF_SuggestFloatingIslandLandingPoint(FloatingIslandParams, Seed,
                                                    StrateTopZ, StrateBottomZ,
                                                    DesiredX, DesiredY, MaxLateralSnap, OutPoint);

    case ECaveGeneratorType::SurfaceWorld:
        // The structural heightfield is analytically seed-based, but production SurfaceWorld
        // resolves biome-selected surface params and per-column overhang state through the
        // manager/cache path. This pure API has no biome context and must not query it.
        return false;

    default:
        return false;
    }
}

bool VoxelCaveMorphology::MayHaveFeatureInSearchBox(
    float SearchMinX, float SearchMinY,
    float SearchMaxX, float SearchMaxY,
    const FStrateGenerationParams& Params,
    uint32 Seed, int32 StrateIndex,
    bool bUseZ, float SearchMinZ, float SearchMaxZ,
    const TArray<FStrateTerrainOpEntry>* TerrainOps)
{
    if (!VoxelMath::IsFinite(SearchMinX) || !VoxelMath::IsFinite(SearchMinY)
        || !VoxelMath::IsFinite(SearchMaxX) || !VoxelMath::IsFinite(SearchMaxY)
        || SearchMinX > SearchMaxX || SearchMinY > SearchMaxY)
    {
        return true;
    }
    if (bUseZ
        && (!VoxelMath::IsFinite(SearchMinZ) || !VoxelMath::IsFinite(SearchMaxZ)
            || SearchMinZ > SearchMaxZ))
    {
        return true;
    }
    if (Params.RoomSpacing <= 0.0f)
    {
        // BuildChunkCache returns an empty cache before creating the origin
        // room when spacing is non-positive.
        return false;
    }

    FVFRoomGraphReach Reach;
    if (!VF_ComputeRoomGraphReach(Params, TerrainOps, Reach))
    {
        return true;
    }

    const float CollectMinX = SearchMinX - Reach.CollectMargin;
    const float CollectMinY = SearchMinY - Reach.CollectMargin;
    const float CollectMaxX = SearchMaxX + Reach.CollectMargin;
    const float CollectMaxY = SearchMaxY + Reach.CollectMargin;
    if (!VoxelMath::IsFinite(CollectMinX) || !VoxelMath::IsFinite(CollectMinY)
        || !VoxelMath::IsFinite(CollectMaxX) || !VoxelMath::IsFinite(CollectMaxY))
    {
        return true;
    }

    auto FloorCell = [](double Value, double CellSize, int32& OutCell) -> bool
    {
        const double Cell = FMath::FloorToDouble(Value / CellSize);
        if (!VoxelMath::IsFinite(Cell)
            || Cell < static_cast<double>(MIN_int32) + 2.0
            || Cell > static_cast<double>(MAX_int32) - 2.0)
        {
            return false;
        }
        OutCell = static_cast<int32>(Cell);
        return true;
    };
    int32 CellMinX = 0;
    int32 CellMaxX = 0;
    int32 CellMinY = 0;
    int32 CellMaxY = 0;
    if (!FloorCell(
            static_cast<double>(CollectMinX),
            static_cast<double>(Params.RoomSpacing), CellMinX)
        || !FloorCell(
            static_cast<double>(CollectMaxX),
            static_cast<double>(Params.RoomSpacing), CellMaxX)
        || !FloorCell(
            static_cast<double>(CollectMinY),
            static_cast<double>(Params.RoomSpacing), CellMinY)
        || !FloorCell(
            static_cast<double>(CollectMaxY),
            static_cast<double>(Params.RoomSpacing), CellMaxY))
    {
        return true;
    }

    const int64 SpanX = static_cast<int64>(CellMaxX)
        - static_cast<int64>(CellMinX) + 1;
    const int64 SpanY = static_cast<int64>(CellMaxY)
        - static_cast<int64>(CellMinY) + 1;
    // This is deliberately a bounded proof pass. If an authored asset asks
    // BuildChunkCache to hash a pathological number of cells, keep the old
    // conservative path instead of making classification unbounded.
    if (SpanX <= 0 || SpanY <= 0 || SpanX > 4096 || SpanY > 4096
        || SpanX > 262144 / FMath::Max<int64>(SpanY, 1))
    {
        return true;
    }

    const float RoomRadiusEnvelope = FMath::Max(
        FMath::Abs(Params.MinRoomRadius),
        FMath::Abs(Params.MaxRoomRadius));
    const float RoomZBuffer = RoomRadiusEnvelope * Params.RoomHeightRatio;
    const float StrateMinZ = Params.StrateBottomWorldZ
        + Params.BoundarySealThickness + RoomZBuffer;
    const float StrateMaxZ = Params.StrateTopWorldZ
        - Params.BoundarySealThickness - RoomZBuffer;
    const float StrateRangeZ = StrateMaxZ - StrateMinZ;
    const float StrateCenterZ = (StrateMinZ + StrateMaxZ) * 0.5f;
    if (!VoxelMath::IsFinite(StrateMinZ) || !VoxelMath::IsFinite(StrateMaxZ)
        || !VoxelMath::IsFinite(StrateRangeZ)
        || !VoxelMath::IsFinite(StrateCenterZ))
    {
        return true;
    }

    TArray<FBuildRoom, TInlineAllocator<128>> Rooms;
    if (Params.OriginRoomRadius > 0.0f)
    {
        FBuildRoom OriginRoom{};
        OriginRoom.Center = FVector(0.0f, 0.0f, StrateCenterZ);
        OriginRoom.RadiusXY = Params.OriginRoomRadius;
        OriginRoom.RadiusZ = Params.OriginRoomRadius
            * Params.RoomHeightRatio;
        OriginRoom.CellX = INT32_MAX;
        OriginRoom.CellY = INT32_MAX;
        OriginRoom.Hash = VoxelHash::Cell(
            0, 0, VoxelCaveMorphology::MakeStrateSeed(Seed, StrateIndex)
                ^ 0x0A161Cu);
        OriginRoom.bIsOrigin = true;
        OriginRoom.bStore = true;
        Rooms.Add(OriginRoom);
    }

    const uint32 StrateSeed = VoxelCaveMorphology::MakeStrateSeed(
        Seed, StrateIndex);
    if (Params.RoomDensity > 0.0f)
    {
        for (int32 CY = CellMinY; CY <= CellMaxY; ++CY)
        {
            for (int32 CX = CellMinX; CX <= CellMaxX; ++CX)
            {
                const uint32 CellHash = VoxelHash::Cell(
                    CX, CY, StrateSeed);
                if (VoxelHash::ToFloat01(CellHash) >= Params.RoomDensity)
                {
                    continue;
                }

                const float JitterX = VoxelHash::ToFloat01(
                    VoxelHash::Mix(CellHash ^ 0x12345678u));
                const float JitterY = VoxelHash::ToFloat01(
                    VoxelHash::Mix(CellHash ^ 0x9ABCDEF0u));
                const float JitterZ = VoxelHash::ToFloat01(
                    VoxelHash::Mix(CellHash ^ 0x55AA55AAu));
                FBuildRoom Room{};
                Room.CellX = CX;
                Room.CellY = CY;
                Room.Hash = CellHash;
                Room.Center.X = (static_cast<float>(CX) + 0.15f
                    + JitterX * 0.7f) * Params.RoomSpacing;
                Room.Center.Y = (static_cast<float>(CY) + 0.15f
                    + JitterY * 0.7f) * Params.RoomSpacing;
                Room.Center.Z = StrateMinZ + JitterZ
                    * FMath::Max(StrateRangeZ, 1.0f);
                const float SizeFactor = VoxelHash::ToFloat01(
                    VoxelHash::Mix(CellHash ^ 0xFEDCBA98u));
                Room.RadiusXY = FMath::Lerp(
                    Params.MinRoomRadius, Params.MaxRoomRadius, SizeFactor);
                Room.RadiusZ = Room.RadiusXY * Params.RoomHeightRatio;
                Room.bStore = true;
                Rooms.Add(Room);
            }
        }
    }

    return VF_MayHaveRoomGraphFeature(
        Rooms, SearchMinX, SearchMinY, SearchMaxX, SearchMaxY,
        Params, StrateSeed, Reach, bUseZ, SearchMinZ, SearchMaxZ);
}

bool VoxelCaveMorphology::IsRoomGraphWindowInvariant(
    const FStrateGenerationParams& Params,
    const TArray<FStrateTerrainOpEntry>* TerrainOps)
{
    FVFRoomGraphReach Reach;
    return VF_ComputeRoomGraphReach(Params, TerrainOps, Reach);
}

//=============================================================================
// PHASE 1: BUILD CHUNK CACHE
//=============================================================================
// Collects all rooms in the COLLECT region, computes a window-invariant
// nearest-neighbor backbone for connectivity, decides tunnel connections, and
// pre-computes all tunnel geometry (radii, floor-aligned wandering chains, bounding
// spheres). Only rooms/tunnels relevant to the chunk (STORE region) are kept.
//
// The result is stored in OutCache and reused for every voxel in the chunk.

void VoxelCaveMorphology::BuildChunkCache(
    FChunkSDFCache& OutCache,
    float SearchMinX, float SearchMinY,
    float SearchMaxX, float SearchMaxY,
    const FStrateGenerationParams& Params,
    uint32 Seed, int32 StrateIndex,
    const TArray<FStrateTerrainOpEntry>* TerrainOps,
    ERoomGraphBuildSite BuildSite)
{
    if (VoxelDensityProfile::AreCountersEnabled())
    {
        VoxelDensityProfile::AddCounter(VoxelDensityProfile::ECounter::SdfCacheBuild);
        const VoxelDensityProfile::ECounter SiteCounter = [&]()
            {
                switch (BuildSite)
                {
                case ERoomGraphBuildSite::GeneratorTile:
                    return VoxelDensityProfile::ECounter::RoomGraphBuildGeneratorTile;
                case ERoomGraphBuildSite::GeneratorTunnelCore:
                    return VoxelDensityProfile::ECounter::RoomGraphBuildGeneratorTunnelCore;
                case ERoomGraphBuildSite::OpShared:
                    return VoxelDensityProfile::ECounter::RoomGraphBuildOpShared;
                case ERoomGraphBuildSite::OpLocal:
                    return VoxelDensityProfile::ECounter::RoomGraphBuildOpLocal;
                case ERoomGraphBuildSite::ClassifierShared:
                    return VoxelDensityProfile::ECounter::RoomGraphBuildClassifierShared;
                case ERoomGraphBuildSite::ClassifierLocal:
                    return VoxelDensityProfile::ECounter::RoomGraphBuildClassifierLocal;
                default:
                    return VoxelDensityProfile::ECounter::RoomGraphBuildUnknown;
                }
            }();
        VoxelDensityProfile::AddCounter(SiteCounter);
    }
    VoxelDensityProfile::FScopedTimer ProfileTimer(
        VoxelDensityProfile::EBucket::RoomGraphBuild);
    // Clear previous data (arrays keep their allocation for reuse)
    OutCache.Reset();
    OutCache.SDFBlendRadius = Params.SDFBlendRadius;

    // Combine world seed with strate index so each strate gets unique caves
    const uint32 StrateSeed = VoxelCaveMorphology::MakeStrateSeed(Seed, StrateIndex);

    const float CellSize = Params.RoomSpacing;
    if (CellSize <= 0.0f) return;

    // This reach proof is shared with the feature-free preflight.  When it succeeds, the collect
    // margin includes the largest direct room reach and the complete pair/chain/tube envelope;
    // when it fails, keep the conservative legacy fallback and retain the malformed-input safety
    // behavior below.  The tile-window optimization is only a field-preserving optimization for
    // the finite parameter envelope accepted by VF_ComputeRoomGraphReach.
    FVFRoomGraphReach FeatureReach;
    const bool bHasFeatureReach = VF_ComputeRoomGraphReach(
        Params, TerrainOps, FeatureReach);

    //=========================================================================
    // INFLUENCE RADII
    //=========================================================================
    // MaxInfluence = how far a room body / tunnel TUBE reaches PERPENDICULAR to its
    // anchor — NOT its length. A room or tunnel whose anchor lies within MaxInfluence
    // of a box can touch a voxel inside that box.
    // Envelope conservatif / conservative bound: Lerp accepts inverted endpoints,
    // so max(Min, Max) covers either radius without changing the authored roll.
    const float RoomRadiusEnvelope = FMath::Max(
        FMath::Abs(Params.MinRoomRadius), FMath::Abs(Params.MaxRoomRadius));
    const float AllRoomRadiusEnvelope = FMath::Max(
        RoomRadiusEnvelope, FMath::Abs(Params.OriginRoomRadius));
    const float AllRoomHeightEnvelope = AllRoomRadiusEnvelope
        * FMath::Abs(Params.RoomHeightRatio);
    // Interior control points can carry the authored radius variation (+/-18%). Include that
    // reach in collection; the per-tunnel index below uses each baked radius exactly.
    const float TunnelRadiusEnvelope = FMath::Max(
        0.5f,
        FMath::Max(FMath::Abs(Params.TunnelMinRadius), FMath::Abs(Params.TunnelMaxRadius))
            * 1.18f);
    const float FloorReliefEnvelope = FMath::Abs(Params.FloorReliefStrength)
        * VOXEL_NOISE_SCALE * 1.5f;
    const float BlendEnvelope = FMath::Max(Params.SDFBlendRadius, 0.0f);
    const float DirectRoomReachFallback = VF_RoomShapeReachUpperBound(
        AllRoomRadiusEnvelope, AllRoomHeightEnvelope)
        + FloorReliefEnvelope + BlendEnvelope * 3.0f;
    const float MaxInfluenceFallback = FMath::Max(
        DirectRoomReachFallback,
        FMath::Abs(Params.TunnelWarpStrength)
            + TunnelRadiusEnvelope + BlendEnvelope * 3.0f);

    const float MaxTunnelLen = FMath::Max(Params.MaxTunnelLength, 0.0f);

    //=========================================================================
    // STORE decision — what we keep for the per-voxel loop.
    //=========================================================================
    // A room/tunnel is stored iff its OWN influence sphere can overlap the search box
    // (RoomReachesSearchBox / tunnel bounding spheres below). Per-voxel culling refines.

    //=========================================================================
    // COLLECT region — what we hash into existence for the connectivity decision.
    //=========================================================================
    // To DECIDE the graph identically in neighboring chunks, we must see, for every
    // room that could emit a tunnel touching this chunk, that room's ENTIRE
    // nearest-neighbor candidate set (all rooms within MaxTunnelLength of it):
    //   - A tunnel touching the box has both endpoint centers inside the pair envelope.
    //   - Each endpoint's NN candidates lie within MaxTunnelLength of that endpoint.
    //   => collect the pair envelope plus two graph hops.  This is independent of the search
    //      box size, so it remains valid for a whole tile as well as a one-chunk window.
    const float CollectMargin = bHasFeatureReach
        ? FeatureReach.CollectMargin
        : 2.0f * MaxTunnelLen + MaxInfluenceFallback;
    const float CollectMinX = SearchMinX - CollectMargin;
    const float CollectMinY = SearchMinY - CollectMargin;
    const float CollectMaxX = SearchMaxX + CollectMargin;
    const float CollectMaxY = SearchMaxY + CollectMargin;

    // Convert COLLECT bounds to cell range
    const int32 CellMinX = FMath::FloorToInt(CollectMinX / CellSize);
    const int32 CellMaxX = FMath::FloorToInt(CollectMaxX / CellSize);
    const int32 CellMinY = FMath::FloorToInt(CollectMinY / CellSize);
    const int32 CellMaxY = FMath::FloorToInt(CollectMaxY / CellSize);

    //=========================================================================
    // Vertical range for room CENTER placement.
    // PLACEMENT CONTRACT: this formula MUST remain identical to
    // VF_FindNearestHashRoomLandingPoint above. The pure landing query deliberately does not call this
    // per-chunk cache builder during Initialize, so the fixed
    // VoxelForge.Determinism.PassageLandsInOpenSpace ring test is the guard that catches drift
    // between the two sites.
    //=========================================================================
    // Buffer = seal thickness + max room half-height.
    // This guarantees the tallest possible room (RoomRadiusEnvelope * RoomHeightRatio)
    // fits entirely within the seal boundary — no room gets its ceiling or floor
    // cut flat by the seal. Smaller rooms have proportionally more margin.
    const float RoomZBuffer = RoomRadiusEnvelope * Params.RoomHeightRatio;
    const float StrateMinZ  = Params.StrateBottomWorldZ + Params.BoundarySealThickness + RoomZBuffer;
    const float StrateMaxZ  = Params.StrateTopWorldZ   - Params.BoundarySealThickness - RoomZBuffer;
    const float StrateRangeZ = StrateMaxZ - StrateMinZ;
    const float StrateCenterZ = (StrateMinZ + StrateMaxZ) * 0.5f;

    const float BlendK = Params.SDFBlendRadius;

    // "This room can reach the search box" test, using the room's OWN reach. The old test
    // compared the center against a box inflated
    // by the SHARED MaxInfluence, which silently assumed every room's reach <= MaxInfluence.
    // That's FALSE for the origin room (OriginRoomRadius >> MaxRoomRadius) — chunks inside
    // the big room but > MaxInfluence from (0,0) didn't store it, so its carve clipped at an
    // arbitrary chunk-aligned radius — and slightly false even for hash rooms (1.5x stretch).
    auto RoomReachesSearchBox = [&](const FVector& C, float RadiusXY, float RadiusZ) -> bool
    {
        const float ShapeReach = VF_RoomShapeReachUpperBound(RadiusXY, RadiusZ);
        const float Reach = FMath::Max(
            ShapeReach + FloorReliefEnvelope + BlendEnvelope * 3.0f,
            bHasFeatureReach ? FeatureReach.TerrainOpReach : 0.0f);
        const float dx = FMath::Max3((float)(SearchMinX - C.X), 0.0f, (float)(C.X - SearchMaxX));
        const float dy = FMath::Max3((float)(SearchMinY - C.Y), 0.0f, (float)(C.Y - SearchMaxY));
        return (dx * dx + dy * dy) <= Reach * Reach;
    };

    // Sphere-vs-search-box test in XY (treated as infinite in Z; per-voxel culling
    // resolves Z). Used to decide whether a tunnel is worth storing for this chunk.
    auto SphereTouchesSearchXY = [&](const FVector& C, float RSq) -> bool
    {
        const float dx = FMath::Max3((float)(SearchMinX - C.X), 0.0f, (float)(C.X - SearchMaxX));
        const float dy = FMath::Max3((float)(SearchMinY - C.Y), 0.0f, (float)(C.Y - SearchMaxY));
        return (dx * dx + dy * dy) <= RSq;
    };

    // A connected edge can be discarded before the expensive player-fit bake when even a
    // conservative AABB around its entire possible chain misses the search window. The endpoint
    // fit stays inside 75% of its room radius; each cave-warped endpoint/control point can move by
    // at most two proven warp envelopes (fit inversion plus the final map); the interior wander is
    // bounded by TunnelWarpStrength. This is only a reject gate: non-finite inputs retain the old
    // full build, and a touching envelope still takes the exact path below.
    const float CaveWarpBound = FMath::Abs(Params.CaveWarpStrength)
        * VOXEL_NOISE_SCALE * 1.5f;
    auto PossibleTunnelTouchesSearchXY = [&](const FBuildRoom& RoomA,
                                             const FBuildRoom& RoomB) -> bool
    {
        // If the finite reach proof declined the authored parameter envelope, do not use a
        // hand-written approximation here. Building the edge is the safe fallback and preserves
        // the malformed-input behavior of the old path.
        if (!bHasFeatureReach)
        {
            return true;
        }
        const float RadiusA = FMath::Abs(RoomA.RadiusXY);
        const float RadiusB = FMath::Abs(RoomB.RadiusXY);
        const float RoomEndpointShift = FMath::Max(
            2.0f, 0.75f * FMath::Max(RadiusA, RadiusB)) + CaveWarpBound;
        const float TunnelRadius = FMath::Max(
            0.5f,
            FMath::Max(FMath::Abs(Params.TunnelMinRadius),
                       FMath::Abs(Params.TunnelMaxRadius)) * 1.18f);
        const float ChainReach = FMath::Max(
            FeatureReach.PairAabbReach,
            RoomEndpointShift + TunnelRadius + FMath::Max(
                FMath::Max(Params.SDFBlendRadius, 0.0f) * 3.0f,
                VoxelPassageGeometry::LandingFloorThicknessVoxels));
        if (!VoxelMath::IsFinite(CaveWarpBound)
            || !VoxelMath::IsFinite(ChainReach)
            || ChainReach < 0.0f
            || !VoxelMath::IsFinite((float)RoomA.Center.X)
            || !VoxelMath::IsFinite((float)RoomA.Center.Y)
            || !VoxelMath::IsFinite((float)RoomB.Center.X)
            || !VoxelMath::IsFinite((float)RoomB.Center.Y))
        {
            return true;
        }

        const float MinX = FMath::Min(
            static_cast<float>(RoomA.Center.X), static_cast<float>(RoomB.Center.X))
            - ChainReach;
        const float MinY = FMath::Min(
            static_cast<float>(RoomA.Center.Y), static_cast<float>(RoomB.Center.Y))
            - ChainReach;
        const float MaxX = FMath::Max(
            static_cast<float>(RoomA.Center.X), static_cast<float>(RoomB.Center.X))
            + ChainReach;
        const float MaxY = FMath::Max(
            static_cast<float>(RoomA.Center.Y), static_cast<float>(RoomB.Center.Y))
            + ChainReach;
        return VoxelMath::IsFinite(MinX) && VoxelMath::IsFinite(MinY)
            && VoxelMath::IsFinite(MaxX) && VoxelMath::IsFinite(MaxY)
            && MaxX >= SearchMinX && MaxY >= SearchMinY
            && MinX <= SearchMaxX && MinY <= SearchMaxY;
    };

    // Temporary array with cell coordinates for tunnel connection decisions
    TArray<FBuildRoom, TInlineAllocator<64>> BuildRooms;

    // --- ORIGIN ROOM ---
    // Guaranteed large origin landing at (0, 0) in each strate — the future shaft landing and
    // graph root room. Inter-strate passage mouths are placed elsewhere and have no radial road.
    // Collected whenever (0,0) is inside the COLLECT region so it participates in the
    // connectivity decision; only stored if it can reach this chunk.
    int32 OriginIdx = -1;
    if (Params.OriginRoomRadius > 0.0f)
    {
        // For a valid reach proof, the origin's own body is necessarily inside the collect region
        // whenever it can reach the search box.  Keep the old own-body fallback only for malformed
        // or unsupported authored parameters, where the proof deliberately declines to claim
        // window invariance.
        const bool bOriginInCollect =
            0.0f >= CollectMinX && 0.0f <= CollectMaxX &&
            0.0f >= CollectMinY && 0.0f <= CollectMaxY;
        const float OriginRZ = Params.OriginRoomRadius * Params.RoomHeightRatio;
        if (bOriginInCollect
            || (!bHasFeatureReach
                && RoomReachesSearchBox(
                    FVector(0.0f, 0.0f, StrateCenterZ),
                    Params.OriginRoomRadius, OriginRZ)))
        {
            FBuildRoom OriginRoom;
            OriginRoom.CellX = INT32_MAX;  // Sentinel — never matches a real grid cell
            OriginRoom.CellY = INT32_MAX;
            OriginRoom.Hash = VoxelHash::Cell(0, 0, StrateSeed ^ 0x0A161Cu);
            OriginRoom.Center = FVector(0.0f, 0.0f, StrateCenterZ);
            OriginRoom.RadiusXY = Params.OriginRoomRadius;
            OriginRoom.RadiusZ = Params.OriginRoomRadius * Params.RoomHeightRatio;
            OriginRoom.bIsOrigin = true;
            OriginRoom.bStore = RoomReachesSearchBox(OriginRoom.Center, OriginRoom.RadiusXY, OriginRoom.RadiusZ);
            OriginIdx = BuildRooms.Add(OriginRoom);
        }
    }

    // --- HASH-BASED ROOMS ---
    for (int32 CY = CellMinY; CY <= CellMaxY; CY++)
    {
        for (int32 CX = CellMinX; CX <= CellMaxX; CX++)
        {
            // Hash this cell to decide if it has a room
            const uint32 CellHash = VoxelHash::Cell(CX, CY, StrateSeed);
            const float RoomChance = VoxelHash::ToFloat01(CellHash);

            // Skip empty cells (no room here)
            if (RoomChance >= Params.RoomDensity) continue;

            // Room position: jittered within the cell
            const float JitterX = VoxelHash::ToFloat01(VoxelHash::Mix(CellHash ^ 0x12345678u));
            const float JitterY = VoxelHash::ToFloat01(VoxelHash::Mix(CellHash ^ 0x9ABCDEF0u));
            const float JitterZ = VoxelHash::ToFloat01(VoxelHash::Mix(CellHash ^ 0x55AA55AAu));

            FBuildRoom Room;
            Room.CellX = CX;
            Room.CellY = CY;
            Room.Hash = CellHash;
            Room.Center.X = (CX + 0.15f + JitterX * 0.7f) * CellSize;
            Room.Center.Y = (CY + 0.15f + JitterY * 0.7f) * CellSize;
            Room.Center.Z = StrateMinZ + JitterZ * FMath::Max(StrateRangeZ, 1.0f);

            // Room size: lerp between min and max
            const float SizeFactor = VoxelHash::ToFloat01(VoxelHash::Mix(CellHash ^ 0xFEDCBA98u));
            Room.RadiusXY = FMath::Lerp(Params.MinRoomRadius, Params.MaxRoomRadius, SizeFactor);
            Room.RadiusZ = Room.RadiusXY * Params.RoomHeightRatio;
            Room.bIsOrigin = false;
            Room.bStore = RoomReachesSearchBox(Room.Center, Room.RadiusXY, Room.RadiusZ);

            BuildRooms.Add(Room);
        }
    }

    // The collect window is intentionally much wider than the stored window:
    // it is needed to make the nearest-neighbor graph seam-invariant. Prove
    // the genuinely feature-free case before the O(N^2) graph and player-fit
    // bake. The helper repeats only the deterministic graph decisions that can
    // affect whether a room/tunnel bound reaches this search window.
    if (bHasFeatureReach
        && !VF_MayHaveRoomGraphFeature(
            BuildRooms, SearchMinX, SearchMinY, SearchMaxX, SearchMaxY,
            Params, StrateSeed, FeatureReach))
    {
        VF_FinalizeCacheBroadPhaseBounds(OutCache, BlendK);
        return;
    }

    const int32 NumRooms = BuildRooms.Num();
    if (NumRooms == 0)
    {
        VF_FinalizeCacheBroadPhaseBounds(OutCache, BlendK);
        return;
    }

    // Keep the room floor calculation in one place so tunnel mouths and overlap joins use the
    // same deterministic support plane as the emitted room. It includes the room-centre relief;
    // the evaluator still applies the spatial relief field per voxel around that anchor.
    const auto RoomFloorZFor = [&Params, Seed](const FBuildRoom& Room) -> float
    {
        return VF_RoomFloorZFromParts(
            Room.Center, Room.RadiusXY, Room.RadiusZ, Room.Hash, Seed, Params);
    };

    //=========================================================================
    // Window-invariant guaranteed backbone
    //=========================================================================
    // Each room gets ONE guaranteed link, chosen among candidates within MaxTunnelLength
    // (that reach filter is what keeps the decision identical across chunk windows).
    //
    // bTunnelsFlowTowardOrigin = true (default): the link target is the best candidate
    // among rooms STRICTLY CLOSER to (0,0) in XY. Every chain of links then descends in
    // origin-distance and terminates at the origin room → the network is a TREE ROOTED AT
    // THE SPINE HUB: every room is reachable, tunnels flow inward like tributaries.
    // (Frontier rooms with no closer candidate in reach fall back to plain NN — a far
    // cluster stays internally chained even when it can't bridge to the origin side.)
    //
    // bTunnelsFlowTowardOrigin = false (legacy): plain nearest-neighbor pairing. NOTE: an
    // NN-graph is a FOREST of small clusters, not a connected tree — isolated cave pockets
    // are expected in this mode.
    //
    // Selection metric (not the reach filter) penalizes vertical separation via
    // TunnelHorizontalBias, so the GUARANTEED links also prefer walkable slopes —
    // biasing only the random TunnelDensity extras lets backbone tunnels come out
    // absurdly steep.
    TArray<int32, TInlineAllocator<64>> NearestNeighbor;
    NearestNeighbor.SetNumUninitialized(NumRooms);

    const float MaxTunnelLenSq = MaxTunnelLen * MaxTunnelLen;
    const bool bFlowToOrigin = Params.bTunnelsFlowTowardOrigin;

    auto LinkMetric = [&](int32 I, int32 J) -> float
    {
        const float D = FVector::Dist(BuildRooms[I].Center, BuildRooms[J].Center);
        const float VertSep = FMath::Abs(BuildRooms[I].Center.Z - BuildRooms[J].Center.Z);
        return D + VertSep * Params.TunnelHorizontalBias * 5.0f;
    };
    // Squared XY distance to the (0,0) spine — the "inward" ordering. Purely positional,
    // so it is window-invariant by construction.
    auto OriginKeySq = [&](int32 I) -> float
    {
        const FVector& C = BuildRooms[I].Center;
        return C.X * C.X + C.Y * C.Y;
    };

    // ExcludeJ: used by the origin-cap redirect below (re-pick ignoring the origin room).
    auto PickNeighbor = [&](int32 I, int32 ExcludeJ) -> int32
    {
        const float MyKeySq = OriginKeySq(I);
        float BestInward = FLT_MAX;  int32 BestInwardJ = -1;
        float BestAny    = FLT_MAX;  int32 BestAnyJ    = -1;
        for (int32 J = 0; J < NumRooms; J++)
        {
            if (J == I || J == ExcludeJ) continue;
            const float DSq = FVector::DistSquared(BuildRooms[I].Center, BuildRooms[J].Center);
            if (DSq > MaxTunnelLenSq) continue;       // out of reach — never a tunnel
            const float M = LinkMetric(I, J);
            if (M < BestAny) { BestAny = M; BestAnyJ = J; }
            if (bFlowToOrigin && OriginKeySq(J) < MyKeySq && M < BestInward)
            {
                BestInward = M; BestInwardJ = J;
            }
        }
        return (bFlowToOrigin && BestInwardJ != -1) ? BestInwardJ : BestAnyJ;
    };

    for (int32 I = 0; I < NumRooms; I++)
    {
        NearestNeighbor[I] = PickNeighbor(I, /*ExcludeJ=*/INDEX_NONE);
    }

    //=========================================================================
    // ORIGIN CONNECTION CAP (deterministic, order-independent)
    //=========================================================================
    // OriginRoomMaxConnections caps how many rooms backbone-force into the origin.
    // The OLD approach counted connections in pair-loop order, which depended on the
    // per-chunk room ordering → non-deterministic across chunks. Instead we gather
    // ALL rooms backbone-linked to origin (window-invariant given the COLLECT region),
    // rank them by a deterministic pair hash, and keep only the top N as forced.
    // The rest are DOWNGRADED to the random TunnelDensity path (connectivity not
    // broken, only the "guaranteed" aspect is limited).
    TSet<int32> OriginDowngraded;
    const int32 MaxOriginConn = Params.OriginRoomMaxConnections;
    if (OriginIdx >= 0 && MaxOriginConn > 0)
    {
        // Collect origin backbone candidates with a deterministic ranking key.
        TArray<TPair<uint32, int32>, TInlineAllocator<32>> OriginLinks;
        for (int32 I = 0; I < NumRooms; I++)
        {
            if (I == OriginIdx) continue;
            const bool bLinked = (NearestNeighbor[I] == OriginIdx) || (NearestNeighbor[OriginIdx] == I);
            if (!bLinked) continue;
            const uint32 Key = VoxelHash::Pair(
                BuildRooms[OriginIdx].CellX, BuildRooms[OriginIdx].CellY,
                BuildRooms[I].CellX, BuildRooms[I].CellY,
                StrateSeed ^ 0x031A1Eu);
            OriginLinks.Add(TPair<uint32, int32>(Key, I));
        }
        // Stable deterministic order by (hash, then index for tie-break).
        OriginLinks.Sort([](const TPair<uint32, int32>& A, const TPair<uint32, int32>& B)
        {
            return A.Key != B.Key ? A.Key < B.Key : A.Value < B.Value;
        });
        for (int32 R = MaxOriginConn; R < OriginLinks.Num(); ++R)
        {
            OriginDowngraded.Add(OriginLinks[R].Value);
        }

        // REDIRECT, don't strand: a downgraded room whose guaranteed link pointed at the
        // origin re-picks its best target EXCLUDING origin. It keeps a guaranteed link
        // (chains to the hub through another room instead of directly), which matters
        // doubly now that rooms with zero connections are culled below. Deterministic:
        // same candidate set, same metric, one exclusion.
        for (int32 DowngradedI : OriginDowngraded)
        {
            if (NearestNeighbor[DowngradedI] == OriginIdx)
            {
                NearestNeighbor[DowngradedI] = PickNeighbor(DowngradedI, /*ExcludeJ=*/OriginIdx);
            }
        }
    }

    //=========================================================================
    // Resolve tunnel connections, pre-compute geometry, store chunk-relevant ones
    //=========================================================================
    // A tunnel exists if EITHER:
    //   1. One is the other's nearest neighbor (backbone — guarantees connectivity),
    //      and (for origin links) it survived the origin cap, OR
    //   2. The pair hash passes TunnelDensity (random extra loops).
    // Both must pass the distance check (MaxTunnelLength). Only tunnels whose bounding
    // sphere reaches the search box are stored for the per-voxel loop.
    // Tracks whether each room ends up with at least one tunnel — DECIDED connections,
    // independent of whether the tunnel itself is stored for this chunk (a room near the
    // window edge may have all its tunnels outside the box; it's still "connected").
    // Stored rooms with zero connections are culled at emission: they'd be sealed air
    // pockets no tunnel ever reaches. Window-invariant: a stored room's full candidate
    // set (and each candidate's own candidates) lies inside the COLLECT region.
    TArray<bool, TInlineAllocator<64>> RoomConnected;
    RoomConnected.Init(false, NumRooms);
    TArray<FBuildRoomMouthRise, TInlineAllocator<64>> RoomMouthRises;

    struct FResolvedGraphEdge
    {
        int32 RoomA = INDEX_NONE;
        int32 RoomB = INDEX_NONE;
        bool bBackbone = false;
        bool bRedundant = false;
        bool bLeaf = false;
        bool bWalkForestEdge = false;
    };

    // Resolve the complete graph before emitting any geometry.  Ledge eligibility is a graph
    // property, not a chunk-window property: an edge may drop only when it is incident to a leaf or
    // when removing it leaves another route between its endpoints.  All other edges must wind.
    // The collect region contains the complete candidate neighbourhood for every stored edge, so
    // this bridge test is also window-invariant.
    TArray<FResolvedGraphEdge, TInlineAllocator<128>> GraphEdges;
    TArray<int32, TInlineAllocator<64>> GraphDegrees;
    GraphDegrees.Init(0, NumRooms);
    for (int32 I = 0; I < NumRooms; ++I)
    {
        for (int32 J = I + 1; J < NumRooms; ++J)
        {
            const FBuildRoom& RoomA = BuildRooms[I];
            const FBuildRoom& RoomB = BuildRooms[J];
            bool bBackbone = (NearestNeighbor[I] == J) || (NearestNeighbor[J] == I);

            if (bBackbone && (RoomA.bIsOrigin || RoomB.bIsOrigin))
            {
                const int32 Other = RoomA.bIsOrigin ? J : I;
                if (OriginDowngraded.Contains(Other))
                {
                    bBackbone = false;
                }
            }

            const float EuclidDist = FVector::Dist(RoomA.Center, RoomB.Center);
            float CheckDist = EuclidDist;
            if (!bBackbone && Params.TunnelHorizontalBias > 0.0f)
            {
                const float VertSep = FMath::Abs(RoomA.Center.Z - RoomB.Center.Z);
                CheckDist += VertSep * Params.TunnelHorizontalBias * 5.0f;
            }
            if (CheckDist > Params.MaxTunnelLength)
            {
                continue;
            }

            if (!bBackbone)
            {
                const uint32 PairHash = VoxelHash::Pair(
                    RoomA.CellX, RoomA.CellY,
                    RoomB.CellX, RoomB.CellY,
                    StrateSeed);
                if (VoxelHash::ToFloat01(PairHash) >= Params.TunnelDensity)
                {
                    continue;
                }
            }

            FResolvedGraphEdge& Edge = GraphEdges.Emplace_GetRef();
            Edge.RoomA = I;
            Edge.RoomB = J;
            Edge.bBackbone = bBackbone;
            RoomConnected[I] = true;
            RoomConnected[J] = true;
            ++GraphDegrees[I];
            ++GraphDegrees[J];
        }
    }

    TArray<TArray<int32>, TInlineAllocator<64>> GraphAdjacency;
    GraphAdjacency.SetNum(NumRooms);
    for (int32 EdgeIndex = 0; EdgeIndex < GraphEdges.Num(); ++EdgeIndex)
    {
        const FResolvedGraphEdge& Edge = GraphEdges[EdgeIndex];
        GraphAdjacency[Edge.RoomA].Add(EdgeIndex);
        GraphAdjacency[Edge.RoomB].Add(EdgeIndex);
    }
    TArray<int32, TInlineAllocator<64>> GraphDiscovery;
    TArray<int32, TInlineAllocator<64>> GraphLowLink;
    TArray<uint8, TInlineAllocator<128>> GraphBridges;
    GraphDiscovery.Init(INDEX_NONE, NumRooms);
    GraphLowLink.Init(INDEX_NONE, NumRooms);
    GraphBridges.Init(0u, GraphEdges.Num());
    int32 GraphDiscoveryTime = 0;
    TFunction<void(int32, int32)> VisitGraph = [&](int32 Node, int32 ParentEdge)
    {
        GraphDiscovery[Node] = GraphDiscoveryTime;
        GraphLowLink[Node] = GraphDiscoveryTime;
        ++GraphDiscoveryTime;
        for (const int32 EdgeIndex : GraphAdjacency[Node])
        {
            if (EdgeIndex == ParentEdge)
            {
                continue;
            }
            const FResolvedGraphEdge& Edge = GraphEdges[EdgeIndex];
            const int32 Other = Edge.RoomA == Node ? Edge.RoomB : Edge.RoomA;
            if (GraphDiscovery[Other] == INDEX_NONE)
            {
                VisitGraph(Other, EdgeIndex);
                GraphLowLink[Node] = FMath::Min(
                    GraphLowLink[Node], GraphLowLink[Other]);
                if (GraphLowLink[Other] > GraphDiscovery[Node])
                {
                    GraphBridges[EdgeIndex] = 1u;
                }
            }
            else
            {
                GraphLowLink[Node] = FMath::Min(
                    GraphLowLink[Node], GraphDiscovery[Other]);
            }
        }
    };
    for (int32 Node = 0; Node < NumRooms; ++Node)
    {
        if (GraphDiscovery[Node] == INDEX_NONE)
        {
            VisitGraph(Node, INDEX_NONE);
        }
    }
    for (int32 EdgeIndex = 0; EdgeIndex < GraphEdges.Num(); ++EdgeIndex)
    {
        FResolvedGraphEdge& Edge = GraphEdges[EdgeIndex];
        Edge.bRedundant = GraphBridges[EdgeIndex] == 0u;
        Edge.bLeaf = GraphDegrees[Edge.RoomA] <= 1
            || GraphDegrees[Edge.RoomB] <= 1;
    }

    // Independent bridge tests are not enough for a batch of drops: two non-bridge edges from the
    // same cycle can become a cut when selected together. Reserve a deterministic spanning forest
    // before the hash choice so every non-leaf room remains connected by walkable edges even when
    // several redundant edges receive ledges. Leaf edges are still allowed to drop by policy.
    TArray<uint8, TInlineAllocator<128>> GraphForestVisited;
    TArray<uint8, TInlineAllocator<128>> GraphForestEdges;
    GraphForestVisited.Init(0u, NumRooms);
    GraphForestEdges.Init(0u, GraphEdges.Num());
    TFunction<void(int32)> VisitWalkForest = [&](int32 Node)
    {
        GraphForestVisited[Node] = 1u;
        for (const int32 EdgeIndex : GraphAdjacency[Node])
        {
            const FResolvedGraphEdge& Edge = GraphEdges[EdgeIndex];
            const int32 Other = Edge.RoomA == Node ? Edge.RoomB : Edge.RoomA;
            if (GraphForestVisited[Other] != 0u)
            {
                continue;
            }
            GraphForestEdges[EdgeIndex] = 1u;
            VisitWalkForest(Other);
        }
    };
    for (int32 Node = 0; Node < NumRooms; ++Node)
    {
        if (GraphForestVisited[Node] == 0u)
        {
            VisitWalkForest(Node);
        }
    }
    for (int32 EdgeIndex = 0; EdgeIndex < GraphEdges.Num(); ++EdgeIndex)
    {
        GraphEdges[EdgeIndex].bWalkForestEdge = GraphForestEdges[EdgeIndex] != 0u;
    }

    const auto ResolvePlayerFitPoint = [](FBuildRoom& Room, const FStrateGenerationParams& InParams,
                                          uint32 InWorldSeed) -> bool
    {
        if (Room.bIsOrigin)
        {
            return false;
        }
        if (Room.bPlayerFitAttempted)
        {
            return Room.bHasPlayerFitPoint;
        }
        Room.bPlayerFitAttempted = true;
        const FVFRoomLandingSite Site{
            Room.Center,
            Room.RadiusXY,
            Room.RadiusZ,
            Room.Hash,
            false};
        Room.bHasPlayerFitPoint = VF_FindPlayerFitPointForRoomMemoized(
            InParams,
            Site,
            InParams.StrateTopWorldZ,
            InParams.StrateBottomWorldZ,
            InWorldSeed,
            Room.PlayerFitPoint);
        return Room.bHasPlayerFitPoint;
    };

#if !UE_BUILD_SHIPPING
    const bool bTrimRoomMouth = CVarVoxelForgeTunnelMouthTrim.GetValueOnAnyThread() != 0;
#else
    constexpr bool bTrimRoomMouth = true;
#endif
    const auto BuildRoomMouth = [
        &BuildRooms, &ResolvePlayerFitPoint, &RoomFloorZFor,
        &Params, Seed, BlendK, bTrimRoomMouth](int32 RoomIndex, const FVector& Towards,
                                               float TunnelRadius) -> FVector
    {
        FBuildRoom& Room = BuildRooms[RoomIndex];
        FVector Anchor = Room.Center;
        if (!Room.bIsOrigin
            && ResolvePlayerFitPoint(Room, Params, Seed))
        {
            Anchor = Room.PlayerFitPoint;
        }

        if (!bTrimRoomMouth)
        {
            // Keep the proven legacy endpoint as the safe default while the room-wall trim
            // remains an explicit investigation switch. The endpoint still uses the room's
            // deterministic floor and player-fit anchor, so the raw tunnel floor remains the
            // authored tunnel surface rather than an added slab.
            const float RoomFloorZ = RoomFloorZFor(Room);
            const float SafeFloorZ = VoxelMath::IsFinite(RoomFloorZ)
                ? RoomFloorZ : Room.Center.Z - Room.RadiusZ;
            FVector End = Room.Center + FVector(
                0.0f, 0.0f, SafeFloorZ + FMath::Abs(TunnelRadius) - Room.Center.Z);
            FVector WorldEnd = VF_UnwarpCavePoint(End, Params, Seed);
            if (!Room.bIsOrigin && Room.bHasPlayerFitPoint)
            {
                WorldEnd = FVector(
                    Room.PlayerFitPoint.X,
                    Room.PlayerFitPoint.Y,
                    Room.PlayerFitPoint.Z - 1.0f + FMath::Abs(TunnelRadius));
            }
            return WorldEnd;
        }

        const FVFRoomLandingSite Site{
            Room.Center,
            Room.RadiusXY,
            Room.RadiusZ,
            Room.Hash,
            Room.bIsOrigin};
        const FVFRoomMouthMemoKey MemoKey = VF_MakeRoomMouthMemoKey(
            Params, Site, Anchor, Towards, TunnelRadius, BlendK, Seed);
        FVector CachedWorldEnd = FVector::ZeroVector;
        GRoomMouthMemo.FindOrCompute(
            MemoKey,
            CachedWorldEnd,
            [&](FVector& OutWorldEnd) -> bool
            {
                FVector MouthXY = Anchor;
                float BoundaryDistance = 0.0f;
                if (!VF_FindRoomMouthBoundary(
                        Site, Params, Seed,
                        Params.StrateTopWorldZ, Params.StrateBottomWorldZ,
                        Anchor, Towards, TunnelRadius, BlendK,
                        MouthXY, BoundaryDistance))
                {
                    FVector FallbackDirection(Towards.X, Towards.Y, 0.0f);
                    if (!FallbackDirection.Normalize())
                    {
                        FallbackDirection = FVector::ForwardVector;
                    }
                    const float MouthInset = VF_TunnelMouthInset(TunnelRadius, BlendK);
                    MouthXY = Anchor + FallbackDirection * FMath::Max(
                        0.0f, FMath::Abs(Room.RadiusXY) - MouthInset);
                }
                (void)BoundaryDistance;

                float MouthFloorZ = VF_RoomFloorZFromParts(
                    FVector(MouthXY.X, MouthXY.Y, Room.Center.Z),
                    Room.RadiusXY, Room.RadiusZ, Room.Hash, Seed, Params);
                if (!VoxelMath::IsFinite(MouthFloorZ))
                {
                    MouthFloorZ = RoomFloorZFor(Room);
                }
                if (!VoxelMath::IsFinite(MouthFloorZ))
                {
                    MouthFloorZ = Room.Center.Z - Room.RadiusZ;
                }
                float WorldFloorZ = -FLT_MAX;
                const bool bFoundWorldFloor = VF_FindRoomFloorCrossingWorld(
                    Site, Params, Seed,
                    Params.StrateTopWorldZ, Params.StrateBottomWorldZ,
                    MouthXY, WorldFloorZ);
                if (bFoundWorldFloor)
                {
                    // The boundary search returns world XY. Put the endpoint directly on the
                    // room's world-space floor crossing; the later VF_ApplyCaveWarp call creates
                    // the matching SDF-chain endpoint. This makes the room/tunnel floor join
                    // exact by construction.
                    OutWorldEnd = FVector(
                        MouthXY.X, MouthXY.Y,
                        WorldFloorZ + FMath::Abs(TunnelRadius));
                }
                else
                {
                    // Malformed/unsupported room data keeps the bounded fallback. The outer
                    // construction path applies VF_ApplyCaveWarp when it creates the SDF-chain
                    // endpoint, so invert that warp here just as the legacy endpoint path does.
                    OutWorldEnd = VF_UnwarpCavePoint(
                        FVector(
                            MouthXY.X, MouthXY.Y,
                            MouthFloorZ + FMath::Abs(TunnelRadius)),
                        Params, Seed);
                }
                return VoxelMath::IsFinite(OutWorldEnd.X)
                    && VoxelMath::IsFinite(OutWorldEnd.Y)
                    && VoxelMath::IsFinite(OutWorldEnd.Z);
            });
        return CachedWorldEnd;
    };

    for (const FResolvedGraphEdge& GraphEdge : GraphEdges)
    {
            const int32 I = GraphEdge.RoomA;
            const int32 J = GraphEdge.RoomB;
            FBuildRoom& RoomA = BuildRooms[I];
            FBuildRoom& RoomB = BuildRooms[J];

            if (!PossibleTunnelTouchesSearchXY(RoomA, RoomB))
            {
                // The graph decision is retained above; only the search-window geometry is
                // omitted. This preserves connectivity, joins, and all neighbouring-window
                // decisions while avoiding player-fit work for edges that cannot be stored.
                continue;
            }

            // --- TUNNEL HASH (for deriving all tunnel properties) ---
            const uint32 TunnelHash = VoxelHash::Pair(
                RoomA.CellX, RoomA.CellY,
                RoomB.CellX, RoomB.CellY,
                StrateSeed ^ 0xDECAF001u
            );

            // --- RADIUS ---
            const float FactorA = VoxelHash::ToFloat01(VoxelHash::Mix(TunnelHash ^ 0xBAADF00Du));
            const float FactorB = VoxelHash::ToFloat01(VoxelHash::Mix(TunnelHash ^ 0x8BADF00Du));
            const float RadA = FMath::Lerp(Params.TunnelMinRadius, Params.TunnelMaxRadius, FactorA);
            const float RadB = FMath::Lerp(Params.TunnelMinRadius, Params.TunnelMaxRadius, FactorB);

            // --- ENDPOINT ROOM-WALL TRIM + FLOOR ALIGNMENT ---
            // Resolve each endpoint against the actual hashed room boundary along the edge. The
            // old endpoint was the room centre/player-fit point, so its capsule necessarily ran
            // through the chamber. The trim leaves half a tunnel radius plus the SDF blend inside
            // the wall, which preserves a walkable overlap without projecting a floor slab into
            // the room.
            FVector TunnelDirection(
                RoomB.Center.X - RoomA.Center.X,
                RoomB.Center.Y - RoomA.Center.Y,
                0.0f);
            if (!TunnelDirection.Normalize())
            {
                TunnelDirection = FVector::ForwardVector;
            }
            const FVector WorldEndA = BuildRoomMouth(I, TunnelDirection, RadA);
            const FVector WorldEndB = BuildRoomMouth(J, -TunnelDirection, RadB);
            const FVector EndA = VF_ApplyCaveWarp(WorldEndA, Params, Seed);
            const FVector EndB = VF_ApplyCaveWarp(WorldEndB, Params, Seed);

            // --- BUILD CACHED TUNNEL ---
            FCachedTunnel CT;
            CT.EndpointA = EndA;
            CT.EndpointB = EndB;
            CT.RadiusA = RadA;
            CT.RadiusB = RadB;
            CT.FloorReliefStrength = Params.FloorReliefStrength;
            CT.FloorReliefFrequency = Params.FloorReliefFrequency;
            CT.FloorSeed = VoxelHash::Mix(TunnelHash ^ 0xF100F1u);
            CT.bHasFloorRoomOwnership = true;
            CT.FloorRoomHashA = RoomA.Hash;
            CT.FloorRoomHashB = RoomB.Hash;
            CT.bTunnelFloorTerracingEnabled = Params.bTunnelFloorTerracingEnabled;
            CT.TunnelFloorTerraceStepHeight = Params.TunnelFloorTerraceStepHeight;
            CT.TunnelFloorMaxLedgeHeight = Params.TunnelFloorMaxLedgeHeight;
            CT.TunnelFloorGentleSlopeGradient =
                VoxelPassageGeometry::TunnelFloorGradientFromDegrees(
                    Params.TunnelFloorGentleSlopeThreshold);
            CT.TunnelFloorLedgeCountPreference = Params.TunnelFloorLedgeCountPreference;
            CT.TunnelFloorMaxLedges = Params.TunnelFloorMaxLedges;

            // A room edge is not a straight capsule between cell centres. Build a deterministic
            // chain of hash-jittered control points instead. The chain is keyed only by the pair
            // hash, so every chunk that collects this edge reconstructs the same path; the wide
            // collect region above keeps that topology seam-safe. The envelope is zero at both
            // mouths so the room/tunnel join remains anchored.
            //
            // A direct room-floor delta beyond the strate's gentle threshold is solved in one of
            // two ways. A walk-critical edge gets a sinusoidal switchback whose horizontal length
            // is chosen from the actual floor delta. A graph-redundant or leaf edge may instead
            // receive a deterministic handful of explicit ledge transitions. Both decisions are
            // made before the SDF chain is derived, so the world chain is the authoritative
            // walkability route and the warped chain remains an exact representation of it.
            const float WorldFloorA = static_cast<float>(WorldEndA.Z)
                - FMath::Abs(RadA);
            const float WorldFloorB = static_cast<float>(WorldEndB.Z)
                - FMath::Abs(RadB);
            const float FloorDelta = WorldFloorB - WorldFloorA;
            const float DirectHorizontalRun = FVector2D(
                static_cast<float>(WorldEndB.X - WorldEndA.X),
                static_cast<float>(WorldEndB.Y - WorldEndA.Y)).Size();
            const float GentleThreshold = VoxelPassageGeometry::TunnelFloorGradientFromDegrees(
                Params.TunnelFloorGentleSlopeThreshold);
            const float SafeGentleThreshold = FMath::Max(
                GentleThreshold, KINDA_SMALL_NUMBER);
            const bool bDirectSteep = VoxelMath::IsFinite(WorldFloorA)
                && VoxelMath::IsFinite(WorldFloorB)
                && FMath::Abs(FloorDelta)
                    > FMath::Max(DirectHorizontalRun, KINDA_SMALL_NUMBER)
                        * SafeGentleThreshold;
            const bool bGraphLedgeEligible = GraphEdge.bLeaf
                || (GraphEdge.bRedundant && !GraphEdge.bWalkForestEdge);
            const uint32 LedgeChoiceHash = VoxelHash::Mix(
                TunnelHash ^ 0xC1A0E5EDu);
            constexpr float DramaticLedgeChance = 0.35f;
            const bool bDramaticLedge = Params.bTunnelFloorTerracingEnabled
                && bDirectSteep
                && bGraphLedgeEligible
                && VoxelHash::ToFloat01(LedgeChoiceHash) < DramaticLedgeChance;
            CT.bLedgeGraphEligible = bGraphLedgeEligible;
            CT.bDramaticLedge = bDramaticLedge;

            const int32 MaxLedges = FMath::Clamp(
                Params.TunnelFloorMaxLedges, 1, 4096);
            const int32 PreferredLedges = FMath::Max(
                Params.TunnelFloorLedgeCountPreference, 0);
            const float MaxLedgeHeight = VoxelMath::IsFinite(
                    Params.TunnelFloorMaxLedgeHeight)
                ? FMath::Max(Params.TunnelFloorMaxLedgeHeight, KINDA_SMALL_NUMBER)
                : KINDA_SMALL_NUMBER;
            const int32 HeightDrivenLedges = FMath::Max(
                1, FMath::CeilToInt(FMath::Abs(FloorDelta) / MaxLedgeHeight));
            const int32 RequestedLedges = FMath::Max(
                PreferredLedges, HeightDrivenLedges);
            // The chain deliberately has a finite number of broad control spans. Even an authored
            // max of 4096 cannot turn into a thin staircase; a long change is represented by at
            // most sixteen large drops separated by walkable spans.
            const int32 ChainLedgeCount = bDramaticLedge
                ? FMath::Clamp(RequestedLedges, 1, FMath::Min(MaxLedges, 16))
                : 0;
            CT.TunnelFloorAuthoredLedgeCount = ChainLedgeCount;

            const float TunnelLength = FVector::Dist(WorldEndA, WorldEndB);
            const int32 BaseSegmentDivisor = bDirectSteep ? 40 : 48;
            const int32 BaseSegmentMin = bDirectSteep ? 4 : 3;
            const int32 BaseSegmentMax = bDirectSteep ? 16 : 12;
            const int32 BaseSegments = TunnelLength > 1.0f
                ? FMath::Clamp(
                    FMath::CeilToInt(TunnelLength / static_cast<float>(BaseSegmentDivisor)),
                    BaseSegmentMin, BaseSegmentMax)
                : 1;
            const int32 MinSegmentsForLedges = ChainLedgeCount > 0
                ? FMath::Max(3, ChainLedgeCount * 2)
                : 1;
            const int32 WanderSegments = FMath::Clamp(
                FMath::Max(BaseSegments, MinSegmentsForLedges),
                1, 32);

            // A raw tunnel still needs a continuous, gentle route when its floor relief/ledge
            // modifier is disabled.  The wind is centreline geometry, not an added floor slab; it
            // supplies the horizontal run for the authored floor plane and keeps the room-to-room
            // graph connected.  The rings observed in the owner asset came from the separate
            // world-core handoff exposing span joins, not from this smooth route decision.
            const bool bNeedsWind = bDirectSteep
                && !bDramaticLedge
                && GentleThreshold > KINDA_SMALL_NUMBER;
            CT.bFloorRouteWasWound = bNeedsWind;
            const float RequiredHorizontalRun = bNeedsWind
                ? FMath::Min(
                    VoxelPassageGeometry::TunnelFloorMaximumRouteLengthVoxels,
                    FMath::Abs(FloorDelta) / SafeGentleThreshold)
                : DirectHorizontalRun;
            const int32 WindWaves = bNeedsWind
                ? FMath::Clamp(
                    FMath::CeilToInt(RequiredHorizontalRun
                        / FMath::Max(DirectHorizontalRun, 16.0f)),
                    1, VoxelPassageGeometry::TunnelFloorMaximumWindWaves)
                : 0;
            FVector HorizontalAxis(
                WorldEndB.X - WorldEndA.X, WorldEndB.Y - WorldEndA.Y, 0.0f);
            FVector PerpA;
            if (HorizontalAxis.Normalize())
            {
                PerpA = FVector(-HorizontalAxis.Y, HorizontalAxis.X, 0.0f);
            }
            else
            {
                PerpA = FVector::RightVector;
            }

            CT.WorldControlPoints.SetNum(WanderSegments + 1);
            CT.WorldControlRadii.SetNum(WanderSegments + 1);
            const float MaxWander = FMath::Min(
                FMath::Max(Params.TunnelWarpStrength, 0.0f),
                FMath::Max(TunnelLength, DirectHorizontalRun) * 0.25f);
            const float MaxRadiusVariation = 0.18f;
            auto PopulateWorldChain = [&](float WindAmplitude)
            {
                float PreviousSide = 0.0f;
                for (int32 ControlIndex = 0;
                     ControlIndex <= WanderSegments;
                     ++ControlIndex)
                {
                    const float T = static_cast<float>(ControlIndex)
                        / static_cast<float>(WanderSegments);
                    const float Envelope = VoxelMath::DetSin(T * PI);
                    FVector WorldControlPoint = FMath::Lerp(WorldEndA, WorldEndB, T);
                    const float RadiusBase = FMath::Lerp(RadA, RadB, T);
                    float ControlRadius = RadiusBase;

                    if (ControlIndex > 0 && ControlIndex < WanderSegments)
                    {
                        const uint32 PointHash = VoxelHash::Mix(
                            TunnelHash ^ (0xBADC0DEu
                                + static_cast<uint32>(ControlIndex) * 0x9E3779B9u));
                        const float RawSide = VoxelHash::ToFloatSigned(
                            VoxelHash::Mix(PointHash ^ 0x13579BDFu));
                        // The original low-pass remains the organic baseline. Wind is a separate
                        // deterministic alternating wave, which gives critical edges a visible
                        // serpentine route without moving either mouth.
                        const float Side = RawSide * 0.65f + PreviousSide * 0.35f;
                        const float WindWave = bNeedsWind
                            ? VoxelMath::DetSin(static_cast<float>(WindWaves) * PI * T)
                                * WindAmplitude * Envelope
                                * ((LedgeChoiceHash & 1u) != 0u ? 1.0f : -1.0f)
                            : 0.0f;
                        WorldControlPoint += PerpA * (
                            Side * MaxWander * Envelope + WindWave);
                        PreviousSide = Side;

                        const float RadiusNoise = VoxelHash::ToFloatSigned(
                            VoxelHash::Mix(PointHash ^ 0x5A17EADu));
                        ControlRadius = FMath::Max(
                            0.5f,
                            RadiusBase * (1.0f + RadiusNoise * MaxRadiusVariation * Envelope));
                    }
                    else if (ControlIndex == 0)
                    {
                        PreviousSide = 0.0f;
                    }

                    // Pin the endpoints after all arithmetic. This is the seam and room-mouth
                    // contract: no residual sin(PI) or interpolation rounding moves a mouth.
                    if (ControlIndex == 0)
                    {
                        WorldControlPoint = WorldEndA;
                        ControlRadius = RadA;
                    }
                    else if (ControlIndex == WanderSegments)
                    {
                        WorldControlPoint = WorldEndB;
                        ControlRadius = RadB;
                    }
                    CT.WorldControlPoints[ControlIndex] = WorldControlPoint;
                    CT.WorldControlRadii[ControlIndex] = ControlRadius;
                }
            };
            auto HorizontalPolylineLength = [&]() -> float
            {
                float Length = 0.0f;
                for (int32 Index = 0; Index < WanderSegments; ++Index)
                {
                    const FVector& A = CT.WorldControlPoints[Index];
                    const FVector& B = CT.WorldControlPoints[Index + 1];
                    Length += FVector2D(
                        static_cast<float>(B.X - A.X),
                        static_cast<float>(B.Y - A.Y)).Size();
                }
                return Length;
            };

            PopulateWorldChain(0.0f);
            if (bNeedsWind)
            {
                const float TargetRun = FMath::Max(
                    RequiredHorizontalRun, DirectHorizontalRun);
                float LowAmplitude = 0.0f;
                float HighAmplitude = FMath::Max(
                    8.0f, TargetRun - DirectHorizontalRun);
                PopulateWorldChain(HighAmplitude);
                for (int32 Expand = 0;
                     Expand < 8 && HorizontalPolylineLength() < TargetRun;
                     ++Expand)
                {
                    HighAmplitude = FMath::Max(
                        HighAmplitude * 2.0f, TargetRun);
                    PopulateWorldChain(HighAmplitude);
                }
                for (int32 Iteration = 0; Iteration < 12; ++Iteration)
                {
                    const float MidAmplitude = (LowAmplitude + HighAmplitude) * 0.5f;
                    PopulateWorldChain(MidAmplitude);
                    if (HorizontalPolylineLength() < TargetRun)
                    {
                        LowAmplitude = MidAmplitude;
                    }
                    else
                    {
                        HighAmplitude = MidAmplitude;
                    }
                }
                PopulateWorldChain(HighAmplitude);
            }

            TArray<float, TInlineAllocator<32>> CumulativeHorizontal;
            CumulativeHorizontal.SetNumZeroed(WanderSegments + 1);
            for (int32 Index = 0; Index < WanderSegments; ++Index)
            {
                const FVector& A = CT.WorldControlPoints[Index];
                const FVector& B = CT.WorldControlPoints[Index + 1];
                CumulativeHorizontal[Index + 1] = CumulativeHorizontal[Index]
                    + FVector2D(
                        static_cast<float>(B.X - A.X),
                        static_cast<float>(B.Y - A.Y)).Size();
            }
            const float TotalHorizontalRun = CumulativeHorizontal.Last();
            CT.FloorLedgeLevels.Reset();
            if (ChainLedgeCount > 0)
            {
                CT.FloorLedgeLevels.SetNum(WanderSegments + 1);
            }
            for (int32 ControlIndex = 0;
                 ControlIndex <= WanderSegments;
                 ++ControlIndex)
            {
                const float ArcLengthAlpha = TotalHorizontalRun > KINDA_SMALL_NUMBER
                    ? CumulativeHorizontal[ControlIndex] / TotalHorizontalRun
                    : static_cast<float>(ControlIndex)
                        / static_cast<float>(WanderSegments);
                // Direct gentle tunnels retain their established parameter-linear chain. A
                // winding route needs horizontal arc length for its slope budget, and a ledge
                // route needs it to distribute the few explicit transitions along the route.
                const float RouteAlpha = (bNeedsWind || ChainLedgeCount > 0)
                    ? ArcLengthAlpha
                    : static_cast<float>(ControlIndex)
                        / static_cast<float>(WanderSegments);
                const int32 LedgeLevel = ChainLedgeCount > 0
                    ? FMath::Clamp(FMath::RoundToInt(
                        RouteAlpha * static_cast<float>(ChainLedgeCount)),
                        0, ChainLedgeCount)
                    : 0;
                if (ChainLedgeCount > 0)
                {
                    CT.FloorLedgeLevels[ControlIndex] = LedgeLevel;
                }
                const float AuthoredFloor = ChainLedgeCount > 0
                    ? WorldFloorA + FloorDelta * static_cast<float>(LedgeLevel)
                        / static_cast<float>(ChainLedgeCount)
                    : WorldFloorA + FloorDelta * RouteAlpha;
                CT.WorldControlPoints[ControlIndex].Z = (bNeedsWind || ChainLedgeCount > 0)
                    ? AuthoredFloor + FMath::Abs(CT.WorldControlRadii[ControlIndex])
                    : FMath::Lerp(WorldEndA.Z, WorldEndB.Z,
                        static_cast<float>(ControlIndex)
                            / static_cast<float>(WanderSegments));
            }
            // Pin Z as well as XY after the arc-length floor assignment. The profile starts and
            // ends at the exact room-mouth feet planes even when the route has no horizontal run.
            CT.WorldControlPoints[0] = WorldEndA;
            CT.WorldControlPoints.Last() = WorldEndB;

            CT.ControlPoints.Reserve(WanderSegments + 1);
            CT.ControlRadii.Reserve(WanderSegments + 1);
            for (int32 ControlIndex = 0;
                 ControlIndex <= WanderSegments;
                 ++ControlIndex)
            {
                const FVector& WorldControlPoint = CT.WorldControlPoints[ControlIndex];
                const FVector ControlPoint = (ControlIndex == 0)
                    ? EndA
                    : ((ControlIndex == WanderSegments)
                        ? EndB
                        : VF_ApplyCaveWarp(WorldControlPoint, Params, Seed));
                CT.ControlPoints.Add(ControlPoint);
                CT.ControlRadii.Add(CT.WorldControlRadii[ControlIndex]);
            }

            if (VoxelDensityProfile::AreCountersEnabled())
            {
                if (bDirectSteep)
                {
                    VoxelDensityProfile::AddCounter(
                        VoxelDensityProfile::ECounter::TunnelFloorSteepEdges);
                }
                if (bNeedsWind)
                {
                    VoxelDensityProfile::AddCounter(
                        VoxelDensityProfile::ECounter::TunnelFloorWindingEdges);
                }
                if (bDramaticLedge)
                {
                    VoxelDensityProfile::AddCounter(
                        VoxelDensityProfile::ECounter::TunnelFloorDropEdges);
                }
                VoxelDensityProfile::AddCounter(
                    VoxelDensityProfile::ECounter::TunnelFloorMultiStepEdges, 0);
            }

            // Author the complete floor profile once, after both representations of the chain
            // are known. Evaluation now only projects onto this immutable profile; no sample can
            // independently decide how many terraces the tunnel needs.
            VF_BuildTunnelFloorProfile(
                CT.ControlPoints, CT.ControlRadii, CT, &CT.FloorLedgeLevels,
                CT.FloorProfiles);
            VF_BuildTunnelFloorProfile(
                CT.WorldControlPoints, CT.WorldControlRadii, CT, &CT.FloorLedgeLevels,
                CT.WorldFloorProfiles);
            CT.bHasCompleteFloorProfile = CT.FloorProfiles.Num() == CT.ControlPoints.Num() - 1
                && CT.FloorProfiles.Num() > 0;
            CT.bHasCompleteWorldFloorProfile = CT.WorldFloorProfiles.Num()
                == CT.WorldControlPoints.Num() - 1
                && CT.WorldFloorProfiles.Num() > 0;
            // The floor hand-off is a short apron around each destination, not a second floor
            // that owns the whole room. Bake the same bounded region in both coordinate spaces;
            // this keeps the room/tunnel boolean deterministic even though the cave warp is 3D.
            CT.SDFMouthBlendRadiusA = FMath::Min(
                FMath::Max(RoomA.RadiusXY, 0.0f), VF_TunnelMouthBlendRadiusVoxels);
            CT.SDFMouthBlendRadiusB = FMath::Min(
                FMath::Max(RoomB.RadiusXY, 0.0f), VF_TunnelMouthBlendRadiusVoxels);
            CT.WorldMouthBlendRadiusA = CT.SDFMouthBlendRadiusA;
            CT.WorldMouthBlendRadiusB = CT.SDFMouthBlendRadiusB;
            if (CT.FloorProfiles.Num() == CT.ControlPoints.Num() - 1
                && CT.FloorProfiles.Num() > 0)
            {
                CT.SDFMouthFloorZA = CT.FloorProfiles[0].StartFloorZ;
                CT.SDFMouthFloorZB = CT.FloorProfiles.Last().EndFloorZ;
            }
            if (CT.WorldFloorProfiles.Num() == CT.WorldControlPoints.Num() - 1
                && CT.WorldFloorProfiles.Num() > 0)
            {
                CT.WorldMouthFloorZA = CT.WorldFloorProfiles[0].StartFloorZ;
                CT.WorldMouthFloorZB = CT.WorldFloorProfiles.Last().EndFloorZ;
            }
            else
            {
                CT.WorldMouthFloorZA = static_cast<float>(
                    CT.WorldControlPoints[0].Z)
                    - FMath::Abs(CT.WorldControlRadii[0]);
                CT.WorldMouthFloorZB = static_cast<float>(
                    CT.WorldControlPoints.Last().Z)
                    - FMath::Abs(CT.WorldControlRadii.Last());
            }

            // Author optional room-side benches now that the tunnel mouth floors are known.  A
            // rise is admitted only toward a higher mouth and only when the room's own ceiling
            // still contains the player capsule.  The evaluator later reads this descriptor after
            // the room's base floor is resolved; no per-sample tunnel search or correction is
            // involved.  Strength zero is the exact legacy path.
            const float MouthRiseStrength = FMath::Clamp(
                VoxelMath::IsFinite(Params.RoomMouthRiseStrength)
                    ? Params.RoomMouthRiseStrength : 0.0f,
                0.0f, 1.0f);
            const float MouthRiseBlend = FMath::Clamp(
                VoxelMath::IsFinite(Params.RoomMouthRiseBlendVoxels)
                    ? Params.RoomMouthRiseBlendVoxels : 8.0f,
                8.0f, 128.0f);
            const float RequiredRoomClearance =
                VoxelPassageGeometry::PlayerHeightVoxels
                + VoxelPassageGeometry::HeadroomVoxels;
            const auto AddRoomMouthRise = [
                &RoomMouthRises, &RoomFloorZFor, MouthRiseStrength,
                MouthRiseBlend, RequiredRoomClearance](
                    int32 RoomIndex, const FBuildRoom& Room,
                    const FVector& Mouth, float TargetFloorZ)
            {
                if (!(MouthRiseStrength > 0.0f)
                    || !VoxelMath::IsFinite(TargetFloorZ)
                    || !VoxelMath::IsFinite(Mouth.X)
                    || !VoxelMath::IsFinite(Mouth.Y)
                    || !VoxelMath::IsFinite(Mouth.Z))
                {
                    return;
                }
                const float BaseFloorZ = RoomFloorZFor(Room);
                if (!VoxelMath::IsFinite(BaseFloorZ)
                    || TargetFloorZ <= BaseFloorZ + KINDA_SMALL_NUMBER
                    || Room.Center.Z + Room.RadiusZ - TargetFloorZ
                        < RequiredRoomClearance)
                {
                    return;
                }
                FBuildRoomMouthRise& Rise = RoomMouthRises.Emplace_GetRef();
                Rise.RoomIndex = RoomIndex;
                Rise.Mouth = Mouth;
                Rise.TargetFloorZ = TargetFloorZ;
                Rise.BlendRadius = MouthRiseBlend;
                Rise.Strength = MouthRiseStrength;
            };
            if (CT.ControlPoints.Num() >= 2)
            {
                AddRoomMouthRise(
                    I, RoomA, CT.ControlPoints[0], CT.SDFMouthFloorZA);
                AddRoomMouthRise(
                    J, RoomB, CT.ControlPoints.Last(), CT.SDFMouthFloorZB);
            }

            CT.Midpoint = CT.ControlPoints.Num() > 2
                ? CT.ControlPoints[CT.ControlPoints.Num() / 2]
                : FVector::ZeroVector;
            CT.RadiusMid = CT.ControlRadii.Num() > 2
                ? CT.ControlRadii[CT.ControlRadii.Num() / 2]
                : 0.0f;
            CT.bHasMidpoint = CT.ControlPoints.Num() > 2;

            // Bounding sphere: enclose every control point and the widest local tube. The bound
            // is built from the complete chain, not a midpoint approximation, so culling cannot
            // clip a bend at a chunk edge.
            CT.BoundCenter = FVector::ZeroVector;
            for (const FVector& Point : CT.ControlPoints)
            {
                CT.BoundCenter += Point;
            }
            CT.BoundCenter /= static_cast<float>(FMath::Max(CT.ControlPoints.Num(), 1));
            float MaxTunnelRadius = 0.0f;
            float BoundR = 0.0f;
            for (int32 PointIndex = 0; PointIndex < CT.ControlPoints.Num(); ++PointIndex)
            {
                MaxTunnelRadius = FMath::Max(
                    MaxTunnelRadius, FMath::Abs(CT.ControlRadii[PointIndex]));
                BoundR = FMath::Max(
                    BoundR, FVector::Dist(CT.BoundCenter, CT.ControlPoints[PointIndex]));
            }
            BoundR += MaxTunnelRadius + FMath::Max(BlendK, 0.0f) * 3.0f;
            CT.BoundRadiusSq = BoundR * BoundR;

            // Keep a separate bound for the world-space structural backstop. The SDF and world
            // chains have the same topology but their cave-warped coordinates do not share a
            // useful rejection sphere.
            CT.WorldBoundCenter = FVector::ZeroVector;
            for (const FVector& Point : CT.WorldControlPoints)
            {
                CT.WorldBoundCenter += Point;
            }
            CT.WorldBoundCenter /= static_cast<float>(
                FMath::Max(CT.WorldControlPoints.Num(), 1));
            float WorldBoundR = 0.0f;
            for (int32 PointIndex = 0;
                 PointIndex < CT.WorldControlPoints.Num();
                 ++PointIndex)
            {
                WorldBoundR = FMath::Max(
                    WorldBoundR,
                    FVector::Dist(CT.WorldBoundCenter, CT.WorldControlPoints[PointIndex]));
            }
            // The support predicate reaches LandingFloorThickness below the centreline floor;
            // the world sphere must cover that slab, not only the blended SDF tube.
            WorldBoundR += MaxTunnelRadius + FMath::Max(
                VoxelPassageGeometry::LandingFloorThicknessVoxels,
                FMath::Max(BlendK, 0.0f));
            CT.WorldBoundRadiusSq = WorldBoundR * WorldBoundR;

            // Store if either representation can reach a voxel in this chunk. The warped SDF
            // chain is needed by the room morphology; the world chain is needed by the final
            // walkable-air backstop. Their bounds differ by design, so testing only the former
            // could make a world-space route disappear at a chunk edge.
            if (SphereTouchesSearchXY(CT.BoundCenter, CT.BoundRadiusSq)
                || SphereTouchesSearchXY(CT.WorldBoundCenter, CT.WorldBoundRadiusSq))
            {
                OutCache.Tunnels.Add(CT);
            }
    }

    //==========================================================================
    // FLATTEN INTERSECTING ROOMS
    //==========================================================================
    // A spherical room union is not automatically walkable when the two rooms' floor cuts land
    // at different Z values. For every overlapping, connected room pair, add a short flat bridge
    // only inside their actual horizontal overlap. This is a floor repair, not a graph edge: it
    // does not connect a landing to the origin and it never creates a corridor through unrelated
    // cells. The common plane is the higher of the two existing floors, so it does not cut below a
    // room's authored support; the bridge is admitted only when the shared player/headroom volume
    // still fits below both room ceilings.
    OutCache.RoomFloorJoins.Reserve(NumRooms / 2);
    const float JoinRequiredHeight =
        VoxelPassageGeometry::PlayerHeightVoxels
        + VoxelPassageGeometry::HeadroomVoxels;
    const float JoinMinimumRadius =
        VoxelPassageGeometry::PlayerRadiusVoxels + 0.5f;
    const float JoinCentreClearance =
        VoxelPassageGeometry::PlayerRadiusVoxels + 2.0f;
    for (int32 I = 0; I < NumRooms; ++I)
    {
        const FBuildRoom& RoomA = BuildRooms[I];
        if (!RoomA.bStore || !RoomConnected[I]) continue;

        for (int32 J = I + 1; J < NumRooms; ++J)
        {
            const FBuildRoom& RoomB = BuildRooms[J];
            if (!RoomB.bStore || !RoomConnected[J]) continue;

            const FVector2D DeltaXY(
                RoomB.Center.X - RoomA.Center.X,
                RoomB.Center.Y - RoomA.Center.Y);
            const float DistanceXY = DeltaXY.Size();
            if (DistanceXY <= KINDA_SMALL_NUMBER
                || DistanceXY >= RoomA.RadiusXY + RoomB.RadiusXY)
            {
                continue;
            }

            const float VerticalReach = RoomA.RadiusZ + RoomB.RadiusZ;
            if (FMath::Abs(RoomB.Center.Z - RoomA.Center.Z) >= VerticalReach)
            {
                continue;
            }

            const float FloorA = RoomFloorZFor(RoomA);
            const float FloorB = RoomFloorZFor(RoomB);
            const float CommonFloor = FMath::Max(FloorA, FloorB);
            const float SharedCeiling = FMath::Min(
                RoomA.Center.Z + RoomA.RadiusZ,
                RoomB.Center.Z + RoomB.RadiusZ);
            if (!VoxelMath::IsFinite(CommonFloor)
                || !VoxelMath::IsFinite(SharedCeiling)
                || SharedCeiling - CommonFloor < JoinRequiredHeight)
            {
                continue;
            }

            // Along the centre-centre line, the two projected disks overlap over this interval.
            // Trim both ends so the bridge remains a local room-edge join and leaves each room's
            // player-fit centre untouched for the pure landing query.
            const float OverlapStart = FMath::Max(
                0.0f, DistanceXY - RoomB.RadiusXY);
            const float OverlapEnd = FMath::Min(
                DistanceXY, RoomA.RadiusXY);
            const float OverlapLength = OverlapEnd - OverlapStart;
            if (OverlapLength < 2.0f * JoinCentreClearance)
            {
                continue;
            }

            const float EdgeInset = FMath::Min(2.0f, OverlapLength * 0.2f);
            const float StartDistance = FMath::Max(
                OverlapStart + EdgeInset, JoinCentreClearance);
            const float EndDistance = FMath::Min(
                OverlapEnd - EdgeInset, DistanceXY - JoinCentreClearance);
            if (EndDistance <= StartDistance)
            {
                continue;
            }

            const float JoinRadius = FMath::Min(
                FMath::Min(RoomA.RadiusXY, RoomB.RadiusXY) * 0.35f,
                (EndDistance - StartDistance) * 0.5f);
            if (!VoxelMath::IsFinite(JoinRadius) || JoinRadius < JoinMinimumRadius)
            {
                continue;
            }

            const FVector2D AxisXY = DeltaXY / DistanceXY;
            const float JoinCentreZ = CommonFloor + 0.5f * JoinRequiredHeight;
            FCachedRoomFloorJoin Join;
            Join.Start = FVector(
                RoomA.Center.X + AxisXY.X * StartDistance,
                RoomA.Center.Y + AxisXY.Y * StartDistance,
                JoinCentreZ);
            Join.End = FVector(
                RoomA.Center.X + AxisXY.X * EndDistance,
                RoomA.Center.Y + AxisXY.Y * EndDistance,
                JoinCentreZ);
            Join.Radius = JoinRadius;
            Join.FloorZ = CommonFloor;
            Join.CeilingZ = CommonFloor + JoinRequiredHeight;
            Join.BoundCenter = (Join.Start + Join.End) * 0.5f;
            const float BoundRadius = 0.5f * FVector::Dist(Join.Start, Join.End)
                + FMath::Abs(Join.Radius)
                + FMath::Max(BlendK, 0.0f) * 3.0f;
            Join.BoundRadiusSq = BoundRadius * BoundRadius;

            if (SphereTouchesSearchXY(Join.BoundCenter, Join.BoundRadiusSq))
            {
                OutCache.RoomFloorJoins.Add(MoveTemp(Join));
            }
        }
    }

    //=========================================================================
    // Copy STORE-relevant rooms to cache (cull radii + per-room terrain ops),
    // then pre-bake their pits / chimneys / columns.
    //=========================================================================
    // Build terrain op pool stats once (outside the per-room loop).
    // TerrainOps is the strate's probability pool; each entry has a Probability
    // in [0,1]. We do a weighted random draw per room using the room's hash.
    //
    // PROBABILITY MODEL:
    //   - Entries are checked cumulatively (like drawing from a bucket).
    //   - The "no op" slot takes the remaining probability space (if sum < 1.0).
    //   - If sum >= 1.0, every room gets an op (normalized selection).
    float TotalOpProb = 0.0f;
    if (TerrainOps)
    {
        for (const FStrateTerrainOpEntry& E : *TerrainOps)
            TotalOpProb += FMath::Max(E.Probability, 0.0f);
    }
    const float NormFactor = (TotalOpProb > 1.0f) ? (1.0f / TotalOpProb) : 1.0f;

    // Temporary params struct for reading op fields (PitDensity, PitMinRadius, etc.)
    FStrateGenerationParams OpParams;

    OutCache.Rooms.Reserve(NumRooms);

    for (int32 RoomIdx = 0; RoomIdx < NumRooms; RoomIdx++)
    {
        const FBuildRoom& BR = BuildRooms[RoomIdx];
        if (!BR.bStore) continue;  // Far room — collected for connectivity only

        // Sealed-bubble cull: a room no tunnel ever reaches would be an isolated air
        // pocket — don't carve it at all. The origin landing is always kept (future shaft/root).
        if (!RoomConnected[RoomIdx] && !BR.bIsOrigin) continue;

        FCachedRoom CR;
        CR.Center = BR.Center;
        CR.RadiusXY = BR.RadiusXY;
        CR.RadiusZ = BR.RadiusZ;
        CR.Hash = BR.Hash;
        CR.bIsOrigin = BR.bIsOrigin;

        // Cull radius: max extent the room can reach + blend margin.
        // 1.5x accounts for capsule shapes extending beyond nominal radius.
        float MaxExtent = FMath::Max(
            FMath::Abs(BR.RadiusXY) * 1.5f,
            FMath::Abs(BR.RadiusZ));
        MaxExtent += FloorReliefEnvelope + BlendEnvelope * 3.0f;

        // --- PRE-BAKED SHAPE ---
        // Same hash roll + thresholds + capsule trig the evaluator would otherwise redo PER VOXEL;
        // done once here → EvaluateSDFCached just switches on ShapeType. Bit-identical output.
        {
            const uint32 ShapeHash = VoxelHash::Mix(CR.Hash ^ 0xDEADBEEFu);
            const float ShapeRoll = CR.bIsOrigin ? 0.0f : VoxelHash::ToFloat01(ShapeHash);
            const float BoxThreshold     = 1.0f - Params.RoomShapeVariety * 0.5f;
            const float CapsuleThreshold = 1.0f - Params.RoomShapeVariety * 0.2f;

            if (ShapeRoll >= BoxThreshold && ShapeRoll < CapsuleThreshold)
            {
                // ROUNDED BOX: angular chamber with smooth corners
                CR.ShapeType = 1;
                CR.ShapeA = FVector(CR.RadiusXY * 0.8f, CR.RadiusXY * 0.8f, CR.RadiusZ * 0.8f);
                CR.ShapeR = CR.RadiusXY * 0.25f;
            }
            else if (ShapeRoll >= CapsuleThreshold)
            {
                // ELONGATED CAPSULE: stretched hall/corridor-room
                CR.ShapeType = 2;
                const float DirAngle = VoxelHash::ToFloat01(VoxelHash::Mix(CR.Hash ^ 0xCAFEBABEu)) * 2.0f * PI;
                const float StretchDist = CR.RadiusXY * 0.7f;
                float SinDir = 0.0f, CosDir = 0.0f;
                VoxelMath::DetSinCos(SinDir, CosDir, DirAngle);
                const FVector Dir(CosDir, SinDir, 0.0f);
                CR.ShapeA = CR.Center + Dir * StretchDist;
                CR.ShapeB = CR.Center - Dir * StretchDist;
                CR.ShapeR = FMath::Min(CR.RadiusXY * 0.6f, CR.RadiusZ);
            }
            else
            {
                // ELLIPSOID (default): smooth oval chamber
                CR.ShapeType = 0;
                CR.ShapeA = FVector(CR.RadiusXY, CR.RadiusXY, CR.RadiusZ);
            }

            // Use the actual pre-baked shape's farthest possible distance, not a guessed 1.5x
            // sphere. The old guess underbounded a rounded-box corner when RadiusZ was large.
            float ShapeReach = 0.0f;
            if (CR.ShapeType == 1)
            {
                const float Round = FMath::Abs(CR.ShapeR);
                const FVector Extent(
                    FMath::Abs(CR.ShapeA.X) + Round,
                    FMath::Abs(CR.ShapeA.Y) + Round,
                    FMath::Abs(CR.ShapeA.Z) + Round);
                ShapeReach = Extent.Size();
            }
            else if (CR.ShapeType == 2)
            {
                ShapeReach = 0.5f * FVector::Dist(CR.ShapeA, CR.ShapeB)
                    + FMath::Abs(CR.ShapeR);
            }
            else
            {
                ShapeReach = FMath::Max3(
                    FMath::Abs(CR.ShapeA.X),
                    FMath::Abs(CR.ShapeA.Y),
                    FMath::Abs(CR.ShapeA.Z));
            }
            MaxExtent = ShapeReach + FloorReliefEnvelope + BlendEnvelope * 3.0f;
            if (bHasFeatureReach)
            {
                MaxExtent = FMath::Max(MaxExtent, FeatureReach.TerrainOpReach);
            }
            CR.CullRadiusSq = MaxExtent * MaxExtent;
        }

        // Flat floor cut: soft floor plane per room, hash-rolled from [Min, Max].
        // SmoothMax applied in EvaluateSDFCached so tunnels/pits don't create hard seams.
        // Sentinel -FLT_MAX means "no cut" so the per-voxel check is a single compare.
        {
            const float FloorRoll = VoxelHash::ToFloat01(VoxelHash::Mix(BR.Hash ^ 0xF100F2u));
            const float FloorCut  = FMath::Lerp(
                FMath::Min(Params.RoomFloorCutMin, Params.RoomFloorCutMax),
                FMath::Max(Params.RoomFloorCutMin, Params.RoomFloorCutMax),
                FloorRoll
            );

            if (FloorCut < 1.0f)
            {
                CR.FloorCutZ            = CR.Center.Z - CR.RadiusZ * FloorCut;
                CR.FloorReliefStrength  = Params.FloorReliefStrength;
                CR.FloorReliefFrequency = Params.FloorReliefFrequency;
                CR.FloorSeed            = VoxelHash::Mix(BR.Hash ^ 0xF100F1u);
            }
            else
            {
                CR.FloorCutZ            = -FLT_MAX;
                CR.FloorReliefStrength  = 0.0f;
                CR.FloorReliefFrequency = 0.015f;
                CR.FloorSeed            = 0;
            }
        }

        // --- PER-ROOM TERRAIN OP SELECTION ---
        if (TerrainOps && TerrainOps->Num() > 0 && TotalOpProb > 0.0f)
        {
            const uint32 OpHash = VoxelHash::Mix(BR.Hash ^ 0x0FEED00u);
            const float Roll = VoxelHash::ToFloat01(OpHash);

            float Cursor = 0.0f;
            for (const FStrateTerrainOpEntry& E : *TerrainOps)
            {
                Cursor += FMath::Max(E.Probability, 0.0f) * NormFactor;
                if (Roll < Cursor)
                {
                    const UVoxelTerrainOpDefinition* Op = E.Operation.Get();
                    if (Op)
                    {
                        CR.RoomOp = Op;
                        CR.RoomOpWeight = E.Weight;
                    }
                    break;
                }
            }
        }

        // ARCHES/PINCHES — all geometry below is a pure function of the room hash and the
        // room-local terrain parameters.  Bake the hash chains and the two trigonometric pairs
        // once per room instead of once per near-surface voxel.  The active decision uses the
        // same `ToFloat01(H) > Density` predicate as the old loop; only its location changes.
        FStrateGenerationParams FeatureParams = Params;
        if (CR.RoomOp)
        {
            CR.RoomOp->ApplyTo(FeatureParams, CR.RoomOpWeight);
        }
        for (int32 i = 0; i < 3; ++i)
        {
            const uint32 AH = VoxelHash::Mix(CR.Hash ^ (0xA4C400u + (uint32)i * 7369u));
            if (VoxelHash::ToFloat01(AH) <= FeatureParams.ArchDensity)
            {
                const uint32 AH2 = VoxelHash::Mix(AH ^ 0xA4C4u);
                const float ArcCX = CR.Center.X
                    + VoxelHash::ToFloatSigned(AH2) * CR.RadiusXY * 0.3f;
                const float ArcCY = CR.Center.Y
                    + VoxelHash::ToFloatSigned(VoxelHash::Mix(AH2)) * CR.RadiusXY * 0.3f;
                const uint32 AH3 = VoxelHash::Mix(AH2 ^ 0xB41Du);
                const float ArcCZ = CR.Center.Z
                    + VoxelHash::ToFloatSigned(AH3) * CR.RadiusZ * 0.4f;
                const uint32 AH4 = VoxelHash::Mix(AH3 ^ 0xCAFEu);
                const float Angle = VoxelHash::ToFloat01(AH4) * PI;
                const float HalfSpan = CR.RadiusXY
                    * (0.5f + VoxelHash::ToFloat01(VoxelHash::Mix(AH4)) * 0.35f);
                float SinA = 0.0f, CosA = 0.0f;
                VoxelMath::DetSinCos(SinA, CosA, Angle);
                FCachedArch& Arch = CR.Arches[i];
                Arch.bActive = true;
                Arch.EndpointA = FVector(ArcCX - CosA * HalfSpan,
                                         ArcCY - SinA * HalfSpan, ArcCZ);
                Arch.EndpointB = FVector(ArcCX + CosA * HalfSpan,
                                         ArcCY + SinA * HalfSpan, ArcCZ);
                const uint32 AH5 = VoxelHash::Mix(AH4 ^ 0xF00Du);
                Arch.Radius = FMath::Lerp(
                    FeatureParams.ArchMinRadius, FeatureParams.ArchMaxRadius,
                    VoxelHash::ToFloat01(AH5));
                Arch.BaseDensity = FeatureParams.BaseDensity;
            }

            const uint32 PnH = VoxelHash::Mix(CR.Hash ^ (0xF1C400u + (uint32)i * 5417u));
            if (VoxelHash::ToFloat01(PnH) <= FeatureParams.PinchDensity)
            {
                const uint32 PnH2 = VoxelHash::Mix(PnH ^ 0xF1C4u);
                const float PnX = CR.Center.X
                    + VoxelHash::ToFloatSigned(PnH2) * CR.RadiusXY * 0.85f;
                const float PnY = CR.Center.Y
                    + VoxelHash::ToFloatSigned(VoxelHash::Mix(PnH2)) * CR.RadiusXY * 0.85f;
                const uint32 PnH3 = VoxelHash::Mix(PnH2 ^ 0x5432u);
                const float PnZ = CR.Center.Z
                    + VoxelHash::ToFloatSigned(PnH3) * CR.RadiusZ * 0.5f;
                const uint32 PnH4 = VoxelHash::Mix(PnH3 ^ 0x9A3Bu);
                const float PnAngle = VoxelHash::ToFloat01(PnH4) * PI;
                FCachedPinch& Pinch = CR.Pinches[i];
                Pinch.bActive = true;
                Pinch.CenterX = PnX;
                Pinch.CenterY = PnY;
                Pinch.CenterZ = PnZ;
                VoxelMath::DetSinCos(Pinch.SinAngle, Pinch.CosAngle, PnAngle);
                Pinch.MaxExtent = FMath::Max(
                    FeatureParams.PinchLength, FeatureParams.PinchStrength) + 5.0f;
                Pinch.HalfLength = FeatureParams.PinchLength * 0.5f;
                Pinch.HalfNarrow = FeatureParams.PinchStrength;
                Pinch.HalfVertical = FeatureParams.PinchStrength * 1.5f;
                Pinch.BaseDensity = FeatureParams.BaseDensity;
            }
        }

        // Attach the complete room-side mouth descriptors after the room shape/floor fields are
        // baked.  Expand the conservative room cull to cover the local blend even when the mouth
        // is offset from the room centre.
        for (const FBuildRoomMouthRise& AuthoredRise : RoomMouthRises)
        {
            if (AuthoredRise.RoomIndex != RoomIdx)
            {
                continue;
            }
            FCachedRoomMouthRise& Rise = CR.MouthRises.Emplace_GetRef();
            Rise.Mouth = AuthoredRise.Mouth;
            Rise.TargetFloorZ = AuthoredRise.TargetFloorZ;
            Rise.BlendRadius = AuthoredRise.BlendRadius;
            Rise.Strength = AuthoredRise.Strength;
            const float Reach = static_cast<float>(FVector::Dist(
                CR.Center, Rise.Mouth)) + FMath::Max(Rise.BlendRadius, 0.0f);
            if (VoxelMath::IsFinite(Reach))
            {
                CR.CullRadiusSq = FMath::Max(CR.CullRadiusSq, Reach * Reach);
            }
        }

        OutCache.Rooms.Add(CR);

        //---------------------------------------------------------------------
        // PRE-BAKE: PITS, CHIMNEYS, COLUMNS for this room
        //---------------------------------------------------------------------
        // Pre-baking makes features independent of NearestRoomIdx (no thin "lid"
        // when the owning room flips mid-shaft). Only stored rooms are baked —
        // a far room's features can't reach this chunk anyway.
        if (!CR.RoomOp) continue;

        OpParams = FStrateGenerationParams{};
        CR.RoomOp->ApplyTo(OpParams, CR.RoomOpWeight);

        // PITS — downward shafts anchored in the room's lower half.
        if (!VoxelDensityAblation::IsPitChimneySDFOff())
        {
            BakeRoomFeature(CR, /*Max*/2, OpParams.PitDensity,
            0xDE1A7Eu, 6271u, 0xABCDu, 0x5EEDu,
            /*XYScale*/0.6f, OpParams.PitMinRadius, OpParams.PitMaxRadius,
            [&](float PX, float PY, float PitRadius, uint32 PH3)
            {
                const uint32 PH4 = VoxelHash::Mix(PH3 ^ 0xF00Du);
                FCachedPit Pit;
                Pit.CenterX       = PX;
                Pit.CenterY       = PY;
                Pit.TopZ          = CR.Center.Z - CR.RadiusZ * 0.5f
                                    + VoxelHash::ToFloat01(PH4) * CR.RadiusZ * 0.2f;
                Pit.Radius        = PitRadius;
                Pit.Depth         = OpParams.PitDepth;
                Pit.FlareDist     = PitRadius * 2.0f;
                Pit.FlareExtra    = PitRadius * 1.0f;
                Pit.BaseDensity   = Params.BaseDensity;
                Pit.BlendK        = Params.SDFBlendRadius;
                const float MaxXYR = PitRadius + PitRadius + Params.SDFBlendRadius + 4.0f;
                Pit.BoundXYRadiusSq = MaxXYR * MaxXYR;
                OutCache.Pits.Add(Pit);
            });

            // CHIMNEYS — mirror of pits: upward tubes anchored in the room's upper half.
            BakeRoomFeature(CR, /*Max*/2, OpParams.ChimneyDensity,
            0xC4F007u, 7919u, 0x1337u, 0xCAFEu,
            /*XYScale*/0.6f, OpParams.ChimneyMinRadius, OpParams.ChimneyMaxRadius,
            [&](float CX, float CY, float ChmRadius, uint32 CH3)
            {
                const uint32 CH4 = VoxelHash::Mix(CH3 ^ 0xD00Du);
                FCachedChimney Chim;
                Chim.CenterX       = CX;
                Chim.CenterY       = CY;
                Chim.BottomZ       = CR.Center.Z + CR.RadiusZ * 0.5f
                                     - VoxelHash::ToFloat01(CH4) * CR.RadiusZ * 0.2f;
                Chim.Radius        = ChmRadius;
                Chim.Height        = OpParams.ChimneyHeight;
                Chim.FlareDist     = ChmRadius * 2.0f;
                Chim.FlareExtra    = ChmRadius * 1.0f;
                Chim.BaseDensity   = Params.BaseDensity;
                Chim.BlendK        = Params.SDFBlendRadius;
                const float MaxXYR = ChmRadius + ChmRadius + Params.SDFBlendRadius + 4.0f;
                Chim.BoundXYRadiusSq = MaxXYR * MaxXYR;
                OutCache.Chimneys.Add(Chim);
            });
        }

        // COLUMNS — full-height solid cylinders (no Z anchor, no flare).
        BakeRoomFeature(CR, /*Max*/4, OpParams.ColumnDensity,
            0xC01C01u, 3571u, 0x1A2B3Cu, 0xBEEFu,
            /*XYScale*/0.75f, OpParams.ColumnMinRadius, OpParams.ColumnMaxRadius,
            [&](float ColX, float ColY, float ColR, uint32 /*H3*/)
            {
                FCachedColumn Col;
                Col.CenterX       = ColX;
                Col.CenterY       = ColY;
                Col.Radius        = ColR;
                Col.BaseDensity   = Params.BaseDensity;
                const float MaxXYR = ColR + 6.0f;
                Col.BoundXYRadiusSq = MaxXYR * MaxXYR;
                OutCache.Columns.Add(Col);
        });
    }

    // All candidate arrays are complete now. Build the immutable broad phase only after every
    // deterministic room/tunnel decision and feature bake has finished.
    VF_FinalizeCacheBroadPhaseBounds(OutCache, BlendK);
}

//=============================================================================
// PHASE 2: EVALUATE SDF WITH CACHED DATA
//=============================================================================
// Pure SDF math — no hashing, no array building, no backbone computation.
// Just loops through the pre-built rooms and tunnels, evaluates distance,
// and smooth-mins everything together. Distance culling skips primitives
// that are clearly too far to contribute.

float VoxelCaveMorphology::EvaluateSDFCached(
    float WorldX, float WorldY, float WorldZ,
    const FChunkSDFCache& Cache,
    float SDFBlendRadius,
    int32* OutNearestRoomIdx,
    bool bUseSpatialIndex,
    const FVector* WorldTunnelPosition,
    bool* OutRoomOwnsBottom)
{
    VoxelDensityProfile::FScopedTimer ProfileTimer(
        VoxelDensityProfile::EBucket::RoomGraphSdf);
    float MinSDF = FLT_MAX;
    const float BlendK = SDFBlendRadius;
    const FVector Pos(WorldX, WorldY, WorldZ);
    const FVFFloorReliefColumnKey ReliefColumn =
        VF_MakeFloorReliefColumnKey(Pos.X, Pos.Y);
    const FCachedRoom* RoomData = Cache.Rooms.GetData();
    const FCachedRoomFloorJoin* RoomFloorJoinData = Cache.RoomFloorJoins.GetData();
    const FCachedTunnel* TunnelData = Cache.Tunnels.GetData();

    // Track which room contributes the smallest (most-inside) raw SDF.
    // This is used by the terrain ops system to find the "owning" room for
    // this voxel and apply that room's per-room terrain operation.
    // We track raw room SDF (before SmoothMin) so tunnel SDFs don't interfere.
    float NearestRoomRawSDF = FLT_MAX;
    int32 NearestIdx = -1;
    // EvaluateRoom already computes the exact raw authored room shape used by the room field.
    // Keep its sign for the room-ownership rule instead of traversing the room spatial index a
    // second time after the room pass.
    bool bInsideRoomShape = false;

    //=========================================================================
    // Room SDFs
    //=========================================================================
    auto EvaluateRoom = [&](int32 RoomIdx)
    {
        const FCachedRoom& Room = RoomData[RoomIdx];

        // --- DISTANCE CULL ---
        const float DistSq = VF_FloatDistSquared(WorldX, WorldY, WorldZ, Room.Center);
        if (DistSq > Room.CullRadiusSq) return;
        if (VoxelDensityProfile::AreCountersEnabled())
        {
            VoxelDensityProfile::AddCounter(
                VoxelDensityProfile::ECounter::CaveRoomEvaluated);
        }

        // --- SHAPE (pre-baked in BuildChunkCache — no per-voxel hash roll / trig) ---
        float RoomSDF;
        switch (Room.ShapeType)
        {
        case 1:  RoomSDF = VoxelSDF::RoundedBox(Pos, Room.Center, Room.ShapeA, Room.ShapeR); break;
        case 2:  RoomSDF = VoxelSDF::Capsule(Pos, Room.ShapeA, Room.ShapeB, Room.ShapeR);    break;
            default: RoomSDF = VoxelSDF::Ellipsoid(Pos, Room.Center, Room.ShapeA);               break;
        }
        // This is deliberately before the optional floor cut. Room ownership is defined by the
        // authored body, not by the room's raised/relieved floor SDF.
        if (RoomSDF < 0.0f)
        {
            bInsideRoomShape = true;
        }

        // Soft floor: SmoothMax of the room SDF and the floor half-space.
        if (Room.FloorCutZ > -FLT_MAX)
        {
            float FloorZ = Room.FloorCutZ;
            for (const FCachedRoomMouthRise& Rise : Room.MouthRises)
            {
                const float DX = static_cast<float>(Pos.X - Rise.Mouth.X);
                const float DY = static_cast<float>(Pos.Y - Rise.Mouth.Y);
                const float Radius = FMath::Max(Rise.BlendRadius, 1.0f);
                const float Distance = FMath::Sqrt(DX * DX + DY * DY);
                if (Distance >= Radius
                    || !(Rise.Strength > 0.0f)
                    || !VoxelMath::IsFinite(Rise.TargetFloorZ))
                {
                    continue;
                }
                const float Weight = 1.0f - SmoothStep01(
                    FMath::Clamp(Distance / Radius, 0.0f, 1.0f));
                const float DesiredFloorZ = FMath::Lerp(
                    FloorZ, Rise.TargetFloorZ,
                    Weight * FMath::Clamp(Rise.Strength, 0.0f, 1.0f));
                // A lower tunnel never cuts down into the room.  The room remains the owner of
                // its floor, while a higher mouth receives a deliberate, smooth bench.
                FloorZ = FMath::Max(FloorZ, DesiredFloorZ);
            }
            const float FloorBlend = BlendK * 0.35f;
            bool bFloorCutIsIdentity = false;
            if (Room.FloorReliefStrength > 0.0f)
            {
                float ReliefBound = 0.0f;
                if (VF_GetFloorReliefBound(
                        Room.FloorReliefStrength, 1.0f, 1.0f,
                        Room.FloorReliefFrequency, ReliefBound))
                {
                    // SmoothMax(A, B, K) is exactly A when A >= B + K. Use the highest
                    // possible relieved floor as B's bound. The extra margin is deliberately
                    // conservative; an uncertain sample pays for the two noise octaves.
                    bFloorCutIsIdentity = RoomSDF >=
                        Room.FloorCutZ + ReliefBound - static_cast<float>(Pos.Z)
                        + FMath::Max(FloorBlend, 0.0f);
                }

                if (!bFloorCutIsIdentity)
                {
                    float N = VF_FloorReliefNoiseDouble(
                        Pos.X, Pos.Y,
                        Room.FloorSeed, Room.FloorReliefFrequency,
                        &Cache, EVFFloorReliefFeature::RoomDouble, RoomIdx,
                        ReliefColumn);
                    N *= VOXEL_NOISE_SCALE;
                    FloorZ += N * Room.FloorReliefStrength;
                }
            }

            if (!bFloorCutIsIdentity)
            {
                RoomSDF = VoxelSDF::SmoothMax(RoomSDF, FloorZ - Pos.Z, FloorBlend);
            }
        }

        // Track the room whose SDF is smallest (most inside).
        if (OutNearestRoomIdx && RoomSDF < NearestRoomRawSDF)
        {
            NearestRoomRawSDF = RoomSDF;
            NearestIdx = RoomIdx;
        }

        MinSDF = VoxelSDF::SmoothMin(MinSDF, RoomSDF, BlendK);
    };

    int32 RoomCandidateCount = 0;
    if (!VoxelDensityAblation::IsRoomSDFOff())
    {
        RoomCandidateCount = VF_ForEachSpatialCandidate(
            Cache.RoomSpatialIndex, Cache.Rooms.Num(), WorldX, WorldY, EvaluateRoom,
            bUseSpatialIndex);
    }
    if (VoxelDensityProfile::AreCountersEnabled())
    {
        VoxelDensityProfile::AddCounter(
            VoxelDensityProfile::ECounter::CaveRoomCandidates,
            static_cast<uint64>(RoomCandidateCount));
    }
    const bool bRoomOwnsBottom = !VoxelDensityAblation::IsRoomSDFOff()
        && bInsideRoomShape;
    if (OutRoomOwnsBottom)
    {
        *OutRoomOwnsBottom = bRoomOwnsBottom;
    }

    //==========================================================================
    // Flattened room joins
    //==========================================================================
    // These short bridges exist only where two room bodies already overlap. Their floor is a
    // single horizontal plane, so a player crossing the shared volume cannot meet the tall lip
    // produced by two independent floor cuts. They are deliberately not reported as rooms and do
    // not participate in nearest-room terrain-op ownership.
    auto EvaluateJoin = [&](int32 JoinIdx)
    {
        const FCachedRoomFloorJoin& Join = RoomFloorJoinData[JoinIdx];
        const float DistSq = VF_FloatDistSquared(WorldX, WorldY, WorldZ, Join.BoundCenter);
        if (DistSq > Join.BoundRadiusSq) return;

        const float HorizontalSDF = VoxelSDF::Capsule(
            Pos, Join.Start, Join.End, Join.Radius);
        const float VerticalSDF = FMath::Max(
            Join.FloorZ - Pos.Z,
            Pos.Z - Join.CeilingZ);
        const float JoinSDF = FMath::Max(HorizontalSDF, VerticalSDF);
        MinSDF = VoxelSDF::SmoothMin(MinSDF, JoinSDF, BlendK);
    };
    if (!VoxelDensityAblation::IsRoomSDFOff())
    {
        VF_ForEachSpatialCandidate(
            Cache.RoomFloorJoinSpatialIndex, Cache.RoomFloorJoins.Num(),
            WorldX, WorldY, EvaluateJoin, bUseSpatialIndex);
    }

    //=========================================================================
    // Tunnel SDFs
    //=========================================================================
    // Keep the tunnel source in the authored world frame when the caller supplies it. Rooms
    // intentionally remain at the warped query position; the world-chain tunnel is the same
    // continuous field used by the core hand-off below, so the source cannot form a second,
    // differently-shaped surface around the route.
    const bool bUseWorldTunnel = WorldTunnelPosition != nullptr;
    const FVector TunnelPosition = bUseWorldTunnel ? *WorldTunnelPosition : Pos;
    // A room owns every point inside its authored body. The short mouth ownership fade below
    // handles the fillet outside the wall; this broader test is the hard A > B rule for the room
    // interior. The exact raw shape sign was captured during the room SDF pass above, so this
    // point does not perform a second spatial-index traversal.
    auto EvaluateTunnel = [&](int32 TunnelIdx)
    {
        const FCachedTunnel& Tunnel = TunnelData[TunnelIdx];
        const bool bHasWorldChain = Tunnel.WorldControlPoints.Num() >= 2
            && Tunnel.WorldControlRadii.Num() == Tunnel.WorldControlPoints.Num();
        const bool bUseWorldChain = bUseWorldTunnel && bHasWorldChain;
        const FVector& QueryPosition = bUseWorldChain ? TunnelPosition : Pos;
        const FVector& CenterlineMin = bUseWorldChain
            ? Tunnel.WorldCenterlineMin : Tunnel.SDFCenterlineMin;
        const FVector& CenterlineMax = bUseWorldChain
            ? Tunnel.WorldCenterlineMax : Tunnel.SDFCenterlineMax;
        const float InfluenceRadius = bUseWorldChain
            ? Tunnel.WorldInfluenceRadius : Tunnel.SDFInfluenceRadius;
        const FVector& BoundCenter = bUseWorldChain
            ? Tunnel.WorldBoundCenter : Tunnel.BoundCenter;
        const float BoundRadiusSq = bUseWorldChain
            ? Tunnel.WorldBoundRadiusSq : Tunnel.BoundRadiusSq;
        if (InfluenceRadius > 0.0f
            && VF_DistanceSquaredToAabb(
                QueryPosition.X, QueryPosition.Y, QueryPosition.Z,
                CenterlineMin, CenterlineMax)
                > FMath::Square(InfluenceRadius))
        {
            return;
        }
        // --- BOUNDING SPHERE CULL ---
        const float DistSq = VF_FloatDistSquared(
            QueryPosition.X, QueryPosition.Y, QueryPosition.Z, BoundCenter);
        if (DistSq > BoundRadiusSq) return;
        if (VoxelDensityProfile::AreCountersEnabled())
        {
            VoxelDensityProfile::AddCounter(
                VoxelDensityProfile::ECounter::CaveTunnelEvaluated);
        }

        // The corridor is a swept floor cut: each tapered capsule segment is intersected with
        // its build-time floor profile before the tunnel union is blended into rooms. The room's
        // finite mouth ownership is also passed into that same shape evaluation; this is the SDF
        // side of A > B, while the world-space post keeps the disturbance path consistent.
        FVFRoomFloorOwnership RoomMouthOwnership;
        const FVFRoomFloorOwnership* RoomMouthOwnershipPtr = nullptr;
        if (!bRoomOwnsBottom
            && VF_MayHaveTunnelMouthOwnership(
                QueryPosition, Tunnel, bUseWorldChain, BlendK))
        {
            RoomMouthOwnership = bUseWorldChain
                ? VF_FindWorldTunnelMouthOwnership(QueryPosition, Tunnel, BlendK)
                : VF_FindSDFTunnelMouthOwnership(Pos, Tunnel, BlendK);
            if (RoomMouthOwnership.bValid || RoomMouthOwnership.bSuppressTunnelBottom)
            {
                RoomMouthOwnershipPtr = &RoomMouthOwnership;
            }
        }
        float TunnelSDF = 0.0f;
        if (!bRoomOwnsBottom && RoomMouthOwnershipPtr == nullptr)
        {
            // Keep the overwhelmingly common open-corridor path on the same literal arguments
            // as the pre-ownership evaluator. Besides avoiding the ownership checks, this lets
            // the compiler fold the floor-cut and null-ownership branches in the swept kernel.
            TunnelSDF = VF_EvaluateSweptTunnel(
                QueryPosition, Tunnel, bUseWorldChain, BlendK,
                /*bApplyFloorCut=*/true, &Cache, TunnelIdx).SDF;
        }
        else
        {
            TunnelSDF = VF_EvaluateSweptTunnel(
                QueryPosition, Tunnel, bUseWorldChain, BlendK,
                /*bApplyFloorCut=*/!(bRoomOwnsBottom
                    || (RoomMouthOwnershipPtr != nullptr
                        && RoomMouthOwnershipPtr->bSuppressTunnelBottom)),
                &Cache, TunnelIdx, RoomMouthOwnershipPtr).SDF;
        }

        MinSDF = VoxelSDF::SmoothMin(MinSDF, TunnelSDF, BlendK);
    };

    int32 TunnelCandidateCount = 0;
    if (!VoxelDensityAblation::IsTunnelSDFOff())
    {
        TunnelCandidateCount = VF_ForEachSpatialCandidate(
            Cache.TunnelSpatialIndex, Cache.Tunnels.Num(), TunnelPosition.X, TunnelPosition.Y,
            EvaluateTunnel,
            bUseSpatialIndex);
    }
    if (VoxelDensityProfile::AreCountersEnabled())
    {
        VoxelDensityProfile::AddCounter(
            VoxelDensityProfile::ECounter::CaveTunnelCandidates,
            static_cast<uint64>(TunnelCandidateCount));
    }

    // Write nearest room index for the caller (terrain ops system)
    if (OutNearestRoomIdx)
    {
        *OutNearestRoomIdx = NearestIdx;
    }

    return MinSDF;
}

FTunnelCoreWorldEvaluation VoxelCaveMorphology::EvaluateTunnelCoreWorld(
    float WorldX, float WorldY, float WorldZ,
    const FChunkSDFCache& Cache,
    const FTunnelSupportFloorColumn* SupportColumn,
    bool bUseSpatialIndex,
    const FVector* RoomQueryPosition,
    const bool* CachedRoomOwnsBottom)
{
    VoxelGenLOD::FScopedReachCost ReachCost(
        VoxelGenLOD::ETileReachCostKind::TunnelCoreWorld);
    if (VoxelDensityAblation::IsTunnelCoreOff())
    {
        return FTunnelCoreWorldEvaluation();
    }
    // GenerateMesh installs the exact tile/block reach mask after building the complete tile
    // cache. Point queries retain the canonical all-reachable TLS default; debug proof calls set
    // the bypass bit so the same sample can be evaluated as the reference field.
    // A reach mask is meaningful only inside the mesher's proven tile session.  Point queries
    // (including the owner-value probe and gameplay queries) must retain the canonical core even
    // if another caller left a conservative flag in this worker's TLS.  The mesher enables the
    // proof bit only after it has installed the exact tile-window decision, so this guard cannot
    // turn an ordinary query into a skipped core.
    if (VoxelGenLOD::bTileCoreReachProofEnabled
        && VoxelGenLOD::bTileReachDiagnosticTile
        && !VoxelGenLOD::IsTilePostReachBypassActive()
        && !VoxelGenLOD::IsTunnelCoreReachable())
    {
        return FTunnelCoreWorldEvaluation();
    }
    VoxelDensityProfile::FScopedTimer ProfileTimer(
        VoxelDensityProfile::EBucket::TunnelCoreWorld);
    const FVector Pos(WorldX, WorldY, WorldZ);
    FTunnelCoreWorldEvaluation Result;
    const FCachedTunnel* TunnelData = Cache.Tunnels.GetData();
    const FVector& RoomPosition = RoomQueryPosition != nullptr
        ? *RoomQueryPosition : Pos;
    const bool bRoomOwnsBottom = CachedRoomOwnsBottom != nullptr
        ? *CachedRoomOwnsBottom
        : VF_IsInsideCachedRoomShape(RoomPosition, Cache, bUseSpatialIndex);

    if (SupportColumn != nullptr && VoxelDensityProfile::AreCountersEnabled())
    {
        VoxelDensityProfile::AddCounter(
            VoxelDensityProfile::ECounter::TunnelSupportFloorQueries);
    }
    auto EvaluateTunnel = [&](int32 TunnelIdx)
    {
        const FCachedTunnel& Tunnel = TunnelData[TunnelIdx];
        const bool bHasWorldChain = Tunnel.WorldControlPoints.Num() >= 2
            && Tunnel.WorldControlRadii.Num() == Tunnel.WorldControlPoints.Num();
        bool bSDFNear = true;
        if (Tunnel.SDFInfluenceRadius > 0.0f
            && VF_DistanceSquaredToAabb(
                WorldX, WorldY, WorldZ,
                Tunnel.SDFCenterlineMin, Tunnel.SDFCenterlineMax)
                > FMath::Square(Tunnel.SDFInfluenceRadius))
        {
            bSDFNear = false;
        }
        if (Tunnel.BoundRadiusSq > 0.0f
            && VF_FloatDistSquared(
                WorldX, WorldY, WorldZ, Tunnel.BoundCenter) > Tunnel.BoundRadiusSq)
        {
            bSDFNear = false;
        }
        bool bWorldNear = false;
        if (bHasWorldChain)
        {
            bWorldNear = true;
            if (Tunnel.WorldInfluenceRadius > 0.0f
                && VF_DistanceSquaredToAabb(
                    WorldX, WorldY, WorldZ,
                    Tunnel.WorldCenterlineMin, Tunnel.WorldCenterlineMax)
                    > FMath::Square(Tunnel.WorldInfluenceRadius))
            {
                bWorldNear = false;
            }
            if (Tunnel.WorldBoundRadiusSq > 0.0f
                && VF_FloatDistSquared(
                    WorldX, WorldY, WorldZ, Tunnel.WorldBoundCenter)
                    > Tunnel.WorldBoundRadiusSq)
            {
                bWorldNear = false;
            }
        }
        if (!bSDFNear && !bWorldNear)
        {
            return;
        }
        if (VoxelDensityProfile::AreCountersEnabled())
        {
            VoxelDensityProfile::AddCounter(
                VoxelDensityProfile::ECounter::TunnelCoreEvaluated);
        }

        FVFRoomFloorOwnership RoomMouthOwnership;
        const FVFRoomFloorOwnership* RoomMouthOwnershipPtr = nullptr;
        if (!bRoomOwnsBottom
            && VF_MayHaveTunnelMouthOwnership(
                Pos, Tunnel, /*bWorldChain=*/true, Cache.SDFBlendRadius))
        {
            RoomMouthOwnership =
                VF_FindWorldTunnelMouthOwnership(Pos, Tunnel, Cache.SDFBlendRadius);
            if (RoomMouthOwnership.bValid || RoomMouthOwnership.bSuppressTunnelBottom)
            {
                RoomMouthOwnershipPtr = &RoomMouthOwnership;
            }
        }
        // The world chain is the authored tunnel shape. Its floor is evaluated by the same
        // SmoothMax profile as the cave-warped chain; there is no second support-column field and
        // no hard floor hand-off at a segment boundary. Keep the legacy argument source-compatible
        // for callers compiled against the previous probe API, but deliberately ignore it.
        (void)SupportColumn;

        FVFTunnelShapeEvaluation TunnelShape;
        if (!bRoomOwnsBottom && RoomMouthOwnershipPtr == nullptr)
        {
            TunnelShape = VF_EvaluateSweptTunnel(
                Pos, Tunnel, bHasWorldChain, Cache.SDFBlendRadius,
                /*bApplyFloorCut=*/true, &Cache, TunnelIdx);
        }
        else
        {
            TunnelShape = VF_EvaluateSweptTunnel(
                Pos, Tunnel, bHasWorldChain, Cache.SDFBlendRadius,
                /*bApplyFloorCut=*/!(bRoomOwnsBottom
                    || (RoomMouthOwnershipPtr != nullptr
                        && RoomMouthOwnershipPtr->bSuppressTunnelBottom)),
                &Cache, TunnelIdx, RoomMouthOwnershipPtr);
        }
        if (TunnelShape.SDF < Result.SDF)
        {
            Result.SDF = TunnelShape.SDF;
            Result.bRoomFloor = bRoomOwnsBottom
                || (RoomMouthOwnershipPtr != nullptr
                    && RoomMouthOwnershipPtr->bSuppressTunnelBottom);
            Result.bHasSweptFloor = TunnelShape.bHasSweptFloor;
            Result.SweptFloorZ = TunnelShape.SweptFloorZ;
            Result.SweptFloorRadius = TunnelShape.SweptFloorRadius;
        }
    };

    const int32 TunnelCandidateCount = VF_ForEachSpatialCandidate(
        Cache.TunnelSpatialIndex, Cache.Tunnels.Num(), WorldX, WorldY, EvaluateTunnel,
        bUseSpatialIndex);
    if (VoxelDensityProfile::AreCountersEnabled())
    {
        VoxelDensityProfile::AddCounter(
            VoxelDensityProfile::ECounter::TunnelCoreCandidates,
            static_cast<uint64>(TunnelCandidateCount));
    }

    return Result;
}

bool VoxelCaveMorphology::AnyTunnelCoreWorldNearLattice(
    const FChunkSDFCache& Cache,
    const FBox& VoxelBox,
    float ReachScale)
{
    if (VoxelDensityAblation::IsTunnelCoreOff())
    {
        return false;
    }
    if (!VoxelBox.IsValid
        || !VoxelMath::IsFinite(ReachScale) || ReachScale <= 0.0f)
    {
        return true;
    }

    const auto TouchesSphere = [&VoxelBox](const FVector& Center, float Radius) -> bool
    {
        if (!VoxelMath::IsFinite(Center.X)
            || !VoxelMath::IsFinite(Center.Y)
            || !VoxelMath::IsFinite(Center.Z)
            || !VoxelMath::IsFinite(Radius) || Radius < 0.0f)
        {
            return true;
        }
        const FVector Closest(
            FMath::Clamp(Center.X, (float)VoxelBox.Min.X, (float)VoxelBox.Max.X),
            FMath::Clamp(Center.Y, (float)VoxelBox.Min.Y, (float)VoxelBox.Max.Y),
            FMath::Clamp(Center.Z, (float)VoxelBox.Min.Z, (float)VoxelBox.Max.Z));
        return FVector::DistSquared(Closest, Center) <= Radius * Radius;
    };

    const auto TouchesExpandedAabb = [&VoxelBox](
        const FVector& Min, const FVector& Max, float Pad) -> bool
    {
        if (!VoxelMath::IsFinite(Min.X) || !VoxelMath::IsFinite(Min.Y)
            || !VoxelMath::IsFinite(Min.Z) || !VoxelMath::IsFinite(Max.X)
            || !VoxelMath::IsFinite(Max.Y) || !VoxelMath::IsFinite(Max.Z)
            || !VoxelMath::IsFinite(Pad) || Pad < 0.0f)
        {
            return true;
        }
        const float DX = FMath::Max3(
            static_cast<float>(VoxelBox.Min.X) - static_cast<float>(Max.X),
            static_cast<float>(Min.X) - static_cast<float>(VoxelBox.Max.X), 0.0f);
        const float DY = FMath::Max3(
            static_cast<float>(VoxelBox.Min.Y) - static_cast<float>(Max.Y),
            static_cast<float>(Min.Y) - static_cast<float>(VoxelBox.Max.Y), 0.0f);
        const float DZ = FMath::Max3(
            static_cast<float>(VoxelBox.Min.Z) - static_cast<float>(Max.Z),
            static_cast<float>(Min.Z) - static_cast<float>(VoxelBox.Max.Z), 0.0f);
        return DX <= Pad && DY <= Pad && DZ <= Pad;
    };

    const float Fade = FMath::Max(Cache.SDFBlendRadius, 0.0f) * 0.35f;
    if (!VoxelMath::IsFinite(Fade))
    {
        return true;
    }

    for (const FCachedTunnel& Tunnel : Cache.Tunnels)
    {
        const bool bHasWorldChain = Tunnel.WorldControlPoints.Num() >= 2
            && Tunnel.WorldControlRadii.Num() == Tunnel.WorldControlPoints.Num();
        const FVector& BoundCenter = bHasWorldChain
            ? Tunnel.WorldBoundCenter : Tunnel.BoundCenter;
        const float BoundRadiusSq = bHasWorldChain
            ? Tunnel.WorldBoundRadiusSq : Tunnel.BoundRadiusSq;
        const float InfluenceRadius = bHasWorldChain
            ? Tunnel.WorldInfluenceRadius : Tunnel.SDFInfluenceRadius;
        const FVector& CenterlineMin = bHasWorldChain
            ? Tunnel.WorldCenterlineMin : Tunnel.SDFCenterlineMin;
        const FVector& CenterlineMax = bHasWorldChain
            ? Tunnel.WorldCenterlineMax : Tunnel.SDFCenterlineMax;

        if (!VoxelMath::IsFinite(BoundCenter.X)
            || !VoxelMath::IsFinite(BoundCenter.Y)
            || !VoxelMath::IsFinite(BoundCenter.Z)
            || !VoxelMath::IsFinite(BoundRadiusSq) || BoundRadiusSq < 0.0f
            || !VoxelMath::IsFinite(InfluenceRadius) || InfluenceRadius < 0.0f
            || !VoxelMath::IsFinite(CenterlineMin.X)
            || !VoxelMath::IsFinite(CenterlineMin.Y)
            || !VoxelMath::IsFinite(CenterlineMin.Z)
            || !VoxelMath::IsFinite(CenterlineMax.X)
            || !VoxelMath::IsFinite(CenterlineMax.Y)
            || !VoxelMath::IsFinite(CenterlineMax.Z))
        {
            return true;
        }
        if (bHasWorldChain)
        {
            for (int32 Index = 0; Index < Tunnel.WorldControlPoints.Num(); ++Index)
            {
                const FVector& Point = Tunnel.WorldControlPoints[Index];
                if (!VoxelMath::IsFinite(Point.X) || !VoxelMath::IsFinite(Point.Y)
                    || !VoxelMath::IsFinite(Point.Z)
                    || !VoxelMath::IsFinite(Tunnel.WorldControlRadii[Index]))
                {
                    return true;
                }
            }
        }

        // VF_TunnelFloorRelief uses the same bounded noise as the floor profile.  Use the
        // profile-independent maximum scale (1.0) here: the normal authored scale is <= .20,
        // but this remains sound for an old or malformed profile that bypassed that clamp.
        float ReliefBound = 0.0f;
        if (Tunnel.FloorReliefStrength > 0.0f)
        {
            if (!VoxelMath::IsFinite(Tunnel.FloorReliefStrength)
                || !VoxelMath::IsFinite(Tunnel.FloorReliefFrequency))
            {
                return true;
            }
            ReliefBound = FMath::Abs(Tunnel.FloorReliefStrength)
                * VOXEL_NOISE_SCALE * 1.01f;
            if (!VoxelMath::IsFinite(ReliefBound))
            {
                return true;
            }
        }

        float MouthReach = 0.0f;
        float MouthFloorDelta = 0.0f;
        if (bHasWorldChain && Tunnel.bHasFloorRoomOwnership)
        {
            MouthReach = FMath::Max(
                FMath::Max(Tunnel.WorldMouthBlendRadiusA, Tunnel.WorldMouthBlendRadiusB),
                0.0f) + Fade + 1.0f;
            if (!VoxelMath::IsFinite(MouthReach))
            {
                return true;
            }
            const float NaturalA = static_cast<float>(Tunnel.WorldControlPoints[0].Z)
                - FMath::Abs(Tunnel.WorldControlRadii[0]);
            const int32 Last = Tunnel.WorldControlPoints.Num() - 1;
            const float NaturalB = static_cast<float>(Tunnel.WorldControlPoints[Last].Z)
                - FMath::Abs(Tunnel.WorldControlRadii[Last]);
            if (!VoxelMath::IsFinite(NaturalA) || !VoxelMath::IsFinite(NaturalB))
            {
                return true;
            }
            if (Tunnel.WorldMouthFloorZA > -FLT_MAX
                && !VoxelMath::IsFinite(Tunnel.WorldMouthFloorZA))
            {
                return true;
            }
            if (Tunnel.WorldMouthFloorZB > -FLT_MAX
                && !VoxelMath::IsFinite(Tunnel.WorldMouthFloorZB))
            {
                return true;
            }
            if (Tunnel.WorldMouthFloorZA > -FLT_MAX)
            {
                MouthFloorDelta = FMath::Max(
                    MouthFloorDelta, FMath::Abs(Tunnel.WorldMouthFloorZA - NaturalA));
            }
            if (Tunnel.WorldMouthFloorZB > -FLT_MAX)
            {
                MouthFloorDelta = FMath::Max(
                    MouthFloorDelta, FMath::Abs(Tunnel.WorldMouthFloorZB - NaturalB));
            }
        }

        // Tighten the construction sphere to the actual swept chain before falling back to the
        // legacy bound.  The evaluator tests a capsule segment, not the sphere around the whole
        // wandering chain; using segment AABBs removes the long-chain false near result while
        // retaining a conservative pad for radius, floor band, relief, and SmoothMax blend.
        const TArray<FVector>& ChainPoints = bHasWorldChain
            ? Tunnel.WorldControlPoints : Tunnel.ControlPoints;
        const TArray<float>& ChainRadii = bHasWorldChain
            ? Tunnel.WorldControlRadii : Tunnel.ControlRadii;
        bool bHasTightChain = ChainPoints.Num() >= 2
            && ChainRadii.Num() == ChainPoints.Num();
        bool bTightTouch = false;
        if (bHasTightChain)
        {
            float MaxRadius = 0.0f;
            for (int32 Index = 0; Index < ChainPoints.Num(); ++Index)
            {
                const FVector& Point = ChainPoints[Index];
                if (!VoxelMath::IsFinite(Point.X) || !VoxelMath::IsFinite(Point.Y)
                    || !VoxelMath::IsFinite(Point.Z)
                    || !VoxelMath::IsFinite(ChainRadii[Index]))
                {
                    bHasTightChain = false;
                    break;
                }
                MaxRadius = FMath::Max(MaxRadius, FMath::Abs(ChainRadii[Index]));
            }
            if (bHasTightChain)
            {
                float ProfileFloorDelta = 0.0f;
                const TArray<FTunnelFloorSegmentProfile>& Profiles = bHasWorldChain
                    ? Tunnel.WorldFloorProfiles : Tunnel.FloorProfiles;
                if (Profiles.Num() == ChainPoints.Num() - 1)
                {
                    for (int32 SegmentIndex = 0;
                         SegmentIndex < Profiles.Num(); ++SegmentIndex)
                    {
                        const FTunnelFloorSegmentProfile& Profile = Profiles[SegmentIndex];
                        if (!VoxelMath::IsFinite(Profile.StartFloorZ)
                            || !VoxelMath::IsFinite(Profile.EndFloorZ))
                        {
                            bHasTightChain = false;
                            break;
                        }
                        const float NaturalA = static_cast<float>(
                            ChainPoints[SegmentIndex].Z)
                            - FMath::Abs(ChainRadii[SegmentIndex]);
                        const float NaturalB = static_cast<float>(
                            ChainPoints[SegmentIndex + 1].Z)
                            - FMath::Abs(ChainRadii[SegmentIndex + 1]);
                        ProfileFloorDelta = FMath::Max(
                            ProfileFloorDelta,
                            FMath::Max(
                                FMath::Abs(Profile.StartFloorZ - NaturalA),
                                FMath::Abs(Profile.EndFloorZ - NaturalB)));
                    }
                }
                if (bHasTightChain)
                {
                    const float FloorPad = FMath::Max(
                        VoxelPassageGeometry::LandingFloorThicknessVoxels,
                        FMath::Max(Cache.SDFBlendRadius, 0.0f) * 0.35f);
                    const float SegmentPad = MaxRadius + FloorPad
                        + ProfileFloorDelta + ReliefBound;
                    if (!VoxelMath::IsFinite(SegmentPad))
                    {
                        return true;
                    }
                    for (int32 SegmentIndex = 0;
                         SegmentIndex + 1 < ChainPoints.Num(); ++SegmentIndex)
                    {
                        const FVector Min(
                            FMath::Min(ChainPoints[SegmentIndex].X,
                                       ChainPoints[SegmentIndex + 1].X),
                            FMath::Min(ChainPoints[SegmentIndex].Y,
                                       ChainPoints[SegmentIndex + 1].Y),
                            FMath::Min(ChainPoints[SegmentIndex].Z,
                                       ChainPoints[SegmentIndex + 1].Z));
                        const FVector Max(
                            FMath::Max(ChainPoints[SegmentIndex].X,
                                       ChainPoints[SegmentIndex + 1].X),
                            FMath::Max(ChainPoints[SegmentIndex].Y,
                                       ChainPoints[SegmentIndex + 1].Y),
                            FMath::Max(ChainPoints[SegmentIndex].Z,
                                       ChainPoints[SegmentIndex + 1].Z));
                        if (TouchesExpandedAabb(Min, Max, SegmentPad * ReachScale))
                        {
                            bTightTouch = true;
                            break;
                        }
                    }

                    if (!bTightTouch && bHasWorldChain
                        && Tunnel.bHasFloorRoomOwnership)
                    {
                        const float MouthHorizontal = MouthReach;
                        const float MouthVertical = FMath::Max(
                            MaxRadius + VoxelPassageGeometry::LandingFloorThicknessVoxels
                                + ReliefBound,
                            MouthFloorDelta + VoxelPassageGeometry::LandingFloorThicknessVoxels
                                + ReliefBound);
                        const float ScaledHorizontal = MouthHorizontal * ReachScale;
                        const float ScaledVertical = MouthVertical * ReachScale;
                        for (int32 Endpoint : {0, ChainPoints.Num() - 1})
                        {
                            const FVector& Point = ChainPoints[Endpoint];
                            if (TouchesExpandedAabb(
                                    FVector(
                                        static_cast<float>(Point.X) - ScaledHorizontal,
                                        static_cast<float>(Point.Y) - ScaledHorizontal,
                                        static_cast<float>(Point.Z) - ScaledVertical),
                                    FVector(
                                        static_cast<float>(Point.X) + ScaledHorizontal,
                                        static_cast<float>(Point.Y) + ScaledHorizontal,
                                        static_cast<float>(Point.Z) + ScaledVertical),
                                    0.0f))
                            {
                                bTightTouch = true;
                                break;
                            }
                        }
                    }
                }
            }
        }
        if (bHasTightChain && !bTightTouch)
        {
            continue;
        }

        // The broad sphere already encloses the swept radius and finite floor band.  Add the
        // vertical relief and room-mouth ownership envelopes before asking whether the lattice
        // box can touch it.  A miss is therefore an identity proof for the core evaluator and
        // its support/room-floor tail; its only field writes are bRoomFloor or
        // CoreSDF < -inset, all of which are inside this envelope.
        const float Reach = (
            FMath::Sqrt(FMath::Max(BoundRadiusSq, 0.0f))
            + ReliefBound + MouthReach + MouthFloorDelta) * ReachScale;
        if (!VoxelMath::IsFinite(Reach)
            || TouchesSphere(BoundCenter, Reach))
        {
            return true;
        }
    }
    return false;
}

float VoxelCaveMorphology::EvaluateTunnelCoreWorldSDF(
    float WorldX, float WorldY, float WorldZ,
    const FChunkSDFCache& Cache,
    bool bUseSpatialIndex)
{
    return EvaluateTunnelCoreWorld(
        WorldX, WorldY, WorldZ, Cache, nullptr, bUseSpatialIndex).SDF;
}

//=============================================================================
// CONVENIENCE WRAPPER (backward compatible)
//=============================================================================
// Builds a temporary cache for a single point, then evaluates.
// For chunk generation, use BuildChunkCache + EvaluateSDFCached directly.
// The search box around the point only needs a MaxInfluence margin — BuildChunkCache
// internally widens the COLLECT region to (2*MaxTunnelLength + MaxInfluence) so the
// graph it builds is the same one the chunk path would build at this point.

