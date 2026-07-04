// VoxelStrateTypes.h
// Shared structs and enums for the strate (layer) system.
//
// WHAT ARE STRATES?
// -----------------
// The Depths world is divided into vertical layers called "strates".
// Each strate is radically different — unique cave shapes, decorations,
// creatures, materials, and even gameplay rules (flooded, toxic, etc.).
//
// This file defines the data structures that carry strate information
// between systems (generator, world manager, future decoration/creature spawners).

#pragma once

#include "CoreMinimal.h"
#include "GameplayTagContainer.h"
#include "VoxelStrateTypes.generated.h"

class UVoxelBiomeDefinition;   // FStrateLandmark::RequiredBiome (optional per-landmark biome filter)

//=============================================================================
// ENUMS
//=============================================================================

/**
 * EVoxelPassageType — The shape/style of an inter-strate passage.
 *
 * Each passage connecting two adjacent strates is randomly assigned one of
 * these types during GeneratePassages(). The type determines the geometry
 * of the carved tunnel (control point layout, radius, overall feel).
 *
 * Types:
 *   SlopedTunnel   — Angled bore with a horizontal midpoint offset (default/legacy).
 *   VerticalShaft  — Straight or near-straight drop between strates.
 *   SpiralDescent  — Helical corkscrew path winding downward.
 *   CascadingDrops — Series of short vertical pits connected by horizontal ledges.
 *   CrackCrevice   — Narrow fracture passage, tight squeeze through rock.
 */
UENUM(BlueprintType)
enum class EVoxelPassageType : uint8
{
    // Angled bore connecting two layers with a horizontal midpoint offset.
    // Two capsule segments: Upper→Mid, Mid→Lower. Comfortable radius (~5 voxels).
    // This is the original passage type — feels like a natural sloped tunnel.
    SlopedTunnel    UMETA(DisplayName = "Sloped Tunnel (angled bore)"),

    // Straight vertical drop between strates. Same XY for upper and lower points.
    // Wider radius (~7-8 voxels) to feel like a natural shaft or sinkhole.
    // Players drop or climb directly between strates.
    VerticalShaft   UMETA(DisplayName = "Vertical Shaft (straight drop)"),

    // Helical corkscrew path with 6-10 control points spiraling downward.
    // Narrower radius (~4 voxels). The spiral gives a sense of descending
    // through layers of rock, and the tight turns create interesting navigation.
    SpiralDescent   UMETA(DisplayName = "Spiral Descent (corkscrew)"),

    // Alternating horizontal ledges and short vertical drops.
    // 3-5 drop segments, each ~8-12 voxels deep with ~10-voxel horizontal runs.
    // Medium radius (~5 voxels). Feels like a natural cascading waterfall path.
    CascadingDrops  UMETA(DisplayName = "Cascading Drops (ledge series)"),

    // Narrow fracture passage — like squeezing through a crack in the rock.
    // Similar shape to SlopedTunnel but much narrower radius (~2-3 voxels)
    // and more pronounced horizontal offset at the midpoint.
    CrackCrevice    UMETA(DisplayName = "Crack/Crevice (narrow fracture)")
};

/**
 * ESurfaceType — Where on a cave surface a decoration can be placed.
 *
 * Used by the decoration system to filter placement locations based
 * on the surface normal direction at each point.
 *
 * Floor:   Normal pointing up (dot with Z > 0.7)
 * Wall:    Normal roughly horizontal
 * Ceiling: Normal pointing down (dot with Z < -0.7)
 * Any:     No restriction — place on any surface
 */
UENUM(BlueprintType)
enum class ESurfaceType : uint8
{
    Floor    UMETA(DisplayName = "Floor (normal up)"),
    Wall     UMETA(DisplayName = "Wall (normal horizontal)"),
    Ceiling  UMETA(DisplayName = "Ceiling (normal down)"),
    Any      UMETA(DisplayName = "Any surface")
};

/**
 * EDecoStreamTier — Which of the two decoration streaming grids an entry uses (§8.5).
 *
 * Decorations stream on a fixed world XY grid by distance (no LOD pop). To keep that flicker-free,
 * the STREAM RADIUS is a property of the GRID, never of an entry — mixing radii inside one grid would
 * force a region to re-stream in place when the player crosses an entry's radius (the old tier system's
 * flicker bug). So there are exactly two grids, and an entry just PICKS one:
 *
 *   Far  — full radius (VoxelSettings::DecorationRadiusChunks) + COARSE column spacing
 *          (DecorationFarSpacingVoxels). The coarse grid is what makes a RARE prop you want visible at
 *          every distance cheap: the worker ray-march cost scales with column count, and a sparse prop
 *          does not need the dense near grid. Default — and the far spacing defaults to the fine value,
 *          so existing assets are byte-identical until you opt in to a coarser far grid. Trees, landmarks.
 *
 *   Near  — short radius (DecorationNearRadiusChunks) + FINE column spacing (DecorationSpacingVoxels).
 *          For dense groundcover (grass, small clutter) that only needs to exist near the player: keeping
 *          it out of the far regions saves their HISM cluster-tree build + instance memory. Pair with the
 *          per-entry CullDistance (GPU draw bound) for the full picture.
 */
UENUM(BlueprintType)
enum class EDecoStreamTier : uint8
{
    Far  UMETA(DisplayName = "Far (full radius, coarse grid — trees/landmarks/rare props)"),
    Near UMETA(DisplayName = "Near (short radius, fine grid — dense groundcover)")
};

//=============================================================================
// NOISE TYPE
//=============================================================================

/**
 * EVoxelNoiseType — Which noise function to use for cave surface roughness.
 *
 * Different noise types give radically different visual character to cave walls:
 * - fBM: smooth, organic, blobby surfaces (default, works everywhere)
 * - Ridged: sharp ridge-like features, great for craggy cliffs and tunnels
 * - Mixed: blend of both — ridged structure with fBM softness
 */
UENUM(BlueprintType)
enum class EVoxelNoiseType : uint8
{
    // Standard fractional Brownian motion (layered Perlin).
    // Smooth, organic look. Blobby hills and valleys on cave walls.
    FBM       UMETA(DisplayName = "fBM (smooth, organic)"),

    // Ridged multifractal — folds the noise at zero-crossings,
    // creating sharp ridge-like features. Looks like craggy rock,
    // natural corridors, and erosion patterns.
    Ridged    UMETA(DisplayName = "Ridged (sharp, craggy)"),

    // 50/50 blend of fBM and Ridged — ridged structure softened
    // by fBM. Good default for "interesting but not too alien."
    Mixed     UMETA(DisplayName = "Mixed (fBM + Ridged)"),

    // Worley/cellular noise — distance-to-nearest-feature-point.
    // Creates rounded, cell-like patterns: bubble walls, grotto
    // pockets, honeycomb textures. Great for alien or crystalline strates.
    Cellular  UMETA(DisplayName = "Cellular (grotto, bubbles)")
};

//=============================================================================
// STRATE TRANSITION TYPE
//=============================================================================

/**
 * ECaveGeneratorType — Which density strategy this strate uses.
 *
 * Each strate is its own "tiny world" and can use a fundamentally different
 * density function. The generator branches on this value before building
 * the density field for a chunk.
 *
 * TunnelNetwork:  The default. Room-and-corridor SDF graph + worm tunnels.
 *                 Classic cave networks with interconnected passages and chambers.
 *                 Uses FStrateGenerationParams (all the room/tunnel/worm settings).
 *
 * FlatPlain:      Underground open plains. A horizontal void between a flat(ish)
 *                 floor and a gently roughened ceiling. Scattered columns.
 *                 Looks like a massive underground prairie — flat ground, low
 *                 ceiling, occasional stone pillars. Uses FSlabGenerationParams.
 *
 * CrystalChamber: Like FlatPlain but with a heavily roughened ceiling that creates
 *                 crystal-like formations hanging downward. Combine with crystal
 *                 decoration actors on the ceiling for the "fake sky" effect.
 *                 Uses FSlabGenerationParams (same as FlatPlain, different defaults).
 */
UENUM(BlueprintType)
enum class ECaveGeneratorType : uint8
{
    // Classic cave network: room SDF + tunnels + worm noise. Uses FStrateGenerationParams.
    TunnelNetwork   UMETA(DisplayName = "Tunnel Network (rooms + corridors)"),

    // Horizontal slab void: flat floor + gentle ceiling + scattered columns. Uses FSlabGenerationParams.
    FlatPlain       UMETA(DisplayName = "Flat Plain (underground prairie)"),

    // Horizontal slab void: flat floor + heavily roughened ceiling (crystal formations). Uses FSlabGenerationParams.
    CrystalChamber  UMETA(DisplayName = "Crystal Chamber (formations hanging from ceiling)"),

    // Tight branching corridors on a deterministic 3D lattice. Uses FMazeGenerationParams.
    Maze            UMETA(DisplayName = "Maze (tight branching tunnels)"),

    // Open-sky terrain: hills/mountains/plains/beaches under a high solid ceiling,
    // lit by placed skylight actors, with an optional water table. Uses FSurfaceGenerationParams.
    SurfaceWorld    UMETA(DisplayName = "Surface World (skylight terrain: hills, rivers, beaches)"),

    // Mostly-vertical cave system: tall shafts with ledges + sparse horizontal links. Uses FVerticalShaftParams.
    VerticalShafts  UMETA(DisplayName = "Vertical Shafts (mostly-vertical cave system)"),

    // Suspended land masses in a large open void — "floating islands". Uses FFloatingIslandParams.
    FloatingIslands UMETA(DisplayName = "Floating Islands (suspended land in open void)"),

    // Flooded cavern network: TunnelNetwork rock with a high water table. Uses FStrateGenerationParams + water.
    Underwater      UMETA(DisplayName = "Underwater (flooded cavern system)"),
};

/**
 * EVoxelStrateTransition — How two adjacent strates blend at their shared boundary.
 *
 * Each strate boundary can use a different transition style. The transition type
 * is defined on the UPPER strate's definition and controls the boundary between
 * itself and the strate below it.
 *
 * Gradient:    Smooth linear interpolation of generation params across BlendChunks.
 *              This is the classic approach — no seam visible, but also no dramatic
 *              geological feature at the boundary. Best for similar-looking strates.
 *
 * Hard:        No blending at all. The strate's params apply right up to the boundary,
 *              then instantly switch to the neighbor's params. The abrupt change in
 *              density and cave shape naturally creates a cliff, ledge, or material
 *              discontinuity at the boundary. Best for strates that should feel like
 *              distinct geological layers with a visible dividing line.
 *
 * Interleaved: The boundary position is warped by a 3D noise field, so "fingers"
 *              of one strate reach into the other. Some XY columns transition early,
 *              others late, creating an irregular, interlocking boundary. The warp
 *              amplitude is ~2 chunks, and Z frequency is lower than XY to produce
 *              horizontal finger-like intrusions. Best for strates that should feel
 *              like they grew into each other over geological time.
 */
UENUM(BlueprintType)
enum class EVoxelStrateTransition : uint8
{
    // Smooth linear interpolation of all generation params across the blend zone.
    // No visible boundary — params change gradually over BlendChunks distance.
    Gradient    UMETA(DisplayName = "Gradient (smooth blend)"),

    // No blending — params switch instantly at the boundary.
    // Creates natural cliffs, ledges, or material discontinuities.
    Hard        UMETA(DisplayName = "Hard (sharp boundary with cliff)"),

    // 3D noise warps the boundary position per XY column.
    // Creates finger-like intrusions of one strate into the other.
    Interleaved UMETA(DisplayName = "Interleaved (domain-warped fingers)")
};

//=============================================================================
// GENERATION PARAMS
//=============================================================================

/**
 * FStrateGenerationParams — Cave generation parameters for one strate.
 *
 * Extracted from a UVoxelStrateDefinition at runtime.
 * At strate boundaries, two sets of params are blended (Lerp) so there's
 * no hard visual seam between strates.
 *
 * The generator reads these instead of its own member variables when the
 * strate system is active.
 */
USTRUCT(BlueprintType)
struct VOXELFORGE_API FStrateGenerationParams
{
    GENERATED_BODY()

    // ----- Rock density -----

