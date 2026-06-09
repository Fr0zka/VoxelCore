// VoxelAtmosphereManager.h
// Whole-strate ambiance: a managed height fog + skylight driven by the strate the
// PLAYER is in, plus persistent ceiling/floor "layer" actors (e.g. seas of clouds)
// that follow the player in XY and hug the strate boundaries.
//
// Unlike the per-chunk content spawner, this is PLAYER-scoped: it updates only when
// the player changes strate, and the layer actors are single persistent instances —
// no spawn/unload churn with chunk streaming.

#pragma once

#include "CoreMinimal.h"
#include "VoxelAtmosphereManager.generated.h"

class UVoxelStrateManager;
class UVoxelStrateDefinition;
class UExponentialHeightFogComponent;
class USkyLightComponent;

UCLASS()
class VOXELFORGE_API UVoxelAtmosphereManager : public UObject
{
    GENERATED_BODY()

public:
    /** Create the managed fog + skylight components on the owner actor. */
    void Initialize(AActor* InOwner, UVoxelStrateManager* InStrateManager);

    /** Call each frame with the player's world position. Cheap: only reacts on strate change. */
    void UpdateForPlayer(const FVector& PlayerWorldPos);

    /** Tear down spawned layer actors + reset (season reset / shutdown). */
    void Reset();

private:
    void ApplyStrate(const UVoxelStrateDefinition* Def);

    TWeakObjectPtr<AActor> Owner;

    UPROPERTY()
    UVoxelStrateManager* StrateManager = nullptr;

    UPROPERTY()
    UExponentialHeightFogComponent* Fog = nullptr;

    UPROPERTY()
    USkyLightComponent* Sky = nullptr;

    // Fully-authored atmosphere BP instance (replaces managed fog/sky when present).
    UPROPERTY()
    AActor* AtmosphereActorInstance = nullptr;

    // Persistent per-strate layer actor instances (single each).
    UPROPERTY()
    AActor* CeilingActor = nullptr;

    UPROPERTY()
    AActor* FloorActor = nullptr;

    // Which strate's atmosphere is currently applied (INT32_MIN = none yet).
    int32 CurrentStrateIndex = INT32_MIN;
};
