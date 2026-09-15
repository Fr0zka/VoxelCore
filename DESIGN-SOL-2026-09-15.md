# DESIGN-SOL-2026-09-15: "build it every better way", asked cold

On the owner's idea (see memory `reframe-when-returns-diminish`): after the exact-skip perf push
reached diminishing returns, we asked Sol (`gpt-5.6-sol`, reasoning high) a first-principles
design question. It was run from an empty folder, read-only, told not to read files, and it read
none (0 commands). It got the game's goals and constraints, never the code. The prompt and the
answer are verbatim below.

## Orchestrator notes (checked against the code, 2026-09-15)
- **Determinism risk confirmed in our code.** Sol warns against platform transcendentals on the
  path that decides the world. VoxelForge has 34 `FMath::Sin/Cos` calls in generation code:
  per-sample strata line/rib detail (`VoxelGenerator.cpp:4708, 4738, 5723`;
  `VoxelDensityOpStack.cpp:7507, 7609`) and shape building (passage spirals/cascades
  `VoxelStrateManager.cpp:1830-2142`, option B winding waves `VoxelCaveMorphology.cpp:6826-6843`,
  tunnel pinches `7487-7521`). The MSVC C runtime switches its math functions to FMA3
  implementations at runtime on CPUs that support it (`_set_FMA3_enable`), so bit-identical results
  across machines are not guaranteed. Found by reading, not measured, and not changed yet.
- **The largest gap is the loop direction.** Every sample searches a spatial index for nearby
  primitives. Sol's recommended architecture rasterises each feature into the bricks it touches.
  Coarse LOD from finer data and shared halo samples are also absent.
- Already shared with Sol's design: built-first traversal (walk forest, D-floors, option B),
  hash-keyed randomness, the collision gate, and partial narrow-band skipping (reach proofs, the
  sealed-rock proof).

---

## The prompt, as sent

# If you had to build this from scratch, in every better way: how would you do it?

You are being asked for **architecture**, not a code review. There is no code here on purpose: we want
your own design, not a critique of ours. Think freely, then be concrete.

⛔ **Do not read, search or open any files on this machine, and do not run commands.** Answer purely
from the description below and your own knowledge. Anchoring on an existing implementation is
exactly what this question is trying to avoid. Your final message is the whole deliverable.

## The game
An underground, multiplayer mining and exploration game in Unreal Engine 5.7 (C++). The world is a
vertical stack of **strates**: thick horizontal layers, each a distinct underground world (tunnel
networks, big chambers, mazes, shafts, floating islands, flooded areas, an open "surface" layer lit
from within). Strates are sealed from each other by solid rock, except through a few authored
**passages** that the player walks through to go deeper. Players spawn at the world origin and
travel down. They **mine and carve** the terrain freely.

## Hard constraints: any design must satisfy these
1. **Deterministic across machines.** Every client generates the world locally from a seed. Only the
   seed, the strate layout and players' terrain edits are replicated, never geometry. The same build
   with the same seed must produce the exact same world on every machine (x64 CPUs; a GPU path would
   have to prove cross-vendor/driver bit-exactness).
2. **Composable generation.** Worlds are not hand-authored. A "composer" invents strates from tagged,
   authored building blocks (room shapes, tunnel styles, noise detail, terrain features) and a seed.
   The generator must stay a composition of operators that can be recombined, and archetypes can mix
   inside one strate.
3. **Traversal guarantees.** From the point where a player arrives in a strate to where they leave,
   there must always be a walkable route (player capsule ~1.8 m tall, ~0.34 m radius, 45 cm step,
   ~44 degree max walkable slope). Tunnels have walkable D-shaped floors, wind when a direct route
   would be too steep, and may have dramatic climbable drops only where a walkable alternative
   exists. Rooms cut cleanly into tunnels.
4. **Terrain is volumetric and editable.** Caves, overhangs and arbitrary mining, so not a
   heightfield. Edits must re-mesh quickly and deterministically.
5. **Streaming must keep up with a running player** (~8 m/s, faster when falling), with collision
   ready under the player before they can stand there. Far terrain is visible at lower detail.
6. Resolution: ~25 cm voxels near the player. Target PCs: 8-core/16-thread desktop CPUs and a decent
   GPU.

## What we would like from you
1. **Two or three genuinely different architectures** for generation, representation, LOD,
   meshing, streaming and editing that satisfy ALL the constraints. For each: how the terrain is
   represented and evaluated, where the work happens, how LODs are produced, how edits work, how
   determinism is guaranteed, the expected performance character, and the biggest risks.