    // How solid the rock is BEFORE any carving.
    // The entire strate volume starts at this density (all solid rock).
    // Worm tunnels and cavern noise then subtract from it to create air.
    //
    // Higher = more rock survives carving = fewer, thinner caves.
    // The carving systems must overcome this value to create air (density < 0).
    //   5-6   → moderate rock, lots of tunnels get through
    //   8-10  → thick rock, only the strongest carving creates caves (good default)
    //   12+   → very dense, barely any caves
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rock")
    float BaseDensity = 8.0f;

    // Vertical stretch factor for ALL noise (worms + caverns).
    // Scales the Z coordinate before noise sampling:
    //   >1 = tall galleries, vertical shafts
    //   <1 = flat chambers, horizontal tunnels
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rock")
    float VerticalScale = 1.0f;

    // ===== WORM TUNNELS (primary cave structure) =====
    //
    // Worm tunnels are the main way caves form. The technique:
    //   Sample TWO 3D noise fields at the same position.
    //   Take abs() of each → both form "sheets" near their zero-crossings.
    //   Where both sheets intersect → a thin, winding 1D curve → the tunnel.
    //
    //   Worm = abs(Noise1) + abs(Noise2)
    //   Where Worm < WormThreshold → air (tunnel)
    //
    // This naturally creates connected, winding tunnel networks.
    // The tunnels branch, merge, and snake through the rock organically.

    // How tightly the tunnels wind. Lower = long gentle curves, higher = tight turns.
    // This controls the SCALE of tunnels — how far apart they are and how long
    // each straight section is before curving.
    //   0.008 → very long winding tunnels spanning many chunks
    //   0.015 → natural cave tunnels (good default)
    //   0.03  → tighter turns, shorter sections
    //   0.06+ → chaotic maze (hard to navigate)
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Worm Tunnels")
    float WormFrequency = 0.015f;

    // Bias tunnels toward horizontal orientation.
    // Multiplies the Z component of worm noise frequency, making the noise
    // change faster vertically. The result: tunnel paths prefer to stay
    // at similar Z levels, giving natural floors and ceilings.
    //   1.0 → no bias (tunnels go in all directions, including vertical shafts)
    //   3.0 → mostly horizontal tunnels with gentle slopes (good default)
    //   5.0 → very flat tunnels, almost no vertical movement
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Worm Tunnels", meta = (ClampMin = "1.0", ClampMax = "10.0"))
    float WormHorizontalBias = 3.0f;

    // Tunnel width. Lower = thinner tunnels, higher = wider passages.
    // This is the threshold below which the worm noise creates air.
    // KEEP THIS SMALL — it controls what fraction of volume becomes tunnel.
    //   0.03 → very narrow crawlspaces
    //   0.06 → walking-width tunnels (good default)
    //   0.10 → wide corridors
    //   0.15+ → WARNING: starts creating too much air
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Worm Tunnels", meta = (ClampMin = "0.01", ClampMax = "0.5"))
    float WormThreshold = 0.06f;

    // How aggressively worm tunnels carve through rock.
    // At the tunnel center (WormValue ≈ 0), the full WormStrength is applied.
    // Must exceed BaseDensity to create air. With BaseDensity=8, you need
    // WormStrength > 8 for tunnels to fully punch through.
    //   9-10  → tunnels just barely pierce through BaseDensity=8
    //   12    → clean tunnels with smooth walls
    //   15+   → very wide tunnel profile
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Worm Tunnels")
    float WormStrength = 10.0f;

    // Worms only carve within this distance (voxels) of the room/tunnel network, fading
    // smoothly to zero at the edge. Keeps worms as organic braids and shortcuts that HUG
    // the cave system instead of spraying disconnected noise pockets through the whole
    // strate (the far-field "confetti"). 0 = unlimited (legacy unmasked behaviour).
    //   16  → tight braiding right along rooms/tunnels
    //   24  → braids + short noodle shortcuts (good default)
    //   48+ → loose, wandering side-passages
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Worm Tunnels", meta = (ClampMin = "0.0"))
    float WormNetworkRange = 24.0f;

    // ===== CAVE MORPHOLOGY (room-and-corridor) =====
    //
    // Cave shape is defined by SDF (Signed Distance Field) primitives:
    //   - ROOMS: ellipsoid voids placed on a hash grid
    //   - TUNNELS: capsule corridors connecting nearby rooms
    // Combined with smooth union for organic, rounded junctions.
    //
    // This replaces the old 2D cave slab system. Instead of a flat
    // heightfield cave, you get actual rooms connected by corridors —
    // explorable spatial structure with clear "this is a room" / "this
    // is a corridor" readability.

    // Distance between room grid cells (in voxels).
    // This controls room SPACING, not size. Larger = rooms further apart.
    //   40  → dense room network, rooms nearly touching
    //   80  → moderate density, corridors between rooms (good default)
    //   120 → sparse rooms, long corridors
    //   200+→ isolated chambers with long tunnel treks
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cave Morphology|Rooms")
    float RoomSpacing = 80.0f;

    // Probability of a room existing in each grid cell (0-1).
    // Not every cell gets a room — this controls how many are filled.
    //   0.2 → sparse — only 20% of cells have rooms (many empty areas)
    //   0.35→ moderate (good default)
    //   0.5 → dense — half of all cells have rooms
    //   0.7+→ very crowded, almost solid cave network
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cave Morphology|Rooms", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float RoomDensity = 0.35f;

    // Smallest possible room radius (in voxels).
    // Small rooms feel like alcoves or nooks.
    //   5-8  → small alcoves
    //   10-15→ natural small chambers (good default)
    //   20+  → even "small" rooms are spacious
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cave Morphology|Rooms")
    float MinRoomRadius = 10.0f;

    // Largest possible room radius (in voxels).
    // Large rooms are cathedral chambers.
    //   15-20→ moderate rooms
    //   25-35→ large caverns (good default)
    //   50+  → massive cathedral spaces
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cave Morphology|Rooms")
    float MaxRoomRadius = 30.0f;

    // How vertically squished rooms are.
    // 1.0 = perfect sphere (tall as wide).
    // Lower = flatter, more horizontal chambers.
    //   0.2-0.3 → very flat caverns (strate-like, wide and low)
    //   0.4-0.5 → natural cave chambers (good default)
    //   0.7-1.0 → tall, cathedral-like rooms
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cave Morphology|Rooms", meta = (ClampMin = "0.1", ClampMax = "1.0"))
    float RoomHeightRatio = 0.4f;

    // Per-room floor flatness range. Each room hash-rolls a value in [Min, Max].
    //   FloorCutZ = RoomCenter.Z - RoomRadiusZ * roll
    //
    // The floor is a soft intersection (SmoothMax) so tunnels/pits pass through
    // without a hard seam — the floor rounds off naturally near openings.
    //
    //   1.0 / 1.0  → all rooms are full ellipsoid bubbles (no flat floor)
    //   0.7 / 1.0  → mix: some rooms flat, some bubbles
    //   0.5 / 0.8  → all rooms have a flat floor, varying how low it sits
    //   0.0        → floor at room center — only the top dome is carved
    //
    // Combine with RoomHeightRatio = 1.0 and Min~0.7 to get tall dome rooms
    // with navigable flat floors.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cave Morphology|Rooms",
        meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float RoomFloorCutMin = 1.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cave Morphology|Rooms",
        meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float RoomFloorCutMax = 1.0f;

