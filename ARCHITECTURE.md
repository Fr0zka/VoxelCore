# VoxelForge — Architecture & Design Deep-Dive

> The 2026 redesign in detail: archetypes, the (0,0) spine, disturbances, content/atmosphere,
> biomes, and the **performance invariants** (the `§8.10` "don't regress" list). Read this
> before touching generation / strates / passages.
> Navigation, file index & conventions live in [CODEMAP.md](CODEMAP.md). Section numbers (`§8.x`)
> are preserved so existing cross-references keep resolving.

## 8. Archetypes, spine, disturbances, content & carving (2026 redesign)

A large A-to-Z expansion. The world is a stack of strates the player descends through;
each strate can be a fundamentally different *archetype*, connected at (0,0).

### 8.0 Human-scale generation contract (2026-09-06)

All authored lengths are voxel-space values, converted at the boundary by `0.25 m/voxel`. The body
reference is a **0.34 m radius, 1.76 m tall** capsule (`1.36 × 7.04` voxels). The default strate
height is now **8 chunks = 256 voxels = 64 m**, which is intentionally larger than the 3-4 chunk
boss-arena anchor so an archetype can reserve seals, vertical play, and a high ceiling together.

The default size families are deliberately independent:

| Feature | Default metre derivation | Coupling kept sound |
|---------|--------------------------|---------------------|
| Room network | radius 4-10 m (8-20 m diameters), origin radius 12 m (24 m hub), cell spacing 32 m | `MaxRoomRadius 40 < RoomSpacing/2 64`; a 6 m radial margin remains around the largest room |
| Round passages | 1.5-2 m radius (3-4 m bores); maze corridor radius 3.125 m (6.25 m nominal bore) | `0.7 × 2 × (12.5−3.75) × 0.25 = 3.0625 m` usable floor after worst roughness; Maze `12.5 < CellSize/2 32` |
| Slab | floor at 16 m, ceiling at 38.4 m, 22.4 m open span | floor/ceiling noise and 4-8 m column diameters remain inside the open span |
| SurfaceWorld | 20 m terrain relief, 12 m warp, 60.8 m sky cap | terrain features are bounded below the 64 m strate envelope |
| VerticalShafts | 4-7 m shaft diameters, 3 m connector bore, 8 m ledge interval | shaft spacing is 20 m; connector/ledge values are derived from the roughness envelope |
| FloatingIslands | 12-24 m island diameters, 28 m cell spacing | the largest island leaves a 2 m centre-to-edge gap; thickness is separately derived |
| Inter-strate passages | 3-4 m mouth bore, 3 m mid bore, 8-24 m reach, 16-48 m offset | width, reach, and wander are separate body/placement decisions |

For wall detail, `VOXEL_NOISE_SCALE=1.25` and the proven `sup|fBM|=1.5` give a roughness reach of
`1.875 × SurfaceRoughness` voxels. The default ratios are **0.625** against the minimum tunnel
radius, **0.300** against the Maze corridor radius, **0.469/0.625** against shaft/connector radii,
**0.234** against the minimum island radius (**0.484** including its 6-voxel blend), and
**0.625/0.938** against slab column radius for floor/ceiling roughness. SurfaceWorld is a
heightfield, so its **0.047** ratio is against its 80-voxel relief scale, not a fictitious radial
feature. FlatPlain and CrystalChamber intentionally share `BuildSlabStack` and the same slab
parameter block; they cannot acquire separate scale geometry without a generator change.

