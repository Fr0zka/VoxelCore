// VoxelWorld.h
// The main world manager - orchestrates chunk loading, generation, meshing, and rendering

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include <atomic>
#include "VoxelTypes.h"
#include "VoxelGenerator.h"
#include "VoxelMarchingCubesMesher.h"
#include "VoxelSettings.h"
#include "VoxelStrateManager.h"
#include "VoxelDiffLayer.h"
#include "VoxelWorld.generated.h"

// Forward declaration
class URealtimeMeshComponent;
class URealtimeMeshSimple;
class UVoxelDiffLayer;
class UVoxelContentManager;
class UVoxelAtmosphereManager;
class UVoxelDensityVolume;
class UMaterialParameterCollection;
class UVolumeTexture;
class UMaterialInterface;
class UMaterialInstanceDynamic;
class FScopedGenerationPause;
namespace RealtimeMesh { struct FRealtimeMeshStreamSet; }   // T1.f — worker-built geometry buffers

/**
 * How a streaming anchor wants its tiles built (ARCHITECTURE §9.3). The local player is an implicit
 * FullVisual anchor (the clipmap). Extra anchors — AI, and later remote players — are registered so
 * their surroundings stream too (so they physically exist away from the local camera).
 *
 * `CollisionOnly` tiles cook collision but are hidden (`SetVisibility(false)`) — no draw / VSM cost —
 * UNLESS the player clipmap also wants that exact level-0 tile (then it renders normally). This is the
 * §9.4 render-skip: it kills the cost of terrain around AI/remote players far from the local camera.
 * (Geometry streams are still built on the worker — off the frame; a deeper "no stream build at all"
 * collision-only path is a later optimization.)
 */
UENUM(BlueprintType)
enum class EVoxelAnchorPolicy : uint8
{
    CollisionOnly UMETA(DisplayName = "Collision Only"),   // enough to stand / pathfind / hit
    FullVisual    UMETA(DisplayName = "Full Visual")       // rendered too (a second local viewpoint)
};

/** A non-player streaming center: keep a small box of level-0 tiles loaded around Actor so it has
 *  collision wherever it is. Plain internal struct (weak ptr is GC-safe without reflection). */
struct FVoxelStreamingAnchor
{
    TWeakObjectPtr<AActor> Actor;
    EVoxelAnchorPolicy Policy = EVoxelAnchorPolicy::CollisionOnly;
    // Thin box around the anchor (level-0 chunks). Default = the chunk it's in + 1 on each horizontal
    // edge (3×3) + 1 chunk BELOW (ground safety when its capsule sits near a chunk's bottom); nothing
    // above (a grounded NPC doesn't need it — raise ZAbove for flyers). Empty tiles in the box are ~free.
    int32 XYRadiusChunks = 1;   // horizontal Chebyshev radius (0 = its own column)
    int32 ZBelowChunks   = 1;   // chunks below the anchor's chunk
    int32 ZAboveChunks   = 0;   // chunks above
    FIntVector LastChunk = FIntVector(MAX_int32, MAX_int32, MAX_int32);   // move detection (rebuild trigger)
};

/**
 * AVoxelWorld - The main voxel terrain actor
 *
 * RESPONSIBILITIES:
 * - Track which chunks should be loaded based on player position
 * - Create/destroy chunks as player moves
 * - Coordinate generation and meshing
 * - Manage mesh components for rendering
 *
 * LIFECYCLE:
 * - BeginPlay: Initialize generator, mesher
 * - Tick: Update chunks around player position
 */
struct FChunkResult
{
    FVoxelTileKey Tile;       // which clipmap tile this mesh is for (carries coord + level)
    // T1.f: the RMC geometry buffers are BUILT ON THE WORKER (BuildTileStreamSet in the gen task)
    // so the game thread only uploads them — the per-vertex builder loop was the dominant
    // game-thread cost while moving (the apply drain). TSharedPtr (not a by-value StreamSet) so
    // FChunkResult stays movable through the MPSC queue with the type only FORWARD-DECLARED here.
    // Null ⇒ empty/all-air tile (no component).
    TSharedPtr<RealtimeMesh::FRealtimeMeshStreamSet> Streams;
    uint32 Epoch = 0;         // Generation epoch — discard if stale
    // Monotonic request timestamp used only for streaming telemetry. It is carried through the
    // worker queue so the profile can report request-to-ready, not just worker generation time.
    uint64 RequestStartCycles = 0;
    bool bAborted = false;    // Worker observed shutdown; never mark this tile loaded
    bool bEmpty = true;       // true ⇒ all-air tile (Streams null); still marked loaded so we don't re-submit
    // F17 — the mesher classifies every triangle semantically (sky-cap = down-facing near the
    // column's CeilSurf; overhangs/cave roofs stay ground) and packs them as two contiguous runs
    // (ground then cap) in the index buffer → polygroups 0/1 → two RMC sections with their own
    // material + shadow flag. These tell the apply path which sections exist (RMC only creates a
    // section for a non-empty polygroup — configuring a missing one is invalid).
    bool bHasGroundTris  = false;
    bool bHasCeilingTris = false;
    // STRATE CONTENT CUT — the chunk-Z band this tile was MESHED with (MIN/MAX = uncut). The
    // apply path clamps its strate/material lookups into it (a coarse tile's raw min corner can
    // sit in a strate whose content was cut out of the mesh entirely).
    int32 BandChunkLo = MIN_int32;
    int32 BandChunkHi = MAX_int32;
    // CAPTURE-DURING-MESHING: the tile's CHUNK_SIZE³ R8 density grid, captured by the mesher (no extra
    // GetDensityAt). Non-empty only for capture-eligible tiles (level 0, full-res). The game thread hands
    // it to UVoxelDensityVolume::IngestTileCapture so the density clipmap reuses the mesher's samples
    // instead of re-sampling. Moved (not copied) through the MPSC queue. See UVoxelDensityVolume.
    TArray<uint8> CaptureGrid;
};