    // Large-scale floor undulation — how many voxels the floor plane rises and falls.
    // This is NOT surface roughness (which adds rock texture). This shifts the entire
    // floor height across the room, creating genuine terrain: hills, shallow basins,
    // gradual ramps. A large room with FloorReliefStrength=5 has floor variation of ±5
    // voxels — navigable, but with noticeable topography.
    //
    // Only meaningful when RoomFloorCutMin < 1.0 (flat floor mode active).
    //
    //   0     → perfectly flat floor (default — use surface roughness for texture)
    //   2-4   → subtle floor variation, barely perceptible gradient
    //   5-10  → clear hills and basins — interesting to walk across
    //   15+   → dramatic terrain — slopes and drops within the room
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cave Morphology|Rooms",
        meta = (ClampMin = "0.0", EditCondition = "RoomFloorCutMin < 1.0"))
    float FloorReliefStrength = 0.0f;

    // How wide the floor hills are. Lower = broader, gentler undulations.
    //   0.005 → very broad (one hill spans the whole room)
    //   0.015 → moderate rolling terrain (good default)
    //   0.04  → tighter, more frequent hills
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cave Morphology|Rooms",
        meta = (ClampMin = "0.001", EditCondition = "RoomFloorCutMin < 1.0"))
    float FloorReliefFrequency = 0.015f;

    // How much variety in room shapes (0 = all ellipsoids, 1 = full variety).
    // At 0: every room is a smooth ellipsoid (uniform, organic caves).
    // At 0.5: some rooms become angular (rounded boxes) or elongated (capsules).
    // At 1.0: maximum variety — ellipsoids, angular chambers, and elongated halls.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cave Morphology|Rooms", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float RoomShapeVariety = 0.5f;

    // ===== ORIGIN ROOM (guaranteed hub at world center) =====
    //
    // A guaranteed large room at (0, 0) in the center of each strate.
    // This is where the cave network originates — the designer can later
    // place the elevator, rest area, or other key features here.
    // All nearby hash rooms will have tunnels forced to connect to it,
    // making it the natural hub of the strate's cave network.
    //   0   → disabled (no guaranteed origin room)
    //   15  → moderate hub room
    //   20  → large central chamber (good default)
    //   30+ → massive starting cavern
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cave Morphology|Rooms")
    float OriginRoomRadius = 20.0f;

    // Maximum number of backbone tunnels that can force-connect to the origin room.
    // Without a limit, every room in the search area that picks origin as its nearest
    // neighbor gets a guaranteed tunnel — with large spacing this can create 8-10
    // tunnels all radiating from origin, overwhelming it visually.
    //   0   → no limit (legacy behavior — every room can backbone to origin)
    //   3-4 → natural hub with a few major exits (good default)
    //   6+  → busy hub
    // Rooms beyond the cap can still connect via TunnelDensity random chance,
    // so connectivity isn't broken — only the "guaranteed" aspect is limited.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cave Morphology|Rooms",
        meta = (ClampMin = "0", ClampMax = "12"))
    int32 OriginRoomMaxConnections = 4;

    // ===== TUNNELS (connecting corridors between rooms) =====

    // Smallest tunnel radius (in voxels). Creates tight squeeze corridors.
    //   2-3 → claustrophobic crawlspaces
    //   3-4 → tight but passable (good default)
    //   5+  → even "narrow" tunnels are comfortable
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cave Morphology|Tunnels")
    float TunnelMinRadius = 3.0f;

    // Largest tunnel radius (in voxels). Creates wide passages.
    //   5-6 → comfortable walking tunnels
    //   7-8 → wide passages (good default)
    //   10+ → practically small rooms
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cave Morphology|Tunnels")
    float TunnelMaxRadius = 7.0f;

    // Probability that two nearby rooms are connected by a tunnel (0-1).
    //   0.2 → sparse connections, mostly dead-end rooms
    //   0.4 → moderate connectivity (good default)
    //   0.8+→ dense network, nearly everything connected
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cave Morphology|Tunnels", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float TunnelDensity = 0.4f;

    // Maximum tunnel length (in voxels). Rooms further apart won't connect.
    //   100 → only close neighbors
    //   200 → moderate reach (good default)
    //   400+→ long-range connections possible
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cave Morphology|Tunnels")
    float MaxTunnelLength = 200.0f;

    // How much tunnel paths curve (in voxels of sideways displacement).
    // Without this, tunnels are straight lines between rooms.
    // This adds a hash-derived midpoint offset to each tunnel, creating
    // unique curves — some bend left, some right, some are nearly straight.
    // Capped at 25% of tunnel length to prevent kinky short tunnels.
    //   0   → perfectly straight tunnels (artificial look)
    //   8-12→ gentle natural curves
    //   15  → clearly winding passages (good default)
    //   25+ → very curvy, meandering corridors
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cave Morphology|Tunnels")
    float TunnelWarpStrength = 15.0f;

    // Preference for horizontal connections over vertical (0-1).
    // Real caves are mostly horizontal — vertical connections are rarer.
    // This penalizes Z-separation when deciding which rooms to connect.
    //   0.0 → no preference (connects freely in all directions)
    //   0.5 → moderate horizontal preference (good default)
    //   1.0 → strongly horizontal — vertical connections very unlikely
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cave Morphology|Tunnels", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float TunnelHorizontalBias = 0.5f;

    // Topology of the guaranteed tunnel network.
    // true  → each room links to its best candidate among rooms CLOSER to (0,0): the whole
    //         network becomes a tree rooted at the origin room — every cave is reachable
    //         from the spine hub, tunnels flow inward like tributaries (intentional descent
    //         structure). TunnelDensity still adds loops on top.
    // false → legacy nearest-neighbor pairing: organic scattered clusters, but connectivity
    //         between clusters is NOT guaranteed (isolated pockets are common).
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cave Morphology|Tunnels")
    bool bTunnelsFlowTowardOrigin = true;

    // How much tunnel endpoints shift up/down within rooms (0-1).
    // Fraction of the room's vertical radius. Each tunnel endpoint gets
    // a hash-derived Z offset, so tunnels enter rooms at different heights.
    // Combined with terrain ops (cliffs, terraces), this creates multi-level rooms
    // where one tunnel exits at the upper ledge and another at the lower floor.
    //   0.0 → all tunnels connect at room center height
    //   0.5 → endpoints range from -50% to +50% of room height (good default)
    //   1.0 → full range — tunnels can enter near floor or ceiling
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cave Morphology|Tunnels", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float TunnelEndpointZOffset = 0.5f;

    // ===== SDF BLEND (junction smoothness) =====

    // Smooth union blend radius. Controls how rounded the junctions
    // are where rooms meet tunnels (or rooms meet rooms).
    //   1-2 → sharp, angular junctions
    //   3-5 → natural rounded junctions (good default)
    //   8+  → very blobby, organic blending
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cave Morphology|Blend")
    float SDFBlendRadius = 4.0f;

    // ===== CAVE WARP (large-scale skeleton distortion) =====
    //
    // Warps the world coordinates BEFORE the SDF is evaluated.
    // This is THE key technique that turns graph-like caves into organic shapes:
    // rooms become irregular blobs, tunnels become winding passages.
    //
    // Without this, the SDF skeleton is a clean graph of geometric primitives.
    // With this, the entire field bends and flows — like real geological forces
    // pushed the rock around over millions of years.
    //
    // Two octaves are applied internally:
    //   - Large-scale (CaveWarpFrequency): bends whole rooms/tunnels gently
    //   - Medium-scale (3x frequency, 0.3x strength): adds irregularity to walls

    // How far the cave shapes shift (in voxels).
    // This controls the overall organic distortion of the cave skeleton.
    //   0   → disabled — pristine geometric rooms and straight tunnels
    //   4-6 → subtle natural variation (rooms slightly irregular)
    //   8-12→ clearly organic caves, tunnels visibly curve (good default)
    //   16+ → heavily distorted, surreal cave shapes
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cave Morphology|Cave Warp")
    float CaveWarpStrength = 8.0f;

    // Frequency of the warp noise. Lower = broader, smoother bends.
    // This controls the SCALE of the distortion.
    //   0.008 → very broad sweeping curves (whole strate-scale)
    //   0.015 → natural cave bending — one curve every ~2 chunks (good default)
    //   0.03  → tighter curves, more chaotic layout
    //   0.06+ → extreme — small-scale jittering (usually too noisy)
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cave Morphology|Cave Warp")
    float CaveWarpFrequency = 0.015f;

    // ===== SURFACE ROUGHNESS (rocky, craggy walls) =====
    //
    // 3D noise applied near cave surfaces to break up the smooth SDF shapes.
    // Creates overhangs, ledges, rocky protrusions, bumps, and cracks.
    // Without this, rooms are perfect ellipsoids. With this, they're rock.

    // How strong the roughness is (in voxels of displacement).
    //   0   → perfectly smooth SDF shapes
    //   2-3 → subtle rocky texture
    //   4-6 → natural rocky cave surfaces (good default)
    //   8+  → very rough, jagged rock
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cave Morphology|Roughness")
    float SurfaceRoughness = 5.0f;

    // Frequency of the roughness noise. Higher = finer details.
    //   0.05  → large rocky features (boulders, ledges)
    //   0.1   → natural rock grain (good default)
    //   0.15+ → fine cracks and bumps
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cave Morphology|Roughness")
    float RoughnessFrequency = 0.1f;

    // Which noise function to use for surface roughness.
    // Different types give caves very different visual character:
    //   fBM    → smooth organic (lava tubes, rounded galleries)
    //   Ridged → sharp craggy (erosion cracks, natural corridors)
    //   Mixed  → blend of both (good general default)
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cave Morphology|Roughness")
    EVoxelNoiseType RoughnessNoiseType = EVoxelNoiseType::FBM;

    // ===== DOMAIN WARPING =====
    //
    // Distorts the noise input coordinates using a secondary noise field.
    // This breaks up repetitive patterns and makes surfaces look more
    // organic — like they were shaped by flowing water or geological forces.
    // Applied to ALL roughness noise (regardless of noise type).
    //
    //   0.0 → disabled (default) — noise is sampled at true world positions
    //   3-5 → subtle organic distortion (good starting point)
    //   8-12 → noticeable warping, very organic cave walls
    //   15+ → extreme distortion, alien/surreal look
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cave Morphology|Domain Warp")
    float DomainWarpStrength = 0.0f;

    // Frequency of the warp noise field. Higher = tighter, more chaotic warps.
    //   0.01  → very broad, sweeping distortion
    //   0.03  → natural geological warping (good default)
    //   0.06+ → tight, detailed warping
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cave Morphology|Domain Warp")
    float DomainWarpFrequency = 0.03f;

    // ===== FLOOR BIAS =====
    //
    // Adds density in the lower portion of rooms to counteract surface roughness
    // creating bumpy, uneven floors. Acts like sediment settling on the cave floor —
    // the noise still carves relief, but the floor stays generally walkable.
    //
    // Only applied below room center Z (fades to 0 at center, strongest at floor).
    // Does NOT affect the open space above room center or walls/ceiling.
    //
    //   0   → disabled — full roughness on all surfaces (default)
    //   2-3 → subtle flattening, floor still has character
    //   4-6 → noticeably flatter floor while keeping wall/ceiling rough
    //   8+  → near-flat floor (good for navigable caves)
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cave Morphology|Roughness",
        meta = (ClampMin = "0.0"))
    float FloorBias = 0.0f;

    // ===== TERRAIN OPERATIONS =====
    //
    // These modify the density field AFTER morphology + roughness to create
    // specific geological features. They run on every voxel near cave surfaces.
    // Set the "strength" or "spacing" param to 0 to disable any operation.
    //
    // Pipeline order: Morphology → Roughness → TerrainOps → Worm Tunnels
    //
    // NOTE: These fields are INTERNAL TRANSPORT — populated by
    // UVoxelTerrainOpDefinition::ApplyTo() and read by the generator.
    // They have no UPROPERTY so they don't clutter the strate definition editor.
    // Designers configure terrain ops via UVoxelTerrainOpDefinition data assets
    // referenced in the strate definition's TerrainOperations array.

    // --- TERRACING ---
    //
    // Creates step-like horizontal ledges on cave walls. The cave surface
    // is broken into a staircase: flat shelf floors connected by short cliffs.
    // Great for platforming — players can jump between ledges.
    //
    // How it works: a smooth staircase function of world Z is used to
    // offset the density field near cave surfaces. Where the staircase
    // is above the real Z → extra solid (shelf floor). Where below → air gap.
    //
    //   0   → disabled (default)
    //   6-8 → natural stone steps (comfortable jump height in Unreal units)
    //   10-12 → tall ledges (need climbing)
    //   16+ → dramatic cliff-like terraces
    float TerraceStepHeight = 0.0f;

    // How sharp the terrace step edges are.
    //   0.0 → rounded, 0.5 → moderate, 0.9 → sharp angular ledges
    float TerraceHardness = 0.5f;

    // Noise displacement on terrace edge Z positions.
    //   0.0 → perfectly horizontal, 0.5 → natural, 1.0 → very wavy
    float TerraceNoiseDisplacement = 0.5f;

    // --- LAYER LINES ---
    //
    // Horizontal grooves carved into cave walls — visible geological strata.
    // Creates thin recessed lines at regular Z intervals, making walls
    // look like layered sedimentary rock. Purely geometric — the
    // material/shader could also add visual layers, but this is structural.
    //
    //   0   → disabled (default)
    //   4-6 → fine layering (detailed geological look)
    //   8-12 → broad visible layers
    //   16+ → dramatic thick bands
    float LayerLineSpacing = 0.0f;

    // Depth of each groove (how much density is subtracted).
    //   0.1 → barely visible, 0.3 → subtle, 0.6 → deep, 1.0+ → dramatic
    float LayerLineDepth = 0.3f;

    // --- OVERHANGS ---
    //
    // Horizontal shelf-like protrusions from cave walls.
    // Uses 3D noise with very low Z frequency, so features extend
    // horizontally for long distances before varying vertically.
    // Creates natural-looking rocky overhangs, shelves, and ledges
    // that are independent of the terracing system.
    //
    // Strength: 0 = disabled, higher = more prominent overhang features.
    //   0.0 → disabled (default)
    //   0.3 → subtle horizontal features
    //   0.5 → moderate overhangs (good default when enabled)
    //   0.8 → very prominent horizontal protrusions
    float OverhangStrength = 0.0f;

    // How far overhangs protrude from walls (in voxels of density added).
    //   2-3 → small, 5-8 → natural, 10+ → dramatic
    float OverhangDepth = 5.0f;

    // Frequency of the overhang noise. Z freq is auto 5x lower for horizontal features.
    //   0.03 → large rare, 0.06 → moderate, 0.1+ → many small
    float OverhangFrequency = 0.06f;

    // --- RIBBING ---
    //
    // Parallel ridge patterns on cave walls and ceilings — like lava tubes.
    // Creates evenly-spaced bumps/ribs that run horizontally along the
    // cave surface. The ridges ADD density (solid), creating protruding
    // bands of rock. This is the complement to layer lines (which subtract).
    //
    // Ribbing direction is along Z (horizontal ribs on walls), which gives
    // the classic lava tube look. Combined with domain warping, the ribs
    // become wavy and organic instead of perfectly straight.
    //
    //   0   → disabled (default)
    //   3-5 → fine ribbing (subtle lava tube feel)
    //   6-10→ prominent ribs (dramatic geological feature)
    //   12+ → thick bands of rock (very stylized)
    float RibbingSpacing = 0.0f;

    // How far ribs protrude from the wall (in voxels of density added).
    //   0.2 → barely visible, 0.4 → subtle, 0.8 → prominent, 1.5+ → dramatic
    float RibbingDepth = 0.4f;

    // --- CLIFF SHARPENING ---
    //
    // Amplifies vertical gradients in the density field to create sheer
    // rock faces. Where the cave surface is already somewhat vertical
    // (density changes quickly along Z), this operation steepens it further.
    //
    // How it works: near cave surfaces, we measure the vertical density
    // gradient. Where it's steep (cave wall is already vertical), we ADD
    // density below and SUBTRACT above — steepening the transition from
    // solid to air. This turns gentle slopes into dramatic cliff faces.
    //
    // Strength: 0 = disabled, higher = steeper cliffs.
    //   0.0 → disabled (default)
    //   0.3 → subtle steepening (walls feel more vertical)
    //   0.6 → strong cliff faces (good for dramatic strates)
    //   1.0 → extreme — nearly all slopes become vertical walls
    float CliffStrength = 0.0f;

    // --- SCALLOP ---
    //
    // Water-erosion-like concave patterns on cave walls.
    // Creates bowl-shaped indentations arranged in a semi-regular pattern,
    // like the scallops you see in real water-carved limestone caves.
    //
    // Uses 3D cellular (Worley) noise near cave surfaces to create
    // concave pockets. The distance-to-nearest-feature-point creates
    // natural bowl shapes when subtracted from density.
    //
    //   0.0 → disabled (default)
    //   0.3 → subtle erosion texture
    //   0.6 → visible scalloped walls (good for wet/limestone strates)
    //   1.0 → deep scallops, dramatic water-carved look
    float ScallopStrength = 0.0f;

    // Size of each scallop bowl. Lower = bigger, higher = smaller.
    //   0.05 → large, 0.1 → medium, 0.2 → small frequent
    float ScallopFrequency = 0.1f;

    // --- ARCH / BRIDGE ---
    //
    // Natural rock bridges spanning gaps in cave chambers.
    // Hash-placed horizontal cylinders of ADDED density that create
    // walkable bridges across open cave spaces. Arches only appear
    // inside caves (where SDF < 0), spanning from one wall to another.
    //
    // Each arch is a horizontal capsule (solid rock tube) that crosses
    // through the cave void. The hash determines position, direction,
    // and size. Arches are relatively rare, dramatic features.
    //
    //   0.0  → disabled (default)
    //   0.02 → very rare arches (impressive when found)
    //   0.06 → moderate (good for cathedral strates)
    //   0.1  → frequent bridges
    float ArchDensity = 0.0f;
    float ArchMinRadius = 3.0f;
    float ArchMaxRadius = 6.0f;

    // ===== COLUMNS / PILLARS =====
    //
    // Vertical solid cylinders connecting floor to ceiling in cave spaces.
    // These are natural rock pillars — where a stalactite and stalagmite
    // would have met, or where cave erosion left a supporting column.
    //
    // Columns are placed using hash-based placement (like rooms) and only
    // appear where the cave morphology has already carved an opening.
    // They ADD density (solid rock) inside what would otherwise be air.

    // Probability that a hash cell contains a column (0-1).
    // 0 = disabled. Higher = more pillars scattered through caves.
    //   0.0  → disabled (default)
    //   0.05 → rare pillars, impressive when found
    //   0.15 → moderate (good for cathedral strates)
    //   0.3  → dense forest of pillars
    float ColumnDensity = 0.0f;
    float ColumnMinRadius = 2.0f;
    float ColumnMaxRadius = 5.0f;

    // ===== PITS / VERTICAL SHAFTS =====
    //
    // Cylindrical downward voids carved into the cave floor.
    // These are natural vertical drops — sinkholes, collapsed ceilings,
    // narrow shafts. They create dramatic vertical gameplay:
    // the player can fall in, climb down, or discover connections
    // to lower areas within the same strate.
    //
    // Pits are hash-placed and only carve where the cave morphology
    // already created solid rock with air above (floor areas).

    // Probability that a hash cell contains a pit (0-1).
    // 0 = disabled. Pits are rare, impactful features.
    //   0.0  → disabled (default)
    //   0.03 → rare sinkholes (dangerous, surprising)
    //   0.08 → moderate (good for deep strates with vertical gameplay)
    //   0.15 → frequent pits (chaotic, maze-like)
    float PitDensity = 0.0f;
    float PitMinRadius = 4.0f;
    float PitMaxRadius = 10.0f;
    float PitDepth = 25.0f;

    // ----- Chimney / Shaft (upward voids — inverse of pits) -----

    // Chimneys: narrow vertical tubes piercing upward from cave ceilings.
    float ChimneyDensity = 0.0f;
    float ChimneyMinRadius = 2.0f;
    float ChimneyMaxRadius = 5.0f;
    float ChimneyHeight = 20.0f;

    // ----- Dome (hemispherical chamber ceilings) -----

    // Domes: hemispherical chamber ceilings for cathedral-like feel.
    float DomeDensity = 0.0f;
    float DomeMinRadius = 8.0f;
    float DomeMaxRadius = 15.0f;
    float DomeHeightRatio = 0.8f;

    // ----- Pinch / Bottleneck (passage narrowing) -----

    // Pinch: passage narrowing for bottlenecks/chokepoints.
    float PinchDensity = 0.0f;
    float PinchStrength = 5.0f;
    float PinchLength = 12.0f;

    // ----- Strate boundary sealing -----

    // Thickness of the solid shell at the top and bottom of each strate (in voxels).
    // Prevents caves from carving through strate boundaries, guaranteeing
    // a solid ceiling at the top and solid floor at the bottom.
    // The player must find actual passages to move between strates.
    //   2-3 → thin seal (some small holes possible near edges)
    //   4-5 → solid boundary (good default)
    //   8+  → very thick, no chance of breakthrough
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Boundary")
    float BoundarySealThickness = 4.0f;

    // ----- Water table (used by the Underwater archetype + water render system) -----

    // Height of the strate water table as a fraction of the strate's height (0-1).
    // 0 = no water. For the Underwater archetype set this high (e.g. 0.9) so almost
    // the whole cavern floods. The density field is unaffected — this only tells the
    // water render system where the surface sits. Rock below it reads as submerged.
    //   0.0  → dry caves (default)
    //   0.5  → half-flooded galleries
    //   0.9  → nearly fully submerged (Underwater archetype)
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Water",
        meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float WaterLevelRelative = 0.0f;

    // Runtime values — set by StrateManager, NOT editable in the definition.
    // These define the strate's Z range in voxel coordinates so the density
    // function can seal the top and bottom boundaries.
    float StrateTopWorldZ = 0.0f;
    float StrateBottomWorldZ = 0.0f;

    /**
     * Linearly interpolate between two param sets.
     * Used at strate boundaries to blend smoothly between adjacent strates.
     *
     * @param A - Params for the strate above
     * @param B - Params for the strate below
     * @param Alpha - 0.0 = fully A, 1.0 = fully B
     * @return Blended params
     */
    static FStrateGenerationParams Lerp(
        const FStrateGenerationParams& A,
        const FStrateGenerationParams& B,
        float Alpha)
    {
        FStrateGenerationParams Result;
        // Rock
        Result.BaseDensity                = FMath::Lerp(A.BaseDensity, B.BaseDensity, Alpha);
        Result.VerticalScale              = FMath::Lerp(A.VerticalScale, B.VerticalScale, Alpha);
        // Worm tunnels
        Result.WormFrequency              = FMath::Lerp(A.WormFrequency, B.WormFrequency, Alpha);
        Result.WormHorizontalBias         = FMath::Lerp(A.WormHorizontalBias, B.WormHorizontalBias, Alpha);
        Result.WormThreshold              = FMath::Lerp(A.WormThreshold, B.WormThreshold, Alpha);
        Result.WormStrength               = FMath::Lerp(A.WormStrength, B.WormStrength, Alpha);
        Result.WormNetworkRange           = FMath::Lerp(A.WormNetworkRange, B.WormNetworkRange, Alpha);
        // Cave morphology
        Result.RoomSpacing                = FMath::Lerp(A.RoomSpacing, B.RoomSpacing, Alpha);
        Result.RoomDensity                = FMath::Lerp(A.RoomDensity, B.RoomDensity, Alpha);
        Result.MinRoomRadius              = FMath::Lerp(A.MinRoomRadius, B.MinRoomRadius, Alpha);
        Result.MaxRoomRadius              = FMath::Lerp(A.MaxRoomRadius, B.MaxRoomRadius, Alpha);
        Result.RoomHeightRatio            = FMath::Lerp(A.RoomHeightRatio, B.RoomHeightRatio, Alpha);
        Result.RoomShapeVariety           = FMath::Lerp(A.RoomShapeVariety, B.RoomShapeVariety, Alpha);
        Result.RoomFloorCutMin            = FMath::Lerp(A.RoomFloorCutMin, B.RoomFloorCutMin, Alpha);
        Result.RoomFloorCutMax            = FMath::Lerp(A.RoomFloorCutMax, B.RoomFloorCutMax, Alpha);
        Result.FloorReliefStrength        = FMath::Lerp(A.FloorReliefStrength, B.FloorReliefStrength, Alpha);
        Result.FloorReliefFrequency       = FMath::Lerp(A.FloorReliefFrequency, B.FloorReliefFrequency, Alpha);
        Result.OriginRoomRadius           = FMath::Lerp(A.OriginRoomRadius, B.OriginRoomRadius, Alpha);
        Result.OriginRoomMaxConnections   = (Alpha < 0.5f) ? A.OriginRoomMaxConnections : B.OriginRoomMaxConnections;
        Result.TunnelMinRadius            = FMath::Lerp(A.TunnelMinRadius, B.TunnelMinRadius, Alpha);
        Result.TunnelMaxRadius            = FMath::Lerp(A.TunnelMaxRadius, B.TunnelMaxRadius, Alpha);
        Result.TunnelDensity              = FMath::Lerp(A.TunnelDensity, B.TunnelDensity, Alpha);
        Result.MaxTunnelLength            = FMath::Lerp(A.MaxTunnelLength, B.MaxTunnelLength, Alpha);
        Result.TunnelWarpStrength         = FMath::Lerp(A.TunnelWarpStrength, B.TunnelWarpStrength, Alpha);
        Result.TunnelHorizontalBias       = FMath::Lerp(A.TunnelHorizontalBias, B.TunnelHorizontalBias, Alpha);
        Result.bTunnelsFlowTowardOrigin   = (Alpha < 0.5f) ? A.bTunnelsFlowTowardOrigin : B.bTunnelsFlowTowardOrigin;
        Result.TunnelEndpointZOffset      = FMath::Lerp(A.TunnelEndpointZOffset, B.TunnelEndpointZOffset, Alpha);
        Result.SDFBlendRadius             = FMath::Lerp(A.SDFBlendRadius, B.SDFBlendRadius, Alpha);
        Result.WaterLevelRelative         = FMath::Lerp(A.WaterLevelRelative, B.WaterLevelRelative, Alpha);
        // Cave warp
        Result.CaveWarpStrength           = FMath::Lerp(A.CaveWarpStrength, B.CaveWarpStrength, Alpha);
        Result.CaveWarpFrequency          = FMath::Lerp(A.CaveWarpFrequency, B.CaveWarpFrequency, Alpha);
        // Roughness
        Result.SurfaceRoughness           = FMath::Lerp(A.SurfaceRoughness, B.SurfaceRoughness, Alpha);
        Result.RoughnessFrequency         = FMath::Lerp(A.RoughnessFrequency, B.RoughnessFrequency, Alpha);
        // Boundary seal
        Result.BoundarySealThickness      = FMath::Lerp(A.BoundarySealThickness, B.BoundarySealThickness, Alpha);
        Result.StrateTopWorldZ            = FMath::Lerp(A.StrateTopWorldZ, B.StrateTopWorldZ, Alpha);
        Result.StrateBottomWorldZ         = FMath::Lerp(A.StrateBottomWorldZ, B.StrateBottomWorldZ, Alpha);
        // Noise profile
        Result.RoughnessNoiseType         = (Alpha < 0.5f) ? A.RoughnessNoiseType : B.RoughnessNoiseType;
        Result.DomainWarpStrength         = FMath::Lerp(A.DomainWarpStrength, B.DomainWarpStrength, Alpha);
        Result.DomainWarpFrequency        = FMath::Lerp(A.DomainWarpFrequency, B.DomainWarpFrequency, Alpha);
        Result.FloorBias                  = FMath::Lerp(A.FloorBias, B.FloorBias, Alpha);
        // Terrain ops
        Result.TerraceStepHeight          = FMath::Lerp(A.TerraceStepHeight, B.TerraceStepHeight, Alpha);
        Result.TerraceHardness            = FMath::Lerp(A.TerraceHardness, B.TerraceHardness, Alpha);
        Result.TerraceNoiseDisplacement   = FMath::Lerp(A.TerraceNoiseDisplacement, B.TerraceNoiseDisplacement, Alpha);
        Result.LayerLineSpacing           = FMath::Lerp(A.LayerLineSpacing, B.LayerLineSpacing, Alpha);
        Result.LayerLineDepth             = FMath::Lerp(A.LayerLineDepth, B.LayerLineDepth, Alpha);
        Result.OverhangStrength           = FMath::Lerp(A.OverhangStrength, B.OverhangStrength, Alpha);
        Result.OverhangDepth              = FMath::Lerp(A.OverhangDepth, B.OverhangDepth, Alpha);
        Result.OverhangFrequency          = FMath::Lerp(A.OverhangFrequency, B.OverhangFrequency, Alpha);
        // Ribbing
        Result.RibbingSpacing             = FMath::Lerp(A.RibbingSpacing, B.RibbingSpacing, Alpha);
        Result.RibbingDepth               = FMath::Lerp(A.RibbingDepth, B.RibbingDepth, Alpha);
        // Cliff
        Result.CliffStrength              = FMath::Lerp(A.CliffStrength, B.CliffStrength, Alpha);
        // Scallop
        Result.ScallopStrength            = FMath::Lerp(A.ScallopStrength, B.ScallopStrength, Alpha);
        Result.ScallopFrequency           = FMath::Lerp(A.ScallopFrequency, B.ScallopFrequency, Alpha);
        // Arch
        Result.ArchDensity                = FMath::Lerp(A.ArchDensity, B.ArchDensity, Alpha);
        Result.ArchMinRadius              = FMath::Lerp(A.ArchMinRadius, B.ArchMinRadius, Alpha);
        Result.ArchMaxRadius              = FMath::Lerp(A.ArchMaxRadius, B.ArchMaxRadius, Alpha);
        // Columns
        Result.ColumnDensity              = FMath::Lerp(A.ColumnDensity, B.ColumnDensity, Alpha);
        Result.ColumnMinRadius            = FMath::Lerp(A.ColumnMinRadius, B.ColumnMinRadius, Alpha);
        Result.ColumnMaxRadius            = FMath::Lerp(A.ColumnMaxRadius, B.ColumnMaxRadius, Alpha);
        // Pits
        Result.PitDensity                 = FMath::Lerp(A.PitDensity, B.PitDensity, Alpha);
        Result.PitMinRadius               = FMath::Lerp(A.PitMinRadius, B.PitMinRadius, Alpha);
        Result.PitMaxRadius               = FMath::Lerp(A.PitMaxRadius, B.PitMaxRadius, Alpha);
        Result.PitDepth                   = FMath::Lerp(A.PitDepth, B.PitDepth, Alpha);
        // Chimneys
        Result.ChimneyDensity             = FMath::Lerp(A.ChimneyDensity, B.ChimneyDensity, Alpha);
        Result.ChimneyMinRadius           = FMath::Lerp(A.ChimneyMinRadius, B.ChimneyMinRadius, Alpha);
        Result.ChimneyMaxRadius           = FMath::Lerp(A.ChimneyMaxRadius, B.ChimneyMaxRadius, Alpha);
        Result.ChimneyHeight              = FMath::Lerp(A.ChimneyHeight, B.ChimneyHeight, Alpha);
        // Domes
        Result.DomeDensity                = FMath::Lerp(A.DomeDensity, B.DomeDensity, Alpha);
        Result.DomeMinRadius              = FMath::Lerp(A.DomeMinRadius, B.DomeMinRadius, Alpha);
        Result.DomeMaxRadius              = FMath::Lerp(A.DomeMaxRadius, B.DomeMaxRadius, Alpha);
        Result.DomeHeightRatio            = FMath::Lerp(A.DomeHeightRatio, B.DomeHeightRatio, Alpha);
        // Pinch
        Result.PinchDensity               = FMath::Lerp(A.PinchDensity, B.PinchDensity, Alpha);
        Result.PinchStrength              = FMath::Lerp(A.PinchStrength, B.PinchStrength, Alpha);
        Result.PinchLength                = FMath::Lerp(A.PinchLength, B.PinchLength, Alpha);
        return Result;
    }
};

