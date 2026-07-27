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

class AActor;                   // IWYU : pointeur / TWeakObjectPtr seulement / pointer-only
class UVoxelStrateManager;
class UVoxelStrateDefinition;
class UVoxelBiomeDefinition;
class UVoxelGenerator;
class UExponentialHeightFogComponent;
class USkyLightComponent;

UCLASS()
class VOXELFORGE_API UVoxelAtmosphereManager : public UObject
{
    GENERATED_BODY()

public:
    /** Create the managed fog + skylight components on the owner actor. Generator supplies
     *  the dominant-biome query so atmosphere can vary by biome within a strate. */
    void Initialize(AActor* InOwner, UVoxelStrateManager* InStrateManager, UVoxelGenerator* InGenerator);

    /** Call each frame with the player's world position. Cheap: only reacts on strate OR
     *  dominant-biome change. */
    void UpdateForPlayer(const FVector& PlayerWorldPos);

    /** Tear down spawned layer actors + reset (season reset / shutdown). */
    void Reset();

private:
    // Full strate apply: layer actors + atmosphere BP + fog/sky. Biome retints fog/sky.
    void ApplyStrate(const UVoxelStrateDefinition* Def, const UVoxelBiomeDefinition* Biome);
    // Just the managed fog + skylight (biome override beats strate when set).
    void ApplyFogSky(const UVoxelStrateDefinition* Def, const UVoxelBiomeDefinition* Biome);

    TWeakObjectPtr<AActor> Owner;

    UPROPERTY()
    UVoxelStrateManager* StrateManager = nullptr;

    UPROPERTY()
    UVoxelGenerator* Generator = nullptr;

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

    // Dominant biome currently driving fog/sky (identity token for change detection).
    TWeakObjectPtr<const UVoxelBiomeDefinition> CurrentBiome;
};