UCLASS()
class VOXELFORGE_API AVoxelWorld : public AActor
{
    GENERATED_BODY()

public:
    AVoxelWorld();

    //=========================================================================
    // SETTINGS
    //=========================================================================
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel World")
    UVoxelSettings* Settings;

    //=========================================================================
    // COMPONENTS & REFERENCES
    //=========================================================================

    /** The terrain generator */
    UPROPERTY()
    UVoxelGenerator* Generator;

    /** The mesh builder */
    UPROPERTY()
    UVoxelMarchingCubesMesher* Mesher;

    /** The strate manager — maps depth to strate definitions */
    UPROPERTY()
    UVoxelStrateManager* StrateManager;

    /** Player terrain modifications (carving & filling).
     *  Stores density diffs on top of procedural terrain.
     *  Created in BeginPlay, passed to Generator for density evaluation. */
    UPROPERTY()
    UVoxelDiffLayer* DiffLayer;

    /** Spawns per-chunk decorations/actors from the strate content pools and the
     *  aesthetic water surfaces. Created in BeginPlay. */
    UPROPERTY()
    UVoxelContentManager* ContentManager;

    /** Drives per-strate fog + ambient + persistent ceiling/floor layer actors
     *  (e.g. seas of clouds) based on the strate the player is in. Created in BeginPlay
     *  when bManageAtmosphere is true. */
    UPROPERTY()
    UVoxelAtmosphereManager* AtmosphereManager;

    /** Player-centred density CLIPMAP streamed onto the GPU for the mini-sun raymarched shadow
     *  system (forward rendering). Created in BeginPlay when Settings->bEnableDensityVolume is on.
     *  Filled on worker threads (re-evaluating GetDensityAt), recentred toroidally as the player
     *  moves, refilled locally on carve. See UVoxelDensityVolume. */
    UPROPERTY()
    UVoxelDensityVolume* DensityVolume;

    /** Shared Material Instance Dynamics that bind the density-volume textures + per-frame shadow params
     *  (clipmap transform + nearest orb) onto the terrain material(s). Keyed by BASE material so every
     *  tile of a given base shares ONE MID (no batching cost). Created lazily in ApplyMeshToTile,
     *  refreshed each Tick by UpdateTerrainMaterialParams. */
    UPROPERTY()
    TMap<TObjectPtr<UMaterialInterface>, TObjectPtr<UMaterialInstanceDynamic>> TerrainMIDs;

    /** When true, VoxelForge spawns & drives its own height fog + skylight + ceiling/floor
     *  layer actors from each strate's settings. Turn OFF if you manage fog/lighting
     *  yourself in the level (avoids a duplicate ExponentialHeightFog). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel World")
    bool bManageAtmosphere = true;

    //=========================================================================
    // CHUNK STORAGE
    //=========================================================================

    //=========================================================================
    // CLIPMAP TILE STORAGE (chunked-LOD)
    //=========================================================================
    // A level-L tile spans (CHUNK_SIZE<<L) voxels meshed at step (1<<L) → constant 32³-cell
    // mesh, one component, one draw, covering 8^L× the volume. Streaming loads concentric
    // shells (level 0 near, coarser far), so total tile count stays low (~1-2k) regardless
    // of view distance. That low count is WHY each tile can have its own component without a
    // game-thread problem (this supersedes the earlier region-batching). Collision + content
    // are level-0 only.

    /** Tiles fully loaded — INCLUDING empty/all-air tiles, so we never re-submit them. */
    TSet<FVoxelTileKey> LoadedTiles;

    /** Render component per NON-empty loaded tile (GC-safe via actor ownership; not UPROPERTY
     *  because FVoxelTileKey isn't a USTRUCT key). */
    TMap<FVoxelTileKey, URealtimeMeshComponent*> TileComponents;

    /** T2.c — COMPONENT POOL. Unloading a tile parks its component here (geometry + collision
     *  stripped via RemoveSectionGroup, hidden, still registered) instead of DestroyComponent;
     *  ApplyMeshToTile pops from here instead of NewObject + RegisterComponent. Kills the
     *  create/register/GC churn of fast travel and regen bursts. Same GC-safety rationale as
     *  TileComponents (registered components are owned by the actor). Bounded — overflow is
     *  destroyed for real. */
    TArray<URealtimeMeshComponent*> TileComponentPool;
    static constexpr int32 MaxPooledTileComponents = 256;

    /** Pop a pooled tile component (made visible again) or create + register a fresh one. */
    URealtimeMeshComponent* AcquireTileComponent();

    /** Park a tile component in the pool (strip geometry/collision, hide) — or destroy it
     *  for real when the pool is full. */
    void ReleaseTileComponent(URealtimeMeshComponent* Comp);

    /** T2.d — the effective concurrent gen-task budget: the asset's MaxConcurrentTasks,
     *  capped to (logical cores − 2) so small CPUs don't thrash on a flat 16 (background
     *  priority stops frame starvation, not the context-switch overhead). */
    int32 GetMaxConcurrentTasks() const;

    //=========================================================================
    // TERRAIN MODIFICATION (player carving & filling)
    //=========================================================================

