# VoxelForge — Code Map & Knowledge Index

> Purpose: a navigation index so anyone (human or AI) can locate and modify code
> without re-reading the whole plugin. Anchors are `File:line` — line numbers drift
> as code is edited, so trust the **symbol name** first and the line as a hint.
>
> Plugin root: `Source/VoxelForge/` · ~8,300 lines of C++ across 22 files.
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
| `CHUNK_SIZE` (32), `CHUNK_SIZE_SQUARED`, `CHUNK_VOLUME` | 19-21 | Chunk dimensions. |
| `VOXEL_SIZE` (25.0f cm) | 23 | World scale. |
| `EVoxelFace` enum + `GetFaceDirection` / `GetFaceNormal` | 33-61 | 6 cube faces. |
| `WorldToChunkCoord` / `WorldToLocalCoord` / `ChunkToWorldPos` | 74-104 | Coord-space conversions (handle negatives via floor/positive-modulo). |
| `LocalToIndex` / `IndexToLocal` / `IsValidLocalCoord` | 107-131 | Flat-array 3D↔1D indexing. |
| `SmoothStep01` | 140 | 3x²-2x³ — used everywhere for blends. |
| `VOXEL_NOISE_SCALE` (1.25f) | 147 | Rescales UE PerlinNoise3D to ~[-1,1]. |
| `FVoxelMeshData` struct | 157-173 | Mesher output (Vertices/Triangles/UVs/Normals). Plain C++, not USTRUCT. |

### 3.3 Chunk identity — `Public/VoxelChunk.h`
`FVoxelChunk` (USTRUCT, line 19): just a `ChunkCoord` + `GetWorldPosition()`. In a
density-only world the chunk stores no voxels — it's a coord wrapper. Room to cache
per-chunk info later.

### 3.4 Settings — `Public/VoxelSettings.h`
`UVoxelSettings : UPrimaryDataAsset` — the single tuning asset assigned on `AVoxelWorld`.
| Group | Fields (line) |
|-------|---------------|
| Streaming | `ViewDistanceXY=16` (26), `ViewDistanceUp/Down=5` (29/32), `MaxConcurrentTasks=16` (36), `MaxMeshAppliesPerFrame=4` (40) |
| LOD | `LOD0Distance=4` (48), `LOD1Distance=8` (53) |
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
| `ApplyMeshToChunk` | 503 | Get/create RealtimeMeshComponent, upload geometry, assign material. |
| `GetStrateAtPosition` | 679 | Gameplay query → strate index. |
| `CarveAtPosition` / `FillAtPosition` | 691 / 709 | Build `FVoxelModification` → DiffLayer → RemeshDirtyChunks. |
| `ClearAllModifications` | 726 | Clears diff layer, regenerates. |
| `ChangeSeed` | 740 | **Season reset**: new seed everywhere, clear diffs, bump season, reload. |
| `GetCurrentSeed` / `GetCurrentSeason` | 784 / 789 | Accessors. |
| `RemeshDirtyChunks` | 798 | Re-queue loaded chunks for async re-mesh (no visual pop). |

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
  | `BuildChunkCache` | 47 | **Phase 1** (once/chunk): collect rooms, nearest-neighbor backbone, decide tunnels, pre-bake pits/chimneys/columns, hash-roll per-room terrain op. |
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
`GenerationParams`(113), `SlabParams`(124), `TerrainOperations`(147), visuals/fog/light,
content lists, audio, `GameplayTags`(223). EditConditions show/hide param groups by generator type.

**`Public/VoxelStrateManager.h` + `.cpp`** — `UVoxelStrateManager : UObject` (h:108).
Maps depth→strate at runtime; owns passages.
- `FVoxelPassage` (h:39): endpoints, radius, type, control points.
- `FStrateSlot` (h:84): definition + chunk-Z range + index.
| Method | .cpp line | Role |
|--------|-----------|------|
| `Initialize` | 10 | Builds the stacked layout from settings+seed (fixed slots + shuffled pool), then `GeneratePassages`. |
| `GeneratePassages` | 146 | Deterministic passages between consecutive strates (per-type control points). |
| `EvaluateModifierSDF` | 371 | SDF of all passages + elevator shaft at a point (for carving). |
| `FindSlotIndexForChunkZ` | 427 | Z → layout index. |
| `GetStrateAt` / `GetStrateIndex` | 443 / 455 | World-Z queries. |
| `GetStrateForChunk` | 466 | Chunk → definition. |
| `GetGeneratorTypeForChunk` | 476 | Chunk → generator type. |
| `GetSlabParamsForChunk` | 490 | Slab params with runtime Z bounds (no blend — slabs use Hard). |
| `GetGenerationParams` | 515 | **Blended** TunnelNetwork params (handles Gradient/Hard/Interleaved transitions). |
| `BuildParamsFromDefinition` (static) | 771 | Base params + merge all referenced terrain op assets. The one place ops fold into params. |

