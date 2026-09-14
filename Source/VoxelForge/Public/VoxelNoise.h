// VoxelNoise.h
// Float, SIMD-batched gradient-noise core (T2.a). Replaces UE's double-precision
// FMath::PerlinNoise3D on the density hot path (~6.6 ms/chunk was all noise math).
//
// WHY THIS EXISTS
//   FMath::PerlinNoise3D is double-precision with a permutation-table lookup. The
//   density field calls it >1M times per surface chunk (fBm octaves + domain warps).
//   This core is:
//     - FLOAT (no double math),
//     - table-free hash-gradient Perlin (pure arithmetic → vectorizes cleanly),
//     - SIMD-BATCHED across fBm octaves: one FractalNoise/Ridged call evaluates up to
//       4 octaves' Perlin samples in one 4-wide SSE pass.
//
//   It is a DIFFERENT noise field than FMath's, so worlds re-tune ONCE (accepted).
//
// DETERMINISM / CACHES
//   Perlin3D is a pure function of (x,y,z) exactly like the old call — every box-validity
//   cache (SDF §8.10, biome, surface column) stays valid. No invariant changes.
//
// SCALAR vs SIMD
//   Perlin3D (scalar) and Perlin3D_x4 (SSE) use the IDENTICAL formula, op-for-op, so on
//   x86 (SSE math == scalar-float math, same IEEE rounding) they produce bit-identical
//   results. The SSE path is therefore a pure speedup with no second re-tune. If it ever
//   fails to build on a given toolchain, force the scalar fallback with one line:
//       #define VF_NOISE_USE_SIMD 0   // (before including this header, or here)
//   The scalar path alone is still a large win over the old double-precision core.
//
// REQUIRES: SSE4.1 for the SIMD path (UE5 x64 baseline is SSE4.2 → always available).

#pragma once

#include "CoreMinimal.h"
#include "VoxelTypes.h"

#if !defined(VF_NOISE_USE_SIMD)
    #if PLATFORM_ENABLE_VECTORINTRINSICS && PLATFORM_CPU_X86_FAMILY
        #define VF_NOISE_USE_SIMD 1
    #else
        #define VF_NOISE_USE_SIMD 0
    #endif
#endif

#if VF_NOISE_USE_SIMD
    #include <immintrin.h>   // SSE4.1: _mm_floor_ps / _mm_mullo_epi32 / _mm_blendv_ps
#endif

namespace VoxelNoise
{
namespace Detail
{
    // Table-free integer hash of a lattice corner → gradient selector. Pure mul/xor/shift
    // so it vectorizes 1:1 (see the SSE Hash lambda below — must stay in lock-step).
    FORCEINLINE uint32 HashCorner(int32 ix, int32 iy, int32 iz)
    {
        uint32 h = (uint32)ix * 0x9E3779B1u
                 ^ (uint32)iy * 0x85EBCA77u
                 ^ (uint32)iz * 0xC2B2AE3Du;
        h ^= h >> 15; h *= 0x2C1B3C6Du;
        h ^= h >> 12; h *= 0x297A2D39u;
        h ^= h >> 15;
        return h;
    }

    // Quintic fade 6t^5-15t^4+10t^3, factored as t^3 * (t*(6t-15)+10) so the SSE twin
    // can mirror the exact grouping.
    FORCEINLINE float Fade(float t)
    {
        const float inner = t * (t * 6.0f - 15.0f) + 10.0f;
        const float t3 = t * t * t;
        return t3 * inner;
    }

    // Ken Perlin's 12-gradient dot (hash&15 picks the gradient). Branchy form here;
    // the SSE twin reproduces it with selects.
    FORCEINLINE float GradDot(uint32 hash, float x, float y, float z)
    {
        const uint32 h = hash & 15u;
        const float u = (h & 8u) == 0u ? x : y;
        float v;
        if      ((h & 12u) == 0u)  v = y;   // h < 4
        else if ((h & 13u) == 12u) v = x;   // h == 12 or 14
        else                       v = z;
        const float ru = (h & 1u) == 0u ? u : -u;
        const float rv = (h & 2u) == 0u ? v : -v;
        return ru + rv;
    }