    /**
     * Carve terrain at a world position (remove rock, create air).
     * Applies a spherical brush that subtracts density.
     *
     * @param Position - World-space center of the carve brush
     * @param Radius - Brush radius in voxels (default 3)
     * @param Strength - How aggressively to carve (default 10, higher = deeper)
     */
    UFUNCTION(BlueprintCallable, Category = "Voxel World|Modification")
    void CarveAtPosition(FVector Position, float Radius = 3.0f, float Strength = 10.0f);

    /**
     * Fill terrain at a world position (add rock, seal holes).
     * Applies a spherical brush that adds density.
     *
     * @param Position - World-space center of the fill brush
     * @param Radius - Brush radius in voxels (default 3)
     * @param Strength - How aggressively to fill (default 10, higher = more solid)
     */
    UFUNCTION(BlueprintCallable, Category = "Voxel World|Modification")
    void FillAtPosition(FVector Position, float Radius = 3.0f, float Strength = 10.0f);

    /**
     * Box brush carve/fill. Position is world-space (Unreal units); ExtentVoxels is the
     * box half-size in voxels. Strength magnitude controls aggressiveness (sign forced).
     */
    UFUNCTION(BlueprintCallable, Category = "Voxel World|Modification")
    void CarveBox(FVector Position, FVector ExtentVoxels, float Strength = 12.0f);

    UFUNCTION(BlueprintCallable, Category = "Voxel World|Modification")
    void FillBox(FVector Position, FVector ExtentVoxels, float Strength = 12.0f);

    /**
     * Capsule brush carve/fill between two world-space points (Unreal units), with a
     * tube radius in voxels. Great for boring tunnels or laying solid pillars/walls.
     */
    UFUNCTION(BlueprintCallable, Category = "Voxel World|Modification")
    void CarveCapsule(FVector WorldA, FVector WorldB, float RadiusVoxels = 3.0f, float Strength = 12.0f);

    UFUNCTION(BlueprintCallable, Category = "Voxel World|Modification")
    void FillCapsule(FVector WorldA, FVector WorldB, float RadiusVoxels = 3.0f, float Strength = 12.0f);

    /**
     * Apply a fully-specified modification (any shape). Centers/endpoints are expected
     * in VOXEL coordinates here (this is the low-level entry the helpers build on).
     * Returns nothing; re-meshes affected chunks.
     */
    UFUNCTION(BlueprintCallable, Category = "Voxel World|Modification")
    void ApplyModification(const FVoxelModification& Modification);

    /**
     * Clear all player modifications (e.g., on season reset).
     * Regenerates all currently loaded chunks to restore procedural terrain.
     */
    UFUNCTION(BlueprintCallable, Category = "Voxel World|Modification")
    void ClearAllModifications();

    //=========================================================================
    // SEED / SEASON MANAGEMENT
    //=========================================================================

    /**
     * Change the world seed and regenerate everything.
     *
     * This is the "season reset" operation:
     * 1. Updates the seed in Settings, Generator, and StrateManager
     * 2. Clears ALL player modifications (carvings are meaningless in a new world)
     * 3. Resets the elevator depth (player must re-discover strates)
     * 4. Increments the season counter
     * 5. Unloads all chunks and lets Tick reload them with the new seed
     *
     * The game layer is responsible for calling this at season boundaries,
     * saving/loading the season counter, and any pre-reset cleanup (inventory, etc.)
     *
     * @param NewSeed - The new world seed
     */
    UFUNCTION(BlueprintCallable, Category = "Voxel World|Season")
    void ChangeSeed(int32 NewSeed);

    /**
     * Get the current world seed.
     */
    UFUNCTION(BlueprintPure, Category = "Voxel World|Season")
    int32 GetCurrentSeed() const;

    /**
     * Get the current season number (incremented each time ChangeSeed is called).
     */
    UFUNCTION(BlueprintPure, Category = "Voxel World|Season")
    int32 GetCurrentSeason() const;

    /** Reviewed manifest digest to replicate/compare at join; empty on the legacy authored path. */
    UFUNCTION(BlueprintPure, Category = "Voxel World|Season")
    FString GetCurrentSeasonContentHash() const;

    //=========================================================================
    // STRATE QUERIES (gameplay integration)
    //=========================================================================

    /**
     * Get which strate index a world position is in.
     *
     * @param WorldPosition - Position in world space (Unreal units)
     * @return Strate index (0 = topmost), or -1 if outside all strates
     */
    //=========================================================================
    // ESPACE MONDE <-> ESPACE ACTEUR / WORLD <-> ACTOR SPACE
    //=========================================================================
    // Le champ de densite est defini en espace ACTEUR : (0,0) est l origine de CET acteur, pas
    // celle du monde Unreal. Tout point venant d Unreal (position du pion, hit de trace, ancre de
    // streaming) DOIT passer par ces helpers avant de toucher quoi que ce soit cote voxel.
    //
    // The density field is authored in ACTOR space: (0,0) is THIS actor s origin, not Unreal s
    // world origin. Any point arriving from Unreal (pawn position, trace hit, streaming anchor)
    // MUST go through these helpers before touching anything voxel-side.
    //
    // INVARIANT VERIFIABLE PAR GREP / GREP-CHECKABLE INVARIANT:
    //   aucun `/ VOXEL_SIZE` applique a un parametre nomme `World*` hors de ces fonctions.
    //   no `/ VOXEL_SIZE` applied to a parameter named `World*` outside these functions.
    // C est la vraie garantie : le risque n est pas un site rate parmi ceux deja trouves, c est
    // le SITE SUIVANT que personne ne verra. The risk is not a missed site among those already
    // found - it is the NEXT one nobody notices.
    //
    // La racine est Static (voir BeginPlay) => la transform ne bouge pas en jeu, donc la relire a
    // chaque appel est gratuit et ne demande aucune invalidation.
    // The root is Static (see BeginPlay), so the transform cannot change during play: re-reading
    // it per call is free and needs no invalidation.

