// VoxelStrateDefinition.h
// Data asset defining everything about a single strate (layer) type.
//
// HOW TO USE:
// -----------
// 1. Right-click in Content Browser → Miscellaneous → Data Asset
// 2. Pick "VoxelStrateDefinition" as the class
// 3. Fill in the fields — cave shape, visuals, content lists, audio, gameplay tags
// 4. Reference it from VoxelSettings (StratePool or FixedStrates)
//
// Each asset is a strate TYPE (e.g., "CrystalCaverns").
// Multiple strate slots can use the same definition.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "GameplayTagContainer.h"
#include "VoxelStrateTypes.h"
#include "VoxelBiomeTypes.h"
#include "Templates/SubclassOf.h"   // IWYU : TSubclassOf<AActor> (Atmosphere/Ceiling/FloorLayerActor)
#include "VoxelStrateDefinition.generated.h"

class UVoxelBiomeDefinition;
// IWYU : tous en pointeur ou en paramètre de TSubclassOf ⇒ déclarations avant suffisantes.
// Le PCH partagé les fournissait ; `FPSemantics = Precise` (AUDIT §C9) nous en prive.
// All pointer-only or TSubclassOf parameters, so forward declarations suffice.
class UMaterialInterface;
class USoundBase;
class AActor;

/**
 * UVoxelStrateDefinition — The content bag for a strate type.
 *
 * This is the central piece of the strate system. Every field here
 * defines what makes a strate unique: how its caves form, what it
 * looks like, what lives in it, how it sounds, and what gameplay
 * rules apply.
 *
 * EXAMPLE STRATES:
 * - "Crystal Caverns": big open caves, blue fog, crystal decorations
 * - "Tight Tunnels": narrow caves, dark, spider creatures
 * - "Flooded Galleries": flat floors, water, diving required
 */
UCLASS(BlueprintType)
class VOXELFORGE_API UVoxelStrateDefinition : public UPrimaryDataAsset
{
    GENERATED_BODY()

public:
    virtual void PostLoad() override;

    //=========================================================================
    // IDENTIFICATION
    //=========================================================================

    // Human-readable name for this strate type (shown in editor and debug)
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Strate")
    FText StrateName;

    // Short description for editor tooltips
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Strate", meta = (MultiLine = true))
    FText StrateDescription;

    //=========================================================================
    // DIMENSIONS
    //=========================================================================

    // How many chunks tall this strate is.
    // Each strate can have a different height!
    // Human-scale defaults use 8 chunks = 64 m. This gives a 20 m+ vertical budget for
    // cathedral rooms, tall shafts, and the surface sky cap without changing the lateral gate.
    // 1 chunk = 32 voxels x 25cm = 8 m, so the maximum 256 chunks is ~= 2 km of vertical strate.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Strate|Dimensions", meta = (ClampMin = "1", ClampMax = "256"))
    int32 StrateHeightInChunks = 8;

    //=========================================================================
    // BOUNDARY TRANSITION
    //=========================================================================
    // Controls how this strate blends with the strate BELOW it.
    // Each boundary between two strates uses the UPPER strate's transition type.
    //
    // Gradient:    Smooth param lerp — no visible boundary (default).
    // Hard:        Abrupt switch — creates a cliff/ledge at the boundary.
    // Interleaved: 3D noise warps the boundary — fingers of one strate
    //              reach into the other for an organic, interlocking look.
    //
    // NOTE: The BOTTOM-MOST strate's transition type is unused (nothing below it).