This scale contract is informed by [Epic's Unreal level-blockout guidance](https://dev.epicgames.com/documentation/en-us/unreal-engine/designer-01-project-setup-and-level-blockout-in-unreal-engine), which recommends a player-sized reference and gives 2-3 m hall / 3-4 m height starting guidance. The
[2010 ADA Standards](https://www.ada.gov/law-and-regs/design-standards/2010-stds/) are a lower-bound
accessibility reference (915 mm clear walking width and larger passing/turning spaces), while
[NPS Lehman Cave dimensions](https://www.nps.gov/grba/learn/nature/lehman-caves-dimensions.htm) support
keeping natural chambers heterogeneous and allowing 20 m-plus cathedral volumes.

Validation is deliberately split by cost: bounded regression fixtures remain 4 chunks, while the
production definition, season defaults, and owner-facing showcase use 8 chunks. The historical
pre-tree production-height sweep exhausted 256 rolls and found **0/8** complete hard-gate survivors;
its 7 fine renders, 1 refusal, and 1 blank are retained as the scale baseline. The historical final
`VoxelForge` run (`VoxelForgeFullMazeTreeFinal3`) recorded **30 clean successes, 2 warning-bearing,
0 failures, 0 not-run** in **2,523.978 s**; its regenerated showcase produced **6 fine renders, 2
refusals, and 0 blanks**. All box-verdict violation counts were **0**, `WorldRadiusVoxels` stayed
**0**, lateral regions stayed gated, and operator-stack equivalence paths remained bit-identical.
The 2026-09-06 landing validation (`VoxelForgeLandingFinal5`) recorded **30 clean successes, 2
warning-bearing successes, 0 failures, 0 not-run** across 32 tests in **3,607.746 s**. The strict
`Composer.Promotion` assertions remain intact and passed; no assertion was weakened. The landing,
connectivity, seal, box-soundness, determinism, and equivalence tests passed. The two warnings are
the known gated lateral-region result and the known TunnelNetwork outside-gate sample count; they
are not landing or box-verdict violations.

### 8.1 Archetypes (`ECaveGeneratorType`, VoxelStrateTypes.h)
Each archetype has its own param `USTRUCT` (on `UVoxelStrateDefinition`, EditCondition-gated
by `GeneratorType`) and its own **operator stack** builder in `VoxelDensityOpStack.cpp`, selected per
chunk by `VF_BuildOpStackForChunk` in `GetDensityAt` (the same factory `ClassifyTile` uses). The stack
is the only density path; a degenerate (zero-height) strate has none and is open air.

| Archetype | Params struct | Stack builder | Idea |
|-----------|---------------|------------|------|
| TunnelNetwork | `FStrateGenerationParams` | `BuildTunnelNetworkStack` (fused evaluator: `GetDensityWithParams`) | rooms+tunnels (original) |
| FlatPlain / CrystalChamber | `FSlabGenerationParams` | `BuildSlabStack` | floor/ceiling void (original) |
| Maze | `FMazeGenerationParams` | `BuildMazeStack` | origin-directed spanning-tree corridors on a 3D lattice, with capped loop edges and a thread-local per-cell capsule cache |
| SurfaceWorld | `FSurfaceGenerationParams` | `BuildSurfaceStack` | heightfield terrain: domain-warped continents+ridged mtns+detail, a low-freq **relief map** (`M`) that scales mountains/elevation for plains↔highland variety, **F20 heightfield terrain ops** (`Surface|Ops` — all default off ⇒ byte-identical): **Cliff** (slope-gated steepening — where the analytic structural slope > `CliffSlopeThreshold`, push the height away from the local mean by `CliffSharpness` ⇒ gentle slopes become sheer walls/canyon faces that hug real steep ground, gentle areas untouched; 4 structural resamples only when enabled), **Terrace** (relief-gated plateau quantize + `TerraceHardness` soft-round↔crisp-mesa), **LayerLines** (sedimentary sine shelves, slope-expressed) — pure per-column height remaps in the single oracle `ComputeSurfaceTerrainZ` (`SampleSurfaceStructuralZ` = pre-op raw height, re-sampled at an XY offset for Cliff's slope), biome-selected + border-blended for free via each biome's `SurfaceParams` + the height output-lerp; plus **phase-2 Overhang** (the first VOLUMETRIC op — real jutting shelves like a cliff lip): in `FOverhangShelfMod`, for AIR voxels in the window `(TerrainZ, TerrainZ+OverhangHeight]` above a steep slope, the heightfield is re-sampled UPHILL (toward the cliff) by a reach that GROWS with height and unioned in. Low in the window the shift is ~0 (borrows nearby low rock ⇒ stays air over the void); high up it reaches the far cliff (solid) ⇒ a shelf attached to the cliff, tapering out over the void with air underneath. Per-column `OverhangAmp`(=strength·slope-gate) + unit uphill dir `(DirX,DirY)` are resolved once in `ComputeSurfaceColumn` — the gradient sampled at the REACH scale (`OverhangReach`) so a point out over the void can "see" the cliff to know which way is uphill — and cached on `FSurfaceColumn`. It's genuine 3D (per-voxel structural re-eval), so it's gated hard to steep overhang columns; the union only ADDS rock (never removes), capped at `TerrainZ+OverhangHeight`, so `ClassifyTile` forces Mixed only in `(TerrainZ, TerrainZ+OverhangMargin]` (upward-only; margin = max `OverhangHeight`) — a shelf never holes a trivially-skipped tile, and only the thin cliff-edge band of air tiles is woken (NOT far-field like phase-3 spikes/holes). Overhang undersides classify as ground rock by the F17 rule (down-facing but below `TerrainZ`). Known v1 limit: applies at all LODs (Step-agnostic) — may alias far; gate to fine tiles later. Beaches at water line, high sky-cap ceiling. The cap is shapeable terrain in its own right (`ComputeSurfaceCeiling`, `Surface|Sky` params: `CeilingUndulation` broad inverted hills/valleys, `CeilingRidgeStrength` hanging ridgelines, `CeilingRoughness`+freq fine bumps, `CeilingWarp*`) — defaults (strengths 0, freq 0.04) = old flat-ish cap. (`Surface|Macro` params = the cheap precursor to biomes; `ReliefStrength=0` ⇒ old uniform terrain.) **Sky-cap vs ground is a PER-TRIANGLE surface class (F17), not a per-tile verdict**: the mesher classifies each unique vertex on the **worker** — only down-facing verts (`N.Z<-0.1`) pay a memoized `GetSurfaceHeightAt` column query; nearer `CeilSurf` ⇒ sky-cap, nearer `TerrainZ` ⇒ terrain overhang stays ground (a future SurfaceWorld cave roof — down-facing but below `TerrainZ` — also lands ground by the same rule; non-surface strates always ground). Triangles take the majority class of their 3 verts and are packed as **two contiguous index runs** (ground then cap, `FVoxelMeshData::NumCeilingTriangles`; skirts inherit their source triangle's run) → RMC **polygroups 0/1** → one **section per non-empty group** (`ApplyMeshToTile`, material slot = group index): slot 0 = strate `OverrideMaterial`/default (min-corner chunk), slot 1 = `CeilingMaterial` resolved at the tile's **TOP chunk** (mid as gap fallback, then min; fallback: ground material) — a coarse tile is 2^level chunks tall, so its min corner can sit in a lower strate/gap while the cap belongs to the strate above (this was the "far cap = ground material" residue), so the shadowless overhead rock is tinted separately instead of reading flat/bright. Shadow is **per section** via `FRealtimeMeshSectionConfig::bCastsShadow` (NOT the component `SetCastShadow` — RMC's proxy ignores the component flag; this is also why level≥2 far tiles only stopped casting once the section flag was wired): ground casts at level≤1, the cap section never casts, so the rock ceiling never shadows the terrain below it. History: v1 was a game-thread centre height-oracle (misclassified coarse far tiles → terrain material on the cap underside); v2 a whole-tile worker normal VOTE — which painted **mixed coarse tiles** (one far tile spanning terrain AND cap) entirely with the winner's material, and put sky material under terrain overhangs. The per-triangle class fixes both and is the identity channel caves/F8 will reuse. |
| VerticalShafts | `FVerticalShaftParams` | `BuildVerticalShaftStack` | full-height shafts + horizontal connectors + partial ledges |
| FloatingIslands | `FFloatingIslandParams` | `BuildFloatingIslandStack` | asymmetric islands: flat land top + underside tapering to a point, lobed (domain-warped) outline, in an open void |
| Underwater | `FStrateGenerationParams` + water | (reuses `BuildTunnelNetworkStack`) | tunnel rock + high water table |

All stacks share the convention: internal **positive=solid**, and `AppendStructuralPost` ends every
stack with origin spine → vertical boundary seal → inter-strate passage tube + landing/floor → XY
edge seal; `EvalMC` negates once (MC: negative=solid). The final MC-facing edge pass in `GetDensityAt` is repeated after disturbances so
the global rim cannot be reopened by a post-process; the diff layer remains the explicit player
override.
StrateManager provides params per chunk via `GetMaze/Surface/VerticalShaft/FloatingIslandParamsForChunk`
(macro `VF_ARCHETYPE_PARAMS_GETTER`) — the ordinary authored path has no cross-boundary blend
(Hard transitions between strates). The offline composer’s lateral manifest is the explicit
within-strate exception described in §8.1a; it blends evaluated density fields, not these params.
On top of the archetype, an optional **biome** layer (§8.14) modulates terrain & content WITHIN a
strate via a window-invariant XY field — currently wired into SurfaceWorld.

### 8.1a Lateral regions (Tier 4d)

An archetype is now allowed to be a **spatial region inside one strate**, rather than a label that
describes the whole strate. The offline composer rolls one to three region records. Each record
owns its own native parameter vector and, for structure candidates, its own recipe; region zero is
also retained as the compatibility identity. A one-region record takes the existing stack path.

The partition is a pure function of `(world XY, world seed, absolute strate index)`. A 256-voxel
hash lattice places jittered sites (`VoxelHash::Cell`), each site receives a deterministic region
id, and the nearest-site assignment is Voronoi-like. Near a bisector, the nearest different-region
site supplies the neighbour; its weight is the same linear boundary curve used by the vertical
transition, split symmetrically so both sides meet at 0.5. The parent lerps the two **evaluated
internal densities**, never their parameter vectors: a shaft spacing and a slab parameter are not
commensurable. Internal density is positive solid; the parent is negated once at the MC boundary.

Region cores are built without structural posts. `FLateralRegionBlendOp` resolves the creative
field, then one parent appends the global spine, vertical seal, passage carve/landing floor, and XY edge seal in
the existing fixed order. No region can disable or duplicate those posts. The manifest is an
offline artifact and the editor hand-off copies it into a worker-local per-chunk cache: the chunk
prepares a small lattice-site window plus an integer XY query grid, so ordinary voxel evaluation is
an O(1) lookup rather than a seed search or allocation. Fractional gradient probes reuse the same
prepared site window.

The lateral `ClassifyBox` proof is deliberately one-sided. It uses a certified nearest-different
site from the bounded local lattice window plus a 2-Lipschitz bound over the box's XY radius; only
a strict lower bound greater than the blend width proves that one region and no blend band can touch
the box. If the bounded query cannot certify that neighbor, proof is refused. Every other box asks
every region stack; any `Mixed`, disagreement, or unknown result remains `Mixed`. Convex density blending means
that equal all-air/all-solid signs are safe, but no cross-region uniformity is inferred from a
single sample. The same key is checked when a season manifest is loaded, and `WorldRadiusVoxels`
remains zero during offline materialisation.

This mechanism is implemented and instrumented, but not ready to promote: the pre-landing
16-seed passage-mouth audit reported **9/16** connected opposite-region pairs. After the landing
pass, the current audit reports **4/16**, while the exact region-zero controls also report **4/16**.
The same cases are **16/16 valid, non-vacuous, and above the 0.50 largest-component survival
threshold** before the law gate. No corridor or threshold tuning was added. The failed primordial-law
gate is therefore retained as a design blocker; see `COMPOSER-NOTES.md §3.2b` and the validation log
in §10.

The operator-stack design lives in `Docs/archive/OPSTACK-PLAN.md` /
`Docs/archive/OPSTACK-DECOMPOSITION.md`, the symbol index in `CODEMAP §3.2d` — not repeated here.
What matters for *this* document: the archetype table describes what the world IS.

### 8.2 (0,0) spine & hybrid connections
- `ApplyOriginSpine` (VoxelGenerator.cpp, static helper) builds a finite, rounded landing room with
  a flat support floor at XY (0,0) in every strate's **interior**. It no longer carves a continuous
  vertical column: the old column supplied open air but no standable surface except at its bottom.
  Room half-width and height reuse the §6.5 capsule formulas; the room and slab are clamped away
  from both vertical seals. Radius = `UVoxelGenerator::OriginSpineRadius` ←
  `VoxelSettings::OriginSpineRadius`. Called before every `ApplyBoundarySeal`.
- `VerticalShafts` treats that structural column as a **connector endpoint**, not as a second
  density primitive. A thread-local rebuild collects a direct-indexed geometric halo once (with
  the stock settings: 9×9 tree-emission cells and a 15×15 roll), emits tree links only for the
  inner 3×3 cells, and resolves each parent from its complete ±2-cell candidate window. Each
  shaft chooses the nearest shaft whose jittered distance to (0,0) is strictly smaller; ties use
  the lowest cell index. If that window has no parent, the deterministic ±3 lower-origin neighbour
  fallback fires; a window touching the origin uses the spine directly, with a final direct-spine
  fallback for an unusually empty finite halo. The wider collection is therefore fully contained
  for every emitted shaft and the parent is independent of chunk or evaluation-cell boundaries.
  Every parent edge strictly decreases origin distance or terminates at the spine, which proves
  that no shaft is orphaned or cyclic. The existing probabilistic `VoxelHash::Pair` links and
  `Spacing*1.6` cutoff remain as texture and loops; they are additive, not the connectivity law.
  Tree links use a radius strictly above the proven `sup|FBM|=1.5` roughness envelope
  (`max(ConnectorRadius, SurfaceRoughness*VOXEL_NOISE_SCALE*1.5 + 1)`) and choose a shared
  ledge-free Z interval. `BuildVerticalShaftStack` mirrors the same tree, random links, windows,
  hashes, radius, and connector-Z rule; `VF_ApplyOriginSpine` remains the sole owner of the
  vertical column itself.
- The seals between rooms remain closed until progression opens the next connection. The one
  optional above-ground opening is the top room's ceiling through strate 0
  (`GeneratePassages`, `bOpenSurfaceEntry`); it never bores the lower rooms together.
- **Base connection:** the default inter-strate passage is a small, single-file, two-leg
  walkable switchback with a named 15° floor-gradient cap and final-density floor/capsule audit.
  A 32 m drop needs 119.4 m of horizontal run at that gradient, so a single direct ramp is not a
  plausible default. The switchback is explicit geometry, not a fall shaft.
- **Hybrid extras:** auto-carved passages per boundary, placed away from (0,0). The upper
  strate's `PassageConfig` still drives count/style/shape; the legacy vertical/spiral/cascade/
  crack styles remain descriptors without the base walkability guarantee.

The tree is a construction guarantee for the shaft field: every seeded shaft has one inward edge,
and every chain strictly decreases distance to the origin until it reaches a local minimum that
links to the spine. The fallback resolver preserves that proof, so no shaft is an orphan or part
of a cycle. It does not alter passage placement. The acceptance measurement remains the actual
arrival→departure flood-fill at the independently placed passage endpoints.

### 8.2a XY edge seal (bounded-world invariant)
`UVoxelSettings::WorldRadiusVoxels` and `EdgeSealThickness` define the bounded world in actor-space
XY around `(0,0)`. The default radius is 8192 voxels (~2.05 km at 25 cm/voxel) and the default
solid ramp is 64 voxels. A radius of **0 is a true no-op**: the hot path returns before doing any
arithmetic, so the legacy unbounded field is unchanged. From `R - Thickness` to `R`,
`VF_ApplyXYEdgeSeal` raises the internal positive-solid density with the same `SmoothStep01` ramp
idiom as the vertical seal; at and beyond `R` it forces positive `BaseDensity`. The generator
negates once for marching cubes, so the externally visible sealed density is negative (solid).

This is the fourth structural post and is appended automatically by
`FVoxelOpStack::AppendStructuralPost`, after the spine, vertical seal, and passage tube + landing
floor. Passage carving deliberately remains before the edge seal: the tube is the intentional
vertical-boundary crossing, while each landing/connector is clamped inside the strate's vertical
seal. A passage whose mouth, landing, or body reaches the XY rim is overwritten by the edge force
and cannot open a hole through the world boundary. The legacy density functions apply the same post
in the same order. `GetDensityAt` reapplies the MC-facing landing air/floor after disturbances as an
idempotent support backstop, before the final XY seal; player diff edits still run last by design
and remain authoritative.

`FXYEdgeSealOp` is `IsXYPure() == true`, uses only a squared radial early-out in the common interior
case, and returns `AllSolid` from `ClassifyBox` only when the closest point of the whole box is at
least one voxel into the ramp (or beyond `R`). Its `ForcedMarginOverBox` reports the actual positive
lower bound, conservatively reduced after the one-voxel proof margin; it never treats the
mathematical inner edge as solid. The same proof is used by the global `ClassifyTile` shell guard,
so tiles wholly in the proven outer band can skip T1.d generation/collision work. A box touching the
band but not meeting that proof remains `Mixed`.

### 8.3 Disturbance layer (the "wow" post-process)
`FStrateDisturbanceParams` (on the definition, all archetypes). `ApplyDisturbances`
(VoxelGenerator.cpp static, **MC convention**) runs in `GetDensityAt` after dispatch:
chasms (carve air), bridges (solid spans), ridges (solid blades). The XY edge seal is reapplied after
this layer, so disturbances cannot reopen the outer shell; the layer otherwise stays inside the
vertical seal bands.
Provided per chunk by `StrateManager::GetDisturbanceParamsForChunk`.

### 8.4 Cross-chunk determinism (the seam-prevention invariant)
`BuildChunkCache` (VoxelCaveMorphology.cpp) uses **two regions**: a wide *COLLECT* region
(`2*MaxTunnelLength + MaxInfluence`) over which connectivity is decided (NN filtered to
`<= MaxTunnelLength`, origin cap = deterministic top-N by hash), and a tight *STORE* region
(`+MaxInfluence`) kept for per-voxel eval. This makes the room/tunnel graph window-invariant.
**If you add a connectivity rule with longer edges, the COLLECT region must still cover the
max edge reach, and decisions must not depend on the stored window.**

#### Maze topology — local spanning tree, optional loops

Maze uses a different, strictly local connectivity contract. A non-origin lattice node `(x,y,z)`
hash-selects one parent from the axes whose coordinate is non-zero, taking one step toward
`(0,0,0)`. The parent step reduces `|x|+|y|+|z|` by one, so the infinite undirected parent graph
has one root, no cycles, and every cell is reachable by construction. A canonical lower-node plus
axis edge is open when either endpoint selects the other as parent. Horizontal and vertical loop
edges are optional visual detail, capped at `0.18 * BranchProbability` and `0.10 * Verticality`;
they are never needed for the connectivity proof.

The evaluation window is exactly the lower nodes in `{-1,0}³`, an 8-node / 2×2×2 local halo. The
edge predicate checks the adjacent `+1` endpoint's parent locally, so it does not need a wider
collect region and cannot disagree at a chunk boundary. `MakeLatticeCorridorSource` rebuilds those
decisions in its thread-local per-cell cache; the
voxel loop evaluates only cached capsule SDFs. This is deliberately independent of
`WorldRadiusVoxels` (which remains `0`) and the still-gated lateral-region system.

The editor composer walk-through preserves this invariant by avoiding a layout rebuild. AVoxelWorld::ApplyComposerCandidate
pauses and drains active generation, installs one temporary override on an existing slot, increments
the manager's layout/passages version, and then reuses RegenerateAllChunks to bump GenerationEpoch.
The slot's Z range and passages therefore remain stable; worker tasks copy the immutable override during
their versioned refetch. A parameter candidate uses the production native stack mapping, while a
structure candidate uses `VF_BuildStackFromRecipe`. Custom recipes now classify with that exact
materialised stack; slot/gap crossings, diffs, and disturbances still fail closed to `Mixed`, and
accepted uniform verdicts are brute-force checked by `VoxelForge.Composer.Season`. The bridge and its editable properties are
WITH_EDITOR/WITH_EDITORONLY_DATA only and do not exist in a shipping build.

### 8.4a Cooked seasons — composition stays offline, evaluation ships

`UVoxelSeasonAsset` is a `UPrimaryDataAsset` imported from the diffable schema-2
`season_manifest.json`. The asset embeds the reviewed JSON, independently copies its seed, season,
spine/radius and SHA-1 content hash, and retains cook references to authored fixed definitions.
Runtime parses and verifies both hash copies before accepting the season. An assigned invalid season
fails closed; it never falls back to an unrelated authored pool.

When `UVoxelSettings::Season` is assigned, `UVoxelStrateManager::Initialize` keeps its existing
half-built-layout/`GeneratePassages` ordering but feeds it the manifest's exact ordered slots, bounds,
archetypes, vectors, per-strate seeds and recipes. `UVoxelGenerator` copies an immutable recipe/vector
on the existing `(owner, chunk, layout version)` refetch and materialises `FVoxelOpStack` locally.
No rolling, corpus load, measurement, selection, promotion, or review code is compiled for Shipping.
When Season is unset, the authored fixed/pool shuffle path is unchanged.

Schema 2 is deliberately density-focused. A fixed slot may retain an authored definition path as its
content/visual/passage bag; a generated slot has deterministic empty/default wider content. Because
passage configuration is not stored, generated slots set `Connections=0` instead of inventing the
UObject default passage that was never measured; the origin spine remains. Lateral-region manifests
are rejected while `VF_LateralRegionsAreShippable()` remains false (measured cross-seam law 9/16).

### 8.5 Content scatter & water — `VoxelContentManager.h/.cpp` (NEW)
`UVoxelContentManager` (owned by `AVoxelWorld`, game-thread). TWO INDEPENDENT subsystems:

**(A) DECORATIONS — distance-based WORLD GRID (the no-pop system, 2026-06-17).** Decorations are placed
on a fixed world XY cell grid (**1 cell = 1 chunk footprint**, `DECO_CELL_VOXELS = CHUNK_SIZE`) and streamed
by DISTANCE from the player, **fully decoupled from clipmap tiles / LOD**. **THE MARCH RUNS ASYNC ON WORKER
THREADS** (mirrors mesh gen — `GetDensityAt` is thread-safe; the synchronous-on-game-thread first cut was a
perf disaster + starved streaming → seams, so it was moved off-thread). Driven by `AVoxelWorld::Tick →
UpdateDecorations(playerWorldPos)`, three phases: **(1)** recompute the desired cell set (`RebuildDesiredCells`)
only when the player crosses a cell boundary OR changes strate — clears out-of-range loaded cells, queues
cells that are NOT loaded and NOT in flight (`PendingLaunch`, nearest-first). A loaded region is NEVER
re-streamed in place while it stays in range (only cleared when it leaves), so decorations don't FLICKER as
the player moves. **TWO STREAMING GRIDS (`FStrateDecoration::StreamTier`, 2026-06-26):** the stream RADIUS is a
property of the GRID, never of an entry — mixing radii inside one grid would force an in-place re-stream when
the player crosses an entry's radius (the original tier system's flicker bug). So there are exactly two
self-contained grids (`FDecoGrid`, each with its own region map + builds + queues + HISMs): **NearGrid**
(`DecorationNearRadiusChunks`, FINE `DecorationSpacingVoxels` column grid — dense groundcover near the player)
and **FarGrid** (`DecorationRadiusChunks`, COARSE `DecorationFarSpacingVoxels` column grid — trees/landmarks/RARE
props visible everywhere; the coarse grid is what makes a rare prop cheap, since the worker march cost scales
with column count). An entry picks a grid via `StreamTier` (default Far); the palette is partitioned by tier so
each grid marches only its own subset. A given world XY is covered by a Far region always, plus a Near region
when close (separate HISMs → crossing the near boundary never touches the far region). **(2)** `LaunchDecoTasks`
(per grid, sharing ONE `MaxConcurrentDecorationTasks` budget — each throttles against the other's in-flight count):
snapshot the update's decoration palette (built once on the GAME thread — biome, see below) then fire an async `UE::Tasks` march
(`BuildCellSpawns`, `BackgroundNormal`, capped at `MaxConcurrentDecorationTasks` in flight via `InFlightCells`).
**(3)** `ProcessDecoResults`: drain finished cell marches (`DecoResults`, `Mpsc`) and fold each into its
grid's region build (`MergeCellResult`; a result whose build is gone or whose `BuildId` no longer matches —
region cleared + re-marched — is discarded), then **apply completed regions budgeted**
(`MaxDecorationCellsPerFrame` regions/frame via `ApplyRegion` — the only game-thread cost; a region that left
range while it marched is dropped unbuilt).
`BuildCellSpawns` (worker) finds each column's surface point(s) and rolls the entries there (shared
`PlaceAtCrossing`). Candidate columns are **snapped to INTEGER voxel XY** (integer jitter) so the generator's
surface-column cache (T1.a, §8.10) applies — FRACTIONAL XY bypasses it and recomputes the noise-heavy
heightfield+biome on every sample. **TWO column strategies by archetype:**
**(a) SurfaceWorld → HEIGHT ORACLE (`Ctx.bSurfaceWorld`), NO marching.** `Generator::GetSurfaceHeightAt(x,y,
chunkZ → TerrainZ, CeilSurf)` returns the heightfield surface + sky-cap ceiling in O(1) (it shares the density
path's `ResolveSurfaceChunkParams`/`ComputeSurfaceColumn` via its own thread_local per-chunk cache, so it's
bit-identical to the rendered ground). Per column: query centre + 4 neighbours (gradient → floor/ceiling
normals), place a Floor crossing at `TerrainZ` and a Ceiling crossing at `CeilSurf` (if open space below). A
single `GetDensityAt` at the surface verifies the column isn't CARVED (passage/spine/diff make it air → skip;
the oracle is the raw heightfield and doesn't know carving). ~5 height evals + 1-2 density samples/column vs
hundreds marched. **(b) other archetypes (caves/shafts/islands) → ray-march** the strate Z-band
(`GetStrateUnrealZRange`, voxel coords) via `GetDensityAt` at a COARSE step (`DecorationMarchStepVoxels`), each
air↔solid sign change **bisection-refined** (4 iters → accuracy independent of step). Either way **a prop sits at
the SAME world position at every LOD → no pop**. (march) The top cap/seal + the open air are always marched first; the scan only stops after
`DecorationColumnDepthVoxels` of CONTIGUOUS solid once it has ENTERED the open space (trims dead bedrock below
the ground without ever stopping short of it — a "below the first crossing" cap was wrong: on a surface world
the first crossing is the high CEILING, so it stopped mid-air before reaching the ground = no floor props).
Outward normal = normalized density gradient (solid→air, matches the mesher), classified
Floor/Wall/Ceiling by `normal.Z`. Each crossing rolls every `FStrateDecoration` independently: surface-type,
density gate (`DecoHash(cell,column,crossing,entry,seed)`), water-relative, align/yaw/scale, per-cell
`MaxPerChunk` + global actor cap → a `FDecoSpawn{EntryIdx, bInstanced, Xf}`. The game thread spawns from the
result's `Entries` snapshot. `DecorationMaxCrossingsPerColumn` caps cave columns (surface worlds have 1).
**Shutdown:** `NotifyShutdown()` (called from `AVoxelWorld::EndPlay`) flags + spin-waits on the in-flight
task count before UObject teardown (tasks read the Generator); `BeginDestroy` is the backstop. **Determinism:**
pure hash of (cell, column, crossing, entry, seed) + the density surface snap. **Decorations exist ONLY in
the player's current strate** (march is strate-bounded) → a strate change wipes + rebuilds them, and there
is **no cross-strate light bleed to cull**.
**Render paths:** `ActorClass` → real actors (lights/logic, pricey game-thread spawn); `InstancedMesh` → HISM
(no tick/actor/collision, emissive glows far), per-cell-per-entry. **Per-entry HISM tuning for dense groundcover**
(`FStrateDecoration`, only the InstancedMesh path): `CullDistance` (cm; 0 = no cull — the lever that makes dense
grass affordable: placed thickly, drawn only near → GPU cost bounded by area-within-cull, NOT the stream radius),
`bCastShadow` (default true; turn OFF for grass — dense instanced shadows are the dominant foliage cost),
`MaxSlopeAngle` (deg from flat = acos(|N.Z|); 90 = no filter, ~35 keeps grass off cliffs — applied in the worker's
`PlaceAtCrossing`). **Placement-constraint gates (all in `PlaceAtCrossing`, deterministic, zero-cost at defaults):**
`MinSlopeAngle` (lower companion to Max — band a prop onto a tilt range, e.g. 30..70 = slopes only),
`bWallExcludeOverhangs` (wall-only-upright: drop normals with N.Z < 0 so downward overhangs don't take wall props).
**Shared vocabulary (2026-07-06):** these gates + spawn/transform/render fields now live on `FPlacementProfile`
(embedded as `Profile` on `FStrateDecoration` and `FStrateLandmark`), so both scatter primitives are
authored identically. Rotation unified to `RotationOffset` (fixed) + `RandomRotation`
(per-axis hash-random) — decoration's ctor defaults `RandomRotation.Yaw = 360` (full random heading, replacing the
old `bRandomYaw`/`MinYaw`/`MaxYaw`; banded yaw = offset + a smaller random range). Distribution is unchanged; the
exact per-instance yaw values reshuffle once (different hash mix).
**F7 AWARE PLACEMENT (2026-07-06):** `FPlacementProfile::Conditions` = a list of `FTerrainCondition`
DERIVED PREDICATES (relief / moisture / biome-border weight, each an inclusive [Min,Max] band, optional
invert), AND-ed and evaluated by `Generator::EvaluateTerrainConditions` at the candidate XY — "conditions,
not annotations": nothing is stored, the phenomenon is queried from the analytic fields (SampleRelief /
SampleMoisture / SampleBiomeAt) on demand, so it's deterministic + worker-safe (the caller hands over the
strate's already-resolved `FBiomeContext`, so no re-resolve). Opt-in per entry (empty list = zero cost);
wired into both the deco worker (`BuildCellSpawns`) and landmark placement (`SpawnLandmarkInstance`). This
is the shared core of the coming quest `FindFeature` locator (same predicate, run as an outward search) and
the landmark anchor gate. The BP bridge `AVoxelWorld::GetVoxelSurfaceHeightAt` exposes the
trace-free deterministic ground/ceiling height so authored ruin/set-piece Blueprints self-arrange on the
real surface before it meshes. Next types (water-edge band, relief-peak local-max, slope) are additive.
**F7 COMPANIONS (relational decoration, `FDecoCompanion`):** each `FStrateDecoration` may list `Companions`
(satellites: rocks/mushrooms around a tree). When `PlaceAtCrossing` emits a parent spawn, it immediately rolls
each companion (probability → count in `[CountMin,CountMax]` → disk offset `RadiusMin/MaxVox`), all hashed off
the PARENT's hash `H` → a pure function of the parent, so NO "did a tree spawn here?" search. Each satellite offsets from the parent in the XY plane (voxel space) and then, by default (`bSnapToSurface`),
**re-snaps to the real surface at its OWN XY** via `FindLandmarkColumn` (cheap `GetSurfaceHeightAt` oracle on
SurfaceWorld, a short ray-march in caves; worker-safe — reads only `Gen`) — this kills floaters on uneven
ground; a satellite that finds no surface at its spot is skipped. Turn `bSnapToSurface` off to inherit the
parent's exact height+normal (cheapest, flat ground only). Satellites can also be **gated by their own
`Profile.Conditions`** evaluated at the satellite XY (e.g. a unique mushroom only at a biome border). They carry
their own `Profile` transform/render. `FDecoSpawn` gained `CompanionIdx` (−1 = the entry; else index into
`Companions`) so `MergeCellResult`/`ApplyRegion` resolve the satellite's mesh/actor + render tuning from
`Companions[ci].Profile` (the region mesh bucket now stores an `FPlacementProfile`, not a whole entry). **Two levels:** a companion may carry `SubCompanions` (`FDecoSubCompanion` — a distinct type, since UHT can't
reflect a self-recursive `FDecoCompanion`) that spawn ON each level-1 satellite (moss on a rock); level-2
**inherits the L1 satellite's snapped point** (no re-snap — the cost lever that keeps nesting cheap) and can
still gate on its own `Conditions`. `FDecoSpawn::SubIdx` routes L2 back to `Companions[ci].SubCompanions[sj]`.
A hard **per-parent budget** (`GMaxCompanionsPerParent`, 256) caps the total L1+L2 count so a misconfiguration
can't blow up regardless of authored counts. Bounded (count × parents, ≤ budget), deterministic, streams inside
the existing two-grid deco system untouched. Depth beyond 2 (and per-entry references for arbitrary nesting) is
the remaining flagged follow-up.

**F7 SET-PIECES — FOLDED INTO LANDMARKS (2026-07-06).** Set-pieces (ruins/shrines/monuments) and landmarks
(mini-suns) had no real reason to be separate once both shared the spawn core and gained `Conditions`, so
`FStrateSetPiece`/`UpdateSetPieces` were MERGED into `FStrateLandmark`/`UpdateLandmarks` — one primitive, one
`UVoxelStrateDefinition::Landmarks` list. Each entry has an `AnchorMode`: **HashLattice** (scatter on a coarse
lattice, `SpacingChunks`) or **PassageMouth** (enumerate `StrateManager->GetPassages()`, keep endpoints whose
Upper/LowerStrateIndex == the current strate — `bAtDescentMouths` = the hole going down, `bAtArrivalMouths` =
where you land; endpoints are global VOXEL coords). Feature-conditioning is just `Profile.Conditions` on either
mode. **Exclusion (self-awareness, optional):** `ExclusionRadiusChunks` (0 = OFF, the default — mini-suns don't
exclude) + `Priority`; a candidate is suppressed if a HIGHER-RANKED one's disk covers it (rank = Priority, then
hash — deterministic). `UpdateLandmarks` now gathers all candidates → resolves exclusion (SKIPPED entirely when
no entry opts in, so pure scatter keeps its old O(n) cost; HashLattice keeps the original hash salt 0x1A2D5u so
mini-sun positions are unchanged) → spawns survivors via `SpawnFromProfile` (+ the orb wrapper).
**Exclusion is now POP-FREE:** each entry is gathered in `StreamRadius + MaxExcl` (MaxExcl = the strate's largest
`ExclusionRadiusChunks`); the extra "ring" candidates carry `bSpawnable = false` and only SUPPRESS (never spawn),
so every conflictor of an in-range candidate is always present regardless of player position → a candidate's fate
is a pure function of (seed, layout), no edge-of-radius flicker.
**Decoration footprint (`bSuppressDecorationsUnder` + `SuppressRadiusChunks`):** a landmark mesh doesn't change
density, so the deco placer can't see it — instead, on spawn it calls `RemoveDecorationsInSphere` to clear grass
in its footprint, stores the footprint on `FLandmarkInstance`, and `ApplyRegion` re-clears under any loaded
suppressing landmark when a deco region streams in fresh (so temple floors stay clear as you leave/return).
**PLAYER DIG → GRASS REMOVAL:** `AVoxelWorld::ApplyModification` (the single funnel for every carve/fill brush)
calls `RemoveDecorationsInSphere(Center·VOXEL_SIZE, Radius·VOXEL_SIZE)` after the diff+remesh — instant,
flicker-free (only the affected HISM instances go, via `GetInstancesOverlappingSphere`+`RemoveInstances`; nothing
is cleared+rebuilt), so grass never floats over a dug hole. This only patches the LIVE instances; the placer's
existing density check (`D(VX,VY,hC) <= 0.5f`) already keeps any future natural rebuild correct. (Editing a
Static-mobility HISM re-caches its proxy — fine for player-paced digging.) Not handled by immediate removal: new
grass on a freshly-exposed ledge / regrowth after fill-back — both self-correct on the next natural re-stream. **Freeze note:** a huge set-piece mesh hitches on register (game-thread proxy/distance-field build —
NOT async-fixable; the spawn is game-thread by engine rule; the asset is a hard ref so already resident) →
mitigate ASSET-side (Nanite on the mesh, bake distance fields), optionally budget spawns across frames.
`ApplyRegion` buckets a region's spawns per mesh (all its cells merged) and builds each HISM with ONE batched
`AddInstances` (single cluster-tree build, set cull/shadow BEFORE `RegisterComponent`) — the game-thread
hitch-killer for dense cells.
**Per-entry tier (`StreamTier`, default Far)** picks NearGrid or FarGrid; radius + column spacing are PER-GRID
settings, never per-entry (a per-entry radius would re-introduce the in-place re-stream flicker — see the two-grid
rationale above). `CullDistance` still bounds GPU draw on top (orthogonal to which grid
streams the entry). **No LOD area-density compensation** (placement is per real
surface point, density-stable with distance). **SpawnDensity semantics CHANGED** vs the old vertex scatter: it
rolls per column surface-point (not per mesh vertex) → expect a one-time density re-tune. **Settings
(`Voxel|Content`):** `DecorationRadiusChunks` (6 — FAR reach in cells), `DecorationNearRadiusChunks` (3 — NEAR
reach), `DecorationSpacingVoxels` (4 → 8×8 cols/cell — NEAR/fine grid), `DecorationFarSpacingVoxels` (4 by default
→ raise to 8–16 for a cheap coarse FAR grid; the rare-prop lever), `DecorationRegionSizeCells` (4 — RxR cells per
region/HISM), `DecorationMarchStepVoxels` (2 — coarse, bisection-refined; cave march only), `DecorationMaxCrossingsPerColumn`
(4 — cave march only), `DecorationColumnDepthVoxels` (160 — bedrock march cap; cave march only),
`MaxDecorationCellsPerFrame` (2 — apply/spawn budget), `MaxConcurrentDecorationTasks` (4 — in-flight task cap;
0 disables decorations). **COST:** surface worlds now use the O(1) oracle (cheap); caves ray-march. The work is
OFF the frame (worker threads) — game thread only pays the budgeted spawn. If streaming slows, lower
`MaxConcurrentDecorationTasks` / raise `DecorationMarchStepVoxels` / shrink radii/spacing. Default
`DecorationRadiusChunks=6` ≈ props ~48 m out — raise for far flora (cost ~r²).

**(A2) LANDMARKS — rare large objects on a COARSE HASH LATTICE (`UpdateLandmarks`, 2026-06-26).** The right
primitive for sparse, far-visible objects like the underground "mini-suns" — where the per-chunk decoration
grid fails: at a 2048-chunk radius that grid enumerates ~13M cells per cell-crossing on the game thread and
FREEZES. Landmarks instead live on a per-entry hash lattice (cell = `FStrateLandmark::SpacingChunks` chunks),
so a radius-R disk holds only ~(R/Spacing)² candidates (≈16 at R=2048, Spacing=512). Listed strate-wide on
`UVoxelStrateDefinition::Landmarks`. Each Tick, for each entry, walk the small lattice box around the player
within `StreamRadiusChunks`: `DecoHash(cell,entry,seed)` rolls existence (`SpawnProbability`), a jittered XY
(`JitterFraction` — effective min spacing ≈ Spacing·(1−Jitter)), then a SINGLE-column surface find
(`FindLandmarkColumn`: SurfaceWorld height oracle, else one density ray-march) snaps to the chosen
`SurfacePlacement` (default Ceiling = sky-cap). Gates mirror decorations (biome via `GetDominantBiomeAt`,
slope band, water-relative). Foliage-style transform tweaks: `LocationOffset` (world XYZ), `RotationOffset` +
per-axis `RandomRotation`, `Min/MaxScale`, `bAlignToSurface`. Spawned as a real actor (`ActorClass`, for a
sun's light) or one Static `UStaticMeshComponent` (`InstancedMesh`, `CullDistance`=0 → never cull). All
SYNCHRONOUS on the game thread (so few candidates it never hitches); a cell's surface-find runs only the
first frame it enters range, then is cached in `LandmarkInstances` (keyed `FIntVector(cellX,cellY,entry)`,
null entry = "evaluated, nothing placed" so it isn't retried). Deterministic (hash → no pop, infinite reach).
Strate-bounded (wiped on strate change, like decorations). This is the real home for "rare prop at all
distances" — the job the decoration FarGrid could approximate at moderate range but not at extreme radius.

**(B) WATER — ONE strate-global ocean plane that follows the player (`UpdateWater`, from `Tick`).** A single
scaled engine plane (`/Engine/BasicShapes/Plane`, no collision, no shadow) snapped to a coarse cell around the
player, so it only repositions when the player crosses a cell; terrain pokes through it, so it reads as water at
every LOD and to the horizon with no per-tile gaps (one draw). Water Z: `bHasWater` + `WaterLevelRelative` →
`StrateManager::GetWaterLevelWorldZForChunk` (no water in the current strate ⇒ plane hidden). Material:
`UVoxelStrateDefinition::WaterMaterial`. ⚠️ OPEN: `UVoxelBiomeDefinition::WaterMaterial` (the biome
content profile's water override, §8.14) is not read by the single-plane path — owner to decide whether
the biome override should come back (REVIEW_FINDINGS, *Owner decisions*).

`ClearAll`/`SetSeed` on `ChangeSeed`/regenerate clears both subsystems (decorations re-stream on the next
Tick via the INT_MIN sentinels). **Per-biome content (§8.14):** decorations resolve the dominant biome **PER COLUMN** on the worker
(`ResolveBiomeSampleAt` via the strate's `FBiomeContext`, box-cached → one rebuild per chunk footprint). The
update builds ONE flat decoration palette per grid (every biome's list concatenated; `EntryBiome[i]` tags entry
`i` with its context-biome index, -1 = strate fallback), and `PlaceAtCrossing` rolls only the entries the
column's biome owns. Borders follow the warped-Voronoi field at column resolution, no straight lines (a
per-CELL biome would snap borders to the 8 m cell grid). `Initialize` also takes `UVoxelSettings*` (for the
grid tunables).

### 8.6 Atmosphere — `VoxelAtmosphereManager.h/.cpp` (NEW)
`UVoxelAtmosphereManager` (owned by `AVoxelWorld`, gated by `bManageAtmosphere`).
`UpdateForPlayer(pos)` each Tick, reacts only on strate change. Drives a managed
`UExponentialHeightFogComponent` + movable `USkyLightComponent` from the player's strate
(`FogColor/FogDensity/bVolumetricFog/AmbientLightColor/AmbientLightIntensity`), and spawns
PERSISTENT ceiling/floor "layer" actors (`Def->CeilingLayerActor`/`FloorLayerActor` + ZOffsets
+ rotations) that follow the player in XY — the sky-island sea-of-clouds / two-sided fog.
`Def->AtmosphereActor` (a full BP with your own fog/sky/postprocess) OVERRIDES the managed
fog+sky for that strate. `Reset()` on ChangeSeed/EndPlay. (Skylight ambient underground is
weak — captures a dark scene; fog is the strong visual.)
**Per-biome atmosphere (§8.14):** `UpdateForPlayer` also resolves the player's dominant biome and,
when the biome has `bOverrideAtmosphere`, its fog/sky beats the strate's (reacts on biome change, not
just strate change). `ApplyFogSky(Def, Biome)` is the shared path; layer actors + the full `AtmosphereActor`
BP stay strate-level. Needs the generator injected (`Initialize(..., Generator)`).

### 8.7 Inter-strate bedrock gap
`VoxelSettings::InterStrateGapChunks` (N) inserts N chunks of SOLID bedrock between consecutive
strates (`StrateManager::Initialize` leaves the gap in the layout). `IsGapChunk` detects it;
`GetDensityAt` renders gap chunks as solid + passages only (no caves/spine/seal) so the player
digs (0,0) through the gap to descend. `GetStrateUnrealZRange` gives a strate's cm Z range.

### 8.8 Inter-strate passages — PER-STRATE (`FStratePassageConfig` on the definition)
Each strate's `PassageConfig` (VoxelStrateTypes.h) controls its descent tunnels to the layer
below: `Connections`, `Style` (`EVoxelPassageStyle`: Straight/Worm/Spiral/Cascading),
`MouthRadius`/`MidRadius` (tapered width → `FVoxelPassage::ControlRadii` + `VoxelSDF::TaperedCapsule`),
`ReachMin/Max` (depth into each strate), `DistanceMin/Max` (from the (0,0) spine), `Wander`,
`Segments`, `VerticalWobble`, Spiral/Cascade params. Built in `StrateManager::GeneratePassages`
as control-point chains. **Worm = independent fBM per horizontal axis** (`PassageFBM` static)
with a flat-top envelope → organic squirm (NOT a 1D zigzag, NOT a same-freq 2-channel spiral).
`EvaluateModifierSDF` (per voxel) first builds a `thread_local` **per-chunk shortlist** of passages
whose bounds reach this chunk (rebuilt on chunk change / `PassagesVersion` bump) — chunks with no
passage near return `FLT_MAX` immediately — then **bounding-sphere-culls** each shortlisted passage
(`FVoxelPassage::BoundCenter/BoundRadiusSq`). The same shortlist serves the tube, landing SDF, and
landing-floor membership; no source query, flood fill, or topology search is in the voxel loop. Both
are perf-critical (§8.10). The (0,0) surface entry is a simple straight tube. Placement, reach, and
shape draws for inter-strate passages are keyed by `(seed, upper-strate index, connection index,
per-value salt)` through `VoxelHash`, so one passage's connection count cannot shift any later
passage. Global passage settings were removed from `VoxelSettings`.

The repeated floor queries use the same `thread_local` passage state: `FPassageEvaluationCache` owns
one indexed `FloorProjections` entry per passage, keyed by exact double `(X,Y,Z)` equality for the
current sample. Native projection, generic fallback projection, and walkable-air classification
reuse the cached float outputs and validity bits; manager lifetime, layout version, and chunk changes
invalidate the cache. `voxel.FloorRound1PassageProjectionCache` is an A/B switch only. It changes no
projection arithmetic and no density value; a cache hit is an exact reuse of the first result.

After each passage's XY is selected, `GeneratePassages` asks **both** participating strates for a
player-fit source point through the pure free function `VF_SuggestLandingPoint`
(`VoxelCaveMorphology`).
TunnelNetwork and Underwater keep the nearest hash-room vertical placement at the requested XY, with
`MakeStrateSeed(world-seed, strate-index)` matching the room graph's existing identity; FlatPlain and
CrystalChamber recompute their slab void band and reject column-overlap points. Maze snaps to the
nearest roughness-safe horizontal lattice corridor within one cell, VerticalShafts to the exact
axis of a roughness-safe shaft on the drainage tree within two shaft spacings at the default site
density, and FloatingIslands
to a validated blob top within one island spacing.
Those three return false when no footing exists inside that explicit lateral budget. GeneratePassages
interpolates each accepted XY through the control-point chain and recomputes its conservative bound;
SurfaceWorld remains deliberately unanswerable because production terrain can be selected through
manager-owned biome/per-column context. Declined answers (unsupported or no-footing) keep the
historical random reach and are reported diagnostically.

The source query proves only a local pose; it does not flood-fill the live network. Therefore every
inter-strate end also receives an explicit `FVoxelPassageLanding`: a rounded chamber with a hard
flat floor. **There is no connector to the origin landing room at `(0,0)`.** The owner deleted
those radial roads after a playtest (`d97373c`, 2026-09-08: they cut straight through everything);
room joins are flattened to walkable height instead. A source-fit landing therefore stays local to
the room it was fitted into (`GeneratePassages`, the `VF_BuildPassageLanding` calls).
⚠️ Open gap (Sol's code review, `Docs/archive/DESIGN-SOL-2026-09-15-CODE.md` §1.3/§2.4): when the footing query
declines, the landing falls back to the historical random reach, and nothing proves that pose joins
the walk network. The canonical capability gate covers one scenario, not every seed or mix.
Proposed fix, not built: retry deterministically at other candidate spots instead of falling back.
This is the join guarantee, including for a source-fit answer — it is not a probability claim about
a nearby room. The connector is a swept flat-floor corridor with a 5-voxel (1.25 m) radius / 2.5 m
clear width and a 12-voxel (3 m) clear height. Its 4.5-voxel support inset is enough for the
1.36-voxel player radius and avoids capping unrelated shaft air; the origin room owns the root
floor, so there is no continuous root column. A connector's level dog-leg is sized against the same
named 15° floor-gradient limit as the default inter-strate tunnel. The room and connector floor are
therefore real walkable geometry, not a point anchor or nearest-component guess, and no
per-passage ordering is involved.

The chamber dimensions come directly from the capsule: player diameter `2×0.34/0.25 = 2.72`
voxels; required floor width `3/0.25 = 12` voxels; capsule height `2×0.88/0.25 = 7.04` voxels;
one metre of headroom adds 4 voxels. `HalfWidth=max(7, MouthRadius+2)` leaves
`2×(HalfWidth−1) >= 12` voxels of flat support after the one-voxel wall inset; the authored room
is therefore at least 14 voxels (3.5 m) wide. Height is
`max(12, 7.04+4, 2×MouthRadius+2)`; the stock 6-voxel mouth is a 14-voxel (3.5 m) room with
an 8-voxel half-width (4 m full width).
`FloorZ=StandingPoint.Z−0.5`, floor thickness is 3 voxels (0.75 m), and the tube centreline
meets the room at `FloorZ+MouthRadius`, so its lower tangent is the floor and there is no step lip.
The final-density floor audit samples the flat plane and checks the player-fit capsule above it. The
default tunnel audit samples every route step against the final density and checks the analytic and
measured route gradients against the explicit ≤15° walkable-tunnel contract. This is deliberately
well below the movement CDO's roughly 44.8° scramble ceiling: 44° is not a sensible default for a
tunnel the player is meant to walk.

The landing is part of the structural post, in the fixed order
`origin spine → vertical seal → passage tube + landing/floor → XY edge seal`. The floor is applied
after the passage carve and after MC-space disturbances as a support backstop, still before the
final XY edge seal. Room/floor Z is clamped inside the vertical seal; the XY edge seal is last and
wins over every landing or tube near the rim. Thus no landing can breach a strate boundary or the
world edge. The op-stack `FPassageCarveOp` calls the manager's `ApplyPassageModifier`, including
the floor, and `ClassifyTile` kills both uniform hypotheses around the bidirectional
landing floor (`Both` in the stack), so an air proof cannot delete support.

The query remains pure — it does not construct an operator stack, touch a cache, or call back into
`UVoxelStrateManager` while `Initialize` builds the layout. Landing descriptors, connector choice,
door direction, and bounds are all produced once from the passage seed salts; passages never read
another passage's result. `LayoutOrderIndependence` compares the complete descriptors bit-for-bit.

### 8.9 Carving — brush shapes + editor controls
`FVoxelModification` has `EVoxelBrushShape {Sphere,Box,Capsule}` + `BoxExtent`/`CapsuleEnd`/
`Falloff` + `GetWorldBounds`. `UVoxelDiffLayer::GetDensityOffset` switches per shape; chunk
overlap uses the shape AABB. `AVoxelWorld`: `CarveBox/FillBox/CarveCapsule/FillCapsule/
ApplyModification` (BlueprintCallable) + `EditorCarveSphere/EditorFillSphere` (CallInEditor)
driven by `EditorBrush*` props.

### 8.10 Performance invariants (DON'T regress)
- **Streaming** (`UpdateChunksAroundPosition`): rebuild/cull the desired set only when the player
  crosses a level-0 chunk boundary, an anchor moves, or a rebuild is forced; use the stamped
  desired map for the cull and idle via `bAllChunksLoaded`. On every rebuild, pending requests whose
  keys left the desired set are marked obsolete through a per-request atomic token. Workers only
  read that token (and `bShuttingDown`/generation pause), abort between expensive stages, and send
  the result through the normal MPSC queue; the game thread reads the token again before applying.
  `FChunkResult::Epoch` still guards the generation epoch. Desired work is sorted from the true pawn
  position, with the occupied/support LOD0 tile and the next tile along the heading in an absolute
  high-priority prefix. Stationary player ≈ free. (Old per-frame O(loaded×desired) scan = 22ms.)
- **LOD** changes HOT-SWAP (`LoadTile` only, never unload-first) → no holes. LOD
  reconciliation lives in the PERSISTENT per-frame submit loop (same loop as new-chunk loads),
  NOT as a one-shot on the boundary-cross frame — a one-shot drops every chunk past the task
  budget and strands it at a stale LOD. Idle (`bAllChunksLoaded`) only when a full scan finds
  no loads AND no LOD mismatches outstanding.
- **SDF cache** (`GetDensityWithParams`): search-BOX validity, not chunk-key — gradient ±1
  sampling must not thrash the (expensive) rebuild.
- **Worm block skip** (`VF_TryGetWormBlockSkip`): inside the unchanged `WormNetworkRange` mask,
  a worker-local direct-indexed cache covers the mesher's 4×4×4 lattice blocks (including the
  normal halo). It samples N1 once at the transformed block centre and proves the entire block
  clears the threshold with the actual 3D hash-gradient field bound
  `L = (15/4) * sqrt(3) * VOXEL_NOISE_SCALE = 8.118988160`, plus a `1e-3` scaled-output
  rounding margin. The radius is measured after the exact `WormFrequency`, `VerticalScale`, and
  `WormHorizontalBias` transform, so anisotropic Z is included. The cache is thread-local and
  keyed by generator owner, seed, params/layout fingerprint, tile origin, step, lattice size, and
  worm parameters; malformed/non-lattice/failed proofs fall through. A proof only suppresses N1
  and N2 because N2 is nonnegative; it never changes `NetworkMask` or density arithmetic.
- **Operator channel metadata** (`FVoxelOpStack`): `ChannelReads`, `ChannelWrites`, and
  `IsAdditive` are called only when an operator is added to a stack and their values are cached in
  the stack entry. `ValidateChannelOrder` is assembly/diagnostic-only; no declaration virtual may
  enter `Eval`'s per-voxel loop. If `FVoxelOpSample` gains a field, add its channel bit and update
  every operator declaration and the validator together.
- **Lateral region lookup/blend** (§8.1a): `FVoxelStrateRegionPartitionCache::PrepareForChunk`
  builds the bounded jittered-site window and a `(CHUNK_SIZE+3)^2` integer query grid once per
  chunk. Integer voxel samples read that grid; fractional gradient probes use the prepared site
  window. `FLateralRegionBlendOp` evaluates one creative stack in a region interior and a second
  only when the cached gap enters the blend band. The parent appends the four global structural
  posts once. Do not move `VoxelHash` site gathering, `TArray` construction, or region-stack
  discovery into `Eval`; the measured band overhead is an explicit Tier 4d budget item.
- **Isolated box proofs** (`FVoxelBoxHypotheses` + `FVoxelBoxSdfInterval`): the box fold carries
  the SDF interval produced so far. An SDF-only source publishes only its own interval; a later
  converter or detail op applies its own state-aware response. Unknown or invalid bounds force the
  fold toward `Mixed`: a missed skip costs CPU, while a false uniform verdict removes geometry and
  collision. Every new SDF writer must implement `PropagateSdfOverBox`; the default is unknown.
- **VerticalShafts field cache**: shaft rolls and connector decisions, including the structural
  tree and structural-spine connector endpoint, are rebuilt only when the thread-local centre cell
  or a geometry-affecting parameter changes. Each rebuild rolls a wider direct-indexed geometric
  halo (stock settings: 9×9 tree-emission cells / 15×15 total roll); it stores/emits only the
  inner 3×3 shafts and their capsules for the per-voxel loop, while spatial culling selects cached
  connectors. The complete ±2 parent and ±3 fallback windows plus connector reach are contained
  by that halo, so no parent decision depends on the evaluation cell. No cell or pair hash, widened
  neighbourhood scan, or tree construction may move into the per-voxel loop (`FShaftFieldSource`).
- **Maze field cache**: parent/loop decisions are rebuilt only when the thread-local cell, seed, or
  loop parameters change. The rebuild evaluates the 24 canonical lower-node/axis edges in the
  local `{-1,0}³` window and emits at most 24 capsules; the per-voxel loop performs no hash,
  parent, loop, or neighbourhood work. The focused audit measured **1.436 μs** per forced rebuild
  and **0.150 μs** per hot call in the focused audit; the final namespace run measured **1.130 μs**
  rebuild and **0.145 μs** hot on the same machine.
- **Per-chunk param cache** in `GetDensityAt`: GenType + param struct + disturbance cached
  thread-locally by `(DensityCacheOwnerId, ChunkCoord, LayoutVersion)`; the process-unique owner ID
  prevents cross-world reuse while adding only one `uint64` compare per voxel. Don't remove the owner
  or layout key, and don't move the fetch/blend back to per-voxel.
- **XY edge seal hot path** (§8.2a): `WorldRadiusVoxels == 0` returns immediately; otherwise the
  common interior case is a squared-distance test with no square root. The forcing op receives the
  two global settings through `FVoxelOpContext` once per chunk, while `ClassifyBox`/`ClassifyTile`
  share the radial proof. This adds no cache, state, RNG, or clipmap stream, and does not touch the
  density grid's two-pass MC loop or any existing `thread_local` cache.
- **Biome cache** (`ResolveBiomeSampleAt`/`FChunkBiomeCache`, §8.14): validity is a world-XY BOX +
  ChunkZ + Seed, NOT a chunk key — same reason as the SDF cache. The cell classification is
  noise-heavy; a chunk-key would thrash it on gradient-normal / +X/+Y boundary samples. Keep
  the box halo (≥ CHUNK_SIZE) + cell margin (warp + CellSize) so the 3x3 lookup never misses.
- **Passage cull** (§8.8) + **morphology two-region** (§8.4): both are per-voxel-cost critical.
- **Per-chunk passage shortlist** (`EvaluateModifierSDF`): runs per voxel and is called from every
  archetype's `ApplyPassageCarving`. Keeps a `thread_local` shortlist (passage INDICES) of passages
  whose bounds reach the current chunk, rebuilt only on chunk change or `PassagesVersion` bump
  (incremented in `GeneratePassages`). Most chunks have NO passage near → instant `FLT_MAX` return
  instead of walking the whole `Passages` array per voxel. Conservative superset (chunk bounding
  sphere vs passage bound) ⇒ bit-identical carve. Store indices + version, never pointers (the array
  is rebuilt on `RebuildStrates`). Landing rooms, connector SDFs, and floor membership reuse this
  exact cache. The final landing audit measured **0.665 μs** per forced rebuild and **0.646 μs**
  per hot call on the validation machine; source-fit queries run only during `GeneratePassages`.
- **Gen tasks run at `UE::Tasks::ETaskPriority::BackgroundNormal`** (`LoadTile`): worker gen yields
  to foreground game/render tasks. Without it, raising `MaxConcurrentTasks` past the spare-core count
  saturates the scheduler and starves the frame (the "concurrency > ~12 = stutter" symptom). Keep gen
  at background priority so the frame keeps its cores.
- **Mesher density grid + margin ring** (`GenerateMesh`): sample each grid point ONCE into a flat
  `(CHUNK_SIZE/Step + 1 + 2)³` array (the `+2` is a 1-point MARGIN ring, indices −1..GridDim, for
  T1.b normals). The cell loop reads 8 corners from it; per-cell sampling would call `GetDensityAt`
  ~8× too often. Geometry is bit-identical (edge positions unchanged). Don't refactor back to
  per-corner `GetDensity` and don't drop the margin ring (normals + seamless borders need it).
  The cell loop is **two-pass**: pass 1 reads the 8 corner densities + builds the MC case index and
  `continue`s on no-surface cells (≈70% of cells); pass 2 computes the 8 positions + grid-gradients
  ONLY for surface cells. Don't hoist position/gradient back above the case-index test. The
  `DensityGrid` and vertex-dedup `TMap` are `thread_local` and reused per worker (Reset / keep
  capacity) — don't make them per-call locals (re-allocates ~170 KB + a hash map every tile).
  **CAPTURE-DURING-MESHING (sanctioned reuse, doesn't regress the above):** `GenerateMesh`'s optional
  `OutCaptureGrid` copies the already-filled `DensityGrid` interior (`CHUNK_SIZE³`, quantized via
  `VF_QuantizeDensity`) out for the density clipmap (mini-sun shadows) — a PURE READ added after the
  grid loop. It does NOT touch the grid shape, the two-pass loop, the margin ring, or the thread_local
  reuse. Only level-0 full-res tiles request it (`Step==1<<Level`, 1:1 with a clipmap level). The
  clipmap (`UVoxelDensityVolume`) reuses these bytes instead of re-evaluating `GetDensityAt` on its own
  fills — see `IngestTileCapture` / `BlitCaptureToWindow` (cache keyed by tile coord) and the
  chunk-aligned level-0 recenter. The volume fill that captures DON'T cover runs on **one dedicated
  thread** (`FVoxelDensityFillRunnable`, off the UE::Tasks pool — the old shared-pool `BackgroundLow`
  fill starved behind mesh-gen, ~10 s to resolve shadows; the dedicated thread fills full-speed without
  stealing a mesh-gen core) and is the backstop for cache misses (vertical strate gaps, cold start,
  carves). Captures are bit-identical to a fill of the same cells (same `GetDensityAt`, same
  `VF_QuantizeDensity`).
- **Normals from the density grid (T1.b)** (`GenerateMesh`): corner gradients = central differences
  on the (margin) grid; edge normals interpolate the two corner gradients by the SAME `t` as the
  position → seamless across chunk borders (both sides use identical pure samples). NO per-vertex
  `GetDensityAt` (was ~6/vertex, often as costly as the whole grid). `ComputeGradientNormal` is now
  unused. Only NORMALS changed vs the old path; geometry is identical.
- **Surface column cache (T1.a)** (density path: the surface stack's `FSurfaceColumnSource` memo;
  `ClassifyTile`: `FSurfaceColumnCache` = LRU of `FSurfaceColumnBox`): the heightfield + sky-cap + biome blend are a PURE function of (XY, seed,
  strate) — **ZERO Z dependence** (climate/Voronoi are pure-XY; surface params are per-strate constant
  under Hard transitions) — yet sampled ~33× per column (once per Z grid-point). Cached per integer XY
  (box-valid, like the SDF cache) and reused down the column. **Keyed by XY box + strate identity + seed
  (plus layout version / params fingerprint), NOT ChunkZ** (the strate identity is derived from
  `StrateBottomWorldZ`, taken from the params so it can't disagree with them) and held as a small **LRU of 6 boxes** so the WHOLE vertical view-distance stack — and XY
  neighbours the scheduler interleaves — share one another's heavy column noise instead of each
  recomputing it ~once per vertical chunk (this was the dominant `GenerateMesh` cost: the same 2D
  heightfield recomputed per altitude). It also makes pure-air / pure-solid chunks cheap (they hit the
  shared box). Box `Halo = CHUNK_SIZE + 8` each side so the T1.b margin ring stays inside (no thrash).
  **Used ONLY for integer-XY queries**; fractional queries compute directly → bit-identical. Don't
  re-introduce a ChunkZ key, don't feed it fractional coords. (`GetSurfaceHeightAt`'s own `OC_*` oracle
  cache is separate and still per-chunk — lower volume, not worth the LRU.)
- **Surface-column construction (field-preserving follow-up)**: `FSurfaceColumn` is intentionally a
  trivial payload. `ComputeSurfaceColumn` writes all five outputs before publishing `Computed`, so
  its default member initialization was never observable; removing that generated TLS leaf avoids
  constructor work without changing a height or density. Integer queries still read the cached
  payload, and fractional queries still use the same direct locals.
- **Sampler unknown-source audit**: the earlier plugin-side `FSurfaceColumn` constructor PCs no longer
  occur after the trivial-payload change. The largest remaining LOD0 unknown cluster in the final
  sampler raw CSV is `UnrealEditor-Core.dll + RVA 0xC080` (runtime PCs around
  `0x7FF88382C080`), inside the private `.pdata` function range `0xB4D0–0xC32B`; the packaged Core
  binary has no PDB, so a source function name cannot be recovered here. Smaller unresolved groups
  map to generated engine reflection/CoreUObject code, not to `Plugins\VoxelForge`.
- **Collision only at LOD0 (T1.c)** (`ApplyMeshToChunk`): `UpdateSectionConfig(..., LOD==0)`.
  LOD1/2 chunks are unreachable (the §8.10 reconciliation hot-swaps to LOD0 before the player
  arrives), so cooking their Chaos collision is waste. Don't force collision on for all LODs.
- **CHUNKED-LOD CLIPMAP — the streaming model** (`FVoxelTileKey` in VoxelTypes.h; `UpdateChunksAroundPosition`
  / `BuildDesiredTiles` / `IsTileInClipRange` / `LoadTile` / `UnloadTile` / `ApplyMeshToTile`; mesher
  `GenerateMesh(OriginVoxels, Step)`). Replaces the fixed-32³-chunk + LOD-step-on-fixed-extent model
  AND supersedes the old region-batching / strate-Z-clamp / wide-ceiling (all removed). A **level-L tile**
  spans `CHUNK_SIZE<<L` voxels meshed at `step 1<<L` → constant 32³-cell mesh, ONE component, ONE draw,
  covering 8^L× the volume. Streaming loads **concentric shells** (level 0 near, each coarser level a 2×
  larger shell beyond; inner hole of level L = the region the finer level covers). **Total tile count
  stays ~flat regardless of view distance** — that's why see-far (ceiling, horizon) is cheap AND why
  per-tile components are fine for the game thread (no batching: ~1-2k tiles, not 40k). **Load-before-
  unload cull** (no holes, STRICT): out-of-range tiles cull now; in-range LOD-transition tiles cull only
  once EVERY desired tile overlapping their footprint is loaded — tested as "no UNLOADED desired tile
  overlaps T" (`ReplacementsReady` + `FootprintsOverlap` vs the `DesiredPending` list, built once per
  crossing = desired-minus-loaded, usually tiny). Scanning all of `DesiredSorted` per candidate was an
  O(loaded×desired) game-thread spike when fast movement turned many tiles non-desired at once. A coarse
  tile is replaced by several finer tiles, so the old center-owner
  check (`ReplacementLoaded`) dropped it as soon as the ONE tile over its centre loaded → the not-yet-
  ready edges flashed a hole; the full-coverage check keeps the old tile at its current resolution until
  the better mesh is wholly in, then swaps. In-flight requests are cancelled when their key leaves the
  desired set: the game thread flips the request token, the worker returns an aborted result, and the
  queue consumer drops it even if cancellation raced the final enqueue. A result must also match the
  current generation epoch and desired membership before it can apply. `TransitionHold` therefore
  protects only already-applied geometry during a LOD transition; obsolete work never becomes loaded
  geometry. Collision level-0 only; water level-0 only; shadows off for level≥2. **Decorations are NO
  LONGER tied to tiles** — they stream on a fixed world
  grid by distance (§8.5), so they don't pop on LOD swaps. Settings: `VoxelSettings::ClipRadius` (full-res near radius, tiles/level),
  `MaxClipLevel` (far reach). **NEAR-FIELD GEN COST levers** (`LoadTile`): levels `< FullResClipLevels`
  mesh at full `CHUNK_SIZE` cells (≈35³ `GetDensityAt` incl. margin ring), coarser levels at
  `CoarseTileCells` (Step = Extent/Cells) for far-cheaper gen. A level-1 tile at `FullResClipLevels=2`
  costs the SAME gen as a level-0 tile (same cell count, 8× extent) — set `FullResClipLevels=1` to drop
  level 1 to `CoarseTileCells` (~6× cheaper) when the near field is gen-bound (slightly harder L0→L1
  seam, hidden by skirts). `ClipRadius` bounds the full-res level-0 tile COUNT independently of reach.
  **STRATE CONTENT CUT** (`StrateContentCutMinLevel`, default 0 = all levels — tested: the level 0/1
  straddler tiles were the visible mixers, a higher floor read as "no improvement"): a level-L tile is 2^L chunks TALL and
  can straddle a strate boundary — at coarse Steps the thin seal/gap solid between two strates' airs
  falls between lattice points (holes into the neighbour strate at far LOD) and one tile mixes both
  strates' materials. From that level up, `GenerateMesh(..., BandZMin/MaxVox)` only meshes cells inside
  the PLAYER's strate chunk-Z band (exact bounds, no margin — the view clamp handles selection; in the
  inter-strate gap: no band). The band travels on `FChunkResult::BandChunkLo/Hi` so `ApplyMeshToTile`
  clamps its strate/material lookups into the meshed content. On band change (strate transition) the
  affected loaded coarse tiles re-queue via `BandRemeshQueue` (budgeted, re-gen in place, no pop).
  Meshed cells stay bit-identical (§8.4 — same pure world samples; the cut only selects cells).
  **Too-coarse skip (ultra levels)**: the cut is CELL-granular, so once one cell (Step voxels tall)
  is taller than the whole band (level ≥7 at CoarseTileCells=16 with typical strate heights), a
  band-overlapping cell still samples both strates' airs — same holes/mixing as uncut. `LoadTile`
  detects `Step > band height` and emits an EMPTY tile (it could only render garbage); an empty
  re-gen result also releases the tile's old component in `ProcessPendingChunks` (otherwise the
  previous strate's geometry would linger after a band change).
  **Horizon past that point = RENDER DISTANCE** (`RenderDistanceChunks`, default 0 = off): set
  `MaxClipLevel` to the coarsest level that still renders strates correctly (one cell must fit
  inside a strate band), then the OUTERMOST shell keeps generating tiles outward until it covers
  the requested distance (`VF_OuterShell`, shared by `BuildDesiredTiles` and `IsTileInClipRange`
  so the cull sees the same horizon; the ring's dz sweep is pre-clamped to the vertical strate
  band). The ring is made of level-MaxClipLevel MC tiles, so its cost grows with
  (distance/2^MaxClipLevel)². The far "sheet" ring (heightfield tiles past `MaxClipLevel`) was
  dropped by the owner's decision; it no longer exists in the selector or the settings.
  (A "step cap" variant — raising CoarseTileCells per level so far levels keep a fine Step — was
  tried and rejected 2026-07-06.)
  Trade-offs accepted: other strates simply don't render at far LOD (they're sealed/enclosed —
  invisible except through passage mouths, which read as dark holes); passage tubes crossing the gap
  are cut at coarse levels only (near levels mesh full).
- **SKIRTS — LOD-seam crack filler** (`GenerateMesh`, after the cell loop; `VoxelSettings::bGenerateSkirts`
  + `SkirtCells`, wired onto the mesher at setup). Neighbouring shells mesh at different resolutions so
  their iso-surfaces don't meet along the shared face → a thin see-through crack. After meshing, every
  triangle edge whose BOTH endpoints lie on one of the tile's 6 outer boundary planes (exact float compare
  — MC keeps the face-axis coordinate fixed) is a surface-contour edge on that face; a skirt quad hangs
  from it INTO the solid along the inverted vertex normals by `SkirtCells × Step × VOXEL_SIZE` (~one cell,
  ≥ the gap to a one-level-coarser neighbour). Emitted DOUBLE-SIDED (both windings) so it shows regardless
  of camera side / material two-sidedness; buried elsewhere → invisible. Adds verts/tris ONLY on boundary
  contour edges (small). Tune `SkirtCells` up if cracks persist, down if skirts peek out on convex edges.
- **Delta cull (stamped desired set)** (`DesiredStamped` + `DesiredStamp` + `TransitionHold`,
  `BuildDesiredTiles`/`UpdateChunksAroundPosition`): the desired set is a `TMap<key, stamp>`;
  each crossing bumps the stamp, upserts the new set, and ONE map sweep yields the **leavers**
  (stale stamp — removed + returned). The cull then considers ONLY leavers + `TransitionHold`
  (tiles kept by load-before-unload from earlier crossings) instead of re-scanning EVERY loaded
  tile per crossing — that scan was the measured ~1.6 ms/crossing `CullTiles` spike (2026-07-05
  trace, plus `BuildDesiredTiles` 0.66 ms on the same frames). Pending requests that become leavers
  are marked obsolete before replacement submission; `ProcessPendingChunks` reads the token, drops
  the aborted result, and does not put it in the hold. `ApplyTileResult` independently rejects a
  wrong generation epoch or undesired key. `UnloadTile` drops hold entries; the settled cull
  (everything loaded) keeps its full scan as the safety net. `DesiredPending` for the overlap test is
  now built LAZILY (only when an in-range transition candidate exists). **The hold is re-evaluated on
  a ROTATING BUDGET**
  (`TransitionHoldQueue` + cursor, ~256 tiles/crossing): re-scanning the whole hold each
  crossing degenerates back to the O(loaded) scan whenever streaming never settles (measured
  2.47 ms/crossing in the first packaged capture) — keeping a tile a few crossings longer is
  always hole-safe, and every held tile passes under the cursor within a few crossings. The
  SET is authoritative membership; the queue may hold stale keys (lazily dropped on scan).
  Same hole-free semantics, O(delta + budget) per crossing.
- **Budgeted teardown** (`PendingUnload` + `ProcessUnloadQueue`, called from `Tick` after the apply
  drain; `VoxelSettings::MaxUnloadsPerFrame`): the cull APPROVES removals (strict load-before-unload) but
  doesn't destroy in place — it queues them. `ProcessUnloadQueue` runs at most `MaxUnloadsPerFrame`
  `UnloadTile`s/frame (scaled up to 4× with backlog, capped so a huge backlog can't re-spike). WHY: a
  fast traversal culls a whole shell's worth of tiles in ONE frame, and each `UnloadTile` strips and
  parks its component (T2.c pool) — an unbudgeted burst = a game-thread spike ("stuff torn down behind you" at speed). Mesh APPLIES were
  already budgeted; this matches it for DESTROYS. Re-desired tiles are cancelled out of the queue (still
  loaded → no reload). `PendingUnload` is cleared in `RegenerateAllChunks`/`EndPlay` (tiles already gone).
- **Collision only at LEVEL 0** (`ApplyMeshToTile`): `UpdateSectionConfig(..., Tile.Level==0)`. Far tiles
  are unreachable; cooking their Chaos collision is waste. (Was T1.c, now per-tile-level.)
- **No shadows on far tiles (draw cut)** (`ApplyMeshToTile`): `SetCastShadow(Tile.Level <= 1)`. Each
  shadow-casting tile emits a second shadow-pass draw; the far coarse tiles don't need it. NOTE: fps is
  RENDER-side (draws ≈ visible tile count × passes); generation cost (workers) and tile *resolution*
  (cuts triangles, not draws/components) don't move the game thread — tile COUNT does (hence the clipmap).
- ⛔ **2026-09-13: streaming no longer calls `ClassifyTile` by default** (`voxel.OuterClassifierMode`
  = 0; `=1` restores it as an A/B instrument). Measured on the headless game path with nested
  refinement off, the classifier plus its exact validation cost more than they saved. Mode 0 had
  ready p95 182 vs 190-195 ms, generation p95 125-127 vs 139-141 ms, and 18% fewer density calls,
  with 841/841 tiles triangle-identical (Docs/archive/WORK-NEXT.md). Proving a 33³ core uniform costs almost
  as much as meshing 35³. The history below explains why it existed. Its soundness tests still
  guard it.
- **Trivial-empty tile reject (T1.d) — v2 SHIPPED (2026-07-05); v1 was reverted 2026-06-26.**
  `UVoxelGenerator::ClassifyTile(Origin, Step, Cells)` runs on the gen worker BEFORE the density
  pre-sample (`LoadTile` task, scope `VoxelForge_ClassifyTile`); `AllSolid`/`AllAir` ⇒ GenerateMesh is
  skipped and the tile stays `bEmpty`. Motivation: the 2026-07-05 Insights trace showed **84 % of
  GenerateMesh calls produced empty tiles** (83 925 gens vs 13 227 meshes — ~500 s of 617 s worker CPU
  wasted) and gen throughput had become the felt gameplay limit ("standing waiting for generation").
  **Why v1 failed & how v2 avoids it:** v1 used a GLOBAL analytic ceiling bound — not conservative when
  the cap hangs low (`CeilingRoughness`/`RidgeStrength`) → holes in the roof. v2 makes NO amplitude
  guesses: it evaluates `ComputeSurfaceColumn` (cached per thread in `GSurfColCache`) — the column
  the surface stack's height operators reproduce (ground height pinned bit for bit by
  `VoxelForge.OpStack.SurfaceHeightEquivalence`) — **on the exact lattice the mesher would sample**
  (margin ring included) — same inputs ⇒ the same floats ⇒ the verdict is exact at the lattice, not
  an estimate.
  Per-z rules: gap chunk = solid; surface seal band (ApplyBoundarySeal inequalities, `BaseDensity>0`)
  = solid; surface interior z: air ⇔ `TerrainZ ≤ z ≤ CeilSurf` (MC `D ≥ 0`); ANY other archetype /
  out-of-layout chunkZ ⇒ `Mixed` (cave interiors are not provable in v1 of this classifier — a future
  extension could use "SDF cache empty + worm network mask" for deep TunnelNetwork rock).
  **Conservative guards** (anything that can carve/fill): player mods (`HasAnyModInChunkRange`) ⇒
  Mixed; passages (`AnyPassageNearBox`, bounding spheres + carve blend pad) and the (0,0) spine
  (circle/box XY) kill AllSolid; disturbance chasms kill AllSolid, bridges/ridges kill AllAir.
  The global XY edge proof runs first and returns AllSolid for a box entirely in the forced band;
  the same force is the final generated post after disturbances, so a near-rim passage cannot
  invalidate that verdict. Every other uncertain case remains Mixed.
  A false `Mixed` only costs CPU; the code must NEVER emit a false AllSolid/AllAir (that's a hole).
  Capture tiles (`bWantCapture`, density-volume shadow window) always generate — the volume wants the
  grid even for uniform cells. A sparse ~5×5 column pre-pass exits Mixed fast on surface-crossing
  tiles; the columns stay warm in `GSurfColCache` for the full pass and for neighbouring tiles.
- **Worker-built StreamSet (T1.f)** (`BuildTileStreamSet`, `LoadTile` task → `FChunkResult::Streams`):
  the RMC vertex/index buffers (`FRealtimeMeshStreamSet`) are built ON THE GEN WORKER, not on the game
  thread. The per-vertex builder loop was the dominant game-thread streaming cost (measured: game
  thread >6 ms while moving, GPU/Draw idle — purely game-bound). `BuildTileStreamSet` touches ONLY the
  POD `FVoxelMeshData` arrays (no UObject, no generator) so it's worker-safe; `ApplyMeshToTile` now only
  resolves material/ceiling (O(1)), gets/creates the component, and hands the finished streams to
  `CreateSectionGroup(MoveTemp(...))` (which already uploads async via its `TFuture`). `FChunkResult`
  carries the streams as a `TSharedPtr` (forward-declared in the header) so it stays movable through the
  MPSC queue; the worker `Enqueue(MoveTemp(Result))` (no payload copy). Don't move the builder loop back
  onto the game thread. Geometry is byte-identical — only WHERE it's built changed. Empty/all-air tiles
  carry no streams (`bEmpty`) → no component. NOTE: RMC collision is already async-cooked
  (`bUseAsyncCook=true`), so level-0 collision (T1.c) is NOT a game-thread spike. Remaining per-apply
  game cost is `NewObject`+`RegisterComponent` for new tiles → component pooling (T2.c) is the next lever
  IF a trace still shows `ApplyMeshToChunk` cost.
- **Insights scopes** `VoxelForge_GenerateMesh` / `VoxelForge_BuildStreams` (worker) /
  `VoxelForge_ApplyMeshToChunk` (game-thread apply, Perf 0) bracket the worker gen + stream build +
  game-thread upload — capture a trace to see if we're density-, build-, or upload-bound.
- **Float SIMD noise core (T2.a)** (`Public/VoxelNoise.h`): the density hot path uses
  `VoxelNoise::Perlin3D` (single-sample, float, table-free hash-gradient) and `VoxelNoise::FBM` /
  `Ridged` (octaves evaluated **4-wide via SSE** `Perlin3D_x4`) — NOT `FMath::PerlinNoise3D`
  (double-precision, the old ~6.6 ms/chunk noise cost). `FractalNoise3D` / `RidgedNoise3D` in
  `VoxelGenerator.cpp` are now thin wrappers over it; every call site is unchanged. It's a
  DIFFERENT noise field than FMath's ⇒ a ONE-TIME world re-tune (fBm/Ridged contracts/[-1,1] are
  identical). Pure function of (x,y,z) ⇒ every box-validity cache stays valid. Scalar `Perlin3D`
  and SSE `Perlin3D_x4` are op-for-op identical (bit-identical on x86) — the SIMD path is a free
  speedup; `#define VF_NOISE_USE_SIMD 0` falls back to scalar with no re-tune if a toolchain
  rejects the SSE4.1 intrinsics. StrateManager's passage/transition Perlin calls were left on
  `FMath` (layout-time, not per-voxel). Don't reintroduce `FMath::PerlinNoise3D` on the density path.
- **LOD-aware octave drop (T2.b, opt-in)** (`VoxelGenLOD` in `VoxelGenerator.h`, guard in
  `GenerateMesh`): coarse tiles (Step>1) drop `Settings->LODOctaveDrop × log2(Step)` octaves from
  the generator's PER-VOXEL volumetric noise via a `thread_local` bias — sub-cell octaves can't
  shape a coarse isosurface. Default **0 = off = byte-identical**; LOD0 is never biased. The bias
  is `TGuardValue`-scoped to the tile, so deco snapping / density-volume fill / game-thread
  queries always see 0. Deliberately NOT applied to XY-field noise (heightfield, ceiling, relief,
  moisture): those feed box-validated caches that outlive a tile task on the same thread, and
  climate/biome must stay LOD-independent. Keep any new per-voxel fractal call site on
  `VoxelGenLOD::Eff(N)` and any new cached-field call site OFF it.
- **Tile component pool (T2.c)** (`TileComponentPool` + `Acquire/ReleaseTileComponent`,
  `VoxelWorld`): unloading parks the tile's RMC component (geometry+collision stripped via
  `RemoveSectionGroup`, hidden, still registered) instead of `DestroyComponent`; applies pop from
  the pool instead of `NewObject`+`RegisterComponent`. Also: `ApplyMeshToTile` now reuses the
  component's existing `URealtimeMesh` (`GetRealtimeMeshAs`) — `InitializeRealtimeMesh` allocates
  a NEW mesh object every call, so calling it per apply (the old code) orphaned one UObject per
  re-mesh to the GC. A parked component MUST have its section group removed (hidden ≠ collision
  off) — don't "optimize" that away. Pool is bounded (`MaxPooledTileComponents`); overflow is
  destroyed for real.