    /** World cm -> actor-local cm. */
    UFUNCTION(BlueprintPure, Category = "Voxel World|Space")
    FVector WorldToLocalCm(FVector WorldPos) const;

    /** World cm -> actor-local VOXEL coords (what every density / diff-layer API expects). */
    UFUNCTION(BlueprintPure, Category = "Voxel World|Space")
    FVector WorldToLocalVoxel(FVector WorldPos) const;

    /** Actor-local VOXEL coords -> world cm (placing actors, debug draws, UI markers). */
    UFUNCTION(BlueprintPure, Category = "Voxel World|Space")
    FVector LocalVoxelToWorld(FVector VoxelPos) const;

    UFUNCTION(BlueprintPure, Category = "Voxel World|Strate")
    int32 GetStrateAtPosition(FVector WorldPosition) const;

    /**
     * Probe the biome field at a world location (e.g. a mouse line-trace hit). Returns the dominant +
     * neighbour biome, the climate fields, the border blend weight, and the dominant biome's decoration
     * count — the SAME resolution the decoration scatter uses per column. Use it to debug placement:
     * a returned DominantDecorationCount of 0 means that biome has no decorations (an empty region),
     * NOT a bug. WorldLocation is full world space (the actor transform is undone internally).
     */
    UFUNCTION(BlueprintCallable, Category = "Voxel World|Biome")
    FVoxelBiomeQuery GetBiomeAtWorldLocation(FVector WorldLocation) const;

    /**
     * SurfaceWorld ground finder for self-arranging prefabs (ruins/set-pieces authored as Blueprints):
     * the terrain surface + sky-cap ceiling world-Z under WorldLocation's XY, WITHOUT any line trace or
     * streamed collision. Deterministic and available before the area meshes. Returns false (outs = the
     * input Z) when the point isn't a SurfaceWorld heightfield (cave strates) — fall back to a trace there.
     * Does NOT account for passage/spine carving; re-check with a trace if the spot might be carved.
     */
    UFUNCTION(BlueprintCallable, Category = "Voxel World|Query")
    bool GetVoxelSurfaceHeightAt(FVector WorldLocation, float& OutSurfaceWorldZ, float& OutCeilingWorldZ) const;

    //=========================================================================
    // STREAMING ANCHORS — keep terrain (collision) loaded around actors that aren't the local player
    // (AI now; remote players later). ARCHITECTURE §9.3. Registering an actor folds a small box of
    // level-0 tiles around it into the desired set, so it has ground to stand on / pathfind / be hit
    // even far from the camera. Idempotent per actor (re-register updates the policy/radius).
    //=========================================================================

    /** Start streaming terrain around Actor. Default box = its chunk + 1 horizontal ring + 1 chunk below
     *  (ground safety), nothing above. Empty tiles are ~free (trivial-tile reject); with the CollisionOnly
     *  policy the SOLID tiles cook collision but don't render (§9.4). Raise ZAbove for flyers/tall NPCs. */
    UFUNCTION(BlueprintCallable, Category = "Voxel World|Streaming")
    void RegisterStreamingAnchor(AActor* Actor,
                                 EVoxelAnchorPolicy Policy = EVoxelAnchorPolicy::CollisionOnly,
                                 int32 XYRadiusChunks = 1,
                                 int32 ZBelowChunks = 1,
                                 int32 ZAboveChunks = 0);

    /** Stop streaming terrain around Actor. Its tiles are released by the normal delta cull. */
    UFUNCTION(BlueprintCallable, Category = "Voxel World|Streaming")
    void UnregisterStreamingAnchor(AActor* Actor);

    //=========================================================================
    // LIGHTING — DENSITY VOLUME (debug / material wiring)
    //=========================================================================

    /** The GPU R8 density volume texture for a clip level (0 = finest, near the player). Null until the
     *  volume has streamed in / if GPU upload is off. STEP 1b-i validation: in a debug BP, create a
     *  dynamic material instance of a Volume-Texture-sampling material and SetTextureParameterValue from
     *  this — you should see the density field, centred on the player, updating as you move & carve. */
    UFUNCTION(BlueprintCallable, Category = "Voxel World|Lighting")
    UVolumeTexture* GetDensityVolumeTexture(int32 Level = 0) const;

private:
    friend class FScopedGenerationPause;

    /** Get/create the shared MID wrapping a base terrain material (binds volume textures + shadow params).
     *  Returns Base unchanged-wrapped, or nullptr if Base is null. */
    UMaterialInstanceDynamic* GetOrCreateTerrainMID(UMaterialInterface* Base);

    /** Recompute the packed volume/orb shader params (TVP0..4) from the density volume + nearest orb, and
     *  push them (and the volume textures) onto every terrain MID. Called each Tick. */
    void UpdateTerrainMaterialParams();

    /** Apply the current TVP0..4 + level-0 volume texture to one MID (also used on MID creation). */
    void SetVolumeParamsOnMID(UMaterialInstanceDynamic* MID) const;

