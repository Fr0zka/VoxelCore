// VoxelBiomeTypes.h
// Shared vocabulary for the biome system (Stage 1).
//
// A biome is a "mini-strate-variant": it can carry a FULL archetype param override
// (e.g. its own FSurfaceGenerationParams) plus a content profile, placed across the
// world by a deterministic, window-invariant XY field (warped Voronoi + climate).
//
// Resolution discipline (protects CODEMAP §8.4 + §8.10):
//   - The biome assigned at any XY is a PURE function of world coords + seed + the
//     strate's biome context. No dependence on the chunk window.
//   - Heightfield archetypes (Surface) resolve dominant + neighbour biome per voxel and
//     blend the OUTPUT height — so ANY param difference (even frequencies) stays seamless,
//     which per-param blending could never do. The expensive climate classification is
//     cached once per chunk; per voxel is just a warp + 3x3 lookup.
//   - Structural/global fields (Z bounds, seal, base density, water level) are always
//     forced from the STRATE so seals / spine / passages / water plane stay intact.

#pragma once

#include "CoreMinimal.h"
#include "VoxelBiomeTypes.generated.h"

class UVoxelBiomeDefinition;

/**
 * FBiomeMapParams — controls the world-XY biome field: how big biome regions are,
 * how organic their borders look, and the two climate fields that ASSIGN a biome to
 * each Voronoi cell. Lives on UVoxelStrateDefinition (one map per strate).
 */
USTRUCT(BlueprintType)
struct VOXELFORGE_API FBiomeMapParams
{
    GENERATED_BODY()

    // Average biome cell size in voxels. Larger = bigger, sweeping biome regions.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Biome|Map", meta = (ClampMin = "32.0"))
    float CellSize = 800.0f;

    // Domain-warp the cell lookup by up to this many voxels → organic, non-hexagonal
    // borders. 0 = raw Voronoi cells (straight-ish borders).
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Biome|Map", meta = (ClampMin = "0.0"))
    float WarpStrength = 250.0f;

    // Frequency of the border-warp noise. Lower = broader, sweeping border bends.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Biome|Map")
    float WarpFrequency = 0.0018f;

    // Width (voxels) of the smooth blend band between neighbouring biomes. The density
    // scalars cross-fade across this band so there is no hard wall at a biome border.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Biome|Map", meta = (ClampMin = "0.0"))
    float BorderBlend = 120.0f;

    // ----- Climate fields (assign each cell its biome) -----
    //
    // IMPORTANT: for COHERENT geography (mountain ranges, desert regions — not confetti)
    // the climate must vary much more SLOWLY than CellSize: aim for ~4-6 cells per climate
    // feature, i.e. ClimateFrequency ≈ 1 / (5 * CellSize). If the climate wavelength is
    // near or below CellSize, neighbouring cells sample unrelated climate → salt-and-pepper.

    // Relief ("elevation") field, [0,1]. Mountain / snow biomes live where this is high.
    // Default tuned for CellSize≈800 (wavelength ~3300 ≈ 4 cells). Lower → bigger regions.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Biome|Climate")
    float ReliefFrequency = 0.0003f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Biome|Climate", meta = (ClampMin = "0.25", ClampMax = "4.0"))
    float ReliefContrast = 2.0f;

    // Moisture field, [0,1] — the second climate axis (wet lowlands vs arid). Same
    // coherence rule as relief: keep the wavelength several cells wide.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Biome|Climate")
    float MoistureFrequency = 0.0004f;
};

/**
 * Channel selector for the 2D biome-preview bake (AVoxelWorld::BakeBiomePreview).
 */
UENUM(BlueprintType)
enum class EBiomePreviewChannel : uint8
{
    Biome    UMETA(DisplayName = "Biome (debug colours)"),
    Relief   UMETA(DisplayName = "Relief / elevation field"),
    Moisture UMETA(DisplayName = "Moisture field")
};

/**
 * FVoxelBiomeQuery — a Blueprint-readable snapshot of the biome field at one world point.
 * Returned by AVoxelWorld::GetBiomeAtWorldLocation so BP can read "which biome is under the
 * cursor" — the SAME resolution the decoration scatter uses per column. DecorationCount is the
 * effective number of decorations the dominant biome would place (its own list, or the strate's
 * when empty): 0 here explains a "band of nothing" — that biome simply has no decorations.
 */
USTRUCT(BlueprintType)
struct VOXELFORGE_API FVoxelBiomeQuery
{
    GENERATED_BODY()

    // False when the point's strate has no biome field (then only the climate fields are filled).
    UPROPERTY(BlueprintReadOnly, Category = "Biome") bool bHasBiomes = false;