- **`ProcessQueue` MUST be `EQueueMode::Mpsc`** (`VoxelWorld.h`): up to `MaxConcurrentTasks`
  `ChunkGen` worker threads `Enqueue` concurrently; the game thread is the sole consumer.
  The default `Spsc` is single-producer — concurrent enqueues race the tail link and silently
  DROP results, leaking `PendingChunkCoord` slots until the budget is exhausted and streaming
  stalls for good (intermittent; worst during the completion bursts right after the player moves).

### 8.11 Live tuning & debug (`AVoxelWorld`, CallInEditor / PIE)
- `RebuildStrates` — re-reads ALL of `VoxelSettings` and rebuilds layout/gap/passages/spine +
  regenerates. Use after changing those (plain `RegenerateAllChunks` keeps the old layout/passages).
- ApplyComposerCandidate — editor-only PIE walk-through for one offline candidate. Set
  ComposerSeed, ComposerCandidateIndex, ComposerTargetStrateIndex (0 = topmost), and
  bComposerRollStructure in Live Edit|Composer, then click Apply Composer Candidate. It uses the
  shared composer roll, overlays density on the existing slot, pauses/drains workers, bumps the
  manager version, and reuses RegenerateAllChunks for the epoch/re-stream. It does not rebuild
  passages or change the authoritative Settings->Seed; RebuildStrates removes the overlay. The
  action logs recipe, archetype, air/largest-component/walkable/feature metrics, and an eight-point
  bit-level density verdict. The button itself still requires owner verification in PIE.