    // Packed shader params, recomputed each Tick. ALL meaningful data is in .xyz — a material Vector
    // Parameter only delivers float3 (RGB) into a Custom node (the alpha is dropped), so we never use .w.
    //   TVP0 = L0 WindowOrigin.xyz (world cm)   TVP1 = L0 OriginMod.xyz (cells)
    //   TVP2 = OrbPos.xyz (world cm)            TVP3 = OrbColor.rgb * OrbIntensity (premultiplied)
    //   TVP4 = (OrbMaxDist, OrbFalloff, MarchSteps)   TVP5 = (Res, L0 CellWorldSize, OrbEnable)
    //   TVP6 = L1 WindowOrigin.xyz   TVP7 = L1 OriginMod.xyz
    //   TVP8 = L2 WindowOrigin.xyz   TVP9 = L2 OriginMod.xyz
    // Coarser levels' cell size is derived in-shader (cell_L = L0Cell * 2^L); Res is shared.
    FLinearColor TVP0 = FLinearColor::Black, TVP1 = FLinearColor::Black, TVP2 = FLinearColor::Black,
                 TVP3 = FLinearColor::Black, TVP4 = FLinearColor::Black, TVP5 = FLinearColor::Black,
                 TVP6 = FLinearColor::Black, TVP7 = FLinearColor::Black,
                 TVP8 = FLinearColor::Black, TVP9 = FLinearColor::Black;

    // Change-detection for the per-Tick MID pushes (MIDs OWN their param values, so skip-if-identical
    // is safe there — unlike the MPC, whose world instance can reset behind our back; see
    // UpdateOrbLightMPC, which deliberately rewrites every frame).
    TWeakObjectPtr<UVolumeTexture> LastBoundVolTex0;              // re-push MIDs if the L0 texture was recreated

public:

    //=========================================================================
    // LIVE EDIT (debug tuning in PIE)
    //=========================================================================

    /** When true, editing your Strate Definition data assets during PIE
     *  will automatically regenerate all chunks so you see the result live.
     *  Also adds a "Regenerate" button in Details for manual refresh. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Live Edit")
    bool bLiveEditStrates = false;

#if WITH_EDITORONLY_DATA
    /** Seed passed to the deterministic offline composer roll. This does not change Settings->Seed. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Live Edit|Composer")
    int32 ComposerSeed = 0;

    /** Candidate row passed to VF_RollStrateCandidate (the offline tests use the same index). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Live Edit|Composer", meta = (ClampMin = "0"))
    int32 ComposerCandidateIndex = 0;

    /** Existing layout slot to replace; 0 is the topmost strate. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Live Edit|Composer", meta = (ClampMin = "0"))
    int32 ComposerTargetStrateIndex = 0;

    /** False rolls one native parameter vector; true rolls the structure recipe and its six blocks. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Live Edit|Composer")
    bool bComposerRollStructure = false;
#endif

#if WITH_EDITOR
    /** Roll and apply the selected offline composer candidate to one live PIE slot. */
    UFUNCTION(CallInEditor, Category = "Live Edit|Composer")
    void ApplyComposerCandidate();
#endif

    /** Click to force-regenerate all chunks right now (useful during PIE). */
    UFUNCTION(CallInEditor, BlueprintCallable, Category = "Live Edit")
    void RegenerateAllChunks();

    /** Re-read ALL of VoxelSettings (strate layout, inter-strate gap, passages, spine)
     *  and rebuild from scratch, then regenerate every chunk. Use this after changing
     *  passage / gap / spine settings so they apply live without restarting PIE —
     *  RegenerateAllChunks alone keeps the existing layout & passages. */
    UFUNCTION(CallInEditor, BlueprintCallable, Category = "Live Edit")
    void RebuildStrates();

    /** F2 — DETERMINISM VALIDATOR (run during PIE, takes ~a second). Samples a band of
     *  densities at a chunk boundary near the player through TWO cache-window alignments
     *  (thread_local chunk caches warmed from the left chunk, then from the right one, same
     *  points re-sampled) plus a same-alignment repeat. Every delta MUST be exactly 0 —
     *  anything else is a window-invariance regression (ARCHITECTURE §8.4). Logs the verdict
     *  and the first offending voxel. Run it after any "bit-identical" hot-path refactor. */
    UFUNCTION(CallInEditor, BlueprintCallable, Category = "Live Edit")
    void ValidateDeterminism();

    //=========================================================================
    // EDITOR BRUSH (manual carve/fill from the Details panel, works in PIE)
    //=========================================================================
    // The diff layer only exists while playing, so these buttons act during PIE.

    /** World-space center (Unreal units) for the editor carve/fill buttons below. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Editor Brush")
    FVector EditorBrushCenter = FVector::ZeroVector;

    /** Brush radius in voxels for the editor buttons. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Editor Brush", meta = (ClampMin = "0.5"))
    float EditorBrushRadius = 6.0f;

    /** Brush strength for the editor buttons (sign forced by the button). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Editor Brush", meta = (ClampMin = "0.1"))
    float EditorBrushStrength = 12.0f;

    /** Carve a sphere at EditorBrushCenter using the brush settings above. */
    UFUNCTION(CallInEditor, BlueprintCallable, Category = "Editor Brush")
    void EditorCarveSphere();

    /** Fill a sphere at EditorBrushCenter using the brush settings above. */
    UFUNCTION(CallInEditor, BlueprintCallable, Category = "Editor Brush")
    void EditorFillSphere();

    /** Generation counter — incremented each time we regenerate.
     *  Async tasks carry this value; stale results are discarded. */
    uint32 GenerationEpoch = 0;