    // Dominant biome asset at this point (what a decoration column here primarily uses). May be null.
    UPROPERTY(BlueprintReadOnly, Category = "Biome") UVoxelBiomeDefinition* DominantBiome = nullptr;

    // Nearest neighbouring biome (the one a border blend fades toward). May be null / same as dominant.
    UPROPERTY(BlueprintReadOnly, Category = "Biome") UVoxelBiomeDefinition* NeighborBiome = nullptr;

    // Dominant biome display name (falls back to the asset name when BiomeName is unset).
    UPROPERTY(BlueprintReadOnly, Category = "Biome") FText DominantName;

    // Dominant biome's preview/debug colour (handy to tint a BP debug draw to match the bake).
    UPROPERTY(BlueprintReadOnly, Category = "Biome") FLinearColor DebugColor = FLinearColor::Black;

    // Border blend weight toward the neighbour: 0 deep inside a cell → ~0.5 at the shared border.
    UPROPERTY(BlueprintReadOnly, Category = "Biome") float NeighborWeight = 0.0f;

    // Climate fields at this XY, both [0,1] (always filled, even when bHasBiomes is false).
    UPROPERTY(BlueprintReadOnly, Category = "Biome") float Relief = 0.0f;
    UPROPERTY(BlueprintReadOnly, Category = "Biome") float Moisture = 0.0f;

    // Effective decoration count for the dominant biome (its list, else the strate's). 0 ⇒ empty band.
    UPROPERTY(BlueprintReadOnly, Category = "Biome") int32 DominantDecorationCount = 0;

    // Dominant biome's index in the strate's biome context (matches the decoration column tag). -1 = none.
    UPROPERTY(BlueprintReadOnly, Category = "Biome") int32 DominantContextIndex = -1;

    //--- DECORATION STREAMING STATE of the region covering this point (debug "why is this chunk blank?") ---
    // Discriminates the three failure modes for a visibly-bare patch:
    //   • bDecoRegionApplied && DecoAppliedInstances > 0  → instances EXIST + are uploaded; if still not
    //     visible it is a RENDER drop (proxy/culling), not data/streaming.
    //   • bDecoRegionApplied && DecoAppliedInstances == 0 → the region applied but the per-column MARCH
    //     produced no spawns here (config/determinism), not a render or streaming bug.
    //   • bDecoRegionBuilding (not applied)               → region is STILL marching; if it never finishes
    //     (DecoCellsAccounted stuck < DecoCellsTotal) it is a streaming/accounting leak.
    //   • neither flag set                                → region was never requested (desired-set / range).

    // True if the region covering this XY is in DecoRegions (applied; HISMs built).
    UPROPERTY(BlueprintReadOnly, Category = "Biome|Deco") bool bDecoRegionApplied = false;
    // Total HISM instances actually uploaded across the applied region's components.
    UPROPERTY(BlueprintReadOnly, Category = "Biome|Deco") int32 DecoAppliedInstances = 0;
    // True if the region is in RegionBuilds (cells still being marched / merged, not yet applied).
    UPROPERTY(BlueprintReadOnly, Category = "Biome|Deco") bool bDecoRegionBuilding = false;
    // Cells accounted vs the region's total (R*R). Building but accounted < total = in progress; if it
    // never reaches total the region is stuck (the permanent-blank-until-regen signature).
    UPROPERTY(BlueprintReadOnly, Category = "Biome|Deco") int32 DecoCellsAccounted = 0;
    UPROPERTY(BlueprintReadOnly, Category = "Biome|Deco") int32 DecoCellsTotal = 0;

    // PER-CELL decisive probe: re-runs the decoration march for the EXACT cell (chunk footprint) under
    // this point, synchronously, with the current strate context — i.e. how many spawns the deterministic
    // scatter produces here RIGHT NOW. Region state above is R*R-cell coarse and can't see a single blank
    // cell; this can. On a visibly-BLANK cell: >0 ⇒ the march works, the spawns were lost in merge/apply or
    // not drawn (streaming/render bug); 0 ⇒ the march genuinely makes nothing here (data/config, or — if a
    // regen brings grass back at the same spot — a non-determinism bug). -1 ⇒ couldn't run (no strate
    // context / point not in the player's current strate).
    UPROPERTY(BlueprintReadOnly, Category = "Biome|Deco") int32 DecoLiveMarchSpawns = -1;

    // FINAL discriminator, paired with DecoLiveMarchSpawns. Counts the applied region's HISM instances
    // that actually fall inside THIS cell's footprint (not the whole region). On a blank cell where the
    // live march says N>0:
    //   • DecoInstancesInCell ≈ N  → the instances ARE uploaded here but not visible → RENDER drop.
    //   • DecoInstancesInCell ≈ 0  → the march's spawns never reached the HISM → MERGE/apply loss.
    UPROPERTY(BlueprintReadOnly, Category = "Biome|Deco") int32 DecoInstancesInCell = 0;
};

