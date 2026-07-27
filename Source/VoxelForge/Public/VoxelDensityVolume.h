// VoxelDensityVolume.h
// Player-centred DENSITY CLIPMAP — the GPU-bound prerequisite for the mini-sun raymarched
// shadow system (forward rendering). Density is CPU-only (UVoxelGenerator::GetDensityAt), so to
// shadow-march on the GPU we stream the density field into a clipmap of 3D textures centred on
// the player: fine near, coarse far — exactly what shadow rays want (the crisp edge lives near
// the shaded surface; far along the ray, coarse is invisible).
//
// FORMAT (committed): single-channel R8 storing QUANTIZED SIGNED density (solid = high, air =
// low, iso at ~0.5), trilinear-filterable so the iso crossing stays sub-voxel crisp. A "solidity"
// MIP pyramid (max-downsample) is built for empty-space skipping in the march. NOT a true SDF:
// digging is the core verb, and an SDF would need re-distancing (JFA / Eikonal) on every carve &
// streaming refill, whereas the mip pyramid just re-maxes a few blocks — trivially correct + local.
//
// CLIPMAP MODEL: N levels. Level L samples every (1<<L) voxels and holds a Res³ grid of CELLS,
// TOROIDALLY addressed (data for cell C lives at C mod Res), so recentring on movement only needs
// to refill the newly-exposed slabs — not the whole volume. A carve marks its voxel box dirty →
// the overlapping cells refill locally.
//
// THREADING: fills run on ONE DEDICATED thread (FVoxelDensityFillRunnable, off the UE::Tasks pool —
// the old BackgroundLow pool path STARVED behind mesh-gen, ~10 s to resolve). The game thread enqueues
// FPendingFill (Spsc FillQueue) + triggers FillWakeEvent; the thread re-evaluates GetDensityAt
// (thread-safe, deterministic — READS the Generator only, checks bFillThreadStop) and returns filled
// sub-boxes via the Mpsc Results queue; the game thread writes them into the toroidal arrays. Each fill
// carries a VolumeEpoch — stale results (after a regen/season reset) are discarded. CAPTURE-DURING-
// MESHING (level 0) short-circuits most fills: the mesher's already-sampled grid is cached by tile
// coord and blitted on the game thread, so the dedicated thread is only the BACKSTOP (cache misses:
// vertical strate gaps, cold start, carves). EndPlay → NotifyShutdown → StopFillThread Kill(true)s the
// thread (blocks until it stops reading the Generator) before UObject teardown. Determinism preserved
// (GetDensityAt + the diff layer, the only non-deterministic overlay, same as the terrain).
//
// STATUS: step 1a = CPU clipmap + worker fills + toroidal streaming + carve dirty + a DEBUG-DRAW
// visualization (no GPU yet). Step 1b adds the Texture3D upload + the in-material Custom-HLSL march.
// The GPU-upload seam is marked below (UploadDirtyRegionsToGPU).

#pragma once

#include "CoreMinimal.h"
#include "Containers/Queue.h"
#include "VoxelTypes.h"
// ⚠️ IWYU, ET CELUI-CI EST PIÉGEUX : `ENABLE_DRAW_DEBUG` est utilisé en `#if` plus bas. Un macro
// NON DÉFINI vaut 0 dans un `#if` — donc sans cet include le bloc de debug disparaît EN SILENCE au
// lieu de provoquer une erreur de compilation. Il venait du PCH partagé ; `FPSemantics = Precise`
// (AUDIT §C9) nous en prive. Défini par DrawDebugHelpers.h (vérifié dans UE 5.7).
// An UNDEFINED macro evaluates to 0 in an #if, so without this include the debug block vanishes
// SILENTLY instead of failing the build. Defined by DrawDebugHelpers.h (verified in UE 5.7).
#include "DrawDebugHelpers.h"
#include <atomic>
#include "VoxelDensityVolume.generated.h"

class AActor;                   // IWYU : pointeur / TWeakObjectPtr seulement / pointer-only
class UVoxelGenerator;
class UVoxelSettings;
class UVolumeTexture;
class FVoxelDensityFillRunnable;   // dedicated fill thread (VoxelDensityVolume.cpp)
class FRunnableThread;
class FEvent;

UCLASS()
class VOXELFORGE_API UVoxelDensityVolume : public UObject
{
    GENERATED_BODY()

public:
    /** Wire up services. Owner is the AVoxelWorld actor (its transform maps world↔voxel, same as the
     *  content manager); Generator supplies GetDensityAt; Settings supplies the clipmap tunables. */
    void Initialize(AActor* InOwner, UVoxelGenerator* InGenerator, UVoxelSettings* InSettings);

    /** Each Tick: recentre the clipmap on the player, queue fills for newly-exposed cells + any
     *  carve-dirtied cells, launch them under the task budget, and drain finished fills. Cheap when
     *  the player hasn't crossed a level-0 cell boundary and nothing is dirty. */
    void Update(const FVector& PlayerWorldPos);