    /** Draw the inter-strate passages each frame (lines along the path + endpoint
     *  spheres) so you can see where they spawned and verify they carve. PIE only. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel World|Debug")
    bool bDebugDrawPassages = false;

    //=========================================================================
    // BIOME MAP PREVIEW (bake the XY biome field to a PNG — works without PIE)
    //=========================================================================
    // Tune biome layout (cell size, warp, climate boxes) without flying around: set
    // the strate + window, click Bake, open Saved/BiomePreview.png. Uses a transient
    // generator seeded from VoxelSettings, so it works in the editor with no PIE.

    /** The strate whose Biomes[] + BiomeMapParams to preview. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Biome Preview")
    UVoxelStrateDefinition* BiomePreviewStrate = nullptr;

    /** Width/height of the sampled window in VOXELS (centred on BiomePreviewCenter). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Biome Preview", meta = (ClampMin = "1.0"))
    float BiomePreviewWorldSize = 16000.0f;

    /** Centre of the preview window in voxel coords (X,Y). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Biome Preview")
    FVector2D BiomePreviewCenter = FVector2D::ZeroVector;

    /** Output image resolution (pixels per side). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Biome Preview", meta = (ClampMin = "64", ClampMax = "2048"))
    int32 BiomePreviewResolution = 512;

    /** Which field to visualise: biome debug colours, relief, or moisture. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Biome Preview")
    EBiomePreviewChannel BiomePreviewChannel = EBiomePreviewChannel::Biome;

    /** Bake the selected channel to Saved/BiomePreview.png. */
    UFUNCTION(CallInEditor, BlueprintCallable, Category = "Biome Preview")
    void BakeBiomePreview();

#if WITH_EDITOR
    virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;

    /** Called by FCoreUObjectDelegates::OnObjectModified when ANY UObject is edited.
     *  We filter for UVoxelStrateDefinition changes and regenerate if live edit is on. */
    void OnObjectModifiedInEditor(UObject* ModifiedObject);

    /** Handle to unbind the delegate when EndPlay is called */
    FDelegateHandle OnObjectModifiedHandle;
#endif

    //=========================================================================
    // ACTOR LIFECYCLE
    //=========================================================================

    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    virtual void Tick(float DeltaTime) override;

    //=========================================================================
    // CHUNK MANAGEMENT - YOU IMPLEMENT THESE
    //=========================================================================

    /**
     * Update which chunks are loaded based on a world position.
     *
     * CONCEPT:
     * - Figure out which chunk the position is in (the "center")
     * - Determine which chunks SHOULD exist (within view distance)
     * - Load any chunks that should exist but don't
     * - Unload any chunks that exist but shouldn't
     *
     * @param CenterPosition - Usually the player's position
     */
    void UpdateChunksAroundPosition(const FVector& CenterPosition);

    /**
     * Load a single chunk at the given coordinate.
     *
     * CONCEPT:
     * - Create chunk data
     * - Generate terrain
     * - Build mesh
     * - Create visual component
     *
     * @param ChunkCoord - Which chunk to load
     */
    void LoadTile(const FVoxelTileKey& Tile, bool bHighPriority = false);

    /**
     * Worker-side gen for one tile: classify → GenerateMesh/GenerateSheetMesh → BuildTileStreamSet.
     * Fills Result (no enqueue, no bookkeeping). Called from the async ChunkGen task AND from the
     * synchronous carve path (SyncRemeshTile) — reads Generator/Mesher only, so it's safe on either
     * thread. See LoadTile for how the parameters are derived.
     */
    void GenerateTileResult(const FVoxelTileKey& Tile, const FIntVector& OriginVoxels,
                            int32 Step, int32 Cells, uint32 Epoch, bool bWantCapture,
                            int32 BandVoxLo, int32 BandVoxHi, int32 BandChunkLo, int32 BandChunkHi,
                            bool bSheetTile, int32 SheetChunkZ,
                            int32 HoleMinX, int32 HoleMinY, int32 HoleMaxX, int32 HoleMaxY,
                            FChunkResult& Result);

    /**
     * Game-thread apply for one gen result (shared by ProcessPendingChunks + SyncRemeshTile):
     * epoch check, mark loaded, ingest capture, then either release the tile's component (empty) or
     * ApplyMeshToTile. Returns true iff a VISIBLE mesh was uploaded (counts against the apply budget).
     * Does NOT touch PendingTiles — the caller owns that.
     */
    bool ApplyTileResult(FChunkResult& Result);

    /**
     * Same-frame level-0 re-mesh on the game thread: gen + apply INLINE so a player carve is visible
     * THIS frame (no async round-trip). Used for the tile under the brush centre; neighbours re-mesh
     * async (prioritised) via RemeshDirtyChunks. One full-res tile gen on the game thread — bounded.
     */
    void SyncRemeshTile(const FVoxelTileKey& Tile);

    /**
     * Unload a single chunk.
     *
     * CONCEPT:
     * - Remove and destroy the mesh component
     * - Remove chunk data from storage
     *
     * @param ChunkCoord - Which chunk to unload
     */
    void UnloadTile(const FVoxelTileKey& Tile);

    /**
     * Upload a tile's geometry to its RealtimeMesh component (game thread).
     *
     * The vertex/index buffers (Streams) are already BUILT on the worker (T1.f — see
     * BuildTileStreamSet / FChunkResult), so this only does the game-thread-only work:
     * material resolution, get-or-create the component, CreateSectionGroup(MoveTemp),
     * and per-section config (collision/shadow). Never called for empty tiles.
     *
     * F17 — the streams carry TWO polygroups (0 = ground, 1 = sky-cap ceiling, classified
     * semantically per triangle on the worker): RMC auto-creates one section per non-empty
     * group, so a coarse tile spanning both the terrain and the cap gets BOTH materials
     * (the old whole-tile vote painted the loser with the winner's material).
     *
     * Takes the whole FChunkResult (tile key, streams — consumed/moved —, per-group flags and
     * the strate content band the mesh was cut to; material lookups clamp into that band).
     * Never called for empty results.
     */
    void ApplyMeshToTile(FChunkResult& Result);