//=============================================================================
// TERRAIN OPERATION REFERENCE
//=============================================================================

// Forward declaration — the actual data asset lives in VoxelTerrainOpDefinition.h.
// We only need a soft pointer here, so no #include needed.
class UVoxelTerrainOpDefinition;
class UStaticMesh;

/**
 * FStrateTerrainOpEntry — A reference to a terrain operation with a weight.
 *
 * Strate definitions hold an array of these. Each entry says "apply this
 * terrain operation at this intensity." The weight scales the op's primary
 * activation field (density, strength, or spacing).
 *
 * USAGE IN EDITOR:
 * In the strate definition's Details panel, expand TerrainOperations and add
 * entries. Pick a DA_Op_* asset and set its weight:
 *   - Weight 1.0 = use the op as configured in the asset
 *   - Weight 0.5 = half intensity (fewer/smaller features)
 *   - Weight 1.5 = more intense than the asset's default
 */
USTRUCT(BlueprintType)
struct VOXELFORGE_API FStrateTerrainOpEntry
{
    GENERATED_BODY()

    // Reference to a terrain operation data asset (e.g., DA_Op_DeepPit).
    // Soft pointer allows async loading and avoids hard dependencies.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terrain Op")
    TSoftObjectPtr<UVoxelTerrainOpDefinition> Operation;

