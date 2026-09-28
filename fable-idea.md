# fable-idea.md — performance & feature ideas

*Fable 5, max-effort pass — 2026-06-09. Grounded in a read of the actual code (mesher, apply path, task launch, content manager), not generic advice. Companion to CODEMAP.md §8. Items marked ✅ are implemented (evidence inline); everything else is still a menu.*

---

## Part I — Performance

### 0. Measure before anything (half a session, directs everything else) — ✅ DONE

*Evidence:* `stat VoxelForge` group (`Public/VoxelStats.h`, `STATGROUP_VoxelForge` tile-classification/meshing
counters) and Insights scopes around the stages (`VoxelForge_GenerateMesh`, `VoxelForge_BuildStreams`,
`VoxelForge_ApplyMeshToChunk`, `VoxelForge_Tick` and its sub-steps).

Add `TRACE_CPUPROFILER_EVENT_SCOPE` around the four stages of a chunk (density grid sample / MC loop / normals / RMC stream build+upload) plus a `stat VoxelForge` group: chunks pending, applies this frame, avg gen ms, verts/chunk. One Unreal Insights capture then tells us if we're density-bound (my bet) or upload-bound, and every item below gets a before/after number. Cheap insurance against optimizing the wrong thing.

### The verified cost model (what I found reading the code)

A LOD0 chunk today costs roughly:

| Stage | Cost | Source |
|---|---|---|
| Density grid | 33³ = 35,937 `GetDensityAt` | `GenerateMesh` pre-sample (already optimal shape) |
| **Vertex normals** | **+6 `GetDensityAt` per unique vertex** (~12–36k more for 2–6k verts) | `ComputeGradientNormal` — central differences, *per vertex* |
| Heightfield redundancy | SurfaceWorld's XY-only terrain stack (~16 Perlin evals: warp 2 + relief 2 + continents 4 + detail 4 + mountains 4) recomputed **for all 33 Z samples of a column** | `GetSurfaceDensity` is pure per-call |
| Noise core | Scalar, **double-precision** `FMath::PerlinNoise3D(FVector)`, 4-octave loops | `FractalNoise3D`/`RidgedNoise3D` |
| Collision | **Cooked for every chunk at every LOD** | `UpdateSectionConfig(SectionKey, Config, /*bShouldCreateCollision=*/true)` |
| Apply | Unbounded `while (ProcessQueue.Dequeue(...))` drain — all finished chunks upload in one frame; `Enqueue(Result)` copies the whole MeshData; RMC StreamSet built on the **game thread** | `VoxelWorld.cpp:410`, `:602`, `ApplyMeshToChunk` |
| Components | `NewObject`+`Register` per load, `DestroyComponent` per unload (no pooling) | `LoadChunk`/`UnloadChunk` |

So: normals can cost as much as the entire density grid; a SurfaceWorld chunk does ~33× redundant heightfield work; and every distant LOD2 chunk pays Chaos tri-mesh cooking the player can never touch.

### Tier 1 — high win / low risk / small-medium effort (do these first)

> **STATUS 2026-07-05: Tier 1 COMPLETE.** T1.a ✅ (surface-column cache; since cleanup/2026-09 the density
> path's copy is the surface stack's `FSurfaceColumnSource` memo and `GSurfColCache` serves `ClassifyTile` only);
> T1.b ✅ (grid-based normals inline in `GenerateMesh`);
> T1.c ✅ (`bShouldCreateCollision` = level 0 only, VoxelWorld.cpp ApplyMeshToTile); T1.d ✅ (v2
> `ClassifyTile`, trace-verified −44 % worker CPU); T1.e ✅ (`MaxMeshAppliesPerFrame`, default 4);
> T1.f ✅ (`BuildTileStreamSet` runs in the gen task; apply only uploads).
> **Per-tile generation cost is now in diminishing-returns territory — remaining perf lives in
> Tier 3, Transvoxel, and the streaming/crossing work (see voxelforge-lod-transition-cost memory).**

