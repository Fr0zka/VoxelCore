// VoxelGenerator.cpp
// Champ de densité du monde. Toute la géométrie des grottes sort d'ici.
//
// Convention INTERNE: positif = solide, négatif = air (plus lisible).
// Convention de SORTIE (marching cubes): on négate → négatif = solide.

#include "VoxelGenerator.h"
#include "VoxelSettings.h"
#include "VoxelStrateManager.h"
#include "VoxelStrateDefinition.h"
#include "VoxelTerrainOpDefinition.h"
#include "VoxelCaveMorphology.h"
#include "VoxelDiffLayer.h"
#include "VoxelBiomeDefinition.h"
#include "VoxelNoise.h"   // T2.a: float, SIMD-batched gradient-noise core
#include "VoxelWormField.h"
#include "VoxelDensityAblation.h" // fingerprinted development-only stage measurements
#include "VoxelDensityPrimitives.h"   // spine / seals / passage — shared with the operator stack
#include "VoxelDensityOpStack.h"      // OPSTACK Phase 1: the opt-in per-strate operator stack
#include "VoxelHeightOp.h"            // IVoxelBiomeField — the adapter below implements it
#include "VoxelStats.h"
#include "VoxelDensityProfile.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "HAL/PlatformTime.h"
#include "HAL/IConsoleManager.h"
#include "Misc/CommandLine.h"

#if WITH_EDITOR
#include "VoxelStrateComposer.h"
#endif

#include <atomic>

namespace VoxelMath
{
    VOXELFORGE_API int32 GFastIsFinite = 1;
}

namespace
{
    // The default-on branch uses VoxelMath::IsFiniteFast, while 0 retains the original
    // FMath::IsFinite call for a real within-round A/B and sampler comparison.
    FAutoConsoleVariableRef CVarVoxelForgeFastIsFinite(
        TEXT("voxel.FastIsFinite"),
        VoxelMath::GFastIsFinite,
        TEXT("Use the exact inline IEEE-754 bit test instead of the CRT-backed FMath::IsFinite."));
    bool GVoxelForgeFastIsFiniteSwitchParsed = false;

    void VF_ParseFastIsFiniteSwitch()
    {
        if (GVoxelForgeFastIsFiniteSwitchParsed)
        {
            return;
        }
        GVoxelForgeFastIsFiniteSwitchParsed = true;
        int32 CommandLineValue = VoxelMath::GFastIsFinite;
        if (FParse::Value(
                FCommandLine::Get(), TEXT("voxel.FastIsFinite="), CommandLineValue))
        {
            VoxelMath::GFastIsFinite = CommandLineValue;
        }
        VoxelMath::GFastIsFinite = VoxelMath::GFastIsFinite != 0 ? 1 : 0;
    }

    // The canonical native TunnelNetwork/Underwater graph has a hand-lowered evaluator.  Keep a
    // command-line switch so every change can be A/B'd against the existing interpreted/block
    // path without changing the authored graph or rebuilding a different world definition.
    int32 GVoxelForgeUseFusedEvaluator = 1;
    FAutoConsoleVariableRef CVarVoxelForgeUseFusedEvaluator(
        TEXT("voxel.UseFusedEvaluator"),
        GVoxelForgeUseFusedEvaluator,
        TEXT("Use the lowered fused evaluator for canonical TunnelNetwork operator stacks."));
    bool GVoxelForgeFusedEvaluatorSwitchParsed = false;

    void VF_ParseFusedEvaluatorSwitch()
    {
        if (GVoxelForgeFusedEvaluatorSwitchParsed)
        {
            return;
        }
        GVoxelForgeFusedEvaluatorSwitchParsed = true;
        int32 CommandLineValue = GVoxelForgeUseFusedEvaluator;
        if (FParse::Value(
                FCommandLine::Get(), TEXT("voxel.UseFusedEvaluator="), CommandLineValue))
        {
            GVoxelForgeUseFusedEvaluator = CommandLineValue;
        }
    }

    // A proved N1-above-threshold block skips both worm noise calls.  The switch is parsed once
    // on the same scalar entry point as the other generation A/B switches; 0 is the exact legacy
    // path and remains the default until a tighter skip is justified by a future measurement.
    int32 GVoxelForgeWormBlockSkip = 0;
    FAutoConsoleVariableRef CVarVoxelForgeWormBlockSkip(
        TEXT("voxel.WormBlockSkip"),
        GVoxelForgeWormBlockSkip,
        TEXT("Skip worm N1/N2 when a worker-local noise-space block proof says N1 >= threshold."));
    bool GVoxelForgeWormBlockSkipSwitchParsed = false;

    void VF_ParseWormBlockSkipSwitch()
    {
        if (GVoxelForgeWormBlockSkipSwitchParsed)
        {
            return;
        }
        GVoxelForgeWormBlockSkipSwitchParsed = true;
        int32 CommandLineValue = GVoxelForgeWormBlockSkip;
        if (FParse::Value(
                FCommandLine::Get(), TEXT("voxel.WormBlockSkip="), CommandLineValue))
        {
            GVoxelForgeWormBlockSkip = CommandLineValue;
        }
        GVoxelForgeWormBlockSkip = GVoxelForgeWormBlockSkip != 0 ? 1 : 0;
    }

    // A tile-sized cache window is the production default on the coarse paths where it wins.
    // Keep the switches in the generator module so native and op-stack sources make the same A/B
    // decision, including when a headless harness applies command-line CVars after module startup.
    int32 GVoxelForgeTileCacheWindow = 1;
    FAutoConsoleVariableRef CVarVoxelForgeTileCacheWindow(
        TEXT("voxel.TileCacheWindow"),
        GVoxelForgeTileCacheWindow,
        TEXT("Use the requesting tile's aligned footprint plus the deterministic warp/halo margin for room caches."));
    bool GVoxelForgeTileCacheWindowSwitchParsed = false;
    // Refreshed profiles put the fused crossover at LOD3 and the op-stack crossover at LOD4.
    // Separate defaults keep a path that loses at LOD3 from paying for a larger candidate set.
    int32 GVoxelForgeTileCacheWindowMinLOD = 3;
    FAutoConsoleVariableRef CVarVoxelForgeTileCacheWindowMinLOD(
        TEXT("voxel.TileCacheWindowMinLOD"),
        GVoxelForgeTileCacheWindowMinLOD,
        TEXT("Minimum SampleStep LOD for the fused tile-sized room cache window."));
    int32 GVoxelForgeTileCacheWindowOpStackMinLOD = 4;
    FAutoConsoleVariableRef CVarVoxelForgeTileCacheWindowOpStackMinLOD(
        TEXT("voxel.TileCacheWindowOpStackMinLOD"),
        GVoxelForgeTileCacheWindowOpStackMinLOD,
        TEXT("Minimum SampleStep LOD for the op-stack tile-sized room cache window."));

    int32 GVoxelForgeSpatialIndex = 1;
    FAutoConsoleVariableRef CVarVoxelForgeSpatialIndex(
        TEXT("voxel.SpatialIndex"),
        GVoxelForgeSpatialIndex,
        TEXT("Room-cache spatial broad phase: -1 measured auto policy, 0 off, 1 on."));
    bool GVoxelForgeSpatialIndexSwitchParsed = false;

    void VF_ParseTileCacheWindowSwitch()
    {
        if (GVoxelForgeTileCacheWindowSwitchParsed)
        {
            return;
        }
        GVoxelForgeTileCacheWindowSwitchParsed = true;
        int32 CommandLineValue = GVoxelForgeTileCacheWindow;
        if (FParse::Value(
                FCommandLine::Get(), TEXT("voxel.TileCacheWindow="), CommandLineValue))
        {
            GVoxelForgeTileCacheWindow = CommandLineValue;
        }
        GVoxelForgeTileCacheWindow = GVoxelForgeTileCacheWindow != 0 ? 1 : 0;

        int32 MinLODCommandLineValue = GVoxelForgeTileCacheWindowMinLOD;
        if (FParse::Value(
                FCommandLine::Get(), TEXT("voxel.TileCacheWindowMinLOD="),
                MinLODCommandLineValue))
        {
            GVoxelForgeTileCacheWindowMinLOD = MinLODCommandLineValue;
        }
        GVoxelForgeTileCacheWindowMinLOD = FMath::Clamp(
            GVoxelForgeTileCacheWindowMinLOD, 0, 5);

        int32 OpStackMinLODCommandLineValue = GVoxelForgeTileCacheWindowOpStackMinLOD;
        if (FParse::Value(
                FCommandLine::Get(), TEXT("voxel.TileCacheWindowOpStackMinLOD="),
                OpStackMinLODCommandLineValue))
        {
            GVoxelForgeTileCacheWindowOpStackMinLOD = OpStackMinLODCommandLineValue;
        }
        GVoxelForgeTileCacheWindowOpStackMinLOD = FMath::Clamp(
            GVoxelForgeTileCacheWindowOpStackMinLOD, 0, 5);
    }

    void VF_ParseSpatialIndexSwitch()
    {
        if (GVoxelForgeSpatialIndexSwitchParsed)
        {
            return;
        }
        GVoxelForgeSpatialIndexSwitchParsed = true;
        int32 CommandLineValue = GVoxelForgeSpatialIndex;
        if (FParse::Value(
                FCommandLine::Get(), TEXT("voxel.SpatialIndex="), CommandLineValue))
        {
            GVoxelForgeSpatialIndex = CommandLineValue;
        }
        GVoxelForgeSpatialIndex = FMath::Clamp(GVoxelForgeSpatialIndex, -1, 1);
    }

}

//=============================================================================
// L'ADAPTATEUR DE CHAMP DE BIOMES / THE BIOME FIELD ADAPTER
//=============================================================================
// Il vit ICI, du côté qui connaît le générateur, et PAS dans la pile d'opérateurs. C'est tout
// l'intérêt de `IVoxelBiomeField` : le résolveur réel est une Voronoï warpée avec un cache par
// chunk sur `UVoxelGenerator`, et un opérateur qui tiendrait ce pointeur ne pourrait jamais devenir
// un asset (Phase 3). En le confinant ici, l'opérateur ne connaît qu'une capacité, pas un
// propriétaire.
//
// ⚠️ DURÉES DE VIE : cet adaptateur pointe vers les `thread_local` `CP_BiomeCtx` / `CP_BiomeCache`
// de `GetDensityAt`. Il est POSSÉDÉ par la pile, elle-même `thread_local` et reconstruite dans le
// MÊME bloc de refetch que ces deux caches — les trois naissent et meurent ensemble, sur le même
// thread. Un pointeur vers un thread_local depuis un objet thread_local du même bloc est sûr ;
// l'échapper ailleurs ne le serait pas.
//
// Lives here, not in the op stack: the real resolver is generator state, and an op holding that
// pointer could never become an asset. LIFETIME: it points at GetDensityAt's thread_locals and is
// owned by the stack, which is itself thread_local and rebuilt in the same refetch block.
class FGeneratorBiomeField final : public IVoxelBiomeField
{
public:
    FGeneratorBiomeField(const UVoxelGenerator* InGen, const FBiomeContext* InCtx,
                         FChunkBiomeCache* InCache, int32 InChunkZ)
        : Gen(InGen), Ctx(InCtx), Cache(InCache), ChunkZ(InChunkZ) {}

    FVoxelBiomeWeights SampleAt(float WorldX, float WorldY) const override
    {
        FVoxelBiomeWeights Out;
        if (!Gen || !Ctx || !Cache) { return Out; }   // dégradation sûre : biome 0 partout

        const FBiomeSample S = Gen->ResolveBiomeSampleAt(WorldX, WorldY, ChunkZ, *Ctx, *Cache);
        Out.Dominant       = FMath::Max(S.DominantIndex, 0);   // -1 ⇒ 0, comme le chemin d'origine
        Out.Neighbor       = S.NeighborIndex;
        Out.NeighborWeight = S.NeighborWeight;
        return Out;
    }

private:
    const UVoxelGenerator* Gen;
    const FBiomeContext*   Ctx;
    FChunkBiomeCache*      Cache;
    int32                  ChunkZ;
};

//=============================================================================
// SURFACE COLUMN CACHE (T1.a) — kill the per-Z heightfield redundancy
//=============================================================================
// The SurfaceWorld heightfield + sky-cap + biome blend are functions of XY ONLY, but
// the mesher samples ~33 Z grid-points per column, each re-running that XY work. We cache
// the column (terrain Z + ceiling Z) once per integer XY and reuse it down the column.
// Used ONLY for integer-XY queries (the density grid); fractional queries (gradient
// normals at interpolated vertices) fall through and compute directly → bit-identical.
// The surface heightfield (TerrainZ + sky-cap CeilSurf) is a PURE function of (worldX, worldY,
// seed, strate) — ZERO Z dependence: the climate/Voronoi fields are pure-XY (see ResolveBiomeSampleAt:
// "result is the pure function of XY either way") and surface params are constant within a strate (Hard
// transitions). So a column computed for one chunk is valid for EVERY chunk stacked above/below it in
// the same strate. This cache is therefore keyed by (XY box, StrateKey, Seed) — NOT ChunkZ — and held as
// a small LRU of boxes so the whole vertical view-distance stack (and XY neighbours interleaved by the
// task scheduler) reuse one another's heavy column noise instead of each recomputing it ~once per
// vertical chunk in view. Validity stays a world-XY BOX (chunk footprint + margin) so the ±1 gradient /
// +X/+Y boundary samples don't thrash it (same discipline as the SDF/biome caches, §8.10).
// F20 phase 2 OVERHANG, resolved once per column (biome-blended, slope-gated) so the per-voxel
// density path is cheap: OverhangAmp = strength · slope-gate (0 ⇒ no overhang here); (DirX,DirY) =
// the UNIT UPHILL direction of the terrain gradient (the lip extends downhill, borrowing rock from
// uphill). See SurfaceDensityFromColumn for the warped-terrain union that makes the shelf.
struct FSurfaceColumn
{
    // Toutes les sorties sont écrites par ComputeSurfaceColumn avant Computed=true ; les valeurs
    // par défaut ne sont donc jamais lues et leur constructeur implicite coûte dans le TLS.
    // Every output is written by ComputeSurfaceColumn before Computed=true; the defaults are never
    // read, and removing them keeps FSurfaceColumn trivial so TLS construction has no leaf work.
    float TerrainZ, CeilSurf;
    float OverhangAmp, DirX, DirY;
};

struct FSurfaceColumnBox
{
    // Covers a chunk footprint + a margin on every side (≥ the LOD margin ring the mesher samples for
    // grid-based normals). Symmetric around the first (allocating) sample so a whole chunk's column
    // queries — including the ±Step margin ring — stay inside one box.
    static constexpr int32 Halo = CHUNK_SIZE + 8;
    static constexpr int32 Dim  = 2 * Halo + 1;
    int32 BaseX = 0, BaseY = 0;                        // box origin (voxel coords)
    int32 StrateKey = MIN_int32, Seed = MIN_int32;     // key: same strate ⇒ identical heightfield params
    // AUDIT C2, étendu : StrateKey vaut round(StrateBottomWorldZ), donc une édition à chaud qui
    // change les params de terrain SANS déplacer la strate (fréquence de bruit, hauteur de
    // montagne, un biome…) laisse la clé identique et sert des colonnes périmées. C'est la forme
    // la plus visible du bug : « j'ai retouché le terrain et une zone a gardé l'ancienne forme ».
    // StrateKey is round(StrateBottomWorldZ), so a live edit that changes terrain params WITHOUT
    // moving the strate leaves the key unchanged and serves stale columns.
    uint32 LayoutVersion = 0xFFFFFFFFu;
    uint32 LastUse = 0;                                // LRU stamp
    bool  bValid = false;
    FSurfaceColumn Cols[Dim * Dim];
    bool  Computed[Dim * Dim];
};

struct FSurfaceColumnCache
{
    // A handful of boxes keeps the player's column + a few XY neighbours warm across interleaved tasks,
    // so vertical reuse survives whatever order the worker pulls chunks in. ~59 KB/box.
    static constexpr int32 NumBoxes = 6;
    FSurfaceColumnBox Boxes[NumBoxes];
    uint32 Clock = 0;

    // Return the box covering (IX,IY) for this (StrateKey,Seed,LayoutVersion); allocate by
    // evicting the LRU box on miss.
    FSurfaceColumnBox& Acquire(int32 IX, int32 IY, int32 InStrateKey, int32 InSeed, uint32 InLayoutVersion)
    {
        ++Clock;
        for (FSurfaceColumnBox& B : Boxes)
        {
            if (B.bValid && B.StrateKey == InStrateKey && B.Seed == InSeed
                && B.LayoutVersion == InLayoutVersion
                && IX >= B.BaseX && IX < B.BaseX + FSurfaceColumnBox::Dim
                && IY >= B.BaseY && IY < B.BaseY + FSurfaceColumnBox::Dim)
            {
                B.LastUse = Clock;
                return B;
            }
        }
        // Miss → reuse the least-recently-used box, recentred on this sample.
        FSurfaceColumnBox* Victim = &Boxes[0];
        for (FSurfaceColumnBox& B : Boxes) { if (B.LastUse < Victim->LastUse) Victim = &B; }
        Victim->BaseX         = IX - FSurfaceColumnBox::Halo;
        Victim->BaseY         = IY - FSurfaceColumnBox::Halo;
        Victim->StrateKey     = InStrateKey;
        Victim->Seed          = InSeed;
        Victim->LayoutVersion = InLayoutVersion;
        Victim->bValid        = true;
        Victim->LastUse       = Clock;
        FMemory::Memzero(Victim->Computed, sizeof(Victim->Computed));
        return *Victim;
    }
};

// Cache de colonnes PARTAGÉ par thread : GetDensityAt (hot path, T1.a) + ClassifyTile (T1.d).
// Les deux stockent des valeurs bit-identiques (même ComputeSurfaceColumn, champs purs en XY),
// donc un ClassifyTile qui rend Mixed laisse ses colonnes chaudes pour le GenerateMesh qui suit.
static thread_local FSurfaceColumnCache GSurfColCache;

//=============================================================================
// WORM N1 BLOCK PROOF (worker-local, scalar GetDensityAt hand-off)
//=============================================================================
// GenerateMesh installs TileOriginVoxels/SampleStep/TileCellsPerAxis around every density-grid
// fill.  Keep the verdicts here, rather than in the mesher, because the scalar GetDensityAt path
// owns the actual worm call and is also used by standalone/classifier queries.  The cache is
// thread_local, direct-indexed by the block coordinate, and invalidated by every field identity
// that can affect N1.  A miss or an invalid context always returns false and therefore executes
// the old N1/N2 code.
namespace
{
    constexpr int32 VF_WormBlockCacheAxis =
        (CHUNK_SIZE + 3 + VoxelNoise::WormBlockSampleSide - 1)
        / VoxelNoise::WormBlockSampleSide;
    constexpr int32 VF_WormBlockCacheEntryCount =
        VF_WormBlockCacheAxis * VF_WormBlockCacheAxis * VF_WormBlockCacheAxis;

    static_assert(VF_WormBlockCacheAxis == 9,
        "The fixed worm block table must cover the 32-cell mesher lattice plus its halo.");

    FORCEINLINE uint32 VF_FloatBits(float Value)
    {
        uint32 Bits = 0;
        FMemory::Memcpy(&Bits, &Value, sizeof(Bits));
        return Bits;
    }

    struct FWormBlockSkipEntry
    {
        bool bValid = false;
        bool bSkip = false;
    };

    struct FWormBlockSkipCache
    {
        bool bContextValid = false;
        uint64 OwnerId = 0;
        uint32 Seed = 0;
        uint32 ParamsFingerprint = 0xFFFFFFFFu;
        uint32 LayoutVersion = 0xFFFFFFFFu;
        FIntVector TileOrigin = FIntVector::ZeroValue;
        int32 Step = 0;
        int32 Cells = 0;
        uint32 WormFrequencyBits = 0;
        uint32 WormHorizontalBiasBits = 0;
        uint32 WormThresholdBits = 0;
        uint32 VerticalScaleBits = 0;
        FWormBlockSkipEntry Entries[VF_WormBlockCacheEntryCount];

        void SetContext(
            uint64 InOwnerId, uint32 InSeed, uint32 InParamsFingerprint,
            uint32 InLayoutVersion, const FIntVector& InTileOrigin,
            int32 InStep, int32 InCells, const FStrateGenerationParams& Params)
        {
            const uint32 InWormFrequencyBits = VF_FloatBits(Params.WormFrequency);
            const uint32 InWormHorizontalBiasBits = VF_FloatBits(Params.WormHorizontalBias);
            const uint32 InWormThresholdBits = VF_FloatBits(Params.WormThreshold);
            const uint32 InVerticalScaleBits = VF_FloatBits(Params.VerticalScale);
            const bool bChanged =
                !bContextValid
                || OwnerId != InOwnerId
                || Seed != InSeed
                || ParamsFingerprint != InParamsFingerprint
                || LayoutVersion != InLayoutVersion
                || TileOrigin != InTileOrigin
                || Step != InStep
                || Cells != InCells
                || WormFrequencyBits != InWormFrequencyBits
                || WormHorizontalBiasBits != InWormHorizontalBiasBits
                || WormThresholdBits != InWormThresholdBits
                || VerticalScaleBits != InVerticalScaleBits;
            if (!bChanged)
            {
                return;
            }

            bContextValid = true;
            OwnerId = InOwnerId;
            Seed = InSeed;
            ParamsFingerprint = InParamsFingerprint;
            LayoutVersion = InLayoutVersion;
            TileOrigin = InTileOrigin;
            Step = InStep;
            Cells = InCells;
            WormFrequencyBits = InWormFrequencyBits;
            WormHorizontalBiasBits = InWormHorizontalBiasBits;
            WormThresholdBits = InWormThresholdBits;
            VerticalScaleBits = InVerticalScaleBits;
            FMemory::Memzero(Entries, sizeof(Entries));
        }
    };

    static thread_local FWormBlockSkipCache GWormBlockSkipCache;

    FORCEINLINE FVector3f VF_WormN1NoisePosition(
        float WorldX, float WorldY, float WorldZ,
        const FStrateGenerationParams& Params, uint32 SeedU)
    {
        float EffectiveZ = WorldZ;
        if (Params.VerticalScale != 1.0f && Params.VerticalScale > 0.0f)
        {
            EffectiveZ = WorldZ / Params.VerticalScale;
        }
        const float WormZFreq = Params.WormFrequency * Params.WormHorizontalBias;
        return FVector3f(
            WorldX * Params.WormFrequency + VoxelHash::SeedOffset(SeedU, 1.0f),
            WorldY * Params.WormFrequency + VoxelHash::SeedOffset(SeedU, 1.7f),
            EffectiveZ * WormZFreq + VoxelHash::SeedOffset(SeedU, 2.3f));
    }

    FORCEINLINE bool VF_TryGetWormBlockSkip(
        uint64 OwnerId, uint32 SeedU, uint32 ParamsFingerprint, uint32 LayoutVersion,
        float WorldX, float WorldY, float WorldZ,
        const FStrateGenerationParams& Params, bool& bOutProofComputed)
    {
        bOutProofComputed = false;

        // Point queries and malformed authored parameters stay on the exact original path. In
        // particular, VerticalScale <= 0 has legacy "no division" semantics; declining the proof
        // here avoids making a new promise for malformed data.
        if (!FMath::IsFinite(WorldX) || !FMath::IsFinite(WorldY) || !FMath::IsFinite(WorldZ)
            || WorldX != FMath::FloorToFloat(WorldX)
            || WorldY != FMath::FloorToFloat(WorldY)
            || WorldZ != FMath::FloorToFloat(WorldZ)
            || !FMath::IsFinite(Params.WormFrequency)
            || !FMath::IsFinite(Params.WormHorizontalBias)
            || !FMath::IsFinite(Params.WormThreshold)
            || !FMath::IsFinite(Params.VerticalScale)
            || !(Params.WormThreshold > 0.0f)
            || !(Params.VerticalScale > 0.0f)
            || !FMath::IsFinite(Params.WormFrequency * Params.WormHorizontalBias))
        {
            return false;
        }

        const int32 Step = VoxelGenLOD::SampleStep;
        const int32 Cells = VoxelGenLOD::TileCellsPerAxis;
        if (Step <= 0 || Cells <= 0 || Cells > CHUNK_SIZE)
        {
            return false;
        }

        // FMath::RoundToInt is only reached after the finite/integer check and a range guard. The
        // tile context itself is int32, so coordinates outside this range cannot belong to it.
        constexpr float SafeInt32Min = -2147483008.0f;
        constexpr float SafeInt32Max =  2147483008.0f;
        if (WorldX < SafeInt32Min || WorldX > SafeInt32Max
            || WorldY < SafeInt32Min || WorldY > SafeInt32Max
            || WorldZ < SafeInt32Min || WorldZ > SafeInt32Max)
        {
            return false;
        }
        const int32 IX = FMath::RoundToInt(WorldX);
        const int32 IY = FMath::RoundToInt(WorldY);
        const int32 IZ = FMath::RoundToInt(WorldZ);
        const FIntVector TileOrigin = VoxelGenLOD::TileOriginVoxels;
        const int64 DX = static_cast<int64>(IX) - static_cast<int64>(TileOrigin.X);
        const int64 DY = static_cast<int64>(IY) - static_cast<int64>(TileOrigin.Y);
        const int64 DZ = static_cast<int64>(IZ) - static_cast<int64>(TileOrigin.Z);
        if (DX % Step != 0 || DY % Step != 0 || DZ % Step != 0)
        {
            return false;
        }

        const int32 LocalX = static_cast<int32>(DX / Step);
        const int32 LocalY = static_cast<int32>(DY / Step);
        const int32 LocalZ = static_cast<int32>(DZ / Step);
        const int32 MinLocalLattice = -1;
        const int32 MaxLocalLattice = Cells + 1;
        if (LocalX < MinLocalLattice || LocalX > MaxLocalLattice
            || LocalY < MinLocalLattice || LocalY > MaxLocalLattice
            || LocalZ < MinLocalLattice || LocalZ > MaxLocalLattice)
        {
            return false;
        }

        const int32 BlockX = (LocalX + 1) / VoxelNoise::WormBlockSampleSide;
        const int32 BlockY = (LocalY + 1) / VoxelNoise::WormBlockSampleSide;
        const int32 BlockZ = (LocalZ + 1) / VoxelNoise::WormBlockSampleSide;
        if (BlockX < 0 || BlockX >= VF_WormBlockCacheAxis
            || BlockY < 0 || BlockY >= VF_WormBlockCacheAxis
            || BlockZ < 0 || BlockZ >= VF_WormBlockCacheAxis)
        {
            return false;
        }

        GWormBlockSkipCache.SetContext(
            OwnerId, SeedU, ParamsFingerprint, LayoutVersion, TileOrigin, Step, Cells, Params);
        FWormBlockSkipEntry& Entry = GWormBlockSkipCache.Entries[
            (BlockZ * VF_WormBlockCacheAxis + BlockY) * VF_WormBlockCacheAxis + BlockX];
        if (Entry.bValid)
        {
            return Entry.bSkip;
        }

        const int32 BlockMinX = BlockX * VoxelNoise::WormBlockSampleSide - 1;
        const int32 BlockMinY = BlockY * VoxelNoise::WormBlockSampleSide - 1;
        const int32 BlockMinZ = BlockZ * VoxelNoise::WormBlockSampleSide - 1;
        const int32 BlockMaxX = FMath::Min(MaxLocalLattice,
            BlockMinX + VoxelNoise::WormBlockSampleSide - 1);
        const int32 BlockMaxY = FMath::Min(MaxLocalLattice,
            BlockMinY + VoxelNoise::WormBlockSampleSide - 1);
        const int32 BlockMaxZ = FMath::Min(MaxLocalLattice,
            BlockMinZ + VoxelNoise::WormBlockSampleSide - 1);

        const auto WorldAt = [Step, &TileOrigin](int32 Local, int32 Origin) -> int64
        {
            return static_cast<int64>(Origin) + static_cast<int64>(Local) * Step;
        };
        const int64 MinWorldX64 = WorldAt(BlockMinX, TileOrigin.X);
        const int64 MinWorldY64 = WorldAt(BlockMinY, TileOrigin.Y);
        const int64 MinWorldZ64 = WorldAt(BlockMinZ, TileOrigin.Z);
        const int64 MaxWorldX64 = WorldAt(BlockMaxX, TileOrigin.X);
        const int64 MaxWorldY64 = WorldAt(BlockMaxY, TileOrigin.Y);
        const int64 MaxWorldZ64 = WorldAt(BlockMaxZ, TileOrigin.Z);
        if (MinWorldX64 < MIN_int32 || MinWorldX64 > MAX_int32
            || MinWorldY64 < MIN_int32 || MinWorldY64 > MAX_int32
            || MinWorldZ64 < MIN_int32 || MinWorldZ64 > MAX_int32
            || MaxWorldX64 < MIN_int32 || MaxWorldX64 > MAX_int32
            || MaxWorldY64 < MIN_int32 || MaxWorldY64 > MAX_int32
            || MaxWorldZ64 < MIN_int32 || MaxWorldZ64 > MAX_int32)
        {
            return false;
        }

        const FVector3f Q0 = VF_WormN1NoisePosition(
            static_cast<float>(static_cast<int32>(MinWorldX64)),
            static_cast<float>(static_cast<int32>(MinWorldY64)),
            static_cast<float>(static_cast<int32>(MinWorldZ64)), Params, SeedU);
        const FVector3f Q1 = VF_WormN1NoisePosition(
            static_cast<float>(static_cast<int32>(MaxWorldX64)),
            static_cast<float>(static_cast<int32>(MaxWorldY64)),
            static_cast<float>(static_cast<int32>(MaxWorldZ64)), Params, SeedU);
        const FVector3f QMin(
            FMath::Min(Q0.X, Q1.X), FMath::Min(Q0.Y, Q1.Y), FMath::Min(Q0.Z, Q1.Z));
        const FVector3f QMax(
            FMath::Max(Q0.X, Q1.X), FMath::Max(Q0.Y, Q1.Y), FMath::Max(Q0.Z, Q1.Z));
        const FVector3f QCenter(
            static_cast<float>((static_cast<double>(QMin.X) + static_cast<double>(QMax.X)) * 0.5),
            static_cast<float>((static_cast<double>(QMin.Y) + static_cast<double>(QMax.Y)) * 0.5),
            static_cast<float>((static_cast<double>(QMin.Z) + static_cast<double>(QMax.Z)) * 0.5));

        Entry.bSkip = VoxelNoise::ProvesScaledAbsPerlin3DAbove(
            QCenter, QMin, QMax, Params.WormThreshold,
            VoxelNoise::WormN1NoiseSpaceLipschitzBound);
        Entry.bValid = true;
        bOutProofComputed = true;
        return Entry.bSkip;
    }
}

//=============================================================================
// FRACTAL NOISE (fBm — fractional Brownian motion)
//=============================================================================
// Empile plusieurs octaves de Perlin: freq x2 et amp /2 à chaque octave.
//   Octave 1: freq=f,  amp=1.0   → grandes collines lisses
//   Octave 2: freq=2f, amp=0.5   → bosses moyennes
//   Octave 3: freq=4f, amp=0.25  → petits cailloux
// Lacunarity = x freq par octave (2 = double à chaque fois)
// Persistence = x amp par octave (0.5 = moitié)

// T2.b — per-thread octave bias for the tile being meshed (see VoxelGenerator.h).
// 0 = full quality; set by the mesher per tile from Step + Settings->LODOctaveDrop.
thread_local int32 VoxelGenLOD::OctaveBias = 0;

//=============================================================================
// OP-MAJOR DENSITY BLOCK HAND-OFF
//=============================================================================
// The public Begin/End pair is only a lifetime marker.  The actual storage is worker-local and
// bounded by the current mesher lattice.  A tile may cross several chunk keys; each key is filled
// by the already-prepared stack that GetDensityAt selected for that key, so no mutable stack state
// is shared across chunk boundaries.
struct FVoxelDensityBlockSession
{
    bool bActive = false;
    FIntVector Origin = FIntVector::ZeroValue;
    int32 Step = 1;
    int32 SizeX = 0;
    int32 SizeY = 0;
    int32 SizeZ = 0;
    TArray<FVoxelOpSample> Samples;
    TArray<uint8> Valid;
    TArray<FVoxelOpSample> BlockScratch;
    TArray<int32> ActiveOps;

    void Begin(FIntVector InOrigin, int32 InStep, int32 InSizeX, int32 InSizeY, int32 InSizeZ)
    {
        bActive = InStep > 0 && InSizeX > 0 && InSizeY > 0 && InSizeZ > 0;
        Origin = InOrigin;
        Step = FMath::Max(InStep, 1);
        SizeX = FMath::Max(InSizeX, 0);
        SizeY = FMath::Max(InSizeY, 0);
        SizeZ = FMath::Max(InSizeZ, 0);
        if (!bActive)
        {
            Samples.Reset();
            Valid.Reset();
            BlockScratch.Reset();
            ActiveOps.Reset();
            return;
        }

        // Allocate lazily on the first operator-stack sample.  Legacy archetypes still use the
        // ordinary scalar generator and should not pay for a block scratch lattice merely because
        // the mesher opened the scoped hand-off.
        BlockScratch.Reset();
    }

    void End()
    {
        bActive = false;
        // Keep capacity for the next tile on this worker, but never retain a verdict between
        // sessions.  A stale sample can only cost the next task a block fill, never change it.
        Samples.Reset();
        Valid.Reset();
        BlockScratch.Reset();
        ActiveOps.Reset();
    }

    int32 Index(int32 X, int32 Y, int32 Z) const
    {
        return (Z * SizeY + Y) * SizeX + X;
    }

    bool LocalIndex(FIntVector World, int32& OutX, int32& OutY, int32& OutZ) const
    {
        const int32 DX = World.X - Origin.X;
        const int32 DY = World.Y - Origin.Y;
        const int32 DZ = World.Z - Origin.Z;
        if (DX % Step != 0 || DY % Step != 0 || DZ % Step != 0) { return false; }
        OutX = DX / Step;
        OutY = DY / Step;
        OutZ = DZ / Step;
        return OutX >= 0 && OutX < SizeX
            && OutY >= 0 && OutY < SizeY
            && OutZ >= 0 && OutZ < SizeZ;
    }

    bool FindAxisRange(int32 Axis, int32 TargetChunk, int32& OutMin, int32& OutMax) const
    {
        OutMin = INT32_MAX;
        OutMax = INT32_MIN;
        const int32 Limit = Axis == 0 ? SizeX : (Axis == 1 ? SizeY : SizeZ);
        const int32 Base = Axis == 0 ? Origin.X : (Axis == 1 ? Origin.Y : Origin.Z);
        for (int32 I = 0; I < Limit; ++I)
        {
            const int32 Coordinate = Base + I * Step;
            if (FMath::FloorToInt(static_cast<float>(Coordinate) / CHUNK_SIZE) == TargetChunk)
            {
                OutMin = FMath::Min(OutMin, I);
                OutMax = FMath::Max(OutMax, I);
            }
        }
        return OutMin != INT32_MAX;
    }

    bool FillChunk(const FVoxelOpStack& Stack, FIntVector ChunkCoord, FIntVector World,
                   FVoxelOpSample& OutSample)
    {
        int32 X = 0, Y = 0, Z = 0;
        if (!LocalIndex(World, X, Y, Z)) { return false; }
        const int32 Count = SizeX * SizeY * SizeZ;
        if (Samples.Num() != Count || Valid.Num() != Count)
        {
            Samples.SetNum(Count);
            Valid.Init(0, Count);
        }
        const int32 GlobalIndex = Index(X, Y, Z);
        if (Valid[GlobalIndex] != 0)
        {
            OutSample = Samples[GlobalIndex];
            return true;
        }

        int32 MinX = 0, MaxX = 0, MinY = 0, MaxY = 0, MinZ = 0, MaxZ = 0;
        if (!FindAxisRange(0, ChunkCoord.X, MinX, MaxX)
            || !FindAxisRange(1, ChunkCoord.Y, MinY, MaxY)
            || !FindAxisRange(2, ChunkCoord.Z, MinZ, MaxZ))
        {
            return false;
        }

        const int32 BlockSizeX = MaxX - MinX + 1;
        const int32 BlockSizeY = MaxY - MinY + 1;
        const int32 BlockSizeZ = MaxZ - MinZ + 1;
        const int32 BlockCount = BlockSizeX * BlockSizeY * BlockSizeZ;
        VoxelDensityProfile::FScopedTimer OperatorBlockTimer(
            VoxelDensityProfile::EBucket::OperatorBlock);
        BlockScratch.SetNum(BlockCount);
        if (VoxelDensityProfile::AreCountersEnabled())
        {
            VoxelDensityProfile::AddCounter(
                VoxelDensityProfile::ECounter::OpBlockScratchBytes,
                static_cast<uint64>(BlockCount) * sizeof(FVoxelOpSample));
        }
        for (FVoxelOpSample& Sample : BlockScratch)
        {
            Sample = FVoxelOpSample();
        }

        FVoxelOpBlock Block;
        Block.OriginVoxels = Origin + FIntVector(MinX * Step, MinY * Step, MinZ * Step);
        Block.Step = Step;
        Block.SizeX = BlockSizeX;
        Block.SizeY = BlockSizeY;
        Block.SizeZ = BlockSizeZ;
        Block.Samples = BlockScratch.GetData();
        const FIntVector BlockLast = Block.OriginVoxels
            + FIntVector((BlockSizeX - 1) * Step,
                         (BlockSizeY - 1) * Step,
                         (BlockSizeZ - 1) * Step);
        const FBox BlockBox(
            FVector(static_cast<float>(Block.OriginVoxels.X),
                    static_cast<float>(Block.OriginVoxels.Y),
                    static_cast<float>(Block.OriginVoxels.Z)),
            FVector(static_cast<float>(BlockLast.X),
                    static_cast<float>(BlockLast.Y),
                    static_cast<float>(BlockLast.Z)));
        const int32 ActiveCount = Stack.BuildActiveOpList(
            BlockBox, Step, Block.OriginVoxels, ActiveOps);
        Stack.EvalBlock(Block, ActiveOps);

        for (int32 Bz = 0; Bz < BlockSizeZ; ++Bz)
        {
            for (int32 By = 0; By < BlockSizeY; ++By)
            {
                for (int32 Bx = 0; Bx < BlockSizeX; ++Bx)
                {
                    const int32 Global = Index(MinX + Bx, MinY + By, MinZ + Bz);
                    const int32 Local = (Bz * BlockSizeY + By) * BlockSizeX + Bx;
                    Samples[Global] = BlockScratch[Local];
                    Valid[Global] = 1;
                }
            }
        }
        if (VoxelDensityProfile::AreCountersEnabled())
        {
            VoxelDensityProfile::AddCounter(
                VoxelDensityProfile::ECounter::OpBlockCopyBytes,
                static_cast<uint64>(BlockCount)
                    * (sizeof(FVoxelOpSample) + sizeof(uint8)));
        }

        if (VoxelDensityProfile::AreCountersEnabled())
        {
            VoxelDensityProfile::AddCounter(VoxelDensityProfile::ECounter::OpBlockBuilds);
            VoxelDensityProfile::AddCounter(
                VoxelDensityProfile::ECounter::OpBlockSamples,
                static_cast<uint64>(BlockCount));
            VoxelDensityProfile::AddCounter(
                VoxelDensityProfile::ECounter::OpBlockOperators,
                static_cast<uint64>(Stack.Num()));
            VoxelDensityProfile::AddCounter(
                VoxelDensityProfile::ECounter::OpBlockActiveOperators,
                static_cast<uint64>(ActiveCount));
            VoxelDensityProfile::AddCounter(
                VoxelDensityProfile::ECounter::OpBlockPrunedOperators,
                static_cast<uint64>(FMath::Max(Stack.Num() - ActiveCount, 0)));
        }

        OutSample = Samples[GlobalIndex];
        return true;
    }
};

static thread_local FVoxelDensityBlockSession GVoxelDensityBlockSession;
thread_local int32 VoxelGenLOD::SampleStep = 1;
thread_local FIntVector VoxelGenLOD::TileOriginVoxels = FIntVector::ZeroValue;
thread_local int32 VoxelGenLOD::TileCellsPerAxis = 0;

int32 VoxelGenLOD::GetThreadOctaveBias()
{
    return OctaveBias;
}

void VoxelGenLOD::SetThreadOctaveBias(int32 Value)
{
    OctaveBias = Value;
}

int32 VoxelGenLOD::GetThreadSampleStep()
{
    return SampleStep;
}

void VoxelGenLOD::SetThreadSampleStep(int32 Value)
{
    SampleStep = Value;
}

bool VoxelGenLOD::IsTileCacheWindowEnabled(bool bFusedPath)
{
    VF_ParseTileCacheWindowSwitch();
    if (GVoxelForgeTileCacheWindow == 0)
    {
        return false;
    }
    const int32 RequiredMinLOD = bFusedPath
        ? GVoxelForgeTileCacheWindowMinLOD
        : GVoxelForgeTileCacheWindowOpStackMinLOD;
    const int32 RequiredStep = 1 << FMath::Clamp(RequiredMinLOD, 0, 5);
    return FMath::Max(SampleStep, 1) >= RequiredStep;
}

bool VoxelGenLOD::GetThreadTileCacheWindow(
    FIntVector& OutOriginVoxels, int32& OutStep, int32& OutCellsPerAxis)
{
    OutOriginVoxels = TileOriginVoxels;
    OutStep = FMath::Max(SampleStep, 1);
    OutCellsPerAxis = TileCellsPerAxis;
    if (TileCellsPerAxis <= 0)
    {
        return false;
    }
    const int64 Extent = static_cast<int64>(OutStep)
        * static_cast<int64>(OutCellsPerAxis);
    // The graph build is quadratic in the collected room count.  Keep the tile policy bounded
    // to a 64-chunk side; the field's LOD4 tile is 32 chunks, while an accidentally giant request
    // falls back to the older bounded window instead of turning one worker into an unbounded bake.
    return Extent > 0 && Extent <= MAX_int32
        && Extent <= static_cast<int64>(CHUNK_SIZE) * 64;
}

void VoxelGenLOD::SetThreadTileContext(
    const FIntVector& OriginVoxels,
    int32 CellsPerAxis)
{
    TileOriginVoxels = OriginVoxels;
    TileCellsPerAxis = CellsPerAxis;
}

bool VoxelGenLOD::ShouldUseSpatialIndex(bool bFusedPath)
{
    VF_ParseSpatialIndexSwitch();
    if (GVoxelForgeSpatialIndex >= 0)
    {
        return GVoxelForgeSpatialIndex != 0;
    }

    // The full game sweep makes the fused LOD0 floor faster without the broad phase.  Keep it
    // enabled for LOD1+ where the candidate reduction pays for GetRange, and keep the measured
    // op-stack policy enabled at every tested LOD.  The explicit override remains useful for
    // field/determinism A/B runs.
    if (bFusedPath && FMath::Max(SampleStep, 1) <= 1)
    {
        return false;
    }
    return true;
}

// NOTE (T2.a): the fBm/Ridged bodies moved to VoxelNoise.h, where octaves are evaluated
// 4-wide via SSE (Perlin3D_x4). These thin wrappers keep every call site unchanged. They
// sample a DIFFERENT (float hash-gradient) noise field than the old FMath::PerlinNoise3D,
// so worlds re-tune once — but the fBm/Ridged math/contracts ([-1,1]) are identical.
static float FractalNoise3D(const FVector3f& Position, int32 Octaves = 4,
                            float Lacunarity = 2.0f, float Persistence = 0.5f)
{
    return VoxelNoise::FBM(Position.X, Position.Y, Position.Z,
                           Octaves, Lacunarity, Persistence);
}

//=============================================================================
// RIDGED MULTIFRACTAL NOISE
//=============================================================================
// Creates sharp, ridge-like features by folding the noise at zero-crossings.
// The absolute value creates "creases" where the noise crosses zero,
// and the 1-abs inverts them into ridges. Weight feedback from each
// octave makes ridges sharper and more detailed.
//
// Returns approximately [-1, 1] to match FractalNoise3D's range.
// Character: craggy cliffs, natural erosion patterns, sharp corridors.

static float RidgedNoise3D(const FVector3f& Position, int32 Octaves = 4,
                           float Lacunarity = 2.0f, float Persistence = 0.5f)
{
    return VoxelNoise::Ridged(Position.X, Position.Y, Position.Z,
                              Octaves, Lacunarity, Persistence);
}

//=============================================================================
// WORLEY / CELLULAR NOISE (3D)
//=============================================================================
// Distance to nearest feature point in a hash grid.
// Creates rounded, cell-like patterns: bubble walls, grotto pockets,
// honeycomb textures. Very different character from Perlin-based noise.
//
// Algorithm:
// 1. Find which grid cell the point is in
// 2. Check the 3x3x3 neighborhood for feature points
// 3. Return (2nd nearest - 1st nearest) distance, normalized to ~[-1, 1]
//
// Using F2-F1 (difference of two closest distances) gives smooth cell
// boundaries with ridges between cells — more interesting than raw distance.

// Le CORPS a déménagé dans Public/VoxelCaveMorphology.h (namespace VoxelNoise), au plus bas point
// qui voit déjà `VoxelHash` : la pile d'opérateurs a besoin exactement du même bruit pour la
// rugosité (4b, type Cellular) et pour les festons (4f), et deux copies d'une fonction pure finissent
// par diverger — c'est littéralement `AUDIT §C1`. Ce forwarder garde les ~3 sites d'appel ci-dessous
// inchangés, comme l'ont fait FractalNoise3D et RidgedNoise3D lors de T2.a. Le chemin chaud garde
// maintenant les coordonnées en float : la compatibilité d'arrondi historique est abandonnée.
static float CellularNoise3D(const FVector3f& Position)
{
    return VoxelNoise::Cellular3D(Position);
}

//=============================================================================
// DENSITY PIPELINE HELPERS (partagés entre TunnelNetwork et Slab)
//=============================================================================

// Les CORPS de ces trois helpers ont déménagé dans Public/VoxelDensityPrimitives.h : la pile
// d'opérateurs (VoxelDensityOpStack) a besoin exactement des mêmes, et deux copies des INVARIANTS
// de monde (descente possible, seals qui tiennent, passages qui percent, bord fermé) finiraient par
// diverger. Ces trois lignes gardent les noms locaux pour que les ~20 sites d'appel ci-dessous ne
// bougent pas d'un caractère — le déplacement ne change AUCUN comportement.
//
// The BODIES moved to Public/VoxelDensityPrimitives.h; the operator stack needs the same primitives,
// and duplicate world invariants would eventually drift. These forwarders keep the local names
// so not one of the ~20 call sites below changes. No behavioural change.
//
// Convention INTERNE ici : positif = SOLIDE. La négation vers MC se fait sur le `return`.
static FORCEINLINE void ApplyBoundarySeal(float& Density, float WorldZ,
    float StrateTopZ, float StrateBottomZ, float Thickness, float BaseDensity)
{
    VF_ApplyBoundarySeal(Density, WorldZ, StrateTopZ, StrateBottomZ, Thickness, BaseDensity);
}

static FORCEINLINE void ApplyOriginSpine(float& Density, float WorldX, float WorldY, float WorldZ,
    float StrateTopZ, float StrateBottomZ, float SealThickness, float BaseDensity, float Radius)
{
    VF_ApplyOriginSpine(Density, WorldX, WorldY, WorldZ,
        StrateTopZ, StrateBottomZ, SealThickness, BaseDensity, Radius);
}

static FORCEINLINE void ApplyOriginLandingFloor(float& Density,
    float WorldX, float WorldY, float WorldZ,
    float StrateTopZ, float StrateBottomZ, float SealThickness, float BaseDensity, float Radius)
{
    VF_ApplyOriginLandingFloor(Density, WorldX, WorldY, WorldZ,
        StrateTopZ, StrateBottomZ, SealThickness, BaseDensity, Radius);
}

// DISTURBANCE LAYER — the "wow" post-process. Operates on the FINAL MC density
// (negative = solid, positive = air), AFTER the archetype produced its terrain, so
// it works uniformly for every generator type. Stays inside the seal bands so it can
// never breach a strate boundary. All features are hash-placed and deterministic.
static void ApplyDisturbances(float& MC, float X, float Y, float Z,
    const FStrateDisturbanceParams& D, uint32 Seed, bool bProtectVerticalShaftAir,
    bool bProtectAuthoredTunnelFloor)
{
    if (VoxelDensityAblation::IsDisturbancesOff())
    {
        return;
    }
    VoxelDensityProfile::FScopedTimer ProfileTimer(
        VoxelDensityProfile::EBucket::ApplyDisturbances);
    const float InnerTop = D.StrateTopWorldZ - D.BoundarySealThickness;
    const float InnerBot = D.StrateBottomWorldZ + D.BoundarySealThickness;
    if (Z <= InnerBot || Z >= InnerTop) return;

    // A tunnel floor is now authored into the cached shape with its clearance budget. The generic
    // disturbance layer is deliberately outside that shape, so it must not reopen or bridge the
    // finite support band after the shape has proved it. This is an ownership hand-off, not a
    // per-sample natural-floor validation.
    if (bProtectAuthoredTunnelFloor)
    {
        if (VoxelDensityProfile::AreCountersEnabled())
        {
            VoxelDensityProfile::AddCounter(
                VoxelDensityProfile::ECounter::TunnelAuthoredFloorDisturbanceSkips);
        }
        return;
    }

    const float Solid = D.BaseDensity * 2.0f;
    const float Blend = 3.0f;
    const FVector P(X, Y, Z);

    // 2D point-to-segment distance helper (XY plane).
    auto Dist2DSeg = [](float px, float py, float ax, float ay, float bx, float by) -> float
    {
        const float abx = bx - ax, aby = by - ay;
        const float apx = px - ax, apy = py - ay;
        const float denom = FMath::Max(abx * abx + aby * aby, KINDA_SMALL_NUMBER);
        float t = FMath::Clamp((apx * abx + apy * aby) / denom, 0.0f, 1.0f);
        const float cx = ax + abx * t, cy = ay + aby * t;
        return FMath::Sqrt(FMath::Square(px - cx) + FMath::Square(py - cy));
    };

    // Each feature's 3×3 lattice candidates (existence roll, jitter, angle trig, Z anchor) are pure
    // functions of (cell, seed, params) yet were re-hashed PER VOXEL — including Cos/Sin per bridge/
    // ridge candidate. Bake each feature's nearby primitives once per centre cell (thread_local);
    // the per-voxel work is just the distance math. Bit-identical (same hashes, same math).

    // --- CHASMS: vertical rifts carve open air ---
    if (D.ChasmDensity > 0.0f && D.ChasmSpacing > 0.0f)
    {
        const float Sp = D.ChasmSpacing;
        const int32 cx = FMath::FloorToInt(X / Sp), cy = FMath::FloorToInt(Y / Sp);

        struct FChasm { float X, Y; };
        thread_local TArray<FChasm, TInlineAllocator<9>> Chasms;
        thread_local int32  CH_CX = INT32_MAX, CH_CY = INT32_MAX;
        thread_local uint32 CH_Seed = 0xFFFFFFFFu;
        thread_local float  CH_Sp = -1.0f, CH_Dens = -1.0f;
        if (cx != CH_CX || cy != CH_CY || Seed != CH_Seed || Sp != CH_Sp || D.ChasmDensity != CH_Dens)
        {
            CH_CX = cx;  CH_CY = cy;  CH_Seed = Seed;  CH_Sp = Sp;  CH_Dens = D.ChasmDensity;
            Chasms.Reset();
            for (int32 dy = -1; dy <= 1; dy++)
            for (int32 dx = -1; dx <= 1; dx++)
            {
                const int32 nx = cx + dx, ny = cy + dy;
                const uint32 h = VoxelHash::Cell(nx, ny, Seed ^ 0x43480001u);
                if (VoxelHash::ToFloat01(h) > D.ChasmDensity) continue;
                const float jx = VoxelHash::ToFloat01(VoxelHash::Mix(h ^ 0x12345678u));
                const float jy = VoxelHash::ToFloat01(VoxelHash::Mix(h ^ 0x9ABCDEF0u));
                Chasms.Add({ (nx + 0.15f + jx * 0.7f) * Sp, (ny + 0.15f + jy * 0.7f) * Sp });
            }
        }

        float sdf = FLT_MAX;
        for (const FChasm& C : Chasms)
        {
            sdf = FMath::Min(sdf, FMath::Sqrt(FMath::Square(X - C.X) + FMath::Square(Y - C.Y)) - D.ChasmRadius);
        }
        if (sdf < Blend)
        {
            float c = FMath::Clamp((Blend - sdf) / (Blend * 2.0f), 0.0f, 1.0f);
            c = SmoothStep01(c);
            MC = FMath::Max(MC, c * Solid);  // force air
        }
    }

    // --- BRIDGES: horizontal solid spans across open space ---
    if (D.BridgeDensity > 0.0f && D.BridgeSpacing > 0.0f)
    {
        const float Sp = D.BridgeSpacing;
        const int32 cx = FMath::FloorToInt(X / Sp), cy = FMath::FloorToInt(Y / Sp);

        struct FBridge { FVector A, B; };
        thread_local TArray<FBridge, TInlineAllocator<9>> Bridges;
        thread_local int32  BR_CX = INT32_MAX, BR_CY = INT32_MAX;
        thread_local uint32 BR_Seed = 0xFFFFFFFFu;
        thread_local float  BR_Sp = -1.0f, BR_Dens = -1.0f, BR_Bot = FLT_MAX, BR_Top = FLT_MAX;
        if (cx != BR_CX || cy != BR_CY || Seed != BR_Seed || Sp != BR_Sp ||
            D.BridgeDensity != BR_Dens || InnerBot != BR_Bot || InnerTop != BR_Top)
        {
            BR_CX = cx;  BR_CY = cy;  BR_Seed = Seed;  BR_Sp = Sp;
            BR_Dens = D.BridgeDensity;  BR_Bot = InnerBot;  BR_Top = InnerTop;
            Bridges.Reset();
            for (int32 dy = -1; dy <= 1; dy++)
            for (int32 dx = -1; dx <= 1; dx++)
            {
                const int32 nx = cx + dx, ny = cy + dy;
                const uint32 h = VoxelHash::Cell(nx, ny, Seed ^ 0x42520001u);
                if (VoxelHash::ToFloat01(h) > D.BridgeDensity) continue;
                const float zc = FMath::Lerp(InnerBot + 8.0f, InnerTop - 8.0f,
                                             VoxelHash::ToFloat01(VoxelHash::Mix(h ^ 0xB1u)));
                const float ang = VoxelHash::ToFloat01(VoxelHash::Mix(h ^ 0xB2u)) * PI;
                const float dxu = FMath::Cos(ang), dyu = FMath::Sin(ang);
                const float bx = (nx + 0.5f) * Sp, by = (ny + 0.5f) * Sp;
                const float half = Sp * 0.6f;
                Bridges.Add({ FVector(bx - dxu * half, by - dyu * half, zc),
                              FVector(bx + dxu * half, by + dyu * half, zc) });
            }
        }

        float sdf = FLT_MAX;
        for (const FBridge& Br : Bridges)
        {
            sdf = FMath::Min(sdf, VoxelSDF::Capsule(P, Br.A, Br.B, D.BridgeRadius));
        }
        if (sdf < Blend)
        {
            float f = FMath::Clamp((Blend - sdf) / (Blend * 2.0f), 0.0f, 1.0f);
            f = SmoothStep01(f);
            if (!bProtectVerticalShaftAir)
            {
                MC = FMath::Min(MC, -f * Solid);  // force solid
            }
        }
    }

    // --- RIDGES: thin solid blades rising from the floor ---
    if (D.RidgeDensity > 0.0f && D.RidgeSpacing > 0.0f && D.RidgeHeight > 0.0f)
    {
        const float Sp = D.RidgeSpacing;
        const float TopZ = FMath::Min(InnerBot + D.RidgeHeight, InnerTop);
        if (Z < TopZ)
        {
            const int32 cx = FMath::FloorToInt(X / Sp), cy = FMath::FloorToInt(Y / Sp);

            struct FRidge { float AX, AY, BX, BY; };
            thread_local TArray<FRidge, TInlineAllocator<9>> Ridges;
            thread_local int32  RG_CX = INT32_MAX, RG_CY = INT32_MAX;
            thread_local uint32 RG_Seed = 0xFFFFFFFFu;
            thread_local float  RG_Sp = -1.0f, RG_Dens = -1.0f;
            if (cx != RG_CX || cy != RG_CY || Seed != RG_Seed || Sp != RG_Sp || D.RidgeDensity != RG_Dens)
            {
                RG_CX = cx;  RG_CY = cy;  RG_Seed = Seed;  RG_Sp = Sp;  RG_Dens = D.RidgeDensity;
                Ridges.Reset();
                for (int32 dy = -1; dy <= 1; dy++)
                for (int32 dx = -1; dx <= 1; dx++)
                {
                    const int32 nx = cx + dx, ny = cy + dy;
                    const uint32 h = VoxelHash::Cell(nx, ny, Seed ^ 0x52470001u);
                    if (VoxelHash::ToFloat01(h) > D.RidgeDensity) continue;
                    const float ang = VoxelHash::ToFloat01(VoxelHash::Mix(h ^ 0x9001u)) * PI;
                    const float dxu = FMath::Cos(ang), dyu = FMath::Sin(ang);
                    const float bx = (nx + 0.5f) * Sp, by = (ny + 0.5f) * Sp;
                    const float half = Sp * 0.45f;
                    Ridges.Add({ bx - dxu * half, by - dyu * half, bx + dxu * half, by + dyu * half });
                }
            }

            float best = -FLT_MAX;  // strongest fill across nearby blades
            for (const FRidge& R : Ridges)
            {
                const float d2d = Dist2DSeg(X, Y, R.AX, R.AY, R.BX, R.BY);
                const float wallSDF = d2d - D.RidgeThickness;     // <0 inside the blade footprint
                if (wallSDF >= Blend) continue;
                const float zFade = 1.0f - FMath::Clamp((Z - InnerBot) / FMath::Max(D.RidgeHeight, 1.0f), 0.0f, 1.0f);
                float f = FMath::Clamp((Blend - wallSDF) / (Blend * 2.0f), 0.0f, 1.0f);
                f = SmoothStep01(f) * zFade;
                best = FMath::Max(best, f);
            }
            if (best > 0.0f && !bProtectVerticalShaftAir)
            {
                MC = FMath::Min(MC, -best * Solid);  // force solid
            }
        }
    }
}

// The disturbance pass is deterministic and spatially sparse.  ClassifyTile used to treat a
// non-zero density as a world-wide influence, which made an otherwise proven all-solid/all-air
// LOD0 box fall back to the full mesher.  These predicates are deliberately one-sided: they only
// prove that no hash-placed primitive can touch the queried box.  An intersecting candidate, an
// invalid range, or an unknown parameter keeps the conservative Mixed result.
static FORCEINLINE bool VF_DisturbanceBandTouchesBox(
    const FBox& Box, const FStrateDisturbanceParams& D)
{
    const float InnerTop = D.StrateTopWorldZ - D.BoundarySealThickness;
    const float InnerBot = D.StrateBottomWorldZ + D.BoundarySealThickness;
    if (!VoxelMath::IsFinite(InnerTop) || !VoxelMath::IsFinite(InnerBot))
    {
        return true; // Unknown boundary placement must not discharge a proof.
    }
    return Box.Max.Z > InnerBot && Box.Min.Z < InnerTop;
}

static FORCEINLINE float VF_DistanceSquaredToXYBox(
    float X, float Y, const FBox& Box)
{
    const float DX = X < Box.Min.X ? Box.Min.X - X : X > Box.Max.X ? X - Box.Max.X : 0.0f;
    const float DY = Y < Box.Min.Y ? Box.Min.Y - Y : Y > Box.Max.Y ? Y - Box.Max.Y : 0.0f;
    return DX * DX + DY * DY;
}

static FORCEINLINE bool VF_ClosedIntervalsOverlap(
    float AMin, float AMax, float BMin, float BMax)
{
    return AMax >= BMin && BMax >= AMin;
}

static FORCEINLINE bool VF_InflatedSegmentAabbTouchesBox(
    const FVector& A, const FVector& B, float Inflation, const FBox& Box)
{
    const float MinX = FMath::Min(A.X, B.X) - Inflation;
    const float MaxX = FMath::Max(A.X, B.X) + Inflation;
    const float MinY = FMath::Min(A.Y, B.Y) - Inflation;
    const float MaxY = FMath::Max(A.Y, B.Y) + Inflation;
    const float MinZ = FMath::Min(A.Z, B.Z) - Inflation;
    const float MaxZ = FMath::Max(A.Z, B.Z) + Inflation;
    return VF_ClosedIntervalsOverlap(MinX, MaxX, Box.Min.X, Box.Max.X)
        && VF_ClosedIntervalsOverlap(MinY, MaxY, Box.Min.Y, Box.Max.Y)
        && VF_ClosedIntervalsOverlap(MinZ, MaxZ, Box.Min.Z, Box.Max.Z);
}

static FORCEINLINE float VF_DistanceSquaredToXYSegment(
    float X, float Y, float AX, float AY, float BX, float BY)
{
    const float ABX = BX - AX, ABY = BY - AY;
    const float APX = X - AX, APY = Y - AY;
    const float Denom = FMath::Max(ABX * ABX + ABY * ABY, KINDA_SMALL_NUMBER);
    const float T = FMath::Clamp((APX * ABX + APY * ABY) / Denom, 0.0f, 1.0f);
    const float CX = AX + ABX * T, CY = AY + ABY * T;
    return FMath::Square(X - CX) + FMath::Square(Y - CY);
}

static FORCEINLINE bool VF_AnyLatticeZInOpenBand(
    const FIntVector& Origin, int32 Step, int32 Cells, float MinZ, float MaxZ)
{
    for (int32 GZ = 0; GZ <= Cells; ++GZ)
    {
        const float Z = static_cast<float>(Origin.Z + GZ * Step);
        if (Z > MinZ && Z < MaxZ)
        {
            return true;
        }
    }
    return false;
}

static bool VF_AnyChasmCanTouchLattice(
    const FIntVector& Origin, int32 Step, int32 Cells,
    const FStrateDisturbanceParams& D, uint32 Seed)
{
    if (D.ChasmDensity == 0.0f)
    {
        return false;
    }
    if (!VoxelMath::IsFinite(D.ChasmDensity) || D.ChasmDensity < 0.0f
        || D.ChasmDensity > 1.0f || !VoxelMath::IsFinite(D.ChasmSpacing)
        || !VoxelMath::IsFinite(D.ChasmRadius) || D.ChasmSpacing <= 0.0f
        || D.ChasmRadius < 0.0f)
    {
        return true;
    }
    const float InnerTop = D.StrateTopWorldZ - D.BoundarySealThickness;
    const float InnerBot = D.StrateBottomWorldZ + D.BoundarySealThickness;
    if (!VoxelMath::IsFinite(InnerTop) || !VoxelMath::IsFinite(InnerBot))
    {
        return true;
    }
    if (!VF_AnyLatticeZInOpenBand(Origin, Step, Cells, InnerBot, InnerTop))
    {
        return false;
    }

    const float Spacing = D.ChasmSpacing;
    const float InfluenceRadius = D.ChasmRadius + 3.0f;
    const float InfluenceRadiusSq = InfluenceRadius * InfluenceRadius;
    for (int32 GY = 0; GY <= Cells; ++GY)
    for (int32 GX = 0; GX <= Cells; ++GX)
    {
        const float X = static_cast<float>(Origin.X + GX * Step);
        const float Y = static_cast<float>(Origin.Y + GY * Step);
        const int32 CX = FMath::FloorToInt(X / Spacing);
        const int32 CY = FMath::FloorToInt(Y / Spacing);
        for (int32 DY = -1; DY <= 1; ++DY)
        for (int32 DX = -1; DX <= 1; ++DX)
        {
            const int32 NX = CX + DX, NY = CY + DY;
            const uint32 H = VoxelHash::Cell(NX, NY, Seed ^ 0x43480001u);
            if (VoxelHash::ToFloat01(H) > D.ChasmDensity)
            {
                continue;
            }
            const float ChasmX = (NX + 0.15f
                + VoxelHash::ToFloat01(VoxelHash::Mix(H ^ 0x12345678u)) * 0.7f) * Spacing;
            const float ChasmY = (NY + 0.15f
                + VoxelHash::ToFloat01(VoxelHash::Mix(H ^ 0x9ABCDEF0u)) * 0.7f) * Spacing;
            if (FMath::Square(X - ChasmX) + FMath::Square(Y - ChasmY) <= InfluenceRadiusSq)
            {
                return true;
            }
        }
    }
    return false;
}

static bool VF_AnyBridgeCanTouchLattice(
    const FIntVector& Origin, int32 Step, int32 Cells,
    const FStrateDisturbanceParams& D, uint32 Seed)
{
    if (D.BridgeDensity == 0.0f)
    {
        return false;
    }
    if (!VoxelMath::IsFinite(D.BridgeDensity) || D.BridgeDensity < 0.0f
        || D.BridgeDensity > 1.0f || !VoxelMath::IsFinite(D.BridgeSpacing)
        || !VoxelMath::IsFinite(D.BridgeRadius) || D.BridgeSpacing <= 0.0f
        || D.BridgeRadius < 0.0f)
    {
        return true;
    }
    const float InnerTop = D.StrateTopWorldZ - D.BoundarySealThickness;
    const float InnerBot = D.StrateBottomWorldZ + D.BoundarySealThickness;
    if (!VoxelMath::IsFinite(InnerTop) || !VoxelMath::IsFinite(InnerBot))
    {
        return true;
    }

    const float Spacing = D.BridgeSpacing;
    const float Inflation = D.BridgeRadius + 3.0f;
    const float InflationSq = Inflation * Inflation;
    for (int32 GY = 0; GY <= Cells; ++GY)
    for (int32 GX = 0; GX <= Cells; ++GX)
    {
        const float X = static_cast<float>(Origin.X + GX * Step);
        const float Y = static_cast<float>(Origin.Y + GY * Step);
        const int32 CX = FMath::FloorToInt(X / Spacing);
        const int32 CY = FMath::FloorToInt(Y / Spacing);
        for (int32 DY = -1; DY <= 1; ++DY)
        for (int32 DX = -1; DX <= 1; ++DX)
        {
            const int32 NX = CX + DX, NY = CY + DY;
            const uint32 H = VoxelHash::Cell(NX, NY, Seed ^ 0x42520001u);
            if (VoxelHash::ToFloat01(H) > D.BridgeDensity)
            {
                continue;
            }
            const float ZC = FMath::Lerp(
                InnerBot + 8.0f, InnerTop - 8.0f,
                VoxelHash::ToFloat01(VoxelHash::Mix(H ^ 0xB1u)));
            const float Angle = VoxelHash::ToFloat01(VoxelHash::Mix(H ^ 0xB2u)) * PI;
            const float DXU = FMath::Cos(Angle), DYU = FMath::Sin(Angle);
            const float BX = (NX + 0.5f) * Spacing, BY = (NY + 0.5f) * Spacing;
            const float Half = Spacing * 0.6f;
            const float HorizontalSq = VF_DistanceSquaredToXYSegment(
                X, Y, BX - DXU * Half, BY - DYU * Half,
                BX + DXU * Half, BY + DYU * Half);
            if (HorizontalSq > InflationSq)
            {
                continue;
            }
            for (int32 GZ = 0; GZ <= Cells; ++GZ)
            {
                const float Z = static_cast<float>(Origin.Z + GZ * Step);
                if (FMath::Square(Z - ZC) + HorizontalSq <= InflationSq)
                {
                    return true;
                }
            }
        }
    }
    return false;
}

static bool VF_AnyRidgeCanTouchLattice(
    const FIntVector& Origin, int32 Step, int32 Cells,
    const FStrateDisturbanceParams& D, uint32 Seed)
{
    if (D.RidgeDensity == 0.0f)
    {
        return false;
    }
    if (!VoxelMath::IsFinite(D.RidgeDensity) || D.RidgeDensity < 0.0f
        || D.RidgeDensity > 1.0f || !VoxelMath::IsFinite(D.RidgeSpacing)
        || !VoxelMath::IsFinite(D.RidgeHeight) || !VoxelMath::IsFinite(D.RidgeThickness)
        || D.RidgeSpacing <= 0.0f || D.RidgeHeight <= 0.0f || D.RidgeThickness < 0.0f)
    {
        return true;
    }
    const float InnerTop = D.StrateTopWorldZ - D.BoundarySealThickness;
    const float InnerBot = D.StrateBottomWorldZ + D.BoundarySealThickness;
    if (!VoxelMath::IsFinite(InnerTop) || !VoxelMath::IsFinite(InnerBot))
    {
        return true;
    }
    const float TopZ = FMath::Min(InnerBot + D.RidgeHeight, InnerTop);
    bool bHasRidgeZ = false;
    for (int32 GZ = 0; GZ <= Cells; ++GZ)
    {
        if (static_cast<float>(Origin.Z + GZ * Step) < TopZ)
        {
            bHasRidgeZ = true;
            break;
        }
    }
    if (!bHasRidgeZ)
    {
        return false;
    }

    const float Spacing = D.RidgeSpacing;
    const float InfluenceRadius = D.RidgeThickness + 3.0f;
    const float InfluenceRadiusSq = InfluenceRadius * InfluenceRadius;
    for (int32 GY = 0; GY <= Cells; ++GY)
    for (int32 GX = 0; GX <= Cells; ++GX)
    {
        const float X = static_cast<float>(Origin.X + GX * Step);
        const float Y = static_cast<float>(Origin.Y + GY * Step);
        const int32 CX = FMath::FloorToInt(X / Spacing);
        const int32 CY = FMath::FloorToInt(Y / Spacing);
        for (int32 DY = -1; DY <= 1; ++DY)
        for (int32 DX = -1; DX <= 1; ++DX)
        {
            const int32 NX = CX + DX, NY = CY + DY;
            const uint32 H = VoxelHash::Cell(NX, NY, Seed ^ 0x52470001u);
            if (VoxelHash::ToFloat01(H) > D.RidgeDensity)
            {
                continue;
            }
            const float Angle = VoxelHash::ToFloat01(VoxelHash::Mix(H ^ 0x9001u)) * PI;
            const float DXU = FMath::Cos(Angle), DYU = FMath::Sin(Angle);
            const float BX = (NX + 0.5f) * Spacing, BY = (NY + 0.5f) * Spacing;
            const float Half = Spacing * 0.45f;
            if (VF_DistanceSquaredToXYSegment(
                    X, Y, BX - DXU * Half, BY - DYU * Half,
                    BX + DXU * Half, BY + DYU * Half) <= InfluenceRadiusSq)
            {
                return true;
            }
        }
    }
    return false;
}

static bool VF_AnyChasmCanTouchBox(
    const FBox& Box, const FStrateDisturbanceParams& D, uint32 Seed)
{
    if (D.ChasmDensity == 0.0f)
    {
        return false;
    }
    if (!VoxelMath::IsFinite(D.ChasmDensity) || D.ChasmDensity < 0.0f
        || D.ChasmDensity > 1.0f || !VoxelMath::IsFinite(D.ChasmSpacing)
        || !VoxelMath::IsFinite(D.ChasmRadius) || D.ChasmSpacing <= 0.0f
        || D.ChasmRadius < 0.0f)
    {
        return true;
    }
    if (!VF_DisturbanceBandTouchesBox(Box, D))
    {
        return false;
    }

    const float Spacing = D.ChasmSpacing;
    const int32 CellX0 = FMath::FloorToInt(Box.Min.X / Spacing) - 1;
    const int32 CellX1 = FMath::FloorToInt(Box.Max.X / Spacing) + 1;
    const int32 CellY0 = FMath::FloorToInt(Box.Min.Y / Spacing) - 1;
    const int32 CellY1 = FMath::FloorToInt(Box.Max.Y / Spacing) + 1;
    const float InfluenceRadius = D.ChasmRadius + 3.0f; // ApplyDisturbances' Blend.
    const float InfluenceRadiusSq = InfluenceRadius * InfluenceRadius;

    for (int32 NY = CellY0; NY <= CellY1; ++NY)
    for (int32 NX = CellX0; NX <= CellX1; ++NX)
    {
        const uint32 H = VoxelHash::Cell(NX, NY, Seed ^ 0x43480001u);
        if (VoxelHash::ToFloat01(H) > D.ChasmDensity)
        {
            continue;
        }
        const float CX = (NX + 0.15f
            + VoxelHash::ToFloat01(VoxelHash::Mix(H ^ 0x12345678u)) * 0.7f) * Spacing;
        const float CY = (NY + 0.15f
            + VoxelHash::ToFloat01(VoxelHash::Mix(H ^ 0x9ABCDEF0u)) * 0.7f) * Spacing;
        if (VF_DistanceSquaredToXYBox(CX, CY, Box) <= InfluenceRadiusSq)
        {
            return true;
        }
    }
    return false;
}

static bool VF_AnyBridgeCanTouchBox(
    const FBox& Box, const FStrateDisturbanceParams& D, uint32 Seed)
{
    if (D.BridgeDensity == 0.0f)
    {
        return false;
    }
    if (!VoxelMath::IsFinite(D.BridgeDensity) || D.BridgeDensity < 0.0f
        || D.BridgeDensity > 1.0f || !VoxelMath::IsFinite(D.BridgeSpacing)
        || !VoxelMath::IsFinite(D.BridgeRadius) || D.BridgeSpacing <= 0.0f
        || D.BridgeRadius < 0.0f)
    {
        return true;
    }
    if (!VF_DisturbanceBandTouchesBox(Box, D))
    {
        return false;
    }

    const float Spacing = D.BridgeSpacing;
    const int32 CellX0 = FMath::FloorToInt(Box.Min.X / Spacing) - 1;
    const int32 CellX1 = FMath::FloorToInt(Box.Max.X / Spacing) + 1;
    const int32 CellY0 = FMath::FloorToInt(Box.Min.Y / Spacing) - 1;
    const int32 CellY1 = FMath::FloorToInt(Box.Max.Y / Spacing) + 1;
    const float Inflation = D.BridgeRadius + 3.0f; // Capsule radius plus Blend.
    const float InnerTop = D.StrateTopWorldZ - D.BoundarySealThickness;
    const float InnerBot = D.StrateBottomWorldZ + D.BoundarySealThickness;

    for (int32 NY = CellY0; NY <= CellY1; ++NY)
    for (int32 NX = CellX0; NX <= CellX1; ++NX)
    {
        const uint32 H = VoxelHash::Cell(NX, NY, Seed ^ 0x42520001u);
        if (VoxelHash::ToFloat01(H) > D.BridgeDensity)
        {
            continue;
        }
        const float ZC = FMath::Lerp(
            InnerBot + 8.0f, InnerTop - 8.0f,
            VoxelHash::ToFloat01(VoxelHash::Mix(H ^ 0xB1u)));
        const float Angle = VoxelHash::ToFloat01(VoxelHash::Mix(H ^ 0xB2u)) * PI;
        const float DX = FMath::Cos(Angle), DY = FMath::Sin(Angle);
        const float BX = (NX + 0.5f) * Spacing, BY = (NY + 0.5f) * Spacing;
        const float Half = Spacing * 0.6f;
        const FVector A(BX - DX * Half, BY - DY * Half, ZC);
        const FVector B(BX + DX * Half, BY + DY * Half, ZC);
        if (VF_InflatedSegmentAabbTouchesBox(A, B, Inflation, Box))
        {
            return true;
        }
    }
    return false;
}

static bool VF_AnyRidgeCanTouchBox(
    const FBox& Box, const FStrateDisturbanceParams& D, uint32 Seed)
{
    if (D.RidgeDensity == 0.0f)
    {
        return false;
    }
    if (!VoxelMath::IsFinite(D.RidgeDensity) || D.RidgeDensity < 0.0f
        || D.RidgeDensity > 1.0f || !VoxelMath::IsFinite(D.RidgeSpacing)
        || !VoxelMath::IsFinite(D.RidgeHeight) || !VoxelMath::IsFinite(D.RidgeThickness)
        || D.RidgeSpacing <= 0.0f || D.RidgeHeight <= 0.0f || D.RidgeThickness < 0.0f)
    {
        return true;
    }
    if (!VF_DisturbanceBandTouchesBox(Box, D))
    {
        return false;
    }

    const float Spacing = D.RidgeSpacing;
    const int32 CellX0 = FMath::FloorToInt(Box.Min.X / Spacing) - 1;
    const int32 CellX1 = FMath::FloorToInt(Box.Max.X / Spacing) + 1;
    const int32 CellY0 = FMath::FloorToInt(Box.Min.Y / Spacing) - 1;
    const int32 CellY1 = FMath::FloorToInt(Box.Max.Y / Spacing) + 1;
    const float Inflation = D.RidgeThickness + 3.0f; // Footprint radius plus Blend.
    const float InnerTop = D.StrateTopWorldZ - D.BoundarySealThickness;
    const float InnerBot = D.StrateBottomWorldZ + D.BoundarySealThickness;
    const float TopZ = FMath::Min(InnerBot + D.RidgeHeight, InnerTop);
    if (Box.Min.Z >= TopZ)
    {
        return false;
    }

    for (int32 NY = CellY0; NY <= CellY1; ++NY)
    for (int32 NX = CellX0; NX <= CellX1; ++NX)
    {
        const uint32 H = VoxelHash::Cell(NX, NY, Seed ^ 0x52470001u);
        if (VoxelHash::ToFloat01(H) > D.RidgeDensity)
        {
            continue;
        }
        const float Angle = VoxelHash::ToFloat01(VoxelHash::Mix(H ^ 0x9001u)) * PI;
        const float DX = FMath::Cos(Angle), DY = FMath::Sin(Angle);
        const float BX = (NX + 0.5f) * Spacing, BY = (NY + 0.5f) * Spacing;
        const float Half = Spacing * 0.45f;
        const FVector A(BX - DX * Half, BY - DY * Half, InnerBot);
        const FVector B(BX + DX * Half, BY + DY * Half, InnerBot);
        if (VF_InflatedSegmentAabbTouchesBox(A, B, Inflation, Box))
        {
            return true;
        }
    }
    return false;
}

static FORCEINLINE bool VF_AnyDisturbanceCanTouchBox(
    const FBox& Box, const FStrateDisturbanceParams& D, uint32 Seed)
{
    return VF_AnyChasmCanTouchBox(Box, D, Seed)
        || VF_AnyBridgeCanTouchBox(Box, D, Seed)
        || VF_AnyRidgeCanTouchBox(Box, D, Seed);
}

//=============================================================================
// LE MAPPING « ARCHÉTYPE → PILE D'OPÉRATEURS » — UNE SEULE DÉFINITION
//=============================================================================
// ⚠️ EXTRAIT DE `GetDensityAt` PARCE QUE `ClassifyTile` EN A BESOIN AUSSI, ET QU'UNE DEUXIÈME COPIE
// SERAIT LA PIRE FORME DE BUG DISPONIBLE ICI.
//
// `ClassifyTile` décide si une tuile est maillée DU TOUT. Si son verdict venait d'une pile
// construite autrement que celle qui produit la densité — ne serait-ce qu'un paramètre de
// construction différent — la tuile serait sautée sur la foi d'un champ qui n'est pas celui que le
// mesher aurait vu. C'est-à-dire un TROU : pas de géométrie, pas de collision, invisible.
// Un commentaire « garder les deux en phase » n'aurait pas suffi ; il fallait qu'il n'y en ait
// qu'une.
//
// Les params ne sont PAS cherchés ici : les deux appelants les ont déjà (le chemin densité les tient
// dans ses `CP_*`, le classifieur les cherche pour son slot). On ne passe que des pointeurs.
//
// ONE definition of the archetype → stack mapping, because ClassifyTile decides whether a tile is
// meshed at all: a verdict from a differently-built stack would be a hole. Params are passed in,
// never fetched here — both callers already have them.
namespace
{
    // Une identité monotone évite qu'un worker réutilise les CP_* d'un monde détruit même si
    // l'allocateur UObject recycle plus tard la même adresse. Relaxed suffit : on ne publie aucune
    // donnée, on alloue seulement une valeur distincte par instance.
    // A monotonic identity prevents stale CP_* reuse even if UObject allocation later recycles an
    // address. Relaxed ordering is sufficient: this allocates uniqueness, it publishes no data.
    std::atomic<uint64> GNextDensityCacheOwnerId { 0 };

    struct FTunnelCoreCacheState
    {
        FChunkSDFCache Cache;
        uint64 OwnerId = 0;
        uint64 ManagerLifetimeId = 0;
        FIntVector Chunk = FIntVector(INT32_MAX, INT32_MAX, INT32_MAX);
        FIntVector TileOrigin = FIntVector::ZeroValue;
        FIntVector SupportChunk = FIntVector(INT32_MAX, INT32_MAX, INT32_MAX);
        int32 TileStep = 0;
        int32 TileCells = 0;
        int32 StrateIndex = INT32_MIN;
        uint32 WorldSeed = 0xFFFFFFFFu;
        uint32 ParamsFingerprint = 0xFFFFFFFFu;
        uint32 LayoutVersion = 0xFFFFFFFFu;
        bool bUsesTileCacheWindow = false;
        bool bValid = false;
    };

    static thread_local FTunnelCoreCacheState GTunnelCoreCache;

    static void BuildTunnelSupportColumnsForChunk(
        FChunkSDFCache& Cache,
        const FIntVector& ChunkCoord,
        bool bUseSpatialIndex);

    static const FTunnelSupportFloorColumn* FindTunnelSupportColumn(
        const FChunkSDFCache& Cache,
        int32 X,
        int32 Y,
        const FTunnelSupportFloorColumn& EmptyColumn);

    // A mesher tile samples an expanded grid (-1..Cells+1), so one worker visits several exact
    // chunk coordinates even while it is generating one tile. The old CP_* cache retained only
    // the last coordinate and consequently rebuilt the whole TunnelNetwork stack whenever the
    // sampling cursor crossed a chunk boundary. Keep a deterministic direct-mapped set of states
    // instead: collisions only cause a safe rebuild, never a stale read or an order-dependent
    // result. Native TunnelNetwork/Underwater states are immutable after construction; the active
    // pointer below lets the hot sample path read the stored stack and tunnel-core cache without
    // moving them on every voxel.
    // The cache is open-addressed and bounded.  Keep the worker footprint at the established
    // 128-entry cap; the expensive native tunnel-core cache is not prepared for an operator-stack
    // chunk because the stack publishes the same result to the common post below.
    constexpr int32 TunnelDensityCacheSlotCount = 128;

    struct FTunnelNetworkDensityCacheEntry
    {
        bool bValid = false;
        uint64 OwnerId = 0;
        uint64 ManagerLifetimeId = 0;
        FIntVector Chunk = FIntVector(INT32_MAX, INT32_MAX, INT32_MAX);
        uint32 LayoutVersion = 0xFFFFFFFFu;
        ECaveGeneratorType GenType = ECaveGeneratorType::TunnelNetwork;
        FStrateGenerationParams Tunnel;
        uint32 TunnelFingerprint = 0xFFFFFFFFu;
        FStrateDisturbanceParams Disturbance;
        bool bUseOpStack = false;
        bool bUsesTileCacheWindow = false;
        FVoxelOpStack OpStack;
        FTunnelCoreCacheState TunnelCore;
    };

    void ReportTunnelDensityCacheFootprint(
        const FTunnelNetworkDensityCacheEntry* Entries)
    {
        if (!VoxelDensityProfile::IsEnabled() || Entries == nullptr)
        {
            return;
        }

        uint64 DynamicBytes = 0;
        uint64 EntryBytes = 0;
        uint64 LargestEntryBytes = 0;
        uint64 ValidEntries = 0;
        VoxelDensityProfile::FCacheMemoryBreakdown Breakdown;
        for (int32 Index = 0; Index < TunnelDensityCacheSlotCount; ++Index)
        {
            const FTunnelNetworkDensityCacheEntry& Entry = Entries[Index];
            if (!Entry.bValid)
            {
                continue;
            }

            ++ValidEntries;
            const uint64 StackBytes = static_cast<uint64>(Entry.OpStack.GetAllocatedSize());
            const uint64 TunnelCoreBytes = static_cast<uint64>(
                Entry.TunnelCore.Cache.GetAllocatedSize());
            Breakdown.OpStackBytes += StackBytes;
            Breakdown += Entry.TunnelCore.Cache.GetAllocatedSizeBreakdown();
            const uint64 EntryDynamicBytes = StackBytes + TunnelCoreBytes;
            const uint64 FullEntryBytes = static_cast<uint64>(
                sizeof(FTunnelNetworkDensityCacheEntry)) + EntryDynamicBytes;
            DynamicBytes += EntryDynamicBytes;
            EntryBytes += FullEntryBytes;
            LargestEntryBytes = FMath::Max(LargestEntryBytes, FullEntryBytes);
        }
        Breakdown.SlotStorageBytes = static_cast<uint64>(sizeof(FTunnelNetworkDensityCacheEntry))
            * TunnelDensityCacheSlotCount;

        VoxelDensityProfile::SetWorkerTunnelCacheFootprint(
            TunnelDensityCacheSlotCount,
            ValidEntries,
            static_cast<uint64>(sizeof(FTunnelNetworkDensityCacheEntry))
                * TunnelDensityCacheSlotCount,
            DynamicBytes,
            EntryBytes,
            LargestEntryBytes,
            ValidEntries,
            Breakdown);
    }

    FORCEINLINE int32 TunnelDensityCacheSlot(const FIntVector& Chunk)
    {
        uint32 Hash = static_cast<uint32>(Chunk.X) * 0x9E3779B9u;
        Hash ^= static_cast<uint32>(Chunk.Y) * 0x85EBCA6Bu;
        Hash ^= static_cast<uint32>(Chunk.Z) * 0xC2B2AE35u;
        Hash ^= Hash >> 16;
        return static_cast<int32>(Hash & (TunnelDensityCacheSlotCount - 1));
    }

    void PrepareTunnelCoreCache(
        const UVoxelStrateManager& Manager,
        const FIntVector& ChunkCoord,
        const FStrateGenerationParams& Params,
        uint32 ParamsFingerprint,
        uint32 LayoutVersion,
        uint64 ManagerLifetimeId,
        uint64 OwnerId,
        FTunnelCoreCacheState& OutState)
    {
        FIntVector RequestedTileOrigin = FIntVector::ZeroValue;
        int32 RequestedTileStep = 1;
        int32 RequestedTileCells = 0;
        const TArray<FStrateTerrainOpEntry>* TerrainOps = nullptr;
        if (UVoxelStrateDefinition* Def = Manager.GetStrateForChunk(ChunkCoord))
        {
            TerrainOps = &Def->TerrainOperations;
        }
        const bool bUseTileCacheWindow = VoxelGenLOD::IsTileCacheWindowEnabled(true)
            && VoxelGenLOD::GetThreadTileCacheWindow(
                RequestedTileOrigin, RequestedTileStep, RequestedTileCells)
            && VoxelCaveMorphology::IsRoomGraphWindowInvariant(Params, TerrainOps);
        const bool bUseSpatialIndex = VoxelGenLOD::ShouldUseSpatialIndex(true);
        const int32 StrateIndex = Manager.GetStrateIndex(
            (static_cast<float>(ChunkCoord.Z) + 0.5f)
            * static_cast<float>(CHUNK_SIZE) * VOXEL_SIZE);
        const uint32 WorldSeed = static_cast<uint32>(Manager.GetWorldSeed());

        const bool bSameGraphWindow = OutState.bValid
            && OutState.OwnerId == OwnerId
            && OutState.ManagerLifetimeId == ManagerLifetimeId
            && OutState.StrateIndex == StrateIndex
            && OutState.WorldSeed == WorldSeed
            && OutState.ParamsFingerprint == ParamsFingerprint
            && OutState.LayoutVersion == LayoutVersion
            && OutState.bUsesTileCacheWindow == bUseTileCacheWindow
            && (!bUseTileCacheWindow
                ? OutState.Chunk == ChunkCoord
                : (OutState.TileOrigin == RequestedTileOrigin
                    && OutState.TileStep == RequestedTileStep
                    && OutState.TileCells == RequestedTileCells));
        if (bSameGraphWindow)
        {
            // The graph is shared by the whole tile, but the sparse support table is deliberately
            // kept for the currently queried exact chunk. Rebuild only that small table when the
            // density cursor crosses a chunk; the room/tunnel graph itself is not rebuilt.
            if (OutState.SupportChunk != ChunkCoord)
            {
                BuildTunnelSupportColumnsForChunk(
                    OutState.Cache, ChunkCoord, bUseSpatialIndex);
                OutState.SupportChunk = ChunkCoord;
            }
            OutState.Chunk = ChunkCoord;
            return;
        }

        const float ChunkMinX = static_cast<float>(ChunkCoord.X * CHUNK_SIZE);
        const float ChunkMinY = static_cast<float>(ChunkCoord.Y * CHUNK_SIZE);
        const float ChunkMaxX = ChunkMinX + static_cast<float>(CHUNK_SIZE);
        const float ChunkMaxY = ChunkMinY + static_cast<float>(CHUNK_SIZE);
        const float WarpMargin = FMath::Abs(Params.CaveWarpStrength)
            * VOXEL_NOISE_SCALE * 1.5f + 2.0f;
        float SearchMinX = ChunkMinX - WarpMargin;
        float SearchMinY = ChunkMinY - WarpMargin;
        float SearchMaxX = ChunkMaxX + WarpMargin;
        float SearchMaxY = ChunkMaxY + WarpMargin;
        if (bUseTileCacheWindow)
        {
            const float TileMinX = static_cast<float>(RequestedTileOrigin.X);
            const float TileMinY = static_cast<float>(RequestedTileOrigin.Y);
            const float TileExtent = static_cast<float>(
                static_cast<int64>(RequestedTileStep)
                * static_cast<int64>(RequestedTileCells));
            const float TileHalo = static_cast<float>(RequestedTileStep) + 2.0f;
            const float Expansion = FMath::Abs(Params.CaveWarpStrength)
                * VOXEL_NOISE_SCALE * 1.5f + TileHalo;
            SearchMinX = TileMinX - Expansion;
            SearchMinY = TileMinY - Expansion;
            SearchMaxX = TileMinX + TileExtent + Expansion;
            SearchMaxY = TileMinY + TileExtent + Expansion;
        }
        VoxelCaveMorphology::BuildChunkCache(
            OutState.Cache,
            SearchMinX, SearchMinY, SearchMaxX, SearchMaxY,
            Params, WorldSeed,
            StrateIndex, nullptr,
            ERoomGraphBuildSite::GeneratorTunnelCore);
        BuildTunnelSupportColumnsForChunk(
            OutState.Cache, ChunkCoord, bUseSpatialIndex);
        OutState.OwnerId = OwnerId;
        OutState.ManagerLifetimeId = ManagerLifetimeId;
        OutState.Chunk = ChunkCoord;
        OutState.TileOrigin = bUseTileCacheWindow
            ? RequestedTileOrigin : FIntVector::ZeroValue;
        OutState.TileStep = bUseTileCacheWindow ? RequestedTileStep : 0;
        OutState.TileCells = bUseTileCacheWindow ? RequestedTileCells : 0;
        OutState.SupportChunk = ChunkCoord;
        OutState.StrateIndex = StrateIndex;
        OutState.WorldSeed = WorldSeed;
        OutState.ParamsFingerprint = ParamsFingerprint;
        OutState.LayoutVersion = LayoutVersion;
        OutState.bUsesTileCacheWindow = bUseTileCacheWindow;
        OutState.bValid = true;
    }

    static void BuildTunnelSupportColumnsForChunk(
        FChunkSDFCache& Cache,
        const FIntVector& ChunkCoord,
        bool bUseSpatialIndex)
    {
        const int32 MinX = ChunkCoord.X * CHUNK_SIZE - 1;
        const int32 MinY = ChunkCoord.Y * CHUNK_SIZE - 1;
        const int32 Cells = CHUNK_SIZE + 2;
        Cache.SupportColumnMinX = MinX;
        Cache.SupportColumnMinY = MinY;
        Cache.SupportColumnCellsX = Cells;
        Cache.SupportColumnCellsY = Cells;
        Cache.SupportColumnEntries.Init(INDEX_NONE, Cells * Cells);
        Cache.SupportColumns.Reset();

        // Empty columns are represented only by INDEX_NONE in the slot map, so
        // each stored column retains the inline bands for actual graph tubes.
        for (int32 Y = 0; Y < Cells; ++Y)
        {
            for (int32 X = 0; X < Cells; ++X)
            {
                FTunnelSupportFloorColumn Column;
                VoxelCaveMorphology::BuildTunnelSupportFloorColumn(
                    static_cast<float>(MinX + X),
                    static_cast<float>(MinY + Y),
                    Cache,
                    Column,
                    bUseSpatialIndex);
                if (Column.Intervals.Num() == 0)
                {
                    continue;
                }

                const int32 EntryIndex = Cache.SupportColumns.Add(MoveTemp(Column));
                Cache.SupportColumnEntries[Y * Cells + X] = EntryIndex;
            }
        }
    }

    static const FTunnelSupportFloorColumn* FindTunnelSupportColumn(
        const FChunkSDFCache& Cache,
        int32 X,
        int32 Y,
        const FTunnelSupportFloorColumn& EmptyColumn)
    {
        if (Cache.SupportColumnCellsX <= 0
            || X < Cache.SupportColumnMinX
            || X >= Cache.SupportColumnMinX + Cache.SupportColumnCellsX
            || Y < Cache.SupportColumnMinY
            || Y >= Cache.SupportColumnMinY + Cache.SupportColumnCellsY)
        {
            return nullptr;
        }

        const int32 Slot =
            (Y - Cache.SupportColumnMinY) * Cache.SupportColumnCellsX
            + (X - Cache.SupportColumnMinX);
        const int32 EntryIndex = Cache.SupportColumnEntries[Slot];
        return EntryIndex == INDEX_NONE
            ? &EmptyColumn
            : &Cache.SupportColumns[EntryIndex];
    }

    struct FVoxelStackParamRefs
    {
        const FSlabGenerationParams*   Slab    = nullptr;
        const FMazeGenerationParams*   Maze    = nullptr;
        const FVerticalShaftParams*    Vert    = nullptr;
        const FFloatingIslandParams*   Float   = nullptr;
        const FStrateGenerationParams* Tunnel  = nullptr;

        // SurfaceWorld uniquement. `Surface == nullptr` ⇒ la fabrique REFUSE cet archétype, ce qui
        // est exactement ce que veut `ClassifyTile` : il prouve SurfaceWorld lui-même, sur le
        // treillis exact du mesher, et n'a aucune raison de passer par la pile pour ça.
        const FSurfaceGenerationParams*         Surface            = nullptr;
        const TArray<FSurfaceGenerationParams>* SurfaceBiomeParams = nullptr;
        TUniquePtr<IVoxelBiomeField>            BiomeField;
    };

    /**
     * Construit la pile de cet archétype dans `OutStack` et remplit les bornes Z de `OutCtx`.
     *
     * @return false quand la pile NE DOIT PAS être utilisée — archétype non porté, params absents,
     *         ou **strate dégénérée**. Ce dernier cas n'est pas de la prudence : cinq fonctions
     *         d'archétype court-circuitent sur `return 1.0f` (= air) quand la hauteur est nulle,
     *         et la pile n'a pas cet early-out, par conception. L'appelant retombe sur le `switch`,
     *         qui EST le comportement de référence. (`GetDensityWithParams`, lui, n'a aucun
     *         early-out de ce genre : TunnelNetwork/Underwater n'ont donc pas cette garde.)
     */
    bool VF_BuildOpStackForChunk(ECaveGeneratorType Type, FVoxelStackParamRefs& Refs,
                                 int32 Seed, float SpineRadius, const UVoxelStrateManager* SM,
                                 FVoxelOpStack& OutStack, FVoxelOpContext& OutCtx)
    {
        switch (Type)
        {
        case ECaveGeneratorType::Maze:
            if (!Refs.Maze) { return false; }
            if (Refs.Maze->StrateTopWorldZ - Refs.Maze->StrateBottomWorldZ <= 0.0f) { return false; }
            OutCtx.StrateTopWorldZ    = Refs.Maze->StrateTopWorldZ;
            OutCtx.StrateBottomWorldZ = Refs.Maze->StrateBottomWorldZ;
            VoxelDensityOps::BuildMazeStack(OutStack, *Refs.Maze, Seed, SpineRadius, SM);
            return true;

        case ECaveGeneratorType::FlatPlain:
        case ECaveGeneratorType::CrystalChamber:
            // UN SEUL cas pour les deux, comme le `switch` de production : `GetSlabDensity` ne les
            // distingue pas non plus. Voir BuildSlabStack.
            if (!Refs.Slab) { return false; }
            if (Refs.Slab->StrateTopWorldZ - Refs.Slab->StrateBottomWorldZ <= 0.0f) { return false; }
            OutCtx.StrateTopWorldZ    = Refs.Slab->StrateTopWorldZ;
            OutCtx.StrateBottomWorldZ = Refs.Slab->StrateBottomWorldZ;
            VoxelDensityOps::BuildSlabStack(OutStack, *Refs.Slab, Seed, SpineRadius, SM);
            return true;

        case ECaveGeneratorType::SurfaceWorld:
            if (!Refs.Surface) { return false; }
            if (Refs.Surface->StrateTopWorldZ - Refs.Surface->StrateBottomWorldZ <= 0.0f) { return false; }
            OutCtx.StrateTopWorldZ    = Refs.Surface->StrateTopWorldZ;
            OutCtx.StrateBottomWorldZ = Refs.Surface->StrateBottomWorldZ;
            VoxelDensityOps::BuildSurfaceStack(
                OutStack, *Refs.Surface, Seed, SpineRadius, SM,
                Refs.SurfaceBiomeParams ? *Refs.SurfaceBiomeParams : TArray<FSurfaceGenerationParams>(),
                MoveTemp(Refs.BiomeField));
            return true;

        case ECaveGeneratorType::VerticalShafts:
            if (!Refs.Vert) { return false; }
            if (Refs.Vert->StrateTopWorldZ - Refs.Vert->StrateBottomWorldZ <= 0.0f) { return false; }
            OutCtx.StrateTopWorldZ    = Refs.Vert->StrateTopWorldZ;
            OutCtx.StrateBottomWorldZ = Refs.Vert->StrateBottomWorldZ;
            VoxelDensityOps::BuildVerticalShaftStack(OutStack, *Refs.Vert, Seed, SpineRadius, SM);
            return true;

        case ECaveGeneratorType::FloatingIslands:
            if (!Refs.Float) { return false; }
            if (Refs.Float->StrateTopWorldZ - Refs.Float->StrateBottomWorldZ <= 0.0f) { return false; }
            OutCtx.StrateTopWorldZ    = Refs.Float->StrateTopWorldZ;
            OutCtx.StrateBottomWorldZ = Refs.Float->StrateBottomWorldZ;
            VoxelDensityOps::BuildFloatingIslandStack(OutStack, *Refs.Float, Seed, SpineRadius, SM);
            return true;

        case ECaveGeneratorType::Underwater:
        case ECaveGeneratorType::TunnelNetwork:
            // Underwater EST TunnelNetwork plus un drapeau d'eau consommé côté rendu. Pas de garde
            // de strate dégénérée : `GetDensityWithParams` n'a pas d'early-out à reproduire.
            if (!Refs.Tunnel) { return false; }
            OutCtx.StrateTopWorldZ    = Refs.Tunnel->StrateTopWorldZ;
            OutCtx.StrateBottomWorldZ = Refs.Tunnel->StrateBottomWorldZ;
            VoxelDensityOps::BuildTunnelNetworkStack(
                OutStack, *Refs.Tunnel, Seed, SpineRadius, SM,
                /*bAppendStructuralPosts=*/true);
            return true;

        default:
            // `UsesOperatorStackForChunk` ne rend true que pour les archétypes portés (les 8), donc
            // on ne devrait jamais arriver ici. Si ça arrive : retomber sur le `switch` plutôt que
            // générer du vide — un monde faux est pire qu'un monde non porté.
            return false;
        }
    }
}

#if WITH_EDITOR
bool VF_BuildNativeStrateStackForCandidate(
    ECaveGeneratorType Archetype,
    const FVoxelStrateArchetypeParams& Params,
    int32 InSeed,
    float SpineRadius,
    float InWorldRadiusVoxels,
    float InEdgeSealThickness,
    const UVoxelStrateManager* StrateManager,
    FVoxelOpStack& OutStack,
    FVoxelOpContext& OutContext)
{
    FVoxelStackParamRefs Refs;
    switch (Archetype)
    {
    case ECaveGeneratorType::FlatPlain:
    case ECaveGeneratorType::CrystalChamber:
        Refs.Slab = &Params.SlabParams;
        break;
    case ECaveGeneratorType::Maze:
        Refs.Maze = &Params.MazeParams;
        break;
    case ECaveGeneratorType::SurfaceWorld:
        // The live surface path can additionally carry biome-specific parameter arrays and a
        // generator-owned biome field. The editor sanity check deliberately handles the exact
        // no-biome/native case only; the caller reports a biome target as unavailable rather than
        // comparing unlike fields.
        Refs.Surface = &Params.SurfaceParams;
        break;
    case ECaveGeneratorType::VerticalShafts:
        Refs.Vert = &Params.VerticalShaftParams;
        break;
    case ECaveGeneratorType::FloatingIslands:
        Refs.Float = &Params.FloatingIslandParams;
        break;
    case ECaveGeneratorType::Underwater:
    case ECaveGeneratorType::TunnelNetwork:
        Refs.Tunnel = &Params.TunnelNetworkParams;
        break;
    default:
        return false;
    }

    OutContext = FVoxelOpContext();
    OutContext.Seed = static_cast<uint32>(InSeed);
    OutContext.LayoutVersion = StrateManager != nullptr ? StrateManager->GetLayoutVersion() : 0;
    OutContext.WorldRadiusVoxels = InWorldRadiusVoxels;
    OutContext.EdgeSealThickness = InEdgeSealThickness;
    if (!VF_BuildOpStackForChunk(Archetype, Refs, InSeed, SpineRadius, StrateManager,
                                 OutStack, OutContext))
    {
        return false;
    }
    return true;
}
#endif

UVoxelGenerator::UVoxelGenerator()
    : DensityCacheOwnerId(GNextDensityCacheOwnerId.fetch_add(1, std::memory_order_relaxed) + 1)
{
}

void UVoxelGenerator::InitializeSettings(const UVoxelSettings* Settings)
{
    VF_ParseFastIsFiniteSwitch();
    // Les paramètres globaux sont copiés ici une seule fois : le chemin voxel ne doit pas
    // déréférencer l'asset de settings.
    Seed = Settings ? Settings->GetEffectiveWorldSeed() : 0;
    OriginSpineRadius = Settings ? Settings->GetEffectiveOriginSpineRadius() : 14.0f;
    WorldRadiusVoxels = Settings ? Settings->GetEffectiveWorldRadiusVoxels() : 0.0f;
    EdgeSealThickness = Settings ? Settings->EdgeSealThickness : 64.0f;
}

void UVoxelGenerator::BeginDensityBlock(FIntVector OriginVoxels, int32 Step,
                                         int32 SizeX, int32 SizeY, int32 SizeZ) const
{
    GVoxelDensityBlockSession.Begin(OriginVoxels, Step, SizeX, SizeY, SizeZ);
}

void UVoxelGenerator::EndDensityBlock() const
{
    GVoxelDensityBlockSession.End();
}

float UVoxelGenerator::GetDensityAt(float WorldX, float WorldY, float WorldZ) const
{
    TRACE_CPUPROFILER_EVENT_SCOPE(VoxelForge_GetDensityAt);
    VF_ParseFastIsFiniteSwitch();
    VF_ParseFusedEvaluatorSwitch();
    VF_ParseWormBlockSkipSwitch();
    VoxelDensityProfile::FScopedTimer DensityProfileTimer(
        VoxelDensityProfile::EBucket::GetDensityAt);
    VoxelDensityProfile::FScopedTimer DensityPrologueTimer(
        VoxelDensityProfile::EBucket::DensityPrologue);
    // ── STRATE SYSTEM ──
    // Query per-chunk params from the manager so each strate has different caves.
    float Result;

    // A vertical tree connector may need to stay open through the shared disturbance and
    // structural-post tail below. This is one bit for this density call, not cached voxel data.
    VoxelPassageGeometry::ResetVerticalShaftConnectorAirMarker();

    FIntVector ChunkCoord(
        FMath::FloorToInt(WorldX / CHUNK_SIZE),
        FMath::FloorToInt(WorldY / CHUNK_SIZE),
        FMath::FloorToInt(WorldZ / CHUNK_SIZE)
    );

    const bool bIsGapChunk = StrateManager && StrateManager->IsGapChunk(ChunkCoord);

    if (StrateManager && bIsGapChunk)
    {
        // SOLID BEDROCK gap between two strates. The auto-carved passages still tunnel
        // through it, but the (0,0) descent stays solid here so the player digs the gap
        // to reach the next layer. No caves, no spine, no vertical seal — just rock + passage
        // tube/landing;
        // the global XY edge seal is applied below for bounded worlds.
        DensityPrologueTimer.End();
        VoxelDensityProfile::FScopedTimer DensityCoreTimer(
            VoxelDensityProfile::EBucket::DensityCore);
        float Density = 8.0f;  // bedrock solidity (positive = solid)
        StrateManager->ApplyPassageModifier(
            Density, WorldX, WorldY, WorldZ, 8.0f, 0.0f);
        VF_ApplyXYEdgeSeal(Density, WorldX, WorldY, WorldRadiusVoxels, EdgeSealThickness, 8.0f);
        Result = -Density;
    }
    else if (StrateManager)
    {
        // ── PER-CHUNK PARAM CACHE ──
        // GetDensityAt runs per voxel AND ~6× more per surface vertex (gradient normals).
        // The generator type, the (boundary-blended) param struct, and the disturbance
        // params are identical for the whole chunk, yet resolving them re-runs a strate
        // lookup + copies large structs (and a ~60-field Lerp for blended cave chunks).
        // Cache them thread-locally, keyed by owner + chunk coord + layout version — refetch only
        // when one of those integer identities changes.
        thread_local uint64                  CP_OwnerId = 0;
        thread_local FIntVector              CP_Chunk(INT32_MAX, INT32_MAX, INT32_MAX);
        thread_local ECaveGeneratorType      CP_GenType = ECaveGeneratorType::TunnelNetwork;
        thread_local FStrateGenerationParams CP_Tunnel;
        // AUDIT §C2 — empreinte de `CP_Tunnel`, rafraîchie avec lui. Elle voyage jusqu'à la clé du
        // cache SDF de `GetDensityWithParams` pour qu'un chunk ne puisse plus être évalué contre
        // les salles d'un chunk voisin aux params blendés différemment.
        thread_local uint32                 CP_TunnelFP = 0xFFFFFFFFu;
        thread_local FSlabGenerationParams   CP_Slab;
        thread_local FMazeGenerationParams   CP_Maze;
        thread_local FSurfaceGenerationParams CP_Surface;
        thread_local FVerticalShaftParams    CP_Vert;
        thread_local FFloatingIslandParams   CP_Float;
        thread_local FStrateDisturbanceParams CP_Dist;
        // Biomes (SurfaceWorld for now): CP_BiomeCtx is the cheap per-chunk flatten;
        // CP_BiomeCache is the box-validated grid; CP_SurfaceBiomeParams holds each biome's
        // resolved surface params (override or strate fallback) parallel to CP_BiomeCtx.Biomes.
        thread_local FBiomeContext            CP_BiomeCtx;
        thread_local FChunkBiomeCache         CP_BiomeCache;
        thread_local TArray<FSurfaceGenerationParams> CP_SurfaceBiomeParams;
        // T1.a per-column surface cache: GSurfColCache (file-scope, shared with ClassifyTile).
        // Discriminates the surface cache by strate: same strate ⇒ identical heightfield params ⇒ columns
        // are shareable across the whole vertical chunk stack. Taken from the params themselves
        // (StrateBottomWorldZ is unique per stacked strate) so the key can never disagree with CP_Surface.
        thread_local int32                    CP_StrateKey = MIN_int32;
        // AUDIT C2 — la clé DOIT contenir la version de layout, pas seulement le chunk. Après un
        // RebuildStrates / une édition à chaud, StrateManager reconstruit le layout et bumpe la
        // version ; un worker dont CP_Chunk vaut encore ce chunk sauterait le refetch et
        // générerait avec les ANCIENS params. Comme RegenerateAllChunks recharge les MÊMES coords
        // de tuile, souvent sur les MÊMES workers, le cas est probable plutôt qu'exotique.
        // The key MUST include the layout version, not just the chunk coord. Symptom without it:
        // "I tweaked the strate asset, regenerated, and one patch kept the old shape."
        thread_local uint32                   CP_Version = 0xFFFFFFFFu;
        // OPSTACK Phase 1 — la pile d'opérateurs, construite dans le MÊME bloc de refetch que les
        // params (donc même clé owner+chunk+version, aucune logique d'invalidation en plus). Vide tant que
        // la strate n'a pas coché `bUseOperatorStack` ET que son archétype n'est pas porté.
        thread_local FVoxelOpStack            CP_OpStack;
        thread_local bool                     CP_UseOpStack = false;
        thread_local uint64                    CP_ManagerLifetimeId = 0;
        thread_local FTunnelNetworkDensityCacheEntry CP_TunnelDensityCache[TunnelDensityCacheSlotCount];
        thread_local FTunnelNetworkDensityCacheEntry* CP_ActiveTunnelDensityCache = nullptr;
        // Cooked-season recipes and editor candidate recipes share this immutable worker-local
        // hand-off. Recipe materialisation is runtime; rolling/selection remains editor-only.
        thread_local bool                       CP_UseCustomRecipe = false;
        thread_local int32                      CP_CustomRecipeSeed = 0;
        thread_local FVoxelStrateArchetypeParams CP_CustomParams;
        thread_local FVoxelOpStackRecipe        CP_CustomRecipe;
#if WITH_EDITOR
        // Editor-only composer override. The candidate recipe is copied into this worker-local
        // cache on the same versioned refetch as the native params; no worker reads mutable editor
        // state while the world is applying a new candidate.
        thread_local bool                       CP_UseComposerRegions = false;
        thread_local int32                      CP_ComposerSeed = 0;
        thread_local FVoxelStrateRegionManifest CP_ComposerRegions;
#endif

        const uint32 LayoutVersion = StrateManager->GetLayoutVersion();
        const uint64 ManagerLifetimeId = StrateManager->GetCacheLifetimeId();
        // TunnelNetwork/Underwater operator-stack parameters are selected from chunk Z only.
        // A coarse tile samples thousands of exact XY chunk keys, but those keys describe the
        // same immutable stack whenever the selected archetype is operator-stack backed.  Share
        // that prepared state by Z; the stack still queries the actual world position, and its
        // room-graph cache remains keyed by the real XY search window.  Legacy tunnel states keep
        // the full XYZ key because their native cache is built for one XY window.
        const ECaveGeneratorType QueryGeneratorType =
            StrateManager->GetGeneratorTypeForChunk(ChunkCoord);
        const bool bShareTunnelStackByZ = !bIsGapChunk
            && (QueryGeneratorType == ECaveGeneratorType::TunnelNetwork
                || QueryGeneratorType == ECaveGeneratorType::Underwater)
            && StrateManager->UsesOperatorStackForChunk(ChunkCoord);
        const FIntVector TunnelCacheKey = bShareTunnelStackByZ
            ? FIntVector(0, 0, ChunkCoord.Z) : ChunkCoord;
        FIntVector RequestedTileOrigin = FIntVector::ZeroValue;
        int32 RequestedTileStep = 1;
        int32 RequestedTileCells = 0;
        const bool bTileCacheContext = VoxelGenLOD::IsTileCacheWindowEnabled(
            !bShareTunnelStackByZ)
            && VoxelGenLOD::GetThreadTileCacheWindow(
                RequestedTileOrigin, RequestedTileStep, RequestedTileCells);
        FVoxelOpStack* ActiveOpStack = &CP_OpStack;
        FTunnelCoreCacheState* ActiveTunnelCoreCache = &GTunnelCoreCache;
        bool bLoadedTunnelDensityCache = false;
        FTunnelNetworkDensityCacheEntry* TunnelDensityCacheEntryForWrite = nullptr;

        // The table is open-addressed rather than one-slot direct-mapped. A direct map is very
        // cheap until a coarse tile's regular lattice happens to put two chunk keys in the same
        // bucket; then the two keys evict each other on every row and rebuild the prepared graph.
        // Probe deterministically to the first empty/stale slot, retaining the bounded memory cap.
        auto FindTunnelDensityCacheEntry = [&](const FIntVector& QueryChunk,
                                                FTunnelNetworkDensityCacheEntry*& OutEntry) -> bool
        {
            const int32 StartSlot = TunnelDensityCacheSlot(TunnelCacheKey);
            FTunnelNetworkDensityCacheEntry* ReuseEntry = nullptr;
            for (int32 Probe = 0; Probe < TunnelDensityCacheSlotCount; ++Probe)
            {
                const int32 Slot = (StartSlot + Probe) & (TunnelDensityCacheSlotCount - 1);
                FTunnelNetworkDensityCacheEntry& Candidate = CP_TunnelDensityCache[Slot];
                if (!Candidate.bValid || Candidate.OwnerId != DensityCacheOwnerId)
                {
                    if (ReuseEntry == nullptr) { ReuseEntry = &Candidate; }
                    if (!Candidate.bValid) { break; }
                    continue;
                }
                const bool bKeyMatches = Candidate.ManagerLifetimeId == ManagerLifetimeId
                    && Candidate.Chunk == TunnelCacheKey
                    && Candidate.LayoutVersion == LayoutVersion
                    && Candidate.bUseOpStack == bShareTunnelStackByZ
                    && Candidate.bUsesTileCacheWindow == bTileCacheContext
                    && (Candidate.GenType == ECaveGeneratorType::TunnelNetwork
                        || Candidate.GenType == ECaveGeneratorType::Underwater);
                if (bKeyMatches)
                {
                    OutEntry = &Candidate;
                    return true;
                }
            }
            OutEntry = ReuseEntry != nullptr
                ? ReuseEntry
                : &CP_TunnelDensityCache[StartSlot];
            return false;
        };

        if (!bIsGapChunk)
        {
            if (VoxelDensityProfile::AreCountersEnabled())
            {
                VoxelDensityProfile::AddCounter(
                    VoxelDensityProfile::ECounter::TunnelCacheLookup);
            }
            FTunnelNetworkDensityCacheEntry* CachedEntryPtr = nullptr;
            const bool bCacheKeyMatches = FindTunnelDensityCacheEntry(ChunkCoord, CachedEntryPtr);
            FTunnelNetworkDensityCacheEntry& CachedEntry = *CachedEntryPtr;
            TunnelDensityCacheEntryForWrite = CachedEntryPtr;
            if (bCacheKeyMatches)
            {
                if (VoxelDensityProfile::AreCountersEnabled())
                {
                    VoxelDensityProfile::AddCounter(
                        VoxelDensityProfile::ECounter::TunnelCacheHit);
                }
                if (CP_ActiveTunnelDensityCache != &CachedEntry)
                {
                    CP_OwnerId = CachedEntry.OwnerId;
                    CP_ManagerLifetimeId = CachedEntry.ManagerLifetimeId;
                    CP_Version = CachedEntry.LayoutVersion;
                    CP_Chunk = CachedEntry.Chunk;
                    CP_GenType = CachedEntry.GenType;
                    CP_Tunnel = CachedEntry.Tunnel;
                    CP_TunnelFP = CachedEntry.TunnelFingerprint;
                    CP_Dist = CachedEntry.Disturbance;
                    CP_UseOpStack = CachedEntry.bUseOpStack;
                    CP_UseCustomRecipe = false;
#if WITH_EDITOR
                    CP_UseComposerRegions = false;
#endif
                    CP_ActiveTunnelDensityCache = &CachedEntry;
                }
                ActiveOpStack = &CachedEntry.OpStack;
                ActiveTunnelCoreCache = &CachedEntry.TunnelCore;
                bLoadedTunnelDensityCache = true;
            }
            else if (VoxelDensityProfile::AreCountersEnabled())
            {
                VoxelDensityProfile::AddCounter(
                    VoxelDensityProfile::ECounter::TunnelCacheMiss);
            }
        }

        const bool bOwnerChanged = DensityCacheOwnerId != CP_OwnerId;
        const bool bManagerChanged = ManagerLifetimeId != CP_ManagerLifetimeId;
        if (!bLoadedTunnelDensityCache
            && (bOwnerChanged || bManagerChanged || ChunkCoord != CP_Chunk
                || LayoutVersion != CP_Version))
        {
            CP_ActiveTunnelDensityCache = nullptr;

            // La grille de biome est validée par une BOÎTE XY, qui ne dit rien du FBiomeContext
            // ayant servi à classer ses cellules : sur un changement de version elle est périmée
            // même si la boîte couvre encore la requête. Même invalidation quand le propriétaire
            // change : deux mondes peuvent partager version et coordonnées, jamais leur contexte.
            // The biome grid's XY box says nothing about its context. Owner changes invalidate it
            // too: two worlds may share version and coordinates, never cached params/context.
            if (bOwnerChanged || bManagerChanged || LayoutVersion != CP_Version)
            {
                CP_BiomeCache.Invalidate();
            }
            CP_OwnerId = DensityCacheOwnerId;
            CP_ManagerLifetimeId = ManagerLifetimeId;
            CP_Version = LayoutVersion;
            CP_Chunk   = ChunkCoord;
            CP_GenType = StrateManager->GetGeneratorTypeForChunk(ChunkCoord);
            CP_UseCustomRecipe = false;
            bool bAllowCustomRecipe = true;
#if WITH_EDITOR
            CP_UseComposerRegions = false;
            if (StrateManager->GetComposerRegionOverrideForChunk(
                ChunkCoord, CP_ComposerRegions))
            {
                CP_UseComposerRegions = true;
                bAllowCustomRecipe = false;
                CP_ComposerSeed = CP_ComposerRegions.Seed;
                if (CP_ComposerRegions.Regions.Num() > 0)
                {
                    CP_GenType = CP_ComposerRegions.Regions[0].Archetype;
                }
            }
#endif
            if (bAllowCustomRecipe
                && StrateManager->GetRecipeForChunk(
                    ChunkCoord, CP_CustomRecipeSeed, CP_GenType,
                    CP_CustomParams, CP_CustomRecipe))
            {
                CP_UseCustomRecipe = true;
            }
            switch (CP_GenType)
            {
            case ECaveGeneratorType::FlatPlain:
            case ECaveGeneratorType::CrystalChamber:
                CP_Slab    = StrateManager->GetSlabParamsForChunk(ChunkCoord);            break;
            case ECaveGeneratorType::Maze:
                CP_Maze    = StrateManager->GetMazeParamsForChunk(ChunkCoord);            break;
            case ECaveGeneratorType::SurfaceWorld:
                ResolveSurfaceChunkParams(ChunkCoord, CP_Surface, CP_BiomeCtx, CP_SurfaceBiomeParams);
                CP_StrateKey = FMath::RoundToInt(CP_Surface.StrateBottomWorldZ);
                break;
            case ECaveGeneratorType::VerticalShafts:
                CP_Vert    = StrateManager->GetVerticalShaftParamsForChunk(ChunkCoord);   break;
            case ECaveGeneratorType::FloatingIslands:
                CP_Float   = StrateManager->GetFloatingIslandParamsForChunk(ChunkCoord);  break;
            default: // TunnelNetwork / Underwater
                CP_Tunnel  = StrateManager->GetGenerationParams(ChunkCoord);
                // AUDIT §C2 — l'empreinte est calculée ICI, une fois par chunk, au seul endroit où
                // les params changent. `FStrateGenerationParams` est du POD pur (aucun TArray /
                // FString / pointeur), donc une CRC mémoire ne peut pas donner de FAUX POSITIF ; au
                // pire un octet de padding donne un faux MANQUE, c'est-à-dire une reconstruction de
                // cache. On se trompe du côté du CPU, jamais du côté d'une salle fausse.
                CP_TunnelFP = FCrc::MemCrc32(&CP_Tunnel, sizeof(CP_Tunnel));
                break;
            }
            CP_Dist = StrateManager->GetDisturbanceParamsForChunk(ChunkCoord);

            // ── OPSTACK Phase 1 : (re)construire la pile si cette strate l'a demandée. ──
            // Une seule branche ajoutée au chemin densité, et elle est FROIDE : la construction est
            // par chunk (comme le refetch de params juste au-dessus), jamais par voxel.
            CP_UseOpStack = StrateManager->UsesOperatorStackForChunk(ChunkCoord);
            CP_UseOpStack = CP_UseOpStack || CP_UseCustomRecipe;
#if WITH_EDITOR
            // A composer override is already an explicit, validated stack description.  It must
            // be evaluated even when the authored definition had the normal operator-stack opt-in
            // disabled; otherwise a multi-region candidate would be installed in the manager but
            // silently fall back to the old one-archetype switch.
            CP_UseOpStack = CP_UseOpStack || CP_UseComposerRegions;
#endif
            if (CP_UseOpStack)
            {
                CP_OpStack = FVoxelOpStack();   // move-assign : libère l'ancienne pile
                bool bBuiltSpecialStack = false;
#if WITH_EDITOR
                if (CP_UseComposerRegions)
                {
                    bBuiltSpecialStack = true;
                    FVoxelOpContext RegionContext;
                    if (VF_BuildStrateRegionStack(
                        CP_ComposerRegions, OriginSpineRadius, StrateManager,
                        CP_OpStack, RegionContext, nullptr))
                    {
                        RegionContext.ChunkCoord = ChunkCoord;
                        RegionContext.Step = 1;
                        RegionContext.LayoutVersion = LayoutVersion;
                        RegionContext.WorldRadiusVoxels = WorldRadiusVoxels;
                        RegionContext.EdgeSealThickness = EdgeSealThickness;
                        CP_OpStack.PrepareChunk(RegionContext);
                    }
                    else
                    {
                        CP_UseOpStack = false;
                    }
                }
#endif
                if (!bBuiltSpecialStack)
                {
                    if (CP_UseCustomRecipe)
                    {
                        FVoxelOpContext RecipeContext;
                        if (VF_BuildStackFromRecipe(
                            CP_CustomRecipe, CP_CustomParams, CP_CustomRecipeSeed,
                            OriginSpineRadius, StrateManager, CP_OpStack, RecipeContext, nullptr))
                        {
                            RecipeContext.ChunkCoord = ChunkCoord;
                            RecipeContext.Step = 1;
                            RecipeContext.LayoutVersion = LayoutVersion;
                            // The recipe builder supplies the candidate's vertical seal to the
                            // structural post. The XY edge seal is global and uses live settings.
                            RecipeContext.WorldRadiusVoxels = WorldRadiusVoxels;
                            RecipeContext.EdgeSealThickness = EdgeSealThickness;
                            CP_OpStack.PrepareChunk(RecipeContext);
                        }
                        else
                        {
                            // Import/load validates recipes before a season becomes active. Keep
                            // the fallback hole-safe if a future schema changes underneath it.
                            CP_UseOpStack = false;
                        }
                    }
                    else
                    {
                        FVoxelOpContext OpCtx;
                        OpCtx.ChunkCoord    = ChunkCoord;
                        OpCtx.Seed          = (uint32)Seed;
                        OpCtx.LayoutVersion = LayoutVersion;
                        OpCtx.WorldRadiusVoxels = WorldRadiusVoxels;
                        OpCtx.EdgeSealThickness = EdgeSealThickness;

                // ⚠️ LE MAPPING VIT DANS `VF_BuildOpStackForChunk` (haut de ce fichier) ET NULLE
                // PART AILLEURS — `ClassifyTile` appelle la MÊME fabrique. Un verdict de tuile issu
                // d'une pile construite autrement serait un trou. Ici on ne fait que fournir les
                // params déjà cherchés juste au-dessus.
                        FVoxelStackParamRefs Refs;
                        Refs.Slab   = &CP_Slab;
                        Refs.Maze   = &CP_Maze;
                        Refs.Vert   = &CP_Vert;
                        Refs.Float  = &CP_Float;
                        Refs.Tunnel = &CP_Tunnel;

                // SurfaceWorld : le champ de biomes est fabriqué ICI, du côté qui connaît le
                // générateur, et TRANSFÉRÉ à la pile. L'opérateur ne voit qu'une `IVoxelBiomeField`,
                // ce qui lui permet de devenir un asset en Phase 3 sans traîner le générateur.
                        TArray<FSurfaceGenerationParams> PerBiome;
                        if (CP_GenType == ECaveGeneratorType::SurfaceWorld)
                        {
                            Refs.Surface = &CP_Surface;
                            if (CP_BiomeCtx.IsValid() && CP_SurfaceBiomeParams.Num() > 0)
                            {
                                PerBiome = CP_SurfaceBiomeParams;
                                Refs.SurfaceBiomeParams = &PerBiome;
                                Refs.BiomeField = MakeUnique<FGeneratorBiomeField>(
                                    this, &CP_BiomeCtx, &CP_BiomeCache, ChunkCoord.Z);
                            }
                        }

                        CP_UseOpStack = VF_BuildOpStackForChunk(
                            CP_GenType, Refs, Seed, OriginSpineRadius,
                            StrateManager, CP_OpStack, OpCtx);

                // Le test appelle PrepareChunk, pas la production : c'est exactement la divergence
                // qui rend un opérateur vert en test et faux en jeu. Les sept `PrepareChunk`
                // actuels sont vides, donc ceci ne change RIEN aujourd'hui — c'est le point : le
                // premier opérateur qui hisse vraiment du travail par chunk doit trouver l'appel
                // déjà là. `Step` reste 1 : GetDensityAt ne connaît pas le pas d'échantillonnage
                // du mesher (voir le contrat T2.b dans VoxelDensityOp.h).
                // The test calls PrepareChunk and production did not — the exact divergence that
                // makes an op green in test and wrong in game. All seven bodies are empty today,
                // which is the point: the first op that hoists real per-chunk work must find the
                // call already here.
                        if (CP_UseOpStack) { CP_OpStack.PrepareChunk(OpCtx); }
                    }
                }
            }

            // The legacy path needs its prepared native tunnel-core cache.  An operator-stack
            // tunnel source publishes the same world-space result after EvalMC, so preparing a
            // second full graph/core cache here would only duplicate the work that made coarse
            // streaming expensive in the first place.  Custom/composer paths retain the legacy
            // cache unless their own stack supplies the hand-off.
            const bool bNativeTunnelCore = !CP_UseCustomRecipe
                && (CP_GenType == ECaveGeneratorType::TunnelNetwork
                    || CP_GenType == ECaveGeneratorType::Underwater);
#if WITH_EDITOR
            const bool bCanPrepareNativeTunnelCore = bNativeTunnelCore
                && (!CP_UseOpStack || CP_UseComposerRegions);
#else
            const bool bCanPrepareNativeTunnelCore = bNativeTunnelCore && !CP_UseOpStack;
#endif
            if (bCanPrepareNativeTunnelCore)
            {
                PrepareTunnelCoreCache(
                    *StrateManager, ChunkCoord, CP_Tunnel, CP_TunnelFP,
                    LayoutVersion, ManagerLifetimeId, DensityCacheOwnerId, GTunnelCoreCache);
            }
            else
            {
                GTunnelCoreCache.bValid = false;
            }

            const bool bNativeTunnelCacheCandidate = !CP_UseCustomRecipe
                && (CP_GenType == ECaveGeneratorType::TunnelNetwork
                    || CP_GenType == ECaveGeneratorType::Underwater);
#if WITH_EDITOR
            const bool bCanStoreTunnelDensityCache = bNativeTunnelCacheCandidate
                && !CP_UseComposerRegions
                // A native direct core uses the worker-global state below. Keeping it in one
                // per-exact-chunk entry would move the tile graph out and force the next chunk to
                // rebuild it. Operator stacks do not prepare this native core, so they retain the
                // existing prepared-stack cache even when the tile context is active.
                && (!bTileCacheContext || CP_UseOpStack);
#else
            const bool bCanStoreTunnelDensityCache = bNativeTunnelCacheCandidate
                && (!bTileCacheContext || CP_UseOpStack);
#endif
            if (!bLoadedTunnelDensityCache && bCanStoreTunnelDensityCache)
            {
                FTunnelNetworkDensityCacheEntry& CachedEntry = TunnelDensityCacheEntryForWrite
                    ? *TunnelDensityCacheEntryForWrite
                    : CP_TunnelDensityCache[TunnelDensityCacheSlot(ChunkCoord)];
                CachedEntry.bValid = false;
                CachedEntry.OwnerId = DensityCacheOwnerId;
                CachedEntry.ManagerLifetimeId = ManagerLifetimeId;
                CachedEntry.Chunk = TunnelCacheKey;
                CachedEntry.LayoutVersion = LayoutVersion;
                CachedEntry.GenType = CP_GenType;
                CachedEntry.Tunnel = CP_Tunnel;
                CachedEntry.TunnelFingerprint = CP_TunnelFP;
                CachedEntry.Disturbance = CP_Dist;
                CachedEntry.bUseOpStack = CP_UseOpStack;
                CachedEntry.bUsesTileCacheWindow = bTileCacheContext;
                CachedEntry.OpStack = MoveTemp(CP_OpStack);
                CachedEntry.TunnelCore = MoveTemp(GTunnelCoreCache);
                GTunnelCoreCache = FTunnelCoreCacheState();
                CachedEntry.bValid = true;
                CP_ActiveTunnelDensityCache = &CachedEntry;
                ActiveOpStack = &CachedEntry.OpStack;
                ActiveTunnelCoreCache = &CachedEntry.TunnelCore;
                ReportTunnelDensityCacheFootprint(CP_TunnelDensityCache);
            }
            else if (!bLoadedTunnelDensityCache)
            {
                ActiveOpStack = &CP_OpStack;
                ActiveTunnelCoreCache = &GTunnelCoreCache;
            }

        }

        DensityPrologueTimer.End();
        VoxelDensityProfile::FScopedTimer DensityCoreTimer(
            VoxelDensityProfile::EBucket::DensityCore);

        bool bUsedOpBlockSample = false;
        float BlockDensity = 0.f;
        bool bBlockHasTunnelCore = false;
        float BlockTunnelCoreSDF = FLT_MAX;
        bool bBlockTunnelCoreSupportFloor = false;
        bool bBlockTunnelCoreRoomFloor = false;
        bool bUsedFusedEvaluator = false;
        FTunnelCoreWorldEvaluation FusedTunnelCore;

        // Le seul point d'entrée de la pile dans le chemin de production. Elle rend la convention
        // MC (négatif = solide) comme les fonctions d'archétype, donc les disturbances et la couche
        // de diff qui suivent ne voient aucune différence.
        if (!VoxelDensityAblation::IsTunnelCoreOff() && CP_UseOpStack)
        {
            const bool bCanUseFusedEvaluator =
                GVoxelForgeUseFusedEvaluator != 0
                && !CP_UseCustomRecipe
                && (CP_GenType == ECaveGeneratorType::TunnelNetwork
                    || CP_GenType == ECaveGeneratorType::Underwater)
#if WITH_EDITOR
                && !CP_UseComposerRegions
#endif
                && ActiveOpStack->GetFusedEvaluator()
                    == EVoxelOpFusedEvaluator::TunnelNetwork;
            if (bCanUseFusedEvaluator)
            {
                // This is the lowered canonical graph.  It returns the same MC core as the
                // interpreted stack, plus the source's structural hand-off, without allocating
                // FVoxelOpSample or traversing the graph one operator at a time for this voxel.
                bUsedFusedEvaluator = true;
                VoxelDensityProfile::FScopedTimer FusedEvaluatorTimer(
                    VoxelDensityProfile::EBucket::FusedEvaluator);
                Result = GetDensityWithParams(
                    WorldX, WorldY, WorldZ, CP_Tunnel, CP_TunnelFP, LayoutVersion,
                    /*bApplyLegacyStructuralPosts=*/false,
                    &FusedTunnelCore,
                    /*bCollectFusedDiagnostics=*/true);
            }

            const bool bIntegerLatticePoint =
                WorldX == FMath::FloorToFloat(WorldX)
                && WorldY == FMath::FloorToFloat(WorldY)
                && WorldZ == FMath::FloorToFloat(WorldZ);
            const bool bBlockSafeGenerator =
                CP_GenType == ECaveGeneratorType::TunnelNetwork
                || CP_GenType == ECaveGeneratorType::Underwater;
            if (!bUsedFusedEvaluator && bIntegerLatticePoint && bBlockSafeGenerator
                && GVoxelDensityBlockSession.bActive)
            {
                // Keep the rich hand-off object in the fallback branch.  The lowered evaluator
                // must not construct a per-sample FVoxelOpSample merely because this compatibility
                // path still needs one when the block session is enabled.
                FVoxelOpSample BlockCoreSample;
                {
                    VoxelDensityProfile::FScopedTimer OperatorBlockTimer(
                        VoxelDensityProfile::EBucket::OperatorBlock);
                    bUsedOpBlockSample = GVoxelDensityBlockSession.FillChunk(
                        *ActiveOpStack,
                        ChunkCoord,
                        FIntVector(
                            FMath::RoundToInt(WorldX),
                            FMath::RoundToInt(WorldY),
                            FMath::RoundToInt(WorldZ)),
                        BlockCoreSample);
                }
                if (bUsedOpBlockSample)
                {
                    BlockDensity = BlockCoreSample.Density;
                    bBlockHasTunnelCore = BlockCoreSample.bHasTunnelCoreWorldEvaluation;
                    BlockTunnelCoreSDF = BlockCoreSample.TunnelCoreWorldSDF;
                    bBlockTunnelCoreSupportFloor = BlockCoreSample.bTunnelCoreSupportFloor;
                    bBlockTunnelCoreRoomFloor = BlockCoreSample.bTunnelCoreRoomFloor;
                }
            }
            if (!bUsedFusedEvaluator)
            {
                if (bUsedOpBlockSample)
                {
                    Result = -BlockDensity;
                }
                else
                {
                    VoxelDensityProfile::FScopedTimer InterpretedOpStackTimer(
                        VoxelDensityProfile::EBucket::InterpretedOpStack);
                    Result = ActiveOpStack->EvalMC(WorldX, WorldY, WorldZ);
                }
            }
        }
        else switch (CP_GenType)
        {
        case ECaveGeneratorType::FlatPlain:
        case ECaveGeneratorType::CrystalChamber:
            Result = GetSlabDensity(WorldX, WorldY, WorldZ, CP_Slab);                 break;
        case ECaveGeneratorType::Maze:
            Result = GetMazeDensity(WorldX, WorldY, WorldZ, CP_Maze);                 break;
        case ECaveGeneratorType::SurfaceWorld:
        {
            // Integer XY (the density grid) → reuse the column down its whole Z extent (T1.a).
            // Fractional XY (gradient-normal samples) → compute directly (no cache key).
            if (WorldX == FMath::FloorToFloat(WorldX) && WorldY == FMath::FloorToFloat(WorldY))
            {
                const int32 IX = (int32)WorldX, IY = (int32)WorldY;
                // XY-keyed LRU box (shared down the whole vertical strate stack). Acquire centres a box on
                // the first sample so the rest of the chunk's queries — incl. the ±Step margin ring — hit.
                FSurfaceColumnBox& Box = GSurfColCache.Acquire(IX, IY, CP_StrateKey, Seed, LayoutVersion);
                const int32 CI = (IY - Box.BaseY) * FSurfaceColumnBox::Dim + (IX - Box.BaseX);
                if (!Box.Computed[CI])
                {
                    ComputeSurfaceColumn(WorldX, WorldY, ChunkCoord.Z, CP_Surface, CP_BiomeCtx,
                        CP_SurfaceBiomeParams, CP_BiomeCache,
                        Box.Cols[CI].TerrainZ, Box.Cols[CI].CeilSurf,
                        Box.Cols[CI].OverhangAmp, Box.Cols[CI].DirX, Box.Cols[CI].DirY);
                    Box.Computed[CI] = true;
                }
                const FSurfaceColumn& Col = Box.Cols[CI];
                Result = SurfaceDensityFromColumn(WorldX, WorldY, WorldZ,
                    Col.TerrainZ, Col.CeilSurf, Col.OverhangAmp,
                    Col.DirX, Col.DirY, CP_Surface);
            }
            else
            {
                float TerrainZ, CeilSurf, OverhangAmp, DirX, DirY;
                ComputeSurfaceColumn(WorldX, WorldY, ChunkCoord.Z, CP_Surface, CP_BiomeCtx,
                    CP_SurfaceBiomeParams, CP_BiomeCache, TerrainZ, CeilSurf,
                    OverhangAmp, DirX, DirY);
                Result = SurfaceDensityFromColumn(WorldX, WorldY, WorldZ,
                    TerrainZ, CeilSurf, OverhangAmp, DirX, DirY, CP_Surface);
            }
            break;
        }
        case ECaveGeneratorType::VerticalShafts:
            Result = GetVerticalShaftDensity(WorldX, WorldY, WorldZ, CP_Vert);        break;
        case ECaveGeneratorType::FloatingIslands:
            Result = GetFloatingIslandDensity(WorldX, WorldY, WorldZ, CP_Float);      break;
        case ECaveGeneratorType::Underwater:
        case ECaveGeneratorType::TunnelNetwork:
        default:
            // Underwater shares tunnel rock (water table is a render-side overlay).
            Result = GetDensityWithParams(WorldX, WorldY, WorldZ, CP_Tunnel,
                                          CP_TunnelFP, LayoutVersion,
                                          /*bApplyLegacyStructuralPosts=*/false);       break;
        }
        DensityCoreTimer.End();

        // Resolve the graph's authored floor before the disturbance pass.  The operator-stack
        // source has already computed this exact result while publishing its sample; native
        // tunnel generation uses the prepared immutable cache.  Keeping the result here means the
        // disturbance layer can respect the shape-level floor and the common tail can reuse the
        // same query instead of discovering a floor only after it has been damaged.
        FTunnelCoreWorldEvaluation PreDisturbanceTunnelCore;
        bool bHavePreDisturbanceTunnelCore = false;
        if (CP_UseOpStack)
        {
            if (bUsedFusedEvaluator)
            {
                PreDisturbanceTunnelCore = FusedTunnelCore;
                bHavePreDisturbanceTunnelCore = true;
            }
            else if (bUsedOpBlockSample && bBlockHasTunnelCore)
            {
                PreDisturbanceTunnelCore.SDF = BlockTunnelCoreSDF;
                PreDisturbanceTunnelCore.bSupportFloor = bBlockTunnelCoreSupportFloor;
                PreDisturbanceTunnelCore.bRoomFloor = bBlockTunnelCoreRoomFloor;
                bHavePreDisturbanceTunnelCore = true;
            }
            else
            {
                bHavePreDisturbanceTunnelCore = ActiveOpStack->TryGetLastTunnelCoreWorldEvaluation(
                    PreDisturbanceTunnelCore);
            }
        }
        if (!VoxelDensityAblation::IsTunnelCoreOff()
            && !bHavePreDisturbanceTunnelCore && ActiveTunnelCoreCache->bValid)
        {
            const FTunnelSupportFloorColumn* SupportColumn = nullptr;
            FTunnelSupportFloorColumn EmptySupportColumn;
            const bool bIntegerXY =
                WorldX == FMath::FloorToFloat(WorldX)
                && WorldY == FMath::FloorToFloat(WorldY);
            if (bIntegerXY)
            {
                const int32 IX = FMath::FloorToInt(WorldX);
                const int32 IY = FMath::FloorToInt(WorldY);
                SupportColumn = FindTunnelSupportColumn(
                    ActiveTunnelCoreCache->Cache, IX, IY, EmptySupportColumn);
            }

            VoxelDensityProfile::FScopedTimer ProfileTimer(
                VoxelDensityProfile::EBucket::TunnelCorePosts);
            PreDisturbanceTunnelCore = VoxelCaveMorphology::EvaluateTunnelCoreWorld(
                WorldX, WorldY, WorldZ, ActiveTunnelCoreCache->Cache, SupportColumn,
                VoxelGenLOD::ShouldUseSpatialIndex(
                    CP_UseOpStack ? bUsedFusedEvaluator : true));
            bHavePreDisturbanceTunnelCore = true;
        }
        const bool bProtectAuthoredTunnelFloor = bHavePreDisturbanceTunnelCore
            && (PreDisturbanceTunnelCore.bSupportFloor
                || PreDisturbanceTunnelCore.bRoomFloor);

        // Disturbance layer (the "wow" post-process) — cached params, MC convention. The
        // vertical shaft tree opens a walkable route before this shared pass, so a bridge/ridge
        // must not refill it. Both native and operator-stack paths use the same marker helper.
        VoxelDensityProfile::FScopedTimer DensityDisturbancesTimer(
            VoxelDensityProfile::EBucket::DensityDisturbances);
        const bool bProtectVerticalShaftAir =
            VoxelPassageGeometry::VerticalShaftConnectorAirMarker();
        ApplyDisturbances(Result, WorldX, WorldY, WorldZ, CP_Dist, (uint32)Seed,
            bProtectVerticalShaftAir, bProtectAuthoredTunnelFloor);
        DensityDisturbancesTimer.End();

        // A disturbance is allowed to add visual detail, but it must not refill the landing's
        // measured air volume or turn its support slab back into a hole. Reassert the landing
        // air first, then the structural floor, in MC space before the final XY seal. The two
        // calls use the same landing geometry already evaluated by the legacy and op-stack post.
        VoxelDensityProfile::FScopedTimer DensityStructuralPostsTimer(
            VoxelDensityProfile::EBucket::DensityStructuralPosts);
        VoxelDensityProfile::FScopedTimer StructuralTailTimer(
            VoxelDensityProfile::EBucket::StructuralTail);
        float LandingBaseDensity = CP_Dist.BaseDensity;
#if WITH_EDITOR
        // A composer parent can deliberately use a different structural base than the authored
        // definition. The landing floor was already applied by PassageCarveOp with that parent
        // base; use the same value for this post-disturbance MC backstop or live-vs-direct stack
        // evaluation would differ exactly on floor voxels.
        if (CP_UseComposerRegions && CP_ComposerRegions.bHasGlobalStructuralParams)
        {
            LandingBaseDensity = CP_ComposerRegions.BaseDensity;
        }
        else
#endif
        if (CP_UseCustomRecipe)
        {
            switch (CP_CustomRecipe.StructuralParamBlock)
            {
            case EVoxelStrateParamBlock::TunnelNetwork:
                LandingBaseDensity = CP_CustomParams.TunnelNetworkParams.BaseDensity;
                break;
            case EVoxelStrateParamBlock::Slab:
                LandingBaseDensity = CP_CustomParams.SlabParams.BaseDensity;
                break;
            case EVoxelStrateParamBlock::Maze:
                LandingBaseDensity = CP_CustomParams.MazeParams.BaseDensity;
                break;
            case EVoxelStrateParamBlock::Surface:
                LandingBaseDensity = CP_CustomParams.SurfaceParams.BaseDensity;
                break;
            case EVoxelStrateParamBlock::VerticalShaft:
                LandingBaseDensity = CP_CustomParams.VerticalShaftParams.BaseDensity;
                break;
            case EVoxelStrateParamBlock::FloatingIsland:
                LandingBaseDensity = CP_CustomParams.FloatingIslandParams.BaseDensity;
                break;
            default:
                break;
            }
        }
        else
        {
            switch (CP_GenType)
            {
            case ECaveGeneratorType::FlatPlain:
            case ECaveGeneratorType::CrystalChamber:
                LandingBaseDensity = CP_Slab.BaseDensity;
                break;
            case ECaveGeneratorType::Maze:
                LandingBaseDensity = CP_Maze.BaseDensity;
                break;
            case ECaveGeneratorType::SurfaceWorld:
                LandingBaseDensity = CP_Surface.BaseDensity;
                break;
            case ECaveGeneratorType::VerticalShafts:
                LandingBaseDensity = CP_Vert.BaseDensity;
                break;
            case ECaveGeneratorType::FloatingIslands:
                LandingBaseDensity = CP_Float.BaseDensity;
                break;
            case ECaveGeneratorType::TunnelNetwork:
            case ECaveGeneratorType::Underwater:
            default:
                LandingBaseDensity = CP_Tunnel.BaseDensity;
                break;
            }
        }
        // Origin rooms are structural too. Reassert their air before either support writer so a
        // bridge/ridge cannot plug a landing, while the two floor writers remain last.
        {
            VoxelDensityProfile::FScopedTimer ProfileTimer(
                VoxelDensityProfile::EBucket::PassageLandingAir);
            VF_ApplyOriginLandingAirMC(
                Result, WorldX, WorldY, WorldZ,
                CP_Dist.StrateTopWorldZ, CP_Dist.StrateBottomWorldZ,
                CP_Dist.BoundarySealThickness, LandingBaseDensity, OriginSpineRadius);
        }
        {
            VoxelDensityProfile::FScopedTimer ProfileTimer(
                VoxelDensityProfile::EBucket::PassageLandingFloor);
            VF_ApplyOriginLandingFloorMC(
                Result, WorldX, WorldY, WorldZ,
                CP_Dist.StrateTopWorldZ, CP_Dist.StrateBottomWorldZ,
                CP_Dist.BoundarySealThickness, LandingBaseDensity, OriginSpineRadius);
        }
        {
            VoxelDensityProfile::FScopedTimer ProfileTimer(
                VoxelDensityProfile::EBucket::PassageStructuralPosts);
            StrateManager->ApplyPassageStructuralPostsMC(
                Result, WorldX, WorldY, WorldZ, LandingBaseDensity,
                CP_Dist.BoundarySealThickness, bProtectAuthoredTunnelFloor);
        }
        // Disturbance features are authored as a generic MC-space post and may add a ridge or
        // bridge over a graph tunnel. Reassert the native cached tunnel core here, after every
        // solid floor writer but before the global XY seal. This cache is built once per chunk,
        // never once per voxel.
        if (!VoxelDensityAblation::IsTunnelCoreOff()
            && (ActiveTunnelCoreCache->bValid || CP_UseOpStack))
        {
            FTunnelCoreWorldEvaluation TunnelCore;
            bool bHaveTunnelCore = bHavePreDisturbanceTunnelCore;
            if (bHaveTunnelCore)
            {
                TunnelCore = PreDisturbanceTunnelCore;
            }
            if (!bHaveTunnelCore && ActiveTunnelCoreCache->bValid)
            {
                const FTunnelSupportFloorColumn* SupportColumn = nullptr;
                FTunnelSupportFloorColumn EmptySupportColumn;
                const bool bIntegerXY =
                    WorldX == FMath::FloorToFloat(WorldX)
                    && WorldY == FMath::FloorToFloat(WorldY);
                if (bIntegerXY)
                {
                    const int32 IX = FMath::FloorToInt(WorldX);
                    const int32 IY = FMath::FloorToInt(WorldY);
                    SupportColumn = FindTunnelSupportColumn(
                        ActiveTunnelCoreCache->Cache, IX, IY, EmptySupportColumn);
                }

                VoxelDensityProfile::FScopedTimer ProfileTimer(
                    VoxelDensityProfile::EBucket::TunnelCorePosts);
                TunnelCore = VoxelCaveMorphology::EvaluateTunnelCoreWorld(
                    WorldX, WorldY, WorldZ, ActiveTunnelCoreCache->Cache,
                    SupportColumn,
                    VoxelGenLOD::ShouldUseSpatialIndex(
                        CP_UseOpStack ? bUsedFusedEvaluator : true));
                bHaveTunnelCore = true;
            }
            if (bHaveTunnelCore)
            {
                const bool bTunnelSupportFloor = TunnelCore.bSupportFloor;
                // The graph floor is now composed here, after disturbances and passage writers.
                // The source has already baked the floor profile and published the exact same
                // core result; this final ownership step only changes an actually-air sample.
                // Previously the operator stack raised every authored floor sample to a strong
                // internal density before those writers ran, which made the protection counter
                // fire for solid samples as well as genuine breaches.
                if (TunnelCore.bRoomFloor)
                {
                    const float StructuralSolidDensity =
                        -FMath::Max(LandingBaseDensity * 2.0f, 1.0f);
                    if (VoxelDensityProfile::AreCountersEnabled())
                    {
                        if (Result > 0.0f)
                        {
                            VoxelDensityProfile::AddCounter(
                                VoxelDensityProfile::ECounter::TunnelRoomFloorBackstopFires);
                        }
                    }
                    // A tunnel that penetrates a room cannot reassert its own floor through the
                    // room's floor. The morphology query marks only the finite room support band;
                    // preserve it after every generic post-disturbance writer.
                    Result = FMath::Min(
                        Result,
                        StructuralSolidDensity);
                }
                // The swept capsule owns the relief profile. This finite support band is now the
                // final composition of that authored shape, after every writer, rather than a
                // pre-disturbance clamp in FVoxelOpStack::EvalSample.
                if (bTunnelSupportFloor)
                {
                    const float StructuralSolidDensity =
                        -FMath::Max(LandingBaseDensity * 2.0f, 1.0f);
                    if (VoxelDensityProfile::AreCountersEnabled())
                    {
                        if (Result > 0.0f)
                        {
                            VoxelDensityProfile::AddCounter(
                                VoxelDensityProfile::ECounter::TunnelSupportFloorBackstopFires);
                        }
                    }
                    Result = FMath::Min(
                        Result,
                        StructuralSolidDensity);
                }
                const float CoreSDF = TunnelCore.SDF;
                if (!bTunnelSupportFloor
                    && CoreSDF < -VoxelPassageGeometry::CaveTunnelAirCoreInsetVoxels)
                {
                    Result = FMath::Max(
                        Result,
                        FMath::Max(LandingBaseDensity * 2.0f, 1.0f));
                }

                // The graph tunnel can overlap an inter-strate landing at a room mouth. Its air
                // backstop is allowed to reopen the tunnel, but the landing's proved support floor
                // must own the final floor band; otherwise the graph post can erase the only support
                // surface at the mouth and leave the player-fit graph with a disconnected pocket.
                {
                    VoxelDensityProfile::FScopedTimer ProfileTimer(
                        VoxelDensityProfile::EBucket::PassageLandingFloor);
                    VF_ApplyOriginLandingFloorMC(
                        Result, WorldX, WorldY, WorldZ,
                        CP_Dist.StrateTopWorldZ, CP_Dist.StrateBottomWorldZ,
                        CP_Dist.BoundarySealThickness, LandingBaseDensity, OriginSpineRadius);
                }
                {
                    VoxelDensityProfile::FScopedTimer ProfileTimer(
                        VoxelDensityProfile::EBucket::PassageLandingFloor);
                    StrateManager->ApplyPassageLandingFloorMC(
                        Result, WorldX, WorldY, WorldZ, LandingBaseDensity);
                }
                {
                    VoxelDensityProfile::FScopedTimer ProfileTimer(
                        VoxelDensityProfile::EBucket::PassageLandingRoomFloor);
                    StrateManager->ApplyPassageLandingRoomFloorMC(
                        Result, WorldX, WorldY, WorldZ, LandingBaseDensity);
                }
            }
        }
        // Inter-strate passages own the same final D-floor contract as graph tunnels.  This is
        // composed from the immutable construction-time profile after generic writers and the
        // graph overlap, so the legacy passage support slab can be disabled without losing the
        // floor to a disturbance refill.
        {
            VoxelDensityProfile::FScopedTimer ProfileTimer(
                VoxelDensityProfile::EBucket::PassageLandingFloor);
            StrateManager->ApplyPassageNativeFloorMC(
                Result, WorldX, WorldY, WorldZ, LandingBaseDensity);
        }
        StructuralTailTimer.End();
        DensityStructuralPostsTimer.End();
    }
    else
    {
        // ── FALLBACK (no strate manager) ──
        // Use default TunnelNetwork params — produces generic caves.
        // `static` : ces params sont constants (construction par défaut), donc leur empreinte l'est
        // aussi. La calculer une fois évite un CRC par voxel sur un chemin qui n'en a aucun besoin.
        // `LayoutVersion = 0` : sans `StrateManager` il n'y a pas de layout, donc rien qui puisse
        // périmer — et l'empreinte constante suffit à distinguer ce cache de tous les autres.
        DensityPrologueTimer.End();
        VoxelDensityProfile::FScopedTimer DensityCoreTimer(
            VoxelDensityProfile::EBucket::DensityCore);
        static const FStrateGenerationParams FallbackParams;
        static const uint32 FallbackFP = FCrc::MemCrc32(&FallbackParams, sizeof(FallbackParams));
        Result = GetDensityWithParams(WorldX, WorldY, WorldZ, FallbackParams, FallbackFP, 0);
        DensityCoreTimer.End();
    }

    // The edge is the final generated structural invariant. Passage carving is ordered before it
    // in every archetype/stack, and this MC-facing pass also covers out-of-layout air plus any
    // disturbance that might otherwise carve back into the shell. Player edits remain separate
    // below and are deliberately still the user override path.
    VoxelDensityProfile::FScopedTimer DensityBoundarySealTimer(
        VoxelDensityProfile::EBucket::DensityBoundarySeal);
    {
        VoxelDensityProfile::FScopedTimer ProfileTimer(
            VoxelDensityProfile::EBucket::XYEdgeSealOp);
        VF_ApplyXYEdgeSealMC(Result, WorldX, WorldY, WorldRadiusVoxels, EdgeSealThickness, 8.0f);
    }
    DensityBoundarySealTimer.End();

    //=========================================================================
    // PLAYER MODIFICATIONS (diff layer)
    //=========================================================================
    // Applied LAST — player carving/filling overrides everything.
    // The diff layer returns density in internal convention (negative = carve).
    // Since Result is already in MC convention (negative = solid, positive = air),
    // we subtract the diff offset:
    //   Carve (diff < 0) → Result -= negative → Result increases → more air ✓
    //   Fill  (diff > 0) → Result -= positive → Result decreases → more solid ✓
    VoxelDensityProfile::FScopedTimer DensityDiffLayerTimer(
        VoxelDensityProfile::EBucket::DensityDiffLayer);
    if (DiffLayer && DiffLayer->HasAnyMods())
    {
        // ── PER-CHUNK MOD SNAPSHOT ──
        // The old HasModifications + GetDensityOffset pair took the diff layer's RWLock + a TMap find
        // TWICE per voxel once any carve existed (~86k lock ops per tile task — during carve gameplay,
        // exactly when re-mesh latency matters). Snapshot a chunk's mod list ONCE per (chunk, version)
        // on this worker and evaluate it lock-free. DIRECT-MAPPED by the chunk coord's low bits (2 bits
        // per axis → 64 slots): the mesher's ±1 margin ring touches up to 27 neighbouring chunk coords
        // per tile, and any two coords within ±3 of each other land in DIFFERENT slots — so a tile task
        // never thrashes its own working set. Version bump (carve / clear) invalidates lazily per slot.
        struct FDiffSlot
        {
            FIntVector Chunk = FIntVector(INT32_MAX, INT32_MAX, INT32_MAX);
            uint32     Version = 0;
            TArray<FVoxelModification> Mods;
        };
        static thread_local FDiffSlot DiffSlots[64];

        const uint32 V = DiffLayer->GetModsVersion();
        FDiffSlot& Slot = DiffSlots[(ChunkCoord.X & 3) | ((ChunkCoord.Y & 3) << 2) | ((ChunkCoord.Z & 3) << 4)];
        if (Slot.Chunk != ChunkCoord || Slot.Version != V)
        {
            Slot.Chunk = ChunkCoord;
            Slot.Version = V;
            DiffLayer->GetChunkModsSnapshot(ChunkCoord, Slot.Mods);   // ONE lock per chunk, not per voxel
        }
        if (Slot.Mods.Num() > 0)
        {
            Result -= UVoxelDiffLayer::EvaluateMods(Slot.Mods, WorldX, WorldY, WorldZ);
        }
    }
    DensityDiffLayerTimer.End();

    VoxelDensityProfile::FScopedTimer DensityTailTimer(
        VoxelDensityProfile::EBucket::DensityTail);
    VoxelPassageGeometry::ResetVerticalShaftConnectorAirMarker();
    return Result;
}

float UVoxelGenerator::GetDensityWithParams(float WorldX, float WorldY, float WorldZ,
                                             const FStrateGenerationParams& Params,
                                             uint32 ParamsFingerprint, uint32 LayoutVersion,
                                             bool bApplyLegacyStructuralPosts,
                                             FTunnelCoreWorldEvaluation* OutTunnelCore,
                                             bool bCollectFusedDiagnostics) const
{
    //=========================================================================
    // STRATE DENSITY FUNCTION (Morphology Pipeline)
    //=========================================================================
    // The density pipeline for underground caves:
    //
    //   1.  Vertical scale
    //   2.  Base density (everything starts solid)
    //   3.  Cave warp: domain warp coordinates before SDF (bends rooms/tunnels organically)
    //   4.  SDF morphology: rooms + tunnels carve the cave structure (using warped coords)
    //   4b. Surface roughness: 3D noise near cave walls (fBM/Ridged/Cellular + domain warp)
    //   4c. Terrain operations: terracing, layer lines, ribbing, cliff, scallop, arch, overhangs
    //   4d. Columns/Pillars: hash-placed vertical cylinders adding solid rock
    //   4e. Pits/Shafts: hash-placed tapered vertical voids carved into cave floors
    //   4f. Chimneys/Shafts: hash-placed tapered vertical voids carved UPWARD
    //   4g. Domes: hemispherical ceiling sculpting in cave chambers
    //   4h. Pinch/Bottleneck: ellipsoidal passage narrowing for chokepoints
    //   5.  Worm tunnels: additional organic connectivity
    //   6.  Boundary seal: solid rock at strate top/bottom
    //   7.  Modifiers: passages between strates (punch through vertical seals)
    //   8.  XY edge seal: global bounded-world shell wins over passages at the rim
    //
    // Convention: positive density = solid, negative = air (internally).
    // At the end, we negate for the MC table (negative = solid there).
    //=========================================================================

    if (OutTunnelCore != nullptr)
    {
        *OutTunnelCore = FTunnelCoreWorldEvaluation();
    }
    if (bCollectFusedDiagnostics && VoxelDensityProfile::AreCountersEnabled())
    {
        VoxelDensityProfile::AddCounter(
            VoxelDensityProfile::ECounter::FusedTunnelSamples);
    }

    const uint32 SeedU = (uint32)Seed;

    //=========================================================================
    // STEP 1: VERTICAL SCALE
    //=========================================================================
    float EffectiveZ = WorldZ;
    if (Params.VerticalScale != 1.0f && Params.VerticalScale > 0.0f)
    {
        EffectiveZ = WorldZ / Params.VerticalScale;
    }

    //=========================================================================
    // STEP 2: BASE DENSITY (everything starts solid)
    //=========================================================================
    float Density = Params.BaseDensity;

    //=========================================================================
    // STEP 3: CAVE WARP (domain warp the SDF skeleton)
    //=========================================================================
    // This is the KEY step that makes caves look natural instead of graph-like.
    //
    // The SDF morphology places rooms as geometric primitives (ellipsoids, boxes)
    // connected by straight capsule tunnels. Without warping, you can clearly see
    // the room-corridor-room graph structure — it looks artificial.
    //
    // By warping the world coordinates BEFORE evaluating the SDF, we bend the
    // entire cave field. Rooms become irregular blobs, tunnels become winding
    // passages. The skeleton is still there as the backbone, but noise pushes
    // and pulls it into shapes that look carved by geological forces.
    //
    // Two octaves:
    //   - Large: CaveWarpFrequency → broad sweeping bends (room-scale)
    //   - Medium: 3x frequency, 0.3x strength → wall-scale irregularity
    //
    // IMPORTANT: Only the SDF query uses warped coordinates. Roughness, terrain
    // ops, and hash-placed features use the REAL world position so they stay
    // geologically correct (terracing stays horizontal, columns stay vertical, etc.)
    float WarpedX = WorldX;
    float WarpedY = WorldY;
    float WarpedZ = EffectiveZ;

    if (!VoxelDensityAblation::IsCaveWarpOff() && Params.CaveWarpStrength > 0.0f)
    {
        const float WF = Params.CaveWarpFrequency;
        const float WS = Params.CaveWarpStrength;

        // Three independent Perlin fields offset by irrational-ish numbers
        // so the X/Y/Z warp channels don't correlate with each other.
        // Single octave to keep per-voxel cost low (3 Perlin calls total).
        WarpedX += VoxelNoise::Perlin3D(FVector3f(
            WorldX * WF + VoxelHash::SeedOffset(SeedU, 0.37f),
            WorldY * WF + 1.3f,
            EffectiveZ * WF + 5.7f)) * VOXEL_NOISE_SCALE * WS;
        WarpedY += VoxelNoise::Perlin3D(FVector3f(
            WorldX * WF + 7.1f,
            WorldY * WF + VoxelHash::SeedOffset(SeedU, 0.59f),
            EffectiveZ * WF + 2.3f)) * VOXEL_NOISE_SCALE * WS;
        WarpedZ += VoxelNoise::Perlin3D(FVector3f(
            WorldX * WF + 11.3f,
            WorldY * WF + 9.7f,
            EffectiveZ * WF + VoxelHash::SeedOffset(SeedU, 0.41f))) * VOXEL_NOISE_SCALE * WS;
    }

    //=========================================================================
    // STEP 4: SDF MORPHOLOGY (rooms + tunnels) — CACHED PER CHUNK
    //=========================================================================
    // The room list and tunnel connections are IDENTICAL for all voxels in a
    // chunk. Without caching, we rebuild them 32,768 times (32³ voxels).
    // With thread_local caching: build once, evaluate 32K times with just SDF math.
    //
    // The cache is keyed on (ChunkX, ChunkY, StrateIndex, Seed). When the key
    // changes (new chunk or strate), the cache is rebuilt. thread_local ensures
    // each task thread has its own cache — no locking needed.
    //
    // The warped position is used for SDF evaluation (bends rooms/tunnels),
    // but the cache search area is expanded by CaveWarpStrength to ensure
    // all reachable rooms are included regardless of warp displacement.
    //
    // Declared before the RoomDensity if-block so SDFCache and NearestRoomIdx
    // are also visible to the terrain ops block further below.
    thread_local FChunkSDFCache SDFCache;
    // Cache validity is tracked by the SEARCH BOX the cache was built for, NOT by chunk
    // equality. Gradient-normal sampling queries at WorldX±1 (and warp displacement) can
    // step a voxel outside the chunk; with chunk-equality keying that flipped the key and
    // rebuilt the (now expensive) cache every boundary cell. Since the stored rooms cover
    // the search box + MaxInfluence, any query INSIDE the box is correct — so we only
    // rebuild when the query actually leaves the box. Result: one build per chunk, no thrash.
    thread_local float CachedSMinX = 1.0f, CachedSMaxX = -1.0f;  // start invalid (min > max)
    thread_local float CachedSMinY = 0.0f, CachedSMaxY = 0.0f;
    thread_local int32 CachedStrate = INT32_MIN;
    thread_local uint32 CachedSeed = 0;
    // ⚠️ AUDIT §C2 (corrigé le 2026-07-28). Les deux lignes qui manquaient à cette clé.
    // La clé ci-dessus décrit la GÉOMÉTRIE de la fenêtre (boîte, strate, seed) et rien de ce qui
    // détermine les PARAMS avec lesquels les salles ont été cuites. Comme `GetGenerationParams`
    // blende à l'intérieur d'une strate (`Alpha` = f(chunk Z), et f(chunk XY) aussi en
    // `Interleaved`), deux chunks voisins produisent la MÊME clé avec des params DIFFÉRENTS, et le
    // deuxième se sert des salles du premier. Non déterministe entre pairs, parce que l'ordre des
    // workers décide lequel est « le premier » — exactement ce que §2.6.1 interdit.
    //
    // Pourquoi ça ne casse PAS l'invariant de perf de §8.10 : la clé reste une BOÎTE, donc les
    // sondes de gradient à `WorldX ± 1` ne font toujours pas tourner le cache. Ce qui le fait
    // tourner en plus, c'est un changement RÉEL de params — une fois par chunk dans une bande de
    // transition, ce qui est le nombre de reconstructions que ce cache aurait toujours dû faire.
    thread_local uint32 CachedFingerprint = 0xFFFFFFFFu;
    thread_local uint32 CachedLayout      = 0xFFFFFFFFu;
    thread_local bool CachedUsesFusedCacheWindow = false;
    thread_local bool CachedUsesTileCacheWindow = false;
    thread_local FIntVector CachedTileOrigin = FIntVector::ZeroValue;
    thread_local int32 CachedTileStep = 0;
    thread_local int32 CachedTileCells = 0;
    thread_local uint64 CachedManagerLifetimeId = 0;
    thread_local const TArray<FStrateTerrainOpEntry>* CachedTerrainOps = nullptr;
    // The envelope proof is constant for a cache-policy key.  Do not run the complete room/edge
    // reach calculation for every density sample: that would turn a build-time guard into a
    // per-sample cost on the LOD0 floor.
    thread_local bool CachedTilePolicyValid = false;
    thread_local bool CachedTilePolicyResult = false;
    thread_local bool CachedTilePolicyHasContext = false;
    thread_local bool CachedTilePolicyFusedPath = false;
    thread_local int32 CachedTilePolicySampleStep = 0;
    thread_local FIntVector CachedTilePolicyOrigin = FIntVector::ZeroValue;
    thread_local int32 CachedTilePolicyStep = 0;
    thread_local int32 CachedTilePolicyCells = 0;
    thread_local uint32 CachedTilePolicyFingerprint = 0xFFFFFFFFu;
    thread_local uint32 CachedTilePolicyLayout = 0xFFFFFFFFu;
    thread_local uint64 CachedTilePolicyManagerLifetimeId = 0;
    thread_local const TArray<FStrateTerrainOpEntry>* CachedTilePolicyTerrainOps = nullptr;

    const bool bUseFusedCacheWindow = OutTunnelCore != nullptr;
    const bool bUseSpatialIndex = VoxelGenLOD::ShouldUseSpatialIndex(bUseFusedCacheWindow);
    FIntVector RequestedTileOrigin = FIntVector::ZeroValue;
    int32 RequestedTileStep = 1;
    int32 RequestedTileCells = 0;
    const uint64 CurrentManagerLifetimeId = StrateManager
        ? StrateManager->GetCacheLifetimeId() : 0;
    const int32 QueryChunkX = FMath::FloorToInt(WorldX / (float)CHUNK_SIZE);
    const int32 QueryChunkY = FMath::FloorToInt(WorldY / (float)CHUNK_SIZE);
    const int32 QueryChunkZ = FMath::FloorToInt(WorldZ / (float)CHUNK_SIZE);
    const int32 CacheRegionChunks = bUseFusedCacheWindow ? 4 : 1;
    const int32 CacheRegionSize = CHUNK_SIZE * CacheRegionChunks;
    const int32 CacheRegionX = FMath::FloorToInt(
        WorldX / static_cast<float>(CacheRegionSize));
    const int32 CacheRegionY = FMath::FloorToInt(
        WorldY / static_cast<float>(CacheRegionSize));
    // BuildChunkCache bakes the selected immutable terrain-operation pool into each stored room.
    // A tile may cross a strate-definition boundary in XY, so the pool identity is part of the
    // cache key just like the generation-parameter fingerprint. The manager lifetime guards an
    // allocator-reused definition address after a live layout/world change.
    const TArray<FStrateTerrainOpEntry>* TerrainOps = nullptr;
    if (StrateManager)
    {
        UVoxelStrateDefinition* Def = StrateManager->GetStrateForChunk(
            FIntVector(
                bUseFusedCacheWindow ? QueryChunkX : CacheRegionX,
                bUseFusedCacheWindow ? QueryChunkY : CacheRegionY,
                QueryChunkZ));
        if (Def) TerrainOps = &Def->TerrainOperations;
    }
    const bool bTileWindowEligible = VoxelGenLOD::IsTileCacheWindowEnabled(
        bUseFusedCacheWindow);
    const bool bHasTileWindowContext = VoxelGenLOD::GetThreadTileCacheWindow(
        RequestedTileOrigin, RequestedTileStep, RequestedTileCells);
    const bool bTilePolicyKeyChanged =
        !CachedTilePolicyValid
        || CachedTilePolicyHasContext != (bTileWindowEligible && bHasTileWindowContext)
        || CachedTilePolicyFusedPath != bUseFusedCacheWindow
        || CachedTilePolicySampleStep != RequestedTileStep
        || CachedTilePolicyOrigin != RequestedTileOrigin
        || CachedTilePolicyStep != RequestedTileStep
        || CachedTilePolicyCells != RequestedTileCells
        || CachedTilePolicyFingerprint != ParamsFingerprint
        || CachedTilePolicyLayout != LayoutVersion
        || CachedTilePolicyManagerLifetimeId != CurrentManagerLifetimeId
        || CachedTilePolicyTerrainOps != TerrainOps;
    if (bTilePolicyKeyChanged)
    {
        CachedTilePolicyValid = true;
        CachedTilePolicyHasContext = bTileWindowEligible && bHasTileWindowContext;
        CachedTilePolicyFusedPath = bUseFusedCacheWindow;
        CachedTilePolicySampleStep = RequestedTileStep;
        CachedTilePolicyOrigin = RequestedTileOrigin;
        CachedTilePolicyStep = RequestedTileStep;
        CachedTilePolicyCells = RequestedTileCells;
        CachedTilePolicyFingerprint = ParamsFingerprint;
        CachedTilePolicyLayout = LayoutVersion;
        CachedTilePolicyManagerLifetimeId = CurrentManagerLifetimeId;
        CachedTilePolicyTerrainOps = TerrainOps;
        CachedTilePolicyResult = CachedTilePolicyHasContext
            && VoxelCaveMorphology::IsRoomGraphWindowInvariant(Params, TerrainOps);
    }
    const bool bUseTileCacheWindow = CachedTilePolicyResult;

    // Index of the room with the smallest (most-inside) SDF for this voxel.
    // Written by EvaluateSDFCached, read by the terrain ops block to pick the
    // per-room terrain op. -1 means no room is nearby (deep-solid or no rooms).
    thread_local int32 NearestRoomIdx = -1;

    float CaveSDF = FLT_MAX;

    if (Params.RoomDensity > 0.0f && Params.RoomSpacing > 0.0f)
    {
        // Get strate index for unique caves per strate.
        // MEMOISED: the index only changes at chunk-Z boundaries, yet this ran the strate layout's
        // linear scan PER VOXEL. Key = (chunk-Z, layout version) — the version guards against editor
        // rebuilds (RebuildStrates / live edit) serving a stale index. The lookup queries the BAND
        // CENTRE so the result is a pure function of the key (order-independent, window-invariant).
        int32 StrateIdx = 0;
        if (StrateManager)
        {
            thread_local int32  SI_ChunkZ  = INT32_MAX;
            thread_local uint32 SI_Version = 0xFFFFFFFFu;
            thread_local int32  SI_Index   = 0;
            const int32 QZ = FMath::FloorToInt(WorldZ / (float)CHUNK_SIZE);
            const uint32 LV = StrateManager->GetLayoutVersion();
            if (QZ != SI_ChunkZ || LV != SI_Version)
            {
                SI_ChunkZ  = QZ;
                SI_Version = LV;
                // GetStrateIndex expects Unreal world units (divides by VOXEL_SIZE internally);
                // WorldZ here is in voxels, so multiply by VOXEL_SIZE.
                SI_Index = StrateManager->GetStrateIndex(((float)QZ + 0.5f) * CHUNK_SIZE * VOXEL_SIZE);
            }
            StrateIdx = SI_Index;
        }

        // Rebuild only when the WARPED query (what EvaluateSDFCached uses) leaves the
        // cached search box, or the strate/seed changed.
        const bool bNeedRebuild =
            StrateIdx != CachedStrate || (uint32)Seed != CachedSeed ||
            ParamsFingerprint != CachedFingerprint || LayoutVersion != CachedLayout ||
            CurrentManagerLifetimeId != CachedManagerLifetimeId ||
            TerrainOps != CachedTerrainOps ||
            bUseFusedCacheWindow != CachedUsesFusedCacheWindow ||
            bUseTileCacheWindow != CachedUsesTileCacheWindow ||
            (bUseTileCacheWindow
                && (RequestedTileOrigin != CachedTileOrigin
                    || RequestedTileStep != CachedTileStep
                    || RequestedTileCells != CachedTileCells)) ||
            WarpedX < CachedSMinX || WarpedX > CachedSMaxX ||
            WarpedY < CachedSMinY || WarpedY > CachedSMaxY;

        if (bNeedRebuild)
        {
            // The lowered evaluator uses the same window-invariance contract as the prepared
            // room-graph source.  With the switch enabled, the search box is the exact requesting
            // tile footprint plus the one-sample normal halo and deterministic warp margin.  A
            // tile worker can visit neighbouring chunk keys in an arbitrary order without
            // rebuilding the graph at every coarse sample.  Point queries without a tile context
            // retain the historical fused four-chunk / ordinary one-chunk fallback.
            const float ChunkMinX = CacheRegionX * (float)CacheRegionSize;
            const float ChunkMinY = CacheRegionY * (float)CacheRegionSize;
            const float ChunkMaxX = ChunkMinX + (float)CacheRegionSize;
            const float ChunkMaxY = ChunkMinY + (float)CacheRegionSize;
            const float WarpMargin = FMath::Abs(Params.CaveWarpStrength)
                * VOXEL_NOISE_SCALE * 1.5f;
            float SMinX = ChunkMinX;
            float SMinY = ChunkMinY;
            float SMaxX = ChunkMaxX;
            float SMaxY = ChunkMaxY;
            if (bUseTileCacheWindow)
            {
                const float TileMinX = static_cast<float>(RequestedTileOrigin.X);
                const float TileMinY = static_cast<float>(RequestedTileOrigin.Y);
                const float TileExtent = static_cast<float>(
                    static_cast<int64>(RequestedTileStep)
                    * static_cast<int64>(RequestedTileCells));
                const float TileMaxX = TileMinX + TileExtent;
                const float TileMaxY = TileMinY + TileExtent;
                const float TileHalo = static_cast<float>(RequestedTileStep) + 2.0f;
                const float Expansion = WarpMargin + TileHalo;
                SMinX = TileMinX - Expansion;
                SMinY = TileMinY - Expansion;
                SMaxX = TileMaxX + Expansion;
                SMaxY = TileMaxY + Expansion;
            }
            else
            {
                const float Expansion = WarpMargin + 2.0f;
                SMinX = ChunkMinX - Expansion;
                SMinY = ChunkMinY - Expansion;
                SMaxX = ChunkMaxX + Expansion;
                SMaxY = ChunkMaxY + Expansion;
            }

            VoxelCaveMorphology::BuildChunkCache(
                SDFCache,
                SMinX, SMinY, SMaxX, SMaxY,
                Params, (uint32)Seed, StrateIdx,
                TerrainOps,
                ERoomGraphBuildSite::GeneratorTile
            );

            CachedSMinX = SMinX; CachedSMaxX = SMaxX;
            CachedSMinY = SMinY; CachedSMaxY = SMaxY;
            CachedStrate = StrateIdx;
            CachedSeed = (uint32)Seed;
            CachedFingerprint = ParamsFingerprint;
            CachedLayout      = LayoutVersion;
            CachedUsesFusedCacheWindow = bUseFusedCacheWindow;
            CachedUsesTileCacheWindow = bUseTileCacheWindow;
            CachedTileOrigin = bUseTileCacheWindow
                ? RequestedTileOrigin : FIntVector::ZeroValue;
            CachedTileStep = bUseTileCacheWindow ? RequestedTileStep : 0;
            CachedTileCells = bUseTileCacheWindow ? RequestedTileCells : 0;
            CachedManagerLifetimeId = CurrentManagerLifetimeId;
            CachedTerrainOps = TerrainOps;
        }

        // Evaluate SDF using cached rooms and tunnels (WARPED coordinates).
        // Also writes NearestRoomIdx — the room with minimum SDF contribution
        // at this voxel's position. Used by terrain ops below to pick per-room params.
        NearestRoomIdx = -1;
        CaveSDF = VoxelCaveMorphology::EvaluateSDFCached(
            WarpedX, WarpedY, WarpedZ,
            SDFCache, Params.SDFBlendRadius,
            &NearestRoomIdx,
            bUseSpatialIndex
        );

        // ── PIT & CHIMNEY SDF INTEGRATION ──
        // Pits and chimneys are SmoothMin'd into CaveSDF here, using REAL
        // (unwarped) world coordinates. This is intentional: pit positions come
        // from unwarped room centers, so evaluating them in warped space would
        // misalign them. Rooms also use unwarped positions — warping is purely a
        // query-coordinate bend, not a data-space transform.
        //
        // Including pits in CaveSDF means:
        //   - The CarveFactor block below handles their density naturally
        //   - SmoothMin at the pit-to-room junction creates the same organic
        //     transition as tunnel-to-room (no hard seam at PitTopZ)
        //   - bNearCaveSurface becomes true inside the shaft — roughness clamp
        //     prevents fill-back, so this is safe
        if (!VoxelDensityAblation::IsPitChimneySDFOff())
        {
            auto EvaluatePit = [&](int32 PitIndex)
            {
                const FCachedPit& Pit = SDFCache.Pits[PitIndex];
                const float DZ = WorldZ - Pit.TopZ;  // Negative = below anchor (shaft)
                if (DZ >= Pit.BlendK) return;       // Above even the blend fringe
                if (-DZ > Pit.Depth + Pit.BlendK) return;

                const float DX = WorldX - Pit.CenterX;
                const float DY = WorldY - Pit.CenterY;
                const float XYDistSq = DX * DX + DY * DY;
                if (XYDistSq > Pit.BoundXYRadiusSq) return;

                float PitSDF;
                if (DZ <= 0.0f)
                {
                    // Shaft: tapered cylinder with flared opening
                    float DepthBelow  = -DZ;
                    float FlareFactor = FMath::Clamp(1.0f - DepthBelow / Pit.FlareDist, 0.0f, 1.0f);
                    FlareFactor       = FlareFactor * FlareFactor;
                    float EffRadius   = Pit.Radius + Pit.FlareExtra * FlareFactor;
                    PitSDF = FMath::Sqrt(XYDistSq) - EffRadius;
                }
                else
                {
                    // Above anchor: only the blend fringe matters here.
                    // We still pass a SDF so SmoothMin can soften the rim from above.
                    // Treat this zone as a flat disc at TopZ (XY cylinder, no Z factor)
                    // so the blend fades horizontally into the room floor.
                    PitSDF = FMath::Sqrt(XYDistSq) - (Pit.Radius + Pit.FlareExtra);
                }

                CaveSDF = VoxelSDF::SmoothMin(CaveSDF, PitSDF, Pit.BlendK);
            };
            VF_ForEachChunkSDFSpatialCandidate(
                SDFCache.PitSpatialIndex, SDFCache.Pits.Num(), WorldX, WorldY, EvaluatePit,
                bUseSpatialIndex);

            auto EvaluateChimney = [&](int32 ChimneyIndex)
            {
                const FCachedChimney& Chim = SDFCache.Chimneys[ChimneyIndex];
                const float DZ = WorldZ - Chim.BottomZ;  // Positive = above anchor (shaft)
                if (-DZ >= Chim.BlendK) return;         // Below even the blend fringe
                if (DZ > Chim.Height + Chim.BlendK) return;

                const float DX = WorldX - Chim.CenterX;
                const float DY = WorldY - Chim.CenterY;
                const float XYDistSq = DX * DX + DY * DY;
                if (XYDistSq > Chim.BoundXYRadiusSq) return;

                float ChmSDF;
                if (DZ >= 0.0f)
                {
                    float FlareFactor = FMath::Clamp(1.0f - DZ / Chim.FlareDist, 0.0f, 1.0f);
                    FlareFactor       = FlareFactor * FlareFactor;
                    float EffRadius   = Chim.Radius + Chim.FlareExtra * FlareFactor;
                    ChmSDF = FMath::Sqrt(XYDistSq) - EffRadius;
                }
                else
                {
                    // Below anchor: flat disc blend into room ceiling
                    ChmSDF = FMath::Sqrt(XYDistSq) - (Chim.Radius + Chim.FlareExtra);
                }

                CaveSDF = VoxelSDF::SmoothMin(CaveSDF, ChmSDF, Chim.BlendK);
            };
            VF_ForEachChunkSDFSpatialCandidate(
                SDFCache.ChimneySpatialIndex, SDFCache.Chimneys.Num(),
                WorldX, WorldY, EvaluateChimney, bUseSpatialIndex);
        }

        // Convert SDF to density carving:
        // CaveSDF < 0 means we're inside a room/tunnel/pit → carve to air
        // CaveSDF > 0 means we're in solid rock → no change
        // The transition zone around SDF=0 gives smooth cave walls
        if (CaveSDF < Params.SDFBlendRadius)
        {
            // Smooth carving factor: 1.0 deep inside cave, 0.0 at blend edge
            float CarveFactor = FMath::Clamp(
                (Params.SDFBlendRadius - CaveSDF) / FMath::Max(Params.SDFBlendRadius * 2.0f, 1.0f),
                0.0f, 1.0f
            );
            // Smoothstep for less abrupt transitions
            CarveFactor = SmoothStep01(CarveFactor);
            Density -= CarveFactor * Params.BaseDensity * 2.0f;
        }
    }

    // The canonical stack's source publishes this same structural result before its detail
    // modifiers run.  The lowered path asks the scalar evaluator for it directly, so the common
    // post-disturbance tail can consume the result without allocating an FVoxelOpSample or
    // invoking the interpreted stack a second time for this sample.
    if (OutTunnelCore != nullptr
        && !VoxelDensityAblation::IsTunnelCoreOff()
        && Params.RoomDensity > 0.0f && Params.RoomSpacing > 0.0f)
    {
        // The no-column form is the exact scalar reference path: it performs the world-bound and
        // floor projection for the current point without eagerly materialising a whole XY table.
        // The interpreted source has a lazy worker LRU for block evaluation; building that table
        // here would turn a cache-window change into hundreds of unnecessary column builds.
        *OutTunnelCore = VoxelCaveMorphology::EvaluateTunnelCoreWorld(
            WorldX, WorldY, WorldZ, SDFCache, nullptr, bUseSpatialIndex);
    }

    //=========================================================================
    // EARLY-OUT: Skip detail work for deep-solid voxels
    //=========================================================================
    // ~70% of voxels in a chunk are deep inside solid rock, far from any cave.
    // Roughness, terrain ops, columns, pits, domes, pinch — ALL of these only
    // matter near cave surfaces. Skipping them for deep-solid voxels is the
    // single biggest performance win.
    //
    // We still need to run worm tunnels (they carve independently) and boundary
    // seal / modifiers, so we jump to Step 5 instead of returning early.
    const float DetailThreshold = Params.SDFBlendRadius * 3.0f;
    const bool bNearCaveSurface = (CaveSDF < DetailThreshold) && (CaveSDF < FLT_MAX);
    const bool bRunDetail = !VoxelDensityAblation::IsDetailOpsOff() && bNearCaveSurface;
    if (bCollectFusedDiagnostics && VoxelDensityProfile::AreCountersEnabled())
    {
        VoxelDensityProfile::AddCounter(
            bRunDetail
                ? VoxelDensityProfile::ECounter::FusedTunnelDetailSamples
                : VoxelDensityProfile::ECounter::FusedTunnelDetailSkipped);
    }

    VoxelDensityProfile::FScopedTimer FusedDetailTimer(
        VoxelDensityProfile::EBucket::FusedDetail);

    //=========================================================================
    // STEP 4b: SURFACE ROUGHNESS (volumetric, SDF-based)
    //=========================================================================
    // 3D noise near cave surfaces (where SDF ≈ 0) creates rocky detail:
    // overhangs, ledges, bumps, cracks. The SDF value directly tells us
    // how far from the surface we are — no need for separate floor/ceiling tracking.
    //
    // The noise type (fBM, Ridged, Mixed) and optional domain warping are
    // per-strate settings, so each strate can have fundamentally different
    // wall character — smooth lava tubes vs craggy erosion cliffs.
    if (bRunDetail && Params.SurfaceRoughness > 0.0f)
    {
        float RoughnessDepth = Params.SurfaceRoughness * 2.0f;
        float DistFromSurface = FMath::Abs(CaveSDF);

        if (DistFromSurface < RoughnessDepth)
        {
            float RF = Params.RoughnessFrequency;

            // Base noise input positions (with seed offsets for uniqueness)
            FVector3f MainPos(
                WorldX * RF + VoxelHash::SeedOffset(SeedU, 11.3f),
                WorldY * RF + VoxelHash::SeedOffset(SeedU, 13.7f),
                EffectiveZ * RF + VoxelHash::SeedOffset(SeedU, 17.1f)
            );
            FVector3f FinePos(
                WorldX * RF * 3.0f + VoxelHash::SeedOffset(SeedU, 19.1f) + 2000.0f,
                WorldY * RF * 3.0f + VoxelHash::SeedOffset(SeedU, 23.7f) + 2500.0f,
                EffectiveZ * RF * 3.0f + VoxelHash::SeedOffset(SeedU, 29.3f) + 3000.0f
            );

            // DOMAIN WARPING: distort noise coordinates with a secondary field.
            // This breaks up repetitive patterns — coordinates are "bent" by noise,
            // making walls look like they were shaped by flowing water or pressure.
            // Each axis uses a different seed offset for independent warping.
            if (Params.DomainWarpStrength > 0.0f)
            {
                float WF = Params.DomainWarpFrequency;
                float WS = Params.DomainWarpStrength;

                // Sample three independent noise fields for X, Y, Z warp
                float WarpX = VoxelNoise::Perlin3D(FVector3f(
                    WorldX * WF + VoxelHash::SeedOffset(SeedU, 5.2f),
                    WorldY * WF + VoxelHash::SeedOffset(SeedU, 1.3f),
                    EffectiveZ * WF + VoxelHash::SeedOffset(SeedU, 9.7f)
                )) * VOXEL_NOISE_SCALE * WS;

                float WarpY = VoxelNoise::Perlin3D(FVector3f(
                    WorldX * WF + 100.0f + VoxelHash::SeedOffset(SeedU, 7.7f),
                    WorldY * WF + 200.0f + VoxelHash::SeedOffset(SeedU, 3.1f),
                    EffectiveZ * WF + 300.0f
                )) * VOXEL_NOISE_SCALE * WS;

                float WarpZ = VoxelNoise::Perlin3D(FVector3f(
                    WorldX * WF + 400.0f,
                    WorldY * WF + 500.0f + VoxelHash::SeedOffset(SeedU, 11.9f),
                    EffectiveZ * WF + 600.0f + VoxelHash::SeedOffset(SeedU, 13.3f)
                )) * VOXEL_NOISE_SCALE * WS;

                // Apply warp to both noise positions
                FVector3f WarpOffset(WarpX, WarpY, WarpZ);
                MainPos += WarpOffset;
                FinePos += WarpOffset;
            }

            // NOISE TYPE SELECTION: sample the right noise function.
            // Octave counts go through VoxelGenLOD::Eff — far tiles (Step>1) drop
            // the sub-cell tail octaves (T2.b); LOD0 keeps the full counts.
            float RoughNoise, FineNoise;
            const int32 Oct3 = VoxelGenLOD::Eff(3);
            const int32 Oct2 = VoxelGenLOD::Eff(2);

            switch (Params.RoughnessNoiseType)
            {
            case EVoxelNoiseType::Ridged:
                // Ridged multifractal: sharp, craggy features
                RoughNoise = RidgedNoise3D(MainPos, Oct3);
                FineNoise = RidgedNoise3D(FinePos, Oct2);
                break;

            case EVoxelNoiseType::Mixed:
                // Blend: ridged structure softened by fBM
                RoughNoise = FractalNoise3D(MainPos, Oct3) * 0.5f
                           + RidgedNoise3D(MainPos, Oct3) * 0.5f;
                FineNoise = FractalNoise3D(FinePos, Oct2) * 0.5f
                          + RidgedNoise3D(FinePos, Oct2) * 0.5f;
                break;

            case EVoxelNoiseType::Cellular:
                // Worley/cellular: grotto, bubble-like patterns
                // Scale down because cellular noise has different frequency behavior
                RoughNoise = CellularNoise3D(MainPos);
                FineNoise = CellularNoise3D(FinePos);
                break;

            case EVoxelNoiseType::FBM:
            default:
                // Standard fBM: smooth, organic
                RoughNoise = FractalNoise3D(MainPos, Oct3);
                FineNoise = FractalNoise3D(FinePos, Oct2);
                break;
            }

            // Scale by VOXEL_NOISE_SCALE (all noise functions return ~[-1,1] but UE
            // Perlin internally returns ~[-0.8,0.8])
            RoughNoise *= VOXEL_NOISE_SCALE;
            FineNoise *= VOXEL_NOISE_SCALE;

            float TotalRough = RoughNoise * Params.SurfaceRoughness
                             + FineNoise * Params.SurfaceRoughness * 0.4f;

            // Inside definite cave air (CaveSDF < 0), roughness must never add solid back.
            // Without this clamp, barely-negative voxels near tunnel junctions or pit rims
            // get pushed back to solid by roughness → thin lids, membrane walls, bad seams.
            // Roughness can still carve further into walls (negative values), just not fill.
            if (CaveSDF < 0.0f)
            {
                TotalRough = FMath::Min(TotalRough, 0.0f);
            }

            // Fade out toward cave center (SurfaceFade = 1.0 at surface, 0.0 far away)
            float SurfaceFade = 1.0f - (DistFromSurface / RoughnessDepth);
            SurfaceFade = SurfaceFade * SurfaceFade;  // Quadratic: concentrate near surface

            Density += TotalRough * SurfaceFade;
        }
    }

    //=========================================================================
    // STEP 4c-4h: TERRAIN OPERATIONS (only near cave surfaces)
    //=========================================================================
    // All terrain ops only affect density near cave walls.
    // Deep-solid voxels skip this entire block (bNearCaveSurface = false).
    if (bRunDetail)
    {
    //=========================================================================
    // PER-ROOM TERRAIN PARAMS
    //=========================================================================
    // Terrain ops are now per-room (not global). NearestRoomIdx was written by
    // EvaluateSDFCached above — it's the room with minimum SDF at this voxel.
    //
    // We copy Params and apply the nearest room's terrain op on top.
    // Base Params has all terrain op fields = 0 (disabled) since
    // BuildParamsFromDefinition no longer merges them globally.
    //
    // Result: voxels inside different rooms see different terrain ops.
    // Rooms with no assigned op leave terrain fields at 0 → no terrain op. Clean.
    FStrateGenerationParams LocalTerrainParams = Params;
    if (NearestRoomIdx >= 0 && SDFCache.Rooms.IsValidIndex(NearestRoomIdx))
    {
        const FCachedRoom& NR = SDFCache.Rooms[NearestRoomIdx];
        if (NR.RoomOp)
        {
            // Writes only the op's specific fields (e.g. TerraceStepHeight for Terrace).
            // All other fields keep Params values unchanged.
            NR.RoomOp->ApplyTo(LocalTerrainParams, NR.RoomOpWeight);
        }
    }
    // Shadow outer Params inside this block so all terrain op code below
    // automatically uses the per-room values without any other changes.
    // The shadow is deliberate (per-room override), so silence C4457 here only.
    PRAGMA_DISABLE_SHADOW_VARIABLE_WARNINGS
    const FStrateGenerationParams& Params = LocalTerrainParams;
    PRAGMA_ENABLE_SHADOW_VARIABLE_WARNINGS

    // Geological features applied after roughness. These modify the density
    // field near cave surfaces to create specific shapes: terracing (step-like
    // ledges), layer lines (horizontal grooves), and overhangs (horizontal
    // shelf protrusions). Each operation is controlled by params in the strate
    // definition — set the main param to 0 to disable any operation.

    // --- TERRACING ---
    // Creates a staircase pattern on cave walls by offsetting density with
    // a smooth staircase function of world Z. The staircase quantizes the
    // cave surface into flat shelves connected by short cliff faces.
    //
    // Math: terracedZ = staircase(worldZ, stepH, hardness)
    //       offset = terracedZ - worldZ
    //       Where offset > 0 → more solid (shelf floor to walk on)
    //       Where offset < 0 → more air (gap under the shelf above)
    if (Params.TerraceStepHeight > 0.0f && CaveSDF < FLT_MAX)
    {
        const float StepH = Params.TerraceStepHeight;
        const float DistFromSurface = FMath::Abs(CaveSDF);
        const float TerraceRange = StepH * 3.0f;  // How far from surface the effect reaches

        if (DistFromSurface < TerraceRange)
        {
            // Surface orientation test: terracing only makes sense on horizontal surfaces
            // (cave floors). On vertical walls (pit shafts, tunnel sides) it creates ugly
            // horizontal ridges. We sample the SDF gradient in Z by querying Z±1:
            // an SDF has ≈unit gradient, so |dSDF/dZ| alone IS the normalized vertical
            // component — near 1 = floor/ceiling, near 0 = wall. (Z-only approximation:
            // the full 6-sample gradient normalization was 3× the SDF cost for the same
            // orientation signal; SmoothMin regions where |∇SDF| < 1 read slightly
            // "flatter", shifting where terraces fade on slopes — accepted visual delta.)
            //
            // We use the cached SDF so this costs two extra SDF evaluations per voxel,
            // only when near a surface — the common case is cheap (DistFromSurface > TerraceRange).
            float SDF_Zp1 = VoxelCaveMorphology::EvaluateSDFCached(
                WorldX, WorldY, WorldZ + 1.0f, SDFCache, Params.SDFBlendRadius,
                nullptr, bUseSpatialIndex);
            float SDF_Zm1 = VoxelCaveMorphology::EvaluateSDFCached(
                WorldX, WorldY, WorldZ - 1.0f, SDFCache, Params.SDFBlendRadius,
                nullptr, bUseSpatialIndex);
            float GZ = (SDF_Zp1 - SDF_Zm1) * 0.5f;
            // Normalized vertical component: 1 = perfectly horizontal surface (floor/ceiling)
            //                               0 = perfectly vertical surface (wall)
            float SurfaceHorizontality = FMath::Clamp(FMath::Abs(GZ), 0.0f, 1.0f);
            // Only apply terrace where the surface is mostly horizontal (> ~45 degrees)
            // Smooth transition to avoid a hard cutoff at exactly 45 degrees
            float TerraceOrientFactor = FMath::Clamp((SurfaceHorizontality - 0.3f) / 0.4f, 0.0f, 1.0f);
            // Noise displacement: perturb Z before staircase to break up straight edges
            float NoisedZ = WorldZ;
            if (Params.TerraceNoiseDisplacement > 0.0f)
            {
                float DispNoise = FractalNoise3D(FVector3f(
                    WorldX * 0.04f + VoxelHash::SeedOffset(SeedU, 31.1f),
                    WorldY * 0.04f + VoxelHash::SeedOffset(SeedU, 37.3f),
                    WorldZ * 0.02f + VoxelHash::SeedOffset(SeedU, 41.7f)
                ), VoxelGenLOD::Eff(2)) * VOXEL_NOISE_SCALE;
                NoisedZ += DispNoise * Params.TerraceNoiseDisplacement * StepH;
            }

            // Smooth staircase function:
            //   K = NoisedZ / StepH (which "step" are we in?)
            //   Frac = fractional part [0, 1) — position within the step
            //   Edge controls transition sharpness (narrow edge = sharp cliff face)
            float K = NoisedZ / StepH;
            float FloorK = FMath::FloorToFloat(K);
            float Frac = K - FloorK;  // Always [0, 1)

            // Build the stair profile: 0 in lower half, 1 in upper half, smooth transition
            float Edge = FMath::Lerp(0.45f, 0.02f, Params.TerraceHardness);
            float StairValue;
            if (Frac < 0.5f - Edge)
            {
                StairValue = 0.0f;  // Lower flat region (below transition)
            }
            else if (Frac > 0.5f + Edge)
            {
                StairValue = 1.0f;  // Upper flat region (above transition)
            }
            else
            {
                // Smoothstep through the transition zone
                float T = (Frac - (0.5f - Edge)) / (2.0f * Edge);
                StairValue = SmoothStep01(T);
            }

            // Reconstruct the terraced Z and compute offset from real Z
            float TerracedZ = (FloorK + StairValue) * StepH;
            float Offset = TerracedZ - NoisedZ;
            // Offset range: approximately [-StepH/2, +StepH/2]
            // Positive → below a shelf surface → add density (solid floor)
            // Negative → above a shelf → subtract density (air gap under next shelf)

            // Fade based on distance from cave surface (no effect deep in rock)
            float Fade = 1.0f - (DistFromSurface / TerraceRange);
            Fade = Fade * Fade;  // Quadratic: concentrate near surface

            // TerraceOrientFactor suppresses terrace on vertical walls (pit shafts, etc.)
            Density += Offset * Fade * TerraceOrientFactor;
        }
    }

    // --- LAYER LINES ---
    // Horizontal grooves in cave walls — visible geological strata.
    // A sine wave along Z, sharpened to create thin lines, subtracts density
    // near cave surfaces. This carves narrow horizontal channels into walls
    // at regular intervals, like sedimentary rock layers in cross-section.
    if (Params.LayerLineSpacing > 0.0f && CaveSDF < FLT_MAX)
    {
        const float DistFromSurface = FMath::Abs(CaveSDF);
        const float LineRange = Params.LayerLineSpacing * 1.5f;  // Influence depth into rock

        if (DistFromSurface < LineRange)
        {
            // Sine wave along Z: peaks at each line position
            float LinePhase = WorldZ * (2.0f * PI) / Params.LayerLineSpacing;
            float LineValue = FMath::Sin(LinePhase);

            // Sharpen to thin grooves: only carve where sine > 0, then cube it.
            // sin → max(sin, 0) → pow(_, 3) turns broad sine humps into thin spikes
            LineValue = FMath::Max(LineValue, 0.0f);
            LineValue = LineValue * LineValue * LineValue;  // Cubic sharpening

            // Fade near surface
            float Fade = 1.0f - (DistFromSurface / LineRange);
            Fade = Fade * Fade;

            // Subtract density to carve the groove
            Density -= LineValue * Params.LayerLineDepth * Fade;
        }
    }

    // --- RIBBING ---
    // Parallel ridge patterns on walls/ceiling (like lava tubes).
    // Uses the same sine-along-Z approach as layer lines, but ADDS density
    // (protruding ribs) instead of subtracting (grooves). The sine is
    // half-wave rectified and smoothed to create rounded bumps.
    if (Params.RibbingSpacing > 0.0f && CaveSDF < FLT_MAX)
    {
        const float DistFromSurface = FMath::Abs(CaveSDF);
        const float RibRange = Params.RibbingSpacing * 1.5f;

        if (DistFromSurface < RibRange)
        {
            // Sine wave along Z, offset by half-period from layer lines
            float RibPhase = WorldZ * (2.0f * PI) / Params.RibbingSpacing + PI * 0.5f;
            float RibValue = FMath::Sin(RibPhase);

            // Half-wave rectify (only positive → ribs, not grooves) then smooth
            RibValue = FMath::Max(RibValue, 0.0f);
            RibValue = RibValue * RibValue;  // Quadratic: rounder bump profile

            // Fade near surface
            float Fade = 1.0f - (DistFromSurface / RibRange);
            Fade = Fade * Fade;

            // Add density to create protruding ribs
            Density += RibValue * Params.RibbingDepth * Fade;
        }
    }

    // --- OVERHANGS ---
    // Horizontal shelf-like protrusions from cave walls.
    // Uses 3D noise with much lower Z frequency than XY, so features extend
    // horizontally for long stretches before varying vertically. This creates
    // natural rocky overhangs and ledges independent of the terracing system.
    //
    // Only positive noise values create protrusions (asymmetric: rock extends
    // INTO the cave, never away). This gives scattered shelf-like features
    // rather than uniform displacement.
    if (Params.OverhangStrength > 0.0f && CaveSDF < FLT_MAX)
    {
        const float DistFromSurface = FMath::Abs(CaveSDF);
        const float OverhangRange = Params.OverhangDepth * 2.0f;

        if (DistFromSurface < OverhangRange)
        {
            // Low Z frequency (0.15x of XY) → features extend horizontally
            float OverhangNoise = FractalNoise3D(FVector3f(
                WorldX * Params.OverhangFrequency + VoxelHash::SeedOffset(SeedU, 53.1f),
                WorldY * Params.OverhangFrequency + VoxelHash::SeedOffset(SeedU, 59.3f),
                EffectiveZ * Params.OverhangFrequency * 0.15f + VoxelHash::SeedOffset(SeedU, 61.7f)
            ), VoxelGenLOD::Eff(2)) * VOXEL_NOISE_SCALE;

            // Only where noise is positive → protrusions (not recesses)
            if (OverhangNoise > 0.0f)
            {
                float Fade = 1.0f - (DistFromSurface / OverhangRange);
                Fade = Fade * Fade;

                // Add density = extend solid rock into cave = overhang shelf
                Density += OverhangNoise * Params.OverhangDepth
                         * Params.OverhangStrength * Fade;
            }
        }
    }

    // --- CLIFF SHARPENING ---
    // Steepens vertical faces by amplifying the Z-axis density gradient.
    // Where the cave surface is already somewhat vertical (density changes
    // quickly along Z), this pushes it toward a sheer cliff face.
    //
    // Math: sample density at Z+1 and Z-1, compute vertical gradient.
    // Where gradient is steep AND we're near the cave surface:
    //   If we're in the UPPER portion of the cliff → subtract density (more air)
    //   If we're in the LOWER portion → add density (more solid)
    // This squeezes the transition zone, making it near-vertical.
    if (Params.CliffStrength > 0.0f && CaveSDF < FLT_MAX)
    {
        const float DistFromSurface = FMath::Abs(CaveSDF);
        const float CliffRange = 8.0f;  // Only affect voxels within 8 of surface

        if (DistFromSurface < CliffRange)
        {
            // Approximate vertical gradient via the CaveSDF sign and position.
            // Near the surface (CaveSDF ≈ 0), the sign of CaveSDF tells us which
            // side we're on: negative = inside cave, positive = solid rock.
            // We use a noise-modulated vertical gradient to detect steep faces.
            float VertGrad = VoxelNoise::Perlin3D(FVector3f(
                WorldX * 0.05f + VoxelHash::SeedOffset(SeedU, 71.3f),
                WorldY * 0.05f + VoxelHash::SeedOffset(SeedU, 73.7f),
                EffectiveZ * 0.15f + VoxelHash::SeedOffset(SeedU, 79.1f)  // 3x faster in Z → detects vertical features
            )) * VOXEL_NOISE_SCALE;

            // VertGrad near ±1 means terrain is changing fast vertically.
            // Multiply by sign of CaveSDF to get direction:
            //   Positive result (solid side, gradient pointing up) → add more solid
            //   Negative result (air side) → carve more air
            float CliffEffect = VertGrad * CaveSDF * Params.CliffStrength;

            // Only apply where gradient is significant (abs > 0.3)
            // and fade with distance from surface
            if (FMath::Abs(VertGrad) > 0.3f)
            {
                float Fade = 1.0f - (DistFromSurface / CliffRange);
                Fade = Fade * Fade;
                Density += CliffEffect * Fade * 3.0f;
            }
        }
    }

    // --- SCALLOP ---
    // Water-erosion-like concave patterns on cave walls.
    // Uses cellular (Worley) noise near cave surfaces — the distance-to-nearest
    // feature point creates natural bowl-shaped indentations. Where the cellular
    // noise value is high (far from feature points = center of a cell), we
    // subtract density to carve shallow bowls into the wall.
    //
    // This gives limestone caves their characteristic scalloped appearance —
    // rows of smooth, concave depressions covering the walls.
    if (Params.ScallopStrength > 0.0f && CaveSDF < FLT_MAX)
    {
        const float DistFromSurface = FMath::Abs(CaveSDF);
        const float ScallopRange = Params.ScallopStrength * 4.0f;

        if (DistFromSurface < ScallopRange)
        {
            // Cellular noise: returns ~[-1, 1] where positive = cell interior (bowl)
            float SF = Params.ScallopFrequency;
            float ScallopNoise = CellularNoise3D(FVector3f(
                WorldX * SF + VoxelHash::SeedOffset(SeedU, 83.1f),
                WorldY * SF + VoxelHash::SeedOffset(SeedU, 89.3f),
                EffectiveZ * SF + VoxelHash::SeedOffset(SeedU, 97.7f)
            ));

            // Only carve where noise is positive (cell interiors = bowl centers)
            if (ScallopNoise > 0.0f)
            {
                float Fade = 1.0f - (DistFromSurface / ScallopRange);
                Fade = Fade * Fade;

                // Subtract density to carve concave bowls
                Density -= ScallopNoise * Params.ScallopStrength * Fade;
            }
        }
    }

    // --- ARCH / BRIDGE (room-relative) ---
    // Horizontal rock bridges spanning the room interior.
    // Anchored to the nearest room: each arch spans from one side of the room
    // to the other at a hash-derived height within the room's Z range.
    // Up to MaxArches per room; ArchDensity = probability per slot.
    if (Params.ArchDensity > 0.0f && CaveSDF < Params.SDFBlendRadius && CaveSDF < FLT_MAX
        && NearestRoomIdx >= 0)
    {
        const FCachedRoom& Room = SDFCache.Rooms[NearestRoomIdx];
        const FVector VoxPos(WorldX, WorldY, WorldZ);

        for (int32 i = 0; i < 3; i++)
        {
            const FCachedArch& Arch = Room.Arches[i];
            if (!Arch.bActive) { continue; }
            const float ArchSDF = VoxelSDF::Capsule(
                VoxPos, Arch.EndpointA, Arch.EndpointB, Arch.Radius);

            const float ArchBlend = 2.0f;
            if (ArchSDF < ArchBlend)
            {
                float Fill = FMath::Clamp((ArchBlend - ArchSDF) / (ArchBlend * 2.0f), 0.0f, 1.0f);
                Fill = SmoothStep01(Fill);
                Density += Fill * Arch.BaseDensity * 1.5f;
            }
        }
    }

    //=========================================================================
    // STEP 4d: COLUMNS (pre-baked from BuildChunkCache)
    //=========================================================================
    // Columns are now pre-baked into SDFCache.Columns — no NearestRoomIdx needed.
    // The old per-voxel approach had columns appear/disappear mid-height when
    // the owning room changed (NearestRoomIdx switch). Pre-baking fixes this.
    auto EvaluateColumn = [&](int32 ColumnIndex)
    {
        const FCachedColumn& Col = SDFCache.Columns[ColumnIndex];
        const float DX = WorldX - Col.CenterX;
        const float DY = WorldY - Col.CenterY;
        const float XYDistSq = DX * DX + DY * DY;
        if (XYDistSq > Col.BoundXYRadiusSq) return;

        const float CylSDF = FMath::Sqrt(XYDistSq) - Col.Radius;

        const float ColBlend = 3.0f;
        if (CylSDF < ColBlend)
        {
            float Fill = FMath::Clamp((ColBlend - CylSDF) / (ColBlend * 2.0f), 0.0f, 1.0f);
            Fill = SmoothStep01(Fill);
            Density += Fill * Col.BaseDensity * 1.5f;
        }
    };
    VF_ForEachChunkSDFSpatialCandidate(
        SDFCache.ColumnSpatialIndex, SDFCache.Columns.Num(), WorldX, WorldY, EvaluateColumn,
        bUseSpatialIndex);

    //=========================================================================
    // STEP 4g: DOMES (room-relative hemispherical ceilings)
    //=========================================================================
    // Domes carve upward from the room's upper area, creating cathedral ceilings.
    // DmCenterZ anchored to the room's Z center so the dome sits naturally
    // in the ceiling zone rather than floating at arbitrary strate heights.
    if (Params.DomeDensity > 0.0f && CaveSDF < Params.SDFBlendRadius && CaveSDF < FLT_MAX
        && NearestRoomIdx >= 0)
    {
        const FCachedRoom& Room = SDFCache.Rooms[NearestRoomIdx];
        // Up to 2 domes per room (large rooms can have multiple cathedral pockets)
        const int32 MaxDomes = 2;

        for (int32 i = 0; i < MaxDomes; i++)
        {
            uint32 DH = VoxelHash::Mix(Room.Hash ^ (0xD0AE0u + (uint32)i * 8191u));

            if (VoxelHash::ToFloat01(DH) > Params.DomeDensity) continue;

            // XY: near room center (domes are wide, keep them centered)
            uint32 DH2 = VoxelHash::Mix(DH ^ 0xD0A0u);
            float DmX = Room.Center.X + VoxelHash::ToFloatSigned(DH2) * Room.RadiusXY * 0.4f;
            float DmY = Room.Center.Y + VoxelHash::ToFloatSigned(VoxelHash::Mix(DH2)) * Room.RadiusXY * 0.4f;

            // Radius: cap at room radius so dome fits inside the room
            uint32 DH3 = VoxelHash::Mix(DH2 ^ 0x90DEu);
            float DmRadius = FMath::Min(
                FMath::Lerp(Params.DomeMinRadius, Params.DomeMaxRadius, VoxelHash::ToFloat01(DH3)),
                Room.RadiusXY * 0.85f
            );

            // Dome center Z: upper portion of the room (ceiling area)
            uint32 DH4 = VoxelHash::Mix(DH3 ^ 0xCAFEu);
            float DmCenterZ = Room.Center.Z + Room.RadiusZ * 0.2f
                            + VoxelHash::ToFloat01(DH4) * Room.RadiusZ * 0.3f;

            float DmHeight = DmRadius * Params.DomeHeightRatio;

            // Quick Z reject
            if (WorldZ > DmCenterZ + DmHeight + 3.0f || WorldZ < DmCenterZ - 3.0f) continue;

            float DXDm = WorldX - DmX;
            float DYDm = WorldY - DmY;
            float DZDm = WorldZ - DmCenterZ;

            // Only carve upward from the dome center anchor
            if (DZDm < 0.0f) continue;

            // Half-ellipsoid SDF (upward only)
            float NormX = DXDm / DmRadius;
            float NormY = DYDm / DmRadius;
            float NormZ = DZDm / DmHeight;
            float EllipDist = FMath::Sqrt(NormX * NormX + NormY * NormY + NormZ * NormZ) - 1.0f;
            float DomeSDF = EllipDist * FMath::Min(DmRadius, DmHeight);

            const float DmBlend = 3.0f;
            if (DomeSDF < DmBlend)
            {
                float Carve = FMath::Clamp((DmBlend - DomeSDF) / (DmBlend * 2.0f), 0.0f, 1.0f);
                Carve = SmoothStep01(Carve);
                Density -= Carve * Params.BaseDensity * 1.5f;
            }
        }
    }

    //=========================================================================
    // STEP 4h: PINCH / BOTTLENECK (room-relative passage narrowing)
    //=========================================================================
    // Pinches squeeze a passage from the sides. Placed at the room's perimeter
    // area (high-radius offset from center) so they act on tunnel entrances
    // and room edges, not the open center of the room.
    if (Params.PinchDensity > 0.0f && CaveSDF < Params.SDFBlendRadius && CaveSDF < FLT_MAX
        && NearestRoomIdx >= 0)
    {
        const FCachedRoom& Room = SDFCache.Rooms[NearestRoomIdx];
        for (int32 i = 0; i < 3; i++)
        {
            const FCachedPinch& Pinch = Room.Pinches[i];
            if (!Pinch.bActive) { continue; }
            const float DXPn = WorldX - Pinch.CenterX;
            const float DYPn = WorldY - Pinch.CenterY;
            const float DZPn = WorldZ - Pinch.CenterZ;

            // Quick reject
            if (FMath::Abs(DXPn) + FMath::Abs(DYPn) + FMath::Abs(DZPn) > Pinch.MaxExtent) continue;

            const float Along  =  DXPn * Pinch.CosAngle + DYPn * Pinch.SinAngle;
            const float Across = -DXPn * Pinch.SinAngle + DYPn * Pinch.CosAngle;

            const float NAlong   = Along   / Pinch.HalfLength;
            const float NAcross  = Across  / Pinch.HalfNarrow;
            const float NUp      = DZPn    / Pinch.HalfVertical;
            const float EllipDist = NAlong * NAlong + NAcross * NAcross + NUp * NUp;

            if (EllipDist < 1.0f)
            {
                float Fill = 1.0f - EllipDist;
                Fill = SmoothStep01(Fill);
                const float AxisDist = FMath::Sqrt(NAcross * NAcross + NUp * NUp);
                const float SideFactor = FMath::Clamp(AxisDist * 2.0f, 0.0f, 1.0f);
                Density += Fill * SideFactor * Pinch.BaseDensity * 1.5f;
            }
        }
    }

    //=========================================================================
    // FLOOR BIAS
    //=========================================================================
    // Adds density in the lower portion of rooms to counteract surface roughness
    // making floors bumpy and hard to walk on. Works by knowing how far below
    // the nearest room center we are — the closer to the floor, the more density
    // is added back, smoothing the roughness-induced relief.
    //
    // Only applies inside cave air (CaveSDF < 0) so it doesn't re-solidify walls.
    // The roughness clamp already prevents fill-back in confirmed air; this works
    // WITH that to give floors a gentler character than walls/ceilings.
    if (Params.FloorBias > 0.0f && NearestRoomIdx >= 0 && CaveSDF < 0.0f)
    {
        const FCachedRoom& NR = SDFCache.Rooms[NearestRoomIdx];
        // NormZ: -1 = at room bottom, 0 = center, +1 = at ceiling
        const float NormZ = (WorldZ - NR.Center.Z) / FMath::Max(NR.RadiusZ, 1.0f);

        // Only apply below room center (floor zone).
        // Quadratic fade: strongest at floor, zero at center.
        if (NormZ < 0.0f)
        {
            float FloorFactor = NormZ * NormZ;  // 0 at center, 1 at bottom
            Density += FloorFactor * Params.FloorBias;
        }
    }

    } // end bNearCaveSurface (terrain ops)
    FusedDetailTimer.End();

    // Steps 4e / 4f (pits and chimneys) are now handled by the CaveSDF SmoothMin
    // block inserted above (between EvaluateSDFCached and the CarveFactor section).
    // The CarveFactor system carves them with the same density logic as rooms and
    // tunnels. No separate density subtraction here — that would double-carve.

    //=========================================================================
    // STEP 5: WORM TUNNELS (additional organic connectivity)
    //=========================================================================
    // Worm tunnels add secondary passages and organic connections
    // that the SDF graph doesn't create. They're noise-based, so they
    // produce natural winding paths that complement the room-and-corridor structure.
    //
    // HORIZONTAL BIAS: Z frequency is scaled up so tunnels prefer horizontal paths.
    //
    // NETWORK MASK: |N1|+|N2| < threshold carves tubes near the intersection of two noise
    // zero-surfaces — but wherever the fields merely GRAZE the threshold it leaves pinhole
    // pockets, and none of it is connected to anything (the far-field "confetti").
    // WormNetworkRange masks the carve by distance to the room/tunnel network (CaveSDF is
    // already computed by Step 4 — free): full strength at the network, smooth fade to zero
    // at Range. Worms become braids/shortcuts hugging the cave system; no isolated speckle.
    // Range = 0 → unmasked legacy behaviour. Bonus: fully-masked voxels skip both Perlins.
    if (Params.WormStrength > 0.0f && Params.WormThreshold > 0.0f)
    {
        VoxelDensityProfile::FScopedTimer FusedWormTimer(
            VoxelDensityProfile::EBucket::FusedWorm);
        float NetworkMask = 1.0f;
        if (Params.WormNetworkRange > 0.0f)
        {
            if (CaveSDF >= Params.WormNetworkRange)  // also true when no network (FLT_MAX)
            {
                NetworkMask = 0.0f;
            }
            else if (CaveSDF > 0.0f)
            {
                NetworkMask = 1.0f - SmoothStep01(CaveSDF / Params.WormNetworkRange);
            }
        }

        if (NetworkMask > 0.0f)
        {
            const bool bCollectWormDiagnostics =
                bCollectFusedDiagnostics && VoxelDensityProfile::AreCountersEnabled();
            if (bCollectWormDiagnostics)
            {
                VoxelDensityProfile::AddCounter(
                    VoxelDensityProfile::ECounter::WormEligibleSamples);
            }

            bool bWormBlockProofComputed = false;
            const bool bSkipWormBlock = GVoxelForgeWormBlockSkip != 0
                && VF_TryGetWormBlockSkip(
                    DensityCacheOwnerId, SeedU, ParamsFingerprint, LayoutVersion,
                    WorldX, WorldY, WorldZ, Params, bWormBlockProofComputed);
            if (bWormBlockProofComputed && bSkipWormBlock && bCollectWormDiagnostics)
            {
                VoxelDensityProfile::AddCounter(
                    VoxelDensityProfile::ECounter::WormBlockProofs);
            }
            if (bSkipWormBlock)
            {
                if (bCollectWormDiagnostics)
                {
                    VoxelDensityProfile::AddCounter(
                        VoxelDensityProfile::ECounter::WormBlockSkippedSamples);
                }
            }
            else
            {
                const VoxelWormField::FParameters WormParameters{
                    Params.WormFrequency,
                    Params.WormHorizontalBias,
                    Params.VerticalScale,
                    SeedU};
                const float WormValue = VoxelWormField::Evaluate(
                    WorldX, WorldY, WorldZ, Params.WormThreshold,
                    WormParameters);

                if (WormValue < Params.WormThreshold)
                {
                    const float t = 1.0f - (WormValue / Params.WormThreshold);
                    Density -= t * Params.WormStrength * NetworkMask;
                }
            }
        }
        FusedWormTimer.End();
    }

    //=========================================================================
    // STEP 6: STRATE BOUNDARY SEAL (haut + bas)
    //=========================================================================
    VoxelDensityProfile::FScopedTimer FusedStructuralTimer(
        VoxelDensityProfile::EBucket::FusedStructural);
    {
        VoxelDensityProfile::FScopedTimer ProfileTimer(
            VoxelDensityProfile::EBucket::OriginSpineOp);
        ApplyOriginSpine(Density, WorldX, WorldY, WorldZ,
            Params.StrateTopWorldZ, Params.StrateBottomWorldZ,
            Params.BoundarySealThickness, Params.BaseDensity, OriginSpineRadius);
    }

    {
        VoxelDensityProfile::FScopedTimer ProfileTimer(
            VoxelDensityProfile::EBucket::BoundarySealOp);
        ApplyBoundarySeal(Density, WorldZ,
            Params.StrateTopWorldZ, Params.StrateBottomWorldZ,
            Params.BoundarySealThickness, Params.BaseDensity);
    }

    //=========================================================================
    // STEP 7: INTER-STRATE PASSAGES (perce le seal)
    //=========================================================================
    if (StrateManager)
    {
        if (bApplyLegacyStructuralPosts)
        {
            VoxelDensityProfile::FScopedTimer ProfileTimer(
                VoxelDensityProfile::EBucket::PassageModifier);
            StrateManager->ApplyPassageModifier(
                Density, WorldX, WorldY, WorldZ,
                Params.BaseDensity, Params.BoundarySealThickness);
        }
        else
        {
            // GetDensityAt owns one common MC-space structural tail after either the legacy or
            // operator-stack branch.  Keep the actual tube carve here, but do not repeat its
            // landing air/floor/tunnel-air posts before the shared disturbance/post sequence.
            VoxelDensityProfile::FScopedTimer ProfileTimer(
                VoxelDensityProfile::EBucket::PassageCarveOp);
            StrateManager->ApplyPassageCarvingOnly(
                Density, WorldX, WorldY, WorldZ,
                Params.BaseDensity, Params.BoundarySealThickness);
        }
    }

    if (bApplyLegacyStructuralPosts)
    {
        // Terrain operations and passage support floors are allowed to write solid density, but
        // the graph's own tunnel floor/core are structural route geometry. Direct callers of
        // GetDensityWithParams retain this historical tail; GetDensityAt skips it and applies the
        // same predicates once in its common MC-space post section below both generation paths.
        const bool bTunnelCoreEnabled = !VoxelDensityAblation::IsTunnelCoreOff();
        const bool bTunnelSupportFloor = bTunnelCoreEnabled
            && VoxelCaveMorphology::IsTunnelSupportFloorWorldPoint(
                WorldX, WorldY, WorldZ, SDFCache, bUseSpatialIndex);
        // Keep the direct legacy entry point capability-safe for callers outside the canonical
        // operator-stack path; its support band follows the same relieved swept floor.
        if (bTunnelSupportFloor)
        {
            const float StructuralSolidDensity =
                FMath::Max(Params.BaseDensity * 2.0f, 1.0f);
            if (VoxelDensityProfile::AreCountersEnabled())
            {
                if (Density < 0.0f)
                {
                    VoxelDensityProfile::AddCounter(
                        VoxelDensityProfile::ECounter::TunnelSupportFloorBackstopFires);
                }
            }
            Density = FMath::Max(Density, StructuralSolidDensity);
        }
        const float TunnelCoreSDF = bTunnelCoreEnabled
            && (Params.RoomDensity > 0.0f && Params.RoomSpacing > 0.0f)
            ? VoxelCaveMorphology::EvaluateTunnelCoreWorldSDF(
                WorldX, WorldY, WorldZ, SDFCache, bUseSpatialIndex)
            : FLT_MAX;
        if (!bTunnelSupportFloor
            && TunnelCoreSDF < -VoxelPassageGeometry::CaveTunnelAirCoreInsetVoxels)
        {
            Density = FMath::Min(
                Density,
                -FMath::Max(Params.BaseDensity * 2.0f, 1.0f));
        }
        ApplyOriginLandingFloor(Density, WorldX, WorldY, WorldZ,
            Params.StrateTopWorldZ, Params.StrateBottomWorldZ,
            Params.BoundarySealThickness, Params.BaseDensity, OriginSpineRadius);

        // Fourth structural post: the XY edge wins over a passage near the rim.
        VF_ApplyXYEdgeSeal(Density, WorldX, WorldY,
            WorldRadiusVoxels, EdgeSealThickness, Params.BaseDensity);
    }

    FusedStructuralTimer.End();

    // Convention MC: négatif = solide, positif = air.
    // La logique interne utilise positif = solide (plus lisible), donc on négate.
    return -Density;
}

//=============================================================================
// SLAB DENSITY (FlatPlain / CrystalChamber generator types)
//=============================================================================
// Produces a large horizontal void between a noisy floor and a noisy ceiling.
// No rooms, no tunnels, no worm noise — just two surfaces with noise displacement.
//
// Key difference from TunnelNetwork: ceiling uses abs(noise) so ALL formations
// point DOWNWARD. This creates the stalactite/crystal hanging-from-above effect.
// Floor uses signed noise for natural ground undulation (hills and valleys).
//
// Columns are placed on a world-space hash grid (no rooms to anchor them to).

float UVoxelGenerator::GetSlabDensity(float WorldX, float WorldY, float WorldZ,
                                        const FSlabGenerationParams& Params) const
{
    const float StrateHeight = Params.StrateTopWorldZ - Params.StrateBottomWorldZ;

    // Degenerate strate (zero or inverted bounds) — return solid.
    if (StrateHeight <= 0.0f) return 1.0f;

    const uint32 SeedU = (uint32)Seed;

    //=========================================================================
    // STEP 1: FLOOR SURFACE
    //=========================================================================
    // The floor is at FloorZ + signed noise displacement.
    // Signed noise allows both hills (noise > 0 → floor rises) and
    // valleys (noise < 0 → floor dips) for natural rolling ground.
    //
    // ⚠️ XY-PUR / XY-PURE (OPSTACK-DECOMPOSITION §3.1, tranché par Jahni 2026-07-27).
    // La 3e coordonnée était `WorldZ * FF * 0.05f` : une hauteur de sol qui dépendait de
    // l'altitude d'où on la demandait. Le coefficient était minuscule, donc ça se lisait comme un
    // léger étirement vertical plutôt que comme un bug — mais ça bloquait le cache de colonnes T1.a
    // et rendait toute classification de boîte inexacte. Constante ⇒ la surface est une vraie
    // fonction de (X,Y). Le monde se re-tune une fois : on échantillonne une autre tranche du champ
    // de bruit, donc la forme du sol change (elle ne se dégrade pas).
    //
    // The 3rd coord was WorldZ * FF * 0.05f — a floor height that depended on the altitude you
    // asked from. Now a constant, so the surface is a genuine function of (X,Y): the T1.a column
    // cache and an exact box verdict both become available. Worlds re-tune once.

    const float FloorZ = Params.StrateBottomWorldZ + StrateHeight * Params.FloorRelativeHeight;

    float FloorNoise = 0.0f;
    if (Params.FloorRoughness > 0.0f)
    {
        float FF = Params.FloorRoughnessFrequency;
        FloorNoise = FractalNoise3D(FVector3f(
            WorldX * FF + VoxelHash::SeedOffset(SeedU, 7.3f),
            WorldY * FF + VoxelHash::SeedOffset(SeedU, 11.1f),
            0.0f                          // XY-pur : plus aucune dépendance en Z / no Z dependence
        ), VoxelGenLOD::Eff(3)) * VOXEL_NOISE_SCALE * Params.FloorRoughness;
    }

    // Actual floor surface Z after noise displacement.
    const float FloorSurface = FloorZ + FloorNoise;

    //=========================================================================
    // STEP 2: CEILING SURFACE (formations hang DOWNWARD)
    //=========================================================================
    // The ceiling uses abs(noise) so ALL displacement pushes the ceiling DOWN.
    // When abs(noise) is high, rock protrudes further into the void — stalactite.
    // When abs(noise) is near 0, the ceiling is near the base CeilZ line.
    //
    // This asymmetry (only downward protrusions, never upward pockets) creates
    // the crystal-forest / stalactite silhouette from below.
    //
    // XY-PUR, même raison que le sol ci-dessus (§3.1). Le `+ 3000.0f` RESTE : ce n'est pas un
    // terme en Z, c'est le décalage qui décorrèle le champ du plafond de celui du sol.
    // XY-pure for the same reason as the floor. The + 3000.0f STAYS — it is not a Z term, it is
    // the offset that decorrelates the ceiling's noise field from the floor's.

    const float CeilZ = Params.StrateBottomWorldZ + StrateHeight * Params.CeilingRelativeHeight;

    float CeilNoise = 0.0f;
    if (Params.CeilingRoughness > 0.0f)
    {
        float CF = Params.CeilingRoughnessFrequency;
        float RawNoise = FractalNoise3D(FVector3f(
            WorldX * CF + VoxelHash::SeedOffset(SeedU, 17.3f) + 1000.0f,
            WorldY * CF + VoxelHash::SeedOffset(SeedU, 19.7f) + 2000.0f,
            3000.0f                         // XY-pur : décalage de décorrélation seul / offset only
        ), VoxelGenLOD::Eff(3)) * VOXEL_NOISE_SCALE;

        // abs() → formations ONLY hang down, never push ceiling up into solid rock.
        // Result: every noise peak creates a downward protrusion (crystal/stalactite).
        CeilNoise = FMath::Abs(RawNoise) * Params.CeilingRoughness;
    }

    // Actual ceiling surface Z (can only move downward due to abs above).
    // Clamp so ceiling never drops below floor + 2 voxels of headroom.
    // Without this clamp, extreme CeilingRoughness could completely fill the void.
    const float CeilSurface = FMath::Max(CeilZ - CeilNoise, FloorSurface + 2.0f);

    //=========================================================================
    // STEP 3: VOID FIELD → BASE DENSITY
    //=========================================================================
    // Each voxel is measured against both surfaces:
    //   DistAboveFloor > 0 → voxel is above the floor (possibly in the void)
    //   DistBelowCeil  > 0 → voxel is below the ceiling (possibly in the void)
    //
    // VoidField = min of both distances.
    // Positive inside the void (between floor and ceiling).
    // Negative outside (below floor or above ceiling = solid rock).
    //
    // Density = -VoidField (internal convention: positive = solid, negative = air).

    const float DistAboveFloor = WorldZ - FloorSurface;   // + when above floor
    const float DistBelowCeil  = CeilSurface - WorldZ;    // + when below ceiling

    const float VoidField = FMath::Min(DistAboveFloor, DistBelowCeil);

    float Density = -VoidField;  // Negative = air (inside void), positive = solid

    //=========================================================================
    // STEP 4: COLUMNS (hash-based, world-space grid)
    //=========================================================================
    // Unlike TunnelNetwork columns (anchored to room centers), slab columns
    // are placed on a regular world-space hash grid. They are infinite-height
    // cylinders — the void field already defines where solid/air is, so the
    // column SDF just adds density everywhere along its XY position.
    // The column is only visible where the void field carved air around it.
    if (Params.ColumnDensity > 0.0f && Params.ColumnSpacing > 0.0f)
    {
        const float Spacing = Params.ColumnSpacing;

        // Which cell are we in?
        const int32 ColCX = FMath::FloorToInt(WorldX / Spacing);
        const int32 ColCY = FMath::FloorToInt(WorldY / Spacing);

        // The 3×3 neighbourhood's columns (existence roll, jitter, radius) are a pure function of
        // (cell, seed, params) yet were re-derived PER VOXEL — 9 hash rolls + mixes in the slab hot
        // loop. Bake them once per centre cell (thread_local); rebuild only when the query crosses a
        // cell border or the seed/params change. Bit-identical (same hashes, same math).
        struct FSlabColumn { float X, Y, R; };
        thread_local TArray<FSlabColumn, TInlineAllocator<9>> SC_Cols;
        thread_local int32  SC_CX = INT32_MAX, SC_CY = INT32_MAX;
        thread_local uint32 SC_Seed = 0xFFFFFFFFu;
        thread_local float  SC_Spacing = -1.0f, SC_Dens = -1.0f, SC_MinR = -1.0f, SC_MaxR = -1.0f;

        if (ColCX != SC_CX || ColCY != SC_CY || (uint32)Seed != SC_Seed || Spacing != SC_Spacing ||
            Params.ColumnDensity != SC_Dens || Params.ColumnMinRadius != SC_MinR || Params.ColumnMaxRadius != SC_MaxR)
        {
            SC_CX = ColCX;  SC_CY = ColCY;  SC_Seed = (uint32)Seed;  SC_Spacing = Spacing;
            SC_Dens = Params.ColumnDensity;  SC_MinR = Params.ColumnMinRadius;  SC_MaxR = Params.ColumnMaxRadius;
            SC_Cols.Reset();

            for (int32 DY = -1; DY <= 1; DY++)
            {
                for (int32 DX = -1; DX <= 1; DX++)
                {
                    const int32 NCX = ColCX + DX;
                    const int32 NCY = ColCY + DY;

                    // Deterministic: same seed → same column pattern every session.
                    // XOR with a prime salt so columns don't correlate with room placement.
                    const uint32 H = VoxelHash::Cell(NCX, NCY, (uint32)Seed ^ 0xC01C01u);

                    // ColumnDensity is the probability this cell has a column.
                    if (VoxelHash::ToFloat01(H) > Params.ColumnDensity) continue;

                    // Jitter the column center within the cell (15%-85% of cell extent)
                    // to avoid a perfectly regular grid pattern.
                    const float JX = VoxelHash::ToFloat01(VoxelHash::Mix(H ^ 0x12345678u));
                    const float JY = VoxelHash::ToFloat01(VoxelHash::Mix(H ^ 0x9ABCDEF0u));

                    FSlabColumn& Col = SC_Cols.AddDefaulted_GetRef();
                    Col.X = (NCX + 0.15f + JX * 0.7f) * Spacing;
                    Col.Y = (NCY + 0.15f + JY * 0.7f) * Spacing;
                    // Column radius: hash-derived within configured range.
                    Col.R = FMath::Lerp(Params.ColumnMinRadius, Params.ColumnMaxRadius,
                        VoxelHash::ToFloat01(VoxelHash::Mix(H ^ 0xBEEFu)));
                }
            }
        }

        float ColumnSDF = FLT_MAX;
        for (const FSlabColumn& Col : SC_Cols)
        {
            // 2D cylinder SDF (infinite height — void field handles top/bottom).
            const float DX2D = WorldX - Col.X;
            const float DY2D = WorldY - Col.Y;
            ColumnSDF = FMath::Min(ColumnSDF, FMath::Sqrt(DX2D * DX2D + DY2D * DY2D) - Col.R);
        }

        // Smoothstep blend zone around the column edge (avoids hard MC aliasing).
        const float ColBlend = 2.0f;
        if (ColumnSDF < ColBlend && ColumnSDF < FLT_MAX)
        {
            float Fill = FMath::Clamp((ColBlend - ColumnSDF) / (ColBlend * 2.0f), 0.0f, 1.0f);
            Fill = SmoothStep01(Fill);  // Smoothstep
            Density += Fill * Params.BaseDensity * 1.5f;
        }
    }

    //=========================================================================
    // STEP 5: BOUNDARY SEAL + STEP 6: PASSAGES + STEP 7: XY EDGE SEAL
    //=========================================================================
    // Même logique que TunnelNetwork — factorisée dans les helpers ci-dessus.
    ApplyOriginSpine(Density, WorldX, WorldY, WorldZ,
        Params.StrateTopWorldZ, Params.StrateBottomWorldZ,
        Params.BoundarySealThickness, Params.BaseDensity, OriginSpineRadius);

    ApplyBoundarySeal(Density, WorldZ,
        Params.StrateTopWorldZ, Params.StrateBottomWorldZ,
        Params.BoundarySealThickness, Params.BaseDensity);

    if (StrateManager)
    {
        StrateManager->ApplyPassageModifier(
            Density, WorldX, WorldY, WorldZ,
            Params.BaseDensity, Params.BoundarySealThickness);
    }
    ApplyOriginLandingFloor(Density, WorldX, WorldY, WorldZ,
        Params.StrateTopWorldZ, Params.StrateBottomWorldZ,
        Params.BoundarySealThickness, Params.BaseDensity, OriginSpineRadius);

    VF_ApplyXYEdgeSeal(Density, WorldX, WorldY,
        WorldRadiusVoxels, EdgeSealThickness, Params.BaseDensity);

    // Convention MC: négatif = solide.
    return -Density;
}

//=============================================================================
// MAZE GENERATOR  (ECaveGeneratorType::Maze)
//=============================================================================
// Solid rock carved by a deterministic 3D lattice of corridors. Each non-origin lattice node
// chooses one parent toward (0,0,0), which is a spanning tree by construction. A small, capped
// hash-gated loop set adds alternate routes without being needed for connectivity. The local
// source evaluates the eight child nodes in the current cell's {-1,0} halo, so chunk seams cannot
// change an edge decision.

float UVoxelGenerator::GetMazeDensity(float WorldX, float WorldY, float WorldZ,
                                      const FMazeGenerationParams& Params) const
{
    const float StrateHeight = Params.StrateTopWorldZ - Params.StrateBottomWorldZ;
    if (StrateHeight <= 0.0f) return 1.0f;

    const float CS = FMath::Max(Params.CellSize, 1.0f);
    const FVector Pos(WorldX, WorldY, WorldZ);
    const uint32 S = (uint32)Seed ^ 0x4D617A65u;  // 'Maze'

    float Density = Params.BaseDensity;  // start solid

    const int32 CX = FMath::FloorToInt(WorldX / CS);
    const int32 CY = FMath::FloorToInt(WorldY / CS);
    const int32 CZ = FMath::FloorToInt(WorldZ / CS);

    // The edge set reachable from this voxel's cell is a pure function of (cell, seed, params).
    // Bake the parent and optional loop capsule endpoints once per cell (thread_local); the
    // per-voxel work is just the capsule SDFs. Rebuilds only on a cell crossing / param change.
    struct FMazeEdge { FVector A, B; };
    thread_local TArray<FMazeEdge, TInlineAllocator<24>> MZ_Edges;
    thread_local FIntVector MZ_Cell(INT32_MAX, INT32_MAX, INT32_MAX);
    thread_local uint32 MZ_Seed = 0xFFFFFFFFu;
    thread_local float  MZ_CS = -1.0f, MZ_Branch = -1.0f, MZ_Vert = -1.0f;

    const FIntVector Cell(CX, CY, CZ);
    if (Cell != MZ_Cell || S != MZ_Seed || CS != MZ_CS ||
        Params.BranchProbability != MZ_Branch || Params.Verticality != MZ_Vert)
    {
        MZ_Cell = Cell;  MZ_Seed = S;  MZ_CS = CS;
        MZ_Branch = Params.BranchProbability;  MZ_Vert = Params.Verticality;
        MZ_Edges.Reset();

        auto NodeCenter = [CS](int32 X, int32 Y, int32 Z)
        {
            return FVector((X + 0.5f) * CS, (Y + 0.5f) * CS, (Z + 0.5f) * CS);
        };
        // Canonical lower-node edges in {-1,0} per axis cover every corridor that can reach this
        // voxel's cell. IsOpenEdge checks both endpoints, including a +1 node's parent choice.
        for (int32 dz = -1; dz <= 0; dz++)
        for (int32 dy = -1; dy <= 0; dy++)
        for (int32 dx = -1; dx <= 0; dx++)
        {
            const int32 nx = CX + dx, ny = CY + dy, nz = CZ + dz;
            const FVector A = NodeCenter(nx, ny, nz);

            if (VoxelMazeTopology::IsOpenEdge(
                    nx, ny, nz, VoxelMazeTopology::EAxis::X,
                    S, Params.BranchProbability, Params.Verticality))
            {
                MZ_Edges.Add({ A, NodeCenter(nx + 1, ny, nz) });
            }
            if (VoxelMazeTopology::IsOpenEdge(
                    nx, ny, nz, VoxelMazeTopology::EAxis::Y,
                    S, Params.BranchProbability, Params.Verticality))
            {
                MZ_Edges.Add({ A, NodeCenter(nx, ny + 1, nz) });
            }
            if (VoxelMazeTopology::IsOpenEdge(
                    nx, ny, nz, VoxelMazeTopology::EAxis::Z,
                    S, Params.BranchProbability, Params.Verticality))
            {
                MZ_Edges.Add({ A, NodeCenter(nx, ny, nz + 1) });
            }
        }
    }

    const float R = FMath::Max(Params.CorridorRadius, 0.5f);
    float MazeSDF = FLT_MAX;
    for (const FMazeEdge& E : MZ_Edges)
    {
        MazeSDF = FMath::Min(MazeSDF, VoxelSDF::Capsule(Pos, E.A, E.B, R));
    }

    // Wall roughness: perturb the corridor surface.
    if (Params.SurfaceRoughness > 0.0f && MazeSDF < R + Params.SurfaceRoughness + 2.0f)
    {
        MazeSDF += FractalNoise3D(FVector3f(WorldX * 0.12f, WorldY * 0.12f, WorldZ * 0.12f), VoxelGenLOD::Eff(3))
                 * VOXEL_NOISE_SCALE * Params.SurfaceRoughness;
    }

    // Carve air where inside a corridor.
    const float Blend = 2.0f;
    if (MazeSDF < Blend)
    {
        float Carve = FMath::Clamp((Blend - MazeSDF) / (Blend * 2.0f), 0.0f, 1.0f);
        Carve = SmoothStep01(Carve);
        Density -= Carve * Params.BaseDensity * 2.0f;
    }

    ApplyOriginSpine(Density, WorldX, WorldY, WorldZ,
        Params.StrateTopWorldZ, Params.StrateBottomWorldZ,
        Params.BoundarySealThickness, Params.BaseDensity, OriginSpineRadius);

    ApplyBoundarySeal(Density, WorldZ,
        Params.StrateTopWorldZ, Params.StrateBottomWorldZ,
        Params.BoundarySealThickness, Params.BaseDensity);

    if (StrateManager)
    {
        StrateManager->ApplyPassageModifier(
            Density, WorldX, WorldY, WorldZ,
            Params.BaseDensity, Params.BoundarySealThickness);
    }
    ApplyOriginLandingFloor(Density, WorldX, WorldY, WorldZ,
        Params.StrateTopWorldZ, Params.StrateBottomWorldZ,
        Params.BoundarySealThickness, Params.BaseDensity, OriginSpineRadius);

    VF_ApplyXYEdgeSeal(Density, WorldX, WorldY,
        WorldRadiusVoxels, EdgeSealThickness, Params.BaseDensity);

    return -Density;
}

//=============================================================================
// SURFACE-WORLD GENERATOR  (ECaveGeneratorType::SurfaceWorld)
//=============================================================================
// A heightfield terrain (fBM continents + ridged mountains + fine detail) under a
// high solid "sky cap" ceiling, with a flattened beach band around the water line.
// Open air fills the gap between ground and ceiling; water is a render-side overlay.

float UVoxelGenerator::SampleSurfaceStructuralZ(float WorldX, float WorldY,
                                                const FSurfaceGenerationParams& Params, float& OutM) const
{
    const float H = Params.StrateTopWorldZ - Params.StrateBottomWorldZ;
    const uint32 SeedU = (uint32)Seed;
    const float BottomZ = Params.StrateBottomWorldZ;

    // --- Heightfield (a function of XY only — Z is a fixed seed slice) ---
    const float GroundBase = BottomZ + H * Params.BaseGroundRelative;

    // Domain-warp the STRUCTURAL query coords (continents + mountains) so coastlines and
    // ridgelines wind organically instead of looking like axis-aligned noise blobs. Detail
    // noise stays on the real XY so fine bumps remain crisp and uncorrelated with the warp.
    float QX = WorldX, QY = WorldY;
    if (Params.HeightWarpStrength > 0.0f)
    {
        const float WF = Params.HeightWarpFrequency;
        const float wx = VoxelNoise::Perlin3D(FVector3f(WorldX * WF + VoxelHash::SeedOffset(SeedU, 0.31f), WorldY * WF + 4.2f, VoxelHash::SeedOffset(SeedU, 1.7f)));
        const float wy = VoxelNoise::Perlin3D(FVector3f(WorldX * WF + 8.6f, WorldY * WF + VoxelHash::SeedOffset(SeedU, 0.53f), VoxelHash::SeedOffset(SeedU, 2.9f)));
        QX += wx * VOXEL_NOISE_SCALE * Params.HeightWarpStrength;
        QY += wy * VOXEL_NOISE_SCALE * Params.HeightWarpStrength;
    }

    // Macro relief map [0,1]: where this XY sits on the plains <-> mountains spectrum.
    // M is the "mountainous-ness"; ReliefStrength=0 → M=1 everywhere (uniform terrain).
    const float Relief = SampleRelief(WorldX, WorldY, Params.ReliefFrequency, Params.ReliefContrast);
    const float M = FMath::Lerp(1.0f, Relief, Params.ReliefStrength);

        float Cont = FractalNoise3D(FVector3f(
        QX * Params.ContinentFrequency + VoxelHash::SeedOffset(SeedU, 3.1f),
        QY * Params.ContinentFrequency + VoxelHash::SeedOffset(SeedU, 5.7f),
        VoxelHash::SeedOffset(SeedU, 0.7f)), 4);  // [-1,1]

        float Detail = FractalNoise3D(FVector3f(
        WorldX * Params.DetailFrequency + 11.0f,
        WorldY * Params.DetailFrequency + 22.0f,
        VoxelHash::SeedOffset(SeedU, 1.3f)), 3);  // [-1,1]

    float Mountain = 0.0f;
    if (Params.MountainStrength > 0.0f)
    {
        float Ridge = RidgedNoise3D(FVector3f(
            QX * Params.MountainFrequency + 99.0f,
            QY * Params.MountainFrequency + 77.0f,
            VoxelHash::SeedOffset(SeedU, 0.9f)), 4);     // [-1,1]
        Ridge = Ridge * 0.5f + 0.5f;  // [0,1] peaks
        Mountain = Ridge * Params.MountainStrength * M;   // mountains rise only in high-relief regions
    }

    // Plains keep a fraction of the continental swell; highlands get the full range.
    const float ContScale = FMath::Lerp(0.45f, 1.0f, M);

    float Terrain = GroundBase
        + Cont * Params.ElevationRange * 0.5f * ContScale
        + Mountain * Params.ElevationRange
        + Detail * Params.SurfaceRoughness;

    OutM = M;
    return Terrain;
}

float UVoxelGenerator::ComputeSurfaceTerrainZ(float WorldX, float WorldY,
                                              const FSurfaceGenerationParams& Params) const
{
    // Raw structural ground + the relief "mountainous-ness" M (reused to relief-gate the ops).
    float M = 1.0f;
    float Terrain = SampleSurfaceStructuralZ(WorldX, WorldY, Params, M);

    // --- F20 heightfield terrain ops (all off by default → byte-identical when unset) ---

    // CLIFF (slope-conditioned STEEPENING): where the surface is already steep, push the height
    // AWAY from the local mean so gentle slopes become sheer walls / canyon faces; gentle ground
    // stays untouched. Slope + local mean come from central differences of the STRUCTURAL field
    // (no op feedback), so it tracks the real landform. The one op that costs extra samples —
    // only when enabled (4 structural resamples, per-column-cheap).
    if (Params.CliffStrength > 0.0f)
    {
        const float D = FMath::Max(Params.CliffSampleDist, 0.5f);
        float Ms;   // relief scratch (unused — we only need the heights)
        const float Zxp = SampleSurfaceStructuralZ(WorldX + D, WorldY, Params, Ms);
        const float Zxm = SampleSurfaceStructuralZ(WorldX - D, WorldY, Params, Ms);
        const float Zyp = SampleSurfaceStructuralZ(WorldX, WorldY + D, Params, Ms);
        const float Zym = SampleSurfaceStructuralZ(WorldX, WorldY - D, Params, Ms);
        const float dZdX = (Zxp - Zxm) / (2.0f * D);
        const float dZdY = (Zyp - Zym) / (2.0f * D);
        const float Slope = FMath::Sqrt(dZdX * dZdX + dZdY * dZdY);   // rise per voxel of XY

        const float Thr = FMath::Max(Params.CliffSlopeThreshold, 0.05f);
        // 0 below the threshold, ramps to 1 within one threshold-width above it.
        const float SlopeGate = FMath::Clamp((Slope - Thr) / Thr, 0.0f, 1.0f);
        if (SlopeGate > 0.0f)
        {
            // Local smoothed reference; push the true height away from it → steepen the wall.
            const float Ref  = 0.25f * (Zxp + Zxm + Zyp + Zym);
            const float Gain = Params.CliffStrength * SlopeGate * Params.CliffSharpness;
            Terrain += (Terrain - Ref) * Gain;
        }
    }

    // TERRACE (relief-gated): quantize height into plateaus, only in high-relief regions and only
    // as strongly as TerraceStrength asks. TerraceHardness controls the riser: soft rounded steps
    // (0) → crisp flat-topped mesas with near-vertical walls (1). Layered cliffs/mesas up top,
    // smooth lowlands.
    if (Params.TerraceStrength > 0.0f && Params.TerraceHeight > 0.0f)
    {
        const float StepH = Params.TerraceHeight;
        const float T     = Terrain / StepH;
        const float K     = FMath::FloorToFloat(T);
        const float Frac  = T - K;                       // [0,1) position within the step
        // Hardness widens the flat plateau and sharpens the riser: half-transition width goes
        // from 0.5 (a smooth S-curve, no plateau) at Hardness=0 to ~0.03 (sharp wall) at 1.
        const float W  = FMath::Lerp(0.5f, 0.03f, FMath::Clamp(Params.TerraceHardness, 0.0f, 1.0f));
        const float Fs = SmoothStep01(FMath::Clamp((Frac - (0.5f - W)) / (2.0f * W), 0.0f, 1.0f));
        const float Stepped = (K + Fs) * StepH;
        Terrain = FMath::Lerp(Terrain, Stepped, Params.TerraceStrength * M);
    }

    // LAYER LINES: fine sedimentary shelves cut into slopes (exposed rock strata). Pull the
    // surface weakly toward each band plane; reads on slopes, invisible on flats (uniform shift).
    if (Params.LayerLineDepth > 0.0f && Params.LayerLineSpacing > 0.0f)
    {
        const float Phase = Terrain * (2.0f * PI / Params.LayerLineSpacing);
        Terrain -= FMath::Sin(Phase) * Params.LayerLineDepth;
    }

    // Beach: flatten terrain toward the water line within BeachWidth. (Water level is
    // strate-global — forced from the strate — so the water plane stays continuous.)
    const float H = Params.StrateTopWorldZ - Params.StrateBottomWorldZ;
    const float WaterZ = Params.StrateBottomWorldZ + H * Params.WaterLevelRelative;
    if (Params.WaterLevelRelative > 0.0f && Params.BeachWidth > 0.0f)
    {
        const float DAbs = FMath::Abs(Terrain - WaterZ);
        if (DAbs < Params.BeachWidth)
        {
            float T = SmoothStep01(DAbs / Params.BeachWidth);
            Terrain = FMath::Lerp(WaterZ, Terrain, T);
        }
    }

    return Terrain;
}

float UVoxelGenerator::ComputeSurfaceCeiling(float WorldX, float WorldY,
                                             const FSurfaceGenerationParams& Params) const
{
    const float H = Params.StrateTopWorldZ - Params.StrateBottomWorldZ;
    const uint32 SeedU = (uint32)Seed;
    float CeilZ = Params.StrateBottomWorldZ + H * Params.CeilingRelative;

    // Domain-warp the broad/ridge query coords so ceiling ridgelines and valleys wind
    // organically (mirrors the ground heightfield's HeightWarp). Fine bumps below stay on the
    // true XY so detail remains crisp and uncorrelated. 0 ⇒ no warp.
    float QX = WorldX, QY = WorldY;
    if (Params.CeilingWarpStrength > 0.0f)
    {
        const float WF = Params.CeilingWarpFrequency;
        const float wx = VoxelNoise::Perlin3D(FVector3f(WorldX * WF + VoxelHash::SeedOffset(SeedU, 0.71f), WorldY * WF + 2.3f,  VoxelHash::SeedOffset(SeedU, 3.3f)));
        const float wy = VoxelNoise::Perlin3D(FVector3f(WorldX * WF + 6.1f,          WorldY * WF + VoxelHash::SeedOffset(SeedU, 0.19f), VoxelHash::SeedOffset(SeedU, 4.7f)));
        QX += wx * VOXEL_NOISE_SCALE * Params.CeilingWarpStrength;
        QY += wy * VOXEL_NOISE_SCALE * Params.CeilingWarpStrength;
    }

    // Broad SIGNED swell: raises/lowers the whole cap → big inverted hills and valleys.
    if (Params.CeilingUndulation > 0.0f)
    {
        const float Swell = FractalNoise3D(FVector3f(
            QX * Params.CeilingUndulationFrequency + VoxelHash::SeedOffset(SeedU, 1.9f),
            QY * Params.CeilingUndulationFrequency + 13.0f,
            VoxelHash::SeedOffset(SeedU, 0.5f)), 3);   // [-1,1]
        CeilZ += Swell * VOXEL_NOISE_SCALE * Params.CeilingUndulation;
    }

    // Downward hang: everything here is >= 0 so features only protrude into the void (never
    // punch up into the seal). Fine bumps + sharp ridged blades sum together.
    float Hang = 0.0f;
    if (Params.CeilingRoughness > 0.0f)
    {
        Hang += FMath::Abs(FractalNoise3D(FVector3f(
            WorldX * Params.CeilingRoughnessFrequency + 5.0f,
            WorldY * Params.CeilingRoughnessFrequency + 6.0f,
            VoxelHash::SeedOffset(SeedU, 2.1f)), 3)) * VOXEL_NOISE_SCALE * Params.CeilingRoughness;
    }
    if (Params.CeilingRidgeStrength > 0.0f)
    {
        float Ridge = RidgedNoise3D(FVector3f(
            QX * Params.CeilingRidgeFrequency + 31.0f,
            QY * Params.CeilingRidgeFrequency + 47.0f,
            VoxelHash::SeedOffset(SeedU, 1.1f)), 4);            // [-1,1]
        Ridge = Ridge * 0.5f + 0.5f;      // [0,1] hanging ridgelines
        Hang += Ridge * Params.CeilingRidgeStrength;
    }

    return CeilZ - Hang;
}

float UVoxelGenerator::SurfaceDensityFromColumn(float WorldX, float WorldY, float WorldZ,
                                                float TerrainZ, float CeilSurf,
                                                float OverhangAmp, float DirX, float DirY,
                                                const FSurfaceGenerationParams& S) const
{
    // Solid below the terrain surface; solid above the sky-cap ceiling.
    float Density = TerrainZ - WorldZ;
    Density = FMath::Max(Density, WorldZ - CeilSurf);

    // F20 phase 2 — OVERHANG shelf (warped-terrain union): for AIR voxels in a window just above a
    // steep slope, re-sample the heightfield UPHILL (toward the cliff) by a height-varying amount and
    // union that rock in → the cliff-top rock juts OUT over the void below, self-capping at the cliff's
    // height. Genuine 3D (per-voxel re-eval), so it's gated hard: only steep overhang columns (amp>0),
    // only air voxels within OverhangHeight of the local ground, only when the shift is ≥ a voxel.
    // The lip is capped at TerrainZ+OverhangHeight, which ClassifyTile treats as ambiguous (no holes).
    if (OverhangAmp > 0.0f && S.OverhangHeight > 0.0f
        && WorldZ > TerrainZ && WorldZ <= TerrainZ + S.OverhangHeight)
    {
        const uint32 SeedU = (uint32)Seed;
        const float f     = S.OverhangFrequency;
        // Shelf-shape noise [0,1]; the Z term makes the reach fold/curl with height (ragged, not a lip).
        const float Ns = FractalNoise3D(FVector3f(
            WorldX * f + VoxelHash::SeedOffset(SeedU, 17.3f),
            WorldY * f + VoxelHash::SeedOffset(SeedU, 23.9f),
            WorldZ * f * S.OverhangZScale + VoxelHash::SeedOffset(SeedU, 5.1f)), 3) * 0.5f + 0.5f;   // [0,1]
        // KEY: the uphill reach GROWS with height in the window (Frac: 0 at ground → 1 at the cap). Low
        // down the shift is tiny ⇒ borrows nearby low rock ⇒ stays AIR over the void; high up the shift
        // reaches the far cliff ⇒ solid ⇒ the lip sits on top with air UNDERNEATH = a real overhang.
        const float Frac   = (WorldZ - TerrainZ) / S.OverhangHeight;            // (0,1] inside the window
        const float ShiftV = S.OverhangReach * OverhangAmp * Frac * Ns;         // uphill reach (voxels)
        if (ShiftV > 0.5f)
        {
            // Borrow the uphill STRUCTURAL height (not the full op'd surface): the shelf underside doesn't
            // need cliff/terrace refinement, and this avoids re-running Cliff's resamples per lip voxel.
            float Ms;
            const float ShiftedTZ = SampleSurfaceStructuralZ(WorldX + DirX * ShiftV, WorldY + DirY * ShiftV, S, Ms);
            Density = FMath::Max(Density, ShiftedTZ - WorldZ);   // union: solid where uphill rock covers Z
        }
    }

    // Structural fields (Z bounds, seal, base) are forced equal across biomes → S is safe.
    ApplyOriginSpine(Density, WorldX, WorldY, WorldZ,
        S.StrateTopWorldZ, S.StrateBottomWorldZ,
        S.BoundarySealThickness, S.BaseDensity, OriginSpineRadius);

    ApplyBoundarySeal(Density, WorldZ,
        S.StrateTopWorldZ, S.StrateBottomWorldZ,
        S.BoundarySealThickness, S.BaseDensity);

    if (StrateManager)
    {
        StrateManager->ApplyPassageModifier(
            Density, WorldX, WorldY, WorldZ,
            S.BaseDensity, S.BoundarySealThickness);
    }
    ApplyOriginLandingFloor(Density, WorldX, WorldY, WorldZ,
        S.StrateTopWorldZ, S.StrateBottomWorldZ,
        S.BoundarySealThickness, S.BaseDensity, OriginSpineRadius);

    VF_ApplyXYEdgeSeal(Density, WorldX, WorldY,
        WorldRadiusVoxels, EdgeSealThickness, S.BaseDensity);

    return -Density;
}

void UVoxelGenerator::ResolveSurfaceChunkParams(const FIntVector& ChunkCoord,
    FSurfaceGenerationParams& OutSurface, FBiomeContext& OutBiomeCtx,
    TArray<FSurfaceGenerationParams>& OutBiomeParams) const
{
    OutSurface  = StrateManager->GetSurfaceParamsForChunk(ChunkCoord);
    OutBiomeCtx = StrateManager->GetBiomeContextForChunk(ChunkCoord);

    // Per-biome surface params: each biome's override (when it overrides SurfaceWorld) else the
    // strate's, with STRUCTURAL fields forced from the strate (Z bounds, seal, base, water level)
    // so seals/spine/water stay intact.
    OutBiomeParams.Reset();
    if (OutBiomeCtx.IsValid())
    {
        const UVoxelStrateDefinition* Def = StrateManager->GetStrateForChunk(ChunkCoord);
        OutBiomeParams.Reserve(OutBiomeCtx.Biomes.Num());
        for (const FBiomeResolved& BR : OutBiomeCtx.Biomes)
        {
            FSurfaceGenerationParams P = OutSurface;   // strate base (+ structural)
            const UVoxelBiomeDefinition* B =
                (Def && Def->Biomes.IsValidIndex(BR.Index)) ? Def->Biomes[BR.Index] : nullptr;
            if (B && B->bOverrideTerrain && B->GeneratorType == ECaveGeneratorType::SurfaceWorld)
            {
                P = B->SurfaceParams;                  // biome shape
                P.StrateTopWorldZ       = OutSurface.StrateTopWorldZ;
                P.StrateBottomWorldZ    = OutSurface.StrateBottomWorldZ;
                P.BoundarySealThickness = OutSurface.BoundarySealThickness;
                P.BaseDensity           = OutSurface.BaseDensity;
                P.WaterLevelRelative    = OutSurface.WaterLevelRelative;  // shared water plane
            }
            OutBiomeParams.Add(P);
        }
    }
}

void UVoxelGenerator::ComputeSurfaceColumn(float WorldX, float WorldY, int32 ChunkZ,
    const FSurfaceGenerationParams& BaseSurface, const FBiomeContext& BiomeCtx,
    const TArray<FSurfaceGenerationParams>& BiomeParams, FChunkBiomeCache& BiomeCache,
    float& OutTerrainZ, float& OutCeilSurf,
    float& OutOverhangAmp, float& OutDirX, float& OutDirY) const
{
    const FSurfaceGenerationParams* PD = &BaseSurface;
    const FSurfaceGenerationParams* PN = &BaseSurface;
    float W = 0.0f;
    if (BiomeCtx.IsValid() && BiomeParams.Num() > 0)
    {
        const FBiomeSample Smp = ResolveBiomeSampleAt(WorldX, WorldY, ChunkZ, BiomeCtx, BiomeCache);
        const int32 Di = BiomeParams.IsValidIndex(Smp.DominantIndex) ? Smp.DominantIndex : 0;
        PD = &BiomeParams[Di];
        if (Smp.NeighborWeight > 0.0f && BiomeParams.IsValidIndex(Smp.NeighborIndex))
        {
            PN = &BiomeParams[Smp.NeighborIndex];
            W = Smp.NeighborWeight;
        }
    }
    OutTerrainZ = ComputeSurfaceTerrainZ(WorldX, WorldY, *PD);
    if (W > 0.0f) OutTerrainZ = FMath::Lerp(OutTerrainZ, ComputeSurfaceTerrainZ(WorldX, WorldY, *PN), W);
    OutCeilSurf = ComputeSurfaceCeiling(WorldX, WorldY, *PD);

    // F20 phase 2 — resolve the per-column OVERHANG gate (strength·slope-gate, biome-blended) and the
    // UPHILL direction. Slope + gradient come from a forward-diff of the STRUCTURAL field (the real
    // landform), computed ONCE here. Off ⇒ amp 0 and the whole block is skipped (no extra samples).
    OutOverhangAmp = 0.0f;
    OutDirX = 0.0f; OutDirY = 0.0f;
    if (PD->OverhangStrength > 0.0f || (W > 0.0f && PN->OverhangStrength > 0.0f))
    {
        // Sample the gradient at ~reach scale (a spot over the void must "see" the nearby cliff to know
        // uphill), but CLAMPED to [4,16] — an un-clamped large Reach would average the slope over a huge
        // span and read even a real cliff as flat (killing the gate; the "big Reach = nothing" bug).
        const float SD = FMath::Clamp(PD->OverhangReach, 4.0f, 16.0f);
        float Ms;
        const float Z0 = SampleSurfaceStructuralZ(WorldX, WorldY, *PD, Ms);
        const float GX = (SampleSurfaceStructuralZ(WorldX + SD, WorldY, *PD, Ms) - Z0) / SD;   // dZ/dX
        const float GY = (SampleSurfaceStructuralZ(WorldX, WorldY + SD, *PD, Ms) - Z0) / SD;   // dZ/dY
        const float Slope = FMath::Sqrt(GX * GX + GY * GY);

        auto Amp = [Slope](const FSurfaceGenerationParams& P) -> float
        {
            if (P.OverhangStrength <= 0.0f) return 0.0f;
            const float Thr  = FMath::Max(P.OverhangSlopeThreshold, 0.05f);
            const float Gate = FMath::Clamp((Slope - Thr) / Thr, 0.0f, 1.0f);
            return P.OverhangStrength * Gate;   // [0,1]; reach comes from the param at apply time
        };
        OutOverhangAmp = (W > 0.0f) ? FMath::Lerp(Amp(*PD), Amp(*PN), W) : Amp(*PD);

        // Unit UPHILL direction (the gradient points uphill). The shelf borrows rock from uphill and
        // extends it downhill over the void. Degenerate on flats — amp is 0 there anyway.
        if (Slope > KINDA_SMALL_NUMBER) { OutDirX = GX / Slope; OutDirY = GY / Slope; }
    }
}

bool UVoxelGenerator::GetSurfaceHeightAt(float WorldX, float WorldY, int32 ChunkZ,
                                         float& OutTerrainZ, float& OutCeilSurf) const
{
    if (!StrateManager) return false;
    const FIntVector ChunkCoord(
        FMath::FloorToInt(WorldX / (float)CHUNK_SIZE),
        FMath::FloorToInt(WorldY / (float)CHUNK_SIZE),
        ChunkZ);
    if (StrateManager->GetGeneratorTypeForChunk(ChunkCoord) != ECaveGeneratorType::SurfaceWorld)
    {
        return false;
    }

    // Own per-chunk cache (independent of GetDensityAt's CP_*) so interleaved gen/deco tasks on the
    // same worker don't thrash each other. All columns of one deco cell share one chunk ⇒ resolve once.
    thread_local FIntVector OC_Chunk(INT32_MAX, INT32_MAX, INT32_MAX);
    thread_local FSurfaceGenerationParams        OC_Surface;
    thread_local FBiomeContext                   OC_BiomeCtx;
    thread_local TArray<FSurfaceGenerationParams> OC_BiomeParams;
    thread_local FChunkBiomeCache                OC_BiomeCache;
    thread_local uint32                          OC_Version = 0xFFFFFFFFu;   // AUDIT C2

    const uint32 OC_LayoutVersion = StrateManager->GetLayoutVersion();
    if (ChunkCoord != OC_Chunk || OC_LayoutVersion != OC_Version)
    {
        if (OC_LayoutVersion != OC_Version) { OC_BiomeCache.Invalidate(); }
        OC_Version = OC_LayoutVersion;
        OC_Chunk = ChunkCoord;
        ResolveSurfaceChunkParams(ChunkCoord, OC_Surface, OC_BiomeCtx, OC_BiomeParams);
    }
    float OcAmp, OcDirX, OcDirY;   // overhang is a density-only 3D shelf; the height oracle ignores it
    ComputeSurfaceColumn(WorldX, WorldY, ChunkZ, OC_Surface, OC_BiomeCtx, OC_BiomeParams, OC_BiomeCache,
                         OutTerrainZ, OutCeilSurf, OcAmp, OcDirX, OcDirY);
    return true;
}

//=============================================================================
// TRIVIAL-TILE CLASSIFICATION (T1.d)
//=============================================================================
// ~84 % des tuiles générées sortent VIDES (tout-roc sous le terrain, tout-air
// au-dessus, cap solide) mais payaient quand même le pré-échantillonnage 33³+
// complet (trace 2026-07-05 : 83 925 GenerateMesh pour 13 227 maillages réels).
// Une 1re tentative (2026-06-26) a été REVERTÉE : borne analytique GLOBALE du
// plafond pas assez conservative (cap bas ⇒ trous dans le toit). Ici on suit la
// prescription du revert : les colonnes sont ÉCHANTILLONNÉES sur le treillis
// exact du mesher (mêmes fonctions ⇒ mêmes floats ⇒ verdict exact, pas une
// estimation), et le cas tout-solide porte des gardes spine/passages/
// disturbances/diff. Tout ce qui n'est pas prouvable ⇒ Mixed (le seul coût d'un
// faux Mixed est du CPU ; un faux AllSolid/AllAir serait un trou).

// A full tile can be Mixed even when every smaller region is provably the same class. This is
// particularly common at level 0: broad operator bounds overlap an unbounded carve, while the
// actual tile-sized sub-boxes are all air. Refine only an already-Mixed proof. Every child must
// resolve, and all resolved children must agree; an unresolved or conflicting child remains Mixed.
// This is a proof refinement, never a density/sample shortcut.
static EVoxelTileClass VF_ClassifyBoxRefined(const FVoxelOpStack& Stack,
                                              const FBox& VoxelBox,
                                              const FVoxelOpContext& Context,
                                              const UVoxelGenerator* Generator,
                                              const UVoxelStrateManager* StructuralManager,
                                              float StructuralBaseDensity = 8.0f,
                                              FVoxelTileClassificationStats* Stats = nullptr)
{
    // The ordinary proof stays shallow.  The finite-domain warp retry is paid only by a
    // tile that already failed the cheap proof, and can afford one more split level because
    // its purpose is to discharge the LOD0 false-Mixed cases without sampling the field.
    const int32 MaxRefinementDepth = Context.bTightenWarpProof ? 5 : 2;

    // Exact certificates are still the mesher's lattice, never a reduced-resolution shortcut.
    // Keep the samples local to this one classifier call. The old recursive fallback evaluated
    // the same shared lattice vertices once per depth-3 leaf (up to 64,000 calls for one LOD0
    // tile); besides being wasteful, that made an empty tile look like a brute-force generation.
    // A byte validity plane is intentional: it is transient, bounded by one tile, and must not
    // become another process-wide cache.
    int32 ExactCacheX0 = 0, ExactCacheY0 = 0, ExactCacheZ0 = 0;
    int32 ExactCacheNX = 0, ExactCacheNY = 0, ExactCacheNZ = 0;
    TArray<float> ExactCoreValues;
    TArray<float> ExactFinalValues;
    TArray<uint8> ExactCoreValid;
    TArray<uint8> ExactFinalValid;
    // A negative structural result is monotone over nested boxes. Compute it once for the root
    // lattice and carry that absence through the refinement tree; otherwise every AllSolid child
    // repeats the same 33³ post-air scan even though a false root result already excludes all of
    // its descendants. A positive result is not cached as absent and keeps the old conservative
    // per-child checks.
    bool bRootPassageAirKnownAbsent = false;
    bool bRootOriginAirKnownAbsent = false;
    const bool bUseExactCache = Context.bUseLatticeProof && Context.Step > 0
        && [&]()
        {
            const float Step = static_cast<float>(Context.Step);
            const FVector Origin(static_cast<float>(Context.LatticeOriginVoxels.X),
                                 static_cast<float>(Context.LatticeOriginVoxels.Y),
                                 static_cast<float>(Context.LatticeOriginVoxels.Z));
            ExactCacheX0 = FMath::CeilToInt((static_cast<float>(VoxelBox.Min.X) - Origin.X)
                                            / Step - 1.0e-4f);
            ExactCacheY0 = FMath::CeilToInt((static_cast<float>(VoxelBox.Min.Y) - Origin.Y)
                                            / Step - 1.0e-4f);
            ExactCacheZ0 = FMath::CeilToInt((static_cast<float>(VoxelBox.Min.Z) - Origin.Z)
                                            / Step - 1.0e-4f);
            const int32 X1 = FMath::FloorToInt((static_cast<float>(VoxelBox.Max.X) - Origin.X)
                                               / Step + 1.0e-4f);
            const int32 Y1 = FMath::FloorToInt((static_cast<float>(VoxelBox.Max.Y) - Origin.Y)
                                               / Step + 1.0e-4f);
            const int32 Z1 = FMath::FloorToInt((static_cast<float>(VoxelBox.Max.Z) - Origin.Z)
                                               / Step + 1.0e-4f);
            ExactCacheNX = X1 - ExactCacheX0 + 1;
            ExactCacheNY = Y1 - ExactCacheY0 + 1;
            ExactCacheNZ = Z1 - ExactCacheZ0 + 1;
            const int64 Count = static_cast<int64>(ExactCacheNX)
                              * static_cast<int64>(ExactCacheNY)
                              * static_cast<int64>(ExactCacheNZ);
            if (ExactCacheNX <= 0 || ExactCacheNY <= 0 || ExactCacheNZ <= 0
                || Count <= 0 || Count > 100000)
            {
                ExactCacheNX = ExactCacheNY = ExactCacheNZ = 0;
                return false;
            }

            const int32 NumVertices = static_cast<int32>(Count);
            ExactCoreValues.SetNumUninitialized(NumVertices);
            ExactFinalValues.SetNumUninitialized(NumVertices);
            ExactCoreValid.Init(0, NumVertices);
            ExactFinalValid.Init(0, NumVertices);
            return true;
        }();

    auto GetLatticeBounds = [&](const FBox& Box,
                                int32& OutX0, int32& OutY0, int32& OutZ0,
                                int32& OutX1, int32& OutY1, int32& OutZ1,
                                int64& OutCount) -> bool
    {
        if (!Context.bUseLatticeProof || Context.Step <= 0)
        {
            return false;
        }
        const float Step = static_cast<float>(Context.Step);
        const FVector Origin(static_cast<float>(Context.LatticeOriginVoxels.X),
                             static_cast<float>(Context.LatticeOriginVoxels.Y),
                             static_cast<float>(Context.LatticeOriginVoxels.Z));
        OutX0 = FMath::CeilToInt((static_cast<float>(Box.Min.X) - Origin.X)
                                 / Step - 1.0e-4f);
        OutY0 = FMath::CeilToInt((static_cast<float>(Box.Min.Y) - Origin.Y)
                                 / Step - 1.0e-4f);
        OutZ0 = FMath::CeilToInt((static_cast<float>(Box.Min.Z) - Origin.Z)
                                 / Step - 1.0e-4f);
        OutX1 = FMath::FloorToInt((static_cast<float>(Box.Max.X) - Origin.X)
                                  / Step + 1.0e-4f);
        OutY1 = FMath::FloorToInt((static_cast<float>(Box.Max.Y) - Origin.Y)
                                  / Step + 1.0e-4f);
        OutZ1 = FMath::FloorToInt((static_cast<float>(Box.Max.Z) - Origin.Z)
                                  / Step + 1.0e-4f);
        OutCount = static_cast<int64>(OutX1 - OutX0 + 1)
                 * static_cast<int64>(OutY1 - OutY0 + 1)
                 * static_cast<int64>(OutZ1 - OutZ0 + 1);
        return OutX1 >= OutX0 && OutY1 >= OutY0 && OutZ1 >= OutZ0 && OutCount > 0;
    };

    auto EvaluateExactLatticeValue = [&](int32 IX, int32 IY, int32 IZ,
                                         bool bUseFinalField) -> float
    {
        const float Step = static_cast<float>(Context.Step);
        const FVector Origin(static_cast<float>(Context.LatticeOriginVoxels.X),
                             static_cast<float>(Context.LatticeOriginVoxels.Y),
                             static_cast<float>(Context.LatticeOriginVoxels.Z));
        TArray<float>& Values = bUseFinalField ? ExactFinalValues : ExactCoreValues;
        TArray<uint8>& Valid = bUseFinalField ? ExactFinalValid : ExactCoreValid;
        int32 CacheIndex = INDEX_NONE;
        if (bUseExactCache
            && IX >= ExactCacheX0 && IX < ExactCacheX0 + ExactCacheNX
            && IY >= ExactCacheY0 && IY < ExactCacheY0 + ExactCacheNY
            && IZ >= ExactCacheZ0 && IZ < ExactCacheZ0 + ExactCacheNZ)
        {
            CacheIndex = (IZ - ExactCacheZ0) * ExactCacheNY * ExactCacheNX
                       + (IY - ExactCacheY0) * ExactCacheNX
                       + (IX - ExactCacheX0);
            if (Valid[CacheIndex] != 0)
            {
                if (Stats)
                {
                    if (bUseFinalField) { ++Stats->ExactFinalCacheHits; }
                    else                { ++Stats->ExactCoreCacheHits; }
                }
                return Values[CacheIndex];
            }
        }

        const float WorldX = Origin.X + static_cast<float>(IX) * Step;
        const float WorldY = Origin.Y + static_cast<float>(IY) * Step;
        const float WorldZ = Origin.Z + static_cast<float>(IZ) * Step;
        // The operator stack is the cheap proof field. A passage/landing post is applied after
        // that stack by GetDensityAt; for a child intersecting one of those modifiers, certify
        // the final MC-facing field rather than trusting the conservative core proof.
        const float Density = bUseFinalField
            ? -Generator->GetDensityAt(WorldX, WorldY, WorldZ)
            : Stack.EvalInternal(WorldX, WorldY, WorldZ);
        if (Stats)
        {
            if (bUseFinalField) { ++Stats->ExactFinalSamples; }
            else                { ++Stats->ExactCoreSamples; }
        }
        if (CacheIndex != INDEX_NONE)
        {
            Values[CacheIndex] = Density;
            Valid[CacheIndex] = 1;
        }
        return Density;
    };

    auto CornersHaveOneSign = [&](const FBox& Box, bool bUseFinalField) -> bool
    {
        int32 IX0 = 0, IY0 = 0, IZ0 = 0, IX1 = 0, IY1 = 0, IZ1 = 0;
        int64 Count = 0;
        if (!GetLatticeBounds(Box, IX0, IY0, IZ0, IX1, IY1, IZ1, Count))
        {
            return false;
        }
        bool bHaveSign = false;
        bool bPositive = false;
        const int32 Xs[2] = { IX0, IX1 };
        const int32 Ys[2] = { IY0, IY1 };
        const int32 Zs[2] = { IZ0, IZ1 };
        for (const int32 IZ : Zs)
        for (const int32 IY : Ys)
        for (const int32 IX : Xs)
        {
            const float Density = EvaluateExactLatticeValue(IX, IY, IZ, bUseFinalField);
            if (!VoxelMath::IsFinite(Density) || Density == 0.0f)
            {
                return false;
            }
            const bool bThisPositive = Density > 0.0f;
            if (!bHaveSign)
            {
                bHaveSign = true;
                bPositive = bThisPositive;
            }
            else if (bPositive != bThisPositive)
            {
                return false;
            }
        }
        return bHaveSign;
    };

    auto ClassifyExactLatticeLeaf = [&](const FBox& Box, bool bUseFinalField) -> EVoxelTileClass
    {
        const uint64 ExactStartCycles = Stats ? FPlatformTime::Cycles64() : 0;
        int32 IX0 = 0, IY0 = 0, IZ0 = 0, IX1 = 0, IY1 = 0, IZ1 = 0;
        int64 Count = 0;
        if (!GetLatticeBounds(Box, IX0, IY0, IZ0, IX1, IY1, IZ1, Count)
            || Count > 8192)
        {
            return EVoxelTileClass::Mixed;
        }
        if (Stats)
        {
            if (bUseFinalField) { ++Stats->ExactFinalLeaves; }
            else                { ++Stats->ExactCoreLeaves; }
        }

        bool bAllSolid = true;
        bool bAllAir = true;
        int32 PositiveCount = 0;
        int32 NegativeCount = 0;
        int32 ZeroOrInvalidCount = 0;
        for (int32 IZ = IZ0; IZ <= IZ1; ++IZ)
        for (int32 IY = IY0; IY <= IY1; ++IY)
        for (int32 IX = IX0; IX <= IX1; ++IX)
        {
            const float Density = EvaluateExactLatticeValue(IX, IY, IZ, bUseFinalField);
            if (!VoxelMath::IsFinite(Density) || Density == 0.0f)
            {
                ++ZeroOrInvalidCount;
                IZ = IZ1;
                IY = IY1;
                break;
            }
            if (Density > 0.0f) { ++PositiveCount; bAllAir = false; }
            else                 { ++NegativeCount; bAllSolid = false; }
            if (!bAllSolid && !bAllAir)
            {
                IZ = IZ1;
                IY = IY1;
                break;
            }
        }
        const EVoxelTileClass Verdict = ZeroOrInvalidCount > 0 || (PositiveCount > 0 && NegativeCount > 0)
            ? EVoxelTileClass::Mixed
            : (bAllSolid ? EVoxelTileClass::AllSolid : EVoxelTileClass::AllAir);
        if (Stats)
        {
            const uint64 Elapsed = FPlatformTime::Cycles64() - ExactStartCycles;
            if (bUseFinalField) { Stats->ExactFinalCycles += Elapsed; }
            else                { Stats->ExactCoreCycles += Elapsed; }
        }
        return Verdict;
    };
    auto Refine = [&](auto&& Self, const FBox& Box, int32 Depth) -> EVoxelTileClass
    {
        if (Stats) { ++Stats->RefineNodes; }
        const uint64 StackStartCycles = Stats ? FPlatformTime::Cycles64() : 0;
        const EVoxelTileClass Whole = Stack.ClassifyBox(Box, Context);
        if (Stats)
        {
            ++Stats->StackBoxCalls;
            Stats->StackBoxCycles += FPlatformTime::Cycles64() - StackStartCycles;
            switch (Whole)
            {
            case EVoxelTileClass::Mixed:    ++Stats->WholeMixedNodes; break;
            case EVoxelTileClass::AllSolid: ++Stats->WholeSolidNodes; break;
            case EVoxelTileClass::AllAir:   ++Stats->WholeAirNodes; break;
            default: break;
            }
            Stats->MaxRefinementDepth = FMath::Max(
                Stats->MaxRefinementDepth, static_cast<uint32>(Depth));
        }
        // The graph stack is not the complete MC field: GetDensityAt reasserts the walkable
        // tunnel core after the stack and after the disturbance/landing posts.  The source's
        // box audit tells us whether that post can have a tunnel candidate in this exact box.
        // Keep this test local to room-graph stacks; the diagnostic is thread-local and would be
        // stale for a slab/maze stack.  A candidate only widens the work, never grants a skip.
        if (Depth == 0 && Context.bUseLatticeProof && StructuralManager != nullptr)
        {
            bRootPassageAirKnownAbsent = !StructuralManager->AnyPassageAirPostNearLattice(
                VoxelBox, Context.LatticeOriginVoxels, Context.Step,
                StructuralBaseDensity, Context.EdgeSealThickness);
            bRootOriginAirKnownAbsent = !StructuralManager->AnyOriginLandingAirNearLattice(
                VoxelBox, Context.LatticeOriginVoxels, Context.Step,
                StructuralBaseDensity, Context.EdgeSealThickness);
        }
        const bool bHasRoomGeometry = Stack.ProvidesResource(VoxelOpResources::RoomGeometry);
        const VoxelDensityOps::FRoomBoxDiagnostic RoomBox = bHasRoomGeometry
            ? VoxelDensityOps::GetLastRoomBoxDiagnostic()
            : VoxelDensityOps::FRoomBoxDiagnostic();
        if (Stats && bHasRoomGeometry)
        {
            Stats->RoomTailQueries += RoomBox.ExactTailQueries;
            Stats->RoomTailEvaluated += RoomBox.ExactTailEvaluated;
            Stats->RoomTailCycles += RoomBox.ExactTailCycles;
            Stats->RoomPropagateCycles += RoomBox.PropagateCycles;
            Stats->RoomExactPrimitiveCycles += RoomBox.ExactPrimitiveCycles;
            Stats->RoomCacheWindowCycles += RoomBox.CacheWindowCycles;
            Stats->RoomNumRooms = RoomBox.NumRooms;
            Stats->RoomNumTunnels = RoomBox.NumTunnels;
            Stats->RoomNumRoomFloorJoins = RoomBox.NumRoomFloorJoins;
            Stats->RoomNumPits = RoomBox.NumPits;
            Stats->RoomNumChimneys = RoomBox.NumChimneys;
        }
        const bool bHasTunnelAirCandidate = bHasRoomGeometry
            && RoomBox.bMayHaveTunnelCoreAir;
        const bool bHasTunnelSupportCandidate = bHasRoomGeometry
            && RoomBox.bMayHaveTunnelSupportFloor;
        // The tail has two polarities.  A solid-only tail cannot turn a proven solid child into
        // geometry, and an air-only tail cannot turn a proven air child into geometry.  Keep the
        // uncertain Mixed parent conservative by descending; at the resolved children only the
        // polarity that can change the answer is allowed to request a final-field certificate.
        const bool bHasTunnelCoreCandidate = Whole == EVoxelTileClass::AllSolid
            ? bHasTunnelAirCandidate
            : Whole == EVoxelTileClass::AllAir
                ? bHasTunnelSupportCandidate
                : false;
        // FPassageCarveOp and FOriginSpine are already in the stack.  Reusing their broad spatial
        // candidates here was the LOD0 bug: a passage that the stack had already proved to miss
        // the sampled sign still forced a final-field refinement of every mixed parent.  Only the
        // writers that run after the stack remain candidates in this section: post-disturbance
        // air, and support floors that can add solid.  Unknown/non-finite input remains a
        // candidate in each exact helper, so this separation cannot authorize a false skip.
        // A Mixed core is not a verdict yet.  Descend until the interval proof resolves its
        // polarity; an air post matters to an AllSolid child, while a solid floor/support post
        // matters to an AllAir child.  Checking both writers on every Mixed ancestor was the
        // remaining LOD0 classifier tax and could not improve the answer before the child split.
        const bool bNeedsPostAirCandidate = Whole == EVoxelTileClass::AllSolid;
        const bool bPassageAirCandidate = bNeedsPostAirCandidate
            && !bRootPassageAirKnownAbsent
            && StructuralManager != nullptr
            && StructuralManager->AnyPassageAirPostNearLattice(
                Box, Context.LatticeOriginVoxels, Context.Step,
                StructuralBaseDensity, Context.EdgeSealThickness);
        const bool bOriginAirCandidate = bNeedsPostAirCandidate
            && !bRootOriginAirKnownAbsent
            && StructuralManager != nullptr
            && StructuralManager->AnyOriginLandingAirNearLattice(
                Box, Context.LatticeOriginVoxels, Context.Step,
                StructuralBaseDensity, Context.EdgeSealThickness);
        // Floors only add solid.  They matter when this core box is already AllAir; on a Mixed
        // parent, descending to its children preserves the proof while avoiding a padded floor
        // AABB forcing a final-field walk over every mixed ancestor.  Ask the exact lattice floor
        // predicate only at an AllAir child.  It is conservative on malformed data and never
        // grants a skip.
        const bool bHasCarveCandidate = bPassageAirCandidate || bOriginAirCandidate;
        const bool bHasStructuralCandidate = Whole == EVoxelTileClass::AllSolid
                ? bHasCarveCandidate
                : Whole == EVoxelTileClass::AllAir
                    && (StructuralManager != nullptr
                    && StructuralManager->AnyLandingFloorAtLattice(
                        Box, Context.LatticeOriginVoxels, Context.Step));
        // A resolved core box still needs a finite-lattice certificate when a common MC tail can
        // rewrite it (passage/landing posts or the graph tunnel-core backstop).  Without this,
        // the interval proves the stack's "all solid" field and silently skips a core tunnel
        // that GetDensityAt opens afterwards.  Boxes with no tail candidate retain the cheap
        // interval path.
        const bool bNeedsFinalField = Generator != nullptr
            && (bHasStructuralCandidate || bHasTunnelCoreCandidate);
        if (Stats && bNeedsFinalField)
        {
            ++Stats->NeedsFinalFieldNodes;
        }
        // A final-field certificate is exact on the same MC lattice as GenerateMesh.  Use it for
        // the small block-sized tail-risk boxes, including an already-resolved AllSolid/AllAir
        // box.  The common streamed tile is deliberately not a final-field sampling job: a
        // coarse tile can contain thousands of MC vertices, and its proof must stay interval-only.
        // Larger tail-risk boxes remain conservative Mixed until the mesher's normal sampling.
        bool bAttemptedExact = false;
        const float FinalFieldBoxExtent = FMath::Max3(
            static_cast<float>(Box.Max.X - Box.Min.X),
            static_cast<float>(Box.Max.Y - Box.Min.Y),
            static_cast<float>(Box.Max.Z - Box.Min.Z));
        const bool bSmallFinalFieldBox = bNeedsFinalField
            && FinalFieldBoxExtent <= 8.0f;
        if (bSmallFinalFieldBox)
        {
            int32 ExactIX0 = 0, ExactIY0 = 0, ExactIZ0 = 0;
            int32 ExactIX1 = 0, ExactIY1 = 0, ExactIZ1 = 0;
            int64 ExactCount = 0;
            if (GetLatticeBounds(Box, ExactIX0, ExactIY0, ExactIZ0,
                                 ExactIX1, ExactIY1, ExactIZ1, ExactCount)
                && ExactCount <= 8192)
            {
                bAttemptedExact = true;
                const EVoxelTileClass Exact = ClassifyExactLatticeLeaf(Box, true);
                if (Exact != EVoxelTileClass::Mixed)
                {
                    return Exact;
                }
                return EVoxelTileClass::Mixed;
            }
        }

        if (Whole != EVoxelTileClass::Mixed && !bNeedsFinalField)
        {
            return Whole;
        }
        // At this size the box contains at most 5^3 exact MC vertices. Evaluating those same
        // vertices is a small, sound certificate for the lattice field the mesher will consume;
        // it is not a lower-resolution fallback and never changes GenerateMesh's sample count.
        // This discharges the last conservative SDF interval misses without walking another
        // refinement level or rebuilding the morphology bounds for every half-box.
        // The corner check is only a work gate. It is not a proof: matching corners still pay the
        // complete exact lattice certificate, so a thin cave or a zero/invalid sample cannot be
        // skipped accidentally. For a genuinely uniform LOD0 empty box this collapses the old
        // 585-node/64k-sample fallback to at most the 33^3 unique lattice vertices.
        // A depth-1 box still contains 17^3 MC vertices; probing its corners and then certifying
        // the whole child is more expensive than the interval refinement it was meant to avoid.
        // Wait until the depth-3 tail (at most 9^3 vertices) before paying an exact core walk.
        const bool bCornersHaveOneSign = !bNeedsFinalField && Depth >= 3
            && CornersHaveOneSign(Box, false);
        if (bCornersHaveOneSign)
        {
            bAttemptedExact = true;
            const EVoxelTileClass Exact = ClassifyExactLatticeLeaf(Box, false);
            if (Exact != EVoxelTileClass::Mixed)
            {
                return Exact;
            }
        }

        // A two-voxel unresolved tail is still cheap to certify on the final MC lattice (3^3
        // points). Doing this before the last split avoids eight more interval folds for the
        // localized passage/post boundary that caused the original LOD0 false-Mixed bail.
        if (!bAttemptedExact && Whole == EVoxelTileClass::Mixed && !bNeedsFinalField
            && Context.bUseLatticeProof && Depth >= 4 && FinalFieldBoxExtent <= 2.0f)
        {
            const EVoxelTileClass ExactFinal = ClassifyExactLatticeLeaf(Box, true);
            if (ExactFinal != EVoxelTileClass::Mixed)
            {
                return ExactFinal;
            }
            bAttemptedExact = true;
        }
        if (!bAttemptedExact && Depth >= 3 && (!bNeedsFinalField || bSmallFinalFieldBox))
        {
            const EVoxelTileClass Exact = ClassifyExactLatticeLeaf(Box, bSmallFinalFieldBox);
            if (Exact != EVoxelTileClass::Mixed)
            {
                return Exact;
            }
            // The final-field leaf is deliberately a conservative sampling certificate.  A
            // mixed result means this child really contains both final signs (or an invalid
            // sample); splitting it again cannot prove the parent uniform and only repeats the
            // expensive post-aware evaluator.
            if (bNeedsFinalField)
            {
                return EVoxelTileClass::Mixed;
            }
        }
        if (Depth >= MaxRefinementDepth)
        {
            // The operator interval can remain Mixed at a one-voxel tail even though the final
            // MC-facing field is uniform after a structural post.  Certify that tiny tail on the
            // exact lattice before conceding the whole tile to the mesher.  This is a proof over
            // all corners the mesher would consume; zero/non-finite values remain Mixed.
            if (Whole == EVoxelTileClass::Mixed && !bNeedsFinalField
                && Context.bUseLatticeProof && FinalFieldBoxExtent <= 1.0f)
            {
                const EVoxelTileClass ExactFinal = ClassifyExactLatticeLeaf(Box, true);
                if (ExactFinal != EVoxelTileClass::Mixed)
                {
                    return ExactFinal;
                }
            }
            return bNeedsFinalField ? EVoxelTileClass::Mixed : Whole;
        }

        const FVector Mid = (Box.Min + Box.Max) * 0.5f;
        if (Stats) { ++Stats->SplitNodes; }
        bool bSawSolid = false;
        bool bSawAir = false;
        for (int32 Z = 0; Z < 2; ++Z)
        for (int32 Y = 0; Y < 2; ++Y)
        for (int32 X = 0; X < 2; ++X)
        {
            const FVector ChildMin(
                X == 0 ? Box.Min.X : Mid.X,
                Y == 0 ? Box.Min.Y : Mid.Y,
                Z == 0 ? Box.Min.Z : Mid.Z);
            const FVector ChildMax(
                X == 0 ? Mid.X : Box.Max.X,
                Y == 0 ? Mid.Y : Box.Max.Y,
                Z == 0 ? Mid.Z : Box.Max.Z);
            const EVoxelTileClass Child = Self(Self, FBox(ChildMin, ChildMax), Depth + 1);
            if (Child == EVoxelTileClass::Mixed)
            {
                return EVoxelTileClass::Mixed;
            }
            bSawSolid |= Child == EVoxelTileClass::AllSolid;
            bSawAir   |= Child == EVoxelTileClass::AllAir;
            if (bSawSolid && bSawAir)
            {
                return EVoxelTileClass::Mixed;
            }
        }
        return bSawSolid ? EVoxelTileClass::AllSolid : EVoxelTileClass::AllAir;
    };
    const EVoxelTileClass Result = Refine(Refine, VoxelBox, 0);
    return Result;
}

// The ordinary proof is deliberately cheap.  Only a box that remains Mixed after its bounded
// spatial refinement pays for the finite-domain warp interval; this keeps the common empty-tile
// path at its existing cost while giving the few conservative misses a second sound chance.
static EVoxelTileClass VF_ClassifyBoxWithWarpRetry(const FVoxelOpStack& Stack,
                                                   const FBox& VoxelBox,
                                                   const FVoxelOpContext& Context,
                                                   const UVoxelGenerator* Generator = nullptr,
                                                   const UVoxelStrateManager* StructuralManager = nullptr,
                                                   float StructuralBaseDensity = 8.0f,
                                                   FVoxelTileClassificationStats* Stats = nullptr)
{
    const EVoxelTileClass Initial = VF_ClassifyBoxRefined(
        Stack, VoxelBox, Context, Generator, StructuralManager, StructuralBaseDensity, Stats);
    // A Mixed fine tile is not evidence of geometry: it can be the conservative result of a
    // coarse operator interval.  Keep the bounded retry for every LOD; skipping it recreates
    // the false-Mixed empty tiles that send the mesher through the expensive path.
    if (Initial != EVoxelTileClass::Mixed
        || Context.bTightenWarpProof)
    {
        return Initial;
    }

    FVoxelOpContext TightContext = Context;
    TightContext.bTightenWarpProof = true;
    return VF_ClassifyBoxRefined(
        Stack, VoxelBox, TightContext, Generator, StructuralManager, StructuralBaseDensity, Stats);
}

EVoxelTileClass UVoxelGenerator::ClassifyTile(const FIntVector& OriginVoxels,
                                              int32 Step, int32 CellsPerAxis) const
{
    return ClassifyTile(OriginVoxels, Step, CellsPerAxis, nullptr);
}

EVoxelTileClass UVoxelGenerator::ClassifyTile(const FIntVector& OriginVoxels, int32 Step,
                                              int32 CellsPerAxis,
                                              FVoxelTileClassificationStats* OutStats) const
{
    if (OutStats)
    {
        *OutStats = FVoxelTileClassificationStats();
    }

    const int32 NormalizedStep = FMath::Max(1, Step);
    const int32 NormalizedCellsPerAxis = FMath::Clamp(CellsPerAxis, 2, CHUNK_SIZE);
    VoxelDensityProfile::AddCounter(
        VoxelDensityProfile::ECounter::TileClassifyCalls);

    const EVoxelTileClass Verdict = ClassifyTileUncached(
        OriginVoxels, NormalizedStep, NormalizedCellsPerAxis, OutStats);
    if (Verdict == EVoxelTileClass::AllSolid)
    {
        VoxelDensityProfile::AddCounter(
            VoxelDensityProfile::ECounter::TileClassifyAllSolid);
    }
    else if (Verdict == EVoxelTileClass::AllAir)
    {
        VoxelDensityProfile::AddCounter(
            VoxelDensityProfile::ECounter::TileClassifyAllAir);
    }
    else
    {
        VoxelDensityProfile::AddCounter(
            VoxelDensityProfile::ECounter::TileClassifyMixed);
    }

    return Verdict;
}

EVoxelTileClass UVoxelGenerator::ClassifyTileUncached(
    const FIntVector& OriginVoxels, int32 Step, int32 CellsPerAxis,
    FVoxelTileClassificationStats* OutStats) const
{
    if (OutStats) { *OutStats = FVoxelTileClassificationStats(); }
    // Mêmes clamps que GenerateMesh — le verdict doit couvrir le treillis réellement échantillonné.
    Step = FMath::Max(1, Step);
    // ClassifyTile runs before GenerateMesh installs its LOD TLS. The room-graph source uses this
    // value only to choose a sound, window-invariant cache region; leaving the previous worker
    // value (normally 1) makes an LOD4 proof rebuild fine per-chunk graph windows across the
    // coarse tile. Scope it exactly like the mesher does so standalone classifier calls are also
    // keyed by their actual lattice step.
    TGuardValue<int32> ClassifySampleStepGuard(VoxelGenLOD::SampleStep, Step);
    const int32 CPA     = FMath::Clamp(CellsPerAxis, 2, CHUNK_SIZE);
    TGuardValue<FIntVector> ClassifyTileOriginGuard(
        VoxelGenLOD::TileOriginVoxels, OriginVoxels);
    TGuardValue<int32> ClassifyTileCellsGuard(
        VoxelGenLOD::TileCellsPerAxis, CPA);
    const int32 GridDim = CPA + 1;   // cell-vertex count: g ∈ [0, CPA]

    // The mesher's outer one-sample halo is used only for central-difference normals. It cannot
    // create a marching-cubes cell: every case index reads vertices g ∈ [0, CPA]. Classify those
    // actual cell vertices, not the halo. Including g=-1/CPA+1 made a coarse tile look Mixed when
    // the halo crossed a surface outside the tile while every cell in the tile was uniform.
    const int32 MinX = OriginVoxels.X, MaxX = OriginVoxels.X + CPA * Step;
    const int32 MinY = OriginVoxels.Y, MaxY = OriginVoxels.Y + CPA * Step;
    const int32 MinZ = OriginVoxels.Z, MaxZ = OriginVoxels.Z + CPA * Step;
    // Division entière PLANCHER (les coords négatives tronquent vers 0 en C++ — pas floor).
    auto FloorDivC = [](int32 A, int32 B) -> int32
    {
        const int32 Q = A / B, R = A % B;
        return (R != 0 && ((R < 0) != (B < 0))) ? Q - 1 : Q;
    };

    // ── Garde diff layer : un mod joueur peut creuser OU remplir n'importe où. ──
    if (DiffLayer && DiffLayer->HasAnyMods())
    {
        const FIntVector MinChunk(FloorDivC(MinX, CHUNK_SIZE), FloorDivC(MinY, CHUNK_SIZE), FloorDivC(MinZ, CHUNK_SIZE));
        const FIntVector MaxChunk(FloorDivC(MaxX, CHUNK_SIZE), FloorDivC(MaxY, CHUNK_SIZE), FloorDivC(MaxZ, CHUNK_SIZE));
        if (DiffLayer->HasAnyModInChunkRange(MinChunk, MaxChunk)) return EVoxelTileClass::Mixed;
    }

    // The bounded shell is global: it also covers gaps and out-of-layout air, so prove it before
    // consulting the strate layout. The final MC edge pass uses a positive base, hence 1.0 is a
    // deliberately conservative proof input here; the stack op performs the same proof with the
    // archetype base for its own sub-box.
    const FBox TileVoxelBox(FVector((float)MinX, (float)MinY, (float)MinZ),
                            FVector((float)MaxX, (float)MaxY, (float)MaxZ));
    if (VF_XYEdgeSealBoxIsForcedSolid(TileVoxelBox, WorldRadiusVoxels,
                                      EdgeSealThickness, 1.0f))
    {
        return EVoxelTileClass::AllSolid;
    }

    if (!StrateManager) return EVoxelTileClass::Mixed;

    // A selected recipe must be classified by the exact same materialised stack that supplies
    // density. Search every touched chunk-Z first: the minimum corner may be in a gap or adjacent
    // slot, and letting that fall through to the native factory would prove the wrong field.
    int32 RecipeSeed = 0;
    ECaveGeneratorType RecipeArchetype = ECaveGeneratorType::TunnelNetwork;
    FVoxelStrateArchetypeParams RecipeParams;
    FVoxelOpStackRecipe Recipe;
    bool bTouchesRecipe = false;
    const int32 RecipeMinChunkZ = FloorDivC(MinZ, CHUNK_SIZE);
    const int32 RecipeMaxChunkZ = FloorDivC(MaxZ, CHUNK_SIZE);
    for (int32 CZ = RecipeMinChunkZ; CZ <= RecipeMaxChunkZ; ++CZ)
    {
        if (StrateManager->GetRecipeForChunk(
                FIntVector(0, 0, CZ), RecipeSeed, RecipeArchetype,
                RecipeParams, Recipe))
        {
            bTouchesRecipe = true;
            break;
        }
    }
    if (bTouchesRecipe)
    {
        int32 RecipeTopChunkZ = 0;
        int32 RecipeBottomChunkZ = 0;
        if (!StrateManager->GetStrateChunkZBounds(
                RecipeMinChunkZ, RecipeTopChunkZ, RecipeBottomChunkZ)
                || RecipeMinChunkZ < RecipeBottomChunkZ
                || RecipeMaxChunkZ > RecipeTopChunkZ)
        {
            return EVoxelTileClass::Mixed;
        }

        // Recipes and disturbances are selected from the strate slot's Z only.  The manager
        // copies the recipe arrays into the output parameters, so repeating this check for every
        // XY key in the sampled box turns one coarse tile into thousands of identical deep copies.
        // Keep one representative per distinct Z: this is equivalent for the APIs above and still
        // rejects a tile when any touched Z changes recipe, bounds, or disturbance state.
        for (int32 CZ = RecipeMinChunkZ; CZ <= RecipeMaxChunkZ; ++CZ)
        {
            int32 TouchedTop = 0;
            int32 TouchedBottom = 0;
            if (!StrateManager->GetStrateChunkZBounds(CZ, TouchedTop, TouchedBottom)
                || TouchedTop != RecipeTopChunkZ || TouchedBottom != RecipeBottomChunkZ)
            {
                return EVoxelTileClass::Mixed;
            }
            int32 TouchedSeed = 0;
            ECaveGeneratorType TouchedArchetype = ECaveGeneratorType::TunnelNetwork;
            FVoxelStrateArchetypeParams TouchedParams;
            FVoxelOpStackRecipe TouchedRecipe;
            const FIntVector TouchedChunk(0, 0, CZ);
            if (!StrateManager->GetRecipeForChunk(
                    TouchedChunk, TouchedSeed, TouchedArchetype,
                    TouchedParams, TouchedRecipe)
                || TouchedSeed != RecipeSeed
                || TouchedArchetype != RecipeArchetype)
            {
                return EVoxelTileClass::Mixed;
            }

            const FStrateDisturbanceParams Disturbances =
                StrateManager->GetDisturbanceParamsForChunk(TouchedChunk);
            if (VF_AnyDisturbanceCanTouchBox(TileVoxelBox, Disturbances, (uint32)Seed))
            {
                return EVoxelTileClass::Mixed;
            }
        }

        FVoxelOpStack RecipeStack;
        FVoxelOpContext RecipeContext;
        if (!VF_BuildStackFromRecipe(
                Recipe, RecipeParams, RecipeSeed, OriginSpineRadius,
                StrateManager, RecipeStack, RecipeContext, nullptr))
        {
            return EVoxelTileClass::Mixed;
        }
        RecipeContext.ChunkCoord = FIntVector(
            FloorDivC(MinX, CHUNK_SIZE), FloorDivC(MinY, CHUNK_SIZE), RecipeMinChunkZ);
        RecipeContext.Step = Step;
        RecipeContext.LayoutVersion = StrateManager->GetLayoutVersion();
        RecipeContext.WorldRadiusVoxels = WorldRadiusVoxels;
        RecipeContext.EdgeSealThickness = EdgeSealThickness;
        RecipeContext.bUseLatticeProof = true;
        RecipeContext.LatticeOriginVoxels = OriginVoxels;
        RecipeStack.PrepareChunk(RecipeContext);
        const EVoxelTileClass RecipeVerdict =
            VF_ClassifyBoxWithWarpRetry(RecipeStack, TileVoxelBox, RecipeContext,
                                        nullptr, nullptr, 8.0f, OutStats);
        if (RecipeVerdict == EVoxelTileClass::Mixed)
        {
        }
        return RecipeVerdict;
    }

#if WITH_EDITOR
    // A multi-region composer override is already a complete parent stack: it owns the lateral
    // blend and the four global structural posts.  Classify it as one field only when the complete
    // mesher box stays inside that one strate; otherwise the Z-category pass below deliberately
    // returns Mixed.  Disturbances and the diff layer remain outside the stack, so either one also
    // vetoes a skip.
    FVoxelStrateRegionManifest ComposerRegions;
    const FIntVector RegionProbeChunk(
        FloorDivC(MinX, CHUNK_SIZE), FloorDivC(MinY, CHUNK_SIZE), FloorDivC(MinZ, CHUNK_SIZE));
    if (StrateManager->GetComposerRegionOverrideForChunk(RegionProbeChunk, ComposerRegions))
    {
        int32 RegionTopChunkZ = 0;
        int32 RegionBottomChunkZ = 0;
        const int32 RegionMinChunkZ = FloorDivC(MinZ, CHUNK_SIZE);
        const int32 RegionMaxChunkZ = FloorDivC(MaxZ, CHUNK_SIZE);
        if (!StrateManager->GetStrateChunkZBounds(RegionProbeChunk.Z,
                                                  RegionTopChunkZ, RegionBottomChunkZ)
            || RegionMinChunkZ < RegionBottomChunkZ
            || RegionMaxChunkZ > RegionTopChunkZ)
        {
            return EVoxelTileClass::Mixed;
        }

        // A representative chunk is not enough for a proof: the mesher box can cross an XY
        // chunk edge, and disturbances are chunk-local.  Require every touched chunk to remain
        // in the same slot and to have no active disturbance before delegating the proof to the
        // complete lateral stack.  Any uncertainty is Mixed; a false uniform answer here can
        // skip a real seam or carve.
        const int32 RegionMinChunkX = FloorDivC(MinX, CHUNK_SIZE);
        const int32 RegionMaxChunkX = FloorDivC(MaxX, CHUNK_SIZE);
        const int32 RegionMinChunkY = FloorDivC(MinY, CHUNK_SIZE);
        const int32 RegionMaxChunkY = FloorDivC(MaxY, CHUNK_SIZE);
        for (int32 CZ = RegionMinChunkZ; CZ <= RegionMaxChunkZ; ++CZ)
        {
            int32 TouchedTopChunkZ = 0;
            int32 TouchedBottomChunkZ = 0;
            if (!StrateManager->GetStrateChunkZBounds(
                    CZ, TouchedTopChunkZ, TouchedBottomChunkZ)
                || TouchedTopChunkZ != RegionTopChunkZ
                || TouchedBottomChunkZ != RegionBottomChunkZ)
            {
                return EVoxelTileClass::Mixed;
            }
            for (int32 CY = RegionMinChunkY; CY <= RegionMaxChunkY; ++CY)
            {
                for (int32 CX = RegionMinChunkX; CX <= RegionMaxChunkX; ++CX)
                {
                    const FIntVector TouchedChunk(CX, CY, CZ);
                    FVoxelStrateRegionManifest TouchedRegions;
                    if (!StrateManager->GetComposerRegionOverrideForChunk(
                            TouchedChunk, TouchedRegions)
                        || TouchedRegions.StrateIndex != ComposerRegions.StrateIndex
                        || TouchedRegions.Seed != ComposerRegions.Seed
                        || TouchedRegions.RegionCount != ComposerRegions.RegionCount
                        || TouchedRegions.PartitionSeed != ComposerRegions.PartitionSeed
                        || TouchedRegions.LatticeCellSize != ComposerRegions.LatticeCellSize
                || TouchedRegions.BlendWidth != ComposerRegions.BlendWidth)
                    {
                        return EVoxelTileClass::Mixed;
                    }

                    const FStrateDisturbanceParams Disturbances =
                        StrateManager->GetDisturbanceParamsForChunk(TouchedChunk);
                    if (VF_AnyDisturbanceCanTouchBox(TileVoxelBox, Disturbances, (uint32)Seed))
                    {
                        return EVoxelTileClass::Mixed;
                    }
                }
            }
        }

        FVoxelOpStack RegionStack;
        FVoxelOpContext RegionContext;
        if (!VF_BuildStrateRegionStack(ComposerRegions, OriginSpineRadius,
                                        StrateManager, RegionStack, RegionContext, nullptr))
        {
            return EVoxelTileClass::Mixed;
        }
        RegionContext.ChunkCoord = RegionProbeChunk;
        RegionContext.Step = Step;
        RegionContext.LayoutVersion = StrateManager->GetLayoutVersion();
        RegionContext.WorldRadiusVoxels = WorldRadiusVoxels;
        RegionContext.EdgeSealThickness = EdgeSealThickness;
        RegionContext.bUseLatticeProof = true;
        RegionContext.LatticeOriginVoxels = OriginVoxels;
        RegionStack.PrepareChunk(RegionContext);
        const EVoxelTileClass RegionVerdict =
            VF_ClassifyBoxWithWarpRetry(RegionStack, TileVoxelBox, RegionContext,
                                        nullptr, nullptr, 8.0f, OutStats);
        if (RegionVerdict == EVoxelTileClass::Mixed)
        {
        }
        return RegionVerdict;
    }

#endif

    bool bCanSolid = true;   // "tout le treillis est solide" encore prouvable
    bool bCanAir   = true;   // "tout le treillis est air" encore prouvable
    // ── Catégorisation par Z du treillis : gap bedrock = solide ; hors layout = air constant ;
    //    SurfaceWorld = test colonne ; un slot cave opt-in = verdict de pile sur sa sous-boîte. ──
    struct FSurfSlot
    {
        int32 BotChunkZ = INT32_MAX;   // identité du slot (borne basse de la strate, en chunks)
        int32 RepChunkZ = 0;           // chunk Z représentatif pour la résolution des params
        int32 StrateKey = MIN_int32;   // même clé que GetDensityAt (StrateBottomWorldZ arrondi)
        float OverhangMargin = 0.0f;   // F20 : hauteur max de corniche (= max OverhangHeight strate+biomes)
        FSurfaceGenerationParams         Params;
        FBiomeContext                    BiomeCtx;
        TArray<FSurfaceGenerationParams> BiomeParams;
        TArray<float>                    InteriorZ;   // z hors bandes de seal → testés par colonne
    };
    // thread_local : les TArray gardent leur capacité d'un appel à l'autre (zéro malloc/tuile).
    static thread_local FSurfSlot Slots[2];
    static thread_local FChunkBiomeCache TC_BiomeCache;   // biome grid du classifieur (valeurs ≡ CP_BiomeCache)
    // AUDIT C2 — même discipline que le chemin densité : la grille de biome du classifieur survit
    // d'un appel à l'autre et sa boîte de validité ne dit rien du contexte qui l'a produite.
    static thread_local uint32 TC_SeenVersion = 0xFFFFFFFFu;
    const uint32 TC_LayoutVersion = StrateManager->GetLayoutVersion();
    if (TC_LayoutVersion != TC_SeenVersion)
    {
        TC_SeenVersion = TC_LayoutVersion;
        TC_BiomeCache.Invalidate();
    }
    int32 NumSlots = 0;

    // ── T1.d GÉNÉRIQUE : la pile d'opérateurs classe les archétypes de CAVE ──
    // Ces valeurs suivent l'UNIQUE slot de cave que la tuile touche. La pile classera seulement la
    // sous-boîte Z de ses échantillons ; les catégories gap/surface/hors-layout plient séparément
    // leurs hypothèses dans `bCanSolid` / `bCanAir`.
    // These values track the ONE cave slot touched by the tile. The stack classifies only its
    // sampled Z sub-box; gap/surface/out-of-layout fold their hypotheses separately.
    int32 CaveBotChunkZ = INT32_MAX;   // identité du slot de cave (borne basse, en chunks)
    int32 CaveRepChunkZ = 0;
    int32 CaveMinZ       = MAX_int32;
    int32 CaveMaxZ       = MIN_int32;
    bool  bAnyCave        = false;
    bool  bAnyGap         = false;
    bool  bAnySurface     = false;
    bool  bAnyOutOfLayout = false;

    int32 MemoChunkZ  = INT32_MAX;
    int32 MemoCat     = -1;            // 0 = gap, 1 = surface, 2 = cave (pile), 3 = hors layout
    int32 MemoSlotIdx = -1;

    // « Ce chunk Z appartient-il à une strate ? » sous forme publique : `FindSlotIndexForChunkZ`
    // est `protected`, `GetStrateChunkZBounds` rend false pour exactement le même cas.
    auto VF_ChunkZHasSlot = [&](int32 Z) -> bool
    {
        int32 UnusedTopCZ = 0, UnusedBotCZ = 0;
        return StrateManager->GetStrateChunkZBounds(Z, UnusedTopCZ, UnusedBotCZ);
    };
    for (int32 g = 0; g <= CPA; ++g)
    {
        const int32 Zi = OriginVoxels.Z + g * Step;
        const int32 ChunkZ = FloorDivC(Zi, CHUNK_SIZE);
        if (ChunkZ != MemoChunkZ)
        {
            MemoChunkZ = ChunkZ;
            const FIntVector CC(0, 0, ChunkZ);   // les requêtes de layout ne dépendent que de Z
            if (StrateManager->IsGapChunk(CC))
            {
                MemoCat = 0;
                bAnyGap = true;
            }
            //=================================================================
            // ⛔ HORS LAYOUT = AIR CONSTANT. C'ÉTAIT LE BLOCAGE DE T1.d.
            //=================================================================
            // `GetGeneratorTypeForChunk` rend `TunnelNetwork` pour tout chunk hors de la pile de
            // strates (« le chemin de repli produit de la roche de toute façon » — CE COMMENTAIRE
            // EST FAUX) et `IsGapChunk` rend false au-dessus du sommet (« open air, NOT a gap »).
            // Résultat : chaque tuile touchant l'air libre au-dessus du monde entrait dans la
            // BRANCHE DE CAVE, n'y trouvait aucun slot, et abandonnait — mesuré en jeu à 83 % des
            // tuiles classées (`Cave Bail Not Op Stack No Layout` = 1.58 / 1.90).
            //
            // La vérité est dans `GetGenerationParams` : hors layout il rend `BaseDensity = -1`,
            // `RoomDensity = 0`, `WormStrength = 0` — un champ CONSTANT, donc de l'air, sans salle
            // ni ver pour le percer. Une telle tuile est prouvable sans échantillonner.
            //
            // Out-of-layout is a CONSTANT AIR field, not a cave archetype. Every tile touching the
            // open air above the world was being routed into the cave branch and bailing there.
            // `GetStrateChunkZBounds` (PUBLIC) rend false exactement quand `FindSlotIndexForChunkZ`
            // rend -1 — ce dernier est `protected`, et cette fonction l'utilise déjà deux fois pour
            // la même question. Pas de nouvelle surface d'API pour un prédicat qui existe.
            // GetStrateChunkZBounds is the public form of "has a layout slot"; the index accessor
            // is protected and this function already uses the bounds call twice for the same test.
            else if (!VF_ChunkZHasSlot(ChunkZ))
            {
                MemoCat = 3;
                bAnyOutOfLayout = true;

                // Les disturbances sont appliquées APRÈS la densité d'archétype et peuvent AJOUTER
                // de la roche (ponts, arêtes). Même prudence que les branches gap et cave : si
                // l'une peut agir ici, on ne prouve rien. Les chasms ne font que creuser ⇒ ils ne
                // menacent pas un verdict d'air.
                const FStrateDisturbanceParams DOut = StrateManager->GetDisturbanceParamsForChunk(CC);
                if (VF_AnyBridgeCanTouchBox(TileVoxelBox, DOut, (uint32)Seed)
                    || VF_AnyRidgeCanTouchBox(TileVoxelBox, DOut, (uint32)Seed))
                {
                    return EVoxelTileClass::Mixed;
                }
            }
            else if (StrateManager->GetGeneratorTypeForChunk(CC) == ECaveGeneratorType::SurfaceWorld)
            {
                // Retrouve (ou résout) le slot de strate — l'identité vient des bornes chunk-Z du layout.
                int32 TopCZ = 0, BotCZ = 0;
                if (!StrateManager->GetStrateChunkZBounds(ChunkZ, TopCZ, BotCZ))
                {
                    return EVoxelTileClass::Mixed;
                }
                MemoSlotIdx = -1;
                for (int32 s = 0; s < NumSlots; ++s)
                {
                    if (Slots[s].BotChunkZ == BotCZ) { MemoSlotIdx = s; break; }
                }
                if (MemoSlotIdx < 0)
                {
                    if (NumSlots >= 2)
                    {
                        return EVoxelTileClass::Mixed;   // >2 strates surface dans une tuile : improbable
                    }
                    MemoSlotIdx = NumSlots++;
                    FSurfSlot& S = Slots[MemoSlotIdx];
                    S.BotChunkZ  = BotCZ;
                    S.RepChunkZ  = ChunkZ;
                    S.InteriorZ.Reset();
                    ResolveSurfaceChunkParams(CC, S.Params, S.BiomeCtx, S.BiomeParams);
                    S.StrateKey  = FMath::RoundToInt(S.Params.StrateBottomWorldZ);

                    // F20 : marge d'overhang = hauteur max (OverhangHeight) sur laquelle une corniche
                    // peut AJOUTER de la roche AU-DESSUS du sol. La corniche est plafonnée à
                    // TerrainZ+Height ⇒ au-delà, air prouvable ; dans (TerrainZ, TerrainZ+Height], ni air
                    // ni solide prouvable → colonne forcée en Mixed. (L'union n'enlève jamais de roche ⇒
                    // sous le terrain reste solide prouvable : marge vers le HAUT uniquement.)
                    S.OverhangMargin = (S.Params.OverhangStrength > 0.0f) ? S.Params.OverhangHeight : 0.0f;
                    for (const FSurfaceGenerationParams& BP : S.BiomeParams)
                    {
                        if (BP.OverhangStrength > 0.0f)
                            S.OverhangMargin = FMath::Max(S.OverhangMargin, BP.OverhangHeight);
                    }

                    // Disturbances de cette strate : les chasms CREUSENT (cassent AllSolid), les
                    // ponts/arêtes AJOUTENT de la roche dans l'intérieur (cassent AllAir).
                    const FStrateDisturbanceParams D = StrateManager->GetDisturbanceParamsForChunk(CC);
                    if (D.ChasmDensity  > 0.0f) bCanSolid = false;
                    if (D.BridgeDensity > 0.0f || D.RidgeDensity > 0.0f) bCanAir = false;
                }
                MemoCat = 1;
                bAnySurface = true;
            }
            else
            {
                // ── ARCHÉTYPE DE CAVE ── Jusqu'ici : `return Mixed`, sans appel. Désormais on tente
                // le pliage générique de la pile — mais SEULEMENT sous des conditions vérifiables,
                // parce qu'un faux verdict ici est un trou (pas de géométrie, pas de collision).
                //
                // Condition 1 : la strate doit RÉELLEMENT être générée par la pile. Sinon on
                // classerait un champ que le mesher ne produira pas. C'est le même drapeau, lu au
                // même endroit, que `GetDensityAt`.
                if (!StrateManager->UsesOperatorStackForChunk(CC))
                {
                    // Attribution DIAGNOSTIQUE uniquement : l'ancien compteur mélangeait une
                    // strate cave entièrement désactivée avec une tuile de frontière qui avait
                    // rencontré un slot désactivé avant la garde « slot différent » ci-dessous.
                    // On résout les bornes APRÈS l'échec du même prédicat ; elles ne changent ni
                    // la condition, ni le point de retour, ni le verdict.
                    // Diagnostic attribution only: the old counter mixed a wholly disabled cave
                    // slot with a boundary tile that met a disabled slot before the different-slot
                    // guard below. Resolve bounds only after the same predicate fails; classification
                    // control flow and return value stay unchanged.
                    int32 FailedTopCZ = 0, FailedBotCZ = 0;
                    if (!StrateManager->GetStrateChunkZBounds(ChunkZ, FailedTopCZ, FailedBotCZ))
                    {
                        INC_DWORD_STAT(STAT_VoxelForgeCaveBailNotOpStackNoLayout);
                    }
                    else
                    {
                        const int32 TileMinCZ = FloorDivC(MinZ, CHUNK_SIZE);
                        const int32 TileMaxCZ = FloorDivC(MaxZ, CHUNK_SIZE);
                        if (TileMinCZ >= FailedBotCZ && TileMaxCZ <= FailedTopCZ)
                        {
                            INC_DWORD_STAT(STAT_VoxelForgeCaveBailNotOpStackSoleSlot);
                        }
                        else
                        {
                            INC_DWORD_STAT(STAT_VoxelForgeCaveBailNotOpStackBoundaryTile);
                        }
                    }
                    return EVoxelTileClass::Mixed;
                }

                // Condition 2 : un seul slot de cave par tuile. Deux slots = deux jeux de params =
                // deux piles, et une pile ne sait répondre que pour SA strate.
                int32 CaveTopCZ = 0, CaveBotCZ = 0;
                if (!StrateManager->GetStrateChunkZBounds(ChunkZ, CaveTopCZ, CaveBotCZ))
                {
                    INC_DWORD_STAT(STAT_VoxelForgeCaveMixOutOfLayout);
                    return EVoxelTileClass::Mixed;   // hors layout
                }
                if (CaveBotChunkZ != INT32_MAX && CaveBotChunkZ != CaveBotCZ)
                {
                    INC_DWORD_STAT(STAT_VoxelForgeCaveBailTwoCaveSlots);
                    return EVoxelTileClass::Mixed;
                }
                CaveBotChunkZ = CaveBotCZ;
                CaveRepChunkZ = ChunkZ;
                bAnyCave = true;
                MemoCat  = 2;
            }
        }

        if (MemoCat == 2)
        {
            // La pile répond après la boucle, sur la sous-boîte Z contenant exactement les
            // échantillons cave (XY reste la boîte complète du treillis).
            CaveMinZ = FMath::Min(CaveMinZ, Zi);
            CaveMaxZ = FMath::Max(CaveMaxZ, Zi);
        }
        else if (MemoCat == 0)
        {
            bCanAir = false;   // bedrock du gap = solide (le carve des passages est déjà gardé)
        }
        else if (MemoCat == 3)
        {
            // Hors layout = air constant (BaseDensity = -1, aucune salle, aucun ver). L'hypothèse
            // « tout solide » meurt ; « tout air » survit. Les passages et la spine ne font que
            // creuser — ils sont déjà gardés plus haut et ne peuvent pas rendre ce z solide.
            // Out of layout = constant air: AllSolid dies, AllAir survives.
            bCanSolid = false;
        }
        else
        {
            FSurfSlot& S = Slots[MemoSlotIdx];
            // Bande de seal ? Mêmes inégalités qu'ApplyBoundarySeal : à l'intérieur, la densité est
            // Max(…, SealFactor·BaseDensity) avec SealFactor > 0 ⇒ solide garanti si BaseDensity > 0.
            const float Z       = (float)Zi;
            const float DistTop = S.Params.StrateTopWorldZ - Z;
            const float DistBot = Z - S.Params.StrateBottomWorldZ;
            const float Th      = S.Params.BoundarySealThickness;
            const bool bInBand  = Th > 0.0f && S.Params.BaseDensity > 0.0f
                && ((DistTop >= 0.0f && DistTop < Th) || (DistBot >= 0.0f && DistBot < Th));
            if (bInBand) { bCanAir = false; }
            else         { S.InteriorZ.Add(Z); }
        }
        if (!bCanSolid && !bCanAir)
        {
            return EVoxelTileClass::Mixed;
        }
    }

    // Out-of-layout is a constant-air field. Passage tubes and origin rooms can only carve that
    // field, so they cannot invalidate AllAir; only their guaranteed support floors can add solid
    // voxels. Do this after the cheap Z categorisation so open-air LOD tiles avoid the exact
    // passage-carve lattice walk and the room scan entirely. A floor-only tile remains Mixed and
    // is meshed normally, preserving the support backstops.
    if (bAnyOutOfLayout && !bAnyCave && !bAnySurface && !bAnyGap)
    {
        if (StrateManager->AnyPassageLandingFloorNearLattice(
                TileVoxelBox, OriginVoxels, Step)
            || StrateManager->AnyOriginLandingFloorNearLattice(
                TileVoxelBox, OriginVoxels, Step))
        {
            return EVoxelTileClass::Mixed;
        }
        return EVoxelTileClass::AllAir;
    }

    // ── Structural passage/origin guards for non-open-air categories. ──
    // Carvers kill AllSolid but never add rock; landing and origin support floors kill AllAir.
    if (StrateManager->AnyPassageNearLattice(
            TileVoxelBox, OriginVoxels, Step))
    {
        bCanSolid = false;
    }
    if (StrateManager->AnyPassageLandingFloorNearLattice(
            TileVoxelBox, OriginVoxels, Step))
    {
        bCanAir = false;
    }
    if (StrateManager->AnyOriginLandingNearLattice(
            TileVoxelBox, OriginVoxels, Step))
    {
        bCanSolid = false;
    }
    if (StrateManager->AnyOriginLandingFloorNearLattice(
            TileVoxelBox, OriginVoxels, Step))
    {
        bCanAir = false;
    }
    // ── LE PLIAGE DE LA PILE D'OPÉRATEURS, POUR LES ARCHÉTYPES DE CAVE ──
    //=========================================================================
    // ⚠️ ERREUR ICI = TROU, PAS RÉGRESSION. Un verdict non-Mixed fait SAUTER `GenerateMesh` : pas
    // de triangles, pas de collision, invisible jusqu'à ce qu'un joueur tombe au travers. Toutes les
    // gardes ci-dessous ÉCHOUENT EN MIXED ; aucune ne donne le bénéfice du doute.
    //
    // Le verdict lui-même ne peut pas être meilleur que la pile : `FVoxelOpStack::ClassifyBox` plie
    // chaque opérateur avec `VF_FoldOp` et rend `Mixed` dès que les deux hypothèses meurent. Ce que
    // ce bloc ajoute, c'est la vérification que la pile interrogée est bien CELLE QUI PRODUIRA LA
    // DENSITÉ de cette tuile — même fabrique, mêmes params, même drapeau.
    if (bAnyCave)
    {
        // Diagnostic de PRÉSENCE avant les gardes : le signal reste visible même si le pliage rend
        // finalement AllSolid/AllAir. Ces compteurs ne sont pas exclusifs entre eux sur une tuile
        // très haute ; chacun répond exactement à « cette catégorie était-elle aussi présente ? ».
        // Presence diagnostics run before the guards, so a successful fold cannot hide the mix.
        // They are not mutually exclusive for a very tall tile; each answers one exact question.
        if (bAnyOutOfLayout || bAnyGap || bAnySurface)
        {
            if (bAnyOutOfLayout) { INC_DWORD_STAT(STAT_VoxelForgeCaveMixOutOfLayout); }
            if (bAnyGap)         { INC_DWORD_STAT(STAT_VoxelForgeCaveMixGap); }
            if (bAnySurface)     { INC_DWORD_STAT(STAT_VoxelForgeCaveMixSurfaceWorld); }
        }

        const FIntVector RepCC(0, 0, CaveRepChunkZ);
        const ECaveGeneratorType CaveType = StrateManager->GetGeneratorTypeForChunk(RepCC);

        //---------------------------------------------------------------------
        // ⚠️ LA GARDE QUI COMPTE : LES PARAMS DOIVENT ÊTRE LES MÊMES SUR TOUTE LA SOUS-BOÎTE CAVE
        //---------------------------------------------------------------------
        // `GetGenerationParams` et ses homologues BLENDENT les params dans les bandes de transition :
        // `Alpha` dépend du chunk Z pour `Gradient`, et du chunk XY EN PLUS pour `Interleaved`. Deux
        // chunks d'une même sous-boîte peuvent donc porter des params différents — c'est le constat de
        // `AUDIT §C2`, confirmé par lecture le 2026-07-28 — et UNE pile ne peut pas représenter DEUX
        // champs. On construit donc les params pour CHAQUE coordonnée de chunk que la boîte touche et
        // on exige qu'ils soient identiques bit à bit.
        //
        // `Memcmp` sur un POD : un padding différent ne peut produire qu'un FAUX ÉCART, donc un
        // `Mixed` de trop. On se trompe du côté du CPU, jamais du côté du trou.
        const int32 CX0 = FloorDivC(MinX, CHUNK_SIZE), CX1 = FloorDivC(MaxX, CHUNK_SIZE);
        const int32 CY0 = FloorDivC(MinY, CHUNK_SIZE), CY1 = FloorDivC(MaxY, CHUNK_SIZE);
        // IMPORTANT : les gardes restent complètes, mais seulement sur les chunks où le mesher
        // échantillonne réellement CETTE strate cave. Inclure gap/surface/hors-layout ici ferait
        // échouer la garde d'archétype avant de pouvoir plier leurs hypothèses indépendantes.
        // The guards stay exhaustive over the cave samples. Non-cave chunks are intentionally not
        // represented by this stack; their hypotheses were folded separately in the Z pass.
        const int32 CZ0 = FloorDivC(CaveMinZ, CHUNK_SIZE), CZ1 = FloorDivC(CaveMaxZ, CHUNK_SIZE);

        // Keep the parameter identity check bounded, but do not use the old 27-key cutoff here:
        // LOD4 legitimately covers 19^3 = 6,859 chunk keys.  Returning Mixed at that point sent
        // the tile straight to GenerateMesh, where GetDensityAt rebuilt one exact-chunk state for
        // almost every coarse sample.  The operator proof now shares its root morphology cache
        // across refined children, so this scan is cheap compared with the fallback it prevents.
        const int64 NumChunkCoords = (int64)(CX1 - CX0 + 1) * (int64)(CY1 - CY0 + 1) * (int64)(CZ1 - CZ0 + 1);
        constexpr int64 MaxClassifiedChunkCoords = 262144;
        if (NumChunkCoords > MaxClassifiedChunkCoords)
        {
            INC_DWORD_STAT(STAT_VoxelForgeCaveBailParams);
            return EVoxelTileClass::Mixed;
        }

        FSlabGenerationParams   TileSlab;
        FMazeGenerationParams   TileMaze;
        FVerticalShaftParams    TileVert;
        FFloatingIslandParams   TileFloat;
        FStrateGenerationParams TileTunnel;
        bool bFirst = true;

        for (int32 cz = CZ0; cz <= CZ1; ++cz)
        for (int32 cy = CY0; cy <= CY1; ++cy)
        for (int32 cx = CX0; cx <= CX1; ++cx)
        {
            const FIntVector CC(cx, cy, cz);
            if (StrateManager->GetGeneratorTypeForChunk(CC) != CaveType)
            {
                INC_DWORD_STAT(STAT_VoxelForgeCaveBailParams);
                return EVoxelTileClass::Mixed;   // la boîte déborde sur un autre archétype
            }

            // Le drapeau doit tenir sur TOUS les chunks de la boîte, pas seulement sur celui qui a
            // déclenché la tentative : un seul chunk hors pile invaliderait le verdict.
            if (!StrateManager->UsesOperatorStackForChunk(CC))
            {
                // Le passage Z précédent a déjà accepté l'unique slot cave. Avec le layout actuel
                // (prédicat indépendant de X/Y), ce recheck est redondant ; un hit nomme donc
                // précisément cette garde tardive au lieu d'être agrégé aux opt-ins désactivés.
                // The prior Z pass already accepted the sole cave slot. With the current X/Y-
                // independent predicate this recheck is redundant, so attribute it separately.
                INC_DWORD_STAT(STAT_VoxelForgeCaveBailNotOpStackRecheck);
                return EVoxelTileClass::Mixed;
            }

            switch (CaveType)
            {
            case ECaveGeneratorType::FlatPlain:
            case ECaveGeneratorType::CrystalChamber:
            {
                const FSlabGenerationParams Q = StrateManager->GetSlabParamsForChunk(CC);
                if (bFirst) { TileSlab = Q; }
                else if (FMemory::Memcmp(&Q, &TileSlab, sizeof(Q)) != 0)
                {
                    INC_DWORD_STAT(STAT_VoxelForgeCaveBailParams);
                    return EVoxelTileClass::Mixed;
                }
                break;
            }
            case ECaveGeneratorType::Maze:
            {
                const FMazeGenerationParams Q = StrateManager->GetMazeParamsForChunk(CC);
                if (bFirst) { TileMaze = Q; }
                else if (FMemory::Memcmp(&Q, &TileMaze, sizeof(Q)) != 0)
                {
                    INC_DWORD_STAT(STAT_VoxelForgeCaveBailParams);
                    return EVoxelTileClass::Mixed;
                }
                break;
            }
            case ECaveGeneratorType::VerticalShafts:
            {
                const FVerticalShaftParams Q = StrateManager->GetVerticalShaftParamsForChunk(CC);
                if (bFirst) { TileVert = Q; }
                else if (FMemory::Memcmp(&Q, &TileVert, sizeof(Q)) != 0)
                {
                    INC_DWORD_STAT(STAT_VoxelForgeCaveBailParams);
                    return EVoxelTileClass::Mixed;
                }
                break;
            }
            case ECaveGeneratorType::FloatingIslands:
            {
                const FFloatingIslandParams Q = StrateManager->GetFloatingIslandParamsForChunk(CC);
                if (bFirst) { TileFloat = Q; }
                else if (FMemory::Memcmp(&Q, &TileFloat, sizeof(Q)) != 0)
                {
                    INC_DWORD_STAT(STAT_VoxelForgeCaveBailParams);
                    return EVoxelTileClass::Mixed;
                }
                break;
            }
            case ECaveGeneratorType::Underwater:
            case ECaveGeneratorType::TunnelNetwork:
            {
                const FStrateGenerationParams Q = StrateManager->GetGenerationParams(CC);
                if (bFirst) { TileTunnel = Q; }
                else if (FMemory::Memcmp(&Q, &TileTunnel, sizeof(Q)) != 0)
                {
                    INC_DWORD_STAT(STAT_VoxelForgeCaveBailParams);
                    return EVoxelTileClass::Mixed;
                }
                break;
            }
            default:
                INC_DWORD_STAT(STAT_VoxelForgeCaveBailParams);
                return EVoxelTileClass::Mixed;   // SurfaceWorld ne peut pas être le type du slot cave
            }

            bFirst = false;
        }

        //---------------------------------------------------------------------
        // La pile — construite par la MÊME fabrique que `GetDensityAt`.
        //---------------------------------------------------------------------
        FVoxelStackParamRefs Refs;
        Refs.Slab   = &TileSlab;
        Refs.Maze   = &TileMaze;
        Refs.Vert   = &TileVert;
        Refs.Float  = &TileFloat;
        Refs.Tunnel = &TileTunnel;
        // `Refs.Surface` reste nul — la fabrique refuse alors SurfaceWorld, et c'est voulu : cette
        // fonction le prouve elle-même sur le treillis EXACT du mesher, ce qu'aucune borne de boîte
        // ne fera mieux.

        FVoxelOpContext OpCtx;
        OpCtx.ChunkCoord    = RepCC;
        OpCtx.Seed          = (uint32)Seed;
        OpCtx.LayoutVersion = TC_LayoutVersion;
        OpCtx.Step          = Step;
        OpCtx.WorldRadiusVoxels = WorldRadiusVoxels;
        OpCtx.EdgeSealThickness = EdgeSealThickness;
        OpCtx.bUseLatticeProof = true;
        OpCtx.bTightenWarpProof = Step <= 2;
        OpCtx.LatticeOriginVoxels = OriginVoxels;

        FVoxelOpStack TileStack;
        if (!VF_BuildOpStackForChunk(CaveType, Refs, Seed, OriginSpineRadius,
                                     StrateManager, TileStack, OpCtx))
        {
            // Strate dégénérée ou archétype non porté : `GetDensityAt` retomberait sur le `switch`,
            // donc la pile ne décrit pas ce que le mesher verra. Aucun verdict.
            INC_DWORD_STAT(STAT_VoxelForgeCaveBailNoStack);
            return EVoxelTileClass::Mixed;
        }
        TileStack.PrepareChunk(OpCtx);
        const FBox CaveBox(FVector((float)MinX, (float)MinY, (float)CaveMinZ),
                           FVector((float)MaxX, (float)MaxY, (float)CaveMaxZ));
        const EVoxelTileClass StackVerdict = VF_ClassifyBoxWithWarpRetry(
            TileStack, CaveBox, OpCtx, this, StrateManager, TileTunnel.BaseDensity, OutStats);
        if (StackVerdict == EVoxelTileClass::Mixed)
        {
            INC_DWORD_STAT(STAT_VoxelForgeCaveBailStackVerdict);
            return EVoxelTileClass::Mixed;
        }

        const bool bCaveOnlyTile = bAnyCave
            && !bAnyGap && !bAnySurface && !bAnyOutOfLayout
            && CaveMinZ == MinZ && CaveMaxZ == MaxZ;
        if (bCaveOnlyTile && StackVerdict == EVoxelTileClass::AllSolid)
        {
            // The stack verdict covers the complete cave-only MC lattice. Its exact structural
            // post path was supplied above, so a broad conservative passage guard must not force
            // this already-certified solid tile through GenerateMesh.
            bCanSolid = true;
            bCanAir = false;
        }
        else if (bCaveOnlyTile && StackVerdict == EVoxelTileClass::AllAir)
        {
            bCanSolid = false;
            bCanAir = true;
        }
        else if (StackVerdict == EVoxelTileClass::AllSolid)
        {
            bCanAir = false;
        }
        else
        {
            bCanSolid = false;
        }

        // Le verdict cave se plie avec gap=solide, hors-layout=air, seals surface=solide. Si les
        // deux hypothèses sont mortes ici, les catégories se contredisent : ce n'est PAS un échec
        // de borne de la pile ni une disturbance.
        // Fold the cave verdict with gap=solid, out-of-layout=air, and solid surface seals. If both
        // hypotheses die here, the categories conflict; this is not a stack-bound/disturbance bail.
        if (!bCanSolid && !bCanAir)
        {
            INC_DWORD_STAT(STAT_VoxelForgeCaveBailFoldConflict);
            return EVoxelTileClass::Mixed;
        }

        //---------------------------------------------------------------------
        // ⚠️ LES DISTURBANCES NE SONT PAS DANS LA PILE (`OPSTACK-DECOMPOSITION §10.2`) :
        // `GetDensityAt` les applique APRÈS, sur la densité déjà négatée. Un verdict qui les
        // ignorerait serait faux exactement là où elles agissent. Mêmes inégalités que la branche
        // SurfaceWorld plus haut, pour la même raison.
        //---------------------------------------------------------------------
        const FStrateDisturbanceParams D = StrateManager->GetDisturbanceParamsForChunk(RepCC);
        // A non-zero strate density is only a possibility.  Discharge it when no deterministic
        // hash-placed primitive can touch this exact tile; a hit keeps the old polarity-specific
        // conservative result (chasms kill AllSolid, bridges/ridges kill AllAir).
        if (VF_AnyChasmCanTouchLattice(
                OriginVoxels, Step, CPA, D, (uint32)Seed))
        {
            bCanSolid = false;
        }
        if (VF_AnyBridgeCanTouchLattice(
                OriginVoxels, Step, CPA, D, (uint32)Seed)
            || VF_AnyRidgeCanTouchLattice(
                OriginVoxels, Step, CPA, D, (uint32)Seed))
        {
            bCanAir = false;
        }

        if (!bCanSolid && !bCanAir)
        {
            INC_DWORD_STAT(STAT_VoxelForgeCaveBailDisturbance);
            return EVoxelTileClass::Mixed;
        }
    }

    // ── Balayage des colonnes XY sur le treillis exact du mesher (marge incluse). Une colonne
    //    tranche chaque z intérieur : air côté MC ⇔ TerrainZ ≤ z ≤ CeilSurf (D = −interne ≥ 0,
    //    cf. SurfaceDensityFromColumn ; spine/passages ne font QUE de l'air → gardés plus haut).
    //    Pré-passe clairsemée ~5×5 pour tuer vite les tuiles traversées par la surface, puis
    //    passe complète — les colonnes recalculées par la pré-passe restent chaudes dans la boîte.
    auto TestColumn = [&](int32 gx, int32 gy) -> bool   // false ⇒ les deux hypothèses sont mortes
    {
        const int32 Xi = OriginVoxels.X + gx * Step;
        const int32 Yi = OriginVoxels.Y + gy * Step;
        for (int32 s = 0; s < NumSlots; ++s)
        {
            FSurfSlot& S = Slots[s];
            if (S.InteriorZ.Num() == 0) continue;
            FSurfaceColumnBox& Box = GSurfColCache.Acquire(Xi, Yi, S.StrateKey, Seed, TC_LayoutVersion);
            const int32 CI = (Yi - Box.BaseY) * FSurfaceColumnBox::Dim + (Xi - Box.BaseX);
            if (!Box.Computed[CI])
            {
                ComputeSurfaceColumn((float)Xi, (float)Yi, S.RepChunkZ, S.Params, S.BiomeCtx,
                                     S.BiomeParams, TC_BiomeCache,
                                     Box.Cols[CI].TerrainZ, Box.Cols[CI].CeilSurf,
                                     Box.Cols[CI].OverhangAmp, Box.Cols[CI].DirX, Box.Cols[CI].DirY);
                Box.Computed[CI] = true;
            }
            const float T = Box.Cols[CI].TerrainZ;
            const float C = Box.Cols[CI].CeilSurf;
            const float M = S.OverhangMargin;
            for (const float Z : S.InteriorZ)
            {
                // F20 : juste au-dessus du sol (jusqu'à +OverhangHeight) une corniche peut ajouter de la
                // roche ⇒ ni air ni solide prouvable → tuile Mixed. (Vers le haut uniquement : l'union
                // n'enlève rien sous le terrain ; le cap n'est pas affecté.)
                if (M > 0.0f && Z > T && Z <= T + M) return false;
                if (Z >= T && Z <= C) { bCanSolid = false; }   // point côté air (surface incluse)
                else                  { bCanAir   = false; }   // sous le terrain / dans le cap
                if (!bCanSolid && !bCanAir) return false;
            }
        }
        return true;
    };

    const bool bNeedColumns = (NumSlots > 0)
        && (Slots[0].InteriorZ.Num() > 0 || (NumSlots > 1 && Slots[1].InteriorZ.Num() > 0));
    if (bNeedColumns)
    {
        const int32 PreStride = FMath::Max(1, GridDim / 4);
        for (int32 gy = 0; gy <= CPA; gy += PreStride)
            for (int32 gx = 0; gx <= CPA; gx += PreStride)
                if (!TestColumn(gx, gy)) return EVoxelTileClass::Mixed;
        for (int32 gy = 0; gy <= CPA; ++gy)
            for (int32 gx = 0; gx <= CPA; ++gx)
                if (!TestColumn(gx, gy)) return EVoxelTileClass::Mixed;
    }

    // Ici exactement UNE hypothèse doit survivre (chaque point testé en tue une ; les tuiles
    // sans point intérieur ont tué AllAir via gap/seal). Égalité = prudence → Mixed.
    if (bCanSolid == bCanAir)
    {
        return EVoxelTileClass::Mixed;
    }
    if (bAnyCave)
    {
        // Compte seulement les verdicts FINAUX qui sautent réellement une tuile. Une pile peut avoir
        // prouvé sa sous-boîte cave puis perdre l'hypothèse sur une colonne SurfaceWorld adjacente.
        // Count only final verdicts that actually skip a tile; a later surface column may still
        // invalidate the hypothesis proved for the cave sub-box.
        if (bCanSolid) { INC_DWORD_STAT(STAT_VoxelForgeTilesOpStackSolid); }
        else           { INC_DWORD_STAT(STAT_VoxelForgeTilesOpStackAir); }
    }
    return bCanSolid ? EVoxelTileClass::AllSolid : EVoxelTileClass::AllAir;
}

float UVoxelGenerator::GetSurfaceDensity(float WorldX, float WorldY, float WorldZ,
                                         const FSurfaceGenerationParams& ParamsD,
                                         const FSurfaceGenerationParams& ParamsN,
                                         float NeighborWeight) const
{
    if (ParamsD.StrateTopWorldZ - ParamsD.StrateBottomWorldZ <= 0.0f) return 1.0f;

    // Heightfield: dominant biome, OUTPUT-BLENDED toward the nearest neighbour in the
    // border band. Blending heights (not params) keeps borders seamless across any param
    // difference. ParamsD == ParamsN, weight 0 ⇒ single eval (bit-identical, no biomes).
    float TerrainZ = ComputeSurfaceTerrainZ(WorldX, WorldY, ParamsD);
    if (NeighborWeight > 0.0f)
    {
        TerrainZ = FMath::Lerp(TerrainZ, ComputeSurfaceTerrainZ(WorldX, WorldY, ParamsN), NeighborWeight);
    }
    const float CeilSurf = ComputeSurfaceCeiling(WorldX, WorldY, ParamsD);

    return SurfaceDensityFromColumn(WorldX, WorldY, WorldZ, TerrainZ, CeilSurf,
                                    /*OverhangAmp*/0.0f, /*DirX*/0.0f, /*DirY*/0.0f, ParamsD);
}

//=============================================================================
// CLIMATE & BIOME FIELDS
//=============================================================================
// Pure functions of world XY (+ seed). The relief field is shared with SurfaceWorld
// terrain so the biome map and the terrain it modulates stay in agreement. The biome
// field is a warped Voronoi whose cells are assigned a biome by climate — coherent
// geography (mountains cluster in high relief), window-invariant by construction.

float UVoxelGenerator::SampleRelief(float WorldX, float WorldY, float Frequency, float Contrast) const
{
    const uint32 SeedU = (uint32)Seed;
    // Same offsets/octaves as the original SurfaceWorld relief so existing worlds are
    // unchanged (this is the function that code path now calls).
    float R = FractalNoise3D(FVector3f(
        WorldX * Frequency + VoxelHash::SeedOffset(SeedU, 7.3f),
        WorldY * Frequency + VoxelHash::SeedOffset(SeedU, 2.1f),
        VoxelHash::SeedOffset(SeedU, 0.5f)), 2) * 0.5f + 0.5f;                 // [0,1]
    R = FMath::Clamp((R - 0.5f) * Contrast + 0.5f, 0.0f, 1.0f);
    return SmoothStep01(R);
}

float UVoxelGenerator::SampleMoisture(float WorldX, float WorldY, float Frequency) const
{
    const uint32 SeedU = (uint32)Seed;
    const float N = FractalNoise3D(FVector3f(
        WorldX * Frequency + VoxelHash::SeedOffset(SeedU, 4.7f),
        WorldY * Frequency + VoxelHash::SeedOffset(SeedU, 8.9f),
        VoxelHash::SeedOffset(SeedU, 1.3f)), 2) * 0.5f + 0.5f;                 // [0,1]
    return FMath::Clamp(N, 0.0f, 1.0f);
}

bool UVoxelGenerator::EvaluateTerrainConditions(const TArray<FTerrainCondition>& Conditions,
                                                float WorldX, float WorldY, const FBiomeContext& BiomeCtx) const
{
    if (Conditions.Num() == 0) return true;   // zero-cost default (the common case)

    // Freq/contrast come from the strate's biome map (defaults when the strate has no biomes) — the same
    // params SampleRelief/SampleMoisture are given everywhere else, so a condition agrees with the terrain.
    const FBiomeMapParams MP = BiomeCtx.IsValid() ? BiomeCtx.Map : FBiomeMapParams();

    // Lazily sample each field only if a condition references it (BiomeBorder's Voronoi is the pricey one).
    float Relief = 0.0f, Moisture = 0.0f, Border = 0.0f;
    bool bHaveRelief = false, bHaveMoisture = false, bHaveBorder = false;

    for (const FTerrainCondition& C : Conditions)
    {
        float V = 0.0f;
        switch (C.Type)
        {
        case ETerrainConditionType::Relief:
            if (!bHaveRelief)   { Relief   = SampleRelief(WorldX, WorldY, MP.ReliefFrequency, MP.ReliefContrast); bHaveRelief = true; }
            V = Relief;   break;
        case ETerrainConditionType::Moisture:
            if (!bHaveMoisture) { Moisture = SampleMoisture(WorldX, WorldY, MP.MoistureFrequency); bHaveMoisture = true; }
            V = Moisture; break;
        case ETerrainConditionType::BiomeBorder:
            if (!bHaveBorder)   { Border = SampleBiomeAt(WorldX, WorldY, BiomeCtx).NeighborWeight; bHaveBorder = true; }
            V = Border;   break;
        default: break;
        }

        bool bPass = (V >= C.Min && V <= C.Max);
        if (C.bInvert) bPass = !bPass;
        if (!bPass) return false;   // AND semantics: any failing condition rejects the candidate
    }
    return true;
}

int32 UVoxelGenerator::ClassifyBiomeAtSite(float SiteX, float SiteY,
                                           const FBiomeContext& Ctx, uint32 SiteHash) const
{
    const float R  = SampleRelief(SiteX, SiteY, Ctx.Map.ReliefFrequency, Ctx.Map.ReliefContrast);
    const float Mo = SampleMoisture(SiteX, SiteY, Ctx.Map.MoistureFrequency);

    int32 Best = 0;
    float BestScore = FLT_MAX;
    for (int32 i = 0; i < Ctx.Biomes.Num(); ++i)
    {
        const FBiomeResolved& B = Ctx.Biomes[i];
        // Distance in climate space to the biome's box (0 when inside it).
        const float dr = (R  < B.ReliefMin)   ? (B.ReliefMin - R)
                       : (R  > B.ReliefMax)   ? (R - B.ReliefMax)   : 0.0f;
        const float dm = (Mo < B.MoistureMin) ? (B.MoistureMin - Mo)
                       : (Mo > B.MoistureMax) ? (Mo - B.MoistureMax) : 0.0f;
        float Score = dr * dr + dm * dm;
        // Tiny deterministic jitter so overlapping boxes don't all collapse to biome 0.
        Score += VoxelHash::ToFloat01(VoxelHash::Mix(SiteHash ^ (0x9e3779b9u * (uint32)(i + 1)))) * 1.0e-4f;
        if (Score < BestScore) { BestScore = Score; Best = i; }
    }
    return Best;
}

FBiomeSample UVoxelGenerator::SampleBiomeAt(float WorldX, float WorldY, const FBiomeContext& Ctx) const
{
    FBiomeSample Out;
    if (Ctx.Biomes.Num() == 0) return Out;                    // biomes disabled
    if (Ctx.Biomes.Num() == 1) { Out.DominantIndex = 0; return Out; }

    const FBiomeMapParams& MP = Ctx.Map;
    const float Cell = FMath::Max(MP.CellSize, 1.0f);
    const uint32 S = (uint32)Seed ^ 0x42494f4du;             // 'BIOM'

    // Domain-warp the query so cell borders wind organically (not a hex grid).
    float QX = WorldX, QY = WorldY;
    if (MP.WarpStrength > 0.0f)
    {
        const uint32 SeedU = (uint32)Seed;
        const float WF = MP.WarpFrequency;
        const float wx = VoxelNoise::Perlin3D(FVector3f(WorldX * WF + VoxelHash::SeedOffset(SeedU, 0.27f), WorldY * WF + 3.1f, VoxelHash::SeedOffset(SeedU, 1.1f)));
        const float wy = VoxelNoise::Perlin3D(FVector3f(WorldX * WF + 7.7f, WorldY * WF + VoxelHash::SeedOffset(SeedU, 0.61f), VoxelHash::SeedOffset(SeedU, 2.3f)));
        QX += wx * VOXEL_NOISE_SCALE * MP.WarpStrength;
        QY += wy * VOXEL_NOISE_SCALE * MP.WarpStrength;
    }

    const int32 CX = FMath::FloorToInt(QX / Cell);
    const int32 CY = FMath::FloorToInt(QY / Cell);

    // Worley F1/F2 over the 3x3 neighbourhood of the (warped) cell. With jitter confined
    // to [0,1) of a cell, the nearest site is always within this neighbourhood.
    float BestD2 = FLT_MAX, SecondD2 = FLT_MAX;
    int32 BestBiome = 0, SecondBiome = 0;
    for (int32 dy = -1; dy <= 1; ++dy)
    for (int32 dx = -1; dx <= 1; ++dx)
    {
        const int32 nx = CX + dx, ny = CY + dy;
        const uint32 h = VoxelHash::Cell(nx, ny, S);
        const float jx = VoxelHash::ToFloat01(h);
        const float jy = VoxelHash::ToFloat01(VoxelHash::Mix(h ^ 0x68bc21ebu));
        const float sx = (nx + jx) * Cell;
        const float sy = (ny + jy) * Cell;
        const float ddx = sx - QX, ddy = sy - QY;
        const float d2 = ddx * ddx + ddy * ddy;

        if (d2 < BestD2)
        {
            SecondD2 = BestD2; SecondBiome = BestBiome;
            BestD2 = d2; BestBiome = ClassifyBiomeAtSite(sx, sy, Ctx, h);
        }
        else if (d2 < SecondD2)
        {
            SecondD2 = d2; SecondBiome = ClassifyBiomeAtSite(sx, sy, Ctx, h);
        }
    }

    Out.DominantIndex = BestBiome;
    Out.NeighborIndex = SecondBiome;

    // Blend weight: 0.5 at the shared border (d1≈d2), fading to 0 a BorderBlend-wide
    // band inside the dominant cell. Only blend when the neighbour is a DIFFERENT biome.
    if (MP.BorderBlend > 0.0f && SecondD2 < FLT_MAX && BestBiome != SecondBiome)
    {
        const float d1 = FMath::Sqrt(BestD2);
        const float d2 = FMath::Sqrt(SecondD2);
        const float t = FMath::Clamp((d2 - d1) / FMath::Max(MP.BorderBlend, 1.0f), 0.0f, 1.0f);
        Out.NeighborWeight = 0.5f * (1.0f - SmoothStep01(t));
    }
    return Out;
}

void UVoxelGenerator::RebuildBiomeGrid(int32 ChunkX, int32 ChunkY, int32 ChunkZ,
                                       const FBiomeContext& Ctx, FChunkBiomeCache& Cache) const
{
    Cache.ChunkZ  = ChunkZ;
    Cache.Seed    = Seed;
    Cache.bActive = Ctx.IsValid();
    Cache.Ctx     = Ctx;
    Cache.CellBiome.Reset();

    if (!Cache.bActive)
    {
        // Mark the whole chunk footprint valid so non-biome chunks don't rebuild per voxel.
        Cache.ValidMinX = (float)ChunkX * CHUNK_SIZE - (float)CHUNK_SIZE;
        Cache.ValidMaxX = (float)(ChunkX + 1) * CHUNK_SIZE + (float)CHUNK_SIZE;
        Cache.ValidMinY = (float)ChunkY * CHUNK_SIZE - (float)CHUNK_SIZE;
        Cache.ValidMaxY = (float)(ChunkY + 1) * CHUNK_SIZE + (float)CHUNK_SIZE;
        return;
    }

    const FBiomeMapParams& MP = Ctx.Map;
    const float Cell = FMath::Max(MP.CellSize, 1.0f);
    const uint32 S = (uint32)Seed ^ 0x42494f4du;             // 'BIOM' (must match SampleBiomeAt)

    // Validity halo: one chunk beyond the footprint, so the +X/+Y boundary corners and
    // ±1 gradient-normal samples stay inside the valid box (no rebuild thrash, §8.10).
    const float Halo = (float)CHUNK_SIZE;
    Cache.ValidMinX = (float)ChunkX * CHUNK_SIZE - Halo;
    Cache.ValidMaxX = (float)(ChunkX + 1) * CHUNK_SIZE + Halo;
    Cache.ValidMinY = (float)ChunkY * CHUNK_SIZE - Halo;
    Cache.ValidMaxY = (float)(ChunkY + 1) * CHUNK_SIZE + Halo;

    // Cell-grid coverage: the valid box expanded by the max warp displacement + one cell,
    // so the 3x3 search around any in-box query's warped point is fully present.
    const float CellMargin = MP.WarpStrength * VOXEL_NOISE_SCALE + Cell + 1.0f;
    Cache.BaseCellX = FMath::FloorToInt((Cache.ValidMinX - CellMargin) / Cell);
    Cache.BaseCellY = FMath::FloorToInt((Cache.ValidMinY - CellMargin) / Cell);
    const int32 MaxCellX = FMath::FloorToInt((Cache.ValidMaxX + CellMargin) / Cell);
    const int32 MaxCellY = FMath::FloorToInt((Cache.ValidMaxY + CellMargin) / Cell);
    Cache.CellsX = MaxCellX - Cache.BaseCellX + 1;
    Cache.CellsY = MaxCellY - Cache.BaseCellY + 1;
    Cache.CellBiome.SetNumUninitialized(Cache.CellsX * Cache.CellsY);

    for (int32 cy = 0; cy < Cache.CellsY; ++cy)
    for (int32 cx = 0; cx < Cache.CellsX; ++cx)
    {
        const int32 nx = Cache.BaseCellX + cx;
        const int32 ny = Cache.BaseCellY + cy;
        const uint32 h = VoxelHash::Cell(nx, ny, S);
        const float jx = VoxelHash::ToFloat01(h);
        const float jy = VoxelHash::ToFloat01(VoxelHash::Mix(h ^ 0x68bc21ebu));
        const float sx = (nx + jx) * Cell;
        const float sy = (ny + jy) * Cell;
        Cache.CellBiome[cy * Cache.CellsX + cx] = ClassifyBiomeAtSite(sx, sy, Ctx, h);
    }
}

FBiomeSample UVoxelGenerator::ResolveBiomeSampleAt(float WorldX, float WorldY, int32 ChunkZ,
                                                   const FBiomeContext& Ctx, FChunkBiomeCache& Cache) const
{
    FBiomeSample Out;

    // Box-validated rebuild (perf-only; result is the pure function of XY either way).
    if (!Cache.Contains(WorldX, WorldY, ChunkZ, Seed))
    {
        RebuildBiomeGrid(FMath::FloorToInt(WorldX / CHUNK_SIZE),
                         FMath::FloorToInt(WorldY / CHUNK_SIZE), ChunkZ, Ctx, Cache);
    }

    if (!Cache.bActive) return Out;
    if (Cache.Ctx.Biomes.Num() == 1) { Out.DominantIndex = 0; return Out; }

    const FBiomeMapParams& MP = Cache.Ctx.Map;
    const float Cell = FMath::Max(MP.CellSize, 1.0f);
    const uint32 S = (uint32)Seed ^ 0x42494f4du;

    // Same warp + 3x3 Worley as SampleBiomeAt → identical assignment (matches the preview).
    float QX = WorldX, QY = WorldY;
    if (MP.WarpStrength > 0.0f)
    {
        const uint32 SeedU = (uint32)Seed;
        const float WF = MP.WarpFrequency;
        const float wx = VoxelNoise::Perlin3D(FVector3f(WorldX * WF + VoxelHash::SeedOffset(SeedU, 0.27f), WorldY * WF + 3.1f, VoxelHash::SeedOffset(SeedU, 1.1f)));
        const float wy = VoxelNoise::Perlin3D(FVector3f(WorldX * WF + 7.7f, WorldY * WF + VoxelHash::SeedOffset(SeedU, 0.61f), VoxelHash::SeedOffset(SeedU, 2.3f)));
        QX += wx * VOXEL_NOISE_SCALE * MP.WarpStrength;
        QY += wy * VOXEL_NOISE_SCALE * MP.WarpStrength;
    }

    const int32 CX = FMath::FloorToInt(QX / Cell);
    const int32 CY = FMath::FloorToInt(QY / Cell);

    float BestD2 = FLT_MAX, SecondD2 = FLT_MAX;
    int32 BestBiome = 0, SecondBiome = 0;
    for (int32 dy = -1; dy <= 1; ++dy)
    for (int32 dx = -1; dx <= 1; ++dx)
    {
        const int32 nx = CX + dx, ny = CY + dy;
        const uint32 h = VoxelHash::Cell(nx, ny, S);
        const float jx = VoxelHash::ToFloat01(h);
        const float jy = VoxelHash::ToFloat01(VoxelHash::Mix(h ^ 0x68bc21ebu));
        const float sx = (nx + jx) * Cell;
        const float sy = (ny + jy) * Cell;
        const float ddx = sx - QX, ddy = sy - QY;
        const float d2 = ddx * ddx + ddy * ddy;

        // Cached biome index (fallback to classify on the rare margin miss → still correct).
        const int32 gx = nx - Cache.BaseCellX;
        const int32 gy = ny - Cache.BaseCellY;
        const int32 BiomeIdx = (gx >= 0 && gx < Cache.CellsX && gy >= 0 && gy < Cache.CellsY)
            ? Cache.CellBiome[gy * Cache.CellsX + gx]
            : ClassifyBiomeAtSite(sx, sy, Cache.Ctx, h);

        if (d2 < BestD2)        { SecondD2 = BestD2; SecondBiome = BestBiome; BestD2 = d2; BestBiome = BiomeIdx; }
        else if (d2 < SecondD2) { SecondD2 = d2; SecondBiome = BiomeIdx; }
    }

    Out.DominantIndex = BestBiome;
    Out.NeighborIndex = SecondBiome;
    if (MP.BorderBlend > 0.0f && SecondD2 < FLT_MAX && BestBiome != SecondBiome)
    {
        const float d1 = FMath::Sqrt(BestD2);
        const float d2 = FMath::Sqrt(SecondD2);
        const float t = FMath::Clamp((d2 - d1) / FMath::Max(MP.BorderBlend, 1.0f), 0.0f, 1.0f);
        Out.NeighborWeight = 0.5f * (1.0f - SmoothStep01(t));
    }
    return Out;
}

const UVoxelBiomeDefinition* UVoxelGenerator::GetDominantBiomeAt(float WorldX, float WorldY, int32 ChunkZ) const
{
    if (!StrateManager) return nullptr;

    const FIntVector Coord(FMath::FloorToInt(WorldX / CHUNK_SIZE),
                           FMath::FloorToInt(WorldY / CHUNK_SIZE), ChunkZ);

    const FBiomeContext Ctx = StrateManager->GetBiomeContextForChunk(Coord);
    if (!Ctx.IsValid()) return nullptr;

    const FBiomeSample S = SampleBiomeAt(WorldX, WorldY, Ctx);
    if (!Ctx.Biomes.IsValidIndex(S.DominantIndex)) return nullptr;

    // Map the context position back to the strate's Biomes[] asset.
    const int32 StrateBiomeIdx = Ctx.Biomes[S.DominantIndex].Index;
    const UVoxelStrateDefinition* Def = StrateManager->GetStrateForChunk(Coord);
    return (Def && Def->Biomes.IsValidIndex(StrateBiomeIdx)) ? Def->Biomes[StrateBiomeIdx] : nullptr;
}

void UVoxelGenerator::QueryBiomeAt(float WorldX, float WorldY, int32 ChunkZ, FVoxelBiomeQuery& Out) const
{
    Out = FVoxelBiomeQuery();
    if (!StrateManager) return;

    const FIntVector Coord(FMath::FloorToInt(WorldX / CHUNK_SIZE),
                           FMath::FloorToInt(WorldY / CHUNK_SIZE), ChunkZ);
    const FBiomeContext Ctx = StrateManager->GetBiomeContextForChunk(Coord);

    // Climate fields are always meaningful (use the strate's map freqs, or defaults when no biomes).
    const FBiomeMapParams MP = Ctx.IsValid() ? Ctx.Map : FBiomeMapParams();
    Out.Relief   = SampleRelief(WorldX, WorldY, MP.ReliefFrequency, MP.ReliefContrast);
    Out.Moisture = SampleMoisture(WorldX, WorldY, MP.MoistureFrequency);

    if (!Ctx.IsValid()) return;   // bHasBiomes stays false → BP knows this strate has no biome field
    Out.bHasBiomes = true;

    const FBiomeSample S = SampleBiomeAt(WorldX, WorldY, Ctx);
    Out.NeighborWeight       = S.NeighborWeight;
    Out.DominantContextIndex = S.DominantIndex;

    const UVoxelStrateDefinition* Def = StrateManager->GetStrateForChunk(Coord);

    if (Ctx.Biomes.IsValidIndex(S.DominantIndex))
    {
        const int32 DomStrateIdx = Ctx.Biomes[S.DominantIndex].Index;
        if (Def && Def->Biomes.IsValidIndex(DomStrateIdx))
        {
            UVoxelBiomeDefinition* Bio = Def->Biomes[DomStrateIdx];
            Out.DominantBiome = Bio;
            if (Bio)
            {
                Out.DebugColor   = Bio->DebugColor;
                Out.DominantName = Bio->BiomeName.IsEmpty() ? FText::FromName(Bio->GetFName()) : Bio->BiomeName;
                // Effective list = the biome's decorations, or the strate's when the biome has none
                // (this is exactly what the per-column scatter falls back to). 0 ⇒ empty band.
                Out.DominantDecorationCount =
                    (Bio->Decorations.Num() > 0) ? Bio->Decorations.Num() : Def->Decorations.Num();
            }
        }
    }
    if (Ctx.Biomes.IsValidIndex(S.NeighborIndex))
    {
        const int32 NbStrateIdx = Ctx.Biomes[S.NeighborIndex].Index;
        if (Def && Def->Biomes.IsValidIndex(NbStrateIdx)) { Out.NeighborBiome = Def->Biomes[NbStrateIdx]; }
    }
}

void UVoxelGenerator::GetBiomeMaterialAt(float WorldX, float WorldY, float WorldZ,
    int32& OutDominantPalette, int32& OutNeighborPalette, float& OutBlendWeight) const
{
    OutDominantPalette = 0;
    OutNeighborPalette = 0;
    OutBlendWeight     = 0.0f;
    if (!StrateManager) return;

    const FIntVector ChunkCoord(
        FMath::FloorToInt(WorldX / CHUNK_SIZE),
        FMath::FloorToInt(WorldY / CHUNK_SIZE),
        FMath::FloorToInt(WorldZ / CHUNK_SIZE));

    // Per-chunk biome context cache, mirroring GetDensityAt: mesher vertices cluster by chunk,
    // so the (cheap) flatten + (noise-heavy, box-validated) grid stay warm across a tile.
    thread_local FIntVector       BM_Chunk(INT32_MAX, INT32_MAX, INT32_MAX);
    thread_local FBiomeContext    BM_Ctx;
    thread_local FChunkBiomeCache BM_Cache;
    thread_local uint32           BM_Version = 0xFFFFFFFFu;   // AUDIT C2

    const uint32 BM_LayoutVersion = StrateManager->GetLayoutVersion();
    if (ChunkCoord != BM_Chunk || BM_LayoutVersion != BM_Version)
    {
        if (BM_LayoutVersion != BM_Version) { BM_Cache.Invalidate(); }
        BM_Version = BM_LayoutVersion;
        BM_Chunk = ChunkCoord;
        BM_Ctx   = StrateManager->GetBiomeContextForChunk(ChunkCoord);
    }
    if (!BM_Ctx.IsValid()) return;   // strate has no biomes → default palette

    const FBiomeSample S = ResolveBiomeSampleAt(WorldX, WorldY, ChunkCoord.Z, BM_Ctx, BM_Cache);
    if (BM_Ctx.Biomes.IsValidIndex(S.DominantIndex))
    {
        OutDominantPalette = BM_Ctx.Biomes[S.DominantIndex].MaterialPaletteIndex;
        // Neighbour defaults to the dominant so an interior vertex blends to itself (no seam).
        OutNeighborPalette = BM_Ctx.Biomes.IsValidIndex(S.NeighborIndex)
            ? BM_Ctx.Biomes[S.NeighborIndex].MaterialPaletteIndex
            : OutDominantPalette;
        OutBlendWeight = S.NeighborWeight;   // 0 inside a cell → ~0.5 at the border
    }
}

//=============================================================================
// VERTICAL-SHAFT GENERATOR  (ECaveGeneratorType::VerticalShafts)
//=============================================================================
// Solid rock carved by hash-placed full-height vertical shafts (cylinders), a deterministic
// drainage tree of horizontal connector tunnels rooted at the origin spine, additive random
// connector loops, and partial ledges inside them. Emphasises climbing and falling.

float UVoxelGenerator::GetVerticalShaftDensity(float WorldX, float WorldY, float WorldZ,
                                               const FVerticalShaftParams& Params) const
{
    const float StrateHeight = Params.StrateTopWorldZ - Params.StrateBottomWorldZ;
    if (StrateHeight <= 0.0f) return 1.0f;

    const float Spacing = FMath::Max(Params.ShaftSpacing, 1.0f);
    const FVector Pos(WorldX, WorldY, WorldZ);
    const uint32 S = (uint32)Seed ^ 0x53686674u;  // 'Shft'

    float Density = Params.BaseDensity;  // start solid

    const int32 CX = FMath::FloorToInt(WorldX / Spacing);
    const int32 CY = FMath::FloorToInt(WorldY / Spacing);

    // The shaft topology is pure, but it is deliberately split into two windows:
    //   * COLLECT a deterministic halo once during this thread-local cache rebuild. This is the
    //     source data needed to resolve every 5×5 parent window and the fixed ±3 fallback window.
    //   * EVALUATE only the INNER 3×3 shaft cylinders. Tree links are collected from a wider
    //     rebuild-only child window and culled to the current cell before the voxel loop sees
    //     them. That matters: a capsule from a child to a parent two or three cells away must be
    //     present in the cells along its segment, not only in the cache centred on the child.
    struct FLocalShaft { float X, Y, R; int32 CellX, CellY; bool bOriginSpine; };
    struct FLocalConn
    {
        FVector A, B;
        float Radius = 0.0f;
        float FloorZ = 0.0f;
        bool bWalkableFloor = false;
    };
    thread_local TArray<FLocalShaft, TInlineAllocator<10>> Shafts;
    thread_local TArray<FLocalConn, TInlineAllocator<32>> Conns;
    thread_local TArray<FLocalShaft, TInlineAllocator<81>> TreeEmitShafts;
    thread_local int32  VS_CX = INT32_MAX, VS_CY = INT32_MAX;
    thread_local uint32 VS_Seed = 0xFFFFFFFFu;
    thread_local float  VS_Spacing = -1.0f, VS_Dens = -1.0f, VS_MinR = -1.0f, VS_MaxR = -1.0f,
                        VS_Cross = -1.0f, VS_ConnR = -1.0f, VS_Rough = -1.0f,
                        VS_SpineR = -1.0f, VS_BotZ = FLT_MAX, VS_TopZ = FLT_MAX,
                        VS_Seal = -1.0f;

    if (CX != VS_CX || CY != VS_CY || S != VS_Seed || Spacing != VS_Spacing ||
        Params.ShaftDensity != VS_Dens || Params.ShaftMinRadius != VS_MinR || Params.ShaftMaxRadius != VS_MaxR ||
        Params.CrossConnectChance != VS_Cross || Params.ConnectorRadius != VS_ConnR ||
        Params.SurfaceRoughness != VS_Rough || OriginSpineRadius != VS_SpineR ||
        Params.StrateBottomWorldZ != VS_BotZ || Params.StrateTopWorldZ != VS_TopZ ||
        Params.BoundarySealThickness != VS_Seal)
    {
        VS_CX = CX;  VS_CY = CY;  VS_Seed = S;  VS_Spacing = Spacing;
        VS_Dens = Params.ShaftDensity;  VS_MinR = Params.ShaftMinRadius;  VS_MaxR = Params.ShaftMaxRadius;
        VS_Cross = Params.CrossConnectChance;  VS_ConnR = Params.ConnectorRadius;
        VS_Rough = Params.SurfaceRoughness;    VS_SpineR = OriginSpineRadius;
        VS_BotZ = Params.StrateBottomWorldZ;  VS_TopZ = Params.StrateTopWorldZ;
        VS_Seal = Params.BoundarySealThickness;
        Shafts.Reset();
        Conns.Reset();
        TreeEmitShafts.Reset();

        constexpr int32 EmitRadius = 1;      // 3×3: shaft cylinders and random links
        constexpr int32 CandidateRadius = 2; // 5×5 parent window around an emitting shaft
        constexpr int32 FallbackRadius = 3;  // fixed fallback window around an emitting shaft
        constexpr uint32 TreeSalt = 0x7A11u;

        const float BottomZ = Params.StrateBottomWorldZ + Params.BoundarySealThickness;
        const float TopZ    = Params.StrateTopWorldZ    - Params.BoundarySealThickness;
        const float RoughnessReach = FMath::Max(Params.SurfaceRoughness, 0.0f)
                                    * VOXEL_NOISE_SCALE * 1.5f;
        // A tree link must remain open at the centreline after the SDF roughness pass. The +1
        // margin makes the radius strictly greater than the proven roughness supremum.
        const float TreeConnectorRadius = FMath::Max(Params.ConnectorRadius, RoughnessReach + 1.0f);
        // A tree capsule can affect a cell even when neither endpoint is in that cell. Keep a
        // rebuild-only child halo for that geometric reach, then add the parent-search halo. The
        // default is a 4-cell tree-emission radius + 3-cell collection pad = 7-cell radius
        // (15×15 rolls; the emitted tree-child box itself is 9×9).
        const int32 ConnectorCellPad = FMath::Max(
            1, FMath::CeilToInt(TreeConnectorRadius / Spacing));
        const int32 TreeEmitRadius = FallbackRadius + ConnectorCellPad;
        const int32 CollectRadius = TreeEmitRadius + FallbackRadius;

        const int32 CollectSide = CollectRadius * 2 + 1;
        TArray<FLocalShaft, TInlineAllocator<225>> ShaftGrid;
        TArray<uint8, TInlineAllocator<225>> ShaftPresent;
        ShaftGrid.SetNum(CollectSide * CollectSide);
        ShaftPresent.Init(0, CollectSide * CollectSide);
        // Collect the wider deterministic neighbourhood once. Only the inner cells are retained
        // in `Shafts` for the per-voxel cylinder/ledge work; `TreeEmitShafts` is still rebuilt
        // before evaluation and its connectors are spatially culled below.
        for (int32 dy = -CollectRadius; dy <= CollectRadius; dy++)
        for (int32 dx = -CollectRadius; dx <= CollectRadius; dx++)
        {
            const int32 nx = CX + dx, ny = CY + dy;
            const uint32 Hh = VoxelHash::Cell(nx, ny, S);
            if (VoxelHash::ToFloat01(Hh) > Params.ShaftDensity) continue;

            const float JX = VoxelHash::ToFloat01(VoxelHash::Mix(Hh ^ 0x12345678u));
            const float JY = VoxelHash::ToFloat01(VoxelHash::Mix(Hh ^ 0x9ABCDEF0u));
            FLocalShaft Sh;
            Sh.X = (nx + 0.15f + JX * 0.7f) * Spacing;
            Sh.Y = (ny + 0.15f + JY * 0.7f) * Spacing;
            Sh.R = FMath::Lerp(Params.ShaftMinRadius, Params.ShaftMaxRadius,
                               VoxelHash::ToFloat01(VoxelHash::Mix(Hh ^ 0xBEEFu)));
            Sh.CellX = nx;
            Sh.CellY = ny;
            Sh.bOriginSpine = false;
            const int32 GridIndex = (dy + CollectRadius) * CollectSide + (dx + CollectRadius);
            ShaftGrid[GridIndex] = Sh;
            ShaftPresent[GridIndex] = 1;
            if (FMath::Abs(dx) <= EmitRadius && FMath::Abs(dy) <= EmitRadius)
            {
                Shafts.Add(Sh);
            }
            if (FMath::Abs(dx) <= TreeEmitRadius && FMath::Abs(dy) <= TreeEmitRadius)
            {
                TreeEmitShafts.Add(Sh);
            }
        }

        auto FindCollectedShaft = [&](int32 CellX, int32 CellY) -> const FLocalShaft*
        {
            const int32 dx = CellX - CX;
            const int32 dy = CellY - CY;
            if (FMath::Abs(dx) > CollectRadius || FMath::Abs(dy) > CollectRadius)
            {
                return nullptr;
            }
            const int32 GridIndex = (dy + CollectRadius) * CollectSide + (dx + CollectRadius);
            return ShaftPresent[GridIndex] ? &ShaftGrid[GridIndex] : nullptr;
        };

        // The structural (0,0) spine is not re-carved here. It participates only as a
        // connector endpoint. The local 3x3 set is origin-adjacent when its centre cell is in
        // the central 3x3; the structural post remains the sole owner of the vertical column.
        const bool bOriginNearby = FMath::Abs(CX) <= 1 && FMath::Abs(CY) <= 1;
        if (bOriginNearby && OriginSpineRadius > 0.0f)
        {
            Shafts.Add({0.0f, 0.0f, OriginSpineRadius, 0, 0, true});
        }

        const float CellMinX = static_cast<float>(CX) * Spacing;
        const float CellMaxX = static_cast<float>(CX + 1) * Spacing;
        const float CellMinY = static_cast<float>(CY) * Spacing;
        const float CellMaxY = static_cast<float>(CY + 1) * Spacing;
        auto ConnectorMayReachCell = [&](const FLocalConn& Conn)
        {
            const float MinX = FMath::Min(Conn.A.X, Conn.B.X) - Conn.Radius;
            const float MaxX = FMath::Max(Conn.A.X, Conn.B.X) + Conn.Radius;
            const float MinY = FMath::Min(Conn.A.Y, Conn.B.Y) - Conn.Radius;
            const float MaxY = FMath::Max(Conn.A.Y, Conn.B.Y) + Conn.Radius;
            const float GapX = FMath::Max3(CellMinX - MaxX, MinX - CellMaxX, 0.0f);
            const float GapY = FMath::Max3(CellMinY - MaxY, MinY - CellMaxY, 0.0f);
            return GapX == 0.0f && GapY == 0.0f;
        };

        auto EmitTreeConnector = [&](const FLocalShaft& Child, const FLocalShaft* Parent)
        {
            const float Zc = VoxelPassageGeometry::VerticalShaftConnectorCenterZ(
                Params.StrateBottomWorldZ,
                BottomZ,
                TopZ,
                Params.LedgeSpacing,
                Params.LedgeDepth,
                TreeConnectorRadius);
            const float FloorZ = VoxelPassageGeometry::VerticalShaftConnectorFloorZ(
                Params.StrateBottomWorldZ,
                BottomZ,
                TopZ,
                Params.LedgeSpacing,
                Params.LedgeDepth,
                TreeConnectorRadius);
            const FVector ParentPoint = Parent != nullptr
                ? FVector(Parent->X, Parent->Y, Zc)
                : FVector(0.0f, 0.0f, Zc);
            const FLocalConn Conn{
                FVector(Child.X, Child.Y, Zc), ParentPoint, TreeConnectorRadius,
                FloorZ, true };
            if (ConnectorMayReachCell(Conn))
            {
                Conns.Add(Conn);
            }
        };

        // Structural drainage tree: every real shaft in the rebuild-only child window first
        // selects one strictly more-central shaft from its complete 5×5 window. If that window
        // has a local minimum, select the nearest shaft in the collected deterministic halo whose
        // origin distance is lower; that shaft is already on a strictly descending path by the
        // same rule. A shaft whose window reaches the origin uses the spine directly, and the
        // final direct-spine fallback covers an unusually empty finite halo. Thus every emitted
        // parent edge either strictly decreases origin distance or terminates at the spine: no
        // cycles and no orphans. Spatial culling makes the same edge visible throughout its
        // capsule, independent of which cache cell is evaluating it.
        // The explicit cell-coordinate tie-break makes this independent of array iteration order;
        // the synthetic spine is not a candidate shaft.
        for (const FLocalShaft& Child : TreeEmitShafts)
        {
            if (Child.bOriginSpine) continue;

            const float ChildOriginSq = FMath::Square(Child.X) + FMath::Square(Child.Y);
            const FLocalShaft* Parent = nullptr;
            float BestDistanceSq = FLT_MAX;
            for (int32 dy = -CandidateRadius; dy <= CandidateRadius; ++dy)
            for (int32 dx = -CandidateRadius; dx <= CandidateRadius; ++dx)
            {
                const FLocalShaft* Candidate = FindCollectedShaft(
                    Child.CellX + dx, Child.CellY + dy);
                if (Candidate == nullptr
                    || (Candidate->CellX == Child.CellX && Candidate->CellY == Child.CellY))
                {
                    continue;
                }

                const float CandidateOriginSq = FMath::Square(Candidate->X)
                                               + FMath::Square(Candidate->Y);
                if (!(CandidateOriginSq < ChildOriginSq))
                {
                    continue;
                }

                const float DistanceSq = FMath::Square(Child.X - Candidate->X)
                                       + FMath::Square(Child.Y - Candidate->Y);
                const bool bLowerCell = Parent == nullptr
                    || Candidate->CellY < Parent->CellY
                    || (Candidate->CellY == Parent->CellY && Candidate->CellX < Parent->CellX);
                if (DistanceSq < BestDistanceSq
                    || (DistanceSq == BestDistanceSq && bLowerCell))
                {
                    BestDistanceSq = DistanceSq;
                    Parent = Candidate;
                }
            }

            if (Parent == nullptr
                && !(FMath::Abs(Child.CellX) <= CandidateRadius
                    && FMath::Abs(Child.CellY) <= CandidateRadius))
            {
                // This is the specified neighbour-shaft fallback. Restricting it to a lower
                // origin distance makes the parent graph well-founded even though the fallback
                // candidate need not be in the child's 5×5 window.
                for (int32 dy = -FallbackRadius; dy <= FallbackRadius; ++dy)
                for (int32 dx = -FallbackRadius; dx <= FallbackRadius; ++dx)
                {
                    const FLocalShaft* Candidate = FindCollectedShaft(
                        Child.CellX + dx, Child.CellY + dy);
                    if (Candidate == nullptr
                        || (Candidate->CellX == Child.CellX && Candidate->CellY == Child.CellY))
                    {
                        continue;
                    }

                    const float CandidateOriginSq = FMath::Square(Candidate->X)
                                                   + FMath::Square(Candidate->Y);
                    if (!(CandidateOriginSq < ChildOriginSq))
                    {
                        continue;
                    }

                    const float DistanceSq = FMath::Square(Child.X - Candidate->X)
                                           + FMath::Square(Child.Y - Candidate->Y);
                    const bool bLowerCell = Parent == nullptr
                        || Candidate->CellY < Parent->CellY
                        || (Candidate->CellY == Parent->CellY
                            && Candidate->CellX < Parent->CellX);
                    if (DistanceSq < BestDistanceSq
                        || (DistanceSq == BestDistanceSq && bLowerCell))
                    {
                        BestDistanceSq = DistanceSq;
                        Parent = Candidate;
                    }
                }
            }

            EmitTreeConnector(Child, Parent);
        }

        // Existing probabilistic links remain as texture and loops. They intentionally stay on
        // the cached inner 3×3 working set; only the structural tree build needs the 9×9/15×15 halo.
        if (Params.CrossConnectChance > 0.0f && Shafts.Num() >= 2)
        {
            for (int32 i = 0; i < Shafts.Num(); i++)
            for (int32 j = i + 1; j < Shafts.Num(); j++)
            {
                const FLocalShaft& A = Shafts[i];
                const FLocalShaft& B = Shafts[j];
                const bool bSpineConnector = A.bOriginSpine || B.bOriginSpine;
                const float DSq = FMath::Square(A.X - B.X) + FMath::Square(A.Y - B.Y);
                if (DSq > FMath::Square(Spacing * 1.6f)) continue;  // only neighbours

                // Symmetric pair hash from quantised endpoints.
                const uint32 PH = VoxelHash::Pair(
                    FMath::RoundToInt(A.X), FMath::RoundToInt(A.Y),
                    FMath::RoundToInt(B.X), FMath::RoundToInt(B.Y), S ^ 0xC04Eu);
                if (VoxelHash::ToFloat01(PH) >= Params.CrossConnectChance) continue;

                const float Zc = FMath::Lerp(BottomZ, TopZ, VoxelHash::ToFloat01(VoxelHash::Mix(PH)));
                const float ConnectorR = bSpineConnector
                    ? FMath::Max(
                        Params.ConnectorRadius,
                        RoughnessReach + 1.0f)
                    : Params.ConnectorRadius;
                const FLocalConn Conn{
                    FVector(A.X, A.Y, Zc), FVector(B.X, B.Y, Zc), ConnectorR,
                    0.0f, false };
                if (ConnectorMayReachCell(Conn))
                {
                    Conns.Add(Conn);
                }
            }
        }
    }

    float CaveSDF = FLT_MAX;

    // Vertical shafts as infinite cylinders (boundary seal handles the ends).
    for (const FLocalShaft& Sh : Shafts)
    {
        if (Sh.bOriginSpine) continue;  // VF_ApplyOriginSpine owns the structural column.
        const float DX = WorldX - Sh.X;
        const float DY = WorldY - Sh.Y;
        CaveSDF = FMath::Min(CaveSDF, FMath::Sqrt(DX * DX + DY * DY) - Sh.R);
    }
    for (const FLocalConn& C : Conns)
    {
        CaveSDF = FMath::Min(CaveSDF, VoxelSDF::Capsule(Pos, C.A, C.B, C.Radius));
    }

    // Wall roughness.
    if (Params.SurfaceRoughness > 0.0f && CaveSDF < Params.SurfaceRoughness + 4.0f)
    {
        CaveSDF += FractalNoise3D(FVector3f(WorldX * 0.1f, WorldY * 0.1f, WorldZ * 0.1f), VoxelGenLOD::Eff(3))
                 * VOXEL_NOISE_SCALE * Params.SurfaceRoughness;
    }

    // Carve air inside shafts/connectors.
    const float Blend = 2.0f;
    if (CaveSDF < Blend)
    {
        float Carve = FMath::Clamp((Blend - CaveSDF) / (Blend * 2.0f), 0.0f, 1.0f);
        Carve = SmoothStep01(Carve);
        Density -= Carve * Params.BaseDensity * 2.0f;
    }

    // Partial ledges inside shafts: thin shelves on one side at LedgeSpacing intervals,
    // leaving the opposite side open so the shaft stays traversable.
    if (Params.LedgeSpacing > 0.0f && Params.LedgeDepth > 0.0f && CaveSDF < 0.0f && Shafts.Num() > 0)
    {
        const float Phase = FMath::Frac((WorldZ - Params.StrateBottomWorldZ) / Params.LedgeSpacing);
        const float BandT = FMath::Min(Phase, 1.0f - Phase) * Params.LedgeSpacing;  // dist to nearest band
        if (BandT < Params.LedgeDepth)
        {
            // Nearest shaft center → only shelf the +X/+Y half so a climb path remains.
            const FLocalShaft* Near = nullptr; float BestSq = FLT_MAX;
            for (const FLocalShaft& Sh : Shafts)
            {
                if (Sh.bOriginSpine) continue;  // ledges belong to real shafts, not the post.
                const float D2 = FMath::Square(WorldX - Sh.X) + FMath::Square(WorldY - Sh.Y);
                if (D2 < BestSq) { BestSq = D2; Near = &Sh; }
            }
            // The shaft axis is the structural tree's terminal point. Keep the mathematical
            // half-plane boundary open: tiny cancellation error must not turn an exact axis
            // landing into a solid ledge cap and sever the parent capsule.
            if (Near && (WorldX - Near->X) + (WorldY - Near->Y) > 1.0e-3f)
            {
                float Shelf = 1.0f - SmoothStep01(BandT / Params.LedgeDepth);
                Density = FMath::Max(Density, Shelf * Params.BaseDensity);
            }
        }
    }

    // The drainage tree is a walkable shaft network, not only a connected air SDF. Reassert its
    // air core after partial shaft ledges, then emit support bands at the common first ledge level
    // so every parent edge can be traversed by the player-fit stencil. This remains local to
    // shaft edges; no landing-to-origin road is added.
    for (const FLocalConn& C : Conns)
    {
        if (C.bWalkableFloor)
        {
            VoxelPassageGeometry::VF_ApplyVerticalShaftConnectorAir(
                Density, WorldX, WorldY, WorldZ,
                C.A, C.B, C.Radius, C.FloorZ, Params.BaseDensity);
        }
    }
    for (const FLocalConn& C : Conns)
    {
        if (C.bWalkableFloor)
        {
            VoxelPassageGeometry::VF_ApplyVerticalShaftConnectorFloor(
                Density, WorldX, WorldY, WorldZ,
                C.A, C.B, C.Radius, C.FloorZ, Params.BaseDensity);
        }
    }

    ApplyOriginSpine(Density, WorldX, WorldY, WorldZ,
        Params.StrateTopWorldZ, Params.StrateBottomWorldZ,
        Params.BoundarySealThickness, Params.BaseDensity, OriginSpineRadius);

    ApplyBoundarySeal(Density, WorldZ,
        Params.StrateTopWorldZ, Params.StrateBottomWorldZ,
        Params.BoundarySealThickness, Params.BaseDensity);

    if (StrateManager)
    {
        StrateManager->ApplyPassageModifier(
            Density, WorldX, WorldY, WorldZ,
            Params.BaseDensity, Params.BoundarySealThickness);
    }
    ApplyOriginLandingFloor(Density, WorldX, WorldY, WorldZ,
        Params.StrateTopWorldZ, Params.StrateBottomWorldZ,
        Params.BoundarySealThickness, Params.BaseDensity, OriginSpineRadius);

    VF_ApplyXYEdgeSeal(Density, WorldX, WorldY,
        WorldRadiusVoxels, EdgeSealThickness, Params.BaseDensity);

    return -Density;
}

//=============================================================================
// FLOATING-ISLAND GENERATOR  (ECaveGeneratorType::FloatingIslands)
//=============================================================================
// A large open void with hash-placed island blobs (flattened-top ellipsoids, rough
// undersides) floating at jittered heights. Top/bottom sealed so the void encloses.

float UVoxelGenerator::GetFloatingIslandDensity(float WorldX, float WorldY, float WorldZ,
                                                const FFloatingIslandParams& Params) const
{
    const float H = Params.StrateTopWorldZ - Params.StrateBottomWorldZ;
    if (H <= 0.0f) return 1.0f;

    const float Spacing = FMath::Max(Params.IslandSpacing, 1.0f);
    const uint32 S = (uint32)Seed ^ 0x49736C64u;  // 'Isld'
    const float BlendK = FMath::Max(Params.SDFBlendRadius, 0.01f);

    float Density = -Params.BaseDensity;  // start as open air (void)

    const int32 CX = FMath::FloorToInt(WorldX / Spacing);
    const int32 CY = FMath::FloorToInt(WorldY / Spacing);

    const float MidZ = (Params.StrateTopWorldZ + Params.StrateBottomWorldZ) * 0.5f;

    float IslandSDF = FLT_MAX;

    // IRREGULAR OUTLINE: domain-warp the horizontal query so island edges are lobed and
    // organic instead of perfect circles. Computed once per voxel and shared by all nearby
    // islands (each samples a different part of the field → distinct silhouettes).
    const float WarpAmp = (Params.IslandMinRadius + Params.IslandMaxRadius) * 0.5f * 0.35f;
    // ⚠️ AUDIT §C1 — DERNIER SITE DU PLUGIN, trouvé en portant cet archétype (2026-07-28). Le
    // balayage du 2026-07-27 cherchait le motif `SeedF * K` et celui-ci s'écrit `(float)S * K`, donc
    // il a survécu : à Seed = 2e9 le terme atteint ~1.4e6, où l'ULP du float vaut 0.125 contre un pas
    // de 0.04 par voxel — le warp s'aplatit et les îles redeviennent des cercles parfaits. Corrigé
    // dans les DEUX chemins (ici et FIslandBlobSource) en une passe, pour que le test d'équivalence
    // reste un oracle valable.
    // NOTE : `SeedOffset` quantifie la clé de site par ×100, donc 0.0007 → site 0. Unique aujourd'hui
    // (toutes les autres clés du plugin sont ≥ 0.19) ; la prochaine clé sous 0.005 devra en choisir
    // une autre plutôt que de collisionner en silence.
    const float WX = WorldX + FractalNoise3D(FVector3f(WorldX * 0.04f + VoxelHash::SeedOffset(S, 0.0007f), WorldY * 0.04f, WorldZ * 0.012f), VoxelGenLOD::Eff(3))
                              * VOXEL_NOISE_SCALE * WarpAmp;
    const float WY = WorldY + FractalNoise3D(FVector3f(WorldX * 0.04f + 31.0f, WorldY * 0.04f + 7.0f, WorldZ * 0.012f), VoxelGenLOD::Eff(3))
                              * VOXEL_NOISE_SCALE * WarpAmp;

    // Per-island constants (existence roll, jitter, radius, Z anchor, taper) are pure functions
    // of (cell, seed, params) yet were re-hashed PER VOXEL. Bake the 3×3 neighbourhood's islands
    // once per centre cell (thread_local); the per-voxel work keeps only the warped-frame
    // distance / taper / top-surface math (those genuinely vary per voxel).
    struct FLocalIsland { float X, Y, Rxy, TopHalf, TopZ, BotZ, TaperEnd; };
    thread_local TArray<FLocalIsland, TInlineAllocator<9>> Islands;
    thread_local int32  FI_CX = INT32_MAX, FI_CY = INT32_MAX;
    thread_local uint32 FI_Seed = 0xFFFFFFFFu;
    thread_local float  FI_Spacing = -1.0f, FI_Dens = -1.0f, FI_MinR = -1.0f, FI_MaxR = -1.0f,
                        FI_Thick = -1.0f, FI_VJit = -1.0f, FI_BotZ = FLT_MAX, FI_TopZ = FLT_MAX;

    if (CX != FI_CX || CY != FI_CY || S != FI_Seed || Spacing != FI_Spacing ||
        Params.IslandDensity != FI_Dens || Params.IslandMinRadius != FI_MinR || Params.IslandMaxRadius != FI_MaxR ||
        Params.ThicknessRatio != FI_Thick || Params.VerticalJitter != FI_VJit ||
        Params.StrateBottomWorldZ != FI_BotZ || Params.StrateTopWorldZ != FI_TopZ)
    {
        FI_CX = CX;  FI_CY = CY;  FI_Seed = S;  FI_Spacing = Spacing;
        FI_Dens = Params.IslandDensity;  FI_MinR = Params.IslandMinRadius;  FI_MaxR = Params.IslandMaxRadius;
        FI_Thick = Params.ThicknessRatio;  FI_VJit = Params.VerticalJitter;
        FI_BotZ = Params.StrateBottomWorldZ;  FI_TopZ = Params.StrateTopWorldZ;
        Islands.Reset();

        for (int32 dy = -1; dy <= 1; dy++)
        for (int32 dx = -1; dx <= 1; dx++)
        {
            const int32 nx = CX + dx, ny = CY + dy;
            const uint32 Hh = VoxelHash::Cell(nx, ny, S);
            if (VoxelHash::ToFloat01(Hh) > Params.IslandDensity) continue;

            const float JX = VoxelHash::ToFloat01(VoxelHash::Mix(Hh ^ 0x12345678u));
            const float JY = VoxelHash::ToFloat01(VoxelHash::Mix(Hh ^ 0x9ABCDEF0u));

            FLocalIsland& Isl = Islands.AddDefaulted_GetRef();
            Isl.X = (nx + 0.15f + JX * 0.7f) * Spacing;
            Isl.Y = (ny + 0.15f + JY * 0.7f) * Spacing;
            Isl.Rxy = FMath::Lerp(Params.IslandMinRadius, Params.IslandMaxRadius,
                                  VoxelHash::ToFloat01(VoxelHash::Mix(Hh ^ 0x5A5Au)));

            // ASYMMETRIC ISLAND PROFILE: a fairly flat land slab on TOP, and an underside that
            // tapers DOWN to a rough point (the hanging "roots"). ThicknessRatio scales how deep
            // the underside hangs. This is what reads as a floating island vs. a sphere.
            Isl.TopHalf = Isl.Rxy * 0.20f;                                          // land slab above centre
            const float UnderDepth = Isl.Rxy * FMath::Max(Params.ThicknessRatio, 0.25f); // tapering underside

            const float SpreadZ = FMath::Max(H * 0.5f - FMath::Max(Isl.TopHalf, UnderDepth) - Params.BoundarySealThickness, 0.0f)
                                  * Params.VerticalJitter;
            const float Cz = MidZ + VoxelHash::ToFloatSigned(VoxelHash::Mix(Hh ^ 0xB17Du)) * SpreadZ;
            Isl.TopZ = Cz + Isl.TopHalf;
            Isl.BotZ = Cz - UnderDepth;

            // Per-island taper sharpness (SmoothStep end point) for varied silhouettes.
            Isl.TaperEnd = FMath::Lerp(0.45f, 0.7f, VoxelHash::ToFloat01(VoxelHash::Mix(Hh ^ 0x7A1Eu)));
        }
    }

    for (const FLocalIsland& Isl : Islands)
    {
        // Horizontal distance in the WARPED (lobed) frame so the outline isn't a circle.
        const float Dxw = WX - Isl.X, Dyw = WY - Isl.Y;
        const float DistXY = FMath::Sqrt(Dxw * Dxw + Dyw * Dyw);

        // Radius envelope by height: full width across the top, narrowing to a point at the
        // bottom tip (SmoothStep taper).
        const float Hgt = FMath::Clamp((WorldZ - Isl.BotZ) / FMath::Max(Isl.TopZ - Isl.BotZ, 1.0f), 0.0f, 1.0f);
        const float Taper = SmoothStep01(FMath::Clamp(Hgt / Isl.TaperEnd, 0.0f, 1.0f));
        const float Env = Isl.Rxy * Taper;

        // Top surface: flat by default; dome the edges down when TopFlatten < 1.
        float TopSurf = Isl.TopZ;
        if (Params.TopFlatten < 1.0f)
        {
            const float Edge = FMath::Clamp(DistXY / FMath::Max(Isl.Rxy, 1.0f), 0.0f, 1.0f);
            TopSurf = Isl.TopZ - (1.0f - Params.TopFlatten) * Isl.TopHalf * 2.0f * Edge * Edge;
        }

        // Pseudo-SDF: outside if beyond the radial envelope OR above the top surface.
        const float Sdf = FMath::Max(DistXY - Env, WorldZ - TopSurf);

        IslandSDF = VoxelSDF::SmoothMin(IslandSDF, Sdf, BlendK);
    }

    // Craggy shells.
    if (Params.SurfaceRoughness > 0.0f && IslandSDF < Params.SurfaceRoughness + BlendK + 2.0f)
    {
        IslandSDF += FractalNoise3D(FVector3f(WorldX * 0.08f, WorldY * 0.08f, WorldZ * 0.08f), VoxelGenLOD::Eff(4))
                   * VOXEL_NOISE_SCALE * Params.SurfaceRoughness;
    }

    // Fill solid inside islands.
    const float Blend = BlendK;
    if (IslandSDF < Blend)
    {
        float Fill = FMath::Clamp((Blend - IslandSDF) / (Blend * 2.0f), 0.0f, 1.0f);
        Fill = SmoothStep01(Fill);
        Density += Fill * Params.BaseDensity * 2.0f;
    }

    ApplyOriginSpine(Density, WorldX, WorldY, WorldZ,
        Params.StrateTopWorldZ, Params.StrateBottomWorldZ,
        Params.BoundarySealThickness, Params.BaseDensity, OriginSpineRadius);

    ApplyBoundarySeal(Density, WorldZ,
        Params.StrateTopWorldZ, Params.StrateBottomWorldZ,
        Params.BoundarySealThickness, Params.BaseDensity);

    if (StrateManager)
    {
        StrateManager->ApplyPassageModifier(
            Density, WorldX, WorldY, WorldZ,
            Params.BaseDensity, Params.BoundarySealThickness);
    }
    ApplyOriginLandingFloor(Density, WorldX, WorldY, WorldZ,
        Params.StrateTopWorldZ, Params.StrateBottomWorldZ,
        Params.BoundarySealThickness, Params.BaseDensity, OriginSpineRadius);

    VF_ApplyXYEdgeSeal(Density, WorldX, WorldY,
        WorldRadiusVoxels, EdgeSealThickness, Params.BaseDensity);

    return -Density;
}