    // Intensity multiplier for this operation in this strate.
    //   1.0 = use as configured in the data asset
    //   0.5 = half intensity (fewer/smaller features)
    //   2.0 = double intensity (use carefully — can overwhelm the cave)
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terrain Op",
        meta = (ClampMin = "0.0", ClampMax = "3.0"))
    float Weight = 1.0f;

    // Probability that any given room rolls this op from the strate's pool.
    // Ops are selected by weighted draw per-room during cache build:
    //   - All probs sum to < 1.0 → some rooms get no terrain op (empty space in the pool)
    //   - All probs sum to >= 1.0 → every room gets an op (pool is fully claimed)
    //   - Multi-op strates: each entry competes for each room's random roll
    //
    // Examples (single op):
    //   0.0 = never selected — op is disabled
    //   0.5 = ~50% of rooms get this op, ~50% get nothing
    //   1.0 = every room gets this op
    //
    // Examples (two ops, Prob=0.4 each → sum 0.8):
    //   ~40% rooms get op A, ~40% get op B, ~20% get nothing
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terrain Op",
        meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float Probability = 0.5f;
};

//=============================================================================
// SLAB GENERATION PARAMS (FlatPlain / CrystalChamber generator types)
//=============================================================================

/**
 * FSlabGenerationParams — Density parameters for slab-based strates.
 *
 * Used by the FlatPlain and CrystalChamber generator types instead of the
 * room-and-corridor FStrateGenerationParams. The entire strate becomes a
 * horizontal void between a noisy floor surface and a noisy ceiling surface.
 *
 * COORDINATE CONVENTION:
 * - Floor and ceiling positions are expressed as a fraction of the strate's
 *   total height (0.0 = strate bottom, 1.0 = strate top).
 * - Roughness values are in voxels of surface displacement.
 * - StrateTopWorldZ / StrateBottomWorldZ are runtime values (voxel coords),
 *   set by StrateManager — do NOT edit these manually.
 */
USTRUCT(BlueprintType)
struct VOXELFORGE_API FSlabGenerationParams
{
    GENERATED_BODY()

    // ===== VOID SHAPE =====

    // Where the floor surface sits, relative to this strate's height.
    // 0.0 = at the very bottom of the strate, 1.0 = at the very top.
    // Keep this well below CeilingRelativeHeight or you get no open space.
    //   0.1-0.2 → floor is near the bottom (lots of headroom)
    //   0.25    → floor at 25% height (good default)
    //   0.4+    → very low ceiling; claustrophobic plains
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slab|Shape",
        meta = (ClampMin = "0.0", ClampMax = "0.95"))
    float FloorRelativeHeight = 0.25f;

    // Where the ceiling surface sits, relative to this strate's height.
    // Must be above FloorRelativeHeight. The difference determines how tall
    // the open void is.
    //   0.6  → moderate ceiling height (tight plains)
    //   0.8  → tall open space (good default for FlatPlain)
    //   0.9+ → nearly the full strate height is open (cavernous plains)
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slab|Shape",
        meta = (ClampMin = "0.05", ClampMax = "1.0"))
    float CeilingRelativeHeight = 0.80f;

    // ===== FLOOR ROUGHNESS =====
    // Perlin-based displacement of the floor surface up/down.
    // Creates gently rolling ground — hills, shallow valleys, bumps.
    // The noise has a very low Z frequency so features extend horizontally.

    // How many voxels the floor surface can shift up or down.
    //   0   → perfectly flat floor (artificial, but dramatic)
    //   3-5 → subtle rolling ground (good for FlatPlain)
    //   8+  → significant hills, deep valleys
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slab|Floor")
    float FloorRoughness = 4.0f;

    // Spatial frequency of the floor noise. Lower = broader hills.
    //   0.02 → very wide rolling hills (strate-scale)
    //   0.04 → moderate hills (good default)
    //   0.08 → frequent small bumps
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slab|Floor")
    float FloorRoughnessFrequency = 0.04f;

    // ===== CEILING ROUGHNESS =====
    // abs(noise) is used so ceiling features only protrude DOWNWARD.
    // This creates stalactite/crystal-like formations hanging from above.
    // For FlatPlain: keep this low for a relatively smooth ceiling.
    // For CrystalChamber: crank this up to create dense formation forests.

    // How far ceiling formations hang down into the void (in voxels).
    //   0     → flat ceiling (no formations)
    //   4-6   → subtle bumps (FlatPlain default)
    //   10-15 → significant formations (CrystalChamber default)
    //   20+   → dramatic columns reaching toward the floor
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slab|Ceiling")
    float CeilingRoughness = 6.0f;

    // Spatial frequency of the ceiling formation noise. Lower = fewer, larger formations.
    //   0.02 → rare massive formations (icebergs)
    //   0.04 → moderate (good default for FlatPlain)
    //   0.06 → dense cluster of crystals (good for CrystalChamber)
    //   0.1+ → very frequent small stalactites
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slab|Ceiling")
    float CeilingRoughnessFrequency = 0.04f;

    // ===== COLUMNS / PILLARS =====
    // Vertical rock cylinders scattered through the open void.
    // Unlike TunnelNetwork columns (room-relative), these are placed on a
    // world-space hash grid — they can appear anywhere in the void.
    //
    // Columns are ALWAYS full-height (floor-to-ceiling). Their radius can
    // vary but they always span the full void from floor to ceiling.

    // Probability that a hash grid cell contains a column (0 = none, 1 = maximum density).
    //   0.0  → disabled (no columns)
    //   0.05 → sparse, dramatic lone pillars
    //   0.12 → moderate scattering (good default)
    //   0.25 → dense forest of pillars
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slab|Columns",
        meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float ColumnDensity = 0.08f;

    // Smallest column radius (in voxels).
    //   1-2 → slender needle-like pillars
    //   3-4 → narrow columns (good default)
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slab|Columns")
    float ColumnMinRadius = 2.0f;

    // Largest column radius (in voxels).
    //   6-8   → natural stone pillars (good default)
    //   12-15 → massive trunks
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slab|Columns")
    float ColumnMaxRadius = 7.0f;

    // Hash grid cell size (higher = columns further apart).
    //   30 → dense grid, many candidate slots (most empty due to ColumnDensity)
    //   60 → moderate spacing (good default)
    //   100+ → only occasional columns
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slab|Columns")
    float ColumnSpacing = 60.0f;

    // ===== BOUNDARY & DENSITY =====

    // Solid rock shell at the top and bottom of the strate.
    // Prevents the void from touching the strate boundary — guarantees a
    // solid ceiling and floor at the geological layer edges.
    //   3-4 → thin seal (good default for slab strates)
    //   6+  → thick solid boundary
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slab|Boundary")
    float BoundarySealThickness = 4.0f;

    // Base solidity strength — used to scale the boundary seal force.
    // Match this to the floor/ceiling noise scale so the seal overpowers roughness.
    //   6-8 → good default (matches typical roughness values)
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slab|Boundary")
    float BaseDensity = 8.0f;

    // ===== RUNTIME (set by StrateManager — do not edit) =====

    // Top Z boundary of this strate in voxel coordinates. Set by StrateManager.
    float StrateTopWorldZ = 0.0f;

    // Bottom Z boundary of this strate in voxel coordinates. Set by StrateManager.
    float StrateBottomWorldZ = 0.0f;
};

