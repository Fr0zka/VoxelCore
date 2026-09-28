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
                         LoadTile → UE::Tasks::Launch  ──────────► background thread
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

### 3.0 Human-scale generation contract (2026-09-06)
`VoxelStrateTypes.h` is now authored against the confirmed player capsule, not against naked voxel
counts: one voxel is **0.25 m**, the capsule is **1.36 voxels / 0.34 m radius** and **7.04 voxels /
1.76 m tall**, and the default strate envelope is **8 chunks / 256 voxels / 64 m**. Core defaults are
therefore: room diameters **8-20 m**, origin hub **24 m**, room cells **32 m**, round tunnel bores
**3-4 m**, maze cells **16 m** with a **6.25 m nominal bore** (**3.125 m** radius; **3.0625 m**
worst-case usable floor after roughness), slab open span **22.4 m**, shaft diameters
**4-7 m**, island diameters **12-24 m**, and ordinary inter-strate bores **3-4 m**. These are
different derivations, not a global multiplier: passage widths follow the 2.5-4 m walking/fight
target, room/cell spacing preserves a rock web, and the 64 m vertical envelope reserves space for
tall rooms, surface sky, and vertical traversal.

The reachable roughness envelope is `roughness * VOXEL_NOISE_SCALE * 1.5`, or `roughness * 1.875`
voxels. The new defaults keep it below the relevant feature radius: **0.625** of the minimum
TunnelNetwork tunnel radius, **0.300** of Maze corridor radius, **0.469/0.625** of shaft/connector
radii, **0.234** of the minimum island radius (**0.484** including the 6-voxel island blend), and
**0.625/0.938** of slab column radius for floor/ceiling roughness. SurfaceWorld has no radial cave
feature, so its ratio is reported against the 80-voxel terrain-relief scale (**0.047**).