- `ValidateDeterminism` (F2) — one-click §8.4 regression test: samples chunk-boundary points under
  two different thread_local cache alignments (left-chunk warm vs right-chunk warm) + a repeat
  pass; every delta must be EXACTLY 0. Run it after any hot-path refactor that claims
  bit-identity (~1 s, game thread, PIE).
- `bDebugDrawPassages` — draws every passage (cyan path, green=upper / red=lower endpoints).
- `EditorCarveSphere`/`EditorFillSphere` + `EditorBrush*` props — manual carve/fill in PIE.

### 8.12 Authoring a strate (data asset)
1. Create `UVoxelStrateDefinition`, pick `GeneratorType` → its param group appears; tune it.
2. `PassageConfig` → how THIS strate connects DOWN (count / style / tapered width / length / placement).
3. `Disturbances` for chasms/bridges/ridges; `bHasWater`+`WaterMaterial`(+`WaterLevelRelative`) for water.
4. Atmosphere: `FogColor/Density`, `AmbientLight*`, `bVolumetricFog`, or a full `AtmosphereActor` BP;
   `CeilingLayerActor`/`FloorLayerActor` (+offsets/rotations) for cloud seas.
5. `Decorations`/`AmbientActors` (placement rules) for content + lights.
6. (Optional) `Biomes[]` + `BiomeMapParams` to vary terrain/content within the strate (§8.14).
   Author `UVoxelBiomeDefinition` assets (climate box + modulation + content), then tune layout
   with `AVoxelWorld::BakeBiomePreview`. Turn `ReliefStrength` down when biomes drive elevation.