//=============================================================================
// MAZE GENERATION PARAMS  (ECaveGeneratorType::Maze)
//=============================================================================
/**
 * FMazeGenerationParams — tight, branching corridors on a deterministic 3D lattice.
 *
 * Each lattice node sits at a cell center. An edge to a +X / +Y / +Z neighbour exists
 * when a symmetric pair-hash passes BranchProbability (Z edges additionally gated by
 * Verticality). Corridors are thin capsules carved through solid rock. Because edges
 * are decided by a pure symmetric hash of the two cells, the maze is identical in every
 * chunk with no caching needed — evaluated per-voxel over the few nearby cells.
 */
USTRUCT(BlueprintType)
struct VOXELFORGE_API FMazeGenerationParams
{
    GENERATED_BODY()

    // Lattice cell size in voxels. Smaller = tighter, more claustrophobic maze.
    //   25-35 → very tight warren · 40 → classic maze (default) · 60+ → roomy
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Maze", meta = (ClampMin = "8.0"))
    float CellSize = 40.0f;

    // Corridor tube radius in voxels. Keep well below CellSize/2 to leave walls.
    //   3 → crawlspace · 4 → walkable (default) · 6 → wide halls
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Maze", meta = (ClampMin = "1.0"))
    float CorridorRadius = 4.0f;

    // Probability that a horizontal edge between adjacent cells is open (0-1).
    // Lower = more dead ends and a more maze-like feel; higher = more open/connected.
    //   0.45 → sparse, lots of dead ends · 0.7 → connected maze (default) · 0.9 → very open
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Maze", meta = (ClampMin = "0.05", ClampMax = "1.0"))
    float BranchProbability = 0.7f;

    // Probability that a vertical (Z) edge between stacked cells is open (0-1).
    // 0 = single-level maze; higher = multi-storey with vertical shafts between levels.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Maze", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float Verticality = 0.3f;

    // Small-scale wall roughness (voxels). 0 = perfectly smooth tubes.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Maze", meta = (ClampMin = "0.0"))
    float SurfaceRoughness = 2.0f;

    // Solid shell thickness at strate top/bottom (voxels).
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Maze", meta = (ClampMin = "0.0"))
    float BoundarySealThickness = 4.0f;

    // Rock solidity before carving (higher = more solid). Matches roughness scale.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Maze")
    float BaseDensity = 8.0f;

    // Runtime Z bounds (voxel coords) — set by StrateManager, do not edit.
    float StrateTopWorldZ = 0.0f;
    float StrateBottomWorldZ = 0.0f;
};

//=============================================================================
// SURFACE-WORLD GENERATION PARAMS  (ECaveGeneratorType::SurfaceWorld)
//=============================================================================
/**
 * FSurfaceGenerationParams — open-sky terrain inside a strate.
 *
 * A heightfield (fBM continents + ridged mountains + fine detail) defines the ground;
 * everything below it is solid, everything above is open air up to a high solid ceiling
 * (the "sky cap"). An optional water table floods valleys to make rivers/lakes/beaches.
 * Skylight is simulated by up-facing light actors placed via the content pool.
 */
USTRUCT(BlueprintType)
struct VOXELFORGE_API FSurfaceGenerationParams
{
    GENERATED_BODY()

    // Mean ground height as a fraction of the strate height (0 = bottom, 1 = top).
    // The terrain heightfield varies around this baseline by ElevationRange.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Surface|Shape", meta = (ClampMin = "0.05", ClampMax = "0.9"))
    float BaseGroundRelative = 0.30f;

    // Total vertical amplitude of the terrain heightfield in voxels (peak-to-trough-ish).
    //   20 → gentle plains · 60 → hills + valleys (default) · 120+ → tall mountains
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Surface|Shape", meta = (ClampMin = "0.0"))
    float ElevationRange = 60.0f;

    // Low-frequency "continent" noise — broad landmasses and basins.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Surface|Shape")
    float ContinentFrequency = 0.006f;

    // How strongly sharp ridged mountains mix into the heightfield (0-1).
    //   0 → rolling hills only · 0.5 → hills + peaks (default) · 1 → dramatic ranges
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Surface|Shape", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float MountainStrength = 0.5f;

    // Frequency of the ridged mountain noise.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Surface|Shape")
    float MountainFrequency = 0.012f;

    // Fine detail frequency — small bumps and rocks on the surface.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Surface|Shape")
    float DetailFrequency = 0.04f;

    // Small-scale surface roughness in voxels (rocks, bumps).
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Surface|Shape", meta = (ClampMin = "0.0"))
    float SurfaceRoughness = 3.0f;

    // ----- Macro relief & landforms -----

    // Domain-warp the heightfield query by this many voxels of XY displacement before
    // sampling continents/mountains. Bends straight coastlines and ridgelines into winding,
    // organic landforms. 0 = no warp (axis-aligned blobby noise, the old look).
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Surface|Macro", meta = (ClampMin = "0.0"))
    float HeightWarpStrength = 35.0f;

    // Frequency of the domain-warp noise. Lower = broader, sweeping bends.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Surface|Macro")
    float HeightWarpFrequency = 0.008f;

    // Macro "relief" map frequency: a very-low-frequency field that makes some regions flat
    // plains and others mountainous highlands — distinct terrain depending on where you
    // stand. (The cheap, continuous precursor to a full biome system.) Lower = larger regions.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Surface|Macro")
    float ReliefFrequency = 0.0015f;

    // How strongly the relief map modulates terrain (0-1). 0 = uniform everywhere (old
    // behaviour); 1 = full plains <-> mountains variation across the world.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Surface|Macro", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float ReliefStrength = 0.7f;

    // Contrast of the relief map. >1 sharpens the plains/highland boundary (more distinct
    // regions); ~1 keeps it a gradual blend.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Surface|Macro", meta = (ClampMin = "0.25", ClampMax = "4.0"))
    float ReliefContrast = 1.6f;

    // Plateau/mesa terracing strength (0-1), applied in high-relief regions only. 0 = off
    // (smooth slopes). Crank up for stepped mesas and layered cliffs in the mountainous areas.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Surface|Macro", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float TerraceStrength = 0.0f;

    // Height of each terrace step in voxels (when TerraceStrength > 0).
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Surface|Macro", meta = (ClampMin = "1.0"))
    float TerraceHeight = 12.0f;

    // ----- Water -----

    // Water table height as a fraction of strate height (0 = no water). Valleys below
    // this flood into lakes/rivers; the band just above is flattened into beaches.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Surface|Water", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float WaterLevelRelative = 0.27f;

    // Width (voxels) of the flattened beach/shore band around the water line.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Surface|Water", meta = (ClampMin = "0.0"))
    float BeachWidth = 8.0f;

    // ----- Sky cap & boundary -----

    // Height of the solid ceiling ("sky cap") as a fraction of strate height (0-1).
    // The open-air gap is between the terrain and this cap. Keep high for big skies.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Surface|Sky", meta = (ClampMin = "0.3", ClampMax = "1.0"))
    float CeilingRelative = 0.95f;

    // Downward bumpiness of the sky-cap ceiling (voxels). 0 = flat ceiling.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Surface|Sky", meta = (ClampMin = "0.0"))
    float CeilingRoughness = 6.0f;

    // Frequency of the fine downward bumpiness above. Higher = smaller, denser bumps.
    // (Was hard-coded to 0.04 — exposed so the cap detail scale is tunable.)
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Surface|Sky", meta = (ClampMin = "0.0"))
    float CeilingRoughnessFrequency = 0.04f;

    // Broad ceiling undulation (voxels): a low-frequency SIGNED swell that raises and lowers
    // the WHOLE sky-cap, giving big inverted "hills and valleys" overhead — the ceiling reads
    // like terrain instead of a flat lid. Independent of the fine bumps. 0 = level cap height.
    //   0    → flat cap (old look)
    //   30   → gentle rolling ceiling
    //   80+  → dramatic overhead valleys and rises
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Surface|Sky", meta = (ClampMin = "0.0"))
    float CeilingUndulation = 0.0f;

    // Frequency of the broad undulation. Lower = larger, sweeping ceiling valleys.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Surface|Sky", meta = (ClampMin = "0.0"))
    float CeilingUndulationFrequency = 0.004f;

    // Ridged hanging formations (voxels): sharp downward ridges/blades carved into the cap —
    // the "valley-like ridges" / inverted-mountain-range look. Adds to the downward hang.
    //   0    → none
    //   20   → clear ridgelines across the ceiling
    //   50+  → dramatic hanging ranges
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Surface|Sky", meta = (ClampMin = "0.0"))
    float CeilingRidgeStrength = 0.0f;

    // Frequency of the ridged ceiling formations. Lower = broader ridges.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Surface|Sky", meta = (ClampMin = "0.0"))
    float CeilingRidgeFrequency = 0.02f;

    // Domain-warp the ceiling ridge/undulation query by this many voxels so ridgelines wind
    // organically instead of looking like axis-aligned noise. 0 = no warp (fine bumps are
    // unaffected, like the ground heightfield's detail layer).
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Surface|Sky", meta = (ClampMin = "0.0"))
    float CeilingWarpStrength = 0.0f;

    // Frequency of the ceiling warp noise. Lower = broader bends.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Surface|Sky", meta = (ClampMin = "0.0"))
    float CeilingWarpFrequency = 0.01f;

    // Solid shell thickness at strate top/bottom (voxels).
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Surface|Boundary", meta = (ClampMin = "0.0"))
    float BoundarySealThickness = 4.0f;

    // Rock solidity before carving.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Surface|Boundary")
    float BaseDensity = 8.0f;

    // Runtime Z bounds (voxel coords) — set by StrateManager, do not edit.
    float StrateTopWorldZ = 0.0f;
    float StrateBottomWorldZ = 0.0f;
};

//=============================================================================
// VERTICAL-SHAFT GENERATION PARAMS  (ECaveGeneratorType::VerticalShafts)
//=============================================================================
/**
 * FVerticalShaftParams — a mostly-vertical cave system.
 *
 * Hash-placed full-height vertical shafts (tapered capsules spanning the strate) carved
 * into solid rock, with periodic horizontal ledges inside them and occasional thin
 * horizontal connectors linking neighbouring shafts. Emphasises climbing/falling.
 */
USTRUCT(BlueprintType)
struct VOXELFORGE_API FVerticalShaftParams
{
    GENERATED_BODY()

    // Hash-grid cell size for shaft XY placement (voxels). Larger = shafts further apart.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shafts", meta = (ClampMin = "10.0"))
    float ShaftSpacing = 55.0f;

    // Probability a grid cell contains a shaft (0-1).
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shafts", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float ShaftDensity = 0.6f;

    // Shaft radius range (voxels).
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shafts", meta = (ClampMin = "1.0"))
    float ShaftMinRadius = 5.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shafts", meta = (ClampMin = "1.0"))
    float ShaftMaxRadius = 11.0f;

    // Chance an adjacent pair of shafts is joined by a horizontal connector tunnel (0-1).
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shafts", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float CrossConnectChance = 0.35f;

    // Radius of horizontal connector tunnels (voxels).
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shafts", meta = (ClampMin = "1.0"))
    float ConnectorRadius = 4.0f;

    // Vertical spacing of ledges inside shafts (voxels). 0 = no ledges (sheer drops).
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shafts", meta = (ClampMin = "0.0"))
    float LedgeSpacing = 24.0f;

    // How far ledges intrude into the shaft (voxels of solid added).
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shafts", meta = (ClampMin = "0.0"))
    float LedgeDepth = 3.0f;

    // Small-scale wall roughness (voxels).
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shafts", meta = (ClampMin = "0.0"))
    float SurfaceRoughness = 3.0f;

    // Solid shell thickness at strate top/bottom (voxels).
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shafts", meta = (ClampMin = "0.0"))
    float BoundarySealThickness = 4.0f;

    // Rock solidity before carving.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shafts")
    float BaseDensity = 8.0f;

    // Runtime Z bounds (voxel coords) — set by StrateManager, do not edit.
    float StrateTopWorldZ = 0.0f;
    float StrateBottomWorldZ = 0.0f;
};

