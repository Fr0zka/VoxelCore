// VoxelWorld.h
// The main world manager - orchestrates chunk loading, generation, meshing, and rendering

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include <atomic>
#include "VoxelTypes.h"
#include "VoxelChunk.h"
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
    FIntVector ChunkCoord;
    FVoxelChunk Chunk;
    FVoxelMeshData MeshData;
    int32 LODLevel = 0;  // 0=full, 1=half, 2=quarter resolution
    uint32 Epoch = 0;    // Generation epoch — discard if stale
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

    /** When true, VoxelForge spawns & drives its own height fog + skylight + ceiling/floor
     *  layer actors from each strate's settings. Turn OFF if you manage fog/lighting
     *  yourself in the level (avoids a duplicate ExponentialHeightFog). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel World")
    bool bManageAtmosphere = true;

    //=========================================================================
    // CHUNK STORAGE
    //=========================================================================

    /** All currently loaded chunks, keyed by chunk coordinate */
    TMap<FIntVector, FVoxelChunk> Chunks;

    /** Mesh components for each chunk, keyed by chunk coordinate */
    UPROPERTY()
    TMap<FIntVector, URealtimeMeshComponent*> ChunkMeshes;



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

    //=========================================================================
    // STRATE QUERIES (gameplay integration)
    //=========================================================================

    /**
     * Get which strate index a world position is in.
     *
     * @param WorldPosition - Position in world space (Unreal units)
     * @return Strate index (0 = topmost), or -1 if outside all strates
     */
    UFUNCTION(BlueprintPure, Category = "Voxel World|Strate")
    int32 GetStrateAtPosition(FVector WorldPosition) const;

    //=========================================================================
    // LIVE EDIT (debug tuning in PIE)
    //=========================================================================

    /** When true, editing your Strate Definition data assets during PIE
     *  will automatically regenerate all chunks so you see the result live.
     *  Also adds a "Regenerate" button in Details for manual refresh. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Live Edit")
    bool bLiveEditStrates = false;

    /** Click to force-regenerate all chunks right now (useful during PIE). */
    UFUNCTION(CallInEditor, BlueprintCallable, Category = "Live Edit")
    void RegenerateAllChunks();

    /** Re-read ALL of VoxelSettings (strate layout, inter-strate gap, passages, spine)
     *  and rebuild from scratch, then regenerate every chunk. Use this after changing
     *  passage / gap / spine settings so they apply live without restarting PIE —
     *  RegenerateAllChunks alone keeps the existing layout & passages. */
    UFUNCTION(CallInEditor, BlueprintCallable, Category = "Live Edit")
    void RebuildStrates();

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
    void LoadChunk(const FIntVector& ChunkCoord);

    /**
     * Unload a single chunk.
     *
     * CONCEPT:
     * - Remove and destroy the mesh component
     * - Remove chunk data from storage
     *
     * @param ChunkCoord - Which chunk to unload
     */
    void UnloadChunk(const FIntVector& ChunkCoord);

    /**
     * Apply mesh data to a RealtimeMesh component.
     *
     * CONCEPT:
     * - Get or create the mesh component for this chunk
     * - Set the mesh data (vertices, triangles, etc.)
     *
     * @param ChunkCoord - Which chunk this mesh belongs to
     * @param MeshData - The generated mesh data
     */
    void ApplyMeshToChunk(const FIntVector& ChunkCoord, const FVoxelMeshData& MeshData);

    //=========================================================================
    // HELPERS
    //=========================================================================

    /** Get the current player position (or zero if no player) */
    FVector GetPlayerPosition() const;

    /** Check if a chunk coordinate is within view distance of a center chunk */
    bool IsChunkInRange(const FIntVector& ChunkCoord, const FIntVector& CenterChunk) const;

    /**
     * Determine LOD level for a chunk based on its distance from the center.
     *
     * LOD CONCEPT:
     * Chunks close to the player get full resolution (LOD0, Step=1).
     * Chunks further away get coarser resolution (LOD1=Step 2, LOD2=Step 4).
     * This dramatically reduces triangle count for distant terrain without
     * visible quality loss (they're far away!).
     *
     * @param ChunkCoord - The chunk to evaluate
     * @param CenterChunk - The player's current chunk
     * @return LOD level: 0 (full), 1 (half), 2 (quarter)
     */
    int32 GetLODForChunk(const FIntVector& ChunkCoord, const FIntVector& CenterChunk) const;

    /**
     * Convert LOD level to marching cubes step size.
     * LOD0 → Step 1 (every voxel)
     * LOD1 → Step 2 (every 2nd voxel)
     * LOD2 → Step 4 (every 4th voxel)
     */
    static int32 LODToStep(int32 LODLevel);

    //=========================================================================
    // ASYNC
    //=========================================================================
    // MPSC: up to MaxConcurrentTasks ChunkGen worker threads Enqueue concurrently,
    // the game thread (ProcessPendingChunks) is the only consumer. The default Spsc
    // mode is NOT safe for multiple producers — concurrent Enqueues race on the tail
    // link and silently drop results, which leaks PendingChunkCoord slots until the
    // budget is exhausted and streaming stalls permanently. Mpsc guards the producer side.
    TQueue<FChunkResult, EQueueMode::Mpsc> ProcessQueue;
    TSet<FIntVector> PendingChunkCoord;

    // Set to true during EndPlay — async tasks check this before accessing UObjects
    std::atomic<bool> bShuttingDown{false};

    // Number of async tasks currently running — EndPlay waits for this to reach 0
    std::atomic<int32> ActiveTaskCount{0};

    // Last known player chunk coord — used by LoadChunk to compute LOD
    FIntVector CurrentCenterChunk = FIntVector::ZeroValue;

    // Track current LOD per loaded chunk (for LOD transitions)
    TMap<FIntVector, int32> ChunkLODs;

    // --- Streaming work-avoidance (perf) ---
    // The desired chunk set only changes when the player crosses a chunk boundary.
    // We cache it and only rebuild/cull/sort on a real move, and go idle once every
    // desired chunk is streamed in — so a stationary player costs ~nothing per frame.
    FIntVector LastUpdateCenter = FIntVector(INT32_MAX, INT32_MAX, INT32_MAX);
    bool bAllChunksLoaded = false;
    TArray<FIntVector> DesiredSorted;   // desired coords, nearest-first
    TSet<FIntVector> DesiredSet;        // O(1) membership for the cull pass

    void ProcessPendingChunks();

    /**
     * Re-queue loaded chunks for async re-generation + re-meshing.
     * Used after terrain modifications: the old mesh stays visible until
     * the new async result comes back, so there's no visual pop.
     *
     * Chunks that aren't currently loaded are ignored (they'll generate
     * fresh with the diff layer when loaded normally).
     *
     * @param DirtyCoords - Chunk coordinates that need re-meshing
     */
    void RemeshDirtyChunks(const TArray<FIntVector>& DirtyCoords);
};