    // Which transition style to use at this strate's lower boundary
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Strate|Boundary",
        meta = (ToolTip = "How this strate transitions to the strate below it. Gradient = smooth blend, Hard = sharp cliff, Interleaved = noisy interlocking fingers."))
    EVoxelStrateTransition TransitionType = EVoxelStrateTransition::Gradient;

    // How many chunks the transition zone spans (used by Gradient and Interleaved).
    // For Hard transitions this is ignored (effectively 0).
    // Higher = wider, more gradual transition zone.
    //   1 → very narrow transition (almost hard)
    //   2 → moderate (good default)
    //   4 → wide, gentle blend across many chunks
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Strate|Boundary",
        meta = (ClampMin = "1", ClampMax = "8", EditCondition = "TransitionType != EVoxelStrateTransition::Hard"))
    int32 TransitionBlendChunks = 2;

    //=========================================================================
    // GENERATOR TYPE
    //=========================================================================
    // Controls which density strategy this strate uses.
    //
    // TunnelNetwork  → classic rooms + tunnels + worm noise (FStrateGenerationParams below)
    // FlatPlain      → horizontal void between floor and ceiling (FSlabGenerationParams below)
    // CrystalChamber → same as FlatPlain but ceiling has heavy downward formations
    //
    // Changing this shows/hides the relevant parameter sections below.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Strate|Generation")
    ECaveGeneratorType GeneratorType = ECaveGeneratorType::TunnelNetwork;

    /**
     * OPERATOR STACK (experimental) — generate this strate through the composable density operator
     * stack instead of the hardcoded archetype switch. Same world, different machinery.
     *
     * This is the A/B switch for `OPSTACK-PLAN §2.6`'s acceptance bar: flip it, regenerate, and
     * judge on a screenshot that it is recognisably the same place. Both systems coexist
     * indefinitely — the switch is not going away until every archetype is ported.
     *
     * ⚠️ ONLY `Maze` IS PORTED SO FAR. On any other GeneratorType this flag is ignored and the
     * switch runs as before, so setting it is harmless but does nothing yet.
     *
     * ⚠️ Do NOT flip this on a strate mid-session and expect the old and new geometry to agree to
     * the bit — they differ by ~1-2 ULP with ZERO isosurface crossings, so the shape is identical
     * but the floats are not (`AUDIT-2026-07.md §C10`). Regenerate the world after changing it
     * rather than letting old and new tiles sit side by side.
     *
     * Pile d'opérateurs (expérimental) : génère cette strate via la pile composable au lieu du
     * `switch` d'archétype. Seul `Maze` est porté ; ailleurs le drapeau est ignoré.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Strate|Generation",
        meta = (DisplayName = "Use Operator Stack (experimental)"))
    bool bUseOperatorStack = false;

    /**
     * Authoritative per-strate worm switch. Existing assets default to enabled. The manager
     * resolves a disabled strate to WormStrength == 0 before any fused, stack, composer, or
     * interval path sees its parameters, so this is a geometry switch rather than a local CVar.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Strate|Generation",
        meta = (DisplayName = "Enable Worms"))
    bool bEnableWorms = true;

    //=========================================================================
    // TUNNEL NETWORK PARAMS (shown only for TunnelNetwork generator type)
    //=========================================================================
    // These params control how the density field is shaped for chunks in this strate.
    // The generator reads these values to produce unique cave geometry per layer.
    // Only relevant when GeneratorType == TunnelNetwork.

    // Used by TunnelNetwork (rooms+tunnels) AND Underwater (same rock, flooded).
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Strate|Generation",
        meta = (EditCondition = "GeneratorType == ECaveGeneratorType::TunnelNetwork || GeneratorType == ECaveGeneratorType::Underwater"))
    FStrateGenerationParams GenerationParams;

    //=========================================================================
    // SLAB PARAMS (shown only for FlatPlain and CrystalChamber generator types)
    //=========================================================================
    // Controls the floor height, ceiling height, roughness, and column density
    // for open-void strates. Only relevant when GeneratorType is FlatPlain or
    // CrystalChamber. Other generator types ignore these entirely.

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Strate|Generation",
        meta = (EditCondition = "GeneratorType == ECaveGeneratorType::FlatPlain || GeneratorType == ECaveGeneratorType::CrystalChamber"))
    FSlabGenerationParams SlabParams;

    //=========================================================================
    // ARCHETYPE PARAMS (each shown only for its generator type)
    //=========================================================================
    // Tight branching corridors on a 3D lattice.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Strate|Generation",
        meta = (EditCondition = "GeneratorType == ECaveGeneratorType::Maze"))
    FMazeGenerationParams MazeParams;

    // Open-sky terrain: hills/mountains/plains/beaches under a high ceiling, with water.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Strate|Generation",
        meta = (EditCondition = "GeneratorType == ECaveGeneratorType::SurfaceWorld"))
    FSurfaceGenerationParams SurfaceParams;

    // Mostly-vertical shafts with ledges and sparse horizontal links.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Strate|Generation",
        meta = (EditCondition = "GeneratorType == ECaveGeneratorType::VerticalShafts"))
    FVerticalShaftParams VerticalShaftParams;

    // Suspended land masses floating in a large open void.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Strate|Generation",
        meta = (EditCondition = "GeneratorType == ECaveGeneratorType::FloatingIslands"))
    FFloatingIslandParams FloatingIslandParams;

    //=========================================================================
    // BIOMES (vary terrain & content WITHIN this strate — any archetype)
    //=========================================================================
    // Optional. When empty, this strate generates exactly as before (no biome field,
    // bit-identical output). When populated, a deterministic world-XY biome field
    // (warped Voronoi + climate, see FBiomeMapParams) assigns regions; each biome can
    // supply its OWN archetype params (a mini-strate-variant, output-blended for surface)
    // plus its own decorations / atmosphere / water. Surface terrain wired today;
    // content/atmosphere work for any archetype.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Strate|Biomes")
    TArray<UVoxelBiomeDefinition*> Biomes;

    // World-XY biome field tuning (cell size, border warp/blend, climate fields).
    // Only relevant when Biomes is non-empty. Bake AVoxelWorld::BakeBiomePreview to
    // see the resulting map before regenerating the world.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Strate|Biomes")
    FBiomeMapParams BiomeMapParams;

    //=========================================================================
    // DISTURBANCES (the "wow" layer — applies on top of ANY archetype)
    //=========================================================================
    // Chasms, natural bridges and rock ridges layered onto whatever the archetype
    // produces, for surprise and variety. All features default to disabled.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Strate|Disturbances")
    FStrateDisturbanceParams Disturbances;

    //=========================================================================
    // INTER-STRATE PASSAGES (how THIS strate connects DOWN to the next)
    //=========================================================================
    // Auto-carved passages from this strate to the one below it: count, style
    // (walkable sloped tunnel / worm / spiral / cascade), tapered width, length, placement.
    // Every strate has its own finite (0,0) landing room; the passage opens the next room through
    // progression. The non-default styles are retained as exotic, non-walkability-guaranteed
    // descriptors.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Strate|Passages")
    FStratePassageConfig PassageConfig;

    //=========================================================================
    // WATER RENDERING (self-contained, aesthetic — no swim/flow simulation)
    //=========================================================================
    // The water table HEIGHT comes from the active generator params
    // (FSurfaceGenerationParams::WaterLevelRelative or FStrateGenerationParams::
    // WaterLevelRelative). These fields control whether/how it is rendered.

    // Master switch for the water surface in this strate.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Strate|Water")
    bool bHasWater = false;

    // Translucent material applied to the generated water surface planes.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Strate|Water",
        meta = (EditCondition = "bHasWater"))
    UMaterialInterface* WaterMaterial = nullptr;

    // Water tint/colour, available to the material as a parameter if it reads it.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Strate|Water",
        meta = (EditCondition = "bHasWater"))
    FLinearColor WaterColor = FLinearColor(0.02f, 0.08f, 0.12f, 0.8f);

    //=========================================================================
    // TERRAIN OPERATIONS
    //=========================================================================
    // Weighted list of terrain operation data assets to apply in this strate.
    //
    // HOW TO USE:
    //   1. Create terrain op assets (Content → Data Asset → VoxelTerrainOpDefinition)
    //   2. Add entries here — pick an asset and set its Weight
    //   3. Weight scales the op's intensity: 1.0 = as configured, 0.5 = half, 2.0 = double
    //
    // EXAMPLE:
    //   "Crystal Caverns" strate might reference:
    //     - DA_Op_TallColumns (Weight 1.0) → floor-to-ceiling pillars
    //     - DA_Op_WideDomes (Weight 0.8) → cathedral ceilings
    //     - DA_Op_LightScallop (Weight 0.5) → subtle erosion texture
    //
    // The same op asset can be shared across multiple strate definitions.
    // At generation time, these are merged into FStrateGenerationParams
    // before being passed to the density pipeline.

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Strate|Terrain Operations")
    TArray<FStrateTerrainOpEntry> TerrainOperations;

    //=========================================================================
    // VISUALS
    //=========================================================================

    // Material override for this strate (null = use the default VoxelMaterial)
    // Allows each strate to have its own rock/ground look
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Strate|Visuals")
    UMaterialInterface* OverrideMaterial = nullptr;

    // Material for the SKY-CAP CEILING tiles of a surface-like strate (the rocky "night sky" overhead).
    // Null = ceiling uses the same material as the ground (OverrideMaterial / default VoxelMaterial).
    // The ceiling casts no shadow, so under it the ground material reads as flat/bright — give the
    // ceiling its own darker/tinted material (or a tinted instance of the terrain material) to sell the
    // "lit from within, dark rock overhead" look. Applies only to tiles classified as ceiling.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Strate|Visuals")
    UMaterialInterface* CeilingMaterial = nullptr;

    // Fog color for this strate (used by a future fog system or post-process)
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Strate|Visuals")
    FLinearColor FogColor = FLinearColor(0.05f, 0.05f, 0.1f, 1.0f);

    // Fog density (0 = no fog, 1 = very thick)
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Strate|Visuals", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float FogDensity = 0.3f;

    // Ambient light tint (color of the "base" underground light)
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Strate|Visuals")
    FLinearColor AmbientLightColor = FLinearColor(0.1f, 0.1f, 0.15f, 1.0f);

    // Ambient light brightness
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Strate|Visuals", meta = (ClampMin = "0.0"))
    float AmbientLightIntensity = 0.5f;

    // Render the managed height fog volumetrically (softer, light-shaft-friendly, costlier).
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Strate|Visuals")
    bool bVolumetricFog = false;

    //=========================================================================
    // ATMOSPHERE LAYERS (persistent ceiling / floor actors — e.g. seas of clouds)
    //=========================================================================
    // These spawn ONCE when the player enters this strate (NOT per chunk, so they
    // don't churn with streaming) and follow the player in XY while hugging the
    // strate's ceiling / floor. Perfect for the "sky island" look: open space
    // between two cloud seas. Author the look as a Blueprint (a large plane with a
    // cloud/fog material, or a Niagara system). Leave null to disable.
    // This is also how you get "fog emitting from ceiling and floor".

    // FULL atmosphere override: a Blueprint you author with your own ExponentialHeightFog,
    // SkyLight, PostProcessVolume, SkyAtmosphere, etc. — tuned exactly how you want.
    // When set, this REPLACES the simple Fog/Ambient knobs above for this strate (the
    // plugin's managed fog + skylight are disabled so there's no double-up). The plugin
    // spawns one instance when you enter the strate and keeps it anchored to the player.
    // Leave null to use the simple knobs instead.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Strate|Atmosphere")
    TSubclassOf<AActor> AtmosphereActor;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Strate|Atmosphere")
    TSubclassOf<AActor> CeilingLayerActor;

    // Vertical offset (Unreal units) from the strate ceiling. Negative = below the ceiling.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Strate|Atmosphere",
        meta = (EditCondition = "CeilingLayerActor != nullptr"))
    float CeilingLayerZOffset = -200.0f;

    // Rotation applied to the ceiling layer actor each update.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Strate|Atmosphere",
        meta = (EditCondition = "CeilingLayerActor != nullptr"))
    FRotator CeilingLayerRotation = FRotator::ZeroRotator;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Strate|Atmosphere")
    TSubclassOf<AActor> FloorLayerActor;

    // Vertical offset (Unreal units) from the strate floor. Positive = above the floor.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Strate|Atmosphere",
        meta = (EditCondition = "FloorLayerActor != nullptr"))
    float FloorLayerZOffset = 200.0f;

    // Rotation applied to the floor layer actor each update.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Strate|Atmosphere",
        meta = (EditCondition = "FloorLayerActor != nullptr"))
    FRotator FloorLayerRotation = FRotator::ZeroRotator;

    //=========================================================================
    // CONTENT LISTS
    //=========================================================================
    // These arrays define what gets spawned in this strate.
    // Future systems (decoration placer, creature spawner) consume these lists.

    // Decorations: actors placed ON cave surfaces (stalactites, mushrooms, crystals)
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Strate|Content")
    TArray<FStrateDecoration> Decorations;

    // Landmarks: RARE, large, far-visible objects placed on a coarse hash lattice (the underground
    // "mini-suns" etc.). Strate-wide; each entry has its own spacing/biome/placement/transform settings.
    // Cheap at any radius — see FStrateLandmark / §8.5. (NOT part of the per-chunk decoration grid.)
    // Landmarks now cover set-pieces too (AnchorMode HashLattice/PassageMouth + exclusion + orb). See
    // FStrateLandmark / §8.5 (F7).
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Strate|Content")
    TArray<FStrateLandmark> Landmarks;

    // Ambient actors: things floating in cave space (fog volumes, particles, lights)
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Strate|Content")
    TArray<FStrateAmbientActor> AmbientActors;

    // Creatures: enemies/NPCs that spawn in this strate
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Strate|Content")
    TArray<FStrateCreature> Creatures;

    //=========================================================================
    // AUDIO
    //=========================================================================

    // Ambient sound loop for this strate (dripping water, wind, etc.)
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Strate|Audio")
    USoundBase* AmbientSound = nullptr;

    // Background music for this strate
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Strate|Audio")
    USoundBase* Music = nullptr;

    // Music volume multiplier
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Strate|Audio", meta = (ClampMin = "0.0", ClampMax = "2.0"))
    float MusicVolume = 1.0f;

    //=========================================================================
    // GAMEPLAY TAGS
    //=========================================================================
    // Extensible tag system for gameplay rules.
    // Game code checks these tags to apply effects (damage, movement restrictions, etc.)
    //
    // Examples:
    //   "Strate.Hazard.Flooded"          → water everywhere, need diving gear
    //   "Strate.Hazard.Toxic"            → air damages without mask
    //   "Strate.Lighting.Dark"           → almost no ambient light
    //   "Strate.Rule.NeedEquipment.Rope" → climbing areas
    //
    // The plugin doesn't act on these — it just stores them.
    // Your game code reads them and applies the appropriate effects.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Strate|Gameplay")
    FGameplayTagContainer GameplayTags;
};
