# VoxelForge — Code Map & Knowledge Index

> Purpose: a navigation index so anyone (human or AI) can locate and modify code
> without re-reading the whole plugin. Anchors are `File:line` — line numbers drift
> as code is edited, so trust the **symbol name** first and the line as a hint.
>
> Plugin root: `Source/VoxelForge/` · ~8,300 lines of C++ across 25 files.
> Comments in the code are mixed **French + English**. UE module = `VoxelForge` (Runtime).

---

## 1. What this plugin is

A **density-field voxel terrain** plugin for Unreal Engine, built around underground
**"strates"** (vertical geological layers, each a self-contained mini-world).

- **No block grid.** Terrain is a continuous scalar density field evaluated on the fly
  from world coordinates. Convention: **negative = solid rock, positive = air**
  (the Marching Cubes convention used throughout).
- **Marching Cubes** turns the density field into a smooth mesh per 32³ chunk.
- **Strates** stack downward from Z=0. Each strate is a `UVoxelStrateDefinition` data
  asset that picks a generator type and a huge bag of cave-shaping params.
- **Async streaming**: chunks load/unload around the player on background tasks; meshes
  are applied on the game thread under a per-frame budget.
- **Player edits** (carve/fill) are stored as a *diff layer* added on top of the
  procedural density — procedural generation stays deterministic.

### Core terminology
| Term | Meaning |
|------|---------|
| Chunk | 32×32×32 voxels (`CHUNK_SIZE`). Voxel = 25 cm (`VOXEL_SIZE`). |
| Density | Scalar field. `< 0` solid, `> 0` air, `0` = surface (`IsoLevel`). |
| Strate | Vertical layer of the world, stacked downward. Has its own generator + params. |
| Generator type | Archetype per strate: `TunnelNetwork`, `FlatPlain`, `CrystalChamber`, `Maze`, `SurfaceWorld`, `VerticalShafts`, `FloatingIslands`, `Underwater`. See §8. |
| (0,0) spine | Guaranteed open landing column at world XY (0,0) in every strate; descent is player-dug through the seals. See §8. |
| Terrain op | Optional density modifier (pit, arch, terrace…) attached to a strate. |
| Passage | Carved tunnel connecting two adjacent strates (progression path). |
| Diff layer | Player carve/fill modifications stored on top of procedural density. |
| Epoch | Generation counter; stale async results are discarded on mismatch. |

---

## 2. The big picture — data flow

```
                         AVoxelWorld (actor, orchestrator)
                         Tick → UpdateChunksAroundPosition
                                   │  (load/unload around player, by distance + LOD)
                                   ▼
                         LoadChunk → UE::Tasks::Launch  ──────────► background thread
                                                                         │
   UVoxelMarchingCubesMesher::GenerateMesh(chunk, step) ◄────────────────┘
        │ samples density per cell corner
        ▼
   UVoxelGenerator::GetDensityAt(x,y,z)        ← THE density entry point
        │ asks StrateManager which strate/params/generator-type applies
        ├─ TunnelNetwork → GetDensityWithParams()  (rooms+tunnels+ops+worms+seal+passages)
        │       └─ VoxelCaveMorphology::BuildChunkCache + EvaluateSDFCached  (room/tunnel SDF)
        ├─ FlatPlain / CrystalChamber → GetSlabDensity()  (floor+ceiling+columns+seal+passages)
        └─ + UVoxelDiffLayer::GetDensityOffset()   (player carve/fill)
        │
        ▼ FVoxelMeshData (verts/tris/uvs/normals)
   ProcessQueue (lock-free) ──► game thread: ProcessPendingChunks → ApplyMeshToChunk
                                                                   (RealtimeMeshComponent)
```

`UVoxelStrateManager` is the side oracle: "what strate is at this Z, what params,
what generator type, and is there a passage/elevator SDF near here?"

---

## 3. File-by-file reference

Paths relative to `Source/VoxelForge/`. `Public/` = headers, `Private/` = impl.

### 3.1 Module & build
| File | Role |
|------|------|
| `../../VoxelForge.uplugin` | Plugin manifest. One Runtime module `VoxelForge`. Beta. |
| `VoxelForge.Build.cs` | Deps: Core, CoreUObject, Engine, **GameplayTags**, **RealtimeMeshComponent**. |
| `Public/VoxelForgeModule.h` / `Private/VoxelForgeModule.cpp` | `FVoxelForgeModule` boilerplate (Startup/Shutdown just log). |