//=============================================================================
// FLOATING-ISLAND GENERATION PARAMS  (ECaveGeneratorType::FloatingIslands)
//=============================================================================
/**
 * FFloatingIslandParams — suspended land masses in a large open void.
 *
 * The strate interior is open air; hash-placed island blobs (flattened-top ellipsoids,
 * roughened underside) float at jittered heights. From below they read as land hanging
 * "in the sky". Top/bottom are sealed solid so the void is enclosed.
 */
USTRUCT(BlueprintType)
struct VOXELFORGE_API FFloatingIslandParams
{
    GENERATED_BODY()

    // Hash-grid cell size for island placement (voxels). Larger = islands further apart.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Islands", meta = (ClampMin = "20.0"))
    float IslandSpacing = 95.0f;

    // Probability a grid cell contains an island (0-1).
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Islands", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float IslandDensity = 0.5f;

    // Island horizontal radius range (voxels).
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Islands", meta = (ClampMin = "2.0"))
    float IslandMinRadius = 18.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Islands", meta = (ClampMin = "2.0"))
    float IslandMaxRadius = 42.0f;

    // Island vertical thickness as a fraction of its horizontal radius.
    //   0.4 → thin plates · 0.7 → chunky (default) · 1.0 → near-spherical
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Islands", meta = (ClampMin = "0.1", ClampMax = "1.5"))
    float ThicknessRatio = 0.7f;

    // How much islands scatter vertically within the void (0 = all mid-height, 1 = full spread).
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Islands", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float VerticalJitter = 0.6f;

    // Flatten island tops into walkable land (0 = round dome, 1 = flat plateau).
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Islands", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float TopFlatten = 0.6f;

    // Surface roughness on island shells (voxels) — craggy undersides, bumpy tops.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Islands", meta = (ClampMin = "0.0"))
    float SurfaceRoughness = 4.0f;

    // SmoothMin blend radius for merging overlapping islands (voxels).
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Islands", meta = (ClampMin = "0.0"))
    float SDFBlendRadius = 5.0f;

    // Solid shell thickness at strate top/bottom (voxels).
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Islands", meta = (ClampMin = "0.0"))
    float BoundarySealThickness = 4.0f;

    // Rock solidity inside islands.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Islands")
    float BaseDensity = 8.0f;

    // Runtime Z bounds (voxel coords) — set by StrateManager, do not edit.
    float StrateTopWorldZ = 0.0f;
    float StrateBottomWorldZ = 0.0f;
};

//=============================================================================
// DISTURBANCE PARAMS  (the "wow" layer — applies to ANY archetype)
//=============================================================================
/**
 * FStrateDisturbanceParams — composable surprises layered on top of whatever
 * archetype a strate uses, as a density post-process. Hash-placed and deterministic,
 * they never breach the strate seals (they only act in the interior). Set a feature's
 * density to 0 to disable it. Designed to make exploration unpredictable: a chasm
 * splitting an otherwise tidy maze, a natural bridge over a chamber, blades of rock
 * rising from the floor.
 */
USTRUCT(BlueprintType)
struct VOXELFORGE_API FStrateDisturbanceParams
{
    GENERATED_BODY()

    // --- CHASMS: large vertical rifts that carve open air through the interior ---
    // Probability a chasm cell is active (0 = disabled).
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Disturbance|Chasms", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float ChasmDensity = 0.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Disturbance|Chasms", meta = (ClampMin = "20.0"))
    float ChasmSpacing = 170.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Disturbance|Chasms", meta = (ClampMin = "1.0"))
    float ChasmRadius = 14.0f;

    // --- BRIDGES: horizontal solid spans across open space ---
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Disturbance|Bridges", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float BridgeDensity = 0.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Disturbance|Bridges", meta = (ClampMin = "20.0"))
    float BridgeSpacing = 120.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Disturbance|Bridges", meta = (ClampMin = "1.0"))
    float BridgeRadius = 5.0f;

    // --- RIDGES: thin solid blades rising from the floor ---
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Disturbance|Ridges", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float RidgeDensity = 0.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Disturbance|Ridges", meta = (ClampMin = "20.0"))
    float RidgeSpacing = 110.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Disturbance|Ridges", meta = (ClampMin = "0.0"))
    float RidgeHeight = 30.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Disturbance|Ridges", meta = (ClampMin = "1.0"))
    float RidgeThickness = 6.0f;

    // Rock solidity used to scale carve/fill strength. Match the strate's BaseDensity.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Disturbance")
    float BaseDensity = 8.0f;

    // Seal thickness — disturbances stay clear of the seal bands. Match the strate.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Disturbance", meta = (ClampMin = "0.0"))
    float BoundarySealThickness = 4.0f;

    // Runtime Z bounds (voxel coords) — set by StrateManager, do not edit.
    float StrateTopWorldZ = 0.0f;
    float StrateBottomWorldZ = 0.0f;
};

//=============================================================================
// INTER-STRATE PASSAGE CONFIG  (per-strate tunnel control)
//=============================================================================
/**
 * EVoxelPassageStyle — the shape of an auto-carved inter-strate tunnel.
 */
UENUM(BlueprintType)
enum class EVoxelPassageStyle : uint8
{
    Straight  UMETA(DisplayName = "Straight (vertical shaft)"),
    Worm      UMETA(DisplayName = "Worm (organic meander around the axis)"),
    Spiral    UMETA(DisplayName = "Spiral (corkscrew descent)"),
    Cascading UMETA(DisplayName = "Cascading (ledge + drop staircase)")
};

/**
 * FStratePassageConfig — how THIS strate connects DOWN to the strate below it.
 *
 * Lives on each UVoxelStrateDefinition: the upper strate of every boundary controls its
 * own descent tunnels, so different layers connect differently. The (0,0) spine descent
 * is separate (player-dug); these are the auto-carved shortcuts placed away from it.
 */
USTRUCT(BlueprintType)
struct VOXELFORGE_API FStratePassageConfig
{
    GENERATED_BODY()

    // How many auto-carved tunnels descend from this strate to the one below (0 = none).
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Passage", meta = (ClampMin = "0", ClampMax = "12"))
    int32 Connections = 1;

    // Tunnel shape.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Passage")
    EVoxelPassageStyle Style = EVoxelPassageStyle::Worm;

    // ----- WIDTH (tapers along the length) -----
    // Radius at the two mouths (entry/exit) and at the middle. Equal = uniform tube;
    // Mouth > Mid = chambers at the ends with a squeeze between; Mid > Mouth = a bulge.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Passage|Width", meta = (ClampMin = "1.0"))
    float MouthRadius = 6.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Passage|Width", meta = (ClampMin = "1.0"))
    float MidRadius = 4.0f;

    // ----- LENGTH -----
    // How far the tunnel reaches INTO each strate (voxels). Auto-capped to the interior.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Passage|Length", meta = (ClampMin = "8.0"))
    float ReachMin = 40.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Passage|Length", meta = (ClampMin = "8.0"))
    float ReachMax = 90.0f;

    // ----- PLACEMENT -----
    // Horizontal distance range from the (0,0) spine (voxels) where tunnels may appear.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Passage|Placement", meta = (ClampMin = "0.0"))
    float DistanceMin = 60.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Passage|Placement", meta = (ClampMin = "0.0"))
    float DistanceMax = 200.0f;

    // ----- SHAPE detail -----
    // Worm: max sideways excursion from the axis (voxels). 0 = straight even in Worm style.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Passage|Shape", meta = (ClampMin = "0.0"))
    float Wander = 15.0f;

    // Path resolution (control points). Higher = smoother curves; lower = more faceted /
    // cheaper. The worm makes several bends, so keep this reasonably high for smoothness.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Passage|Shape", meta = (ClampMin = "1", ClampMax = "48"))
    int32 Segments = 20;

    // Vertical wobble (voxels) — dips/rises along the descent for a less uniform fall.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Passage|Shape", meta = (ClampMin = "0.0"))
    float VerticalWobble = 0.0f;

    // Spiral style: helix radius (voxels) and number of full turns over the descent.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Passage|Spiral", meta = (ClampMin = "1.0"))
    float SpiralRadius = 16.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Passage|Spiral", meta = (ClampMin = "0.25"))
    float SpiralTurns = 2.0f;

    // Cascading style: number of ledge+drop steps, and how far each ledge runs (voxels).
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Passage|Cascade", meta = (ClampMin = "1", ClampMax = "16"))
    int32 CascadeSteps = 4;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Passage|Cascade", meta = (ClampMin = "0.0"))
    float CascadeLedge = 14.0f;
};

//=============================================================================
// CONTENT ENTRY STRUCTS
//=============================================================================

/**
 * FStrateDecoration — One decoration type that can spawn on surfaces.
 *
 * Decorations are actors placed ON the cave surface (stalactites on ceilings,
 * mushrooms on floors, crystals on walls, etc.).
 * The decoration placer (future system) reads these entries from the active
 * strate definition and spawns actors accordingly.
 */
USTRUCT(BlueprintType)
struct VOXELFORGE_API FStrateDecoration
{
    GENERATED_BODY()

    // The actor class to spawn (e.g., BP_Stalactite, BP_CrystalCluster).
    // Real actors: lights, logic, interaction. They cost game-thread time per instance —
    // prefer InstancedMesh for pure visual props, and consider the Far tier so the coarse grid keeps
    // their spawn count down.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Decoration")
    TSubclassOf<AActor> ActorClass;

    // INSTANCED path: if set, this entry renders as batched HISM instances instead of
    // spawning ActorClass (which is then ignored). No tick, no per-actor overhead,
    // engine-culled — orders of magnitude cheaper. Use for everything that doesn't need
    // logic/lights/interaction; an emissive material still glows at distance without a light.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Decoration")
    UStaticMesh* InstancedMesh = nullptr;

    // Which of the two decoration streaming grids this entry uses (§8.5). Far (default) = full radius +
    // coarse column grid (cheap for rare/large props visible everywhere); Near = short radius + fine
    // column grid (dense groundcover near the player only). The radius/spacing presets live on
    // VoxelSettings; this only PICKS a grid. Defaults reproduce the legacy single-radius fine grid until
    // you opt into a coarser far spacing or move an entry to Near. (Replaces the old vestigial MaxLODLevel.)
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Decoration")
    EDecoStreamTier StreamTier = EDecoStreamTier::Far;

    // Which surface type this decoration can be placed on
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Decoration")
    ESurfaceType SurfacePlacement = ESurfaceType::Any;

    // Maximum surface tilt (degrees from flat) this decoration tolerates. The surface tilt is
    // acos(|normal.Z|): 0 = perfectly flat floor/ceiling, 90 = vertical wall. Grass on gentle
    // ground → ~30-40; rock/lichen that clings to slopes → 90 (no filter, the default).
    //   90 → place anywhere (default, no filter) · 35 → grass that avoids cliffs · 15 → flats only
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Decoration", meta = (ClampMin = "0.0", ClampMax = "90.0"))
    float MaxSlopeAngle = 90.0f;

    // Minimum surface tilt (degrees from flat) — the LOWER companion to MaxSlopeAngle. Rejects surfaces
    // FLATTER than this, so a prop can be kept OFF flat ground and restricted to slopes / walls. Same
    // metric as MaxSlopeAngle: acos(|normal.Z|), 0 = flat, 90 = vertical. Pair the two to band a prop
    // onto a tilt range (e.g. 30..70 = slopes only, never flats or sheer walls).
    //   0 → no filter (default) · 45 → slopes & walls only · 70 → near-vertical only
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Decoration", meta = (ClampMin = "0.0", ClampMax = "90.0"))
    float MinSlopeAngle = 0.0f;

    // Chance per valid surface point to spawn this decoration (0-1)
    // 0.01 = rare, 0.1 = common, 0.5 = very dense
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Decoration", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float SpawnDensity = 0.05f;

    // Random scale range for variety
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Decoration")
    float MinScale = 0.8f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Decoration")
    float MaxScale = 1.2f;

    // ----- Placement rules -----

    // Rotate the actor so its up-axis follows the surface normal (stalactites point
    // down on ceilings, plants stand up on floors). If false, keeps world-up.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Decoration|Placement")
    bool bAlignToSurface = true;

    // Apply a deterministic random yaw so instances don't all face the same way.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Decoration|Placement")
    bool bRandomYaw = true;