2. **Where the real performance wins are**, compared with the obvious design ("for every voxel
   sample, evaluate every nearby shape and noise"). Be specific: algorithms and data structures.
3. **What you would deliberately NOT build**, and why.
4. **If you could only change three things** in a working implementation of the obvious design,
   what would they be?

Be concrete. "Use the GPU" or "use ECS" is not an answer unless it addresses constraint 1 and the
composition requirement. Say plainly where you are unsure.


---

## Sol's answer, verbatim

I would make the deterministic artifact a compact “world plan,” then compile that plan locally into terrain. The plan describes topology, portals, feature instances, archetype regions, and generation-version hashes—not voxels or meshes.

My preferred implementation would be a feature-rasterized sparse brick volume. It retains operator composition, but executes operators feature-by-feature into the bricks they touch instead of asking every voxel about every feature. That is usually the best balance of predictable performance, editability, and determinism.

Two constraints need precise interpretation:

- If traversal must remain guaranteed after arbitrary player edits, “mine freely” and “always walkable” conflict. Players can mine away every possible floor. You must either protect a route, reject edits that destroy the last route, or automatically create a replacement.
- Collision readiness cannot be guaranteed under arbitrary stalls or unbounded falling using prefetch alone. A hard guarantee requires bounded terminal velocity plus admission control, an immediate conservative collision proxy, or both.

Everything below assumes generated terrain must guarantee traversal. For post-edit traversal, I would use a server-enforced “last-route covenant”: ordinary mining is unrestricted, but an edit that would destroy the final walkable route is rejected or replaced with an authored support/bridge.

## Common foundation

Before discussing terrain representations, all three architectures should share the same deterministic world-planning layer.

### 1. The composer produces a WorldPlan

For each strate, the composer creates:

- Bounds and interface rock thickness.
- Entry and exit passage transforms.
- A mandatory traversal graph.
- Optional chambers, side routes, drops, shafts, islands, flooded regions, and rewards.
- Spatial archetype regions and their tags.
- Feature instances and their stable IDs.
- Material and detail rules.
- A generation-format version and content-pack hash.

The replicated “strate layout” could be the full WorldPlan or only the high-level choices needed to reconstruct it. I favor replicating the compact plan: it prevents small composer changes from silently changing an existing saved world while still avoiding geometry replication.

Adjacent strates share authored portal contracts. The interface is solid by default, with only explicitly reserved passage volumes allowed to cross it. Cave noise is bounded and cannot affect the interface exclusion zone.

### 2. Traversal is constructed, not discovered afterward

Generate the mandatory route before decorative terrain:

1. Place entry, exit, and required intermediate anchors.
2. Route a centerline through a coarse 3D planning grid.
3. Include elevation, heading, and accumulated grade in the search state.
4. When a direct segment exceeds the design grade, generate switchbacks or a spiral.
5. Sweep a guaranteed-clear tunnel profile along that centerline.
6. Add rooms and optional routes.
7. Apply destructive detail and noise only outside the protected clearance envelope.
8. Run an independent capsule validator as an assertion.

I would design the mandatory route below the mechanical limit—perhaps 35–38 degrees rather than 44—to leave room for meshing, collision quantization, and network movement tolerances.

The protected route volume is the capsule’s configuration space: at least the capsule radius plus margin laterally, enough headroom vertically, and a conservative floor envelope. It is stronger than merely checking whether an air voxel exists.

D-shaped tunnels should come from a swept profile with:

- A controlled floor curve.
- An arched roof.
- Fixed-point parallel-transport frames along the centerline.
- Junction transition pieces where tunnels meet rooms.
- A “no detail displacement” band around the walking surface.

A dramatic drop is added only as a graph chord or optional branch. It never replaces a mandatory walkable graph edge. Graph bridges and articulation points make that easy to verify.

The final validator should perform capsule sweeps or search through capsule-eroded free space at collision resolution. If it fails, retry only the optional layout using a deterministic attempt number; do not rely on an unconstrained random retry loop.

### 3. Determinism is an explicit numeric ABI

I would not trust “same C++ code on x64” by itself.

The generation contract would require:

- Integer or fixed-point world coordinates, preferably `int64`.
- Fixed-point density and interpolation with specified rounding and saturation.
- Integer lattice noise with fixed lookup tables—no platform `sin`, `pow`, or floating-point noise libraries.
- Counter-based random values keyed by tuples such as  
  `(world seed, strate ID, feature ID, decision kind, attempt)`.
- No shared sequential PRNG streams.
- Canonical ordering by stable ID whenever operations are noncommutative.
- No dependence on hash-map iteration order, thread scheduling, pointer values, or allocation order.
- Canonical triangulation, vertex ordering, ambiguity resolution, and material selection.
- Explicit overflow behavior and wide intermediate arithmetic.
- A generation-version/content hash checked during multiplayer connection and save loading.

Floating point can be used after canonical geometry exists—for rendering transforms, shading, and other non-authoritative work. It should not determine topology, occupancy, route selection, or edit results.

GPU work is limited to rendering, culling, upload, and possibly non-authoritative visual effects. I would not make GPU field evaluation or meshing canonical unless an integer-only implementation had been proven bit-exact across vendors and drivers.

---

# Architecture A: Lazy compiled implicit field

This is the most flexible architecture for arbitrary procedural shapes.

## Representation and evaluation

The WorldPlan is compiled into an immutable field program. Operators include:

- Room primitives.
- Swept tunnel primitives.
- Terrain unions and subtractions.
- Bounded displacement/noise.
- Material masks.
- Authored feature fields.
- Player edit operators.

The field is a deterministic integer scalar field, not necessarily a mathematically exact signed-distance field. Only its sign, interpolation, bounds, and material semantics need to be defined precisely.

Every feature has:

- A conservative bounding box.
- A bound on field contribution.
- A stable ID and priority.
- A reference to a specialized evaluation kernel.

A spatial index maps macro-cells and bricks to relevant operators. When a chunk is requested, its operator set is gathered once, canonically sorted, simplified, and compiled into a chunk-local program.

Examples of compilation:

- Collapse unions of compatible room primitives into one loop.
- Separate terrain topology from material evaluation.
- Group identical tunnel kernels.
- Remove operators whose bounds cannot influence the chunk.
- Create archetype masks so that a noise style is evaluated only in its own region.
- Propagate maximum noise amplitude for later pruning.

## LOD and meshing

Use world-aligned sparse bricks, for example:

- 4 m brick: `16³` cells at 25 cm.
- 8 m mesh page: `2³` bricks.
- 32–64 m macro-cell for planning and indexing.

Sample only requested bricks and their one-cell halos. Cache corner samples so neighboring chunks do not recompute them.

LOD is generated by evaluating the same field on world-aligned grids at spacings of 25, 50, 100 cm, and so on. An interval hierarchy records minimum and maximum possible density over coarse nodes:

- If the interval is entirely solid or air, no finer samples are needed.
- If it crosses zero, refine.
- Mandatory passages can be marked as topology-critical so coarse visual LOD does not accidentally seal them.

For meshing, I would use either:

- Fixed-point surface nets with deterministic vertex placement, or
- Deterministic marching cubes with an integer asymptotic decider.

Dual contouring can produce better sharp features, but its QEF solver must be fixed-point with completely specified pivoting and rounding. A conventional floating-point QEF is a determinism trap.

Neighboring LODs remain 2:1 balanced and use fixed transition-cell tables. Every page owns vertices according to a canonical boundary rule, avoiding duplicate and differently rounded seam vertices.

LOD0 supplies the canonical collision triangles. Far LOD is render-only.

## Edits

Edits are quantized CSG stamps:

- Shape and dimensions.
- Fixed-point transform.
- Operation: mine, add, replace material, and so forth.
- Server sequence number.
- Author and optional gameplay metadata.

The server assigns ordering for noncommutative operations. Pure subtraction can be treated as commutative, but I would still preserve a total order for debugging and save compatibility.

Edits are indexed spatially. A changed edit invalidates only intersected bricks plus the meshing halo. Long edit lists are deterministically folded into per-brick edit blocks or checkpoints; the checkpoint is still player-edit data, not base-world geometry.

## Performance character

Advantages:

- Excellent composability.
- Low persistent memory.
- Arbitrary procedural primitives remain natural.
- Any LOD can be evaluated directly.
- Untouched, distant regions never need materialization.

Costs:

- Worst-case chunks can still have expensive field evaluation.
- Domain warping and heavily overlapping operators weaken spatial bounds.
- Repeated edits require compaction or their query cost grows indefinitely.
- Fixed-point vector math and meshing require careful engineering.

The main risk is believing the spatial index alone solves performance. If every surface sample still evaluates 40 candidate features and six noise octaves, this architecture can miss streaming deadlines.

---

# Architecture B: Feature-rasterized sparse brick volume

This is the architecture I would most likely ship.

Instead of the field being the runtime truth, the WorldPlan is compiled into canonical sparse voxel bricks. Operators remain composable, but execute in bulk.

## Representation and generation

Each brick is one of:

- Uniform solid.
- Uniform air.
- Compressed material-only block.
- Dense or narrow-band fixed-point density samples.
- Edited dense block.

The generator processes features rather than samples:

1. Determine which bricks a tunnel, room, or feature intersects.
2. Rasterize that feature into those bricks using a specialized kernel.
3. Apply phases in a canonical order: structural voids, rooms, optional features, surface detail, materials, edits.
4. Never touch bricks outside the feature’s conservative bounds.

A swept tunnel rasterizer iterates its centerline segments and their affected scanlines. A room stamp writes spans or masks. Authored building blocks can contain canonical sampled volumes and be placed using resampling-free transforms such as 90-degree rotations and reflections. More general transforms use a precisely specified fixed-point resampler.

For boolean-only phases, contributions can be accumulated independently and reduced in any order. Noncommutative phases use sorted feature IDs and ping-pong channels.

Noise becomes a bulk brick kernel. Better still, it runs only on bricks already known to contain a possible surface. Bounded noise cannot turn a deeply solid or deeply empty brick into a surface brick.

A sparse radix tree or hashed macro-cell table owns bricks. The storage itself need not have deterministic iteration order, because all externally visible processing uses sorted integer coordinates.

## LOD and meshing

Every brick produces a small hierarchy containing:

- Minimum density.
- Maximum density.
- Material summary.
- Surface-presence mask.
- Optional conservative solid/air coverage.

Parent LOD bricks are derived using an exact integer reduction. A simple averaged density is insufficient because it can erase thin walls or close tunnels. Min/max information decides whether a coarse region is definitely homogeneous or must retain detail.

Mesh generation reads the brick and its halo, then uses the same deterministic extractor and transition cells as Architecture A.

A useful split is:

- 25 cm render and collision near players.
- Potentially coarser conservative collision away from traversal-critical regions.
- Far render pages assembled from brick hierarchy nodes.

## Edits

Edits operate directly on affected brick spans or samples. This makes mining latency extremely predictable:

- Copy-on-write the changed brick.
- Apply the quantized edit kernel.
- Recompute only its mip ancestry.
- Remesh it and its boundary neighbors.
- Recook only affected collision pages.

Keep the original generated brick reconstructible from the WorldPlan. A save stores the edit operations or a canonical edit-derived brick delta, never the generated base brick.

## Performance character

Advantages:

- Work is proportional to affected brick volume, not `samples × candidate operators`.
- Excellent cache locality and straightforward SIMD.
- Uniform regions are extremely cheap.
- Edits are fast and bounded.
- Collision and LOD data naturally derive from the same representation.
- Performance is more predictable than lazy per-sample evaluation.

Costs:

- Higher memory use.
- A requested area must be materialized before use.
- Poorly bounded features can still touch too many bricks.
- Large filled volumes are wasteful unless uniform-brick detection happens early.
- Authored sampled blocks can become repetitive if the composer does not provide enough transforms and procedural variation.

The main risk is turning it into a dense voxel world. The architecture works because homogeneous regions remain implicit and only surface/edit bricks become dense.

---

# Architecture C: Deterministic shell-first base with local voxelization for edits

This is a genuinely different option and can be extremely fast when most terrain remains untouched. It is also the most difficult to engineer robustly.

## Representation and generation

The unedited world is represented as deterministic watertight boundary patches:

- Polygonal swept tunnels.
- Parametric room shells.
- Terrain surface patches.
- Portal and junction patches.
- Bounded displacement patches.

Solid rock is implicit outside the air shells. There is no full base density volume.

All topology decisions use fixed-point or exact rational predicates. Curves are canonical polygonal approximations generated from quantized tables. Intersections and room/tunnel cuts use a deliberately restricted exact boolean system—not a general floating-point mesh boolean library.

The WorldPlan spatially indexes these patches. Untouched chunks triangulate only the surface patches that intersect them.

## LOD and meshing

Base patches have deterministic subdivision hierarchies. Adjacent patches share boundary vertex schedules, so independently generated LODs meet exactly. Far LOD selects coarser patch subdivisions.

When a player edits terrain, the affected area switches representation:

1. Allocate sparse voxel bricks around the edit.
2. Classify the base shell into those bricks.
3. Apply the edit operations.
4. Mesh the local volume.
5. Use a collar region whose boundary samples match the untouched shell.

An edit completely inside solid rock begins from a uniform-solid brick and creates a new cavity. An edit touching a cave wall classifies the nearby base shell first.

Collision uses base shell triangles in untouched regions and voxel-derived triangles in edited regions.

## Determinism

The shell topology, clipping predicates, tessellation, and triangle order must all be integer/rational and canonical. Conventional runtime mesh booleans are not acceptable.

## Performance character

Advantages:

- Untouched terrain costs roughly proportional to visible surface area.
- No field evaluation across solid interiors.
- Very cheap far LOD.
- Works especially well for huge chambers and relatively localized mining.
- Traversal proxy geometry is available immediately from the route plan.

Costs and risks:

- Exact, watertight joins between arbitrary procedural shapes are hard.
- Noisy displacement and multi-way junctions are particularly troublesome.
- The boundary between shell terrain and voxelized edited terrain is complex.
- Large-scale mining gradually converts the world into Architecture B anyway.
- Tooling and debugging burden are substantially higher.

I would consider this only if profiling showed base-world generation dominating and player edits remained spatially sparse. I would not choose it as the first production architecture.

---

# Streaming and collision

The streaming scheduler should be shared across architectures and driven by deadlines, not simple distance.

Maintain separate interest sets:

- Immediate safety set: LOD0 collision required.
- Near visual set: LOD0 or LOD1 meshes.
- Predicted movement set: locations reachable within a time horizon.
- Far visual set.
- Edit-dirty set.
- Passage prefetch set for the next strate.

Priority is approximately “time until the player could touch this page,” accounting for:

- Current velocity and acceleration.
- Character braking.
- Network prediction uncertainty.
- Gravity and terminal fall velocity.
- Nearby shaft connectivity.
- Other players on the authoritative server.

A vertical fall requires a swept ballistic volume, not a horizontal streaming radius. Known shafts and authored passages should prefetch their entire reachable landing corridor as soon as a player approaches them.

The worker pipeline is:

`plan/index lookup → field sampling or brick rasterization → mesh → collision cook → game-thread publication`

Each stage has revision numbers and supports cancellation. Results are published only if their input revision is still current.

Collision receives the highest priority. For a hard guarantee, I would use both:

- Immediately available conservative collision proxies derived from the route plan.
- Movement admission: a character cannot cross into a page whose required collision revision is not active.

The latter should almost never trigger, but without it “collision will always be ready” is only a performance hope. Under severe overload, slowing a character at a streaming boundary is better than letting them fall through the world.

On Unreal’s side, generation data should remain plain C++ memory off-thread. UObject/component creation and scene publication happen on the game thread. I would use a custom terrain scene proxy and a dedicated collision provider rather than one heavy actor/component per tiny brick.

The server generates collision for all relevant players and remains authoritative. Clients generate the same static terrain for prediction and rendering. Physics simulation itself does not need cross-machine determinism as long as the server is authoritative, but the collision input triangles must still be canonical.

---

# Where the real performance wins are

The dominant improvement is changing the work from:

`number of samples × number of nearby shapes × number of noise layers`

toward:

`features touching requested bricks + cells near actual surfaces + dirty edit cells`

Specifically:

## 1. Feature-to-brick compilation

Build a macro-cell index when the WorldPlan is created. A requested brick gets a small, already-filtered operator program.

Better still, Architecture B iterates each feature over its covered cell spans. A sphere or swept tunnel should rasterize scanlines; it should not be rediscovered independently by every voxel sample.

Use separate indices for immutable world features and mutable edits.

## 2. Interval bounds and narrow-band evaluation

Every primitive and noise stack should expose conservative bounds.

If the base field for a region is farther from zero than the maximum possible displacement, skip all detail noise. This is especially important for expensive domain warps.

For an octree node:

- Entirely solid: emit no samples.
- Entirely air: emit no samples.
- Possibly crosses the surface: subdivide or evaluate.

This often removes more work than SIMD optimization.

## 3. Stop evaluating every archetype everywhere

Give archetypes spatial masks determined at planning resolution. A crystal-cavern noise stack should not even enter the chunk program for a neighboring sedimentary tunnel.

At transitions, compile only the two or three contributing archetypes and a deterministic blend mask.

## 4. Cache lattice samples and halos

Adjacent cells and chunks share corners. Cache canonical world-grid samples by brick revision. Do not evaluate each corner up to eight times, and do not let two chunks compute a shared boundary using different local coordinate paths.

## 5. Use bulk data layouts

For dense bricks:

- Structure-of-arrays density and material channels.
- Fixed-size aligned slabs.
- SIMD integer kernels.
- Bitmasks for active surface cells.
- Scanline/span writes for primitives.
- Small thread-local scratch arenas.
- No allocation in inner loops.

Bitsets are particularly effective for “possibly contains surface,” material-presence, and dirty-cell masks.

## 6. Incremental edit dependency tracking

An edit should invalidate:

- Intersected bricks.
- One-cell mesh halos.
- Their LOD ancestors.
- Their collision pages.

Nothing else.

Give every brick separate generation, edit, mesh, and collision revisions. This prevents a late worker result from publishing obsolete terrain.

An ever-growing edit list is not viable. Compact edits into canonical per-brick deltas after a threshold while preserving the original network/save semantics.

## 7. Separate collision urgency from visual quality

Generate collision first. It can initially use a conservative, lower-complexity surface as long as it preserves walkability and does not allow passage into solid rock. Refine the render mesh afterward.

Do not block collision readiness on material extraction, tangents, decoration, meshlets, or high-quality far-LOD work.

## 8. Exploit underground visibility

In an underground world, Euclidean distance is a poor visibility predictor. Build coarse visibility cells from the chamber/tunnel graph. Far terrain behind several sealed turns can be deprioritized even if spatially close.

GPU occlusion and meshlet culling help rendering, but avoiding CPU generation of invisible far surfaces is the larger win.

---

# What I would deliberately not build

- A dense 3D array for an entire strate. Solid rock and empty chambers must remain sparse or implicit.
- A global sequential random stream. Adding one decoration would change the rest of the world.
- Floating-point generation decisions, floating mesh booleans, or platform math functions in the canonical path.
- GPU-authoritative generation or meshing without proven integer bit-exactness.
- A generic interpreted CSG graph evaluated from its root for every sample.
- Unlimited domain-warp noise. It destroys useful spatial and amplitude bounds.
- A general-purpose runtime B-rep boolean engine. Architecture C should support a narrow, controlled set of exact operations.
- Independent marching-cubes meshes at each LOD with skirts added later as a patch. LOD ownership and transition topology need to be designed from the start.
- Runtime full-strate flood fills as the primary traversal guarantee. Connectivity should be structural; validation is a backstop.
- A never-compacted replicated edit list.
- One Unreal actor or UObject per voxel brick.
- Dependence on dynamic Nanite behavior for core terrain correctness. It may be useful for rendering, but collision, edits, and determinism need an independent path.
- Streaming based only on concentric distance rings.
- A system where render mesh completion is the only thing preventing a player from falling through unloaded terrain.
- A claim that arbitrary post-edit traversal is guaranteed without restricting, rejecting, or repairing edits.

---

# If I could change only three things

1. **Introduce a deterministic topology-first WorldPlan compiler.**  
   This establishes stable IDs, counter-based randomness, portal contracts, protected walkable routes, bounded feature instances, archetype masks, and a versioned numeric ABI. It fixes correctness and gives later systems much better information.

2. **Replace per-sample feature iteration with feature-to-brick compilation and sparse surface bricks.**  
   Build spatial indices, rasterize features in bulk where possible, apply noise only within bounded surface bands, cache shared samples, and maintain min/max LOD hierarchies. This is likely the largest CPU improvement.

3. **Replace distance-only chunk streaming with a revisioned, deadline-driven collision pipeline.**  
   Predict reachable motion including falls, prefetch passages and shafts, prioritize collision independently, invalidate only edit dependencies, and retain a conservative collision/admission fallback. This turns streaming from “usually fast enough” into an enforceable gameplay contract.

My first production choice would be Architecture B, while retaining Architecture A’s immutable operator language as the authoring and compilation input. In other words: compose with procedural operators, but execute them into sparse bricks feature-by-feature. It gives up a little theoretical elegance in exchange for much more controllable frame time, edit latency, collision readiness, and debugging.