    FORCEINLINE float Lerp(float a, float b, float t) { return a + t * (b - a); }
}

// Single 3D Perlin sample, ~[-1,1] (typically [-0.7,0.7], same character as the old core
// so VOXEL_NOISE_SCALE still applies). Used for all single-sample domain warps.
FORCEINLINE float Perlin3D(float x, float y, float z)
{
    using namespace Detail;
    const float xf = FMath::FloorToFloat(x);
    const float yf = FMath::FloorToFloat(y);
    const float zf = FMath::FloorToFloat(z);
    const int32 X = (int32)xf, Y = (int32)yf, Z = (int32)zf;
    const float fx = x - xf, fy = y - yf, fz = z - zf;
    const float su = Fade(fx), sv = Fade(fy), sw = Fade(fz);

    const uint32 h000 = HashCorner(X,   Y,   Z  );
    const uint32 h100 = HashCorner(X+1, Y,   Z  );
    const uint32 h010 = HashCorner(X,   Y+1, Z  );
    const uint32 h110 = HashCorner(X+1, Y+1, Z  );
    const uint32 h001 = HashCorner(X,   Y,   Z+1);
    const uint32 h101 = HashCorner(X+1, Y,   Z+1);
    const uint32 h011 = HashCorner(X,   Y+1, Z+1);
    const uint32 h111 = HashCorner(X+1, Y+1, Z+1);

    const float fx1 = fx - 1.0f, fy1 = fy - 1.0f, fz1 = fz - 1.0f;
    const float n000 = GradDot(h000, fx,  fy,  fz );
    const float n100 = GradDot(h100, fx1, fy,  fz );
    const float n010 = GradDot(h010, fx,  fy1, fz );
    const float n110 = GradDot(h110, fx1, fy1, fz );
    const float n001 = GradDot(h001, fx,  fy,  fz1);
    const float n101 = GradDot(h101, fx1, fy,  fz1);
    const float n011 = GradDot(h011, fx,  fy1, fz1);
    const float n111 = GradDot(h111, fx1, fy1, fz1);

    const float x00 = Lerp(n000, n100, su);
    const float x10 = Lerp(n010, n110, su);
    const float x01 = Lerp(n001, n101, su);
    const float x11 = Lerp(n011, n111, su);
    const float y0  = Lerp(x00, x10, sv);
    const float y1  = Lerp(x01, x11, sv);
    return Lerp(y0, y1, sw);
}

FORCEINLINE float Perlin3D(const FVector& P)
{
    return Perlin3D((float)P.X, (float)P.Y, (float)P.Z);
}

// Hot density callers already own float voxel coordinates.  Keeping this overload separate from
// FVector avoids constructing UE5's double-precision FVector merely to round the coordinates back
// to float at the noise entry point.  It is the same scalar formula and remains deterministic on
// every build of the module.
FORCEINLINE float Perlin3D(const FVector3f& P)
{
    return Perlin3D(P.X, P.Y, P.Z);
}

//=============================================================================
// LIPSCHITZ BOUND FOR THE ACTUAL 3D FIELD
//=============================================================================
// This is deliberately derived from the implementation above, rather than borrowed from a
// textbook Perlin constant. GradDot's h&15 table has the twelve UNNORMALIZED gradients
//
//   (±1,±1,0), (±1,0,±1), (0,±1,±1)
//
// (the four duplicate selectors do not add any vectors). For one partial derivative, write the
// four x-edges of the trilinear interpolation as independent gradient pairs. With
// d = Fade'(x) = 30 x²(1-x)², each edge contributes the support of the actual gradient set at
//
//   c0 = (1-x-dx, -dy, -dz),   c1 = (x-d(1-x), dy, dz).
//
// The support is max(|a|+|b|, |a|+|c|, |b|+|c|), because every allowed gradient has exactly two
// non-zero ±1 components. Taking the four y/z edge weights and the exact fade derivative, a
// finite case split over those three support branches has supremum 15/4 on [0,1]^3 (attained at
// x=y=z=1/2). This maximization permits every corner hash to choose any member of the actual
// set, so it is an upper bound for every hash cell; it is also the supremum of that finite support
// relaxation, not the looser 1 + 4*max(Fade') = 8.5 bound used by the unrelated 2D floor relief.
//
// The vector bound below follows the same per-axis-bound × sqrt(dimension) method used by the
// floor-relief proof. Abs is 1-Lipschitz and VOXEL_NOISE_SCALE is positive, so the scaled N1
// field has this bound in noise-space coordinates. WormBlockSkip computes its radius after the
// anisotropic world->noise transform, so the HBias and VerticalScale factors are included there.
static constexpr double Perlin3DPartialAbsBound = 15.0 / 4.0;
static constexpr double Perlin3DGradientBound =
    Perlin3DPartialAbsBound * 1.732050807568877293527446341505872L;
static constexpr double WormN1NoiseSpaceLipschitzBound =
    static_cast<double>(VOXEL_NOISE_SCALE) * Perlin3DGradientBound;

// A four-point side gives the natural small worker-local blocks for the mesher's 35-point
// (32 cells + one-sample normal halo) LOD0 lattice. The cache is only an optimization; a failed
// proof falls through to the original per-sample N1/N2 code.
static constexpr int32 WormBlockSampleSide = 4;

// The center value is evaluated with the same float Perlin expression as N1. The endpoint noise
// coordinates are already float results of monotone positive/negative affine transforms, so their
// min/max box contains every float lattice sample exactly. The 1e-3 scaled-N1 margin covers the
// remaining center-evaluation/interpolation rounding with substantial headroom; the proof remains
// strict because both the Lipschitz term and this margin are subtracted before comparing with T.
static constexpr double WormBlockOutputRoundMargin = 1.0e-3;

FORCEINLINE bool ProvesScaledAbsPerlin3DAbove(
    const FVector3f& CenterNoisePosition,
    const FVector3f& MinNoisePosition,
    const FVector3f& MaxNoisePosition,
    float Threshold,
    double ScaledFieldLipschitzBound)
{
    if (!FMath::IsFinite(CenterNoisePosition.X)
        || !FMath::IsFinite(CenterNoisePosition.Y)
        || !FMath::IsFinite(CenterNoisePosition.Z)
        || !FMath::IsFinite(MinNoisePosition.X)
        || !FMath::IsFinite(MinNoisePosition.Y)
        || !FMath::IsFinite(MinNoisePosition.Z)
        || !FMath::IsFinite(MaxNoisePosition.X)
        || !FMath::IsFinite(MaxNoisePosition.Y)
        || !FMath::IsFinite(MaxNoisePosition.Z)
        || !FMath::IsFinite(Threshold)
        || !(Threshold > 0.0f)
        || !FMath::IsFinite(ScaledFieldLipschitzBound)
        || !(ScaledFieldLipschitzBound > 0.0))
    {
        return false;
    }

    const double Dx = FMath::Max(
        FMath::Abs(static_cast<double>(MinNoisePosition.X)
                   - static_cast<double>(CenterNoisePosition.X)),
        FMath::Abs(static_cast<double>(MaxNoisePosition.X)
                   - static_cast<double>(CenterNoisePosition.X)));
    const double Dy = FMath::Max(
        FMath::Abs(static_cast<double>(MinNoisePosition.Y)
                   - static_cast<double>(CenterNoisePosition.Y)),
        FMath::Abs(static_cast<double>(MaxNoisePosition.Y)
                   - static_cast<double>(CenterNoisePosition.Y)));
    const double Dz = FMath::Max(
        FMath::Abs(static_cast<double>(MinNoisePosition.Z)
                   - static_cast<double>(CenterNoisePosition.Z)),
        FMath::Abs(static_cast<double>(MaxNoisePosition.Z)
                   - static_cast<double>(CenterNoisePosition.Z)));
    const double Radius = FMath::Sqrt(Dx * Dx + Dy * Dy + Dz * Dz);

    // Keep this operation in float and in the same order as the production N1 expression. The
    // double conversion is only for the conservative comparison below.
    const float CenterN1 = FMath::Abs(
        Perlin3D(CenterNoisePosition) * VOXEL_NOISE_SCALE);
    if (!FMath::IsFinite(CenterN1))
    {
        return false;
    }

    return static_cast<double>(CenterN1)
        - ScaledFieldLipschitzBound * Radius
        >= static_cast<double>(Threshold) + WormBlockOutputRoundMargin;
}

//=============================================================================
// 4-WIDE BATCH — the SIMD multiplier. Computes 4 independent Perlin samples.
// Inputs are 4-element arrays; unused lanes must be zero-filled by the caller
// (FBM/Ridged below do). Results written to Out[0..3].
//=============================================================================
#if VF_NOISE_USE_SIMD
FORCEINLINE void Perlin3D_x4(const float* Xs, const float* Ys, const float* Zs, float* Out)
{
    const __m128 x = _mm_loadu_ps(Xs);
    const __m128 y = _mm_loadu_ps(Ys);
    const __m128 z = _mm_loadu_ps(Zs);

    const __m128 xf = _mm_floor_ps(x);
    const __m128 yf = _mm_floor_ps(y);
    const __m128 zf = _mm_floor_ps(z);

    const __m128i X = _mm_cvttps_epi32(xf);
    const __m128i Y = _mm_cvttps_epi32(yf);
    const __m128i Z = _mm_cvttps_epi32(zf);

    const __m128 fx = _mm_sub_ps(x, xf);
    const __m128 fy = _mm_sub_ps(y, yf);
    const __m128 fz = _mm_sub_ps(z, zf);

    const __m128 c6  = _mm_set1_ps(6.0f);
    const __m128 c15 = _mm_set1_ps(15.0f);
    const __m128 c10 = _mm_set1_ps(10.0f);
    auto Fade = [&](const __m128 t) -> __m128
    {
        const __m128 inner = _mm_add_ps(_mm_mul_ps(t, _mm_sub_ps(_mm_mul_ps(t, c6), c15)), c10);
        const __m128 t3    = _mm_mul_ps(_mm_mul_ps(t, t), t);
        return _mm_mul_ps(t3, inner);
    };
    const __m128 su = Fade(fx);
    const __m128 sv = Fade(fy);
    const __m128 sw = Fade(fz);

    const __m128i one = _mm_set1_epi32(1);
    const __m128i X1 = _mm_add_epi32(X, one);
    const __m128i Y1 = _mm_add_epi32(Y, one);
    const __m128i Z1 = _mm_add_epi32(Z, one);

    const __m128i k1 = _mm_set1_epi32((int32)0x9E3779B1u);
    const __m128i k2 = _mm_set1_epi32((int32)0x85EBCA77u);
    const __m128i k3 = _mm_set1_epi32((int32)0xC2B2AE3Du);
    const __m128i m1 = _mm_set1_epi32((int32)0x2C1B3C6Du);
    const __m128i m2 = _mm_set1_epi32((int32)0x297A2D39u);
    auto Hash = [&](const __m128i ix, const __m128i iy, const __m128i iz) -> __m128i
    {
        __m128i h = _mm_xor_si128(_mm_xor_si128(_mm_mullo_epi32(ix, k1),
                                                _mm_mullo_epi32(iy, k2)),
                                  _mm_mullo_epi32(iz, k3));
        h = _mm_xor_si128(h, _mm_srli_epi32(h, 15)); h = _mm_mullo_epi32(h, m1);
        h = _mm_xor_si128(h, _mm_srli_epi32(h, 12)); h = _mm_mullo_epi32(h, m2);
        h = _mm_xor_si128(h, _mm_srli_epi32(h, 15));
        return h;
    };

    const __m128 fx1 = _mm_sub_ps(fx, _mm_set1_ps(1.0f));
    const __m128 fy1 = _mm_sub_ps(fy, _mm_set1_ps(1.0f));
    const __m128 fz1 = _mm_sub_ps(fz, _mm_set1_ps(1.0f));

    const __m128i i8    = _mm_set1_epi32(8);
    const __m128i i12   = _mm_set1_epi32(12);
    const __m128i i13   = _mm_set1_epi32(13);
    const __m128i i15   = _mm_set1_epi32(15);
    const __m128i i1    = _mm_set1_epi32(1);
    const __m128i i2    = _mm_set1_epi32(2);
    const __m128i izero = _mm_setzero_si128();
    const __m128  sgn   = _mm_set1_ps(-0.0f);
    auto Grad = [&](const __m128i hash, const __m128 gx, const __m128 gy, const __m128 gz) -> __m128
    {
        const __m128i h = _mm_and_si128(hash, i15);
        // u = (h&8)==0 ? gx : gy
        const __m128 mU    = _mm_castsi128_ps(_mm_cmpeq_epi32(_mm_and_si128(h, i8), izero));
        const __m128 u     = _mm_blendv_ps(gy, gx, mU);
        // v = (h&12)==0 ? gy : ((h&13)==12 ? gx : gz)
        const __m128 mLt4  = _mm_castsi128_ps(_mm_cmpeq_epi32(_mm_and_si128(h, i12), izero));
        const __m128 m1214 = _mm_castsi128_ps(_mm_cmpeq_epi32(_mm_and_si128(h, i13), i12));
        const __m128 vTmp  = _mm_blendv_ps(gz, gx, m1214);
        const __m128 v     = _mm_blendv_ps(vTmp, gy, mLt4);
        // signs: (h&1)? -u:u  + (h&2)? -v:v
        const __m128 mNegU = _mm_castsi128_ps(_mm_cmpeq_epi32(_mm_and_si128(h, i1), i1));
        const __m128 mNegV = _mm_castsi128_ps(_mm_cmpeq_epi32(_mm_and_si128(h, i2), i2));
        const __m128 ru    = _mm_blendv_ps(u, _mm_xor_ps(u, sgn), mNegU);
        const __m128 rv    = _mm_blendv_ps(v, _mm_xor_ps(v, sgn), mNegV);
        return _mm_add_ps(ru, rv);
    };

    const __m128 n000 = Grad(Hash(X,  Y,  Z ), fx,  fy,  fz );
    const __m128 n100 = Grad(Hash(X1, Y,  Z ), fx1, fy,  fz );
    const __m128 n010 = Grad(Hash(X,  Y1, Z ), fx,  fy1, fz );
    const __m128 n110 = Grad(Hash(X1, Y1, Z ), fx1, fy1, fz );
    const __m128 n001 = Grad(Hash(X,  Y,  Z1), fx,  fy,  fz1);
    const __m128 n101 = Grad(Hash(X1, Y,  Z1), fx1, fy,  fz1);
    const __m128 n011 = Grad(Hash(X,  Y1, Z1), fx,  fy1, fz1);
    const __m128 n111 = Grad(Hash(X1, Y1, Z1), fx1, fy1, fz1);

    auto Lerp = [&](const __m128 a, const __m128 b, const __m128 t) -> __m128
    {
        return _mm_add_ps(a, _mm_mul_ps(t, _mm_sub_ps(b, a)));
    };
    const __m128 x00 = Lerp(n000, n100, su);
    const __m128 x10 = Lerp(n010, n110, su);
    const __m128 x01 = Lerp(n001, n101, su);
    const __m128 x11 = Lerp(n011, n111, su);
    const __m128 y0  = Lerp(x00, x10, sv);
    const __m128 y1  = Lerp(x01, x11, sv);
    _mm_storeu_ps(Out, Lerp(y0, y1, sw));
}
#else
FORCEINLINE void Perlin3D_x4(const float* Xs, const float* Ys, const float* Zs, float* Out)
{
    for (int32 i = 0; i < 4; ++i) Out[i] = Perlin3D(Xs[i], Ys[i], Zs[i]);
}
#endif

//=============================================================================
// fBm / Ridged — octaves evaluated 4 at a time through Perlin3D_x4.
// Accumulation stays scalar in octave order, so the result is independent of
// whether the SIMD or scalar Perlin3D_x4 is used (bit-identical either way).
//=============================================================================
FORCEINLINE float FBM(float x, float y, float z,
                       int32 Octaves = 4, float Lacunarity = 2.0f, float Persistence = 0.5f)
{
    float Total = 0.0f, MaxValue = 0.0f, Freq = 1.0f, Amp = 1.0f;
    for (int32 o = 0; o < Octaves; )
    {
        const int32 N = FMath::Min(4, Octaves - o);
        float Xs[4] = {}, Ys[4] = {}, Zs[4] = {}, Out[4];
        float f = Freq;
        for (int32 i = 0; i < N; ++i) { Xs[i] = x * f; Ys[i] = y * f; Zs[i] = z * f; f *= Lacunarity; }
        Perlin3D_x4(Xs, Ys, Zs, Out);
        for (int32 i = 0; i < N; ++i) { Total += Out[i] * Amp; MaxValue += Amp; Amp *= Persistence; }
        Freq = f;
        o += N;
    }
    return Total / MaxValue;
}

FORCEINLINE float Ridged(float x, float y, float z,
                         int32 Octaves = 4, float Lacunarity = 2.0f, float Persistence = 0.5f)
{
    // Matches the original RidgedNoise3D fold exactly (NS scale, square, weight feedback).
    static constexpr float NS = 1.25f;
    float Total = 0.0f, MaxValue = 0.0f, Freq = 1.0f, Amp = 1.0f, Weight = 1.0f;
    for (int32 o = 0; o < Octaves; )
    {
        const int32 N = FMath::Min(4, Octaves - o);
        float Xs[4] = {}, Ys[4] = {}, Zs[4] = {}, Out[4];
        float f = Freq;
        for (int32 i = 0; i < N; ++i) { Xs[i] = x * f; Ys[i] = y * f; Zs[i] = z * f; f *= Lacunarity; }
        Perlin3D_x4(Xs, Ys, Zs, Out);
        for (int32 i = 0; i < N; ++i)
        {
            float Nn = Out[i] * NS;
            Nn = 1.0f - FMath::Abs(Nn);   // fold → ridge at zero-crossings
            Nn = Nn * Nn;                 // sharpen
            Nn *= Weight;                 // detail follows previous ridge
            Weight = FMath::Clamp(Nn * 2.0f, 0.0f, 1.0f);
            Total += Nn * Amp; MaxValue += Amp; Amp *= Persistence;
        }
        Freq = f;
        o += N;
    }
    return (Total / MaxValue) * 2.0f - 1.0f;
}

} // namespace VoxelNoise