    /** Mini-sun lighting (bounded directional). Each frame writes the nearest 4 active orbs' WORLD
     *  positions (+ reach radius in .w) into OrbLightMPC's Orb0..3 vector params; the Directional
     *  Light's Light Function material reads them to mask its contribution into a pool around each
     *  orb. No-op until OrbLightMPC is assigned. Replaces the density-volume raymarch. */
    void UpdateOrbLightMPC();

    /** The Material Parameter Collection (MPC_VoxelOrbs) the orb Light Function reads. Assign in the
     *  AVoxelWorld details. Params expected: Vector Orb0,Orb1,Orb2,Orb3 = (x,y,z, reachRadiusCm). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel|Lighting")
    UMaterialParameterCollection* OrbLightMPC = nullptr;

    /** Build the clipmap desired-tile set (concentric shells) around the player tile.
     *  OutLeavers = les tuiles désirées au crossing PRÉCÉDENT qui ne le sont plus — les seuls
     *  candidats au cull (delta), au lieu de re-scanner TOUTES les tuiles chargées par crossing. */
    void BuildDesiredTiles(const FIntVector& CenterChunkCoord, TArray<FVoxelTileKey>& OutLeavers);

    /** True if a tile's world footprint is still within the outermost clip shell (so a
     *  not-desired loaded tile there is mid-LOD-transition and must wait for its replacement,
     *  vs. one that has left the view entirely and can be culled immediately). */
    bool IsTileInClipRange(const FVoxelTileKey& Tile, const FIntVector& CenterChunkCoord) const;

    //=========================================================================
    // HELPERS
    //=========================================================================

    /** Get the current player position (or zero if no player) */
    FVector GetPlayerPosition() const;

    // (GetLODForChunk / LODToStep / IsChunkInRange removed — dead since the clipmap
    //  streaming replaced the distance-LOD scheme; the level lives in FVoxelTileKey.)

    //=========================================================================
    // ASYNC
    //=========================================================================
    // MPSC: up to MaxConcurrentTasks ChunkGen worker threads Enqueue concurrently,
    // the game thread (ProcessPendingChunks) is the only consumer. The default Spsc
    // mode is NOT safe for multiple producers — concurrent Enqueues race on the tail
    // link and silently drop results, which leaks PendingChunkCoord slots until the
    // budget is exhausted and streaming stalls permanently. Mpsc guards the producer side.
    TQueue<FChunkResult, EQueueMode::Mpsc> ProcessQueue;
    TSet<FVoxelTileKey> PendingTiles;   // tiles with a gen task in flight

    // STRATE CONTENT CUT (see UVoxelSettings::StrateContentCutMinLevel) — the player-strate
    // chunk-Z band coarse tiles are meshed to (MIN/MAX sentinels = no cut, e.g. in the gap).
    // Updated each crossing in UpdateChunksAroundPosition; on change (strate transition) the
    // loaded coarse tiles whose content depends on it are re-queued through BandRemeshQueue
    // (drained by the budgeted submit loop — LoadTile re-gens in place, no visual pop).
    int32 MeshBandChunkLo = MIN_int32;
    int32 MeshBandChunkHi = MAX_int32;
    TSet<FVoxelTileKey> BandRemeshQueue;

    // DIG RESPONSIVENESS — loaded level-0 tiles touched by a player carve/fill that still need an
    // async re-mesh (the neighbours of the synchronously-remeshed centre tile, plus any tile that was
    // mid-gen at carve time). Drained FIRST in the submit loop (ahead of streaming + band) and launched
    // at BackgroundHigh so a dig never waits behind streaming. Unlike the old inline RemeshDirtyChunks,
    // a tile that's in flight is KEPT queued (not dropped) so the stale pre-carve result is corrected
    // once it lands — the source of "the hole shows up a beat late, or not until I move".
    TSet<FVoxelTileKey> DirtyRemeshQueue;

    // F18 — TROU XY de l'anneau feuille (voxels ; Max EXCLUSIF ; sentinelles MAX/MIN = pas de
    // trou) : la zone couverte par les coquilles MC (boîte niveau-MaxClipLevel autour du joueur,
    // rétrécie d'une tuile pour garder un anneau de recouvrement au raccord) est DÉCOUPÉE des
    // feuilles — sinon une feuille partiellement couverte recouvre le terrain proche avec son
    // échantillonnage grossier. Mis à jour par crossing ; changement ⇒ re-queue des feuilles
    // chevauchantes via BandRemeshQueue (re-gen en place).
    int32 SheetHoleMinXVox = MAX_int32, SheetHoleMinYVox = MAX_int32;
    int32 SheetHoleMaxXVox = MIN_int32, SheetHoleMaxYVox = MIN_int32;

    // Set to true during EndPlay — async tasks check this before accessing UObjects
    std::atomic<bool> bShuttingDown{false};

    // Set during editor-driven generation mutations; distinct from teardown/shutdown semantics.
    // Active pendant les mutations de génération lancées par l'éditeur, sans signifier la destruction.
    std::atomic<bool> bGenerationPaused{false};

    // Number of async tasks currently running — EndPlay waits for this to reach 0
    std::atomic<int32> ActiveTaskCount{0};

    FORCEINLINE bool ShouldAbortWork() const
    {
        return bShuttingDown.load(std::memory_order_relaxed)
            || bGenerationPaused.load(std::memory_order_relaxed);
    }