    /** A carve/fill touched this VOXEL box (inclusive, voxel coords) → refill the overlapping
     *  clipmap cells next Update. GetDensityAt already includes the diff layer, so re-sampling
     *  picks the edit up. Cheap + local. */
    void MarkDirtyVoxelBox(const FIntVector& MinVoxel, const FIntVector& MaxVoxel);

    /** CAPTURE-DURING-MESHING (level-0 only). The mesher already sampled this level-0 tile's density
     *  grid while building its mesh; instead of re-evaluating GetDensityAt in a worker fill, we reuse
     *  those samples. Grid = CHUNK_SIZE³ R8 (X-fast, then Y, then Z), quantized by VF_QuantizeDensity
     *  (bit-identical to a worker fill). Stored in CaptureCache keyed by tile coord and blitted into
     *  level 0's toroidal window now (immediate freshness) + on RecenterLevel(0) (so a window scroll
     *  fills from the cache, not a re-sample). The worker fill stays as the backstop for cache misses
     *  (vertical strate gaps, cold start, evicted tiles). Game-thread only (called from
     *  AVoxelWorld::ProcessPendingChunks). Moves Grid. */
    void IngestTileCapture(const FIntVector& L0TileCoord, TArray<uint8>&& Grid);

    /** True if a level-0 tile's capture could be consumed (inside the shadow window + lead-shell
     *  margin, or no window yet). AVoxelWorld checks this BEFORE asking the mesher to capture, so the
     *  many level-0 tiles streaming outside the small shadow window don't pay the quantize+copy for a
     *  grid IngestTileCapture would refuse anyway. Game-thread only. */
    bool IsTileCaptureUseful(const FIntVector& L0TileCoord) const;

    /** Season reset / full regen: bump the epoch (drops in-flight fills), drop all data, force a
     *  full refill on the next Update. */
    void Reset();

    /** EndPlay: flag shutdown + spin-wait for in-flight fills (they read the Generator) before the
     *  owner tears UObjects down. */
    void NotifyShutdown();

    virtual void BeginDestroy() override;

#if ENABLE_DRAW_DEBUG
    /** Step-1a visual check: draw boxes for solid level-0 cells near the player (gated + capped). */
    void DebugDraw() const;
#endif

    //--- GPU accessors (step 1b: the material march / debug visualization sample these) ----------
    /** The R8 volume texture for a clip level (null if GPU upload is off / not yet created). */
    UVolumeTexture* GetLevelTexture(int32 Level) const;

    /** Shader params for a level: OriginCells (min cell coord), Step (voxels/cell), Res (cells/axis).
     *  The material maps WorldPos → local voxel → cell C = floor(localVoxel/Step), then samples at
     *  UVW = (C + 0.5)/Res with WRAP addressing (the texture is toroidal). Returns false if the level
     *  has no data yet. (Feeds the MPC in 1b-ii.) */
    bool GetLevelShaderParams(int32 Level, FIntVector& OutOriginCells, float& OutStep, int32& OutRes) const;

private:
    // One concentric clip level. Step = 1<<L voxels; the grid covers Res cells (= Res*Step voxels).
    struct FClipLevel
    {
        int32       Step = 1;                                          // voxel sampling step (1<<L)
        FIntVector  OriginCells = FIntVector(INT32_MAX, INT32_MAX, INT32_MAX); // min CELL coord; sentinel = no data
        TArray<uint8> Density;                                         // Res³ R8, toroidally addressed
        bool        bHasData = false;
        bool        bGPUDirty = false;                                // CPU data changed → re-upload the texture
    };

    // A fill request (game-thread queue, drained under the task budget). Box is in CELL coords.
    struct FPendingFill
    {
        int32      Level = 0;
        FIntVector MinCells = FIntVector::ZeroValue;
        FIntVector DimCells = FIntVector::ZeroValue;
        uint32     Epoch = 0;
    };

    // Worker → game-thread result: one filled sub-box, linear row-major (X fastest, then Y, then Z).
    struct FFillResult
    {
        int32         Level = 0;
        uint32        Epoch = 0;
        FIntVector    MinCells = FIntVector::ZeroValue;
        FIntVector    DimCells = FIntVector::ZeroValue;
        TArray<uint8> Data;
    };

    void  EnsureAllocated();
    int32 ResPerAxis() const;                 // clamped Settings->DensityVolumeResolution
    int32 NumLevels()  const;                 // clamped Settings->DensityVolumeLevels

    // Recentre one level on the player voxel; push fills for the slabs that scrolled into view.
    void  RecenterLevel(int32 L, const FIntVector& PlayerVoxel);
    // new\old box subtraction → up to 6 disjoint cell-boxes (the toroidal slabs to refill).
    static void BoxDifference(const FIntVector& NewMin, const FIntVector& NewDim,
                              const FIntVector& OldMin, const FIntVector& OldDim,
                              TArray<TPair<FIntVector, FIntVector>>& OutBoxes);
    // Split a cell-box into Z-slabs + enqueue as FPendingFills (so no single task is huge).
    void  QueueFillSplit(int32 L, const FIntVector& MinCells, const FIntVector& DimCells);