The complete generated `name | voxels | METRES | body/level meaning` audit is emitted by
`VoxelForge.Composer.Showcase` and includes optional transport fields. The official Unreal blockout
guidance ([Unreal level blockout guidance](https://dev.epicgames.com/documentation/en-us/unreal-engine/designer-01-project-setup-and-level-blockout-in-unreal-engine)) uses a player-sized reference and treats 2-3 m halls / 3-4 m heights as useful starting
guidelines; the [2010 ADA Standards](https://www.ada.gov/law-and-regs/design-standards/2010-stds/)
provide a 915 mm clear walking-width floor, not a game-space target; and [NPS Lehman Cave
dimensions](https://www.nps.gov/grba/learn/nature/lehman-caves-dimensions.htm) show why natural
chambers need a wide range rather than one universal volume ratio.

Maze topology is an origin-directed spanning tree, not independent edge percolation. Every
non-origin lattice node chooses one hashed parent from the non-zero axes that point toward
`(0,0,0)`; Manhattan distance therefore falls by exactly one on every parent step, so every node
reaches the root. Optional horizontal and vertical loop rolls are capped at `0.18 * BranchProbability`
and `0.10 * Verticality`; removing them cannot disconnect the tree. A voxel cell evaluates the
canonical lower-node `+X/+Y/+Z` edges in its local `{-1,0}³` (**2×2×2 = 8-node**) window, and each
edge checks both endpoints' parent choices. No wider collect region is needed, `WorldRadiusVoxels`
remains `0`, and lateral regions remain gated off.

### 3.1 Module & build
| File | Role |
|------|------|
| `../../VoxelForge.uplugin` | Plugin manifest. Runtime module `VoxelForge` plus the Editor-only `VoxelForgeEditor` commandlet module. Beta. |
| `VoxelForge.Build.cs` | Deps: Core, CoreUObject, Engine, **GameplayTags**, **RealtimeMeshComponent**. |
| `Tools/VoxelForgeTest.ps1` | The single guarded test entry point. `-Scenario canonical|owner|probe|perf|tests|surface-fall|crossing|gate-stress|parity|horizon|horizon-render|determinism`, optional `-Build`, `-Assets owner|default`, `-Cvars`, absolute `-Out`, `-Label`, `-HorizonLevel`, `-HorizonRenderDistanceChunks`, and `-SharedDDC`; stages the host/assets, isolates user/DDC/config state, serializes one Unreal launch at a time, enforces the 8 GiB/30-minute guards, and emits compact `result.json`/`summary.txt` plus full logs. Horizon traces report desired/loaded tiles, resident stream geometry, worker seconds, steady wall time, and peak private memory; `horizon-render` captures the owner view; `determinism` compares per-tile hashes across the default CRT path and `-voxel.CrtFma3=0`. |
| `Public/VoxelForgeModule.h` / `Private/VoxelForgeModule.cpp` | `FVoxelForgeModule` boilerplate (Startup/Shutdown just log). |
| `Source/VoxelForgeEditor/VoxelForgeEditor.Build.cs` / `Private/VoxelForgeExploreCommandlet.*` | Editor/commandlet-only `-run=VoxelForgeExplore`; owns bounded render/`VF_` walk/OBJ export orchestration and is not linked by Game/Shipping. |
| `Public/VoxelGeometryHash.h` | Header-only geometry digest shared by commandlet export and runtime tile-hash dump; hashes mesh counts and the vertex/normal/UV/color/triangle arrays with the historical CRC order. |
| `Public/VoxelStats.h` / `Private/VoxelStats.cpp` | `stat VoxelForge` DWORD counters for tile classification, skipping, meshing, operator-stack verdicts, and cave-bail diagnosis. The former ambiguous `Cave Bail Not Op Stack` is split into `Sole Slot`, `Boundary Tile`, `No Layout`, and late `Recheck` counters, so each increment names one guard/context. |

### 3.2 Foundational types — `Public/VoxelTypes.h` (no UClass, everyone includes it)
| Symbol | Line | Notes |
|--------|------|-------|
| `CHUNK_SIZE` (32) | 211 | Chunk dimensions. 64³ would cut draw calls but makes streaming too bursty; fps is fixed render-side instead. |
| `VOXEL_SIZE` (25.0f cm) | 213 | World scale. |
| `EVoxelFace` enum | 266 | 6 cube faces. |
| `WorldToChunkCoord` | 287 | World cm → chunk coord (handles negatives via floor). |
| `VoxelMath::IsFiniteFast` / `IsFinite` | 24 / 45 | Exact IEEE bit test (`voxel.FastIsFinite` A/B switch). |
| `VoxelMath::DetSinCos` / `DetSin` / `DetCos` | 63 / 181 / 189 | **The only sin/cos allowed where the world is decided** (2026-09-16, `10fd0b6`). Fixed double period reduction and float polynomial, no CRT: `FMath::Sin/Cos` are CRT `sinf/cosf`, whose FMA3 variant is picked by CPU. Pinned by `VoxelForge.Determinism.DetSinCos`; `-voxel.CrtFma3=0` flips the CRT path to prove it. |
| `SmoothStep01` | 306 | 3x²-2x³ — used everywhere for blends. |
| `VOXEL_NOISE_SCALE` (1.25f) | 313 | Rescales UE PerlinNoise3D to ~[-1,1]. |
| `EVoxelTileClass` enum (`Mixed`/`AllSolid`/`AllAir`) | 231 | T1.d verdict. Lives here so `VoxelDensityOp.h` can share it without a UCLASS dependency. A false `AllSolid`/`AllAir` is a HOLE; a false `Mixed` only costs CPU. |
| `FVoxelMeshData` struct | 365-391 | Mesher output (Vertices/Triangles/UVs/Normals/**Colors**). Plain C++, not USTRUCT. `Colors` = F6 material masks (R=dominant biome palette, G=slope, B=border blend weight, A=neighbour biome palette). §8.15. |

### 3.2b Density operator stack contract — `Public/VoxelDensityOp.h` (plain C++, no UHT)
**Production path behind a per-strate opt-in** — the stack is built/evaluated by `GetDensityAt` for
all eight archetypes when `bUseOperatorStack` is enabled, while the legacy `switch` remains the
reference path for opt-out strates. See [Docs/archive/OPSTACK-PLAN.md](Docs/archive/OPSTACK-PLAN.md) for the decomposition.

| Symbol | Notes |
|--------|-------|
| `EVoxelOpRole` | The four roles: `FieldSource` (what the field IS) · `Combiner` (how fields merge) · `DetailModifier` (today's `UVoxelTerrainOpDefinition`) · `StructuralPost` (spine→vertical seal→passage tube/landing floor→XY edge seal→diff, appended automatically, never author-omittable). |
| `EVoxelOpChannel` / `EVoxelOpChannelMask` | The explicit `FVoxelOpSample` channel set: `Density` and `Sdf` (plus `None`/`All` masks). Every density op declares the channels it reads and writes. |
| `EVoxelOpCombine` | `Replace`/`Union`(min)/`Subtract`(max)/`SmoothUnion`/`SmoothSubtract`/`Add`/`Mask`. Sign reminder: negative = solid, so "add solid" is `min`. |
| `EVoxelOpEffect` | `Identity`/`CarveOnly`/`FillOnly`/`Both`. Conservative: `Both` is always safe, the wrong one is a hole. |
| `FVoxelOpContext` | Chunk-constant inputs. Carries `LayoutVersion` plus the copied global `WorldRadiusVoxels`/`EdgeSealThickness`, so a new op cannot forget them. |
| `IVoxelDensityOp` | `ChannelReads` / `ChannelWrites` / `IsAdditive` plus `RequiredResources` / `ProvidedResources`, `PrepareChunk` / `Eval`, and intrinsic/state-aware `EffectOverBox` / `MaxCarveOverBox` / `MaxFillOverBox` / `PropagateSdfOverBox` / `ClassifyBox` / `IsXYPure`. Box-query methods are never called per voxel; channel/resource declarations are snapshotted during stack assembly. |
| `IVoxelDensityOp::ClassifyBox` | ⚠️ **not source-only.** Forcing ops (the vertical seal and XY edge seal inside their proven bands) overwrite the input entirely, which pure direction cannot express. |
| `FVoxelOpSample` | The state threaded through the stack: **two** channels, `Density` (INTERNAL convention, **positive = SOLID**, negated to MC once by the caller) and `Sdf` (standard SDF, negative = inside). ⚠️ `min()` therefore means opposite things on the two channels. |
| `FVoxelBoxHypotheses` + `FVoxelBoxSdfInterval` + `VF_ForceHypotheses` / `VF_FoldEffect` / `VF_FoldOp` | The fold that turns a stack into an `EVoxelTileClass`, carrying the SDF interval between independent writers and consumers. Unknown is biased toward `Mixed`; the mapping and soundness rule are written out in the header. |

### 3.2c Structural primitives — `Public/VoxelDensityPrimitives.h`
`VF_ApplyOriginSpine` · `VF_ApplyBoundarySeal` · `VF_ApplyPassageCarving` · `VF_ApplyXYEdgeSeal` —
the four world invariants every archetype appends, with landing/floor geometry joined by the
manager passage post, **moved here 2026-07-27** so the generator and
the operator stack share ONE copy. The XY helper is a radial smooth ramp in actor-space and is a
true no-op when `WorldRadiusVoxels == 0`; `VF_XYEdgeSealForcedMarginOverBox` is the shared sound
proof for `ClassifyBox` and T1.d's global shell skip. `VoxelGenerator.cpp` keeps same-named
`static FORCEINLINE` forwarders for `ApplyBoundarySeal` and `ApplyOriginSpine`; the passage and
edge helpers are called directly. **Convention: INTERNAL (positive = solid); `VF_ApplyXYEdgeSealMC` is the output wrapper.**

### 3.2d Operator stack — `Public/VoxelDensityOpStack.h` + `Private/VoxelDensityOpStack.cpp`
⚠️ **Feeds the game, behind a per-strate opt-in** (Phase 1 step 3). `GetDensityAt` builds the stack
in its per-chunk refetch block and evaluates it *instead of* the `switch` only when
`UVoxelStrateManager::UsesOperatorStackForChunk` says so — strate ticked `bUseOperatorStack` **and**
archetype in the ported list, which is now **all 8 of 8**: Maze, FlatPlain, CrystalChamber,
SurfaceWorld, VerticalShafts, FloatingIslands, TunnelNetwork, Underwater. A strate that has not
ticked the box still takes the `switch`, unchanged — the flag is the only thing that switches paths.
**`ClassifyTile` IS wired now**, for CAVE archetypes only and behind the same per-strate opt-in:
where it used to `return Mixed` without a call, it builds the strate's stack through the *same*
factory `GetDensityAt` uses (`VF_BuildOpStackForChunk`) and folds `ClassifyBox`. SurfaceWorld and
bedrock gaps keep their hand-written proofs — the exact-lattice column test is better than any box
bound. Guards, all failing to `Mixed`: one cave slot per tile, no mixed cave/surface/gap tile, the
opt-in true on *every* chunk the box touches, the params **bit-identical** across every chunk coord
the box touches (blended transition bands make one stack unable to represent the tile — `AUDIT §C2`),
a 27-chunk-coord cap, and the disturbances folded in by hand since they are applied after the stack.
Brute-forced end to end by `VoxelForge.OpStack.ClassifyTileSoundness`.
⛔ Never run both paths in one world. **Comparing them IS legitimate now** — the ~1 ULP residue of
AUDIT §C10 is gone since `FPSemantics = Precise`, and all eight equivalence tests compare bit for
bit. They are port-correctness oracles, not fidelity checks: the acceptance bar is §2.6.1 (same seed
⇒ same world on every peer), which does not require resembling the pre-refactor world.

| Symbol | Role | Notes |
|--------|------|-------|
| `FVoxelOpStack` | — | Ordered move-only list of `TUniquePtr` entries. `Add` snapshots channel metadata; `PrepareChunk` / `EvalInternal` / `EvalMC` / `ClassifyBox` (the fold lets a later forcing post overwrite a dead hypothesis). |
| `FVoxelOpStack::ValidateChannelOrder` | — | Assembly/diagnostic-only channel/resource DAG validator. It requires prior producers, rejects write-only clobbers and invalid additive declarations, and requires room/shaft/surface state providers before consumers. It permits the one root SDF identity fold used by Maze. `VoxelForge.OpStack.ChannelDAG` validates all 8 shipping stacks. |
| `FVoxelOpStack::AppendStructuralPost` | 4 | Appends spine → vertical seal → passage tube/landing floor → XY edge seal **in that fixed order**. An author cannot omit or reorder them. The diff layer is NOT here yet — it still lives in `GetDensityAt` after the MC negate, with disturbances. |
| `VoxelDensityOps::MakeConstantRockSource` | 1 | `Density = BaseDensity`. `ClassifyBox` → **AllSolid**, exact and free. Shared by TunnelNetwork, Maze, VerticalShafts and bedrock gaps. Class is `FConstantFieldSource` (one class, two factories). |
| `VoxelDensityOps::MakeConstantVoidSource` | 1 | The **same class, negated**: `Density = -BaseDensity`, and `ClassifyBox` → **AllAir** — the first source in the plugin that can prove it. FloatingIslands' root; that verdict is what makes a mostly-empty island strate skippable. |
| `VoxelDensityOps::MakeLatticeCorridorSource` | 1 | Maze corridors, SDF channel. Each canonical lower-node/axis edge is open when it is a parent edge in the origin-directed tree or passes a capped loop roll; both endpoints are hashed locally, so adjacent chunks cannot disagree. The source reports its own conservative SDF interval; a later converter supplies its own carve/fill response, so the source is independent of the chosen consumer. |
| `VoxelDensityOps::MakeSdfRoughnessMod` | 3 | Wall roughness in **SDF** space (Maze/Shafts/Islands variant). TunnelNetwork's density-space roughness is a **different op** — see OPSTACK-DECOMPOSITION §1. |
| `VoxelDensityOps::MakeSdfCarve` | 2 | SDF → density carve. The same six lines currently copied in three archetypes. Class is `FSdfConvertOp(Sign = -1)`. |
| `VoxelDensityOps::MakeSdfFill` | 2 | The same op with `Sign = +1` — FloatingIslands' `Density += Fill·Base·2`. ±1 multiplication is exact in IEEE-754, so the carve path is bit-for-bit unchanged by the generalisation. |
| `VoxelDensityOps::MakeSlabVoidSource` | 1 | Floor surface + ceiling surface → void field. **XY-pure** since §3.1, which is what gives it an **exact `ClassifyBox` with no sampling**: the proved FBM supremum `1.5` bounds both surfaces into known Z bands. Serves FlatPlain **and** CrystalChamber. |
| `VoxelDensityOps::MakeGridColumnMod` | 3 | Infinite-height cylinders on a world grid, 3×3 cell memo. Adds solid only ⇒ `FillOnly` when a column reaches the box, `Identity` otherwise — and that `Identity` is what lets the source's `AllAir` verdict survive. |
| `FShaftFieldSource` (internal) | 1 | VerticalShafts' memoised inner 3×3 shaft/capsule source. Each rebuild uses a direct-indexed wider halo (default 9×9 tree-emission / 15×15 roll) so every emitted shaft has its complete 5×5 parent window plus capsule reach; the nearest strictly more-central shaft in ±2 cells is the deterministic parent, with a deterministic lower-origin ±3 fallback and a direct `(0,0)` spine fallback. Thus every shaft has a provable path to the spine. Existing `VoxelHash::Pair`/`Spacing*1.6` links remain additive texture/loops. Tree and spine capsules use a radius strictly above `sup|FBM|=1.5`, and tree Z is chosen in a ledge-free band; the structural post still owns the column and the ledge modifier keeps its mathematical axis boundary open. The source publishes a conservative interval covering both shafts and connector capsules; it does not answer for the ledge or converter. |
| `VoxelDensityOps::BuildSlabStack` | — | 6 ops, **no branch on archetype**: FlatPlain and CrystalChamber differ only in defaults, exactly as `GetSlabDensity` already had it. 8 archetypes → 7. |
| `FSurfaceColumnSource` (internal) | 1 | The bridge between the two spaces: consumes the ground + sky-cap **height** stacks and produces density. `IsXYPure()` **false** — the heights are XY-pure, a distance to them never is. Owns a **six-box spatial LRU** of direct-indexed per-column cells, keyed by `PrepareChunk` on `(StrateBottomWorldZ, LayoutVersion, Seed, ParamsFingerprint)` so it is **shared down the whole vertical strate stack**, exactly like `GSurfColCache`. Six 81×81 boxes preserve hot columns across interleaved regions at roughly 0.79 MiB TLS before padding (more memory, fewer whole-cache recenter/recompute misses). Fractional XY remains direct-compute. `FSurfaceColumn` is deliberately trivial: all five outputs are written before the cache's `Computed` publish, so removing its default initialization removes a TLS constructor leaf without changing a value. |
| `VoxelDensityOps::BuildSurfaceStack` | — | SurfaceWorld, complete: column + overhang + 4 structural, plus biome blending when `PerBiomeParams` is non-empty. Takes ownership of an `IVoxelBiomeField`. |
| `VoxelDensityOps::BuildVerticalShaftStack` | — | 9 ops, and **three are Maze's reused unchanged** (`ConstantRock`, `SdfRoughness`, `SdfCarve`) with different tuning (freq 0.1 vs 0.12, window `rough+4` vs `R+rough+2`). Passes the runtime spine radius into `FShaftFieldSource`, which mirrors the generator's connector-only origin endpoint and roughness-safe spine capsule. The measured proof of `OPSTACK-PLAN §2.5`'s reuse claim. |
| `FRoomGraphSource` (internal) | 1 | TunnelNetwork's SDF spine. **CALLS `BuildChunkCache`/`EvaluateSDFCached` — does not transcribe them**: that is where §8.4's two-region discipline lives and a copy would fork it. Owns the cave warp (scope = this op alone; pits/chimneys read *unwarped* coords, which is why no FRAME op was needed). Its cache key adds a **params CRC + LayoutVersion**; the original lacked them until AUDIT §C2 was fixed (2026-07-28) and now carries them too. The source now publishes a conservative SDF interval through the box fold; it does not answer for the carve or detail stack. It builds the cache for the queried box into a *second* per-worker cache (never `FState::Cache`), and the interval uses the same cull/threshold disjunction as the per-voxel source: a primitive is harmless when it fails its cull or its own SDF stays ≥ `T+K`; `T = max(3·SDFBlendRadius, WormNetworkRange)`, with `−K` slack for the final `SmoothMin`. Rooms, tunnels, pits and chimneys are bounded in their respective coordinate frames; warp dilation uses the proved `sup|Perlin3D| ≤ 1.5` envelope. |
| `FWormFieldSource` (internal) | 1 | Fielded 3D-noise threshold carve, masked by the propagated SDF interval (reads `InOut.Sdf` *after* pits/chimneys). Its state-aware box response is `Identity` only when the preceding interval proves `Sdf ≥ WormNetworkRange`; otherwise it reports its own `CarveOnly` direction and bound. With no preceding SDF source it remains conservative, so it cannot inherit a verdict from an unrelated future writer. |
| `VoxelDensityOps::BuildTunnelNetworkStack` | — | **COMPLETE, 20 ops** — the biggest port in the plugin (~1080 lines), done in three stages: SDF spine (A) → the twelve detail modifiers of 4b–4h (B) → the per-room op override (C) → 4 structural posts. Serves **TunnelNetwork and Underwater** from one builder. Operator order is the original's, line for line. `FFloorBiasMod` is an independent additive term on the density accumulated before it; it is kept late because it does not undo the roughness delta. The requested 4b→4h fuse is not semantically justified. |
| `FCaveRoughnessMod` (internal) | 3 | STEP 4b, **density space** — a different op from `MakeSdfRoughnessMod`: two octave sets, optional domain warp, four noise types, an anti-fill clamp inside definite air, quadratic fade. ⚠️ **Reads STRATE params, not the per-room copy** — the original's shadow is declared *after* step 4b. Eleven of twelve modifiers read the room copy; this one does not. |
| `FCaveTerraceMod` (internal) | 3 | STEP 4c. The only modifier that **re-queries the SDF** (Z±1, through `FRoomGraphSource::ProbeSdfUnwarped`) for its horizontality gate — which is why the room source's cache is exposed at all. ⚠️ Those probes use unwarped X/Y and raw Z although the field was evaluated warped: transcribed as-is, see OPSTACK-PROGRESS. |
| `FLayerLineMod` / `FRibbingMod` (internal) | 3 | The same sine along Z: cubed and subtracted (grooves) vs quarter-phase-shifted, squared and added (ribs). `CarveOnly` / `FillOnly` — two of the few detail modifiers that keep a usable direction for the fold. |
| `FCaveOverhangMod` / `FCaveCliffMod` / `FScallopMod` (internal) | 3 | STEP 4c. ⚠️ The cliff's own comment promises a sampled vertical gradient; the **code** uses a Z-stretched Perlin as a proxy and samples nothing. Ported as written — fixing it would change the world. |
| `FCaveArchMod` / `FDomeMod` / `FPinchMod` / `FFloorBiasMod` (internal) | 3 | Room-relative: they read `FRoomGraphSource::GetNearestRoomIdx()` and the cached room. Their gate is `SDF < SDFBlendRadius`, **not** the shared `·3` one — they live in the cave's void, not its wall. |
| `FRoomColumnMod` (internal) | 3 | STEP 4d, and **not** `MakeGridColumnMod`: it walks `SDFCache.Columns`, pre-baked per room. ⚠️ It has **no strate parameter at all** — neither the bake nor the loop reads `FStrateGenerationParams::ColumnDensity`. Columns exist only through a `Column` terrain-op asset in the strate's pool, and the only way to prove they fired is to look at `SDFCache.Columns.Num()`. |
| `FRoomGraphSource::LocalParams()` | — | **The per-room op override** (DECOMPOSITION §2's "no clean home"). Strate params + the nearest room's `UVoxelTerrainOpDefinition::ApplyTo`, memoised once per voxel and read by eleven modifiers. One op owns the shared state, the rest read it — the same pattern as `FOverhangShelfMod` ← `FSurfaceColumnSource`. The detail consumers use the propagated interval for their shared near/far gate; unknown remains conservative, and the per-room parameter override is never used to claim an unproved box identity. |
| `FIslandBlobSource` (internal) | 1 | Hash-placed tapered flat-top blobs, `SmoothMin`'d, in a **domain-warped XY frame** (the warp stays inside the op — see the deviation note vs DECOMPOSITION §7). SDF channel only. The source publishes an interval covering the full authored radius envelope (`max(MinRadius, MaxRadius)`), the two-axis warp·**√2** envelope, top surface and SmoothMin dip; the fill converter makes its own decision. **No lower Z bound exists** — a hairline thread of matter hangs below each island down its axis, so only the TOP may reject. |
| `VoxelDensityOps::BuildFloatingIslandStack` | — | 8 ops, and **the stack runs backwards**: void source + fill instead of rock source + carve, using the *same* classes with the opposite sign. Only the blob source is new. Reuse by **inversion** — a stronger result than reuse by identity, since it says the abstract axis (the density sign) is the right one. |
| `VoxelDensityOps::BuildMazeStack` | — | The 8-op Maze stack (4 structural posts included). Its lattice source mirrors the origin-directed tree + capped loops and rebuilds the 2×2×2 lower-node window once per cell; the per-voxel loop only evaluates cached capsules. If this ever becomes one op, the refactor failed its own test (§2.5). Callers must skip it on a **degenerate strate** (top−bottom ≤ 0): `GetMazeDensity` early-outs to air there and the stack has no such early-out by design — `GetDensityAt` falls back to the `switch`. |

#### Lateral region parent (Tier 4d)

`VF_RollStrateRegionManifest` rolls a deterministic count of 1–3 regions and gives each region its
own native parameter vector and, for structure candidates, its own recipe. `VF_QueryStrateRegion`
assigns XY through jittered `VoxelHash::Cell` sites on a 256-voxel lattice; the pure query is keyed
by `(WorldXY, Seed, StrateIndex)` and the worker-side `FVoxelStrateRegionPartitionCache` prepares
the site window and integer query grid once per chunk. `FLateralRegionBlendOp` evaluates the primary
region core and, inside a 24-voxel band, the nearest different-region core, then lerps **density**
(not native parameters). The four structural posts are appended once to the parent after the
blend. Count==1 uses the ordinary one-stack path and is covered by native and recipe bit-identity
oracles. `ClassifyBox` proves a single-region/non-band box only from a conservative Lipschitz gap
bound; otherwise it folds every region and returns `Mixed` on any disagreement or unknown. The
runtime/editor override reads the immutable manifest per chunk; it never searches lattice seeds per
voxel. The dedicated `VoxelForge.Composer.LateralRegions` test reports cross-boundary brute-force
box samples, interior-vs-band timing, 16 opposite-region passage-mouth trials, and a saved preview.

### 3.2e Height-space operators — `Public/VoxelHeightOp.h` + `Private/VoxelHeightOpStack.cpp`
⚠️ **Feeds nothing yet** — built and exercised only by `VoxelForge.OpStack.SurfaceHeightEquivalence`.
**A SECOND op family, and it exists for a reason worth knowing:** SurfaceWorld's terrain ops (cliff /
terrace / layer lines / beach) read and write an **altitude**, not a density. They have no input Z
(they produce one), are XY-pure (once per column, not per voxel), and touch neither density nor SDF —
so they do not fit `IVoxelDensityOp` at all. Forcing them in would need a per-voxel channel for what
is a **column** property, or one opaque op (`OPSTACK-PLAN §2.5`'s failure mode). Same lesson as
`§0.1` one step further: some things are not another channel, they are another **space**.

| Symbol | Notes |
|--------|-------|
| `FVoxelHeightSample` | Two channels: `Height` (voxel Z) + `Relief` (the original's `M`). Relief is produced by the structural source and consumed by the terrace gate — threading it beats resampling it. |
| `IVoxelHeightOp` | `Eval(X, Y, FVoxelHeightSample&)`. No `IsXYPure` (XY-purity is structural here — there is no Z to wrongly put in), no `PrepareChunk` (already per-column). `MaxDisplacement()` is the conservative vertical bound for a future heightfield `ClassifyBox`. |
| `FVoxelHeightStack` | Move-only, like `FVoxelOpStack`. `EvalHeight` / `EvalSample` / `MaxTotalDisplacement`. |
| `VoxelHeightOps::MakeStructuralHeightSource` | Continents + mountains + detail under a warp frame. Hands back a **non-owning pointer** so the cliff mod can resample it. |
| `VoxelHeightOps::MakeCliffHeightMod` | Slope-gated steepening; 4 resamples of the **structural** field (never the modified height — that would feed back). |
| `VoxelHeightOps::MakeTerraceHeightMod` | Relief-gated plateaus. The `* Relief` is the original's `* M`. |
| `VoxelHeightOps::MakeLayerLineHeightMod` / `MakeBeachHeightMod` | Sine bands; flatten toward the water line. Both have exact `MaxDisplacement`. |
| `VoxelHeightOps::BuildSurfaceHeightStack` | 5 ops in `ComputeSurfaceTerrainZ`'s order — structural → cliff → terrace → layer lines → beach. **Order is not negotiable.** |

### 3.3 Chunk identity
Tile identity lives in `FVoxelTileKey` (VoxelTypes.h:326).

### 3.4 Settings — `Public/VoxelSettings.h`
`UVoxelSettings : UPrimaryDataAsset` — the single tuning asset assigned on `AVoxelWorld`.
| Group | Fields (line) |
|-------|---------------|
| Streaming | `ViewDistanceXY=16`, `ViewDistanceUp/Down=5`, `MaxConcurrentTasks=16`, `MaxMeshAppliesPerFrame=4` (defaults — actual values live on the data asset) |
| Clipmap | `ClipRadius`, `MaxClipLevel=5`, `FullResClipLevels`, `CoarseTileCells`, `RenderDistanceChunks=768` (measured ship defaults; custom horizontal reach: the outermost shell keeps generating until it covers this many chunks; 0 = off), skirts, `LODOctaveDrop` (T2.b octave drop on coarse tiles — 0 = off/byte-identical). Existing serialized data assets retain their authored values until deliberately migrated. |
| World bounds | `WorldRadiusVoxels=8192` and `EdgeSealThickness=64` (actor-space XY radial shell; radius 0 = true legacy no-op) |
| Lighting | `bEnableDensityVolume` + DensityVolume* tunables (§3.11 density clipmap / mini-sun shadows) |
| Rendering | `VoxelMaterial` (377) |
| Strates | Optional cooked `Season`; effective seed/spine/radius/season accessors read it when assigned. Otherwise `Seed`, `CurrentSeason=1`, `StratePool`, `FixedStrates`, and `TotalStrates=10` retain the authored path. |
| Carving budget | `MaxModifications=0` (449), `MaxBrushRadius=15` (454), `MaxTotalVolume=0` (459). 0 = unlimited. |

### 3.5 World orchestrator — `Public/VoxelWorld.h` + `Private/VoxelWorld.cpp`
`AVoxelWorld : AActor` — owns everything, drives streaming. Also `FChunkResult` struct
(VoxelWorld.h:99) = async task payload (coord, chunk, meshdata, LOD, **Epoch**, request timing,
worker-built stream geometry counts/bytes, desired-set epoch and obsolete/cancellation state).

**Owned objects (UPROPERTY):** `Settings`, `Generator`, `Mesher`, `StrateManager`,
`DiffLayer`, `ContentManager`, `AtmosphereManager`, `DensityVolume` (VoxelWorld.h:188-228).
**Storage:** `LoadedTiles` (TSet), `TileComponents` map + `TileComponentPool` (VoxelWorld.h:260-308),
the four MPSC result queues `CriticalProcessQueue` / `EditedProcessQueue` / `NearProcessQueue` /
`ProcessQueue`, and `PendingTiles` (TSet) (VoxelWorld.h:908-912).
**Async state:** `bShuttingDown`, `ActiveTaskCount` (atomics), `GenerationEpoch`, and the
game-thread `PendingTileCancellation` map whose per-request atomic flags are read-only on workers.

**⭐ Espace ACTEUR / ACTOR SPACE (2026-08-17).** Le champ de densite est defini en espace **acteur** :
`(0,0)` est l origine de l acteur `AVoxelWorld`, pas celle du monde Unreal. `WorldToLocalCm` /
`WorldToLocalVoxel` / `LocalVoxelToWorld` sont la **SEULE frontiere autorisee** entre coordonnees
Unreal et coordonnees voxel. **Invariant verifiable par grep : aucun `/ VOXEL_SIZE` applique a un
parametre nomme `World*` hors de ces trois fonctions.** Avant cette date le plugin etait *a moitie*
actor-relative (biome/deco/eau/volume/debug convertissaient ; le **centre de streaming**, les **ancres**,
les **6 entrees carve/fill**, `GetStrateAtPosition` et `UVoxelAtmosphereManager::UpdateForPlayer` ne le
faisaient pas) — ce qui ne marchait que tant que l acteur restait a l origine.
⚠️ **TRANSLATION SEULEMENT** : tuiles, `ClassifyBox`, fenetre clipmap, culling et distance de streaming
supposent tous des boites **alignees sur les axes du MONDE**. Une rotation ou une echelle non unitaire
casse cette arithmetique *structurellement* — `BeginPlay` loggue une **Error** si on en met une.


| Method | .cpp line | Role |
|--------|-----------|------|
| `AVoxelWorld()` ctor | 176 | Enables Tick. |
| `RegenerateAllChunks()` | 530 | Bumps epoch, unloads all → Tick reloads. CallInEditor button. |
| `ValidateDeterminism()` | — | **F2 CallInEditor button (PIE)**: re-samples boundary points under left- vs right-chunk cache warm-ups + a same-alignment repeat; any non-zero delta = window-invariance regression (§8.4). Run after every "bit-identical" hot-path refactor. |
| `GetMaxConcurrentTasks()` | — | T2.d — asset `MaxConcurrentTasks` capped to logical cores − 2 (all three budget checks use it). |
| `PostEditChangeProperty` | 1000 | Editor live-edit hook. |
| `OnObjectModifiedInEditor` | 1022 | Regenerates when a strate asset is edited (if `bLiveEditStrates`). |
| `EndPlay` | 1118 | Sets `bShuttingDown`, **waits for `ActiveTaskCount`→0**, unbinds delegate. |
| `BeginPlay` | 1429 | Constructs Generator/Mesher/StrateManager/DiffLayer, wires services, seeds. |
| `Tick` | 4395 | `UpdateChunksAroundPosition(player, heading)` + `ProcessPendingChunks()`; the clean LOD0 latency summary is flushed at `EndPlay`. |
| `GetPlayerPosition` | 4831 | Pawn position or zero. |
| `ProcessPendingChunks` | 5856 | Drains ProcessQueue under per-frame budget; reads the per-request cancellation token before removing its map entry, drops obsolete/aborted results, then applies each live result via `ApplyTileResult`. |
| `ApplyTileResult` | — | **Shared game-thread apply** for one `FChunkResult` (async drain + sync carve): discards stale generation epochs and keys no longer desired, marks loaded, ingests capture, EMPTY releases the tile's existing component (a re-gen can flip content→empty on band change — old geometry must not linger), else `ApplyMeshToTile`. Returns true iff a visible mesh uploaded (counts the budget). Doesn't touch `PendingTiles` (caller's). |
| `FVoxelStackSampler` / `FScopedVoxelStackRegistration` (`VoxelStackSampler.h/.cpp`) | — | **Diagnostic raw stack sampler, off by default.** A sampler thread captures registered generation workers' stacks (`CaptureThreadStackBackTrace`) every N µs and tags each with the tile's LOD. It writes bounded raw PCs to CSV plus a symbolization-free summary under `Saved/`; symbol resolution is deferred until after process exit so the measured process never initializes/calls DbgHelp for this diagnostic. Lifecycle teardown is serialized and registration TLS is session-tagged. Registration is per tile TASK in `LoadTile` (and the export's tasks), never per sample; disabled costs one relaxed atomic load. Game `-voxel.SampleStacks=<us>`, commandlet `-samplestacks[=us]`. |
| Room-cache performance (2026-09-13): `VF_FindPlayerFitPointForRoomMemoized` / `GPlayerFitMemo` (`VoxelCaveMorphology.cpp`), `voxel.TileCacheWindow` + `VoxelGenLOD::TileOriginVoxels/TileCellsPerAxis` (`VoxelGenerator.h`), `FChunkSDFSpatialIndex` | — | **Player-fit memo:** exact 97-word key, process-wide, `voxel.PlayerFitMemo`. **Tile cache window:** the room cache covers the requesting tile's footprint plus a fixed collect margin. It is set via `TGuardValue` in `GenerateMesh`/classify, and is window-invariant by construction (the margin does not grow with the tile). **LOD-gated:** fused path at LOD ≥ `voxel.TileCacheWindowMinLOD` (3), op-stack at LOD ≥ `voxel.TileCacheWindowOpStackMinLOD` (4); lower LODs keep the 4-chunk worker window, because the tile window slowed LOD0 evaluation ~6%. Strates whose authored terrain ops exceed the proven 256-voxel extent (or are malformed/unloaded) fall back to the legacy window (`VF_ComputeTerrainOpEnvelope`). `voxel.SpatialIndex` is -1 auto (off at fused LOD0, on elsewhere), 0 or 1. **Spatial index:** ordered XY buckets over cache primitives, exact rejection. The `static_assert` in `VoxelStrateTypes.h` ties `VF_STRATE_PARAM_FIELDS` to `FStrateGenerationParams` (83 fields + 8 B padding). |
| `GenerateTileResult` | — | **Shared worker-side gen** for one tile (async `LoadTile` task + sync `SyncRemeshTile`): [ClassifyTile (T1.d) + exact validation, only when `voxel.OuterClassifierMode=1`; **default 0 since 2026-09-13**] → `GenerateMesh` → `BuildTileStreamSet`; records vertex count and resident stream allocation bytes for the horizon trace. Reads Generator/Mesher only → safe on a worker or the game thread; fills `FChunkResult`, no enqueue. |
| `RecordTileHash` / `WriteTileHashDump` | — | Headless `-voxel.TileHashDump=<absolute path>` parity instrument. Records the exact `FVoxelMeshData` used by the streaming result, including empty/classified tiles, then writes sorted absolute tile keys, LOD step/cells, content-band limits, settings digest context, and the shared `VoxelGeometryHash` at `EndPlay`. |
| `SyncRemeshTile` | — | **INSTANT DIG**: level-0 same-frame re-mesh on the game thread (`GenerateTileResult` + `ApplyTileResult` inline, Cells=CHUNK_SIZE/Step=1 + strate band, no capture). Used for the tile under the brush centre so a carve is visible THIS frame; one full-res gen on the game thread. |
| `UpdateChunksAroundPosition` | 6870 | Builds desired set, cancels pending keys that left it, sorts from the **true pawn position**, submits the occupied/support LOD0 tile and the next tile along the pawn heading as an absolute `BackgroundHigh` prefix, then loads/unloads and handles LOD changes. **Delta cull**: `BuildDesiredTiles` returns the LEAVERS (stamped `DesiredStamped` map, one sweep) — only those + `TransitionHold` are considered per crossing, not every loaded tile. `BuildDesiredTiles` also applies `RenderDistanceChunks`: the outermost shell widens to cover the distance as level-MaxClipLevel MC tiles (`VF_OuterShell` shared with `IsTileInClipRange` so the cull sees the same horizon), dz pre-clamped to the vertical band. **§9.3 anchors:** also prunes dead `StreamingAnchors` + detects their chunk crossings → rebuilds the desired set when an anchor moves/(un)registers (`bAnchorsMoved`/`bForceDesiredRebuild`), same cadence as player movement. §8.10. |
| `AdvanceHeadlessCollisionGateStressTest` / `ObserveHeadlessCollisionGateStressTest` / `MaybeFinishHeadlessCollisionGateStressTest` | — | Explicit `-voxel.TestCollisionGateStress=1` harness only. The `GetDensityAt` column at capsule feet defines the local air/solid surface; each frame records actor position, velocity, direct foot tile and gate support readiness, gate state, signed feet clearance, and a ≤0.5-voxel sweep between positions. PASS requires no measured clearance below −5 cm; support submission/readiness, travel, and holds are diagnostics. Result logs include the terminal density column and the first crossing below spawn height. |
| `BuildDesiredTiles` | 6482 | Builds `DesiredSorted`+`DesiredStamped` (player clipmap shells) then `AddAnchorDesiredTiles()` before the leaver sweep; it orders from the true local pawn position and builds `CriticalDesiredTiles` for the occupied/support and heading tiles. Also rebuilds `DesiredTransitionDescendants` (ancestor → desired descendants) for the reconcile pass. |
| `VoxelClipmapDesiredTiles::Build` / `AlignedRingBounds` (`Private/VoxelClipmapDesiredTiles.h`) | — | The pure clipmap selector. **Every finer ring snaps outward to a child-pair boundary** (`AlignedRingBounds`, alignment 2; the outermost ring needs none), so a coarser tile is wholly inside or wholly outside the finer ring: **no two desired tiles of different levels overlap** (an unaligned 7-wide odd ring leaves one half-covered coarse tile per axis side, drawn over the fine one: 635 pairs at the owner config). Cost: LOD0 ring 343 → 512 tiles. Guarded by `VoxelForge.Streaming.ClipmapDesiredTilesNoOverlapCoverage`. Anchor tiles appended later are outside this guarantee. |
| `ReconcileReadyTransitionVisibility` | — | Visibility-only pass while streaming is unsettled: hides a retired tile once all desired tiles over its footprint are loaded, reveals new tiles once no visible retired tile overlaps them. **Indexed** (dyadic ancestors + `DesiredTransitionDescendants`, visible retired tiles collected once per pass): moving p95 32 ms → 0.31 ms. `voxel.MeasureReadyTransitionVisibility=1` logs per-frame samples. |
| `RequestCollisionGateSupportTile` / `UpdatePawnCollisionGate` | — | The gate submits the pawn's level-0 support tile **immediately** as `CollisionCritical`, even before the first desired set exists (the desired rebuild then adopts the same pending key). The gate holds downward Z only, with **no timeout** (a 2 s release would fire at every startup, while the game thread applies nothing for ~2.2 s). |
| `AddAnchorDesiredTiles` | — | **§9.3 multi-anchor:** folds each `FVoxelStreamingAnchor`'s thin level-0 box (`XYRadiusChunks`/`ZBelowChunks`/`ZAboveChunks`) into the SAME desired set (deduped by stamp) so AI/remote players keep collision loaded around them; delta cull releases them on move/unregister. **§9.4:** a tile only a CollisionOnly anchor wants (clipmap didn't stamp it) → `CollisionOnlyTiles` → hidden at apply. Zero cost when no anchors. |
| `ReconcileAnchorTileVisibility` | — | **§9.4:** after each rebuild, toggle `SetVisibility` on ALREADY-LOADED tiles that flipped render↔collision-only (diff `CollisionOnlyTiles` vs prev — bounded, no O(loaded) scan; unhide only if still desired). No-op without CollisionOnly anchors. |
| `RegisterStreamingAnchor` / `UnregisterStreamingAnchor` | — | **BlueprintCallable §9.3:** add/remove an actor as a streaming anchor (`EVoxelAnchorPolicy` CollisionOnly/FullVisual + XY/ZBelow/ZAbove box). Idempotent; forces a rebuild next Tick. §9.4: CollisionOnly tiles cook collision but are hidden (no draw/VSM) unless the player clipmap wants them too. |
| `LoadTile` | 7291 | Budget check → `UE::Tasks::Launch` background gen+mesh; RAII task guard. Worker runs `Generator->ClassifyTile` first (T1.d): AllSolid/AllAir ⇒ skip `GenerateMesh`, tile stays empty (capture tiles always generate). It carries `GenerationEpoch`/desired epoch and checks `bShuttingDown`, generation pause, and its read-only cancellation token between expensive stages; obsolete work returns through `ProcessQueue`. STRATE CONTENT CUT: tiles ≥ `StrateContentCutMinLevel` pass the player-strate band (`MeshBandChunkLo/Hi` → voxels) to `GenerateMesh` + stamp it on `FChunkResult::BandChunkLo/Hi`; band change re-queues via `BandRemeshQueue` (see `UpdateChunksAroundPosition`). TOO-COARSE SKIP: if one cell is taller than the band (`Step > band height` — level ≥7 territory) the tile is enqueued EMPTY without launching a task (cell-granular cut could only render garbage). §8.10. |
| `UnloadTile` | — | Clears tile state; marks any matching generation request obsolete without removing its pending slot, so a late result cannot clear a replacement request; the component is PARKED in the pool (T2.c), not destroyed. |
| `ApplyMeshToTile` | — | Upload geometry. One component per tile (clipmap keeps count low; supersedes the old region batching); worker-built streams (T1.f) → `CreateSectionGroup(MoveTemp)`. Reuses the component's existing `URealtimeMesh` (no per-apply mesh alloc). **F17: two polygroups** (0 ground / 1 sky-cap, per-triangle class from the mesher) → RMC auto-section per non-empty group; slot 0 = override/default material, slot 1 = `CeilingMaterial` (fallback ground); config gated by `FChunkResult::bHasGroundTris/bHasCeilingTris`. Takes `FChunkResult&`; strate lookups clamp Z into `Result.BandChunkLo/Hi` (strate content cut) — ground at clamped bottom chunk, cap at clamped top (mid = gap fallback). Collision level-0 only (T1.c) both groups; shadow per SECTION: ground casts at level≤1, cap never. **§9.4:** `SetVisibility(false)` when the tile is in `CollisionOnlyTiles` (anchor-only, hidden — collision still cooks). §8.10 + ARCHITECTURE SurfaceWorld row. |
| `AcquireTileComponent` / `ReleaseTileComponent` | — | **T2.c component pool** (`TileComponentPool`, bounded): park on unload (geometry+collision stripped, hidden, stays registered), pop on apply — no `NewObject`/`RegisterComponent`/GC churn during travel & regen. §8.10. |
| `GetStrateAtPosition` | 8351 | Gameplay query → strate index. |
| `GetBiomeAtWorldLocation` | — | **BlueprintCallable** biome probe at a world point (undoes actor xf → voxel → `Generator::QueryBiomeAt`). Returns `FVoxelBiomeQuery` for BP debug ("what biome / how many decos under the cursor?"). |
| `GetVoxelSurfaceHeightAt` | 8385 | **BlueprintCallable** ground finder (F7 bridge): world XY → terrain + sky-cap world-Z via `Generator::GetSurfaceHeightAt`, NO trace/collision, deterministic, available before the area meshes → self-arranging prefab/ruin BPs snap their parts to the real ground. False (outs=input Z) on non-SurfaceWorld strates; ignores passage/spine carving. |
| `CarveAtPosition` / `FillAtPosition` | 8413 / 8422 | Build `FVoxelModification` → `ApplyModification`. |
| `ApplyModification` | 8431 | Single funnel for all brushes: DiffLayer → **sync-remesh the brush-centre level-0 tile** (`SyncRemeshTile`, instant hole; skipped if that tile is mid-gen — would race a stale in-flight result) → `RemeshDirtyChunks(..., ExcludeTile)` for the neighbours → `RemoveDecorationsInSphere`. |
| `ClearAllModifications` | 8696 | Clears diff layer, regenerates. |
| `ChangeSeed` | 8710 | **Legacy season reset**: new seed everywhere, clear diffs, bump season, reload. Rejected while a cooked season owns the seed. |
| `GetCurrentSeed` / `GetCurrentSeason` / `GetCurrentSeasonContentHash` | — | Effective cooked-or-legacy identity; the hash is the join-time stale-season comparison value. |
| `RemeshDirtyChunks` | 8801 | Queue loaded level-0 dirty tiles onto `DirtyRemeshQueue` (async re-mesh, no pop) + `MarkDirtyVoxelBox` the volume. Optional `ExcludeTile` = the sync'd centre. Drained FIRST in the submit loop at **BackgroundHigh** (ahead of streaming/band) so a dig never waits behind streaming; in-flight tiles stay QUEUED (not dropped) so a stale pre-carve result is corrected once it lands — fixes "hole shows up a beat late / not until I move". |

> **Game-thread profiling (Perf):** `AVoxelWorld::Tick` and its sub-steps are wrapped in `TRACE_CPUPROFILER_EVENT_SCOPE` — `VoxelForge_Tick / UpdateChunks / BuildDesiredTiles / CullTiles / SubmitTiles / ProcessPending / ProcessUnload / UpdateDecorations / UpdateWater`. Capture a `Count/Incl/Excl` Insights timer export and read the `Excl` column to see which step owns the per-frame cost (the actor tick shows as `BP_VoxelWorld_C` if subclassed in BP). `VoxelForge_ClassifyTile` (T1.d) / `VoxelForge_GenerateMesh` + `VoxelForge_BuildStreams` are worker-side (off the frame): the RMC `FRealtimeMeshStreamSet` is now built on the gen worker (`BuildTileStreamSet`) and carried on `FChunkResult::Streams` (TSharedPtr), so `ApplyMeshToTile` is game-thread-cheap — just material/ceiling resolve + `CreateSectionGroup(MoveTemp)`. See ARCHITECTURE §8.10 "Worker-built StreamSet (T1.f)".

### 3.6 Density generator — `Public/VoxelGenerator.h` + `Private/VoxelGenerator.cpp`
`UVoxelGenerator : UObject` — lightweight; holds `Seed`, a process-unique
`DensityCacheOwnerId`, and injected services `StrateManager` + `DiffLayer` (both nullable).
This is **where terrain shape lives.**

| Symbol | .cpp line | Role |
|--------|-----------|------|
| `UVoxelGenerator` / `DensityCacheOwnerId` | — | Constructor allocates a process-unique integer identity (relaxed atomic, once per object). `GetDensityAt` includes it in the `CP_*` thread-local key, preventing a worker from serving another generator/world's params, biome context, `CP_UseOpStack`, or stack when `(ChunkCoord, LayoutVersion)` happens to match. Hot-path cost: one `uint64` compare per voxel. Scope is deliberately only the proved `CP_*` path. |
| `FractalNoise3D` (static) | 932 | fBM (layered Perlin). |
| `RidgedNoise3D` (static) | 950 | Ridged multifractal — craggy. |
| `CellularNoise3D` (static) | 977 | Worley/cellular — grotto/scallop. |
| `ApplyBoundarySeal` (static) | 997 | Solidifies strate top/bottom shells. |
| `InitializeSettings` | 2694 | Copies the effective seed, spine radius, and global XY world-bound settings from `UVoxelSettings` (the season owns all three when assigned); radius 0 preserves the unbounded field. |
| **`GetDensityAt`** | 2715 | **Entry point.** Picks strate + generator type, materialises a cooked-season recipe on the existing per-chunk refetch when present, dispatches, then adds disturbances/diff. Its `CP_*` state is keyed by `(DensityCacheOwnerId, ChunkCoord, LayoutVersion)`; workers copy immutable recipe/vector data and never read editor composer state. |
| **`GetDensityWithParams`** | 3851 | TunnelNetwork pipeline (~1000 lines). See §4. ⚠️ Takes **required** `ParamsFingerprint` + `LayoutVersion` since the AUDIT §C2 fix (2026-07-28) — they go into the SDF cache key so a chunk can no longer be evaluated against a neighbour's rooms. Callers compute the CRC **once per chunk** (`CP_TunnelFP`), never per voxel. The optional `voxel.WormBlockSkip` path (default 0) uses `VF_TryGetWormBlockSkip` + a worker-local 4³ lattice-block cache to prove `|N1| >= WormThreshold` and remove both worm noise calls only; it never changes `NetworkMask` or the fallback field path. |
| **`GetSlabDensity`** | 5188 | FlatPlain/CrystalChamber pipeline. See §4.2. |
| `SampleSurfaceStructuralZ` | — | **F20:** the RAW SurfaceWorld heightfield (continents+mountains+detail), BEFORE any terrain op; returns terrain Z + relief M. Cliff re-samples it at an XY offset for a cheap analytic slope. |
| `ComputeSurfaceTerrainZ` / `GetSurfaceDensity` | — | SurfaceWorld heightfield → terrain Z, then density; biome **output-blend** lerps dominant/neighbour heights (`ParamsD`/`ParamsN`/weight). **F20 surface ops** (`FSurfaceGenerationParams`, biome-selected + slope/relief-conditioned, all default off): Cliff (slope-gated STEEPENING — push height from local mean where steep ⇒ sheer walls; 4 structural resamples only when on), Terrace (relief-gated + `TerraceHardness`), LayerLines (sedimentary shelves) — pure per-column height REMAPS applied here so the single height oracle stays consistent (MC/ClassifyTile/deco/BP bridge). **Phase 2 OVERHANG** (volumetric — real jutting shelves): in `SurfaceDensityFromColumn`, for AIR voxels in a window `(TerrainZ, TerrainZ+OverhangHeight]` above a steep slope, the heightfield is re-sampled UPHILL (toward the cliff) by a reach that GROWS with height (tiny low ⇒ air over the void, full high ⇒ borrows the far cliff rock) and unioned in ⇒ a shelf attached to the cliff, tapering out over the void with air beneath (the sketch). Per-column `OverhangAmp`(=strength·slope-gate) + unit uphill `(DirX,DirY)` resolved once in `ComputeSurfaceColumn` (gradient sampled at the REACH scale so a spot over the void can see the cliff), cached on `FSurfaceColumn`. Genuine 3D (per-voxel structural re-eval, gated to steep overhang columns). Off ⇒ byte-identical. The field-preserving LOD0 pass removes only needless temporaries/default construction; every output remains bit-identical. §8.14. |
| `VF_BuildOpStackForChunk` (file-static) | — | **The archetype → stack mapping, written down once.** `GetDensityAt` and `ClassifyTile` both call it; params are passed in, never fetched here. A second copy would be the worst bug available in this file — a tile skipped on the verdict of a stack that is not the one producing its density is a hole. Returns false (⇒ caller falls back to the `switch`) for an unported archetype, missing params, or a **degenerate strate**, since five archetype functions early-out to air there and the stack deliberately has no such early-out. `Refs.Surface == nullptr` makes it refuse SurfaceWorld, which is how `ClassifyTile` keeps its own exact-lattice proof. |
| `ClassifyTile` | — | **T1.d trivial-tile reject. ⛔ Not called by streaming by default since 2026-09-13** (`voxel.OuterClassifierMode=0`; net-negative in game, see ARCHITECTURE §8.10). Cooked/editor recipes now build the exact recipe stack and call its proved `ClassifyBox`; a tile crossing a slot/gap, any diff, or any disturbance returns `Mixed`. This replaces the old global `HasComposerRecipeOverride()` force-to-Mixed. Native cave/surface/gap proofs and all §8.10 caches remain unchanged. |
| `SampleRelief` / `SampleMoisture` | — | Climate fields (pure XY, [0,1]). Relief = shared source of truth for the relief map M. §8.14. |
| `SampleBiomeAt` | — | Warped-Voronoi + climate biome query (dominant + neighbour + weight). Reference used by the preview bake + `GetDominantBiomeAt`. §8.14. |
| `ResolveBiomeSampleAt` / `RebuildBiomeGrid` | — | Hot-path biome resolve (FBiomeSample) via a box-validated per-chunk cell-grid cache. Bit-identical to `SampleBiomeAt`. §8.14, §8.10. |
| `GetDominantBiomeAt` | — | Game-thread query → dominant biome ASSET (content/atmosphere). §8.14. |
| `QueryBiomeAt` | — | Rich game-thread biome probe → `FVoxelBiomeQuery` (dominant/neighbour asset, relief/moisture, blend weight, dominant deco count). Diagnostic behind `AVoxelWorld::GetBiomeAtWorldLocation`. §8.14. |
| `EvaluateTerrainConditions` | 7549 | **F7 aware placement:** AND-evaluate an entry's `FTerrainCondition[]` (relief/moisture/biome-border) at a candidate voxel XY. Empty = true (zero cost). Pure query (SampleRelief/SampleMoisture/SampleBiomeAt) → worker-safe + game-thread; caller passes the strate's `FBiomeContext` (freq/contrast + Voronoi map). Consumed by deco `BuildCellSpawns` + `SpawnLandmarkInstance`; shared core of the future quest FindFeature locator. |

> **Per-voxel hot-path memos (perf pass 2, all bit-identical — same hashes/math, hoisted per
> chunk/cell):** `GetDensityAt` uses the DiffLayer snapshot cache (§3.9); `GetDensityWithParams`
> memoizes the strate index per (chunkZ, `GetLayoutVersion()`); `thread_local` per-cell lattice
> bakes cover slab columns (`GetSlabDensity` step 4), maze open edges, vertical shafts +
> cross-connectors, floating-island constants, and the disturbance chasms/bridges/ridges; worm
> tunnels short-circuit the 2nd Perlin when N1 ≥ WormThreshold (N2 ≥ 0 ⇒ can't carve). With
> `voxel.WormBlockSkip=1`, a worker-local cache proves whole 4³ lattice blocks only when the
> actual 3D Perlin bound (`L = 8.118988160`, including `VOXEL_NOISE_SCALE`) plus a `1e-3`
> output-rounding margin clears the threshold; failed proofs use the original per-sample path.
> Room
> shapes are pre-baked in `FCachedRoom` (§3.7). **T2.b:** per-voxel fractal call sites take their
> octave count through `VoxelGenLOD::Eff(N)` (VoxelGenerator.h) — a `thread_local` bias set per
> tile by the mesher drops tail octaves on coarse tiles (opt-in `LODOctaveDrop`, default 0 = off);
> XY-field noise (heightfield/ceiling/relief/moisture) deliberately stays un-biased. §8.10.

### 3.6b Strate measurement — `Public/VoxelStrateMeasure.h` + `Private/VoxelStrateMeasure.cpp`
Headless, read-only Tier 2 measurement pass. `VF_MeasureStrate` samples one strate between a
boundary-seal-derived (or explicitly overridden, including explicit zero) interior margin into a bounded grid.
When both `CoverPointA` and `CoverPointB` are set, the XY window is their AABB expanded by
`CoverMarginVoxels`; otherwise it is the legacy square centered on `CenterXY` with
`RadiusInVoxels`. Origin-rooted callers set `bIncludeOriginInCoverWindow` to expand that fitted
AABB to include `(0,0)` before the `MaxCells` check; an over-cap route window is refused rather
than cropped back to a mouth-only box.
It performs one deterministic 6-connected air flood fill, and derives fractions, components,
walkability, feature scale, and clearance from that grid. It then performs one bounded 4-neighbour
flood fill over the projected XY columns that contain a walkable surface; this is a floor-continuity
indicator, not another source-density sample or a 3D route proof. Metrics report the resolved margin and
the inclusive/exclusive voxel Z window used, the exact sampled dimensions/bounds, and the largest
component's deterministic lowest-cell representative point, cell count, and count of components
holding at least 1% of the air. The flood fill also retains component cell counts in discovery order,
so diagnostics can classify small non-largest components without a second source sample.
Callers may optionally receive that exact polarity grid and scalar values as `FVoxelStrateSampleGrid`; the
editor-only composer preview consumes this hand-off rather than sampling the world again. The filled view uses
the captured `Air` polarity, while the paired density=0 contour uses the captured scalar field directly (no blur
of the coarse raster). Capture refuses a grid over `MaxCells` before allocating it; ordinary metric callers do
not retain either buffer.
`VF_AreConnected` recovers a deterministic BFS parent path from the same kind of grid and checks
candidate routes at full voxel resolution before returning `Connected`. When a candidate is
refuted, its specific coarse cell edge is added to a deterministic array blocklist and BFS is
retried up to `MaxRouteRetries` (default 16). It returns an explicit `EVoxelConnectivityResult`;
a solid endpoint cell is repaired by snapping to the nearest air cell among its 26 neighbours
when possible, and reports the snap through its out flags. A missing coarse route after edge
exclusions returns `NotConnectedAtThisResolution`: that is evidence only at the grid's
resolution, because a corridor thinner than `SampleStep` is invisible. Exhausting the retry
budget (or finding a refutation in an endpoint segment with no coarse edge to exclude) returns
`CoarseLiedBudgetExhausted`, which is unknown rather than disconnected. Every `Connected` result
has a full-resolution air-verified route, and the query reports the alternate route retry count.
No UObject state, cache, actor, world, or PIE is required.
| Symbol | Role |
|--------|------|
| `FVoxelStrateMeasureSettings` / `FVoxelStrateMetrics` | Plain settings/result structs for bounded strate sampling and derived measurements; callers may override the interior margin (including zero to sample the seal) or fit the XY window to two points with a margin, and results identify the resolved window/dimensions/Z range, deterministic air-component facts, walkable floor-area columns/fraction, projected surface components/share, and median clearance. |
| `FVoxelStrateSampleGrid` | Optional one-pass `density > 0` `Air` polarity plus exact scalar `Density` capture; player-fit walks may also export the exact `PlayerFitMask` and count. Bounded by `MaxCells`, shared by explorer render/walk/export, and never retained on ordinary metrics calls. |
| `VF_MeasureStrate` | One-grid/one-flood-fill strate metrics; resolves either the legacy centered square or the two-point fitted AABB, and refuses invalid bounds or a grid over `MaxCells`. |
| `VF_AreConnected` | Coarse 6-connected BFS plus deterministic blocked-edge retries, with a full-resolution air recheck for every candidate route and explicit endpoint-solid, out-of-window, `NotConnectedAtThisResolution`, `CoarseLiedBudgetExhausted` (unknown), and connected outcomes; diagnostics include endpoint component facts and the retry count from that same grid. |
| `VF_DiagnoseConnectivity` | The same query plus each mouth's air-component size/share and exact geometric distance to the nearest cell of the other component; proximity is measured in coarse-cell coordinates and does not claim a route through solid. |
| `FVoxelPlayerFitWalkReport` / `VF_MeasurePlayerFitWalkWithSampler` | Editor-only experiential instrument over the existing exact `VF_` capsule/floor-fit mask: deterministic graph-walk distance, dead ends, reachable fit volume, and a labelled narrow-gap proxy; optionally exports the one sampled grid/mask for other explorer consumers; no second movement model. |

`VoxelForgeExploreCommandlet` builds a transient, fixed-definition world and calls the production
`UVoxelGenerator::GetDensityAt` and `UVoxelMarchingCubesMesher::GenerateMesh` paths. `walk` (or the
private seed walk used by render-only runs) captures one exact sampled grid and player-fit mask;
render and export reuse that hand-off. `render` calls the canonical 32³ mesher in a deterministic
tile grid (four tiles per axis for the 128³ default), merges the region once, builds a deterministic
CPU BVH, and rasterises eight viewpoints from the mesh without per-pixel density calls. `export`
uses that same canonical aggregate for metre-space OBJ plus `manifest.json` (canonical UVs remain
voxel-space). Output JSON/manifest are emitted by fixed-order writers and compared with a second
serialization before they are written. The commandlet keeps `WorldRadiusVoxels=0`, does not touch an
authored asset or diff layer, and enforces the default 25-minute / maximum 30-minute wall-clock
budget, reporting partial artifacts and completed modes on truncation. `-gameconfig=1` opts into the
authored settings' skirts/LOD octave drop and permits export-only parity of a single-strate asset;
the historical interior-slot requirement remains in force for walk/probe runs. `VoxelGeometryHash.h`
is the shared digest used by `Tools/VoxelForgeTest.ps1 -Scenario parity`.

### 3.7 Cave morphology (SDF rooms/tunnels) — `Public/VoxelCaveMorphology.h` + `.cpp`
Header is rich with inline docs. Two namespaces + a per-chunk cache system.

- `namespace VoxelSDF` (h:78): `Sphere`, `Ellipsoid`, `Capsule`, `RoundedBox`,
  `TaperedCapsule`, `SmoothMin`, `SmoothMax` — all FORCEINLINE SDF primitives.
- `namespace VoxelHash` (h:210): `Mix`, `Cell`, `Pair`, `ToFloat01`, `ToFloatSigned`
  — deterministic hashing for room/tunnel placement (no storage, infinite worlds).
- Cache structs (h:520-950): `FCachedRoom`, `FCachedTunnel`, `FCachedPit`,
  `FCachedChimney`, `FCachedColumn`, `FChunkSDFCache`.
- `namespace VoxelCaveMorphology`:
  | Function | .cpp line | Role |
  |----------|-----------|------|
  | `BuildChunkCache` | 6782 | **Phase 1** (once/chunk): collect rooms, guaranteed backbone (`bTunnelsFlowTowardOrigin`: tree rooted at the (0,0) hub — every room reachable, links flow inward; false = legacy NN forest), slope-aware link metric (`TunnelHorizontalBias` now applies to backbone too), decide tunnels, **cull zero-connection rooms** (no sealed bubbles), store rooms by their OWN reach (fixes origin-room clipping at `MaxInfluence`), pre-bake pits/chimneys/columns via the shared `BakeRoomFeature` hash-placement skeleton (one gate/XY/radius pattern + per-type Emit lambda), hash-roll per-room terrain op. |
  | `EvaluateSDFCached` | 8569 | **Phase 2** (per voxel): SmoothMin over cached rooms/tunnels; returns nearest room idx for terrain-op lookup. **Signature changed (perf pass 2): `RoomShapeVariety` param REMOVED** — the shape roll + capsule trig are pre-baked into `FCachedRoom` (`ShapeType/ShapeA/ShapeB/ShapeR`) by `BuildChunkCache`, bit-identical. |

`MakeStrateSeed` (h:965) is the shared pure world-seed/strate-index salt used by both
`BuildChunkCache` and the TunnelNetwork/Underwater destination landing queries.
`VF_SuggestLandingPoint` (h:1121, implemented in `VoxelCaveMorphology.cpp`) is the pure player-fit
landing-site query: it evaluates the local archetype source through the same capsule/support
contract as the measurement pass — support within step height, walkable slope, and full capsule
clearance. Room queries use a room-floor core, slabs keep their requested XY, and the sparse
sources use bounded feature cores: a horizontal lattice corridor (one Maze cell), a selected
VerticalShafts tree shaft plus deterministic ledge-side offsets (up to two local shaft spacings at
the 0.6 site density), or a validated blob top (one IslandSpacing). It is intentionally not a
flood fill: the pure query can prove a local pose, not a large component. The
VerticalShafts result is therefore on the drainage tree, not merely in an open roughness disk.
It returns `false` when those feature-bearing conditions cannot be proven inside the supplied budget;
SurfaceWorld remains refused because its production height can depend on manager-owned biome/per-column
context. It never touches a generator, operator stack, manager, cache, or mutable state.

`FVoxelPassageLanding`, `VF_BuildPassageLanding`, `VF_EvaluatePassageLandingSDF`, and
`VF_IsPassageLandingFloor` (h:57/184-199, implemented near `.cpp:6357`) turn each inter-strate mouth
into a deterministic rounded room plus a hard support slab. The room is sized from the player
capsule (minimum 12 voxels / 3 m flat floor, 12 voxels / 3 m clear height, and at least 14 voxels /
3.5 m authored width); the `(0,0)` origin landing is enlarged for the reserved future shaft, while
inter-strate mouths remain local rooms and deliberately have no landing-to-root connector road.
The manager's shared thread-local passage shortlist serves tube, room, and floor evaluation; no
landing search occurs per voxel. `GeneratePassages` stores the full descriptors and the
order-independence test compares them bit-for-bit.

  Performance note (h:520-531): caching rooms/tunnels once per chunk instead of per
  voxel is the single biggest CPU win.

### 3.8 Strate system
**`Public/VoxelStrateTypes.h`** — shared structs/enums (1228 lines, the data vocabulary):
| Symbol | Line | Role |
|--------|------|------|
| `EVoxelPassageType` | 42 | Sloped/Vertical/Spiral/Cascading/Crack passage shapes. |
| `ESurfaceType` | 81 | Floor/Wall/Ceiling/Any (decoration placement). |
| `EVoxelNoiseType` | 128 | FBM/Ridged/Mixed/Cellular. |
| `ECaveGeneratorType` | 175 | TunnelNetwork / FlatPlain / CrystalChamber. |
| `EVoxelStrateTransition` | 228 | Gradient / Hard / Interleaved boundary blends. |
| **`FStrateGenerationParams`** | 373 | The giant TunnelNetwork param bag (rock, worms, rooms, tunnels, warp, roughness, live terrain-detail fields, dead terrain-op transport slots, boundary seal). `Lerp()` static blends two sets at boundaries — it expands the **`VF_STRATE_PARAM_FIELDS` X-macro** (defined just above the struct): **adding a field to the struct? add it to that list** or blends silently reset it to default. |
| `FStrateTerrainOpEntry` | 1169 | Soft-ptr to a terrain op + Weight + Probability. |
| **`FSlabGenerationParams`** | 1223 | Floor/ceiling heights, roughness, columns, seal — for slab generators. |
| **`FPlacementProfile`** | 2000 | **Shared placement vocabulary** for every scatter primitive (`FStrateDecoration`, `FStrateLandmark`; set-pieces are landmarks): spawn (ActorClass/InstancedMesh), Filter gates (surface/slope/overhang/water/RequiredBiome + **F7 awareness `Conditions[]`** — `FTerrainCondition` relief/moisture/biome-border predicates, AND-ed, evaluated by `Generator::EvaluateTerrainConditions`), Transform (align/offsets/RotationOffset+RandomRotation/scale), Render (cull/shadow). Each primitive embeds it as `Profile` + keeps only its own DISTRIBUTION fields. Per-primitive defaults set in each struct's ctor (deco: scale 0.8-1.2 + RandomRotation.Yaw=360; landmark: Ceiling + no align). |
| `FStrateDecoration` / `FStrateLandmark` | 2207 / 2271 | `Profile` + distribution: deco = StreamTier/SpawnDensity/MaxPerChunk; landmark = SpacingChunks/JitterFraction/SpawnProbability/StreamRadiusChunks + Light-Orb block. |
| `ELandmarkAnchor` (on `FStrateLandmark`) | 2251 | F7: `AnchorMode` = HashLattice / PassageMouth + passage toggles + exclusion (`ExclusionRadiusChunks`/`Priority`) folded into `FStrateLandmark` (set-pieces merged in — one primitive, one `Landmarks` list, `UpdateLandmarks`). |
| `FStrateAmbientActor` / `FStrateCreature` | 2405 / 2425 | Content spawn entries (consumed by future systems). |

**`Public/VoxelStrateDefinition.h`** — `UVoxelStrateDefinition : UPrimaryDataAsset`
(line 46). One asset = one strate *type*. Fields: identity, `StrateHeightInChunks`(75),
`TransitionType`(93)/`TransitionBlendChunks`(103), `GeneratorType`(116),
`GenerationParams`(160), `SlabParams`(171), `Biomes[]`+`BiomeMapParams` (the biome list +
field tuning — empty ⇒ unchanged world, §8.14), `TerrainOperations`(275), visuals/fog/light,
content lists, audio, `GameplayTags`(416), `bUseOperatorStack` (the OPSTACK A/B opt-in — only bites
if the archetype is in `UsesOperatorStackForChunk`'s ported list). EditConditions show/hide param
groups by generator type.

**`Public/VoxelStrateManager.h` + `.cpp`** — `UVoxelStrateManager : UObject` (h:152).
Maps depth→strate at runtime; owns passages.
- `FVoxelPassage` (h:46): standing endpoints, radius, type, wandering control points, and
  upper/lower local landing descriptors (room, floor, and door; no root connector).
- `FStrateSlot` (h:128): definition + chunk-Z range + index.
| Method | .cpp line | Role |
|--------|-----------|------|
| `Initialize` | 1165 | Keeps the existing sequencing through `GeneratePassages`, but selects one slot source: a validated cooked season (exact ordered bounds/vector/recipe) or the unchanged authored fixed+sorted/shuffled pool. Assigned-invalid seasons fail closed. Authored source definitions are duplicated as content bags; generated slots use deterministic C++ defaults and zero auto-passages because schema 2 does not store passage configuration. |
| `GeneratePassages` | 1735 | Deterministic passages between consecutive strates (per-strate `PassageConfig::Style` control points; auto passages retain the existing `EVoxelPassageType` default); placement and shape values are independently salted hashes of seed + boundary slot + connection index. Each mouth independently queries its own strate's pure player-fit `VF_SuggestLandingPoint` with its contract seed (cave room salt, otherwise world seed), may move within its archetype-specific budget, then receives a body-sized local room/floor. The origin landing is reserved for the future shaft; no landing-to-root road is generated. Unsupported/no-fit answers preserve the old random reach; a deterministic level dog-leg is inserted when the final ramp needs it to stay ≤44°. All descriptors and bounds are generated without reading another passage. |
| `EvaluateModifierSDF` | 2719 | SDF of passage tubes plus local landing rooms at a point. A per-chunk `thread_local` shortlist (`PassagesVersion`-stamped) → far chunks return `FLT_MAX` without walking `Passages`; the same cache serves floor membership. `FPassageEvaluationCache::FloorProjections` reuses native/generic floor projections and walkable-air results once per `(sample, passage)` using exact double XYZ equality and unchanged float outputs; `voxel.FloorRound1PassageProjectionCache=0` disables only this A/B cache. §8.10. |
| `ApplyPassageModifier` | 2848 | Shared legacy/op-stack passage post: carves tube and local landing air through the idempotent landing primitive, then restores the local landing support slab in internal positive-solid density. |
| `VF_ApplyPassageLandingCarving` | — | Idempotent density-independent landing-air threshold shared by the structural pass and the post-disturbance MC backstop; keeps legacy and operator-stack results bit-identical. |
| `ApplyPassageLandingFloorMC` | 3059 | Reasserts the same support slab after MC-space disturbances and before the final XY edge seal. |
| `AnyPassageNearBox` | — | Conservative sphere-vs-AABB test of every passage's bound against a voxel box (+carve blend pad). Per TILE (ClassifyTile guard), never per voxel. |
| `AnyPassageLandingFloorNearBox` | — | Conservative local-landing-floor AABB guard. It kills the `AllAir` hypothesis wherever a proved landing floor can occur; the op-stack returns `Both` for the same box. |
| `FindSlotIndexForChunkZ` | 6187 | Z → layout index. |
| `GetStrateAt` / `GetStrateIndex` | 6203 / 6215 | World-Z queries. |
| `GetLayoutVersion` | h:250 (inline) | Layout/passage generation counter (= `PassagesVersion`, bumped by every `Initialize` and by the editor composer override). Hot-path callers key `thread_local` memos on it (strate-index memo in `GetDensityWithParams`, passage shortlist) so live changes never serve stale data. |
| `GetStrateForChunk` | 6226 | Chunk → definition. |
| `GetGeneratorTypeForChunk` | 6252 | Chunk → generator type. |
| `UsesOperatorStackForChunk` | 6279 | Chunk → should `GetDensityAt` take the operator stack? `bUseOperatorStack` on the definition **AND** archetype in the ported list — **now all 8 of 8** (Maze, FlatPlain, CrystalChamber, SurfaceWorld, VerticalShafts, FloatingIslands, TunnelNetwork, Underwater). **That list is written down here and nowhere else.** With every archetype ported the flag is now the *only* thing that decides the path, so ticking the box is no longer a no-op anywhere — it is a real switch onto the operator stack for that strate. |
| `GetRecipeForChunk` | — | Runtime immutable recipe/vector/strate-seed copy for cooked seasons; editor slot overrides use the same worker hand-off. |
| `GetSlabParamsForChunk` | 6359 | Slab params with runtime Z bounds (no blend — slabs use Hard). |
| `GetBiomeContextForChunk` | — | Flatten the strate's `Biomes[]` + `BiomeMapParams` into a POD `FBiomeContext` for the biome field. Empty ⇒ biomes disabled. §8.14. |
| `GetGenerationParams` | 6605 | **Blended** TunnelNetwork params (handles Gradient/Hard/Interleaved transitions). |
| `SetComposerOverrideForStrate` / `GetComposerOverrideForChunk` | editor-only | Temporary density-only candidate overlay for one existing slot. Copies immutable params/recipe into the worker refetch path, leaves layout/content/passages intact, and bumps `PassagesVersion` to invalidate generator memos. |
| `BuildParamsFromDefinition` (static) | 6911 | Returns the definition's base `GenerationParams` only. Terrain-op assets are passed separately to `BuildChunkCache`, which applies one selected op per room and pre-bakes Column/Pit/Chimney features. |

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

**`Public/VoxelStrateComposer.h` + `Private/VoxelStrateComposer.cpp`** (Tier 4a/4b + Tier 5) — offline
`FVoxelStrateCorpus`, `VF_RollStrateParamsDetailed`, and `VF_RollStrateStructure`. `LoadFromAssetRegistry` enumerates every project
`UVoxelStrateDefinition` through the Asset Registry, excludes `Saved/Autosaves` and `Saved/Cooked`
copies, and adds one `VoxelStrateTypes.h` default vector for each of the eight exact archetypes.
The corpus is grouped by exact `ECaveGeneratorType`: the 2026-09-04 run loaded **4 project
vectors + 8 defaults = 12 members** in **8 groups**. Sibling families use their native structs
(`FSlabGenerationParams`, `FMazeGenerationParams`, `FSurfaceGenerationParams`,
`FVerticalShaftParams`, `FFloatingIslandParams`); no cross-archetype blend is attempted.
Spreads are measured per group, including the plain-native tunnel fields. The fixed-lattice audit
found **23 live terrain-detail fields** (Terrace, LayerLines, Overhang, Ribbing, Cliff, Scallop,
Arch, Dome, Pinch) and **11 dead direct transport fields** (Column, Pit, Chimney); only the latter
remain excluded. Live fields receive one matching `UVoxelTerrainOpDefinition` header-default sample
in addition to authored strate values, so an all-default strate corpus still has useful activation
spread. Rolls are pure in `(corpus contents, seed, index)`: weighted 2–3-parent same-archetype
selection → `FStrateGenerationParams::Lerp` or reflected native-family blend → ±15% of measured
field range jitter (or the season-zero ±25% own-magnitude bootstrap for near-zero ranges) →
editor-reflection clamps → ordered-pair repair. Bool
`bTunnelsFlowTowardOrigin` inherits from the dominant parent; integer/enum SNAP fields are not
jittered. Normal runtime generation never calls this API; the editor-only AVoxelWorld bridge does so
only from its PIE button. The settings audit found **one unique path**,
`/Game/VoxelForge/DA_Strate3`, which explains the old one-vector corpus; the project assets were
never in that pool.

`FVoxelOpStackRecipe` is the serialisable Tier 4b manifest: root polarity, root/source/conversion
class IDs, an ordered modifier ID list, and one native parameter-family ID per entry. Posts are not
represented in the manifest; `VF_BuildStackFromRecipe` appends spine, vertical seal, passage carve,
and XY edge seal in the fixed order. The roller derives modifier legality from the op channel and
resource declarations, rolls 4–8 unique modifiers, and validates the recipe before materialising
it. **Rolling is editor-only; `VF_BuildStackFromRecipe` is runtime evaluation and is used by cooked
seasons.** `RequiredResources` / `ProvidedResources` close the room-state hole that channel masks alone
could not express. `VoxelForgeComposerStructureRollTest` rolls 64 candidates, measures them through
the offline sampler, and brute-forces every uniform box verdict from the novel stack itself.

Tier 5 adds `FVoxelStratePromotableRecord`, `FVoxelStratePromotionPolicy`, and
`FVoxelStratePromotionBatchResult`. `VF_SaveStratePromotedRecords` writes a sorted schema-2 JSON
corpus store containing the complete six-family parameter vector, recipe, archetype, gate evidence,
metrics, season/seed, and input corpus hash; `VF_SaveStrateSeasonManifest` writes one diffable JSON
summary beside it. `FVoxelStrateCorpus::LoadPromotedRecords` re-verifies every record through the
world-specific `IVoxelStratePromotionVerifier` before admission, remeasures project/default members
when that verifier supplies the optional audit, and tags membership `project` / `default` /
`promoted`. `VF_SelectStratePromotions` uses normalized measured-metric distance (threshold 0.20),
the existing corpus contents hash, deterministic RecordId ordering, and a six-record season cap.
The promotion test simulates five seasons and deliberately corrupts one stored metric to prove the
loader replaces it with a fresh measurement.

`EVoxelStrateCorpusFreeSamplingMode`, `VF_RollStrateParamsCorpusFree`, and
`VF_ValidateStrateCorpusFreeConstraints` are the offline corpus-free control/experiment. They never
read a corpus entry or strate asset: `NaiveUniform` rolls every live scalar independently inside a
finite, code-declared envelope, while `ConstraintSampled` draws geometry quantities and derives
dependent quantities from the generator equations before applying the envelope as a final safety
net. `VoxelForgeComposerCorpusFreeTest.cpp` compares both arms with today's corpus blend using the
same structure recipe, fixed measurement window, one arrival/departure law, and box-verdict brute
force. The requested 256×3 and 64×3 runs were abandoned before aggregate output because the fixed
8M-cell law/box workload was too slow; the completed equal-arm run is 16 candidates per arm and
22.995 s inside the test. Its final result is **13/16 corpus-blend survivors, 7/16 naive, and
7/16 constraint-sampled**. Survivor quality is not equivalent: corpus survivors averaged
**0.066071 walkable / 92.307693 voxel feature scale**, versus **0.012566 / 64.571426** for naive
and **0.012851 / 31.428572** for constrained. All three arms had **0 box-verdict violations**;
proved boxes / checked lattice voxels were **196 / 260,876**, **310 / 412,610**, and
**262 / 348,722** respectively.

Public/VoxelWorld.h + Private/VoxelWorld.cpp — ApplyComposerCandidate exposes the four
Live Edit|Composer properties and the CallInEditor button. It preflights the candidate, pauses
generation before installing the manager override, and calls the existing epoch-aware full reset.
The post-apply check compares eight densities against the shared native/recipe stack where the live
world has no extra diff, disturbance, or biome context. It reports SKIPPED rather than comparing
unlike fields.

**`Public/VoxelStratePreview.h` + `Private/VoxelStratePreview.cpp`** — editor/automation-only PNG
renderer and self-contained contact sheet. It consumes `FVoxelStrateSampleGrid` from the measurement
pass, emits deterministic centre-Y XZ and data-selected XY slices via `IImageWrapper`, and shows filled-cell
and scalar density=0 contour views side by side without blurring the sampled raster. Each image is capped at
512 pixels (including its scale footer). `FVoxelStrateFinePreviewSettings` is a separate caller-selected
fine pass: the composer selects only survivors at step 1, radius 64 (a 128×128 XY ROI), and `MaxCells=4,000,000`;
the caller centres it on each coarse survivor's `LargestComponentPoint` and the writer labels it
“largest open space” with its exact ROI and step; cap refusals and blank fine ROIs are recorded explicitly
instead of showing empty evidence. The ordinary 64-card sheet remains sorted by survivor status then
descending corpus-centroid distance. The `VoxelForge.Composer.Showcase` test additionally writes an
alphabetized one-card-per-archetype PIE hand-off page with the Part A survivor distributions, exact seed /
candidate / target-slot values, and no runtime generation hook.

**`Public/VoxelSeasonManifest.h` + `Private/VoxelSeasonManifest.cpp`** — Tier 4c's offline composer
and runtime-readable schema-2 artifact. `VF_ComposeSeason` generates a bounded configurable candidate batch, applies the Tier 2
non-vacuous / largest-component / primordial-law gates, then selects an ordered Tier A spine with the
explicit provisional grounded/outlier, adjacency, variety, and five-slot boss policy. The JSON stores
the complete six-family parameter vector (float values carry both a readable number and exact IEEE-754
bits), seed/index/archetype/recipe, bounds, measured metrics, gate facts, candidate audit, and rejection
counts. A canonical SHA-1 `content_hash` covers the complete payload. `VF_DeserializeVoxelSeasonManifest`
verifies it in memory; `VF_LoadVoxelSeasonManifest` plus `VF_RebuildVoxelSeasonStrate` are the round-trip seam; generated
recipe entries rebuild through the mandatory-post builder, while authored `FixedStrates` retain their
absolute slot and native vector. The selected-only descent review reuses `VoxelStratePreview`, and all
composition/review work is editor/build-box code; parsing and recipe materialisation are runtime. Lateral
region manifests remain rejected because their post-landing cross-seam law is only **4/16** (the
pre-landing snapshot was 9/16). The focused
season test currently reports **24 generated / 19 hard-gate survivors / 6 selected / 18 unselected or
rejected**, with **4 vacuous**, **1 primordial-law budget**, **13 policy-not-selected**, and **393,216**
round-trip density samples bit-identical. The specified UE 5.7 editor build succeeded and the final
`VoxelForge` namespace report `VoxelForgeFullMazeTreeFinal3` recorded **30 clean successes, 2
warning-bearing, 0 failed, 0 not run** in **2,523.978 s**. The regenerated showcase produced **6 fine
renders, 2 refusals, and 0 blanks**; all box-verdict violations remained zero. The later focused
production-height showcase history is recorded separately below.

The current landing validation is recorded in `VoxelForgeLandingFinal5`: **30 clean successes, 2
warning-bearing successes, 0 failures, 0 not-run** across 32 tests in **3,607.746 s**. The strict
promotion assertions passed and no assertion was weakened. The landing-specific geometry test,
both connectivity tests, all box verdicts, seals, determinism, and op-stack equivalence tests passed.

**`Public/VoxelSeasonAsset.h` + `Private/VoxelSeasonAsset.cpp`** — `UVoxelSeasonAsset :
UPrimaryDataAsset`, the cookable carrier. Its editor button imports the reviewable JSON, stores the exact
text plus independently copied metadata/hash, and retains soft references to any authored definitions.
`UVoxelSettings::Season` is the only activation switch.

### 3.9 Player edits — `Public/VoxelDiffLayer.h` + `.cpp`
`UVoxelDiffLayer : UObject` (h:137). Stores `FVoxelModification` (h:60: Center/Radius/Strength;
**negative Strength = carve, positive = fill**) grouped by chunk in `TMap ChunkMods`.
| Method | .cpp line | Role |
|--------|-----------|------|
| `SetBudget` | 102 | From VoxelSettings carving caps. |
| `CanModify` | 112 | Budget check (no consume) — for UI. |
| `GetRemainingModifications` / `GetRemainingVolume` | 169 / 175 | -1 = unlimited. |
| `ApplyModification` | 185 | Enforces budget, stores in all overlapped chunks, returns dirty coords. |
| `GetDensityOffset` | 299 | Per-voxel combined diff (smoothstep falloff, additive). |
| `HasModifications` | 421 | Fast reject for hot path. |
| `HasAnyMods` / `GetModsVersion` | h:246 / h:249 (inline) | Lock-free atomics: any-mod-exists flag + monotonic mod-state version (bumped by `ApplyModification`/`Clear`). |
| `GetChunkModsSnapshot` | — | Copy one chunk's mod list under ONE read lock. Workers snapshot per (chunk, version) instead of locking per voxel — `GetDensityAt` keys a `thread_local` 64-slot direct-mapped cache on it (~27 lock ops per tile task instead of ~86k once any carve exists). |
| `HasAnyModInChunkRange` | — | Any modified chunk key in an inclusive chunk box? One key walk under a read lock — ClassifyTile's diff guard (per tile, conservative by construction: mods are stored in every chunk their radius overlaps). |
| `EvaluateMods` (static) | — | Lock-free pure evaluation of a mod list at a voxel — shared core of `GetDensityOffset` and the generator's snapshot path. |
| `Clear` | 435 | Wipe all (season reset). |
| `GetTotalModificationCount` / `GetModifiedChunkCount` | 454 / 465 | Stats. |

### 3.10 Mesher — `Public/VoxelMarchingCubesMesher.h` + `.cpp`
`UVoxelMarchingCubesMesher : UObject` (h:42). Holds `Generator` ptr, `IsoLevel=0`, skirt params.
The pre-sampled grid supplies positions AND gradients inline (T1.b).
| Method | .cpp line | Role |
|--------|-----------|------|
| **`GenerateMesh`** | 96 | The MC loop over cells; `Step` controls LOD sampling. Sets the T2.b octave bias for the tile (`TGuardValue` on `VoxelGenLOD::OctaveBias`, from `LODOctaveDrop` × log2(Step); 0 at LOD0/off). Edge `t` + grid-gradient normals computed inline (`SampleG`/`GradAt`). Optional `OutCaptureGrid` (4th arg) = CAPTURE-DURING-MESHING: when non-null + full-res (`CellsPerAxis==CHUNK_SIZE`), copies the already-sampled `CHUNK_SIZE³` density grid (quantized via `VF_QuantizeDensity`, VoxelTypes.h) so the density clipmap reuses it instead of re-sampling `GetDensityAt`. Pure read of the grid — §8.10 untouched. **F17 surface class**: each unique vertex is classified sol/sky-cap in `GetOrCreateVertex` (down-facing only → memoized `GetSurfaceHeightAt`, nearer `CeilSurf` = cap); triangles bucket by majority into `GroundTris`/`CapTris` (thread_local), skirts emit per bucket, then `Triangles = ground‖cap` + `FVoxelMeshData::NumCeilingTriangles` (→ polygroups in `BuildTileStreamSet`). Same tris, only index ORDER changes. STRATE CONTENT CUT: optional `BandZMin/MaxVox` params restrict the cz cell loop (+ gz sampling rows) to the player-strate band — coarse straddling tiles mesh ONE strate (kills far-LOD inter-strate aliasing holes); meshed cells bit-identical. |

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
The plugin's first tests (`Docs/archive/OPSTACK-PLAN.md` Phase 0.5). Run them from the editor's
**Session Frontend → Automation**, filter `VoxelForge`.

| File | Test name | What it proves |
|------|-----------|----------------|
| `VoxelForgeTestFixture.h` | — | `FTestWorld`: a headless world (transient strate definitions → `UVoxelSettings` → a real `UVoxelStrateManager::Initialize`) so tests hit `GetDensityAt`, where the thread_local caches live. One strate per archetype, **pinned via `FixedStrates`** so slot index → archetype is stable across seeds (`SlotSurfaceWorld` etc.). Bounded regression fixtures default to 4 chunks; the production/default definition and the owner-facing showcase explicitly use 8 chunks. |
| `VoxelForgeDensityPurityTest.cpp` | `VoxelForge.Determinism.DensityPurity` | 10k points re-sampled in shuffled order, same thread **and** on N workers, asserting BIT equality. `ValidateDeterminism` is game-thread only and cannot see worker-cache divergence. Includes a flat-field canary (AUDIT C1) and a diff-layer pass. |
| ″ | `VoxelForge.Determinism.LiveEditInvalidation` | AUDIT C2 regression: triple the heightfield params, `Initialize` again, require the density to MOVE. The edit does not move the strate, so only `LayoutVersion` changes. |
| `VoxelForgeClassifyTileTest.cpp` | `VoxelForge.Determinism.ClassifyTileSoundness` | Scans for a non-`Mixed` verdict, then brute-forces the exact mesher lattice (`g ∈ [-1, Cells+1]`). **A false verdict is an invisible, collisionless hole** — T1.d v1 was reverted for exactly this. Errors out rather than passing if it found nothing to check. |
| `VoxelForgeDiffLayerTest.cpp` | `VoxelForge.Determinism.DiffLayerContention` | N readers running the worker call mix while the game thread writes and `Clear()`s. Survival + monotonic `ModsVersion`. |
| `VoxelForgeClassifyTileTest.cpp` | `VoxelForge.OpStack.BoxVerdictFold` | Pure-logic walk of the fold in `VoxelDensityOp.h`, case by case — including the seal-forces-AllSolid case that justifies `ClassifyBox` existing. Also the only `.cpp` that includes the op header, so the build actually sees it. |
| `VoxelForgeHeightStackTest.cpp` | `VoxelForge.OpStack.SurfaceHeightEquivalence` | The height-space stack vs `ComputeSurfaceTerrainZ`, in **altitudes**. Runs twice: defaults, then **all F20 terrain ops ON** — the load-bearing pass, since the ops are off by default and the defaults pass exercises only the structural source. Also brute-forces `MaxDisplacement` (a false bound would be a hole). Bar is bit-identity; a height delta is a visibly different world, not rounding. |
| `VoxelForgeCrossPlatformTest.cpp` | `VoxelForge.Determinism.CrossPlatformDigest` | SHAPE digest (sign of density = the world) + FIELD digest (bit-for-bit) over a fixed integer grid, plus `NearIso` bounding how many samples could flip sign. Reports rather than asserts until pinned. Run on Windows and Linux and compare. |
| `VoxelForgeOpStackSlabTest.cpp` | `VoxelForge.OpStack.SlabEquivalence` | **Phase 2's first port.** The same 6-op slab stack vs `GetSlabDensity` over 20k points, run twice — FlatPlain **and** CrystalChamber — which is what demonstrates the two archetypes really are one op. Plus window-invariance and box-verdict brute force. Compares against the reference **as it is now** (post Z-term removal), so green = pure refactor and any visual delta is attributable to §3.1 alone. |
| `VoxelForgeOpStackTunnelTest.cpp` | `VoxelForge.OpStack.TunnelNetworkSpineEquivalence` | **Stage A of the last port.** Zeroes the 13 detail-op amplitudes so the *original* takes the path stage A ported — that is what makes an incomplete stack verifiable now. Samples in **clusters** (24 chunks × 250 points), because the SDF cache rebuilds when a query leaves its box and uniform sampling would rebuild per point on both paths. Check 3 (two param sets, A/B interleaved) compares each stack **to itself alone, never to the original** — the original would fail it, see AUDIT §C2. Asserts **zero** box verdicts, which is the honest stage-A result. |
| `VoxelForgeOpStackShaftTest.cpp` | `VoxelForge.OpStack.VerticalShaftEquivalence` | The port that tests **reuse**, not fidelity: three of the five ops are Maze's, unchanged. Forces connectors + ledges on, because both are off or negligible at defaults and a resting param is an untested operator. The source and mirror now share a deterministic inner-3×3-emitted drainage tree whose parents come from rebuild-only ±2/±3 windows in a wider direct-indexed halo (default 9×9 emit / 15×15 roll), plus the old random links. Tree connector Z is exact and ledge-free; tree and random capsules are spatially culled from the cached rebuild. **Fixed 2026-07-29:** its box query used to return `CarveOnly` because a shaft merely *existed* within a `Spacing*1.6` halo — true almost everywhere at `ShaftSpacing 55 / ShaftDensity 0.6`, hence **0 of 60** tiles. It now publishes an isolated interval for tree and random capsules (same row-major cell order ⇒ same `VoxelHash::Pair`, so symmetry of `Pair()` is not assumed), with **Z exact** and XY conservative; the ledge and converter decide their own effects. The sampler was also widened from ±48 voxels to ±440. Every proved tile is brute-forced over its full lattice, and the test reports rebuild-vs-hot cache cost. |
| `VoxelForgeOpStackIslandTest.cpp` | `VoxelForge.OpStack.FloatingIslandEquivalence` | The port that runs the stack **backwards** — void + fill vs rock + carve, same classes with the opposite sign. Counts interior-solid and open-void samples separately (on this archetype an aggregate "N solid" is dominated by the seal bands and says nothing about the islands). Counts `AllSolid` and `AllAir` verdicts **separately** too: `AllAir` is the one no cave archetype could ever prove, and it is the entire perf argument here. |
| `VoxelForgeOpStackMazeTest.cpp` | `VoxelForge.OpStack.MazeEquivalence` | **Phase 1's load-bearing test.** The 8-op Maze stack vs `GetMazeDensity` over 20k points (bit-identity; a side-of-iso disagreement is the hard fail), plus purity across workers, a 64-seed spanning-tree/connectivity audit, corridor morphology, cache rebuild-vs-hot timing, and brute force on every box verdict the stack emits. Latest audit: **0 tree edge-count violations, 0 tree-disconnected, 0 Maze-disconnected**; interior degrees `d1=15.91%`, `d2=36.60%`, `d3=31.73%`, `d4=12.95%`, `d5=2.56%`, `d6=0.25%`, mean **2.504** versus a cubic grid's degree-6/mean-6.000; runs `min=1 / median=1 / p90=3 / max=18`. Latest focused cache timing: **1.436 μs rebuild**, **0.150 μs hot call**; final namespace timing: **1.130 μs rebuild**, **0.145 μs hot call**. Latest box audit: **104 proved, 152 Mixed, 138,424 voxels, 0 violations**. |
| `VoxelForgeStrateParamCoverageTest.cpp` | `VoxelForge.Determinism.StrateParamBlendCoverage` | **The X-macro guard** (added 2026-08-17). `FStrateGenerationParams::Lerp` blends the hand-written `VF_STRATE_PARAM_FIELDS` list, **not** the struct — so a field added to one and not the other compiles, tests green, and silently takes its **default** inside every Gradient/Interleaved transition band. This expands the X-macro a **third** way (after LERP and SNAP), into a name list, and diffs it against the struct's UObject reflection. Pure shape test: no fixture, no world, instant. `GExemptFieldNames` is **empty** — every reflected field is covered today, and any exemption must be written down as a decision. Stakes rise with the world composer, which intends to invent parameter sets through this same `Lerp` (`COMPOSER-NOTES.md`). |
| `VoxelForgeComposerParameterRollTest.cpp` | `VoxelForge.Composer.ParameterRoll` | Asset-Registry corpus audit + complete per-archetype spread/exclusion/clamp table; asserts bit-identical deterministic rerolls and same-archetype parents; measures 64 transient candidate strates with `VF_MeasureStrate` plus the exact unsnapped arrival→departure law; brute-forces every rolled production box verdict. World-scale run: **1,422 applications / 168 distinct fields**; **36/64 survival**, 60 non-vacuous, 58 largest-component, 40 exact-law passes; **1,411 Mixed + 731 AllSolid + 418 AllAir = 1,149 proved boxes**, **1,529,319 lattice voxels checked, 0 violations**, 148.387 s. |
| ″ | `VoxelForge.Composer.TerrainDetailLiveness` | Fixed 4,096-point `GetDensityAt` lattice, legacy and operator-stack paths; changes one terrain-detail group at a time with an empty terrain-op pool. Proves 9 live groups / 23 fields and 3 dead groups / 11 fields; all 24 rows match. |
| `VoxelForgeComposerPromotionTest.cpp` | `VoxelForge.Composer.Promotion` | Re-measures the 12 project/default members, simulates five deterministic 24-candidate seasons with normalized measured-metric novelty (`<0.20`), cap 6, cumulative JSON promotion, provenance counts, corpus-hash checks, fresh-load gate verification, spread/survival reporting, and deliberate stale-metric corruption. The final landing validation passed the strict existing gates; no assertion was weakened. |
| `VoxelForgeComposerStructureRollTest.cpp` | `VoxelForge.Composer.StructureRoll` | Rolls root polarity → legal shape source → polarity-derived conversion → 4–8 declaration-legal modifiers → mandatory structural posts; blends the six native parameter families independently, measures 64 novel stacks, captures the same grid for the deterministic filled/contour XZ/XY preview, runs a separate step-1 radius-64 ROI pass for the 33 survivors centred on `LargestComponentPoint`, checks exact arrival→departure connectivity, rerolls every recipe/stack for determinism, and brute-forces every uniform box verdict. World-scale run: **33/64 survival (51.6%)**, **64 distinct recipes**, **0 invalid recipes**, **715 proved custom boxes / 951,665 lattice voxels / 0 violations**, blank plan/card **4/33→5/33**, **174.854 s**, **0 refusals**. |
| `VoxelForgeComposerShowcaseTest.cpp` | `VoxelForge.Composer.Showcase` | Exhausts the bounded parameter-roll set (seeds **0, 7331**, indices **0–63**), excludes multi-region rolls while the lateral gate is off, and measures every missing-family candidate in all six interior target slots at step 4 / radius 256 / `MaxCells=8,000,000`. Hard gates are non-vacuous, largest air share ≥ **0.50**, and exact unsnapped arrival→departure connectivity; selection score is floor-area fraction + clearance tie-break + projected-surface tie-break. It asserts roll/manifest determinism, density sign, zero `WorldRadiusVoxels`, bit-identical metric reruns, and writes one alphabetized card per archetype to `Saved/VoxelForge/Showcase/index.html`, with step-1 radius-64 filled/contour plan + vertical ROI images centred on `LargestComponentPoint`. The final landing regeneration evaluated **256** rolls (**82** eligible single-region, **174** skipped multi-region), produced **7 fine renders, 1 refusal, 0 blanks**, and retained a diagnostic hard-gate survivor set of **0/8**. |
| `VoxelForgeComposerSeasonTest.cpp` | `VoxelForge.Composer.Season` | Existing compose/review/393,216-sample round trip plus schema hash tamper rejection, two independent runtime manager/generator instances, season-authoritative seed/spine/radius, unset-season regression, and brute-force verification of every sampled non-Mixed recipe tile verdict (up to 64). |
| `VoxelForgeComposerCorpusFreeTest.cpp` | `VoxelForge.Composer.CorpusFree` | Shares each structure recipe across today's corpus blend, naive independent uniform rolls, and constraint-sampled rolls. The completed equal-arm world-scale run uses **16 candidates per arm**, step 4 / radius 256 / `MaxCells=8,000,000`, fixed passage-law mouths, and 40 box probes per candidate. Result: **10/16, 9/16, 8/16** survival; survivor feature-scale ranges are **8..268**, **8..60**, **8..92** voxels. Box checks: **185/246,235**, **260/346,060**, **299/397,969** proved/voxels, **0 violations** in every arm; constraint parameter and box-stop violations were also 0. |
| `VoxelForgeLayoutOrderIndependenceTest.cpp` | `VoxelForge.Determinism.LayoutOrderIndependence` | Builds a known transient soft-pointer pool, then rebuilds it in original, reversed, and swapped orders. Requires a non-empty layout and passage set, and compares every slot's definition/Z/height plus passage endpoints, landing descriptors, control geometry, and bounds bit-for-bit. |
| `VoxelForgePassageOpenSpaceTest.cpp` | `VoxelForge.Determinism.PassageLandsInOpenSpace` | Uses the real fixture density path to check both generated mouths against independent source queries, then audits all inter-strate landing ends: body-derived room dimensions, final-density flat-floor slope, local floor/join geometry, seal containment, hot-call/rebuild timing, and targeted `ClassifyTile` box soundness. Reports source-fit/refused mouths and per-archetype landing slopes; fails on any floor, join, seal, or box violation. |
| `VoxelForgeStrateConnectivityTest.cpp` | `VoxelForge.Generation.StrateConnectivity` / `VoxelForge.Generation.StrateConnectivityRefinement` / `VoxelForge.Generation.VerticalShaftSeamFreedom` | Bounded strate metrics with density-polarity and solid-gap controls, deterministic route rechecks, and refinement sweeps. The refinement test uses the bounded 4-chunk density fixture, fits each measurement AABB to the arrival/departure mouth pair plus three shaft spacings (the prescribed local tree fallback), and caps the fitted control at 120,000,000 cells. It reports exact before/after seed-6 cell counts, reruns every negative with the fitted margin doubled, measures tree orphan candidates/path reachability and roughness-bubble proxies, asserts mouths remain inside the deterministic shaft feature core (the exact axis is an unsupported open cylinder), checks source/mirror physical paths, and requires 16/16 effective arrival→departure results across the 16 VerticalShafts seeds. Margin-binding negatives are reported as measurement limits, never as gap findings. The shaft seam test re-evaluates cell-boundary positions after warming distinct neighbouring chunk contexts. |
| `VoxelForgeMazeSeamTest.cpp` | `VoxelForge.Generation.MazeSeamFreedom` | Re-evaluates **42** cell-boundary probes after warming six different neighbouring chunk contexts in both legacy and operator-stack paths. Latest result: **0 legacy mismatches, 0 operator mismatches**. The Maze source's 2×2×2 lower-node window is deliberately local; no wide collect is involved. |
| `VoxelForgeClipmapNoOverlapTest.cpp` | `VoxelForge.Streaming.ClipmapDesiredTilesNoOverlapCoverage` | Sweeps 5,145 cases (343 centres with negative coords and every parity × 5 radius/level/render-distance settings × 3 vertical clamps): **zero overlapping desired pairs AND coverage ⊇ the frozen `7486c29` selector**. Failed on `7486c29` (1,022,679 overlapping pairs) as intended. |
| `VoxelForgeDensityBlockEquivalenceTest.cpp` | `VoxelForge.Density.OperatorBlockMatchesScalarGrid` | Block evaluation (`EvalBlock` after `BuildActiveOpList` pruning) vs scalar, **bit-identical**: 5.1M mesher-grid samples (6 seeds × 8 archetypes × levels 0–5, with edits) + 300k direct `EvalBlock` samples. Scopes `voxel.UseFusedEvaluator=0` so the real block path runs. An injected optimistic `FPassageCarveOp::EffectOverBox` produced 5,612 mismatches. |
| `VoxelForgeWorldEdgeSealTest.cpp` | `VoxelForge.Generation.WorldEdgeSeal` | Positive samples outside the radius across every fixture strate, bit-identical interior negative control against radius 0, monotonic smooth-ramp check, radius-0 no-op, radial `ClassifyBox`/T1.d proofs brute-forced for every reported voxel, and synthetic plus generated near-rim passage coverage. Emits one aggregate summary with proved-box and voxel counts. |
| `VoxelForgeCorrectnessRoundTest.cpp` | `VoxelForge.Determinism.DiffApiBoundary` / `DiffLayerIdentity` / `EmptyPassageVersion` / `SurfaceBiomeCacheIdentity` / `TileCacheContextSwitch` / `ZeroTransitionBlend` | Regression coverage for the cold-audit correctness fixes (diff-layer API edges, cache identities, empty-passage layout version, zero transition blend). |
| `VoxelForgeDescentCorridorTest.cpp` | `VoxelForge.Descent.OwnerPassageCorridor` / `VoxelForge.Descent.OwnerSeedSweep` | Samples the authored A-to-B passage as a body-sized walking corridor in the final owner field. |
| `VoxelForgeDetSinCosTest.cpp` | `VoxelForge.Determinism.DetSinCos` | Pinned contract for `VoxelMath::DetSin` / `DetCos` / `DetSinCos`. |
| `VoxelForgeIsFiniteTest.cpp` | `VoxelForge.Math.IsFiniteExact` | Bit-exact equivalence of the generation-path finite predicate (`VoxelMath::IsFinite`). |
| `VoxelForgeLargeSeedTest.cpp` | `VoxelForge.Determinism.LargeSeedSurvives` | AUDIT C1: the world must still be a world at a large seed (bounded `VoxelHash::SeedOffset`). |
| `VoxelForgeLateralRegionsTest.cpp` | `VoxelForge.Composer.LateralRegions` | Tier 4d lateral region partition, density blending, conservative box proofs, and the arrival→departure gate (asserts `VF_LateralRegionsAreShippable` stays false while the seam rate is imperfect). |
| `VoxelForgeOpStackChannelTest.cpp` | `VoxelForge.OpStack.ChannelDAG` | Channel/resource declarations are executable stack-assembly metadata (`FVoxelOpStack::ValidateChannelOrder`). |
| `VoxelForgeScaleDiagnosisTest.cpp` | `VoxelForge.Composer.ScaleDiagnosis` | Measurement-only scale diagnosis for the player-fit showcase. |
| `VoxelForgeSealedSolidProofTest.cpp` | `VoxelForge.Determinism.SealedSolidProofSoundness` | Sealed-solid tile proof soundness and invalidation (one-sided: may say false too often, never a wrong true). |
| `VoxelForgeTilePostReachTest.cpp` | `VoxelForge.Correctness.TilePostReachProof` | The tile-reach proof must fail loudly when its envelope is deliberately made too small. |
| `VoxelForgeVerticalStrateReachTest.cpp` | `VoxelForge.Streaming.VerticalStrateReach` | Surface-strate streaming keeps the ground visible when the pawn is high in the same strate. |
| `VoxelForgeWormBlockSkipTest.cpp` | `VoxelForge.Determinism.WormBlockSkipSoundness` | Soundness and coverage of the worker-local worm N1 block proof (`voxel.WormBlockSkip`). |
| `VoxelForgePlayerFitWindow.h` | — | Shared measurement-window policy for player-fit diagnostics (test/editor side only). |

## 4. The density pipeline (most-edited hot path)

### 4.1 `GetDensityWithParams` (TunnelNetwork) — VoxelGenerator.cpp:3851
Stage order (negative=solid throughout). Each stage's anchor:
| Step | Line | What |
|------|------|------|
| 1 — Vertical scale | 3906 | Stretch Z before noise (`VerticalScale`). |
| 2 — Base density | 3915 | Everything starts solid at `BaseDensity`. |
| 3 — Cave warp | 3920 | Domain-warp the SDF query coords (organic shapes). |
| 4 — SDF morphology | 3974 | Rooms+tunnels via `BuildChunkCache`/`EvaluateSDFCached`. |
| 4b — Surface roughness | 4378 | Volumetric noise near surfaces (fBM/Ridged/Mixed). |
| 4c–4h — Terrain ops | 4506 | Per-room op applied near surfaces. Sub-anchors below. |
| · Terracing | 4548 | Step-like ledges. |
| · Layer lines | 4645 | Horizontal grooves (sin of Z). |
| · Ribbing | 4675 | Parallel ridges (sin of Z). |
| · Overhangs | 4704 | Low-Z-freq noise shelves. |
| · Cliff sharpening | 4740 | Amplify vertical gradient. |
| · Scallop | 4784 | Cellular erosion bowls. |
| · Arch/Bridge | 4820 | Hash-placed capsules across voids. |
| 4d — Columns | 4849 | Pre-baked vertical cylinders. |
| 4g — Domes | 4877 | Room-relative hemispherical ceilings. |
| 4h — Pinch | 4942 | Passage bottlenecks. |
| 5 — Worm tunnels | 5016 | abs(noise1)+abs(noise2), masked by distance-to-network (`WormNetworkRange`: braids hugging rooms/tunnels, no far-field speckle; 0 = legacy unmasked). |
| 6 — Boundary seal | 5098 | Solid top/bottom shells (`ApplyBoundarySeal`). |
| 7 — Inter-strate passages + landings | `GetDensityWithParams`: 5119; `GetSlabDensity`: 5372 | Carve wandering passage tubes and deterministic local landing rooms, restore their support floors, then leave the final XY seal to the outer MC post (`ApplyPassageModifier` / `ApplyPassageLandingFloorMC`). |

### 4.2 `GetSlabDensity` (FlatPlain / CrystalChamber) — VoxelGenerator.cpp:5188
| Step | Line | What |
|------|------|------|
| 1 — Floor surface | 5199 | Noisy floor height. |
| 2 — Ceiling surface | 5234 | Formations hang downward (`abs(noise)`). |
| 3 — Void→base density | 5271 | Solid outside [floor,ceiling]. |
| 4 — Columns | 5291 | World-space hash grid, full-height. |
| 5+6 — Seal + passages | 5372 | Same seal, passage-tube, landing-room, connector, and support-floor path as TunnelNetwork. |

---

## 5. "I want to change X" → go here

| Goal | Location |
|------|----------|
| Chunk size / voxel scale | `VoxelTypes.h:211-213` (rebuild everything). |
| View distance / task budget / LOD distances | `VoxelSettings.h` (no recompile of logic — data asset). |
| LOD step mapping | `LoadTile` VoxelWorld.cpp:7329-7335: extent `CHUNK_SIZE << Level`, cells from `FullResClipLevels`/`CoarseTileCells`, `Step = Extent / Cells`; tile selection in `VoxelClipmapDesiredTiles::Build` (`Private/VoxelClipmapDesiredTiles.h`). |
| How chunks stream in/out | `UpdateChunksAroundPosition` VoxelWorld.cpp:6870. |
| Async threading / stale-result handling | `LoadTile` :7291, `ProcessPendingChunks` :5856, generation Epoch + per-request cancellation token. |
| Add a new cave feature / terrain op | Add enum in `VoxelTerrainOpDefinition.h:36`, params there, `ApplyTo` (.cpp:6), transport fields in `FStrateGenerationParams`, consume it in a new Step inside `GetDensityWithParams`. |
| Tweak room/tunnel shapes | `VoxelCaveMorphology.cpp` `BuildChunkCache` :6782 / `EvaluateSDFCached` :8569. |
| Worm tunnel behavior | `GetDensityWithParams` Step 5, VoxelGenerator.cpp:5016. |
| Strate stacking / which strate where | `UVoxelStrateManager::Initialize` :1165. |
| Publish/use an offline season | Create `UVoxelSeasonAsset`, set `SourceManifestJson`, press `ImportSeasonManifestJson`, then assign it to `UVoxelSettings::Season`. |
| Boundary blend between strates | `GetGenerationParams` :6605 + `FStrateGenerationParams::Lerp` (expands `VF_STRATE_PARAM_FIELDS`, StrateTypes.h — new fields go in that list). |
| Passages / landing geometry between strates | `GeneratePassages` :1735 + `EvaluateModifierSDF` :2719 + `ApplyPassageModifier` / `ApplyPassageLandingFloorMC` (StrateManager.cpp) + `ClassifyTile` floor guard (Generator.cpp). |
| Player carve/fill | `CarveAtPosition`/`FillAtPosition` VoxelWorld.cpp:8413/8422 → `UVoxelDiffLayer::ApplyModification` :185. |
| Mesh smoothness / normals | Grid-gradient in `GenerateMesh` (`GradAt` lambda), `IsoLevel` (h). |
| New slab/flat-world generator | `GetSlabDensity` Generator.cpp:5188 + `FSlabGenerationParams` (StrateTypes.h:1223). |
| Biome placement / layout | `BiomeMapParams` on the strate (cell size, warp, climate freqs) + each biome's climate box. Bake `AVoxelWorld::BakeBiomePreview` to tune. §8.14. |
| What a biome does to terrain | A full archetype param override on the biome (`bOverrideTerrain` + `SurfaceParams`); surface output-blends dominant/neighbour heights in `GetSurfaceDensity`. Caves = content/atmosphere only (determinism, §8.14). |
| Add a biome / biome content | New `UVoxelBiomeDefinition` asset → add to the strate's `Biomes[]`. §8.14 / §8.12. |
| Season reset | `AVoxelWorld::ChangeSeed` :8710. |

---

## 6. Conventions & gotchas

- **Density sign is the #1 source of confusion.** Internally (MC) **negative = solid,
  positive = air**. Carve = subtract density (toward positive); Fill = add (toward negative).
  `FVoxelModification::Strength` negative = carve. Comments sometimes say the inverse in
  different layers — trust the MC convention at the mesher.
- **Coordinate units:** density functions take **voxel coords** (not cm). World↔voxel
  conversions live in `VoxelTypes.h`. Mesher converts before calling the generator.
- **Espace ACTEUR / actor space:** le champ de densite est en espace **acteur** — `(0,0)` = origine de
  l acteur `AVoxelWorld`, pas du monde Unreal. Tout point venant d Unreal (pion, trace, ancre) passe par
  `AVoxelWorld::WorldToLocalCm` / `WorldToLocalVoxel` / `LocalVoxelToWorld`. **Regle grep : aucun
  `/ VOXEL_SIZE` sur un parametre `World*` hors de ces trois fonctions.** ⚠️ **Translation seulement** —
  rotation/echelle cassent les boites alignees sur les axes (tuiles, `ClassifyBox`, clipmap, culling) ;
  `BeginPlay` loggue une Error. The density field is in ACTOR space; translation only.
- **Determinism:** all randomness is hash-of-(coord, seed, strateIndex) — no RNG state.
  Same seed ⇒ identical world. Player edits are the only non-deterministic overlay.
- **Async safety:** background tasks must check `bShuttingDown` and only touch the
  generator/mesher (no UObject mutation). Results return via `ProcessQueue`. `EndPlay`
  blocks until `ActiveTaskCount == 0`.
- **Epoch:** every regeneration bumps `GenerationEpoch`; results tagged with an old epoch
  are dropped in `ProcessPendingChunks`. Always carry the epoch through new async paths.
- **Generated code** under `Intermediate/` and `Binaries/` is build output — never edit.
  `*.generated.h` / `*.gen.cpp` are UHT output for the `UCLASS`/`USTRUCT` above.
- **`UPackage` completeness:** files that call `NewObject(..., GetTransientPackage(), ...)` must include
  `UObject/Package.h` directly; files that do not include `VoxelForgeTestFixture.h` do not get it transitively.

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
