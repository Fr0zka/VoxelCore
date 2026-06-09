// VoxelSettings.h
// Settings du plugin VoxelForge.
//
// Data asset unique (assigné sur AVoxelWorld) qui regroupe tous les tuning
// du monde: seed, streaming, LOD, pool de strates, budgets de carving.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "VoxelStrateDefinition.h"
#include "VoxelSettings.generated.h"

UCLASS(BlueprintType)
class UVoxelSettings : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:

	//=========================================================================
	// STREAMING (distance de vue)
	//=========================================================================

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel|Streaming")
	int32 ViewDistanceXY = 16;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel|Streaming")
	int32 ViewDistanceUp = 5;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel|Streaming")
	int32 ViewDistanceDown = 5;

	// Nombre max de tâches de génération/mesh en parallèle.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel|Streaming")
	int32 MaxConcurrentTasks = 16;

	// Nombre max de meshes appliqués par frame (évite les stutters d'upload GPU).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel|Streaming")
	int32 MaxMeshAppliesPerFrame = 4;

	//=========================================================================
	// LOD
	//=========================================================================

	// Distance en chunks pour LOD0 (pleine résolution, step=1).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel|LOD")
	int32 LOD0Distance = 4;

	// Distance en chunks pour LOD1 (demi-résolution, step=2).
	// Au-delà → LOD2 (quart-résolution, step=4).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel|LOD")
	int32 LOD1Distance = 8;

	//=========================================================================
	// RENDERING
	//=========================================================================

	// Matériau global par défaut (peut être override par strate).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel|Rendering")
	UMaterialInterface* VoxelMaterial;

	//=========================================================================
	// STRATES
	//=========================================================================

	// Seed du monde. Pilote toute la génération procédurale.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel|Strates")
	int32 Seed = 0;

	// Numéro de saison (incrementé par ChangeSeed). Purement informatif côté gameplay.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel|Strates")
	int32 CurrentSeason = 1;

	// Pool de définitions de strates disponibles pour l'assignation aléatoire.
	// Chaque définition a sa propre hauteur — les strates ne sont PAS uniformes.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel|Strates")
	TArray<TSoftObjectPtr<UVoxelStrateDefinition>> StratePool;

	// Strates fixées: {index → définition}. Ex: {5, CrystalCaverns} =
	// la strate 5 est toujours CrystalCaverns. Les autres sont tirées du pool.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel|Strates")
	TMap<int32, TSoftObjectPtr<UVoxelStrateDefinition>> FixedStrates;

	// Nombre total de strates à générer vers le bas.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel|Strates", meta = (ClampMin = "1"))
	int32 TotalStrates = 10;

	// Vertical SEPARATION between consecutive strates, in chunks of solid bedrock.
	// 0 = strates touch (just the seals between them). Higher = a thick rock layer
	// between each biome that the player must dig through at (0,0), and that the
	// auto-carved passages tunnel across.
	//   0 → adjacent layers (default) · 1-2 → a real bedrock band · 4+ → deep separation
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel|Strates", meta = (ClampMin = "0", ClampMax = "16"))
	int32 InterStrateGapChunks = 0;

	//=========================================================================
	// SPINE & INTER-STRATE CONNECTIONS  (the (0,0) descent axis)
	//=========================================================================

	// Radius (voxels) of the guaranteed open vertical "landing" column carved at
	// world XY (0,0) in EVERY strate, regardless of archetype. This is the prepared
	// space the player digs THROUGH the seal into when descending. 0 = disabled.
	//   10-14 → cozy shaft (default) · 20+ → wide landing chamber
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel|Spine", meta = (ClampMin = "0.0"))
	float OriginSpineRadius = 14.0f;

	// Auto-open the very top seal at (0,0) so the world starts with a hole to the
	// surface (the entry shaft). Lower boundaries are NOT auto-opened — the player
	// digs those. Disable for a fully sealed world the player must breach from above.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel|Spine")
	bool bOpenSurfaceEntry = true;

	// NOTE: inter-strate tunnel count + shape (style, width, length, placement, worming,
	// spiral, cascade) are now PER-STRATE — see UVoxelStrateDefinition::PassageConfig.
	// Each strate controls its own descent tunnels to the layer below it.

	//=========================================================================
	// CARVING (modifications du joueur)
	//=========================================================================

	// Nombre max d'opérations de modification par saison. 0 = illimité.
	//   500  = édition légère, 2000 = mining modéré, 0 = créatif.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel|Carving",
		meta = (ClampMin = "0"))
	int32 MaxModifications = 0;

	// Rayon max d'un brush (en voxels). Empêche les édits géants.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel|Carving",
		meta = (ClampMin = "1.0"))
	float MaxBrushRadius = 15.0f;

	// Volume total maximum (somme des volumes de brush). 0 = illimité.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel|Carving",
		meta = (ClampMin = "0.0"))
	float MaxTotalVolume = 0.0f;
};