### 3.2 Foundational types — `Public/VoxelTypes.h` (no UClass, everyone includes it)
| Symbol | Line | Notes |
|--------|------|-------|
| `CHUNK_SIZE` (32), `CHUNK_SIZE_SQUARED`, `CHUNK_VOLUME` | 19-21 | Chunk dimensions. (64³ tried for fewer draws → reverted: streaming too bursty. fps is fixed render-side instead.) |
| `VOXEL_SIZE` (25.0f cm) | 23 | World scale. |
| `EVoxelFace` enum + `GetFaceDirection` / `GetFaceNormal` | 33-61 | 6 cube faces. |
| `WorldToChunkCoord` / `WorldToLocalCoord` / `ChunkToWorldPos` | 74-104 | Coord-space conversions (handle negatives via floor/positive-modulo). |
| `LocalToIndex` / `IndexToLocal` / `IsValidLocalCoord` | 107-131 | Flat-array 3D↔1D indexing. |
| `SmoothStep01` | 140 | 3x²-2x³ — used everywhere for blends. |
| `VOXEL_NOISE_SCALE` (1.25f) | 147 | Rescales UE PerlinNoise3D to ~[-1,1]. |
| `EVoxelTileClass` enum (`Mixed`/`AllSolid`/`AllAir`) | — | T1.d verdict. **MOVED here from `VoxelGenerator.h` 2026-07-27** so `VoxelDensityOp.h` can share it without a UCLASS dependency. A false `AllSolid`/`AllAir` is a HOLE; a false `Mixed` only costs CPU. |
| `FVoxelMeshData` struct | 157-173 | Mesher output (Vertices/Triangles/UVs/Normals/**Colors**). Plain C++, not USTRUCT. `Colors` = F6 material masks (R=dominant biome palette, G=slope, B=border blend weight, A=neighbour biome palette). §8.15. |

### 3.2b Density operator stack contract — `Public/VoxelDensityOp.h` (plain C++, no UHT)
**Phase 1 skeleton, added 2026-07-27. Nothing is wired in yet** — `GetDensityAt`'s archetype
`switch` is untouched and no operator exists. See [OPSTACK-PLAN.md](OPSTACK-PLAN.md) for the plan and
[OPSTACK-DECOMPOSITION.md](OPSTACK-DECOMPOSITION.md) for the per-archetype breakdown.

| Symbol | Notes |
|--------|-------|
| `EVoxelOpRole` | The four roles: `FieldSource` (what the field IS) · `Combiner` (how fields merge) · `DetailModifier` (today's `UVoxelTerrainOpDefinition`) · `StructuralPost` (spine→seal→passage→diff, appended automatically, never author-omittable). |
| `EVoxelOpCombine` | `Replace`/`Union`(min)/`Subtract`(max)/`SmoothUnion`/`SmoothSubtract`/`Add`/`Mask`. Sign reminder: negative = solid, so "add solid" is `min`. |
| `EVoxelOpEffect` | `Identity`/`CarveOnly`/`FillOnly`/`Both`. Conservative: `Both` is always safe, the wrong one is a hole. |
| `FVoxelOpContext` | Chunk-constant inputs. **Carries `LayoutVersion` by construction** so a new op cannot forget it (AUDIT C2). |
| `IVoxelDensityOp` | `PrepareChunk` / `Eval` / `EffectOverBox` / `ClassifyBox` / `IsXYPure`. |
| `IVoxelDensityOp::ClassifyBox` | ⚠️ **not source-only.** Forcing ops (the boundary seal inside its band) overwrite the input entirely, which pure direction cannot express. |
| `FVoxelBoxHypotheses` + `VF_ForceHypotheses` / `VF_FoldEffect` / `VF_FoldOp` | The fold that turns a stack into an `EVoxelTileClass`. Reproduces today's hand-written `ClassifyTile` line for line — the mapping is written out in the header. |

### 3.3 Chunk identity
`VoxelChunk.h` (the old `FVoxelChunk` coord wrapper) was DELETED — dead since the tile
redesign; tile identity lives in `FVoxelTileKey` (VoxelWorld.h).

### 3.4 Settings — `Public/VoxelSettings.h`
`UVoxelSettings : UPrimaryDataAsset` — the single tuning asset assigned on `AVoxelWorld`.
| Group | Fields (line) |
|-------|---------------|
| Streaming | `ViewDistanceXY=16`, `ViewDistanceUp/Down=5`, `MaxConcurrentTasks=16`, `MaxMeshAppliesPerFrame=4` (defaults — actual values live on the data asset) |
| Clipmap | `ClipRadius`, `MaxClipLevel`, `FullResClipLevels`, `CoarseTileCells`, `RenderDistanceChunks` (custom horizontal reach: the outermost shell keeps generating until it covers this many chunks; 0 = off), `bFarSheetRing` + `FarSheetSpanLevels` (F18 — the render-distance ring streams per-surface SHEETS: level MaxClipLevel+span heightfield tiles instead of MC, see `GenerateSheetMesh`), skirts, `LODOctaveDrop` (T2.b octave drop on coarse tiles — 0 = off/byte-identical) (the old `LOD0/1Distance` + `ContentMaxLevel` were dead → removed) |
| Lighting | `bEnableDensityVolume` + DensityVolume* tunables (§3.11 density clipmap / mini-sun shadows) |
| Rendering | `VoxelMaterial` (61) |
| Strates | `Seed` (69), `CurrentSeason=1` (73), `StratePool` (78), `FixedStrates` map (83), `TotalStrates=10` (87) |
| Carving budget | `MaxModifications=0` (97), `MaxBrushRadius=15` (102), `MaxTotalVolume=0` (107). 0 = unlimited. |

### 3.5 World orchestrator — `Public/VoxelWorld.h` + `Private/VoxelWorld.cpp`
`AVoxelWorld : AActor` — owns everything, drives streaming. Also `FChunkResult` struct
(VoxelWorld.h:35) = async task payload (coord, chunk, meshdata, LOD, **Epoch**).

**Owned objects (UPROPERTY):** `Settings`, `Generator`, `Mesher`, `StrateManager`,
`DiffLayer` (VoxelWorld.h:55-78). **Storage:** `Chunks` map, `ChunkMeshes` map,
`ChunkLODs`, `ProcessQueue` (TQueue), `PendingChunkCoord` (TSet) (VoxelWorld.h:85-312).
**Async state:** `bShuttingDown`, `ActiveTaskCount` (atomics), `GenerationEpoch`.

| Method | .cpp line | Role |
|--------|-----------|------|
| `AVoxelWorld()` ctor | 12 | Enables Tick. |
| `RegenerateAllChunks()` | 21 | Bumps epoch, unloads all → Tick reloads. CallInEditor button. |
| `ValidateDeterminism()` | — | **F2 CallInEditor button (PIE)**: re-samples boundary points under left- vs right-chunk cache warm-ups + a same-alignment repeat; any non-zero delta = window-invariance regression (§8.4). Run after every "bit-identical" hot-path refactor. |
| `GetMaxConcurrentTasks()` | — | T2.d — asset `MaxConcurrentTasks` capped to logical cores − 2 (all three budget checks use it). |
| `PostEditChangeProperty` | 45 | Editor live-edit hook. |
| `OnObjectModifiedInEditor` | 58 | Regenerates when a strate asset is edited (if `bLiveEditStrates`). |
| `EndPlay` | 140 | Sets `bShuttingDown`, **waits for `ActiveTaskCount`→0**, unbinds delegate. |
| `BeginPlay` | 177 | Constructs Generator/Mesher/StrateManager/DiffLayer, wires services, seeds. |
| `Tick` | 220 | `UpdateChunksAroundPosition(player)` + `ProcessPendingChunks()`. |
| `GetPlayerPosition` | 231 | Pawn position or zero. (`GetLODForChunk`/`LODToStep`/`IsChunkInRange` removed — dead since the clipmap.) |
| `ProcessPendingChunks` | 301 | Drains ProcessQueue under per-frame budget; applies each via `ApplyTileResult`. |
| `ApplyTileResult` | — | **Shared game-thread apply** for one `FChunkResult` (async drain + sync carve): discards stale epochs, marks loaded, ingests capture, EMPTY releases the tile's existing component (a re-gen can flip content→empty on band change — old geometry must not linger), else `ApplyMeshToTile`. Returns true iff a visible mesh uploaded (counts the budget). Doesn't touch `PendingTiles` (caller's). |
| `GenerateTileResult` | — | **Shared worker-side gen** for one tile (async `LoadTile` task + sync `SyncRemeshTile`): ClassifyTile (T1.d) → `GenerateMesh`/`GenerateSheetMesh` → `BuildTileStreamSet`. Reads Generator/Mesher only → safe on a worker or the game thread; fills `FChunkResult`, no enqueue. |
| `SyncRemeshTile` | — | **INSTANT DIG**: level-0 same-frame re-mesh on the game thread (`GenerateTileResult` + `ApplyTileResult` inline, Cells=CHUNK_SIZE/Step=1 + strate band, no capture). Used for the tile under the brush centre so a carve is visible THIS frame; one full-res gen on the game thread. |
| `UpdateChunksAroundPosition` | 362 | Builds desired set, sorts by distance, loads/unloads, handles LOD changes. **Delta cull**: `BuildDesiredTiles` returns the LEAVERS (stamped `DesiredStamped` map, one sweep) — only those + `TransitionHold` are considered per crossing, not every loaded tile. `BuildDesiredTiles` also applies `RenderDistanceChunks`: the outermost shell widens to cover the distance — as level-MaxClipLevel MC tiles, or (F18 `bFarSheetRing`) as a SHEET ring at level MaxClipLevel+span (covered-check vs the MaxClipLevel box; `VF_OuterShell` shared with `IsTileInClipRange` so the cull sees the same horizon), dz pre-clamped to the vertical band. **§9.3 anchors:** also prunes dead `StreamingAnchors` + detects their chunk crossings → rebuilds the desired set when an anchor moves/(un)registers (`bAnchorsMoved`/`bForceDesiredRebuild`), same cadence as player movement. §8.10. |
| `BuildDesiredTiles` | ~722 | Builds `DesiredSorted`+`DesiredStamped` (player clipmap shells + F18 sheet ring) then `AddAnchorDesiredTiles()` before the leaver sweep. |
| `AddAnchorDesiredTiles` | — | **§9.3 multi-anchor:** folds each `FVoxelStreamingAnchor`'s thin level-0 box (`XYRadiusChunks`/`ZBelowChunks`/`ZAboveChunks`) into the SAME desired set (deduped by stamp) so AI/remote players keep collision loaded around them; delta cull releases them on move/unregister. **§9.4:** a tile only a CollisionOnly anchor wants (clipmap didn't stamp it) → `CollisionOnlyTiles` → hidden at apply. Zero cost when no anchors. |
| `ReconcileAnchorTileVisibility` | — | **§9.4:** after each rebuild, toggle `SetVisibility` on ALREADY-LOADED tiles that flipped render↔collision-only (diff `CollisionOnlyTiles` vs prev — bounded, no O(loaded) scan; unhide only if still desired). No-op without CollisionOnly anchors. |
| `RegisterStreamingAnchor` / `UnregisterStreamingAnchor` | — | **BlueprintCallable §9.3:** add/remove an actor as a streaming anchor (`EVoxelAnchorPolicy` CollisionOnly/FullVisual + XY/ZBelow/ZAbove box). Idempotent; forces a rebuild next Tick. §9.4: CollisionOnly tiles cook collision but are hidden (no draw/VSM) unless the player clipmap wants them too. |
| `LoadChunk` | 445 | Budget check → `UE::Tasks::Launch` background gen+mesh; RAII task guard. Worker runs `Generator->ClassifyTile` first (T1.d): AllSolid/AllAir ⇒ skip `GenerateMesh`, tile stays empty (capture tiles always generate). STRATE CONTENT CUT: tiles ≥ `StrateContentCutMinLevel` pass the player-strate band (`MeshBandChunkLo/Hi` → voxels) to `GenerateMesh` + stamp it on `FChunkResult::BandChunkLo/Hi`; band change re-queues via `BandRemeshQueue` (see `UpdateChunksAroundPosition`). TOO-COARSE SKIP: if one cell is taller than the band (`Step > band height` — level ≥7 territory) the tile is enqueued EMPTY without launching a task (cell-granular cut could only render garbage). F18: `Tile.Level > MaxClipLevel` = SHEET tile → routed to `GenerateSheetMesh` (band mid-chunk = strate ref; band unarmed ⇒ empty; MC-ring sampling density, cells capped 128/axis; carries the XY hole `SheetHole*Vox` — hole moves ⇒ overlapping sheets re-queue via `BandRemeshQueue`, see `UpdateChunksAroundPosition`). §8.10. |
| `UnloadTile` | — | Clears tile state; the component is PARKED in the pool (T2.c), not destroyed. |
| `ApplyMeshToTile` | — | Upload geometry. One component per tile (clipmap keeps count low; supersedes the old region batching); worker-built streams (T1.f) → `CreateSectionGroup(MoveTemp)`. Reuses the component's existing `URealtimeMesh` (no per-apply mesh alloc). **F17: two polygroups** (0 ground / 1 sky-cap, per-triangle class from the mesher) → RMC auto-section per non-empty group; slot 0 = override/default material, slot 1 = `CeilingMaterial` (fallback ground); config gated by `FChunkResult::bHasGroundTris/bHasCeilingTris`. Takes `FChunkResult&`; strate lookups clamp Z into `Result.BandChunkLo/Hi` (strate content cut) — ground at clamped bottom chunk, cap at clamped top (mid = gap fallback). Collision level-0 only (T1.c) both groups; shadow per SECTION: ground casts at level≤1, cap never. **§9.4:** `SetVisibility(false)` when the tile is in `CollisionOnlyTiles` (anchor-only, hidden — collision still cooks). §8.10 + ARCHITECTURE SurfaceWorld row. |
| `AcquireTileComponent` / `ReleaseTileComponent` | — | **T2.c component pool** (`TileComponentPool`, bounded): park on unload (geometry+collision stripped, hidden, stays registered), pop on apply — no `NewObject`/`RegisterComponent`/GC churn during travel & regen. §8.10. |
| `GetStrateAtPosition` | 965 | Gameplay query → strate index. |
| `GetBiomeAtWorldLocation` | — | **BlueprintCallable** biome probe at a world point (undoes actor xf → voxel → `Generator::QueryBiomeAt`). Returns `FVoxelBiomeQuery` for BP debug ("what biome / how many decos under the cursor?"). |
| `GetVoxelSurfaceHeightAt` | ~1534 | **BlueprintCallable** ground finder (F7 bridge): world XY → terrain + sky-cap world-Z via `Generator::GetSurfaceHeightAt`, NO trace/collision, deterministic, available before the area meshes → self-arranging prefab/ruin BPs snap their parts to the real ground. False (outs=input Z) on non-SurfaceWorld strates; ignores passage/spine carving. |
| `CarveAtPosition` / `FillAtPosition` | 691 / 709 | Build `FVoxelModification` → `ApplyModification`. |
| `ApplyModification` | ~1581 | Single funnel for all brushes: DiffLayer → **sync-remesh the brush-centre level-0 tile** (`SyncRemeshTile`, instant hole; skipped if that tile is mid-gen — would race a stale in-flight result) → `RemeshDirtyChunks(..., excludeCenter)` for the neighbours → `RemoveDecorationsInSphere`. |
| `ClearAllModifications` | 726 | Clears diff layer, regenerates. |
| `ChangeSeed` | 740 | **Season reset**: new seed everywhere, clear diffs, bump season, reload. |
| `GetCurrentSeed` / `GetCurrentSeason` | 784 / 789 | Accessors. |
| `RemeshDirtyChunks` | 798 | Queue loaded level-0 dirty tiles onto `DirtyRemeshQueue` (async re-mesh, no pop) + `MarkDirtyVoxelBox` the volume. Optional `ExcludeTile` = the sync'd centre. Drained FIRST in the submit loop at **BackgroundHigh** (ahead of streaming/band) so a dig never waits behind streaming; in-flight tiles stay QUEUED (not dropped) so a stale pre-carve result is corrected once it lands — fixes "hole shows up a beat late / not until I move". |

> **Game-thread profiling (Perf):** `AVoxelWorld::Tick` and its sub-steps are wrapped in `TRACE_CPUPROFILER_EVENT_SCOPE` — `VoxelForge_Tick / UpdateChunks / BuildDesiredTiles / CullTiles / SubmitTiles / ProcessPending / ProcessUnload / UpdateDecorations / UpdateWater`. Capture a `Count/Incl/Excl` Insights timer export and read the `Excl` column to see which step owns the per-frame cost (the actor tick shows as `BP_VoxelWorld_C` if subclassed in BP). `VoxelForge_ClassifyTile` (T1.d) / `VoxelForge_GenerateMesh` + `VoxelForge_BuildStreams` are worker-side (off the frame): the RMC `FRealtimeMeshStreamSet` is now built on the gen worker (`BuildTileStreamSet`) and carried on `FChunkResult::Streams` (TSharedPtr), so `ApplyMeshToTile` is game-thread-cheap — just material/ceiling resolve + `CreateSectionGroup(MoveTemp)`. See ARCHITECTURE §8.10 "Worker-built StreamSet (T1.f)".

### 3.6 Density generator — `Public/VoxelGenerator.h` + `Private/VoxelGenerator.cpp`
`UVoxelGenerator : UObject` — lightweight; holds `Seed`, and injected services
`StrateManager` + `DiffLayer` (both nullable). This is **where terrain shape lives.**

| Symbol | .cpp line | Role |
|--------|-----------|------|
| `FractalNoise3D` (static) | 25 | fBM (layered Perlin). |
| `RidgedNoise3D` (static) | 55 | Ridged multifractal — craggy. |
| `CellularNoise3D` (static) | 101 | Worley/cellular — grotto/scallop. |
| `ApplyBoundarySeal` (static) | 170 | Solidifies strate top/bottom shells. |
| `ApplyPassageCarving` (static) | 197 | Punches passages/elevator through the seal. |
| `InitializeSettings` | 211 | Copies seed from settings. |
| **`GetDensityAt`** | 218 | **Entry point.** Picks strate + generator type, dispatches, adds diff offset. |
| **`GetDensityWithParams`** | 277 | TunnelNetwork pipeline (~1000 lines). See §4. |
| **`GetSlabDensity`** | 1306 | FlatPlain/CrystalChamber pipeline. See §4.2. |
| `SampleSurfaceStructuralZ` | — | **F20:** the RAW SurfaceWorld heightfield (continents+mountains+detail), BEFORE any terrain op; returns terrain Z + relief M. Cliff re-samples it at an XY offset for a cheap analytic slope. |
| `ComputeSurfaceTerrainZ` / `GetSurfaceDensity` | — | SurfaceWorld heightfield → terrain Z, then density; biome **output-blend** lerps dominant/neighbour heights (`ParamsD`/`ParamsN`/weight). **F20 surface ops** (`FSurfaceGenerationParams`, biome-selected + slope/relief-conditioned, all default off): Cliff (slope-gated STEEPENING — push height from local mean where steep ⇒ sheer walls; 4 structural resamples only when on), Terrace (relief-gated + `TerraceHardness`), LayerLines (sedimentary shelves) — pure per-column height REMAPS applied here so the single height oracle stays consistent (MC/sheets/ClassifyTile/deco/BP bridge). **Phase 2 OVERHANG** (volumetric — real jutting shelves): in `SurfaceDensityFromColumn`, for AIR voxels in a window `(TerrainZ, TerrainZ+OverhangHeight]` above a steep slope, the heightfield is re-sampled UPHILL (toward the cliff) by a reach that GROWS with height (tiny low ⇒ air over the void, full high ⇒ borrows the far cliff rock) and unioned in ⇒ a shelf attached to the cliff, tapering out over the void with air beneath (the sketch). Per-column `OverhangAmp`(=strength·slope-gate) + unit uphill `(DirX,DirY)` resolved once in `ComputeSurfaceColumn` (gradient sampled at the REACH scale so a spot over the void can see the cliff), cached on `FSurfaceColumn`. Genuine 3D (per-voxel structural re-eval, gated to steep overhang columns). Off ⇒ byte-identical. §8.14. |
| `ClassifyTile` | — | **T1.d trivial-tile reject** (worker, called by `LoadTile` before `GenerateMesh`): proves a tile AllSolid/AllAir on the mesher's exact lattice (gap chunks + SurfaceWorld columns via the SHARED `GSurfColCache`; seal bands; guards: diff mods, passages, spine, disturbances, **F20 overhang** — a column point in `(TerrainZ, TerrainZ+OverhangMargin]` (margin = max `OverhangHeight`) is unprovable ⇒ Mixed, UPWARD only since the shelf union only ADDS rock above ground, so an overhang shelf never holes a trivially-skipped tile) → skip gen. Mixed = generate normally. §8.10. |
| `SampleRelief` / `SampleMoisture` | — | Climate fields (pure XY, [0,1]). Relief = shared source of truth for the relief map M. §8.14. |
| `SampleBiomeAt` | — | Warped-Voronoi + climate biome query (dominant + neighbour + weight). Reference used by the preview bake + `GetDominantBiomeAt`. §8.14. |
| `ResolveBiomeSampleAt` / `RebuildBiomeGrid` | — | Hot-path biome resolve (FBiomeSample) via a box-validated per-chunk cell-grid cache. Bit-identical to `SampleBiomeAt`. §8.14, §8.10. |
| `GetDominantBiomeAt` | — | Game-thread query → dominant biome ASSET (content/atmosphere). §8.14. |
| `QueryBiomeAt` | — | Rich game-thread biome probe → `FVoxelBiomeQuery` (dominant/neighbour asset, relief/moisture, blend weight, dominant deco count). Diagnostic behind `AVoxelWorld::GetBiomeAtWorldLocation`. §8.14. |
| `EvaluateTerrainConditions` | ~2548 | **F7 aware placement:** AND-evaluate an entry's `FTerrainCondition[]` (relief/moisture/biome-border) at a candidate voxel XY. Empty = true (zero cost). Pure query (SampleRelief/SampleMoisture/SampleBiomeAt) → worker-safe + game-thread; caller passes the strate's `FBiomeContext` (freq/contrast + Voronoi map). Consumed by deco `BuildCellSpawns` + `SpawnLandmarkInstance`; shared core of the future quest FindFeature locator. |

> **Per-voxel hot-path memos (perf pass 2, all bit-identical — same hashes/math, hoisted per
> chunk/cell):** `GetDensityAt` uses the DiffLayer snapshot cache (§3.9); `GetDensityWithParams`
> memoizes the strate index per (chunkZ, `GetLayoutVersion()`); `thread_local` per-cell lattice
> bakes cover slab columns (`GetSlabDensity` step 4), maze open edges, vertical shafts +
> cross-connectors, floating-island constants, and the disturbance chasms/bridges/ridges; worm
> tunnels short-circuit the 2nd Perlin when N1 ≥ WormThreshold (N2 ≥ 0 ⇒ can't carve). Room
> shapes are pre-baked in `FCachedRoom` (§3.7). **T2.b:** per-voxel fractal call sites take their
> octave count through `VoxelGenLOD::Eff(N)` (VoxelGenerator.h) — a `thread_local` bias set per
> tile by the mesher drops tail octaves on coarse tiles (opt-in `LODOctaveDrop`, default 0 = off);
> XY-field noise (heightfield/ceiling/relief/moisture) deliberately stays un-biased. §8.10.

### 3.7 Cave morphology (SDF rooms/tunnels) — `Public/VoxelCaveMorphology.h` + `.cpp`
Header is rich with inline docs. Two namespaces + a per-chunk cache system.

- `namespace VoxelSDF` (h:46): `Sphere`, `Ellipsoid`, `Capsule`, `RoundedBox`,
  `TaperedCapsule`, `SmoothMin`, `SmoothMax` — all FORCEINLINE SDF primitives.
- `namespace VoxelHash` (h:158): `Mix`, `Cell`, `Pair`, `ToFloat01`, `ToFloatSigned`
  — deterministic hashing for room/tunnel placement (no storage, infinite worlds).
- Cache structs (h:224-330): `FCachedRoom`, `FCachedTunnel`, `FCachedPit`,
  `FCachedChimney`, `FCachedColumn`, `FChunkSDFCache`.
- `namespace VoxelCaveMorphology`:
  | Function | .cpp line | Role |
  |----------|-----------|------|
  | `BuildChunkCache` | 47 | **Phase 1** (once/chunk): collect rooms, guaranteed backbone (`bTunnelsFlowTowardOrigin`: tree rooted at the (0,0) hub — every room reachable, links flow inward; false = legacy NN forest), slope-aware link metric (`TunnelHorizontalBias` now applies to backbone too), decide tunnels, **cull zero-connection rooms** (no sealed bubbles), store rooms by their OWN reach (fixes origin-room clipping at `MaxInfluence`), pre-bake pits/chimneys/columns via the shared `BakeRoomFeature` hash-placement skeleton (one gate/XY/radius pattern + per-type Emit lambda), hash-roll per-room terrain op. |
  | `EvaluateSDFCached` | 757 | **Phase 2** (per voxel): SmoothMin over cached rooms/tunnels; returns nearest room idx for terrain-op lookup. **Signature changed (perf pass 2): `RoomShapeVariety` param REMOVED** — the shape roll + capsule trig are pre-baked into `FCachedRoom` (`ShapeType/ShapeA/ShapeB/ShapeR`) by `BuildChunkCache`, bit-identical. |
  | `EvaluateSDF` | 738 | Convenience wrapper (builds temp cache) for one-off queries. |

  Performance note (h:209-220): caching rooms/tunnels once per chunk instead of per
  voxel is the single biggest CPU win.

### 3.8 Strate system
**`Public/VoxelStrateTypes.h`** — shared structs/enums (1228 lines, the data vocabulary):
| Symbol | Line | Role |
|--------|------|------|
| `EVoxelPassageType` | 38 | Sloped/Vertical/Spiral/Cascading/Crack passage shapes. |
| `ESurfaceType` | 78 | Floor/Wall/Ceiling/Any (decoration placement). |
| `EVoxelNoiseType` | 99 | FBM/Ridged/Mixed/Cellular. |
| `ECaveGeneratorType` | 146 | TunnelNetwork / FlatPlain / CrystalChamber. |
| `EVoxelStrateTransition` | 183 | Gradient / Hard / Interleaved boundary blends. |
| **`FStrateGenerationParams`** | ~350 | The giant TunnelNetwork param bag (rock, worms, rooms, tunnels, warp, roughness, all terrain-op transport fields, boundary seal). `Lerp()` static blends two sets at boundaries — it expands the **`VF_STRATE_PARAM_FIELDS` X-macro** (defined just above the struct): **adding a field to the struct? add it to that list** or blends silently reset it to default. |
| `FStrateTerrainOpEntry` | 965 | Soft-ptr to a terrain op + Weight + Probability. |
| **`FSlabGenerationParams`** | 1019 | Floor/ceiling heights, roughness, columns, seal — for slab generators. |
| **`FPlacementProfile`** | ~1747 | **Shared placement vocabulary** for every scatter primitive (`FStrateDecoration`, `FStrateLandmark`, coming `FStrateSetPiece`): spawn (ActorClass/InstancedMesh), Filter gates (surface/slope/overhang/water/RequiredBiome + **F7 awareness `Conditions[]`** — `FTerrainCondition` relief/moisture/biome-border predicates, AND-ed, evaluated by `Generator::EvaluateTerrainConditions`), Transform (align/offsets/RotationOffset+RandomRotation/scale), Render (cull/shadow). Each primitive embeds it as `Profile` + keeps only its own DISTRIBUTION fields. Per-primitive defaults set in each struct's ctor (deco: scale 0.8-1.2 + RandomRotation.Yaw=360; landmark: Ceiling + no align). |
| `FStrateDecoration` / `FStrateLandmark` | ~1830 / ~1900 | `Profile` + distribution: deco = StreamTier/SpawnDensity/MaxPerChunk; landmark = SpacingChunks/JitterFraction/SpawnProbability/StreamRadiusChunks + Light-Orb block. |
| `ELandmarkAnchor` (on `FStrateLandmark`) | ~1940 | F7: `AnchorMode` = HashLattice / PassageMouth + passage toggles + exclusion (`ExclusionRadiusChunks`/`Priority`) folded into `FStrateLandmark` (set-pieces merged in — one primitive, one `Landmarks` list, `UpdateLandmarks`). |
| `FStrateAmbientActor` / `FStrateCreature` | ~2090 / ~2110 | Content spawn entries (consumed by future systems). |

**`Public/VoxelStrateDefinition.h`** — `UVoxelStrateDefinition : UPrimaryDataAsset`
(line 36). One asset = one strate *type*. Fields: identity, `StrateHeightInChunks`(60),
`TransitionType`(79)/`TransitionBlendChunks`(89), `GeneratorType`(102),
`GenerationParams`(113), `SlabParams`(124), `Biomes[]`+`BiomeMapParams` (the biome list +
field tuning — empty ⇒ unchanged world, §8.14), `TerrainOperations`(147), visuals/fog/light,
content lists, audio, `GameplayTags`(223). EditConditions show/hide param groups by generator type.

**`Public/VoxelStrateManager.h` + `.cpp`** — `UVoxelStrateManager : UObject` (h:108).
Maps depth→strate at runtime; owns passages.
- `FVoxelPassage` (h:39): endpoints, radius, type, control points.
- `FStrateSlot` (h:84): definition + chunk-Z range + index.
| Method | .cpp line | Role |
|--------|-----------|------|
| `Initialize` | 10 | Builds the stacked layout from settings+seed (fixed slots + shuffled pool), then `GeneratePassages`. |
| `GeneratePassages` | 146 | Deterministic passages between consecutive strates (per-type control points). |
| `EvaluateModifierSDF` | 357 | SDF of passages at a point (for carving). Per-chunk `thread_local` shortlist (`PassagesVersion`-stamped) → far chunks return `FLT_MAX` without walking `Passages`. §8.10. |
| `AnyPassageNearBox` | — | Conservative sphere-vs-AABB test of every passage's bound against a voxel box (+carve blend pad). Per TILE (ClassifyTile guard), never per voxel. |
| `FindSlotIndexForChunkZ` | 427 | Z → layout index. |
| `GetStrateAt` / `GetStrateIndex` | 443 / 455 | World-Z queries. |
| `GetLayoutVersion` | h:161 (inline) | Layout/passage generation counter (= `PassagesVersion`, bumped by every `Initialize`). Hot-path callers key `thread_local` memos on it (strate-index memo in `GetDensityWithParams`, passage shortlist) so editor rebuilds never serve stale data. |
| `GetStrateForChunk` | 466 | Chunk → definition. |
| `GetGeneratorTypeForChunk` | 476 | Chunk → generator type. |
| `GetSlabParamsForChunk` | 490 | Slab params with runtime Z bounds (no blend — slabs use Hard). |
| `GetBiomeContextForChunk` | — | Flatten the strate's `Biomes[]` + `BiomeMapParams` into a POD `FBiomeContext` for the biome field. Empty ⇒ biomes disabled. §8.14. |
| `GetGenerationParams` | 515 | **Blended** TunnelNetwork params (handles Gradient/Hard/Interleaved transitions). |
| `BuildParamsFromDefinition` (static) | 771 | Base params + merge all referenced terrain op assets. The one place ops fold into params. |

**`Public/VoxelTerrainOpDefinition.h` + `.cpp`** — `UVoxelTerrainOpDefinition : UPrimaryDataAsset`
(h:67). One asset = one terrain op. `EVoxelTerrainOpType` (h:36): Terrace, LayerLines,
Ribbing, Cliff, Scallop, Overhang, Arch, Column, Pit, Chimney, Dome, Pinch. Per-type
param groups gated by EditCondition. `ApplyTo(OutParams, Weight)` (.cpp:6) copies only
the active type's fields into `FStrateGenerationParams`, scaled by Weight.

**`Public/VoxelBiomeTypes.h`** (NEW) — biome vocabulary. `FBiomeMapParams` (Voronoi cell size /
border warp+blend / climate field freqs), `EBiomePreviewChannel` (preview-bake selector),
`FVoxelBiomeQuery` (BlueprintType result of `GetBiomeAtWorldLocation` — dominant/neighbour asset,
climate, blend weight, deco count), and plain runtime PODs `FBiomeResolved` / `FBiomeContext` /
`FBiomeSample` / `FChunkBiomeCache` (the box-validated per-chunk grid cache). See §8.14.
`FChunkBiomeCache::Invalidate()` (added 2026-07-27, AUDIT C2) — force a rebuild when the strate
layout version moves. The validity BOX says nothing about the `FBiomeContext` the cells were
classified against, so after a `RebuildStrates` the grid is stale even though the box still covers
the query. Called by all four callers on a `GetLayoutVersion()` change.

**`Public/VoxelBiomeDefinition.h` + `.cpp`** (NEW) — `UVoxelBiomeDefinition : UPrimaryDataAsset`.
One asset = one biome: identity + `DebugColor`, climate placement box (`ReliefMin/Max`,
`MoistureMin/Max`), terrain override (`bOverrideTerrain` + `GeneratorType` + archetype params), content profile (`Decorations`/`AmbientActors`,
atmosphere override, `WaterMaterial`, `MaterialPaletteIndex` (F6 — baked to vertex colour, §8.15)), `GameplayTags`. Referenced from
`UVoxelStrateDefinition::Biomes[]`. Generator-agnostic (surface biomes now, cave biomes later). §8.14.

### 3.9 Player edits — `Public/VoxelDiffLayer.h` + `.cpp`
`UVoxelDiffLayer : UObject` (h:77). Stores `FVoxelModification` (h:43: Center/Radius/Strength;
**negative Strength = carve, positive = fill**) grouped by chunk in `TMap ChunkMods`.
| Method | .cpp line | Role |
|--------|-----------|------|
| `SetBudget` | 10 | From VoxelSettings carving caps. |
| `CanModify` | 20 | Budget check (no consume) — for UI. |
| `GetRemainingModifications` / `GetRemainingVolume` | 47 / 53 | -1 = unlimited. |
| `ApplyModification` | 63 | Enforces budget, stores in all overlapped chunks, returns dirty coords. |
| `GetDensityOffset` | 131 | Per-voxel combined diff (smoothstep falloff, additive). |
| `HasModifications` | 160 | Fast reject for hot path. |
| `HasAnyMods` / `GetModsVersion` | h:238 / h:241 (inline) | Lock-free atomics: any-mod-exists flag + monotonic mod-state version (bumped by `ApplyModification`/`Clear`). |
| `GetChunkModsSnapshot` | — | Copy one chunk's mod list under ONE read lock. Workers snapshot per (chunk, version) instead of locking per voxel — `GetDensityAt` keys a `thread_local` 64-slot direct-mapped cache on it (~27 lock ops per tile task instead of ~86k once any carve exists). |
| `HasAnyModInChunkRange` | — | Any modified chunk key in an inclusive chunk box? One key walk under a read lock — ClassifyTile's diff guard (per tile, conservative by construction: mods are stored in every chunk their radius overlaps). |
| `EvaluateMods` (static) | — | Lock-free pure evaluation of a mod list at a voxel — shared core of `GetDensityOffset` and the generator's snapshot path. |
| `Clear` | 170 | Wipe all (season reset). |
| `GetTotalModificationCount` / `GetModifiedChunkCount` | 182 / 192 | Stats. |

### 3.10 Mesher — `Public/VoxelMarchingCubesMesher.h` + `.cpp`
`UVoxelMarchingCubesMesher : UObject` (h:21). Holds `Generator` ptr, `IsoLevel=0`, skirt params.
(The dead trio `GetDensity`/`InterpolateEdge`/`ComputeGradientNormal` + `GradientOffset` was
removed — since T1.b the pre-sampled grid supplies positions AND gradients inline.)
| Method | .cpp line | Role |
|--------|-----------|------|
| **`GenerateSheetMesh`** | ~455 | **F18 far-field SHEET** (render-distance ring, `Tile.Level > MaxClipLevel`): two displaced heightfield grids per tile — ground (polygroup 0) + sky-cap (polygroup 1) from `GetSurfaceHeightAt` columns (StrateChunkZ = band mid identifies the strate; non-SurfaceWorld ⇒ empty). Classes true BY CONSTRUCTION (no vote/probes). Same conventions as `GenerateMesh` (world-cm positions, planar UVs, F6 colour masks, −N double-faced perimeter skirts per bucket, ground‖cap + `NumCeilingTriangles`). Margin ring keeps normals continuous between sheets. XY HOLE params: cells fully inside the MC-covered box around the player (`AVoxelWorld::SheetHole*Vox`, shrunk 1 tile for seam overlap) are skipped — a partially-covered sheet must not overlay near terrain; hole-edge cells get no skirt. Carved features (passages/spine/chasms) + diff layer NOT represented — accepted at sheet distance. |
| **`GenerateMesh`** | ~15 | The MC loop over cells; `Step` controls LOD sampling. Sets the T2.b octave bias for the tile (`TGuardValue` on `VoxelGenLOD::OctaveBias`, from `LODOctaveDrop` × log2(Step); 0 at LOD0/off). Edge `t` + grid-gradient normals computed inline (`SampleG`/`GradAt`). Optional `OutCaptureGrid` (4th arg) = CAPTURE-DURING-MESHING: when non-null + full-res (`CellsPerAxis==CHUNK_SIZE`), copies the already-sampled `CHUNK_SIZE³` density grid (quantized via `VF_QuantizeDensity`, VoxelTypes.h) so the density clipmap reuses it instead of re-sampling `GetDensityAt`. Pure read of the grid — §8.10 untouched. **F17 surface class**: each unique vertex is classified sol/sky-cap in `GetOrCreateVertex` (down-facing only → memoized `GetSurfaceHeightAt`, nearer `CeilSurf` = cap); triangles bucket by majority into `GroundTris`/`CapTris` (thread_local), skirts emit per bucket, then `Triangles = ground‖cap` + `FVoxelMeshData::NumCeilingTriangles` (→ polygroups in `BuildTileStreamSet`). Same tris, only index ORDER changes. STRATE CONTENT CUT: optional `BandZMin/MaxVox` params restrict the cz cell loop (+ gz sampling rows) to the player-strate band — coarse straddling tiles mesh ONE strate (kills far-LOD inter-strate aliasing holes); meshed cells bit-identical. |

**`Public/MarchingCubesTables.h`** — `EdgeTable` + `TriTable` reference data (Paul
Bourke). Cube corner/edge layout documented at top (lines 7-37). Rarely needs editing.

### 3.11 Per-chunk content & per-strate atmosphere (2026 redesign — see §8)
| File | Role |
|------|------|
| `Public/Private/VoxelContentManager.h/.cpp` | `UVoxelContentManager` — distance-based world-grid decoration scatter (no LOD pop, surface-snapped via `GetDensityAt`) + level-0 water planes. **F7 companions:** each `FStrateDecoration` may list `FDecoCompanion` satellites (rocks/mushrooms around a tree) — emitted per placed parent in `PlaceAtCrossing`, deterministic (pure fn of the parent hash), inherit the parent's surface point; `FDecoSpawn::CompanionIdx` routes each back to its `Companions[ci].Profile` at apply. **TWO streaming grids** (`FDecoGrid` Near/Far, picked per entry via `FStrateDecoration::StreamTier`): NearGrid = short radius + fine column grid (groundcover); FarGrid = full radius + coarse grid (cheap rare/large props). **Plus `UpdateLandmarks`** — rare deliberately-placed objects: mini-suns, ruins, shrines, monuments (`FStrateLandmark`; set-pieces folded in 2026-07-06). Per entry an `AnchorMode`: **HashLattice** (coarse hash lattice, cell = SpacingChunks chunks → cheap at any radius, no per-chunk freeze) or **PassageMouth** (`GetPassages()` endpoints landing in this strate). Gated by `Profile` + `Profile.Conditions`; optional deterministic **exclusion radius** (Priority+hash rank, `ExclusionRadiusChunks`; 0 = off → pure scatter is unchanged, skips the O(n²) resolve). Synchronous, deterministic, strate-wide; spawns via `SpawnFromProfile` (+ orb wrapper). Exclusion is POP-FREE (gathers a `MaxExcl` ring of suppress-only candidates so a piece's fate is player-position-independent). `bSuppressDecorationsUnder`/`SuppressRadiusChunks` clear grass under a footprint (on spawn + re-cleared in `ApplyRegion`). **`RemoveDecorationsInSphere`** (world sphere → both grids' HISMs via `GetInstancesOverlappingSphere`+`RemoveInstances`) is the shared primitive, also called by `AVoxelWorld::ApplyModification` so player digging removes floating grass instantly. Owned by `AVoxelWorld`. §8.5. |
| `Public/Private/VoxelAtmosphereManager.h/.cpp` | `UVoxelAtmosphereManager` — per-strate fog/skylight + persistent ceiling/floor layer actors + full `AtmosphereActor` override. Owned by `AVoxelWorld`. §8.6. |
| `Public/Private/VoxelDensityVolume.h/.cpp` | `UVoxelDensityVolume` — player-centred DENSITY CLIPMAP (N toroidal R8 levels, fine near / coarse far) streamed to GPU `UVolumeTexture`s for the mini-sun raymarched shadow march. Fills run on ONE dedicated thread (`FVoxelDensityFillRunnable`, off the task pool); level 0 is mostly fed by CAPTURE-DURING-MESHING (mesher grid reuse, gated by `IsTileCaptureUseful` so only tiles near the shadow window pay the capture). Carve → `MarkDirtyVoxelBox` refills locally. `VolumeEpoch` drops stale fills. Owned by `AVoxelWorld` (`bEnableDensityVolume`); shader params pushed via shared per-base-material MIDs (`AVoxelWorld::UpdateTerrainMaterialParams`, change-detected). |

> The big 2026 redesign (8 archetypes, (0,0) spine, inter-strate gap, per-strate passages,
> disturbances, content/atmosphere, brush shapes, perf invariants) is documented in **§8** —
> read it first when touching generation/strates/passages.

---

### 3.12 Automation tests — `Private/Tests/` (added 2026-07-27, `#if WITH_DEV_AUTOMATION_TESTS`)
The plugin's first tests (`OPSTACK-PLAN.md` Phase 0.5). Run them from the editor's
**Session Frontend → Automation**, filter `VoxelForge`.

| File | Test name | What it proves |
|------|-----------|----------------|
| `VoxelForgeTestFixture.h` | — | `FTestWorld`: a headless world (transient strate definitions → `UVoxelSettings` → a real `UVoxelStrateManager::Initialize`) so tests hit `GetDensityAt`, where the thread_local caches live. One strate per archetype, **pinned via `FixedStrates`** so slot index → archetype is stable across seeds (`SlotSurfaceWorld` etc.). |
| `VoxelForgeDensityPurityTest.cpp` | `VoxelForge.Determinism.DensityPurity` | 10k points re-sampled in shuffled order, same thread **and** on N workers, asserting BIT equality. `ValidateDeterminism` is game-thread only and cannot see worker-cache divergence. Includes a flat-field canary (AUDIT C1) and a diff-layer pass. |
| ″ | `VoxelForge.Determinism.LiveEditInvalidation` | AUDIT C2 regression: triple the heightfield params, `Initialize` again, require the density to MOVE. The edit does not move the strate, so only `LayoutVersion` changes. |
| `VoxelForgeClassifyTileTest.cpp` | `VoxelForge.Determinism.ClassifyTileSoundness` | Scans for a non-`Mixed` verdict, then brute-forces the exact mesher lattice (`g ∈ [-1, Cells+1]`). **A false verdict is an invisible, collisionless hole** — T1.d v1 was reverted for exactly this. Errors out rather than passing if it found nothing to check. |
| `VoxelForgeDiffLayerTest.cpp` | `VoxelForge.Determinism.DiffLayerContention` | N readers running the worker call mix while the game thread writes and `Clear()`s. Survival + monotonic `ModsVersion`. |

## 4. The density pipeline (most-edited hot path)

### 4.1 `GetDensityWithParams` (TunnelNetwork) — VoxelGenerator.cpp:277
Stage order (negative=solid throughout). Each stage's anchor:
| Step | Line | What |
|------|------|------|
| 1 — Vertical scale | 307 | Stretch Z before noise (`VerticalScale`). |
| 2 — Base density | 316 | Everything starts solid at `BaseDensity`. |
| 3 — Cave warp | 321 | Domain-warp the SDF query coords (organic shapes). |
| 4 — SDF morphology | 368 | Rooms+tunnels via `BuildChunkCache`/`EvaluateSDFCached`. |
| 4b — Surface roughness | 564 | Volumetric noise near surfaces (fBM/Ridged/Mixed). |
| 4c–4h — Terrain ops | 688 | Per-room op applied near surfaces. Sub-anchors below. |
| · Terracing | 727 | Step-like ledges. |
| · Layer lines | 823 | Horizontal grooves (sin of Z). |
| · Ribbing | 853 | Parallel ridges (sin of Z). |
| · Overhangs | 882 | Low-Z-freq noise shelves. |
| · Cliff sharpening | 918 | Amplify vertical gradient. |
| · Scallop | 962 | Cellular erosion bowls. |
| · Arch/Bridge | 998 | Hash-placed capsules across voids. |
| 4d — Columns | 1053 | Pre-baked vertical cylinders. |
| 4g — Domes | 1077 | Room-relative hemispherical ceilings. |
| 4h — Pinch | 1142 | Passage bottlenecks. |
| 5 — Worm tunnels | 1241 | abs(noise1)+abs(noise2), masked by distance-to-network (`WormNetworkRange`: braids hugging rooms/tunnels, no far-field speckle; 0 = legacy unmasked). |
| 6 — Boundary seal | 1274 | Solid top/bottom shells (`ApplyBoundarySeal`). |
| 7 — Inter-strate passages | 1281 | Carve passages/elevator (`ApplyPassageCarving`). |

### 4.2 `GetSlabDensity` (FlatPlain / CrystalChamber) — VoxelGenerator.cpp:1306
| Step | Line | What |
|------|------|------|
| 1 — Floor surface | 1317 | Noisy floor height. |
| 2 — Ceiling surface | 1343 | Formations hang downward (`abs(noise)`). |
| 3 — Void→base density | 1379 | Solid outside [floor,ceiling]. |
| 4 — Columns | 1399 | World-space hash grid, full-height. |
| 5+6 — Seal + passages | 1461 | Same seal/passage carving as TunnelNetwork. |

---

## 5. "I want to change X" → go here

| Goal | Location |
|------|----------|
| Chunk size / voxel scale | `VoxelTypes.h:19-23` (rebuild everything). |
| View distance / task budget / LOD distances | `VoxelSettings.h` (no recompile of logic — data asset). |
| LOD step mapping | `AVoxelWorld::LODToStep` VoxelWorld.cpp:268; `GetLODForChunk` :242. |
| How chunks stream in/out | `UpdateChunksAroundPosition` VoxelWorld.cpp:362. |
| Async threading / stale-result handling | `LoadChunk` :445, `ProcessPendingChunks` :301, Epoch logic. |
| Add a new cave feature / terrain op | Add enum in `VoxelTerrainOpDefinition.h:36`, params there, `ApplyTo` (.cpp:6), transport fields in `FStrateGenerationParams`, consume it in a new Step inside `GetDensityWithParams`. |
| Tweak room/tunnel shapes | `VoxelCaveMorphology.cpp` `BuildChunkCache` :47 / `EvaluateSDFCached` :757. |
| Worm tunnel behavior | `GetDensityWithParams` Step 5, VoxelGenerator.cpp:1241. |
| Strate stacking / which strate where | `UVoxelStrateManager::Initialize` :10. |
| Boundary blend between strates | `GetGenerationParams` :515 + `FStrateGenerationParams::Lerp` (expands `VF_STRATE_PARAM_FIELDS`, StrateTypes.h — new fields go in that list). |
| Passages between strates | `GeneratePassages` :146 + `EvaluateModifierSDF` :371 + `ApplyPassageCarving` (Generator.cpp:197). |
| Player carve/fill | `CarveAtPosition`/`FillAtPosition` VoxelWorld.cpp:691/709 → `UVoxelDiffLayer::ApplyModification` :63. |
| Mesh smoothness / normals | Grid-gradient in `GenerateMesh` (`GradAt` lambda), `IsoLevel` (h). |
| New slab/flat-world generator | `GetSlabDensity` Generator.cpp:1306 + `FSlabGenerationParams` (StrateTypes.h:1019). |
| Biome placement / layout | `BiomeMapParams` on the strate (cell size, warp, climate freqs) + each biome's climate box. Bake `AVoxelWorld::BakeBiomePreview` to tune. §8.14. |
| What a biome does to terrain | A full archetype param override on the biome (`bOverrideTerrain` + `SurfaceParams`); surface output-blends dominant/neighbour heights in `GetSurfaceDensity`. Caves = content/atmosphere only (determinism, §8.14). |
| Add a biome / biome content | New `UVoxelBiomeDefinition` asset → add to the strate's `Biomes[]`. §8.14 / §8.12. |
| Season reset | `AVoxelWorld::ChangeSeed` :740. |

---

## 6. Conventions & gotchas

- **Density sign is the #1 source of confusion.** Internally (MC) **negative = solid,
  positive = air**. Carve = subtract density (toward positive); Fill = add (toward negative).
  `FVoxelModification::Strength` negative = carve. Comments sometimes say the inverse in
  different layers — trust the MC convention at the mesher.
- **Coordinate units:** density functions take **voxel coords** (not cm). World↔voxel
  conversions live in `VoxelTypes.h`. Mesher converts before calling the generator.
- **Determinism:** all randomness is hash-of-(coord, seed, strateIndex) — no RNG state.
  Same seed ⇒ identical world. Player edits are the only non-deterministic overlay.
- **Async safety:** background tasks must check `bShuttingDown` and only touch the
  generator/mesher (no UObject mutation). Results return via `ProcessQueue`. `EndPlay`
  blocks until `ActiveTaskCount == 0`.
- **Epoch:** every regeneration bumps `GenerationEpoch`; results tagged with an old epoch
  are dropped in `ProcessPendingChunks`. Always carry the epoch through new async paths.
- **Generated code** under `Intermediate/` and `Binaries/` is build output — never edit.
  `*.generated.h` / `*.gen.cpp` are UHT output for the `UCLASS`/`USTRUCT` above.

---

## 7. Files NOT to touch
`Binaries/`, `Intermediate/` — compiler/UHT output, regenerated on build.
`MarchingCubesTables.h` — canonical reference tables, only change if switching MC variant.

---

## 8. Architecture & design deep-dive  →  [ARCHITECTURE.md](ARCHITECTURE.md)

Moved out of the codemap to keep this file a fast navigation index. The archetypes, (0,0)
spine, disturbances, content/atmosphere, biomes, and the **performance invariants**
(`§8.10` — read before optimizing the hot path) now live in **[ARCHITECTURE.md](ARCHITECTURE.md)**.
All `§8.x` cross-references throughout this file point there. **`§9` = the MULTIPLAYER model**
(listen-server-first, design-only) — read before touching streaming / carve / AI: terrain is never
replicated (determinism = replicate seed+layout+diff events only), streaming goes multi-anchor
(collision-only vs full-visual policy per anchor), carves are server-authoritative.