//=============================================================================
// RUNTIME STRUCTS (plain C++ — not USTRUCT; live in the thread-local hot path)
//=============================================================================

/**
 * One biome flattened from its asset for fast, asset-free evaluation. The biome field
 * and the per-chunk cache only ever see these PODs — never a UObject — so they stay
 * cheap to copy and safe to touch from worker threads.
 */
struct FBiomeResolved
{
    // Index back into the owning strate's Biomes[] array (for param / content lookup).
    int32 Index = 0;

    // Climate placement box in (relief, moisture) space, both [0,1].
    float ReliefMin = 0.0f, ReliefMax = 1.0f;
    float MoistureMin = 0.0f, MoistureMax = 1.0f;

    // Colour used by the 2D preview bake.
    FColor DebugColor = FColor::White;

    // Palette layer this biome's terrain uses in the master triplanar material (F6).
    // Baked into the mesh vertex colour so one material can re-skin per biome. 0 = default.
    int32 MaterialPaletteIndex = 0;
};

/**
 * Everything the biome field needs for one strate. Built once by StrateManager
 * (GetBiomeContextForChunk) from the strate definition. Empty Biomes ⇒ biomes
 * disabled for this strate (legacy behaviour, bit-identical output).
 */
struct FBiomeContext
{
    TArray<FBiomeResolved> Biomes;
    FBiomeMapParams Map;

    bool IsValid() const { return Biomes.Num() > 0; }
};

/**
 * Result of a single biome query at a world XY: the dominant biome cell, the nearest
 * neighbour cell, and a blend weight toward that neighbour (0 deep inside a cell,
 * → 0.5 at the shared border). Indices are positions into FBiomeContext::Biomes.
 */
struct FBiomeSample
{
    int32 DominantIndex = -1;
    int32 NeighborIndex = -1;
    float NeighborWeight = 0.0f;
};

/**
 * Per-chunk biome cache for the density hot path. The EXPENSIVE part of a biome query
 * (classifying each Voronoi cell by its climate — several noise samples per cell) is
 * done ONCE here, into a small grid covering the chunk footprint + margin; per voxel
 * the resolver then only warps + does a cheap 3x3 lookup + blend.
 *
 * VALIDITY IS A WORLD-XY BOX (+ ChunkZ + Seed), NOT a chunk key (CODEMAP §8.10). The
 * grid covers a halo beyond the chunk, so gradient-normal samples and the +X/+Y chunk
 * boundary corners stay inside the valid box and DO NOT thrash the noise-heavy rebuild.
 * The box logic is identical in spirit to the SDF cache in GetDensityWithParams.
 */
struct FChunkBiomeCache
{
    // World-XY box (voxel coords) over which the cached grid answers correctly.
    float ValidMinX = 1.0f, ValidMaxX = -1.0f;   // start invalid (min > max)
    float ValidMinY = 0.0f, ValidMaxY = 0.0f;
    int32 ChunkZ = MIN_int32;                     // which strate slice this was built for
    int32 Seed = MIN_int32;
    bool bActive = false;                         // does this strate have biomes?

    FBiomeContext Ctx;                            // resolved biomes + map (for blending)

    // Cell grid (row-major, CellsX × CellsY), each entry = biome index into Ctx.Biomes.
    int32 BaseCellX = 0, BaseCellY = 0, CellsX = 0, CellsY = 0;
    TArray<int32> CellBiome;

    bool Contains(float X, float Y, int32 InChunkZ, int32 InSeed) const
    {
        return InSeed == Seed && InChunkZ == ChunkZ
            && X >= ValidMinX && X <= ValidMaxX
            && Y >= ValidMinY && Y <= ValidMaxY;
    }

    /** AUDIT C2 — force a rebuild on the next query. The validity box says nothing about the
     *  FBiomeContext the cells were classified AGAINST, so when the strate layout is rebuilt
     *  (RebuildStrates / an editor live edit) the grid is stale even though the box still
     *  covers the query. Callers that key on GetLayoutVersion() call this on a version change.
     *  Le box de validité ne dit rien du contexte de biome ayant servi à classer les cellules :
     *  après un rebuild de layout, la grille est périmée alors que la boîte couvre encore. */
    void Invalidate()
    {
        ValidMinX = 1.0f; ValidMaxX = -1.0f;   // min > max ⇒ Contains() is false everywhere
        ChunkZ = MIN_int32;
    }
};
