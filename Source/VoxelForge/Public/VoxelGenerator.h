// VoxelGenerator.h
// Fournit le champ de densité du monde (positif = solide, négatif = air).
//
// Pipeline density-only: plus de grille de blocs, plus de surface heightfield.
// Le mesher appelle GetDensityAt() par voxel pour reconstruire la géométrie.
// Toute la logique de caves vit dans GetDensityWithParams / GetSlabDensity,
// pilotée par les params de strate récupérés auprès du StrateManager.

#pragma once

#include "CoreMinimal.h"
#include "VoxelTypes.h"
#include "VoxelStrateTypes.h"
#include "VoxelGenerator.generated.h"

// Forward decls (évite les includes transitifs)
class UVoxelSettings;
class UVoxelStrateManager;
class UVoxelDiffLayer;

/**
 * UVoxelGenerator
 *
 * Objet UObject léger — ne stocke pas de données lourdes.
 * Tient juste un pointeur sur les settings (pour le seed) et les services
 * dont il a besoin (StrateManager pour les params par chunk, DiffLayer pour
 * les carves du joueur).
 */
UCLASS(BlueprintType)
class VOXELFORGE_API UVoxelGenerator : public UObject
{
    GENERATED_BODY()

public:
    //=========================================================================
    // SEED (source unique: Settings->Seed)
    //=========================================================================
    // On copie juste le seed au démarrage pour éviter un déréférencement
    // par voxel. Tout le reste vient des params de strate.
    int32 Seed = 0;

    // Radius (voxels) of the guaranteed open vertical landing column carved at world
    // XY (0,0) in every strate — the (0,0) descent spine. Copied from VoxelSettings.
    // 0 disables the spine carve.
    float OriginSpineRadius = 14.0f;

    //=========================================================================
    // SERVICES (injectés par AVoxelWorld::BeginPlay)
    //=========================================================================

    // Manager des strates — fournit les params de cave par chunk.
    // Peut être nullptr: dans ce cas on utilise des params par défaut.
    UPROPERTY()
    const UVoxelStrateManager* StrateManager = nullptr;

    // Diff layer des modifications joueur (carve/fill).
    // Peut être nullptr: dans ce cas pas de modifs appliquées.
    UPROPERTY()
    const UVoxelDiffLayer* DiffLayer = nullptr;

    void SetStrateManager(const UVoxelStrateManager* InManager) { StrateManager = InManager; }
    void SetDiffLayer(const UVoxelDiffLayer* InDiffLayer)       { DiffLayer = InDiffLayer; }

    // Copie le seed depuis les settings. Appelé au démarrage et lors d'un ChangeSeed.
    void InitializeSettings(const UVoxelSettings* Settings);

    //=========================================================================
    // API DE DENSITÉ
    //=========================================================================

    /**
     * Densité combinée (strates + diff du joueur).
     *
     * Convention de sortie (MC): négatif = solide, positif = air.
     * C'est ce que le marching cubes attend comme champ scalaire.
     *
     * @param WorldX, WorldY, WorldZ - coordonnées monde en voxels (pas cm)
     */
    float GetDensityAt(float WorldX, float WorldY, float WorldZ) const;

    /**
     * Densité pour une strate TunnelNetwork (rooms + tunnels + worm noise).
     * Utilisée en interne par GetDensityAt quand la strate est de ce type.
     * Exposée pour permettre des tests isolés avec des params custom.
     */
    float GetDensityWithParams(float WorldX, float WorldY, float WorldZ,
                               const FStrateGenerationParams& Params) const;

    /**
     * Densité pour une strate Slab (FlatPlain / CrystalChamber).
     * Vide horizontal entre un sol bruité et un plafond bruité.
     */
    float GetSlabDensity(float WorldX, float WorldY, float WorldZ,
                         const FSlabGenerationParams& Params) const;

    /**
     * Densité pour une strate Maze — couloirs étroits sur un treillis 3D déterministe.
     * Pas de cache: évalué par voxel sur les quelques cellules voisines.
     */
    float GetMazeDensity(float WorldX, float WorldY, float WorldZ,
                         const FMazeGenerationParams& Params) const;

    /**
     * Densité pour une strate SurfaceWorld — terrain à ciel ouvert (collines,
     * montagnes, plages) sous un plafond solide, avec nappe d'eau optionnelle.
     */
    float GetSurfaceDensity(float WorldX, float WorldY, float WorldZ,
                            const FSurfaceGenerationParams& Params) const;

    /**
     * Densité pour une strate VerticalShafts — puits verticaux pleine hauteur,
     * vires horizontales, et connecteurs horizontaux occasionnels.
     */
    float GetVerticalShaftDensity(float WorldX, float WorldY, float WorldZ,
                                  const FVerticalShaftParams& Params) const;

    /**
     * Densité pour une strate FloatingIslands — masses de terre suspendues dans
     * un grand vide ouvert (îles flottantes), sommets aplatis, dessous rugueux.
     */
    float GetFloatingIslandDensity(float WorldX, float WorldY, float WorldZ,
                                   const FFloatingIslandParams& Params) const;
};
