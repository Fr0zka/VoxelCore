#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "VoxelStreamingSourceComponent.generated.h"

/**
 * UVoxelStreamingSourceComponent - Attach to any actor to make it stream voxel chunks.
 *
 * **Use Cases:**
 * - Player character: Full streaming with collision and visibility culling
 * - Spectator camera: Streaming without collision, no visibility culling
 * - NPC/Vehicle: Lower priority streaming, collision only nearby
 * - Multiple players: Each player controls their own view frustum
 *
 * **How It Works:**
 * - VoxelWorld scans for all streaming sources each frame
 * - Chunks are generated based on merged priority from all sources
 * - Frustum controls PRIORITY (not hard culling) - chunks in view generated first
 * - Only player sources control visibility culling (SetVisibility on chunks)
 *
 * **Priority System:**
 * - Base priority = PriorityWeight / (distance + 1)
 * - Frustum multiplier: 2x priority for chunks in view cone
 * - Multiple sources: Chunk uses MAX priority from all sources
 * - Result: Chunks in player view load first, chunks behind load later (no pop-in)
 */
UCLASS(ClassGroup=(Voxel), meta=(BlueprintSpawnableComponent))
class VOXELCORE_API UVoxelStreamingSourceComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UVoxelStreamingSourceComponent();

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

public:
	// ==================== STREAMING RADII ====================

	/**
	 * View distance in chunks (horizontal XY radius).
	 * Chunks within this distance will be generated and streamed.
	 * Recommended: Player = 32, NPC = 8, Spectator = 16
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel Streaming|Radii", meta = (ClampMin = "1", ClampMax = "64"))
	int32 ViewDistanceChunks = 32;

	/**
	 * Vertical view distance in chunks (Z radius).
	 * Set to 0 to use ViewDistanceChunks for vertical too.
	 * Recommended: 5-10 for normal gameplay, 0 for unlimited vertical
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel Streaming|Radii", meta = (ClampMin = "0", ClampMax = "32"))
	int32 ViewDistanceChunksZ = 5;

	/**
	 * Collision generation radius in chunks.
	 * Only chunks within this distance will have collision meshes.
	 * Recommended: Player = 2-3, NPC = 1-2, Spectator = 0
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel Streaming|Radii", meta = (ClampMin = "0", ClampMax = "16"))
	int32 CollisionRadius = 2;

	/**
	 * Ambient occlusion radius in chunks.
	 * Only chunks within this distance will have AO baked.
	 * Recommended: Player = 3-5, NPC = 0, Spectator = 2
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel Streaming|Radii", meta = (ClampMin = "0", ClampMax = "16"))
	int32 AORadius = 3;

	// ==================== FRUSTUM PRIORITY ====================

	/**
	 * Use view frustum to PRIORITIZE chunk generation (not hard-cull).
	 * Chunks in view: 2x priority (loaded first)
	 * Chunks outside view: 1x priority (loaded later)
	 * Recommended: true for players, false for spectator cameras
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel Streaming|Frustum")
	bool bUseFrustumPriority = true;

	/**
	 * Horizontal field of view for frustum priority (in degrees).
	 * Larger = more chunks get high priority, smaller = narrower high-priority cone.
	 * Recommended: 120° (player FOV ~90° + 30° margin for turning)
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel Streaming|Frustum",
		meta = (ClampMin = "30.0", ClampMax = "360.0", EditCondition = "bUseFrustumPriority"))
	float FrustumHorizontalFOV = 120.0f;

	/**
	 * Apply frustum priority vertically (prioritize chunks in vertical view cone).
	 * Recommended: false (vertical priority less useful, players often look up/down)
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel Streaming|Frustum",
		meta = (EditCondition = "bUseFrustumPriority"))
	bool bFrustumPriorityVertical = false;

	/**
	 * Vertical field of view for frustum priority (in degrees).
	 * Only used if bFrustumPriorityVertical=true.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel Streaming|Frustum",
		meta = (ClampMin = "30.0", ClampMax = "180.0", EditCondition = "bUseFrustumPriority && bFrustumPriorityVertical"))
	float FrustumVerticalFOV = 100.0f;

	// ==================== PRIORITY & VISIBILITY ====================

	/**
	 * Priority weight for this streaming source (higher = more important).
	 * Multiple sources: Chunk uses MAX priority from all sources.
	 * Recommended: Player = 1.0, NPC = 0.5, Spectator = 0.8
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel Streaming|Priority", meta = (ClampMin = "0.0", ClampMax = "10.0"))
	float PriorityWeight = 1.0f;

	/**
	 * Is this the player's streaming source?
	 * Player sources control visibility culling (SetVisibility on chunks outside frustum).
	 * Only ONE streaming source should be marked as player at a time.
	 * Non-player sources (NPCs, cameras) don't affect visibility, only generation priority.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel Streaming|Priority")
	bool bIsPlayerSource = false;

	/**
	 * Use disk-shaped loading (sphere) instead of cube.
	 * True: Chunks loaded in sphere (smoother, fewer corner chunks)
	 * False: Chunks loaded in cube (more uniform, simpler)
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel Streaming|Shape")
	bool bDiskShapedLoading = true;

	// ==================== PUBLIC API ====================

	/** Get current actor location (cached for performance) */
	FORCEINLINE FVector GetSourceLocation() const { return CachedLocation; }

	/** Get current actor rotation (cached for performance) */
	FORCEINLINE FRotator GetSourceRotation() const { return CachedRotation; }

	/** Get forward vector (cached for performance) */
	FORCEINLINE FVector GetForwardVector() const { return CachedForward; }

	/** Get horizontal forward vector (XY plane only, cached) */
	FORCEINLINE FVector GetForwardVectorXY() const { return CachedForwardXY; }

	/** Check if this source is currently enabled and active */
	FORCEINLINE bool IsActiveSource() const { return bIsActive && GetOwner() != nullptr; }

	/** Enable/disable this streaming source dynamically */
	UFUNCTION(BlueprintCallable, Category = "Voxel Streaming")
	void SetEnabled(bool bEnabled);

private:
	// Cached transform data (updated in TickComponent)
	FVector CachedLocation;
	FRotator CachedRotation;
	FVector CachedForward;
	FVector CachedForwardXY;
	bool bIsActive;

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
};