**T1.a — Per-column XY cache for heightfield work.** Split SurfaceWorld density into `f_XY` (everything up to `Terrain`, + WaterZ) and a trivial Z-combine. In the chunk task, compute a thread-local `(GridDim+margin)²` column grid once, then `GetDensityAt` reads it. Heightfield evals drop ~33× (36k → ~1.2k). Same pattern later for any archetype with an XY-only sub-field (biome map will be one). Keep it keyed like the existing per-chunk param cache; purity in world coords is preserved so it stays bit-identical and window-invariant.

**T1.b — Normals from the density grid, not 6 fresh samples per vertex.** Sample the grid with a 1-point margin ring — `(GridDim+2)³` = 42.9k at LOD0, +19% — then compute vertex normals by central differences *on the grid* (trilinear-interpolate the 8 cell-corner gradients at the vertex position). Net: ~48–72k density calls → ~43k, normals become batchable, and chunk-border shading stays continuous because the margin uses the same pure world-coord samples a neighbor would. Trade-off: gradient resolution becomes `Step` instead of `GradientOffset` — slightly softer normals, smoother (good) at distance. Verify visually at LOD0.

**T1.c — Collision only where it matters.** `bShouldCreateCollision = (LOD == 0)`. Distant chunks are unreachable by definition (if the player got there, they'd be LOD0 — and the LOD reconciliation loop §8.10 guarantees a hot-swap on approach). Kills Chaos cooking + collision memory for the large majority of loaded chunks. Also check RMC's async-collision setting is on. **Likely the best win-per-line-changed in the whole document.**

**T1.d — Chunk classification: skip trivially solid/air chunks before sampling.** ✅ DONE 2026-07-05
⛔ **Turned OFF in streaming 2026-09-13.** After the operator stack, it cost more than it saved on the
game path (Docs/archive/WORK-NEXT.md A/B). Do not re-propose tile classification without a design that is
cheaper than meshing the tile.
(v2 — `UVoxelGenerator::ClassifyTile`, see ARCHITECTURE §8.10; a 2026-06-26 v1 with a global ceiling
bound was reverted for roof holes. Trigger: trace showed 84 % of GenerateMesh calls produced empty
tiles.) In a tall multi-strate world most chunks in the desired set are full bedrock or full sky. Conservative per-chunk test before the 33³ sample: for heightfield strates, min/max terrain over the footprint (free from T1.a's column grid ± noise amplitude bound) vs the chunk's Z range; for cave strates, "no room/tunnel/passage/spine/seal/diff-layer bounds intersect" (all bounding data already exists). Fully-solid/air ⇒ empty MeshData, no component, done. Cuts whole chunks, not percentages — compounds with everything else.

**T1.e — Bound the apply side.** The submit loop is budgeted; the drain loop isn't. Cap mesh applies per frame (~2–4, generous for carves), keep draining *results* into a pending-apply list sorted by player distance. Also `ProcessQueue.Enqueue(MoveTemp(Result))` — currently the whole vertex/index payload is copied. Smooths the burst hitch (the "upload spikes" already observed).

**T1.f — Build the RMC StreamSet inside the worker task.** `FRealtimeMeshStreamSet` is plain data — the per-vertex Builder loop in `ApplyMeshToChunk` can run in the chunk task; the game thread then only does `CreateSectionGroup(MoveTemp(Streams))` + config. Removes a few ms of game-thread work per applied chunk; pairs with T1.e.

### Tier 2 — multiplicative, more effort

> **STATUS 2026-07-04:** T2.a ✅ DONE (float SSE `VoxelNoise.h` core — see ARCHITECTURE §8.10);
> T2.b ✅ DONE (opt-in `LODOctaveDrop`, default 0); T2.c ✅ DONE (`TileComponentPool`);
> T2.d ✅ DONE (BackgroundNormal priority had already shipped; core-clamp via
> `GetMaxConcurrentTasks`); T2.e ✅ DONE (per-chunk passage shortlist). **Tier 2 complete.**

**T2.a — Float + SIMD noise core (the big multiplier).** Everything funnels into scalar double-precision `FMath::PerlinNoise3D`. Two routes: an **ISPC kernel** (UBT compiles `.ispc` natively — zero third-party deps; used by Chaos/Niagara) or **FastNoise2** (MIT, runtime SIMD dispatch). Evaluate fractal/ridged noise over the whole flat grid / column grid in one batch call per field. Realistic 4–10× on the noise-bound part, on top of T1.
⚠️ Two correctness notes: (1) world changes for existing seeds — do this *before* content lock-in, bump a generator-version constant; (2) seed offsets like `SeedF * 7.3f` can reach 1e8+ where float precision is ~64 units — hash the seed into a bounded offset range (e.g. [0, 16k]) in the float core or noise quantizes.

**T2.b — LOD-aware octave count.** At Step=4, octaves with wavelength < the cell size are pure aliasing cost. `EffectiveOctaves = Octaves - LODBias(Step)` per field (keep the *low* octaves identical so the coarse shape matches). 30–50% off distant chunks; iso-surface shifts by sub-cell amounts that Transvoxel/skirts have to stitch anyway. Cheap, do together with T2.a.

**T2.c — Component pooling + no components for empty chunks.** Recycle `URealtimeMeshComponent`s through a free list on unload instead of `DestroyComponent`/`NewObject`/`Register` churn (T1.d already stops creating them for empty chunks). Reduces GC pressure and register/unregister hitches during fast travel.

**T2.d — Task priority + worker count.** `UE::Tasks::Launch(..., LowLevelTasks::ETaskPriority::BackgroundNormal)` so a 16-task gen burst can't starve game/render workers; consider `MaxConcurrentTasks = Clamp(NumberOfCores - 2, 2, 16)` instead of a flat 16 on smaller CPUs.

**T2.e — Per-chunk passage gather.** `EvaluateModifierSDF` sphere-tests *every* passage per voxel. Gather the passages whose bounds intersect the current chunk once into the thread-local chunk cache; the per-voxel loop walks that short (usually empty) list. Matters as passage counts grow with deeper worlds.

### Tier 3 — when carving becomes the moment-to-moment verb

**T3.a — LRU base-density grid cache for carve re-mesh.** Keep the LOD0 density grid for the ~32–64 most recently carved/near-player chunks (~144 KB each ⇒ < 10 MB). A carve then re-meshes as *cached grid + diff + MC* — no noise at all. Dig feedback becomes effectively instant, which is exactly where the game's feel lives.

**T3.b — Streaming feel:** frustum-weighted priority bonus in the submit sort (load what the player looks at first), and the pre-load/unload hysteresis ring already discussed (kill leading-edge pop and boundary churn).

### Explicitly NOT now (and why)

- **GPU density/meshing** — 100× throughput on paper, but a rewrite: readback latency, CPU collision still needed, float determinism across GPUs. North star only if T1+T2 ever hit a wall; they won't for this scope.
- **Octree/adaptive within-chunk structures** — T1.d gets the win at chunk granularity for ~5% of the complexity.
- **CHUNK_SIZE change, Nanite, greedy meshing** — lever already documented (§8.10) / no runtime procedural Nanite / MC isn't blocky.

**Expected compound for a SurfaceWorld chunk: T1.a × T1.b × T2.a ≈ order-of-magnitude on generation; T1.c/d/e attack frame-time and chunk count independently.**

---

## Part II — Features

### A. Tooling first — force multipliers for everything after

**F1 — 2D world-preview editor tool. ✅ BUILT 2026-09-04 (measurement + preview).** ★ my top pick. The
editor/automation pass captures the exact coarse measurement grid, writes deterministic PNG XZ and
XY slices plus a self-contained 64-candidate contact sheet, and labels walkable cells, scale, metric
windows, rejection reasons, and the 2D-connectivity caveat. Output is under
`Saved/ComposerPreview/<runid>/`; no runtime generation path is involved. The original idea remains:
sample `f_XY` (terrain height, relief M, water mask — later the biome map) over an N×N window with
the Surface|Macro knobs live, then extend with a top-view passage-path overlay (the data exists in
`bDebugDrawPassages`).

**F2 — Determinism validator button.** ✅ DONE 2026-07-04 (`AVoxelWorld::ValidateDeterminism`,
Live Edit category). CallInEditor: sample a band of densities from two different chunk-window alignments, diff, report max delta. Turns the scariest invariant (§8.4 — window invariance) into a one-click regression test *before* biome code starts landing.

**F3 — `stat VoxelForge` + Insights scopes. ✅ DONE** (see Perf §0 for the evidence). Same as Perf §0 — listed here because it's also the tool that tells us when a feature regressed something.

### B. The "this becomes a game" features

**F4 — Save/load.** The diff layer is the player's entire footprint and it currently dies with the session. Serialize: seed + settings hash + generator version + per-chunk modification lists (they're compact structs already). Versioning matters: stamp saves with a gen-version so a noise change (T2.a!) can refuse/migrate old saves instead of silently shifting terrain under bases.

**F5 — Biome system. ✅ DONE (warped-Voronoi + climate XY field, per-chunk resolve —
`ResolveBiomeSampleAt` / `FBiomeContext`, VoxelGenerator.cpp §biomes; deco borders follow the
Voronoi field).** Original sketch (kept for the cave-biome extension): Deterministic XY biome map = warped Voronoi/cellular cells (seeded, window-invariant by construction, same family as the relief map — and relief M should be an *input*: mountain biomes live where M is high). Resolution rules to protect §8.10: resolve **per chunk** (dominant biome + ≤2 neighbors + blend weights, stored in the thread-local chunk cache); per **voxel** blend only a handful of scalars (height offset, roughness, terrace, water tint index). Per biome: a *content profile* — decoration set, atmosphere/audio override, material palette index, water level offset. Archetype transitions stay Hard; biomes vary *within* SurfaceWorld first, cave-biomes (crystal/fungal/ice) reuse the identical pattern later.

**F6 — Material identity: vertex-data masks + triplanar palette material. ✅ DONE (Stage 1)** — the mesher
writes per-vertex masks into `FVoxelMeshData::Colors` (R = dominant biome palette, G = slope, B = border blend,
A = neighbour palette; `VoxelMarchingCubesMesher.cpp`), fed by `UVoxelBiomeDefinition::MaterialPaletteIndex`
(ARCHITECTURE §8.15). Original note: Geometry variety without *surface* variety still reads samey. At mesh time, pack per-vertex: slope (from the T1.b normal), relative height, biome/material index (from F5) into vertex color channels. One master material: triplanar rock/grass/sand/snow layers selected & blended by those masks + a macro-variation texture. This is the single biggest *visual* multiplier available and it's mostly material-graph work.

**F7 — POI / set-piece system. ◐ PARTLY DONE** — the placement side is built: `FStrateLandmark` with
`AnchorMode` (HashLattice / PassageMouth), exclusion and set-pieces folded in, `FTerrainCondition` aware
placement, and `FDecoCompanion` satellites (ARCHITECTURE §8.5). Not built: composed SDF carve/fill stamps.
Original note: Noise terrain everywhere = beautiful nowhere. Deterministic destinations: chunk-hash-placed stamps (composed SDF carves/fills + a decoration prefab + optional ambient actor), e.g. buried shrines, crystal gardens at passage mouths, ruins on mesas. Placement uses the same two-region COLLECT discipline as rooms (§8.4). Destinations are what turn wandering into stories ("found a shrine at −400 m").

**F8 — Ore veins / diggable resources.** The game's verb is digging; give digging a reward loop. A secondary material-id field (cheap 3D noise threshold, per-strate/biome tables with depth curves) evaluated **only at mesh vertices** (≈ free) → vertex color → material shows veins; on carve, query the field at the brush center → grant resource. No per-voxel density cost, fully deterministic.

**F9 — Audio/ambience manager.** The exact architectural twin of the atmosphere manager (player strate/biome → assets): ambient loop crossfade, cave reverb submix, dig impacts by surface type, a stinger + title card on first strate entry. Sound is half of cave atmosphere and this is days, not weeks.

**F17 — Generator surface-class tag (ceiling/ground/cave material the *right* way). ✅ DONE 2026-07-05** (per-vertex semantic class in the mesher — down-facing verts query a memoized `GetSurfaceHeightAt`, nearer CeilSurf ⇒ sky-cap — per-tri majority → two contiguous polygroup runs → RMC section per group, slot 1 = `CeilingMaterial`, per-section shadow. Trigger: the whole-tile normal vote painted mixed coarse tiles with one material. Cave-roof discrimination hook is in place: down-facing below TerrainZ ⇒ ground/rock. Remaining polish idea: fully sideways cap-fold tris (all 3 verts |N.Z|≤0.1) default to ground.) Original design note: ★ do this when caves land. Today `ApplyMeshToTile` picks one material per tile from a ceiling test — first a height-oracle sample (midpoint), now a worker-side **normal vote** over the tile's mesh normals (down-facing ⇒ `bIsCeiling` ⇒ `CeilingMaterial` + no shadow; gated to SurfaceWorld by one `GetSurfaceHeightAt` probe). That's a **stopgap that only works because down-facing == sky-cap *while no caves exist*.** The moment a mountain-biome cave uses the same density/mesh system, its roof is also down-facing and would wrongly get the sky-cap material — orientation can't tell a cave ceiling from a surface ceiling. **The discriminator is semantic, not geometric, and only the generator knows it:** a sky-cap surface is the `ComputeSurfaceCeiling` (`CeilSurf`) boundary; a cave ceiling is a 3D-noise **carve** below `TerrainZ`. Cheap test the generator already has the inputs for — at a down-facing surface vertex, compare world Z to the column's `TerrainZ`/`CeilSurf` (both from the surface-column cache the mesher already holds): near `CeilSurf` ⇒ sky-cap, below `TerrainZ` ⇒ cave. **Plan:** stamp a discrete *surface class* (ground / sky-cap / cave-ceiling / cave-wall…) per vertex/triangle **at mesh time** in the mesher → carry it as the **polygroup** (already enabled, `Builder.EnablePolyGroups()`, every tri currently group 0) → `ApplyMeshToTile` maps polygroup → material slot (slot 0 terrain, slot 1 sky-cap, slot 2 cave-rock, biome-specific via the F5 palette mask) and sets per-section shadow. Discrete "which material" → polygroup/slot; continuous masks (biome blend, slope — F6) stay in `Colors`. This **subsumes** the current ground/ceiling split (it falls out as a special case), fixes the coarse mixed-tile horizon artifact exactly (per-triangle, not per-tile dominant-wins), and is the only version that survives caves. Pairs naturally with F5/F6/F8 (all want generator-stamped per-vertex material identity). Cost: a per-tri classify in the mesher (cheap, has the cache) + multi-slot setup in the apply path (RMC supports it; confirm the v5 per-section `UpdateSectionConfig` / slot-per-polygroup calls + empty-polygroup = no draw). Until then: the normal vote is fine — it's commented as "no caves yet → down == cap."

**F18 — Far-field per-surface SHEETS. ⛔ REMOVED (owner decision, 2026-09 cleanup).** Built 2026-07-06 as a
render-distance ring of heightfield "sheet" tiles past `MaxClipLevel` (meshed by `GenerateSheetMesh`); the
runtime path, its settings and the selector's sheet branch are deleted; the render-distance ring is plain
level-`MaxClipLevel` MC tiles (ARCHITECTURE §8.10).

**F19 — AI navigation & agents (function-based). PARKED (no NPCs yet); FOUNDATION BUILT 2026-07-07.**
Mobs need two things the world didn't give them: (1) to *exist* away from the local player, (2) to *route*.
- **(1) DONE — multi-anchor collision streaming + render-skip** (ARCHITECTURE §9.3/§9.4, built 2026-07-07):
  `AVoxelWorld::RegisterStreamingAnchor(actor, CollisionOnly, thin box)` keeps a small box of level-0
  collision tiles loaded around any actor; `CollisionOnly` tiles cook collision but are hidden (no draw/VSM)
  unless the player clipmap also wants them. So an NPC/remote-player has ground to stand on / be hit on,
  cheaply, anywhere. Same system serves MP (stream around every player) — [[voxelforge-multiplayer]] §9.
- **(2) TO BUILD — routing, function-based (NOT Recast).** The world is a cheap deterministic function, so
  nav = a query, not a baked navmesh: coarse A* / flow-field over a grid sampling `GetSurfaceHeightAt`
  (slope + water gated, + the diff layer so AI sees carves) → **funnel/string-pull** → **Catmull-Rom spline**
  → a **steering follow-component** for smooth, non-robotic, cheap locomotion (path once, re-path on a timer;
  budget requests like the streaming loop). Needs **zero loaded geometry**, deterministic (same seed = same
  path), and **digging costs it nothing** (no per-carve re-cook). Bridgeable to `AIController`/`CharacterMovement`.
- **Analytic surface-follow (the free tier):** a pure *surface walker* needs no collision AND no navmesh —
  pin it to `GetVoxelSurfaceHeightAt` each tick. Reserve real collision (the anchor) for physics / caves /
  carved terrain / being hit by player traces. Both coexist, decided per NPC type.
- **Recast rejected:** would need cooked collision everywhere AI roams, re-cooking on every dig — and it's
  unusable on a future headless dedicated server (no meshes), where function nav is not just cheaper but
  mandatory. AI is server-authoritative (§9.6). ★ Build when NPCs actually land.

**F20 — Biome-selected surface terrain ops (terrace / cliff / layer-lines / overhang / spike / hole). SPEC 2026-07-07.**
Terrain ops were cave-only at the time (per-room in `GetDensityWithParams`; the SurfaceWorld density applied NONE — that's
the "ops don't work on the surface" report). This brings them to the SURFACE, as a **biome** property,
**conditioned on local terrain** so they read geological instead of random. (Slots in ahead of the later
cave-system redo, which will add biome support cave-side reusing this same op→biome model.)

*Data:* add `TArray<FStrateTerrainOpEntry> SurfaceOps` to `UVoxelBiomeDefinition`, each entry gated by a
condition (Min/MaxSlopeAngle like `FStrateDecoration` already has, + optional relief/height band via the F7
`FTerrainCondition` set — add a `Slope` type). Resolved through the biome field (dominant biome per column —
already cached). Empty ⇒ early-out ⇒ **zero cost**, so the feature is free in every biome that doesn't use it.

*The cheapness architecture (the whole point — think per-COLUMN first, per-voxel only when forced):*
- **Two op classes.** HEIGHTFIELD ops modify the cached column → **~free**: **Terrace** (quantize TerrainZ into
  steps), **Cliff** (sharpen the height transition where slope is high), **LayerLines/Ribbing** (a `sin(Z)`
  groove in the near-surface band — no noise). VOLUMETRIC ops need genuine 3D near the surface → real but
  bounded: **Overhang**, **Spike**, **Hole**.
- **Slope/relief conditioning is free AND is what makes them look right.** Slope = the surface height gradient
  (2 extra cached-column samples, or reuse the mesh normal / F6 slope channel). Overhang strength ∝ slope ⇒
  overhangs grow out of EXISTING cliffs, never poke out of flat ground (the "would it look weird" answer: no,
  it's cliff-conditioned, not random). Terrace on moderate slopes; spikes where relief M is high (mountains).
  Conditions cost ~0 (fields already computed) and double as the "not random" guarantee.
- **Volumetric work is banded + tile-skipped.** Overhang = low-freq 3D displacement only in a ±few-voxel band
  around TerrainZ, only where slope-gated + the biome has it. Non-straddling tiles never pay (T1.d ClassifyTile).
  Coarse/far tiles stay pure heightfield (LOD-cull the 3D ops — they're near-field detail; the sheet ring ignores
  them entirely).
- **Spikes/holes = hash-placed SDF via a per-tile shortlist** (cone/capsule UP for spikes, shaft DOWN for holes;
  placed on a hash lattice like landmarks, collected once per tile like rooms/passages; per-voxel tests a 0-3
  shortlist). Biome+condition-gated so empty biomes collect nothing.
- **GOTCHA — the dominant cost, spikes/holes only:** a spike rises INTO otherwise-all-air tiles and a hole carves
  INTO otherwise-all-solid tiles → they DEFEAT T1.d's trivial-skip for every tile they pass through (those must
  now mesh). So ClassifyTile needs a spike/hole shortlist guard (cheap AABB, like `AnyPassageNearBox`), and a tall
  spike "wakes up" the whole vertical stack of air tiles it crosses = more meshed tiles + collision. Overhang/
  terrace do NOT do this (they stay in the already-meshed surface band). ⇒ keep spikes SHORT + SPARSE; they're the
  priciest of the set in tiles-meshed + triangle/collision terms, not just density evals.

*Cost:* heightfield ops ≈ free. Overhang ≈ 1.5-2× noise on slope-gated surface tiles, ~0 elsewhere. Spike/hole ≈
that PLUS the woken tiles (the real cost — budget by count/height). All biome-gated ⇒ world-average cost ≈ 0;
you pay only near the player, in the biomes that opt in.

*Determinism/borders:* op strength blends by biome weight (like the surface height output-blend), conditioned ops
fade with slope ⇒ seamless at biome borders; placement hashes are pure `(seed, coord)` ⇒ window-invariant (§8.4).

*Build phasing:* **(1)** heightfield ops (terrace/cliff/layerlines) — cheap, high payoff, low risk, ships the
"ops finally work on the surface" win; **(2)** overhang (3D band, slope-conditioned); **(3)** spike/hole (placed +
shortlist + ClassifyTile guard) — most cost, do last.

**PHASE 1 — ✅ BUILT & WORKING** (built 2026-07-08; marker ticked 2026-07-27 — confirmed working by Jahni 2026-07-26, see `Docs/archive/AUDIT-2026-07.md §0`). Heightfield ops shipped: **Cliff** (slope-gated STEEPENING —
push height from the local mean where steep ⇒ sheer walls; the slope-conditioned one, hugs steep terrain;
v1 band-snap was too subtle, reformulated to steepening after Jahni's "doesn't change much"), **Terrace** (relief-gated plateau quantize, now with
`TerraceHardness` soft-round↔crisp-mesa), **LayerLines** (sedimentary sine shelves, slope-expressed). *Design
deviation from the spec above, deliberate:* instead of a `SurfaceOps` array of `FStrateTerrainOpEntry` +
`FTerrainCondition` gating on `UVoxelBiomeDefinition`, phase-1 ops are **direct fields on
`FSurfaceGenerationParams`** (the `Surface|Ops` category). Rationale: that struct is ALREADY the per-biome
surface-shape carrier (`B->SurfaceParams` when `bOverrideTerrain`) and is already biome-resolved +
border-blended by `ResolveSurfaceChunkParams`/`ComputeSurfaceColumn` (the height output-lerp) — so biome
selection AND seamless border blending come for FREE with zero new resolution path, and the conditioning
(relief `M`, analytic slope) is intrinsic to each op. Cost: a biome must set `bOverrideTerrain` to carry its own
ops (fine — biome-differentiated terrain already implies that), and a strate can also carry ops with no biomes at
all (more flexible than biome-only). All fields default OFF ⇒ current world byte-identical. Applied in the single
height oracle `ComputeSurfaceTerrainZ` (new `SampleSurfaceStructuralZ` helper = pre-op raw height, re-sampled at
an XY offset for Cliff's slope) so MC/ClassifyTile/deco/BP-bridge all agree, no T1.d interference. Revisit
the array+condition model for **phase 2 (overhangs)** where per-entry slope-gating earns its keep.

**PHASE 2 — ✅ BUILT & WORKING** (built 2026-07-08; marker ticked 2026-07-27 — confirmed working by Jahni 2026-07-26, see `Docs/archive/AUDIT-2026-07.md §0`). Overhang (first VOLUMETRIC op) as
`FSurfaceGenerationParams` fields (`OverhangStrength/Reach/Height/Frequency/ZScale/SlopeThreshold`, default
off). **Design NOTE — v1 additive-noise-band was WRONG (Jahni: "does nothing" + sketch of a real cliff lip):
band-additive noise can only bump the surface where it already is, never make rock jut OUT over a void.**
Rewritten to a **warped-terrain UNION**: for air voxels in `(TerrainZ, TerrainZ+OverhangHeight]` above a
steep slope, re-sample the heightfield UPHILL by a reach that GROWS with height (tiny low ⇒ air over the
void; full high ⇒ borrows the far cliff rock) and `max()` it in ⇒ a shelf attached to the cliff, tapering
out over the void with air beneath (matches the sketch). Per-column `OverhangAmp`(=strength·slope-gate) +
unit uphill `(DirX,DirY)` resolved in `ComputeSurfaceColumn` — gradient sampled at the REACH scale so a
point over the void can SEE the cliff — cached on `FSurfaceColumn`. **ClassifyTile guard:**
`FSurfSlot::OverhangMargin`=max `OverhangHeight`; a column Z in `(TerrainZ, TerrainZ+margin]` ⇒ Mixed
(UPWARD only — the union only adds rock). Thin cliff-edge band woken, NOT far tiles. Genuine 3D per-voxel
structural re-eval (gated hard to steep overhang columns → localized to cliff edges; flagged as a real but
bounded cost). KNOWN v1 LIMITS (flagged): applies at ALL LODs (may alias far — gate to fine later); shelf
sits at ~`OverhangHeight` above the ground below it, not necessarily at the cliff TOP (raise Height for
taller); terrain-overriding biomes use strate params for the warped sample (minor seam). NEXT = **phase 3
spike/hole** (hash-placed SDF + per-tile shortlist + the real T1.d wake-guard — keep SHORT+SPARSE).

### C. Experience polish (cheap, high feel-per-effort)

- **F10 — Swimmable water:** physics volume + underwater post-process tied to the existing water-chunk regions (the plane is visual-only today). Buoyancy later.
- **F11 — Depth & place HUD:** depth meter, strate name title cards (the atmosphere manager already detects strate change), simple explored-chunks map.
- **F12 — Day/night + weather on strate 0 only** — surface gets a sky lifecycle; underground untouched (free scoping).
- **F13 — Carve UX:** runtime brush ghost preview, tool tiers (radius/speed), material-aware dig speed (bedrock slow), rockfall-dust juice on carve.
- **F14 — The (0,0) spine as gameplay:** buildable lift/teleport anchors per strate — descent is the game, but re-ascent shouldn't be the chore. BP prototype on the existing carve/actor APIs.
- **F15 — Ambient life:** Niagara bats/fireflies/fish schools per biome profile (no AI, pure atmosphere). Real mobs = **F19** (nav strategy now decided: function-based).
- **F16 — Decorations as HISM: ✅ DONE** (region-granular HISMs, two-grid Near/Far streaming, Static mobility). Original note: scatter currently `SpawnActor`s every prop — actors tick, register, and pile up fast. Pure props should be `UHierarchicalInstancedStaticMeshComponent` instances (per mesh type, per chunk); keep actors only for lit/interactable things. This is also a perf item wearing a feature hat.

### D. Deliberately deferred

Multiplayer — **NO LONGER just "deferred": it's the confirmed direction (listen-server first), design + first foundation IN.** Full model in ARCHITECTURE §9 ([[voxelforge-multiplayer]]): determinism = replicate seed+layout+diff events, never geometry; server-authoritative diff; multi-anchor streaming + §9.4 render-skip BUILT 2026-07-07. Remaining netcode (§9.7): seed replication at join, `Server_RequestModification` RPC, late-join diff snapshot. Diff records stay compact/replicatable-shaped (they already are). — GPU generation, mod/scripting API, Nanite: all real, none load-bearing for the current vision.

---

## Suggested order (if it were mine to pick)

1. ✅ **Perf 0 + T1.c + T1.a + T1.b** — one focused session: measurement, the one-line collision win, the two big density cuts.
2. ✅ **T1.e + T1.f + F16** — smooth the game thread (apply budget, worker-side streams, HISM props).
3. ✅ **F1 preview tool + F2 validator** — before biome work starts, build the instruments.
4. ✅ **F5 biomes + F6 materials** — the look of the game. (F9 audio: not built.)
5. **F4 save/load** — the moment it feels like a game, players will want to keep one. *(open)*
6. **F7 POIs + F8 ores** — destinations and rewards. *(F7 placement built, stamps open; F8 open)*
7. ✅ **T2.a SIMD noise** — after content direction settles (it changes seeds), before world-size ambitions grow.
8. **Transvoxel** (already chosen) whenever LOD cracks become the loudest remaining flaw. *(open — skirts fill LOD cracks today)*