7. Reference from `VoxelSettings` (`StratePool`/`FixedStrates`). Global knobs there:
   `OriginSpineRadius`, `bOpenSurfaceEntry`, `InterStrateGapChunks`, view distances, LOD, carving budget.

### 8.13 New files this redesign
`Public/Private/VoxelContentManager.h/.cpp` (§8.5) · `Public/Private/VoxelAtmosphereManager.h/.cpp` (§8.6) ·
`Public/VoxelBiomeTypes.h` + `Public/VoxelBiomeDefinition.h`/`Private/VoxelBiomeDefinition.cpp` (§8.14).
Everything else extended existing files: `VoxelStrateTypes.h` (archetype params, disturbance,
`FStratePassageConfig`, enums), `VoxelStrateDefinition.h`, `VoxelGenerator.h/.cpp` (archetype
density fns + spine/disturbance/param-cache), `VoxelStrateManager.h/.cpp` (per-archetype getters,
passages, gap, atmosphere Z helper), `VoxelWorld.h/.cpp` (managers, streaming perf, brush API,
editor buttons), `VoxelDiffLayer.h/.cpp` (brush shapes), `VoxelSettings.h`, `VoxelCaveMorphology.cpp`
(two-region determinism). Status: compiles & runs in-editor.

### 8.14 Biome system (Stage 1 — climate-driven, full-param overrides)
Biomes vary terrain **and** content WITHIN a strate. A biome is a **"mini-strate-variant"**: it
can carry a FULL archetype param override (its own `FSurfaceGenerationParams`, …) plus a content
profile, placed by a deterministic, window-invariant world-XY field. Empty `Biomes[]` ⇒ bit-identical
to the pre-biome world. (Replaces the earlier `FBiomeModulation` scalar bag — full params let a biome
change *anything*, e.g. frequencies, which scalar multipliers couldn't.)

- **Assets/data.** `UVoxelBiomeDefinition` (one per biome): `DebugColor`, climate box (relief,
  moisture), `bOverrideTerrain` + `GeneratorType` + the matching archetype param struct (Surface
  wired), content profile (decorations/atmosphere/water). + `UVoxelStrateDefinition::Biomes[]` &
  `BiomeMapParams`. Types in `VoxelBiomeTypes.h` (§3.8).
- **The field (pure XY, window-invariant — §8.4).** `SampleBiomeAt` (VoxelGenerator.cpp): warped
  **Voronoi** over a jittered grid → dominant cell + nearest neighbour (F1/F2) + border blend weight.
  Each cell's biome is chosen by `ClassifyBiomeAtSite` from the site's **climate** = `SampleRelief`
  (the relief map M, shared with SurfaceWorld terrain) + `SampleMoisture`, matched against each
  biome's (relief, moisture) box → coherent geography. **Climate must vary much slower than
  `CellSize`** (~4-6 cells/feature) or it's salt-and-pepper.
- **Per-chunk resolution (perf — §8.10).** `ResolveBiomeSampleAt`/`RebuildBiomeGrid` build a
  `FChunkBiomeCache`: the expensive cell classification is done ONCE into a small grid; per voxel only
  a warp + 3x3 lookup, returning `FBiomeSample` (dominant + neighbour + weight). **Cache validity is a
  world-XY BOX + ChunkZ + Seed (NOT a chunk key)** — gradient-normal + boundary samples stay inside
  the box and don't thrash the noise-heavy rebuild (same as the SDF cache). Bit-identical to
  `SampleBiomeAt`, so the baked preview matches the terrain. `GetBiomeContextForChunk` supplies the
  flattened POD context per chunk (thread-local `CP_BiomeCtx`).
- **Consumption — SURFACE (output-blend).** Per chunk, `CP_SurfaceBiomeParams[]` holds each biome's
  resolved surface params (its override when `bOverrideTerrain` + GeneratorType matches, else the
  strate's) with **structural fields forced from the strate** (Z bounds, seal, base density, water
  level). Per voxel: `ResolveBiomeSampleAt` → dominant `PD` (+ neighbour `PN`); the surface stack's
  biome-blend height source (and `ComputeSurfaceColumn`) computes the terrain height for `PD` and, in
  the border band, for `PN`, and **lerps the resulting HEIGHTS**. Blending heights (not params) is seamless across *any* difference (frequencies
  included) — what per-param blend never could. `PD==PN`, weight 0 ⇒ bit-identical, no biomes.
- **Consumption — CAVES: structural overrides are NOT applied (determinism).** Rooms/tunnels are
  decided over a wide COLLECT region spanning chunks (§8.4); making room params vary by region would
  need the biome sampled per *room site* inside `BuildChunkCache`, or it breaks window-invariance
  (a room near a border resolves differently per querying chunk → seams/holes). So SDF archetypes
  (Tunnel/Maze/Shaft/Islands) keep strate-level structure; biomes affect them via **content +
  atmosphere only** (below). Per-room-site biome params = a future deep task.
- **Consumption (content/atmosphere).** ContentManager DECORATIONS resolve the biome **per column** on the
  worker (`ResolveBiomeSampleAt`, box-cached) → organic borders (§8.5); a column rolls only its biome's
  decorations (else the strate's). `GetDominantBiomeAt(x,y,chunkZ)` (game-thread, uncached) → biome ASSET is
  still used for the cheaper single-point picks: ContentManager water material + AtmosphereManager player
  dominant biome fog/sky (`bOverrideAtmosphere`). Works for ANY archetype.
  Water LEVEL stays strate-global (continuous plane); biomes retint material only.
- **Preview tool.** `AVoxelWorld::BakeBiomePreview()` (CallInEditor) bakes biome / relief / moisture
  to `Saved/BiomePreview.png` via a transient generator (no PIE). Needs the `ImageWrapper` module.
- **Status:** A (field+asset+preview), B (terrain), C (content/atmosphere) verified in-editor.
  Full-param redesign (surface output-blend) ✅ BUILT & WORKING (ticked 2026-07-27). Cave structural biomes
  deferred (determinism, see above). Per-voxel biome warp (+2 Perlin) & content `GetDominantBiomeAt`
  are future T1.a column-cache candidates.

### 8.15 Biome material identity — vertex-colour palette (F6, Stage 1)
A biome re-skins the terrain SURFACE (not just content/atmosphere) through a single master material,
with NO extra draw calls / material slots and NO per-tile material swap (which would seam at tile
borders). The biome's `MaterialPaletteIndex` (0-255) is **baked into the mesh vertex colour** and a
master triplanar material switches/blends its layers on it. Works for ANY archetype (it rides the
generic biome field), not just SurfaceWorld. Empty `Biomes[]` ⇒ all-zero colour ⇒ bit-identical look.

- **Vertex-colour layout** (`FVoxelMeshData::Colors`, packed in `UVoxelMarchingCubesMesher::GenerateMesh`
  `GetOrCreateVertex`): **R** = dominant biome `MaterialPaletteIndex`; **G** = slope (`1-|N.z|`: 0 flat
  floor/ceiling, 1 vertical wall — for rock-on-cliffs); **B** = biome border blend weight (0 deep in a
  cell → ~0.5 at the border); **A** = NEIGHBOUR biome `MaterialPaletteIndex`. The master material does
  `lerp(layer[R], layer[A], B)` for a seamless cross-fade along the biome field's own border (B peaks at
  ~0.5 = 50/50 at the border; the identities swap across it, so 50/50 both sides ⇒ no discontinuity —
  do NOT rescale B to reach 1.0 or the swap becomes a hard seam).
  Height/snow-line is derived in-material from `WorldPosition.Z` (no channel needed). Skirt verts inherit
  their source vertex's colour (`AddSkirtVert` takes the colour) so the `Colors` array stays parallel.
- **Data path.** `UVoxelBiomeDefinition::MaterialPaletteIndex` → `FBiomeResolved::MaterialPaletteIndex`
  (set in `StrateManager::GetBiomeContextForChunk`) → `UVoxelGenerator::GetBiomeMaterialAt(x,y,z →
  dominant/neighbour palette + weight)`. That method mirrors `GetDensityAt`'s biome caching: a
  thread_local per-chunk `FBiomeContext` + box-validated `FChunkBiomeCache`, so the noise-heavy classify
  is reused across a tile's vertices. Resolved per UNIQUE vertex (after dedup), not per triangle corner.
  Window-invariant (`ResolveBiomeSampleAt`, bit-identical to `SampleBiomeAt`).
- **Apply.** `AVoxelWorld::ApplyMeshToTile` calls `Builder.EnableColors()` + `Vertex.SetColor(...)`.
  The terrain material slot is still strate `OverrideMaterial` / `Settings->VoxelMaterial` — author THAT
  as the master palette material. No biome terrain-material asset field (palette index is the contract).
- **Perf.** Free where a strate has no biomes (`GetBiomeMaterialAt` early-outs to palette 0). Otherwise
  one biome resolve per unique vertex, bounded by the per-chunk biome cache (don't feed it a chunk key —
  keep the box validity, §8.10). Coarse far tiles have few vertices.
- **Status:** C++ ✅ BUILT & WORKING (ticked 2026-07-27). The master material graph is still
  editor-side work and is deliberately NOT ticked — that half is Jahni's, not the code's.

### 8.16 Explorer commandlet — one authoritative field, one sampled grid, three consumers
`-run=VoxelForgeExplore` lives in the separate `VoxelForgeEditor` module, so the commandlet and its
PNG/JSON/OBJ writers are editor/commandlet-only and cannot enter a Game or Shipping link. It creates
only transient settings, definitions, manager, generator, diff layer, and mesher objects; no authored
asset, live manager, generator, or diff layer is mutated. The transient world keeps
`WorldRadiusVoxels=0` and `InterStrateGapChunks=0`, preserving the lateral-region gate.

- **Render.** Render obtains the player-fit seed/mask from the shared measurement pass, then calls
  the canonical `UVoxelMarchingCubesMesher` once per 32³ tile for the bounded region. The aggregate
  mesh is merged in fixed Z/Y/X tile order, indexed into a deterministic CPU BVH, and rasterised
  from eight player-fit viewpoints with a depth buffer and fixed Lambert lights. The per-pixel path
  never calls `GetDensityAt`; `-renderstep` and the old bisection fields remain only as compatibility
  metadata. Each view overlays a projected 1.76 m human-height marker and the JSON records mesh,
  acceleration, per-view raster timings, and marker projection status.
- **Walk.** `VF_MeasurePlayerFitWalkWithSampler` builds the existing step-1 capsule/floor-fit stencil
  and captures that exact sampled grid plus player-fit mask for the run. It uses the same
  six-neighbour/full-resolution route gate as the existing diagnostics. Its
  deterministic depth-first agent walk counts metres travelled (including branch backtracking),
  dead ends per 100 m, the arrival component's reachable fit volume, and traversed edges touching a
  narrow-gap proxy below 1.5× the 0.68 m capsule width. The proxy is explicitly axis-aligned air span;
  it is not a second player-fit or movement definition. The explorer passes the shared measurement
  `MaxCells` refusal at a 32,000,000-cell default cap, with a conservative 20-byte/cell estimate
  against a documented 768 MiB working-memory budget. Reachable fit volume is explicitly the bounded
  fitted route window because `WorldRadiusVoxels=0` deliberately has no finite whole-world volume.
- **Export.** `UVoxelMarchingCubesMesher::GenerateMesh` is still the only mesher. Because that
  canonical entry point deliberately clamps one call to a 32³ tile, a 128³ request is four-by-four-
  by-four canonical calls. Their returned vertices/triangles are merged once and the same aggregate
  is consumed by both OBJ export and render; no second mesher or density evaluator exists. Export
  reuses the walk's grid hand-off when a walk/render mode is present. The bounded result is metre-space
  OBJ with source winding, voxel-space UVs, normals, and a manifest containing seed, archetype, slot,
  bounds, and player dimensions. Tile skirts are disabled at the export boundary because they are a
  render seam aid that would extend beyond the manifest box. A conservative preflight cap is checked
  before the canonical mesher is called.

- **Budget.** The default wall-clock budget is 25 minutes; `-budget=` may lower or raise it up to a
  hard 30-minute maximum. Setup, sampling, meshing, acceleration, rasterisation, and export check
  the budget and report `truncated`, `truncated_during`, and `completed_modes` while retaining any
  completed artifacts.

The report and export manifest use fixed-order writers and serialize twice in-process to assert the
deterministic contract. The fixed tile merge, BVH ordering, and raster traversal are deterministic;
timings are intentionally diagnostic metadata and do not affect world or image content. The browser
viewer remains a separate display-only consumer: it must load this OBJ/manifest and never evaluate
density or generate geometry.

## 9. Multiplayer model (listen-server first, dedicated-friendly)

> **Status: transport is DESIGN ONLY — nothing is networked in-tree yet** (no `Replicated`/`HasAuthority`/RPCs; a
> single `GetPlayerPosition()` center; a local diff layer). This section locks in the invariants so the
> streaming / AI / carve systems are built network-aware from the start instead of retrofitted. Target
> **now = listen server** (the host is a player AND the authority); **dedicated server = future / out of
> scope**, but the abstractions below (anchor *policy* + *role*) already cover it so it's additive later.

### 9.1 The core invariant — determinism means you NEVER replicate geometry
The world is a pure function of **(seed, strate layout)** (§8.4). So terrain is reconstructed identically
on every peer from a tiny amount of shared state — it is **never streamed as geometry over the wire**:
- Replicate the **effective seed + strate layout identity** ONCE (at join). For a cooked season the
  layout is the reviewed manifest, not a pool shuffle: peers must load the same season asset and compare
  `AVoxelWorld::GetCurrentSeasonContentHash()` before accepting play. For the legacy path the layout remains
  deterministic from the effective seed. Every peer then generates terrain locally; geometry never crosses
  the wire. The transport/RPC that performs this join handshake remains out of tree.
- The **diff layer is the ONLY non-deterministic terrain state** (§3.9, [[voxelforge-difflayer-threading]])
  → it is the only thing that must sync. Since carving is a minor feature, this traffic is small.

### 9.2 Authority — server-authoritative diff, deterministic local re-mesh
- A carve/fill is a **request**: client → `Server_RequestModification(FVoxelModification)` → the authority
  (the host) validates it (`DiffLayer` budget / anti-cheat, §3.9 `CanModify`) → applies to the
  **authoritative diff layer** → **multicasts the small `FVoxelModification`** (center/radius/strength ~a
  few floats) → every peer applies it to its LOCAL diff layer and re-meshes locally via the existing
  `ApplyModification` path (§3.5). Geometry never crosses the wire; only the edit event does.
- On the **listen server** the host is also a player, so a host carve applies directly (still through
  validation) then multicasts. Remote clients only ever send requests.
- **Season / seed change** (`ChangeSeed`, `RegenerateAllChunks`) bumps a **local** `GenerationEpoch` today
  — in MP this must become a **server-driven multicast event** (everyone bumps epoch + regenerates from the
  new seed). Epoch stays a per-peer local counter; the *trigger* is networked, the counter is not.

### 9.3 Multi-anchor streaming — the backbone, not just an AI feature
**IMPLEMENTED 2026-07-07 (the streaming/collision half; the collision-only render-skip §9.4 is still
pending).** `AVoxelWorld::RegisterStreamingAnchor(Actor, Policy, RadiusChunks)` / `UnregisterStreamingAnchor`
(BlueprintCallable) add an actor to `StreamingAnchors`. `UpdateChunksAroundPosition` prunes dead anchors +
detects chunk crossings (rebuilds the desired set when any anchor crosses a level-0 boundary — same cadence
as player movement, coalesced into one rebuild). `AddAnchorDesiredTiles` (inside `BuildDesiredTiles`, after
the player clipmap) folds each anchor's Chebyshev box of **level-0** tiles into the SAME
`DesiredStamped`/`DesiredSorted` set (deduped vs the clipmap by stamp) → the existing delta cull releases an
anchor's tiles automatically when it moves away / unregisters. Zero cost when no anchors (empty loop). The box
defaults to a THIN shape (its chunk + 1 horizontal ring + 1 chunk below for ground safety, nothing above —
`XYRadiusChunks`/`ZBelowChunks`/`ZAboveChunks`, per-register). `CollisionOnly` anchor tiles are hidden (§9.4).
Anchor tiles sort by distance-to-*player*, so one far from every player streams last (fine for now; a
per-anchor priority is a later tweak).

The general model MP requires — **N centers**: the authority streams around **every connected player + every
AI**, because that's how a remote pawn gets server-side collision / movement authority. The registry of
**anchors**:
- **Anchor = { actor, policy }**, `policy ∈ { CollisionOnly, FullVisual }`, plus a **role** on the world
  (client / listen-host / [future] dedicated).
- **Listen-host role:** `FullVisual` anchor on its OWN camera (it renders for itself) + **`CollisionOnly`**
  anchors around every REMOTE player + AI (it needs their collision for authority, not their pixels).
- **Remote-client role:** `FullVisual` anchor on its own camera + collision around its own pawn (local
  prediction). It does not stream other players' far tiles.
- **[Future] dedicated role:** ALL anchors `CollisionOnly` — no visual mesh anywhere server-side. The
  listen-host's `CollisionOnly` path IS this path, so dedicated is just "no local FullVisual anchor."
- Keep today's single-player fast path exactly when the registry has one FullVisual anchor and no others.

### 9.4 Collision-only tiles — render-skip (IMPLEMENTED 2026-07-07)
A level-0 tile that ONLY a `CollisionOnly` anchor wants (the player clipmap did not stamp that exact key this
crossing) goes into `CollisionOnlyTiles`; `ApplyMeshToTile` cooks its collision but `SetVisibility(false)` —
**no draw, no VSM, no shadow** — killing the cost of terrain around AI / remote players far from the local
camera. If the clipmap (or a `FullVisual` anchor) also wants the tile, it renders normally. Visibility flips
on ALREADY-LOADED tiles (player walks toward/away from a cluster) are handled by `ReconcileAnchorTileVisibility`
diffing `CollisionOnlyTiles` vs its previous set each crossing (bounded by the small anchor set, no O(loaded)
scan). Collision is independent of visibility in UE, so a hidden tile still collides.
- **Still on the frame:** the geometry streams are built on the worker (`BuildTileStreamSet`) even for hidden
  tiles — off the frame, but it's CPU+memory. A deeper "cook collision without building render streams" path
  (true `CollisionOnly`, and the future dedicated-server terrain) is a later optimization.
- `SetCanEverAffectNavigation(false)` stays — nav is function-based (§9.6), not Recast, so collision tiles
  never feed a navmesh.

### 9.5 Late join
A joiner receives the seed/layout (regenerates everything locally) + a **compacted diff snapshot** replayed
into its diff layer. Nothing else needs transfer — the rest of the world is a function. The diff layer is
already chunk-keyed and lock-guarded ([[voxelforge-difflayer-threading]]); a serialize/replay path is the
main new piece.

### 9.6 AI is authority-side + function-based nav (see the AI-nav plan)
AI runs on the authority (host now, dedicated later). Nav is **function-based** — a coarse A* + funnel +
spline route over `GetVoxelSurfaceHeightAt`/`GetDensityAt` (+ the diff layer so AI sees carves), followed by
a steering component for smooth (non-robotic), cheap locomotion. Crucially it queries the world FUNCTION, so
it needs **zero loaded geometry** — ideal for the authority side and mandatory for a future headless
dedicated server (which has no meshes). Recast is rejected: it would need cooked collision everywhere AI
roams, server-side, re-cooking on every dig. Build order: multi-anchor collision streaming (§9.3) FIRST
(now MP-foundational), then the function nav.

### 9.7 What's NOT built (greenfield checklist)
Seed/layout replication at join · `Server_RequestModification` RPC + multicast of applied mods · ~~anchor
registry~~ (DONE 2026-07-07, §9.3) + ~~`CollisionOnly` render-skip~~ (DONE 2026-07-07, §9.4 — hide-based; the
deeper no-stream-build path still open) + role awareness · networked season/seed (epoch multicast) ·
diff-layer serialize/replay snapshot for late join · server-side AI + function nav. All additive on top of
today's deterministic single-player core.
