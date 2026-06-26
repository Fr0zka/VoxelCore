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
| `GetStrateAtPosition` | 965 | Gameplay query → strate index. |
| `GetBiomeAtWorldLocation` | — | **BlueprintCallable** biome probe at a world point (undoes actor xf → voxel → `Generator::QueryBiomeAt`). Returns `FVoxelBiomeQuery` for BP debug ("what biome / how many decos under the cursor?"). |
| `CarveAtPosition` / `FillAtPosition` | 691 / 709 | Build `FVoxelModification` → DiffLayer → RemeshDirtyChunks. |
| `ClearAllModifications` | 726 | Clears diff layer, regenerates. |
| `ChangeSeed` | 740 | **Season reset**: new seed everywhere, clear diffs, bump season, reload. |
| `GetCurrentSeed` / `GetCurrentSeason` | 784 / 789 | Accessors. |
| `RemeshDirtyChunks` | 798 | Re-queue loaded chunks for async re-mesh (no visual pop). |

> **Game-thread profiling (Perf):** `AVoxelWorld::Tick` and its sub-steps are wrapped in `TRACE_CPUPROFILER_EVENT_SCOPE` — `VoxelForge_Tick / UpdateChunks / BuildDesiredTiles / CullTiles / SubmitTiles / ProcessPending / ProcessUnload / UpdateDecorations / UpdateWater`. Capture a `Count/Incl/Excl` Insights timer export and read the `Excl` column to see which step owns the per-frame cost (the actor tick shows as `BP_VoxelWorld_C` if subclassed in BP). `VoxelForge_GenerateMesh` + `VoxelForge_BuildStreams` are worker-side (off the frame): the RMC `FRealtimeMeshStreamSet` is now built on the gen worker (`BuildTileStreamSet`) and carried on `FChunkResult::Streams` (TSharedPtr), so `ApplyMeshToTile` is game-thread-cheap — just material/ceiling resolve + `CreateSectionGroup(MoveTemp)`. See ARCHITECTURE §8.10 "Worker-built StreamSet (T1.f)".

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
| `QueryBiomeAt` | — | Rich game-thread biome probe → `FVoxelBiomeQuery` (dominant/neighbour asset, relief/moisture, blend weight, dominant deco count). Diagnostic behind `AVoxelWorld::GetBiomeAtWorldLocation`. §8.14. |

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
border warp+blend / climate field freqs), `EBiomePreviewChannel` (preview-bake selector),
`FVoxelBiomeQuery` (BlueprintType result of `GetBiomeAtWorldLocation` — dominant/neighbour asset,
climate, blend weight, deco count), and plain runtime PODs `FBiomeResolved` / `FBiomeContext` /
`FBiomeSample` / `FChunkBiomeCache` (the box-validated per-chunk grid cache). See §8.14.

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

## 8. Architecture & design deep-dive  →  [ARCHITECTURE.md](ARCHITECTURE.md)

Moved out of the codemap to keep this file a fast navigation index. The archetypes, (0,0)
spine, disturbances, content/atmosphere, biomes, and the **performance invariants**
(`§8.10` — read before optimizing the hot path) now live in **[ARCHITECTURE.md](ARCHITECTURE.md)**.
All `§8.x` cross-references throughout this file point there.
