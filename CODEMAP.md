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
| `FVoxelMeshData` struct | 157-173 | Mesher output (Vertices/Triangles/UVs/Normals/**Colors**). Plain C++, not USTRUCT. `Colors` = F6 material masks (R=dominant biome palette, G=slope, B=border blend weight, A=neighbour biome palette). §8.15. |

### 3.3 Chunk identity — `Public/VoxelChunk.h`
`FVoxelChunk` (USTRUCT, line 19): just a `ChunkCoord` + `GetWorldPosition()`. In a
density-only world the chunk stores no voxels — it's a coord wrapper. Room to cache
per-chunk info later.

### 3.4 Settings — `Public/VoxelSettings.h`
`UVoxelSettings : UPrimaryDataAsset` — the single tuning asset assigned on `AVoxelWorld`.
| Group | Fields (line) |
|-------|---------------|
| Streaming | `ViewDistanceXY=16`, `ViewDistanceUp/Down=5`, `MaxConcurrentTasks=16`, `MaxMeshAppliesPerFrame=4` (defaults — actual values live on the data asset) |
| LOD | `LOD0Distance=4`, `LOD1Distance=8` |
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
| `PostEditChangeProperty` | 45 | Editor live-edit hook. |
| `OnObjectModifiedInEditor` | 58 | Regenerates when a strate asset is edited (if `bLiveEditStrates`). |
| `EndPlay` | 140 | Sets `bShuttingDown`, **waits for `ActiveTaskCount`→0**, unbinds delegate. |
| `BeginPlay` | 177 | Constructs Generator/Mesher/StrateManager/DiffLayer, wires services, seeds. |
| `Tick` | 220 | `UpdateChunksAroundPosition(player)` + `ProcessPendingChunks()`. |
| `GetPlayerPosition` | 231 | Pawn position or zero. |
| `GetLODForChunk` / `LODToStep` | 242 / 268 | Distance→LOD (0/1/2) → step (1/2/4). |
| `IsChunkInRange` | 275 | View-distance test. |
| `ProcessPendingChunks` | 301 | Drains ProcessQueue under per-frame budget; **discards stale epochs**; applies meshes. |
| `UpdateChunksAroundPosition` | 362 | Builds desired set, sorts by distance, loads/unloads, handles LOD changes. |
| `LoadChunk` | 445 | Budget check → `UE::Tasks::Launch` background gen+mesh; RAII task guard. |
| `UnloadChunk` | 493 | Destroys mesh component + map entries. |
| `ApplyMeshToChunk` | — | Upload geometry. **LOD0 → own component (`ChunkMeshes`, collision); LOD1/2 → batched into one component per region (`ChunkRegions`), each chunk a SectionGroup, no collision.** Handles LOD promote/demote between the two. §8.10. |
| `ChunkToRegion` / `ChunkSectionGroupName` / `RemoveChunkFromRegion` / `DestroyIndividualChunkComponent` | — | Plumbing for the batched far-chunk scheme. |
| `GetStrateAtPosition` | 679 | Gameplay query → strate index. |
| `CarveAtPosition` / `FillAtPosition` | 691 / 709 | Build `FVoxelModification` → DiffLayer → RemeshDirtyChunks. |
| `ClearAllModifications` | 726 | Clears diff layer, regenerates. |
| `ChangeSeed` | 740 | **Season reset**: new seed everywhere, clear diffs, bump season, reload. |
| `GetCurrentSeed` / `GetCurrentSeason` | 784 / 789 | Accessors. |
| `RemeshDirtyChunks` | 798 | Re-queue loaded chunks for async re-mesh (no visual pop). |

> **Game-thread profiling (Perf):** `AVoxelWorld::Tick` and its sub-steps are wrapped in `TRACE_CPUPROFILER_EVENT_SCOPE` — `VoxelForge_Tick / UpdateChunks / BuildDesiredTiles / CullTiles / SubmitTiles / ProcessPending / ProcessUnload / UpdateDecorations / UpdateWater`. Capture a `Count/Incl/Excl` Insights timer export and read the `Excl` column to see which step owns the per-frame cost (the actor tick shows as `BP_VoxelWorld_C` if subclassed in BP). `VoxelForge_GenerateMesh` is worker-side (off the frame).

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
| `ComputeSurfaceTerrainZ` / `GetSurfaceDensity` | — | SurfaceWorld heightfield → terrain Z, then density; biome **output-blend** lerps dominant/neighbour heights (`ParamsD`/`ParamsN`/weight). §8.14. |
| `SampleRelief` / `SampleMoisture` | — | Climate fields (pure XY, [0,1]). Relief = shared source of truth for the relief map M. §8.14. |
| `SampleBiomeAt` | — | Warped-Voronoi + climate biome query (dominant + neighbour + weight). Reference used by the preview bake + `GetDominantBiomeAt`. §8.14. |
| `ResolveBiomeSampleAt` / `RebuildBiomeGrid` | — | Hot-path biome resolve (FBiomeSample) via a box-validated per-chunk cell-grid cache. Bit-identical to `SampleBiomeAt`. §8.14, §8.10. |
| `GetDominantBiomeAt` | — | Game-thread query → dominant biome ASSET (content/atmosphere). §8.14. |

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
  | `BuildChunkCache` | 47 | **Phase 1** (once/chunk): collect rooms, guaranteed backbone (`bTunnelsFlowTowardOrigin`: tree rooted at the (0,0) hub — every room reachable, links flow inward; false = legacy NN forest), slope-aware link metric (`TunnelHorizontalBias` now applies to backbone too), decide tunnels, **cull zero-connection rooms** (no sealed bubbles), store rooms by their OWN reach (fixes origin-room clipping at `MaxInfluence`), pre-bake pits/chimneys/columns, hash-roll per-room terrain op. |
  | `EvaluateSDFCached` | 589 | **Phase 2** (per voxel): SmoothMin over cached rooms/tunnels; returns nearest room idx for terrain-op lookup. |
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
| **`FStrateGenerationParams`** | 213 | The giant TunnelNetwork param bag (rock, worms, rooms, tunnels, warp, roughness, all terrain-op transport fields, boundary seal). `Lerp()` static at 844 blends two sets at boundaries. |
| `FStrateTerrainOpEntry` | 965 | Soft-ptr to a terrain op + Weight + Probability. |
| **`FSlabGenerationParams`** | 1019 | Floor/ceiling heights, roughness, columns, seal — for slab generators. |
| `FStrateDecoration` / `FStrateAmbientActor` / `FStrateCreature` | 1160 / 1192 / 1212 | Content spawn entries (consumed by future systems). |

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
| `FindSlotIndexForChunkZ` | 427 | Z → layout index. |
| `GetStrateAt` / `GetStrateIndex` | 443 / 455 | World-Z queries. |
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
border warp+blend / climate field freqs), `EBiomePreviewChannel` (preview-bake selector), and plain
runtime PODs `FBiomeResolved` / `FBiomeContext` / `FBiomeSample` / `FChunkBiomeCache` (the
box-validated per-chunk grid cache). See §8.14.

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
| `Clear` | 170 | Wipe all (season reset). |
| `GetTotalModificationCount` / `GetModifiedChunkCount` | 182 / 192 | Stats. |

### 3.10 Mesher — `Public/VoxelMarchingCubesMesher.h` + `.cpp`
`UVoxelMarchingCubesMesher : UObject` (h:21). Holds `Generator` ptr, `IsoLevel=0`,
`GradientOffset=1`.
| Method | .cpp line | Role |
|--------|-----------|------|
| `GetDensity` | 11 | Local coord → world → `Generator->GetDensityAt`. |
| `InterpolateEdge` | 28 | Linear edge crossing between two corner densities. |
| `ComputeGradientNormal` | 48 | Central-difference gradient → smooth normal. |
| **`GenerateMesh`** | 75 | The MC loop over cells; `Step` controls LOD sampling. |

**`Public/MarchingCubesTables.h`** — `EdgeTable` + `TriTable` reference data (Paul
Bourke). Cube corner/edge layout documented at top (lines 7-37). Rarely needs editing.

### 3.11 Per-chunk content & per-strate atmosphere (2026 redesign — see §8)
| File | Role |
|------|------|
| `Public/Private/VoxelContentManager.h/.cpp` | `UVoxelContentManager` — distance-based world-grid decoration scatter (no LOD pop, surface-snapped via `GetDensityAt`) + level-0 water planes. Owned by `AVoxelWorld`. §8.5. |
| `Public/Private/VoxelAtmosphereManager.h/.cpp` | `UVoxelAtmosphereManager` — per-strate fog/skylight + persistent ceiling/floor layer actors + full `AtmosphereActor` override. Owned by `AVoxelWorld`. §8.6. |

> The big 2026 redesign (8 archetypes, (0,0) spine, inter-strate gap, per-strate passages,
> disturbances, content/atmosphere, brush shapes, perf invariants) is documented in **§8** —
> read it first when touching generation/strates/passages.

---

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
| Tweak room/tunnel shapes | `VoxelCaveMorphology.cpp` `BuildChunkCache` :47 / `EvaluateSDFCached` :589. |
| Worm tunnel behavior | `GetDensityWithParams` Step 5, VoxelGenerator.cpp:1241. |
| Strate stacking / which strate where | `UVoxelStrateManager::Initialize` :10. |
| Boundary blend between strates | `GetGenerationParams` :515 + `FStrateGenerationParams::Lerp` (StrateTypes.h:844). |
| Passages between strates | `GeneratePassages` :146 + `EvaluateModifierSDF` :371 + `ApplyPassageCarving` (Generator.cpp:197). |
| Player carve/fill | `CarveAtPosition`/`FillAtPosition` VoxelWorld.cpp:691/709 → `UVoxelDiffLayer::ApplyModification` :63. |
| Mesh smoothness / normals | `UVoxelMarchingCubesMesher::ComputeGradientNormal` :48, `IsoLevel`/`GradientOffset` (h:51/55). |
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

## 8. Archetypes, spine, disturbances, content & carving (2026 redesign)

A large A-to-Z expansion. The world is a stack of strates the player descends through;
each strate can be a fundamentally different *archetype*, connected at (0,0).

### 8.1 Archetypes (`ECaveGeneratorType`, VoxelStrateTypes.h)
Each archetype has its own param `USTRUCT` (on `UVoxelStrateDefinition`, EditCondition-gated
by `GeneratorType`) and its own density function in `VoxelGenerator.cpp`, dispatched by the
`switch` in `GetDensityAt`.

| Archetype | Params struct | Density fn | Idea |
|-----------|---------------|------------|------|
| TunnelNetwork | `FStrateGenerationParams` | `GetDensityWithParams` | rooms+tunnels (original) |
| FlatPlain / CrystalChamber | `FSlabGenerationParams` | `GetSlabDensity` | floor/ceiling void (original) |
| Maze | `FMazeGenerationParams` | `GetMazeDensity` | tight corridors on a 3D lattice (per-voxel, no cache; edge = lower node + axis hash) |
| SurfaceWorld | `FSurfaceGenerationParams` | `GetSurfaceDensity` | heightfield terrain: domain-warped continents+ridged mtns+detail, a low-freq **relief map** (`M`) that scales mountains/elevation for plains↔highland variety, opt-in plateau **terracing**, beaches at water line, high sky-cap ceiling. The cap is shapeable terrain in its own right (`ComputeSurfaceCeiling`, `Surface|Sky` params: `CeilingUndulation` broad inverted hills/valleys, `CeilingRidgeStrength` hanging ridgelines, `CeilingRoughness`+freq fine bumps, `CeilingWarp*`) — defaults (strengths 0, freq 0.04) = old flat-ish cap. (`Surface|Macro` params = the cheap precursor to biomes; `ReliefStrength=0` ⇒ old uniform terrain.) **Sky-cap tiles don't cast shadows**: `ApplyMeshToTile` classifies a near tile as ceiling (centre above the terrain↔cap midpoint via the `GetSurfaceHeightAt` oracle) and drives the **RMC per-section** `FRealtimeMeshSectionConfig::bCastsShadow` (NOT the component `SetCastShadow` — RMC's proxy ignores the component flag; this is also why level≥2 far tiles only stopped casting once the section flag was wired). So the rock ceiling never shadows the terrain below it (cap and ground are one mesh but live in different clipmap tiles). The same `bIsCeiling` classification (now computed at every LOD, not just near tiles) also selects the strate's `CeilingMaterial` when set, so the shadowless overhead rock can be tinted/darkened separately from the ground instead of reading flat/bright. |
| VerticalShafts | `FVerticalShaftParams` | `GetVerticalShaftDensity` | full-height shafts + horizontal connectors + partial ledges |
| FloatingIslands | `FFloatingIslandParams` | `GetFloatingIslandDensity` | asymmetric islands: flat land top + underside tapering to a point, lobed (domain-warped) outline, in an open void |
| Underwater | `FStrateGenerationParams` + water | (reuses `GetDensityWithParams`) | tunnel rock + high water table |

All density fns share the convention: internal **positive=solid**, apply origin spine →
boundary seal → inter-strate passages, then `return -Density` (MC: negative=solid).
StrateManager provides params per chunk via `GetMaze/Surface/VerticalShaft/FloatingIslandParamsForChunk`
(macro `VF_ARCHETYPE_PARAMS_GETTER`) — no cross-boundary blend (Hard transitions between archetypes).
On top of the archetype, an optional **biome** layer (§8.14) modulates terrain & content WITHIN a
strate via a window-invariant XY field — currently wired into SurfaceWorld.

### 8.2 (0,0) spine & hybrid connections
- `ApplyOriginSpine` (VoxelGenerator.cpp, static helper) carves a guaranteed open vertical
  column at XY (0,0) in every strate's **interior** (seals untouched). Radius =
  `UVoxelGenerator::OriginSpineRadius` ← `VoxelSettings::OriginSpineRadius`. Called before
  every `ApplyBoundarySeal`.
- Descent is **player-dug** through the thin seals at (0,0). The single auto-opened
  connection is the **surface entry shaft** at (0,0) through the top of strate 0
  (`GeneratePassages`, `bOpenSurfaceEntry`).
- **Hybrid extras:** auto-carved *shortcut* passages per boundary, placed away from (0,0).
  Now fully **per-strate** — see §8.8 (the upper strate's `PassageConfig` drives count/style/shape).

### 8.3 Disturbance layer (the "wow" post-process)
`FStrateDisturbanceParams` (on the definition, all archetypes). `ApplyDisturbances`
(VoxelGenerator.cpp static, **MC convention**) runs in `GetDensityAt` after dispatch:
chasms (carve air), bridges (solid spans), ridges (solid blades). Stays inside seal bands.
Provided per chunk by `StrateManager::GetDisturbanceParamsForChunk`.

### 8.4 Cross-chunk determinism (the seam-prevention invariant)
`BuildChunkCache` (VoxelCaveMorphology.cpp) uses **two regions**: a wide *COLLECT* region
(`2*MaxTunnelLength + MaxInfluence`) over which connectivity is decided (NN filtered to
`<= MaxTunnelLength`, origin cap = deterministic top-N by hash), and a tight *STORE* region
(`+MaxInfluence`) kept for per-voxel eval. This makes the room/tunnel graph window-invariant.
**If you add a connectivity rule with longer edges, the COLLECT region must still cover the
max edge reach, and decisions must not depend on the stored window.**

### 8.5 Content scatter & water — `VoxelContentManager.h/.cpp` (NEW)
`UVoxelContentManager` (owned by `AVoxelWorld`, game-thread). TWO INDEPENDENT subsystems:

**(A) DECORATIONS — distance-based WORLD GRID (the no-pop system, 2026-06-17).** Decorations are placed
on a fixed world XY cell grid (**1 cell = 1 chunk footprint**, `DECO_CELL_VOXELS = CHUNK_SIZE`) and streamed
by DISTANCE from the player, **fully decoupled from clipmap tiles / LOD**. **THE MARCH RUNS ASYNC ON WORKER
THREADS** (mirrors mesh gen — `GetDensityAt` is thread-safe; the synchronous-on-game-thread first cut was a
perf disaster + starved streaming → seams, so it was moved off-thread). Driven by `AVoxelWorld::Tick →
UpdateDecorations(playerWorldPos)`, three phases: **(1)** recompute the desired cell set (`RebuildDesiredCells`)
only when the player crosses a cell boundary OR changes strate — clears out-of-range loaded cells, queues
cells that are NOT loaded and NOT in flight (`PendingLaunch`, nearest-first). **SINGLE radius, NO near/far tiers:**
a loaded cell is NEVER re-streamed in place while it stays in range (only cleared when it leaves), so
decorations don't FLICKER as the player moves / as terrain LOD shells shift (re-streaming on tier crossings was
the flicker cause — tiers removed). **(2)** `LaunchDecoTasks`:
resolve the cell's decoration list on the GAME thread (biome — see below) then fire an async `UE::Tasks` march
(`BuildCellSpawns`, `BackgroundNormal`, capped at `MaxConcurrentDecorationTasks` in flight via `InFlightCells`).
**(3)** `ProcessDecoResults`: drain finished tasks' results (`Mpsc` queue → `ReadyResults`), epoch-guarded
(`DecoEpoch`, bumped on clear/strate-change so stale in-flight results are discarded) + range-checked, and
**apply (spawn) budgeted** (`MaxDecorationCellsPerFrame` — the only game-thread cost, SpawnActor/AddInstance).
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
is **no cross-strate light bleed to cull** (the old `SetActiveStrate` light-culling pass is SUBSUMED — gone).
**Render paths:** `ActorClass` → real actors (lights/logic, pricey game-thread spawn); `InstancedMesh` → HISM
(no tick/actor/collision, emissive glows far), per-cell-per-entry. **Per-entry HISM tuning for dense groundcover**
(`FStrateDecoration`, only the InstancedMesh path): `CullDistance` (cm; 0 = no cull — the lever that makes dense
grass affordable: placed thickly, drawn only near → GPU cost bounded by area-within-cull, NOT the stream radius),
`bCastShadow` (default true; turn OFF for grass — dense instanced shadows are the dominant foliage cost),
`MaxSlopeAngle` (deg from flat = acos(|N.Z|); 90 = no filter, ~35 keeps grass off cliffs — applied in the worker's
`PlaceAtCrossing`). `ApplyDecoResult` buckets spawns per entry and builds each HISM with ONE batched `AddInstances`
(single cluster-tree build, set cull/shadow BEFORE `RegisterComponent`) — the game-thread hitch-killer for dense cells.
Note: NO per-entry placement radius (it would fight the no-re-stream cell model — cells stream once at the outer ring
and persist, so a smaller radius would never populate already-loaded far cells as the player approaches; CullDistance
covers the render cost instead). **All entries stream within ONE radius**
(`DecorationRadiusChunks`) — the old near/far tier split was removed (it caused flicker). `MaxLODLevel` and
`DecorationActorRadiusChunks` are now LEGACY/unused. **No LOD area-density compensation** (placement is per real
surface point, density-stable with distance). **SpawnDensity semantics CHANGED** vs the old vertex scatter: it
rolls per column surface-point (not per mesh vertex) → expect a one-time density re-tune. **Settings
(`Voxel|Content`):** `DecorationRadiusChunks` (6 — reach in cells), `DecorationSpacingVoxels` (4 → 8×8 cols/cell),
`DecorationMarchStepVoxels` (2 — coarse, bisection-refined; cave march only), `DecorationMaxCrossingsPerColumn`
(4 — cave march only), `DecorationColumnDepthVoxels` (160 — bedrock march cap; cave march only),
`MaxDecorationCellsPerFrame` (2 — apply/spawn budget), `MaxConcurrentDecorationTasks` (4 — in-flight task cap;
0 disables decorations). **COST:** surface worlds now use the O(1) oracle (cheap); caves ray-march. The work is
OFF the frame (worker threads) — game thread only pays the budgeted spawn. If streaming slows, lower
`MaxConcurrentDecorationTasks` / raise `DecorationMarchStepVoxels` / shrink radii/spacing. Default
`DecorationRadiusChunks=6` ≈ props ~48 m out — raise for far flora (cost ~r²).

**(B) WATER — tile-driven, level-0 only (continuous plane, never pops).** `PopulateTileWater(tile)` in
`ApplyMeshToTile` (level-0 tiles), `ClearTileWater(tile)` in `UnloadTile`. One scaled engine plane
(`/Engine/BasicShapes/Plane`) per water-surface chunk (per-chunk-Z plane logic assumes a single chunk's
vertical span — hence level-0 only), keyed `TMap<FIntVector, UStaticMeshComponent*>` (reflected UPROPERTY).
Water Z: `bHasWater` + `WaterLevelRelative` → `StrateManager::GetWaterLevelWorldZForChunk`. Biome
`WaterMaterial` overrides `UVoxelStrateDefinition::WaterMaterial` (level stays strate-global).

`ClearAll`/`SetSeed` on `ChangeSeed`/regenerate clears both subsystems (decorations re-stream on the next
Tick via the INT_MIN sentinels). **Per-biome content (§8.14):** the cell-centre (decorations) / chunk-centre
(water) dominant biome (`Generator::GetDominantBiomeAt`, game-thread, uncached — once per cell, NOT per
column) supplies the decoration list (replaces the strate's) + water material. `Initialize` now also takes
`UVoxelSettings*` (for the grid tunables). `ContentMaxLevel` is now legacy/dead for decorations.

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
(`FVoxelPassage::BoundCenter/BoundRadiusSq`). Both are perf-critical (§8.10). The (0,0) surface entry
is a simple straight tube. Global passage settings were removed from `VoxelSettings`.

### 8.9 Carving — brush shapes + editor controls
`FVoxelModification` has `EVoxelBrushShape {Sphere,Box,Capsule}` + `BoxExtent`/`CapsuleEnd`/
`Falloff` + `GetWorldBounds`. `UVoxelDiffLayer::GetDensityOffset` switches per shape; chunk
overlap uses the shape AABB. `AVoxelWorld`: `CarveBox/FillBox/CarveCapsule/FillCapsule/
ApplyModification` (BlueprintCallable) + `EditorCarveSphere/EditorFillSphere` (CallInEditor)
driven by `EditorBrush*` props.

### 8.10 Performance invariants (DON'T regress)
- **Streaming** (`UpdateChunksAroundPosition`): rebuild/cull the desired set ONLY when the
  player crosses a chunk boundary (`LastUpdateCenter`); use the `DesiredSet` TSet for the cull;
  idle via `bAllChunksLoaded`. Stationary player ≈ free. (Old per-frame O(loaded×desired) scan = 22ms.)
- **LOD** changes HOT-SWAP (`LoadChunk` only, never unload-first) → no holes. LOD
  reconciliation lives in the PERSISTENT per-frame submit loop (same loop as new-chunk loads),
  NOT as a one-shot on the boundary-cross frame — a one-shot drops every chunk past the task
  budget and strands it at a stale LOD. Idle (`bAllChunksLoaded`) only when a full scan finds
  no loads AND no LOD mismatches outstanding.
- **SDF cache** (`GetDensityWithParams`): search-BOX validity, not chunk-key — gradient ±1
  sampling must not thrash the (expensive) rebuild.
- **Per-chunk param cache** in `GetDensityAt`: GenType + param struct + disturbance cached
  thread-locally per chunk; don't move the fetch/blend back to per-voxel.
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
  is rebuilt on `RebuildStrates`).
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
- **Normals from the density grid (T1.b)** (`GenerateMesh`): corner gradients = central differences
  on the (margin) grid; edge normals interpolate the two corner gradients by the SAME `t` as the
  position → seamless across chunk borders (both sides use identical pure samples). NO per-vertex
  `GetDensityAt` (was ~6/vertex, often as costly as the whole grid). `ComputeGradientNormal` is now
  unused. Only NORMALS changed vs the old path; geometry is identical.
- **Surface column cache (T1.a)** (`FSurfaceColumnCache` = LRU of `FSurfaceColumnBox`, `GetDensityAt`
  SurfaceWorld branch): the heightfield + sky-cap + biome blend are a PURE function of (XY, seed,
  strate) — **ZERO Z dependence** (climate/Voronoi are pure-XY; surface params are per-strate constant
  under Hard transitions) — yet sampled ~33× per column (once per Z grid-point). Cached per integer XY
  (box-valid, like the SDF cache) and reused down the column. **Keyed by (XY box, StrateKey, Seed), NOT
  ChunkZ** (`StrateKey = round(StrateBottomWorldZ)`, taken from the params so it can't disagree with
  them) and held as a small **LRU of 6 boxes** so the WHOLE vertical view-distance stack — and XY
  neighbours the scheduler interleaves — share one another's heavy column noise instead of each
  recomputing it ~once per vertical chunk (this was the dominant `GenerateMesh` cost: the same 2D
  heightfield recomputed per altitude). It also makes pure-air / pure-solid chunks cheap (they hit the
  shared box). Box `Halo = CHUNK_SIZE + 8` each side so the T1.b margin ring stays inside (no thrash).
  **Used ONLY for integer-XY queries**; fractional queries compute directly → bit-identical. Don't
  re-introduce a ChunkZ key, don't feed it fractional coords. (`GetSurfaceHeightAt`'s own `OC_*` oracle
  cache is separate and still per-chunk — lower volume, not worth the LRU.)
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
  the better mesh is wholly in, then swaps. In-flight (pending) tiles are NEVER cancelled on a rebuild —
  they finish, apply, and are culled later if no longer desired. Collision level-0 only; water level-0
  only; shadows off for level≥2. **Decorations are NO LONGER tied to tiles** — they stream on a fixed world
  grid by distance (§8.5), so they don't pop on LOD swaps. Settings: `VoxelSettings::ClipRadius` (full-res near radius, tiles/level),
  `MaxClipLevel` (far reach). **NEAR-FIELD GEN COST levers** (`LoadTile`): levels `< FullResClipLevels`
  mesh at full `CHUNK_SIZE` cells (≈35³ `GetDensityAt` incl. margin ring), coarser levels at
  `CoarseTileCells` (Step = Extent/Cells) for far-cheaper gen. A level-1 tile at `FullResClipLevels=2`
  costs the SAME gen as a level-0 tile (same cell count, 8× extent) — set `FullResClipLevels=1` to drop
  level 1 to `CoarseTileCells` (~6× cheaper) when the near field is gen-bound (slightly harder L0→L1
  seam, hidden by skirts). `ClipRadius` bounds the full-res level-0 tile COUNT independently of reach.
  `GetLODForChunk` / `LODToStep` / `IsChunkInRange` / `GetStrateChunkZBounds` and the
  ViewDistance/LOD/strate-Z/ceiling settings are now DEAD/unused (left in place).
- **SKIRTS — LOD-seam crack filler** (`GenerateMesh`, after the cell loop; `VoxelSettings::bGenerateSkirts`
  + `SkirtCells`, wired onto the mesher at setup). Neighbouring shells mesh at different resolutions so
  their iso-surfaces don't meet along the shared face → a thin see-through crack. After meshing, every
  triangle edge whose BOTH endpoints lie on one of the tile's 6 outer boundary planes (exact float compare
  — MC keeps the face-axis coordinate fixed) is a surface-contour edge on that face; a skirt quad hangs
  from it INTO the solid along the inverted vertex normals by `SkirtCells × Step × VOXEL_SIZE` (~one cell,
  ≥ the gap to a one-level-coarser neighbour). Emitted DOUBLE-SIDED (both windings) so it shows regardless
  of camera side / material two-sidedness; buried elsewhere → invisible. Adds verts/tris ONLY on boundary
  contour edges (small). Tune `SkirtCells` up if cracks persist, down if skirts peek out on convex edges.
- **Budgeted teardown** (`PendingUnload` + `ProcessUnloadQueue`, called from `Tick` after the apply
  drain; `VoxelSettings::MaxUnloadsPerFrame`): the cull APPROVES removals (strict load-before-unload) but
  doesn't destroy in place — it queues them. `ProcessUnloadQueue` runs at most `MaxUnloadsPerFrame`
  `UnloadTile`s/frame (scaled up to 4× with backlog, capped so a huge backlog can't re-spike). WHY: a
  fast traversal culls a whole shell's worth of tiles in ONE frame, and each `UnloadTile` does
  `DestroyComponent` + (level 0) `ContentManager::ClearChunk` → `Destroy()` of every decoration actor —
  an unbudgeted burst = a game-thread spike ("stuff torn down behind you" at speed). Mesh APPLIES were
  already budgeted; this matches it for DESTROYS. Re-desired tiles are cancelled out of the queue (still
  loaded → no reload). `PendingUnload` is cleared in `RegenerateAllChunks`/`EndPlay` (tiles already gone).
- **Collision only at LEVEL 0** (`ApplyMeshToTile`): `UpdateSectionConfig(..., Tile.Level==0)`. Far tiles
  are unreachable; cooking their Chaos collision is waste. (Was T1.c, now per-tile-level.)
- **No shadows on far tiles (draw cut)** (`ApplyMeshToTile`): `SetCastShadow(Tile.Level <= 1)`. Each
  shadow-casting tile emits a second shadow-pass draw; the far coarse tiles don't need it. NOTE: fps is
  RENDER-side (draws ≈ visible tile count × passes); generation cost (workers) and tile *resolution*
  (cuts triangles, not draws/components) don't move the game thread — tile COUNT does (hence the clipmap).
- **Insights scopes** `VoxelForge_GenerateMesh` / `VoxelForge_ApplyMeshToChunk` (Perf 0) bracket
  the worker gen + game-thread apply — capture a trace to see if we're density- or upload-bound.
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
- **`ProcessQueue` MUST be `EQueueMode::Mpsc`** (`VoxelWorld.h`): up to `MaxConcurrentTasks`
  `ChunkGen` worker threads `Enqueue` concurrently; the game thread is the sole consumer.
  The default `Spsc` is single-producer — concurrent enqueues race the tail link and silently
  DROP results, leaking `PendingChunkCoord` slots until the budget is exhausted and streaming
  stalls for good (intermittent; worst during the completion bursts right after the player moves).

### 8.11 Live tuning & debug (`AVoxelWorld`, CallInEditor / PIE)
- `RebuildStrates` — re-reads ALL of `VoxelSettings` and rebuilds layout/gap/passages/spine +
  regenerates. Use after changing those (plain `RegenerateAllChunks` keeps the old layout/passages).
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
  level). Per voxel: `ResolveBiomeSampleAt` → dominant `PD` (+ neighbour `PN`); `GetSurfaceDensity`
  computes `ComputeSurfaceTerrainZ` for `PD` and, in the border band, for `PN`, and **lerps the
  resulting HEIGHTS**. Blending heights (not params) is seamless across *any* difference (frequencies
  included) — what per-param blend never could. `PD==PN`, weight 0 ⇒ bit-identical, no biomes.
- **Consumption — CAVES: structural overrides are NOT applied (determinism).** Rooms/tunnels are
  decided over a wide COLLECT region spanning chunks (§8.4); making room params vary by region would
  need the biome sampled per *room site* inside `BuildChunkCache`, or it breaks window-invariance
  (a room near a border resolves differently per querying chunk → seams/holes). So SDF archetypes
  (Tunnel/Maze/Shaft/Islands) keep strate-level structure; biomes affect them via **content +
  atmosphere only** (below). Per-room-site biome params = a future deep task.
- **Consumption (content/atmosphere).** `GetDominantBiomeAt(x,y,chunkZ)` (game-thread, uncached) →
  biome ASSET. ContentManager: dominant biome's decorations (else strate's) + water material override.
  AtmosphereManager: player's dominant biome fog/sky (`bOverrideAtmosphere`). Works for ANY archetype.
  Water LEVEL stays strate-global (continuous plane); biomes retint material only.
- **Preview tool.** `AVoxelWorld::BakeBiomePreview()` (CallInEditor) bakes biome / relief / moisture
  to `Saved/BiomePreview.png` via a transient generator (no PIE). Needs the `ImageWrapper` module.
- **Status:** A (field+asset+preview), B (terrain), C (content/atmosphere) verified in-editor.
  Full-param redesign (surface output-blend) code-complete, pending build. Cave structural biomes
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
- **Status:** C++ code-complete, pending in-editor build + the master material graph (editor-side work).