    // Player's level-0 tile coord (= chunk coord). The desired set is rebuilt when this changes.
    FIntVector CurrentCenterChunk = FIntVector::ZeroValue;

    // Non-player streaming anchors (AI now, remote players later — ARCHITECTURE §9.3). BuildDesiredTiles
    // folds each anchor's small level-0 box into the desired set (same DesiredStamped machinery → the
    // delta cull releases an anchor's tiles automatically when it moves away / is unregistered). The
    // desired-set rebuild also fires when any anchor crosses a chunk boundary (UpdateChunksAroundPosition).
    TArray<FVoxelStreamingAnchor> StreamingAnchors;
    // Adds each anchor's level-0 tiles to DesiredSorted/DesiredStamped (dedup vs the player clipmap)
    // and records the ones ONLY a CollisionOnly anchor wants in CollisionOnlyTiles.
    void AddAnchorDesiredTiles();
    // Forces a desired-set rebuild next Tick even if neither the player nor an anchor crossed a tile
    // boundary (set by UnregisterStreamingAnchor so a removed anchor's tiles get culled).
    bool bForceDesiredRebuild = false;

    // §9.4 RENDER-SKIP — level-0 tiles wanted ONLY by CollisionOnly anchors (no player-clipmap / no
    // FullVisual desirer this crossing): they cook collision but are hidden. Rebuilt each crossing in
    // BuildDesiredTiles. `Prev` lets ReconcileAnchorTileVisibility toggle just the DELTA on already-
    // loaded tiles when a tile flips render↔collision-only (player walks toward/away from a cluster),
    // without an O(loaded) scan. Both empty when there are no CollisionOnly anchors → zero cost.
    TSet<FVoxelTileKey> CollisionOnlyTiles;
    TSet<FVoxelTileKey> PrevCollisionOnlyTiles;
    void ReconcileAnchorTileVisibility();

    // --- Streaming work-avoidance (perf) ---
    // The desired tile set only changes when the player crosses a level-0 tile boundary.
    // We cache it and only rebuild/cull/sort on a real move, and go idle once every desired
    // tile is streamed in — so a stationary player costs ~nothing per frame.
    FIntVector LastUpdateCenter = FIntVector(INT32_MAX, INT32_MAX, INT32_MAX);
    bool bAllChunksLoaded = false;
    TArray<FVoxelTileKey> DesiredSorted;   // desired tiles, nearest-first

    // Desired-set membership STAMPÉE : clé → numéro du dernier crossing où la tuile était désirée.
    // BuildDesiredTiles upserte le stamp courant puis balaie la map UNE fois : les entrées à stamp
    // périmé sont les "leavers" (retirées + renvoyées). Le cull ne considère que ces leavers + la
    // TransitionHold — fini le scan O(toutes-les-tuiles-chargées) à chaque crossing (le spike
    // CullTiles ~1.6 ms/crossing de la trace 2026-07-05).
    TMap<FVoxelTileKey, uint32> DesiredStamped;
    uint32 DesiredStamp = 0;
    bool IsDesired(const FVoxelTileKey& T) const
    {
        const uint32* S = DesiredStamped.Find(T);
        return S && *S == DesiredStamp;
    }

    // Tuiles chargées qui ont quitté le desired set mais sont RETENUES (load-before-unload : leur
    // remplacement n'est pas encore complet, ou le backlog a sauté le test de recouvrement).
    // Re-considérées par BUDGET tournant (curseur sur la queue, ~256/crossing) — re-scanner TOUTE
    // la hold par crossing redevient le vieux scan O(loaded) dès que le streaming ne "settle"
    // jamais (mesuré 2.47 ms/crossing en packagé). Garder une tuile plus longtemps est toujours
    // hole-safe. Le set est la MEMBERSHIP autoritaire ; la queue peut contenir des clés périmées
    // (retirées paresseusement au scan). Le "settled cull" reste le filet de sécurité plein-scan.
    TSet<FVoxelTileKey> TransitionHold;
    TArray<FVoxelTileKey> TransitionHoldQueue;
    int32 TransitionHoldCursor = 0;
    void AddToTransitionHold(const FVoxelTileKey& T)
    {
        bool bAlready = false;
        TransitionHold.Add(T, &bAlready);
        if (!bAlready) { TransitionHoldQueue.Add(T); }
    }

    // Tiles approved for removal but whose teardown (component destroy + content actor Destroy())
    // is spread across frames. Unbudgeted, a fast traversal culls a whole shell's worth of tiles in
    // ONE frame → a game-thread teardown spike. Drained by ProcessUnloadQueue (catch-up scaled).
    TSet<FVoxelTileKey> PendingUnload;

    void ProcessPendingChunks();
    void ProcessUnloadQueue();   // budgeted teardown drain (see PendingUnload)

    /**
     * Re-queue loaded chunks for async re-generation + re-meshing.
     * Used after terrain modifications: the old mesh stays visible until
     * the new async result comes back, so there's no visual pop.
     *
     * Chunks that aren't currently loaded are ignored (they'll generate
     * fresh with the diff layer when loaded normally).
     *
     * @param DirtyCoords - Chunk coordinates that need re-meshing
     * @param ExcludeTile - optional level-0 tile already handled synchronously this frame
     *                      (SyncRemeshTile): skipped for the async re-queue, but still marked dirty
     *                      for the density volume.
     */
    void RemeshDirtyChunks(const TArray<FIntVector>& DirtyCoords, const FVoxelTileKey* ExcludeTile = nullptr);
};