    // When bRandomYaw is set, constrain the random yaw to [MinYaw, MaxYaw] degrees instead of a full
    // turn. Lets a prop face roughly one way with a little variation (wind-bent grass: 80..100). The
    // default 0..360 is a full unrestricted turn — byte-identical to the legacy behaviour. Ignored when
    // bRandomYaw is false.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Decoration|Placement",
        meta = (EditCondition = "bRandomYaw", ClampMin = "0.0", ClampMax = "360.0"))
    float MinYaw = 0.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Decoration|Placement",
        meta = (EditCondition = "bRandomYaw", ClampMin = "0.0", ClampMax = "360.0"))
    float MaxYaw = 360.0f;

    // Wall props only: exclude downward-facing OVERHANGS. A "wall" is any surface between floor and
    // ceiling (|normal.Z| <= 0.5), which still includes surfaces that lean slightly DOWNWARD (overhang
    // ceilings). For props that must sit on upright walls (vines, wall torches) set this so only normals
    // with Z >= 0 (vertical or up-leaning) qualify. Ignored unless the point resolves as a wall.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Decoration|Placement")
    bool bWallExcludeOverhangs = false;

    // Offset along the surface normal (world units). Positive = lift off the surface,
    // negative = sink into it. Useful to embed roots or float crystals slightly.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Decoration|Placement")
    float SurfaceOffset = 0.0f;

    // Hard cap on how many of THIS decoration spawn per chunk (perf safety).
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Decoration|Placement", meta = (ClampMin = "1"))
    int32 MaxPerChunk = 40;

    // Only place where this is below the strate water line (true) or above it (false).
    // Ignored unless RequireWaterRelative is set. Lets you put seaweed underwater and
    // grass above water in the same strate.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Decoration|Placement")
    bool bRequireWaterRelative = false;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Decoration|Placement",
        meta = (EditCondition = "bRequireWaterRelative"))
    bool bPlaceBelowWater = false;

    // ----- Performance (HISM render tuning — only affects the InstancedMesh path) -----

    // Distance (world units / cm) past which instances stop rendering. This is THE lever that makes
    // DENSE groundcover affordable: grass can be placed thickly but only drawn near the player, so the
    // GPU cost is bounded by area-within-cull, not by the whole streaming radius. 0 = never cull (the
    // default — correct for trees / large props you want visible to the horizon).
    //   0     → no cull (props, trees)
    //   2000  → ~20 m, typical grass / small ground clutter
    //   4000  → ~40 m, taller plants you want visible a bit further
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Decoration|Performance", meta = (ClampMin = "0.0"))
    float CullDistance = 0.0f;

    // Whether these instances cast dynamic shadows. Dense instanced shadows are the single biggest cost
    // of heavy foliage — turn this OFF for grass / small clutter (an unlit-from-below tuft loses almost
    // nothing visually). Leave ON for trees and anything large enough that its shadow reads as grounding.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Decoration|Performance")
    bool bCastShadow = true;
};

/**
 * FStrateLandmark — A RARE, large, far-visible object placed on a coarse HASH LATTICE (§8.5).
 *
 * This is the right primitive for things like the underground "mini-suns" (in-lore light sources): one
 * object per ~`SpacingChunks` lattice cell, so the work scales with how MANY landmarks are in range
 * (a handful), NOT with the streamed area. That makes a HUGE stream radius (e.g. visible 16 km out so it
 * never pops) cheap — unlike the per-chunk decoration grid, which enumerates every chunk in the disk and
 * freezes at large radius. Placement is deterministic (pure hash of cell + entry + seed → no pop, same
 * landmark in the same place forever), evaluated synchronously on the game thread only when a NEW lattice
 * cell enters range (there are so few candidates this never hitches). Strate-wide (listed on the strate
 * definition), with an optional per-landmark biome filter. Foliage-style transform tweaks are exposed.
 */
USTRUCT(BlueprintType)
struct VOXELFORGE_API FStrateLandmark
{
    GENERATED_BODY()

    // ----- What to spawn (one of these; ActorClass wins if both set) -----

    // Real actor — use this for a sun that carries its own LIGHT / logic. Rare, so the per-actor cost is fine.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Landmark")
    TSubclassOf<AActor> ActorClass;

    // OR a plain static mesh (spawned as one StaticMeshComponent — no actor/tick overhead). An emissive
    // material glows at distance without a light. Ignored if ActorClass is set.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Landmark")
    UStaticMesh* InstancedMesh = nullptr;

    // ----- Rarity / spacing (the hash lattice — this is what makes it cheap) -----

    // Average spacing between landmarks, IN CHUNKS. This is the lattice cell size: exactly one candidate is
    // considered per SpacingChunks×SpacingChunks cell, so cost scales with (radius/spacing)². This is also
    // the primary "distance between two instances" control. Large = rare & far apart.
    //   16  → fairly frequent landmarks · 64 → sparse (good default) · 256+ → one every few km
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Landmark|Spacing", meta = (ClampMin = "1.0"))
    float SpacingChunks = 64.0f;

    // How far within its cell a candidate may wander (0 = dead-centre grid, 1 = anywhere in the cell).
    // The effective MINIMUM spacing between two instances ≈ SpacingChunks·(1 − JitterFraction); keep it
    // below 1 to preserve a spacing guarantee while still breaking up the grid regularity.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Landmark|Spacing", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float JitterFraction = 0.5f;

    // Probability that a lattice cell actually contains this landmark (0-1). Combine with SpacingChunks for
    // "rare AND well-spaced": SpacingChunks sets the grid, SpawnProbability sets how many slots fill.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Landmark|Spacing", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float SpawnProbability = 1.0f;

    // How far out (in chunks) landmarks stream / stay visible. CHEAP to make large here (the lattice means a
    // 2048-chunk radius is still only ~(2048/Spacing)² candidates). Set big enough that a massive object
    // never pops in at a jarring distance.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Landmark|Spacing", meta = (ClampMin = "1"))
    int32 StreamRadiusChunks = 256;

    // ----- Placement restriction (mirrors the base decoration gates) -----

    // Optional: only place inside this biome (resolved at the candidate XY). Null = any biome in the strate.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Landmark|Placement")
    UVoxelBiomeDefinition* RequiredBiome = nullptr;

    // Which surface to snap to. Suns typically sit on the sky-cap CEILING; set Floor for ground monuments,
    // Any for the first surface found. Wall-leaning surfaces are matched by the same normal test as decos.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Landmark|Placement")
    ESurfaceType SurfacePlacement = ESurfaceType::Ceiling;

    // Surface-tilt band (deg from flat = acos(|normal.Z|); 0 = flat, 90 = vertical). MaxSlopeAngle rejects
    // surfaces STEEPER than it (90 = no filter); MinSlopeAngle rejects surfaces FLATTER than it (0 = none).
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Landmark|Placement", meta = (ClampMin = "0.0", ClampMax = "90.0"))
    float MaxSlopeAngle = 90.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Landmark|Placement", meta = (ClampMin = "0.0", ClampMax = "90.0"))
    float MinSlopeAngle = 0.0f;

    // Water-relative gate (ignored unless the strate has a water table): place only below (true) / above
    // (false) the water line when bRequireWaterRelative is set.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Landmark|Placement")
    bool bRequireWaterRelative = false;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Landmark|Placement", meta = (EditCondition = "bRequireWaterRelative"))
    bool bPlaceBelowWater = false;

    // ----- Transform tweaks (foliage-style) -----

    // Rotate the object so its up-axis follows the surface normal. OFF by default — a sun usually wants to
    // stay world-upright regardless of the ceiling tilt. ON makes it lie against the surface.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Landmark|Transform")
    bool bAlignToSurface = false;

    // WORLD-space position offset (cm) added after the surface snap. E.g. +Z lifts a sun up off the
    // sky-cap into the open cavern; use X/Y to nudge it off the exact column.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Landmark|Transform")
    FVector LocationOffset = FVector::ZeroVector;

    // Fixed rotation applied on top of the (optional) surface alignment.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Landmark|Transform")
    FRotator RotationOffset = FRotator::ZeroRotator;

    // Per-axis RANDOM rotation range (degrees) — each instance gets a hash-deterministic ±value/2 on each
    // axis (Pitch/Yaw/Roll). 0 on an axis = no randomisation there. Yaw alone = spin variety; all three =
    // tumbled debris look.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Landmark|Transform")
    FRotator RandomRotation = FRotator::ZeroRotator;

    // Uniform scale range (hash-random per instance).
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Landmark|Transform")
    float MinScale = 1.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Landmark|Transform")
    float MaxScale = 1.0f;

    // ----- Render tuning (the InstancedMesh / StaticMeshComponent path) -----

    // Distance (cm) past which the mesh stops drawing. 0 = NEVER cull (the right choice for a far-visible
    // sun). Only affects the InstancedMesh path.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Landmark|Performance", meta = (ClampMin = "0.0"))
    float CullDistance = 0.0f;

    // Whether the mesh casts a shadow. Only affects the InstancedMesh path.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Landmark|Performance")
    bool bCastShadow = true;

    // ----- MINI-SUN LIGHT ORB (feeds the terrain material's raymarched shadows) -----
    // When set, this landmark is also a LIGHT SOURCE: the terrain material marches the density volume
    // toward it for from-the-orb, crisp, dynamic shadows (forward rendering). The visible glowing mesh is
    // still the InstancedMesh/ActorClass above — this just declares the lighting. NOT a UE light actor.

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Landmark|Light Orb")
    bool bIsLightOrb = false;

    // Light colour of the orb.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Landmark|Light Orb", meta = (EditCondition = "bIsLightOrb"))
    FLinearColor OrbColor = FLinearColor(1.0f, 0.95f, 0.85f, 1.0f);

    // Overall brightness multiplier.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Landmark|Light Orb", meta = (EditCondition = "bIsLightOrb", ClampMin = "0.0"))
    float OrbIntensity = 3.0f;

    // Orb emitter radius in VOXELS (visual/softness reference; falloff origin).
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Landmark|Light Orb", meta = (EditCondition = "bIsLightOrb", ClampMin = "0.0"))
    float OrbRadiusVoxels = 16.0f;

    // Distance in VOXELS over which the orb's light falls to zero. YOU author this (no inverse-square
    // blowout) — bigger = lights a wider area. (1 voxel = 25 cm.)
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Landmark|Light Orb", meta = (EditCondition = "bIsLightOrb", ClampMin = "1.0"))
    float OrbFalloffVoxels = 2000.0f;

    // Max distance in VOXELS along the shadow ray we test for occlusion (bounds the per-pixel march cost;
    // past this the point is treated as lit). Keep ≤ the level-0 volume reach for crisp contact shadows.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Landmark|Light Orb", meta = (EditCondition = "bIsLightOrb", ClampMin = "1.0"))
    float OrbMaxShadowDistanceVoxels = 1000.0f;
};

/**
 * FStrateAmbientActor — An actor that spawns in open cave space.
 *
 * Unlike decorations (placed on surfaces), ambient actors float in the void:
 * fog volumes, particle emitters, light sources, floating crystals, etc.
 */
USTRUCT(BlueprintType)
struct VOXELFORGE_API FStrateAmbientActor
{
    GENERATED_BODY()

    // The actor class to spawn
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ambient")
    TSubclassOf<AActor> ActorClass;

    // Probability of this actor spawning per chunk (0-1)
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ambient", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float SpawnChancePerChunk = 0.1f;
};

/**
 * FStrateCreature — A creature/enemy type that can spawn in this strate.
 *
 * Creatures are managed by a future spawning system that respects
 * per-strate population limits and weighted random selection.
 */
USTRUCT(BlueprintType)
struct VOXELFORGE_API FStrateCreature
{
    GENERATED_BODY()

    // The creature actor class
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Creature")
    TSubclassOf<AActor> ActorClass;

    // Maximum number of this creature type alive in the entire strate at once
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Creature")
    int32 MaxPerStrate = 5;

    // Relative spawn weight (higher = more likely to be chosen when spawning)
    // If two creatures have weights 2.0 and 1.0, the first spawns 2x as often
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Creature")
    float SpawnWeight = 1.0f;
};