**`Public/VoxelTerrainOpDefinition.h` + `.cpp`** — `UVoxelTerrainOpDefinition : UPrimaryDataAsset`
(h:67). One asset = one terrain op. `EVoxelTerrainOpType` (h:36): Terrace, LayerLines,
Ribbing, Cliff, Scallop, Overhang, Arch, Column, Pit, Chimney, Dome, Pinch. Per-type
param groups gated by EditCondition. `ApplyTo(OutParams, Weight)` (.cpp:6) copies only
the active type's fields into `FStrateGenerationParams`, scaled by Weight.

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
| `Public/Private/VoxelContentManager.h/.cpp` | `UVoxelContentManager` — deterministic decoration scatter (LOD0) + aesthetic water planes, per chunk. Owned by `AVoxelWorld`. §8.5. |
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
| 5 — Worm tunnels | 1241 | abs(noise1)+abs(noise2) connectivity. |
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
| SurfaceWorld | `FSurfaceGenerationParams` | `GetSurfaceDensity` | heightfield terrain (continents+ridged mtns+detail), beaches at water line, high sky-cap ceiling |
| VerticalShafts | `FVerticalShaftParams` | `GetVerticalShaftDensity` | full-height shafts + horizontal connectors + partial ledges |
| FloatingIslands | `FFloatingIslandParams` | `GetFloatingIslandDensity` | asymmetric islands: flat land top + underside tapering to a point, lobed (domain-warped) outline, in an open void |
| Underwater | `FStrateGenerationParams` + water | (reuses `GetDensityWithParams`) | tunnel rock + high water table |

All density fns share the convention: internal **positive=solid**, apply origin spine →
boundary seal → inter-strate passages, then `return -Density` (MC: negative=solid).
StrateManager provides params per chunk via `GetMaze/Surface/VerticalShaft/FloatingIslandParamsForChunk`
(macro `VF_ARCHETYPE_PARAMS_GETTER`) — no cross-boundary blend (Hard transitions between archetypes).

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
`UVoxelContentManager` (owned by `AVoxelWorld`, game-thread). Chunk lifecycle:
`PopulateChunk(coord, mesh, LOD)` in `ApplyMeshToChunk`, `ClearChunk` in `UnloadChunk`,
`ClearAll`/`SetSeed` in `ChangeSeed`. Deterministic decoration scatter on mesh vertices
(surface-type / water-relative / align-to-normal / random-yaw / scale / `MaxPerChunk`, from
`FStrateDecoration`) — **only at LOD 0** (perf). Aesthetic water = one scaled engine plane
(`/Engine/BasicShapes/Plane`) per water-surface chunk (any LOD), material
`UVoxelStrateDefinition::WaterMaterial`. Water Z: `bHasWater` + `WaterLevelRelative`
(Surface/tunnel params) → `StrateManager::GetWaterLevelWorldZForChunk`.

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
`EvaluateModifierSDF` (per voxel) **bounding-sphere-culls** each passage
(`FVoxelPassage::BoundCenter/BoundRadiusSq`) — perf-critical. The (0,0) surface entry is a
simple straight tube. Global passage settings were removed from `VoxelSettings`.

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
- **Passage cull** (§8.8) + **morphology two-region** (§8.4): both are per-voxel-cost critical.
- **Mesher density grid** (`GenerateMesh`): sample each grid point ONCE into a flat
  `(CHUNK_SIZE/Step + 1)³` array, then the cell loop reads its 8 corners from it. Adjacent
  cells share corners, so per-cell sampling calls `GetDensityAt` ~8× too often (262k→36k at
  LOD0). Output is bit-identical (`GetDensityAt` is pure in world coords). Don't refactor
  back to per-corner `GetDensity` calls inside the cell loop.
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
6. Reference from `VoxelSettings` (`StratePool`/`FixedStrates`). Global knobs there:
   `OriginSpineRadius`, `bOpenSurfaceEntry`, `InterStrateGapChunks`, view distances, LOD, carving budget.

### 8.13 New files this redesign
`Public/Private/VoxelContentManager.h/.cpp` (§8.5) · `Public/Private/VoxelAtmosphereManager.h/.cpp` (§8.6).
Everything else extended existing files: `VoxelStrateTypes.h` (archetype params, disturbance,
`FStratePassageConfig`, enums), `VoxelStrateDefinition.h`, `VoxelGenerator.h/.cpp` (archetype
density fns + spine/disturbance/param-cache), `VoxelStrateManager.h/.cpp` (per-archetype getters,
passages, gap, atmosphere Z helper), `VoxelWorld.h/.cpp` (managers, streaming perf, brush API,
editor buttons), `VoxelDiffLayer.h/.cpp` (brush shapes), `VoxelSettings.h`, `VoxelCaveMorphology.cpp`
(two-region determinism). Status: compiles & runs in-editor.