    // CAPTURE-DURING-MESHING (level 0). Fill a newly-exposed cell box from the capture cache where a
    // tile is present (game-thread memcpy, no GetDensityAt); queue a worker fill for the rest (backstop).
    void  FillBoxFromCacheOrQueue(const FIntVector& MinCells, const FIntVector& DimCells);
    // Write a cached tile's in-window cells into level 0's toroidal array (+ GPU dirty). Idempotent.
    bool  BlitCaptureToWindow(const FIntVector& L0TileCoord, const TArray<uint8>& Grid);
    // The tile-coord box worth caching: level 0's window +1 tile margin (the lead shell). False = no window yet.
    bool  GetCaptureKeepBounds(FIntVector& OutLo, FIntVector& OutHi) const;
    // Drop cache entries whose tile is fully outside the keep bounds.
    void  EvictFarCaptures();
    void  LaunchPendingFills();               // flush PendingFills onto the dedicated fill thread
    void  DrainResults();                     // apply finished fills into the toroidal arrays (+ GPU dirty)

    // DEDICATED FILL THREAD. The volume fill used to run on the shared UE::Tasks pool (BackgroundLow),
    // where it STARVED behind mesh-gen (10 s to resolve shadows at a fresh spot). It now runs on its own
    // thread (off the pool) so it's fast AND never steals a core from mesh-gen. Game thread enqueues
    // FPendingFill (Spsc), the thread re-evaluates GetDensityAt and pushes FFillResult into Results
    // (existing Mpsc, drained on the game thread by DrainResults). Capture still short-circuits most of
    // this (cache hits blit on the game thread, no fill); the thread is the backstop for misses.
    friend class FVoxelDensityFillRunnable;
    void  ProcessOneFill(const FPendingFill& F);   // RUNS ON THE FILL THREAD (reads Generator only)
    void  EnsureFillThread();
    void  StopFillThread();

    // GPU upload (step 1b-i). EnsureTextures (re)creates the per-level R8 volume textures when the
    // resolution / level count changes; UploadDirtyTextures enqueues a render command per dirty level
    // that RHIUpdateTexture3D's the whole level from the CPU array (the array IS the toroidal texture
    // layout, so a full re-upload is correct without wrap-splitting; sub-box upload is a later optim).
    void  EnsureTextures();
    void  UploadDirtyTextures();

    static FORCEINLINE uint8 Quantize(float MCDensity);   // MC density (neg=solid) → R8 (solid=high)
    static FORCEINLINE int32 FloorDiv(int32 A, int32 B);  // true floor division (B>0)

    TWeakObjectPtr<AActor> Owner;

    UPROPERTY()
    UVoxelGenerator* Generator = nullptr;

    UPROPERTY()
    UVoxelSettings* Settings = nullptr;

    TArray<FClipLevel> Levels;
    int32 AllocatedRes = 0;                   // resolution the arrays were sized for (realloc on change)

    // Per-level R8 volume textures (GPU). UPROPERTY so they're GC-kept; contents updated via RHI.
    UPROPERTY()
    TArray<TObjectPtr<UVolumeTexture>> LevelTextures;
    int32 AllocatedTexRes = 0;                // resolution the textures were created at (recreate on change)

    TArray<FPendingFill> PendingFills;        // cell-boxes staged on the game thread, flushed to FillQueue
    TQueue<FPendingFill, EQueueMode::Spsc> FillQueue;   // game thread → fill thread
    TQueue<FFillResult, EQueueMode::Mpsc> Results;     // fill thread enqueues, game thread drains

    // Dedicated fill thread handles (see EnsureFillThread / StopFillThread / ProcessOneFill).
    FVoxelDensityFillRunnable* FillRunnable = nullptr;
    FRunnableThread* FillThread = nullptr;
    FEvent* FillWakeEvent = nullptr;
    std::atomic<bool> bFillThreadStop{ false };

    // CAPTURE-DURING-MESHING (level-0 only): tile coord → CHUNK_SIZE³ R8 captured density. Populated by
    // IngestTileCapture (free — the mesher already sampled it), consumed by RecenterLevel(0) to fill
    // exposed cells without re-sampling. Game-thread only; evicted by window distance (EvictFarCaptures).
    TMap<FIntVector, TArray<uint8>> CaptureCache;

    std::atomic<bool> bShuttingDown{ false };
    uint32 VolumeEpoch = 1;                    // bumped on Reset → stale fills discarded

    FIntVector LastPlayerVoxel = FIntVector(INT32_MAX, INT32_MAX, INT32_MAX);
    bool bInitialized = false;
};
