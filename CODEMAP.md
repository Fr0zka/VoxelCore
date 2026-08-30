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
| `Public/VoxelStats.h` / `Private/VoxelStats.cpp` | `stat VoxelForge` DWORD counters for tile classification, skipping, meshing, operator-stack verdicts, and cave-bail diagnosis. The former ambiguous `Cave Bail Not Op Stack` is split into `Sole Slot`, `Boundary Tile`, `No Layout`, and late `Recheck` counters, so each increment names one guard/context. |

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
| `FVoxelOpSample` | The state threaded through the stack: **two** channels, `Density` (INTERNAL convention, **positive = SOLID**, negated to MC once by the caller) and `Sdf` (standard SDF, negative = inside). ⚠️ `min()` therefore means opposite things on the two channels. |
| `FVoxelBoxHypotheses` + `VF_ForceHypotheses` / `VF_FoldEffect` / `VF_FoldOp` | The fold that turns a stack into an `EVoxelTileClass`. Reproduces today's hand-written `ClassifyTile` line for line — the mapping is written out in the header. |

### 3.2c Structural primitives — `Public/VoxelDensityPrimitives.h`
`VF_ApplyOriginSpine` · `VF_ApplyBoundarySeal` · `VF_ApplyPassageCarving` — the three world
invariants every archetype appends, **moved here 2026-07-27** so the generator and the operator
stack share ONE copy. `VoxelGenerator.cpp` keeps same-named `static FORCEINLINE` forwarders so its
~20 call sites are unchanged; bodies are byte-identical. Also `VoxelDensityReach::SpineBlend` /
`PassageBlend`, the blend radii `ClassifyTile` currently hand-duplicates.
**Convention: INTERNAL (positive = solid).**

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
| `FVoxelOpStack` | — | Ordered `TUniquePtr` list. `PrepareChunk` / `EvalInternal` / `EvalMC` / `ClassifyBox` (the fold, with an early-out when both hypotheses die). |
| `FVoxelOpStack::AppendStructuralPost` | 4 | Appends spine → seal → passage **in that fixed order**. An author cannot omit or reorder them. The diff layer is NOT here yet — it still lives in `GetDensityAt` after the MC negate, with disturbances. |
| `VoxelDensityOps::MakeConstantRockSource` | 1 | `Density = BaseDensity`. `ClassifyBox` → **AllSolid**, exact and free. Shared by TunnelNetwork, Maze, VerticalShafts and bedrock gaps. Class is `FConstantFieldSource` (one class, two factories). |
| `VoxelDensityOps::MakeConstantVoidSource` | 1 | The **same class, negated**: `Density = -BaseDensity`, and `ClassifyBox` → **AllAir** — the first source in the plugin that can prove it. FloatingIslands' root; that verdict is what makes a mostly-empty island strate skippable. |
| `VoxelDensityOps::MakeLatticeCorridorSource` | 1 | Maze corridors, SDF channel. Edge identity = `hash(lower node, axis)` ⇒ adjacent chunks cannot disagree (AUDIT §6.4's preferred pattern). Its `EffectOverBox` answers for the source+carve **pair** (Phase 1 simplification) so it must be told the downstream `ExtraReach`. |
| `VoxelDensityOps::MakeSdfRoughnessMod` | 3 | Wall roughness in **SDF** space (Maze/Shafts/Islands variant). TunnelNetwork's density-space roughness is a **different op** — see OPSTACK-DECOMPOSITION §1. |
| `VoxelDensityOps::MakeSdfCarve` | 2 | SDF → density carve. The same six lines currently copied in three archetypes. Class is `FSdfConvertOp(Sign = -1)`. |
| `VoxelDensityOps::MakeSdfFill` | 2 | The same op with `Sign = +1` — FloatingIslands' `Density += Fill·Base·2`. ±1 multiplication is exact in IEEE-754, so the carve path is bit-for-bit unchanged by the generalisation. |
| `VoxelDensityOps::MakeSlabVoidSource` | 1 | Floor surface + ceiling surface → void field. **XY-pure** since §3.1, which is what gives it an **exact `ClassifyBox` with no sampling**: FBM's `[-1,1]` contract bounds both surfaces into known Z bands. Serves FlatPlain **and** CrystalChamber. |
| `VoxelDensityOps::MakeGridColumnMod` | 3 | Infinite-height cylinders on a world grid, 3×3 cell memo. Adds solid only ⇒ `FillOnly` when a column reaches the box, `Identity` otherwise — and that `Identity` is what lets the source's `AllAir` verdict survive. |
| `FShaftFieldSource` (internal) | 1 | VerticalShafts' memoised inner 3×3 shaft/capsule source. Each rebuild collects 7×7 cells so every emitted inner shaft has its complete 5×5 candidate window; the nearest strictly more-central shaft is the deterministic parent, and a local minimum links to the runtime `(0,0)` spine. Existing `VoxelHash::Pair`/`Spacing*1.6` links remain additive texture/loops. Tree and spine capsules use a radius strictly above `sup|FBM|=1.5`; the structural post still owns the column and the ledge modifier ignores the synthetic endpoint. `EffectOverBox` mirrors both tree and random capsules conservatively. |
| `VoxelDensityOps::BuildSlabStack` | — | 5 ops, **no branch on archetype**: FlatPlain and CrystalChamber differ only in defaults, exactly as `GetSlabDensity` already had it. 8 archetypes → 7. |
| `FSurfaceColumnSource` (internal) | 1 | The bridge between the two spaces: consumes the ground + sky-cap **height** stacks and produces density. `IsXYPure()` **false** — the heights are XY-pure, a distance to them never is. Owns a **six-box spatial LRU** of direct-indexed per-column cells, keyed by `PrepareChunk` on `(StrateBottomWorldZ, LayoutVersion, Seed, ParamsFingerprint)` so it is **shared down the whole vertical strate stack**, exactly like `GSurfColCache`. Six 81×81 boxes preserve hot columns across interleaved regions at roughly 0.79 MiB TLS before padding (more memory, fewer whole-cache recenter/recompute misses). Fractional XY remains direct-compute. |
| `VoxelDensityOps::BuildSurfaceStack` | — | SurfaceWorld, complete: column + overhang + 3 structural, plus biome blending when `PerBiomeParams` is non-empty. Takes ownership of an `IVoxelBiomeField`. |
| `VoxelDensityOps::BuildVerticalShaftStack` | — | 8 ops, and **three are Maze's reused unchanged** (`ConstantRock`, `SdfRoughness`, `SdfCarve`) with different tuning (freq 0.1 vs 0.12, window `rough+4` vs `R+rough+2`). Passes the runtime spine radius into `FShaftFieldSource`, which mirrors the generator's connector-only origin endpoint and roughness-safe spine capsule. The measured proof of `OPSTACK-PLAN §2.5`'s reuse claim. |
| `FRoomGraphSource` (internal) | 1 | TunnelNetwork's SDF spine. **CALLS `BuildChunkCache`/`EvaluateSDFCached` — does not transcribe them**: that is where §8.4's two-region discipline lives and a copy would fork it. Owns the cave warp (scope = this op alone; pits/chimneys read *unwarped* coords, which is why no FRAME op was needed). Its cache key adds a **params CRC + LayoutVersion**; the original lacked them until AUDIT §C2 was fixed (2026-07-28) and now carries them too. **`EffectOverBox` ANSWERS SPATIALLY** since 2026-07-28 — this is the T1.d switch (measured: **6 of 40 tiles proved AllSolid at production defaults, 7986 voxels brute-forced, 0 violations**). It builds the cache for the queried box into a *second* per-worker cache (never `FState::Cache`), then applies a **disjunction** per primitive: it doesn't matter if it **fails its cull** *or* if **its own SDF stays ≥ `T+K`** over the box. Cull wins for rooms (`Rmax+3K` < `Rmax+T+K`); the threshold wins hugely for tunnels, whose cull is a capsule *bounding sphere* (~107 radius for a 200-long tube of radius 7). ⚠️ **`Identity` therefore means `Sdf ≥ T`, not `Sdf == FLT_MAX`**, with `T = max(3·SDFBlendRadius, WormNetworkRange)` — **any new consumer of the `Sdf` channel must have a threshold ≤ T or be added to that max**, or it gets tiles with no geometry and no collision. The `−K` slack covers any number of primitives because `SmoothMin`'s penalty is exactly 0 once `\|A−B\| ≥ K`. Pits/chimneys use the cull only; columns are **not** tested (their sole consumer gates on `Sdf`, so the test was redundant). Verdict memoised per box; warp dilation uses a **provable** `\|Perlin3D\| ≤ 2`. |
| `FWormFieldSource` (internal) | 1 | Fielded 3D-noise threshold carve, masked by distance to the room network (reads `InOut.Sdf` *after* pits/chimneys). **`EffectOverBox` INHERITS `FRoomGraphSource`'s verdict** since 2026-07-28 — its `Eval` sets `NetworkMask = 0` when `CaveSDF >= WormNetworkRange`, which `FLT_MAX` always satisfies, so where the room source proves `Identity` the worm doesn't execute at all. ⚠️ This was **the** blocker: `BaseDensity = 8` < `WormStrength = 10` **by default** (the field comment requires it), so an unconditional `CarveOnly` drove `SolidMargin` negative on every tile in the world and no room-source proof could survive behind it. Deliberately **not** `VF_NoCaveOverBox` — that helper answers "identity" for a null `Rooms`, which is wrong for an op that could sit behind a different SDF writer. |
| `VoxelDensityOps::BuildTunnelNetworkStack` | — | **COMPLETE, 19 ops** — the biggest port in the plugin (~1080 lines), done in three stages: SDF spine (A) → the twelve detail modifiers of 4b–4h (B) → the per-room op override (C). Serves **TunnelNetwork and Underwater** from one builder. Operator order is the original's, line for line, and it is load-bearing (`FFloorBiasMod` exists to undo what `FCaveRoughnessMod` did to floors). |
| `FCaveRoughnessMod` (internal) | 3 | STEP 4b, **density space** — a different op from `MakeSdfRoughnessMod`: two octave sets, optional domain warp, four noise types, an anti-fill clamp inside definite air, quadratic fade. ⚠️ **Reads STRATE params, not the per-room copy** — the original's shadow is declared *after* step 4b. Eleven of twelve modifiers read the room copy; this one does not. |
| `FCaveTerraceMod` (internal) | 3 | STEP 4c. The only modifier that **re-queries the SDF** (Z±1, through `FRoomGraphSource::ProbeSdfUnwarped`) for its horizontality gate — which is why the room source's cache is exposed at all. ⚠️ Those probes use unwarped X/Y and raw Z although the field was evaluated warped: transcribed as-is, see OPSTACK-PROGRESS. |
| `FLayerLineMod` / `FRibbingMod` (internal) | 3 | The same sine along Z: cubed and subtracted (grooves) vs quarter-phase-shifted, squared and added (ribs). `CarveOnly` / `FillOnly` — two of the few detail modifiers that keep a usable direction for the fold. |
| `FCaveOverhangMod` / `FCaveCliffMod` / `FScallopMod` (internal) | 3 | STEP 4c. ⚠️ The cliff's own comment promises a sampled vertical gradient; the **code** uses a Z-stretched Perlin as a proxy and samples nothing. Ported as written — fixing it would change the world. |
| `FCaveArchMod` / `FDomeMod` / `FPinchMod` / `FFloorBiasMod` (internal) | 3 | Room-relative: they read `FRoomGraphSource::GetNearestRoomIdx()` and the cached room. Their gate is `SDF < SDFBlendRadius`, **not** the shared `·3` one — they live in the cave's void, not its wall. |
| `FRoomColumnMod` (internal) | 3 | STEP 4d, and **not** `MakeGridColumnMod`: it walks `SDFCache.Columns`, pre-baked per room. ⚠️ It has **no strate parameter at all** — neither the bake nor the loop reads `FStrateGenerationParams::ColumnDensity`. Columns exist only through a `Column` terrain-op asset in the strate's pool, and the only way to prove they fired is to look at `SDFCache.Columns.Num()`. |
| `FRoomGraphSource::LocalParams()` | — | **The per-room op override** (DECOMPOSITION §2's "no clean home"). Strate params + the nearest room's `UVoxelTerrainOpDefinition::ApplyTo`, memoised once per voxel and read by eleven modifiers. One op owns the shared state, the rest read it — the same pattern as `FOverhangShelfMod` ← `FSurfaceColumnSource`. ⚠️ `EffectOverBox` still answers from STRATE params, so a box verdict can be **too optimistic** on a strate with a terrain-op pool; harmless until `ClassifyTile` consumes `ClassifyBox`, and it must be fixed before that. |
| `FIslandBlobSource` (internal) | 1 | Hash-placed tapered flat-top blobs, `SmoothMin`'d, in a **domain-warped XY frame** (the warp stays inside the op — see the deviation note vs DECOMPOSITION §7). SDF channel only. `EffectOverBox` → `FillOnly` when a blob reaches the box, `Identity` otherwise; its pad must cover warp·**√2** (two independent noise axes), roughness, fill blend and the `SmoothMin` dip. **No lower Z bound exists** — a hairline thread of matter hangs below each island down its axis, so only the TOP may reject. |
| `VoxelDensityOps::BuildFloatingIslandStack` | — | 7 ops, and **the stack runs backwards**: void source + fill instead of rock source + carve, using the *same* classes with the opposite sign. Only the blob source is new. Reuse by **inversion** — a stronger result than reuse by identity, since it says the abstract axis (the density sign) is the right one. |
| `VoxelDensityOps::BuildMazeStack` | — | The 7-op Maze stack. If this ever becomes one op, the refactor failed its own test (§2.5). Callers must skip it on a **degenerate strate** (top−bottom ≤ 0): `GetMazeDensity` early-outs to air there and the stack has no such early-out by design — `GetDensityAt` falls back to the `switch`. |

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
`UVoxelGenerator : UObject` — lightweight; holds `Seed`, a process-unique
`DensityCacheOwnerId`, and injected services `StrateManager` + `DiffLayer` (both nullable).
This is **where terrain shape lives.**

| Symbol | .cpp line | Role |
|--------|-----------|------|
| `UVoxelGenerator` / `DensityCacheOwnerId` | — | Constructor allocates a process-unique integer identity (relaxed atomic, once per object). `GetDensityAt` includes it in the `CP_*` thread-local key, preventing a worker from serving another generator/world's params, biome context, `CP_UseOpStack`, or stack when `(ChunkCoord, LayoutVersion)` happens to match. Hot-path cost: one `uint64` compare per voxel. Scope is deliberately only the proved `CP_*` path. |
| `FractalNoise3D` (static) | 25 | fBM (layered Perlin). |
| `RidgedNoise3D` (static) | 55 | Ridged multifractal — craggy. |
| `CellularNoise3D` (static) | 101 | Worley/cellular — grotto/scallop. |
| `ApplyBoundarySeal` (static) | 170 | Solidifies strate top/bottom shells. |
| `ApplyPassageCarving` (static) | 197 | Punches passages/elevator through the seal. |
| `InitializeSettings` | 211 | Copies seed from settings. |
| **`GetDensityAt`** | 218 | **Entry point.** Picks strate + generator type, dispatches, adds diff offset. Its `CP_*` per-chunk state is keyed by `(DensityCacheOwnerId, ChunkCoord, LayoutVersion)`; every key component is an integer compare and a different generator/world cannot inherit the previous owner's cached params or op stack. |
| **`GetDensityWithParams`** | 277 | TunnelNetwork pipeline (~1000 lines). See §4. ⚠️ Takes **required** `ParamsFingerprint` + `LayoutVersion` since the AUDIT §C2 fix (2026-07-28) — they go into the SDF cache key so a chunk can no longer be evaluated against a neighbour's rooms. Callers compute the CRC **once per chunk** (`CP_TunnelFP`), never per voxel. |
| **`GetSlabDensity`** | 1306 | FlatPlain/CrystalChamber pipeline. See §4.2. |
| `SampleSurfaceStructuralZ` | — | **F20:** the RAW SurfaceWorld heightfield (continents+mountains+detail), BEFORE any terrain op; returns terrain Z + relief M. Cliff re-samples it at an XY offset for a cheap analytic slope. |
| `ComputeSurfaceTerrainZ` / `GetSurfaceDensity` | — | SurfaceWorld heightfield → terrain Z, then density; biome **output-blend** lerps dominant/neighbour heights (`ParamsD`/`ParamsN`/weight). **F20 surface ops** (`FSurfaceGenerationParams`, biome-selected + slope/relief-conditioned, all default off): Cliff (slope-gated STEEPENING — push height from local mean where steep ⇒ sheer walls; 4 structural resamples only when on), Terrace (relief-gated + `TerraceHardness`), LayerLines (sedimentary shelves) — pure per-column height REMAPS applied here so the single height oracle stays consistent (MC/sheets/ClassifyTile/deco/BP bridge). **Phase 2 OVERHANG** (volumetric — real jutting shelves): in `SurfaceDensityFromColumn`, for AIR voxels in a window `(TerrainZ, TerrainZ+OverhangHeight]` above a steep slope, the heightfield is re-sampled UPHILL (toward the cliff) by a reach that GROWS with height (tiny low ⇒ air over the void, full high ⇒ borrows the far cliff rock) and unioned in ⇒ a shelf attached to the cliff, tapering out over the void with air beneath (the sketch). Per-column `OverhangAmp`(=strength·slope-gate) + unit uphill `(DirX,DirY)` resolved once in `ComputeSurfaceColumn` (gradient sampled at the REACH scale so a spot over the void can see the cliff), cached on `FSurfaceColumn`. Genuine 3D (per-voxel structural re-eval, gated to steep overhang columns). Off ⇒ byte-identical. §8.14. |
| `VF_BuildOpStackForChunk` (file-static) | — | **The archetype → stack mapping, written down once.** `GetDensityAt` and `ClassifyTile` both call it; params are passed in, never fetched here. A second copy would be the worst bug available in this file — a tile skipped on the verdict of a stack that is not the one producing its density is a hole. Returns false (⇒ caller falls back to the `switch`) for an unported archetype, missing params, or a **degenerate strate**, since five archetype functions early-out to air there and the stack deliberately has no such early-out. `Refs.Surface == nullptr` makes it refuse SurfaceWorld, which is how `ClassifyTile` keeps its own exact-lattice proof. |
| `ClassifyTile` | — | **T1.d trivial-tile reject** (worker, called by `LoadTile` before `GenerateMesh`): proves a tile AllSolid/AllAir on the mesher's exact lattice (gap chunks + SurfaceWorld columns via the SHARED `GSurfColCache`; seal bands; **cave archetypes via `FVoxelOpStack::ClassifyBox` when the strate opted in** — see §3.2d for the six guards, all failing to `Mixed`; guards: diff mods, passages, spine, disturbances, **F20 overhang** — a column point in `(TerrainZ, TerrainZ+OverhangMargin]` (margin = max `OverhangHeight`) is unprovable ⇒ Mixed, UPWARD only since the shelf union only ADDS rock above ground, so an overhang shelf never holes a trivially-skipped tile) → skip gen. Mixed = generate normally. Its diagnostic-only not-op-stack bail attribution distinguishes a tile wholly inside the disabled slot, a boundary tile, and an unresolved layout; the classifier's conditions/returns are unchanged. §8.10. |
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

### 3.6b Strate measurement — `Public/VoxelStrateMeasure.h` + `Private/VoxelStrateMeasure.cpp`
Headless, read-only Tier 2 measurement pass. `VF_MeasureStrate` samples one strate between a
boundary-seal-derived (or explicitly overridden, including explicit zero) interior margin into a bounded coarse grid,
performs one deterministic 6-connected air flood fill, and derives fractions, components,
walkability, feature scale, and clearance from that grid. Metrics report the resolved margin and
the inclusive/exclusive voxel Z window used, plus the largest component's deterministic lowest-cell
representative point, cell count, and count of components holding at least 1% of the air.
`VF_AreConnected` recovers a deterministic BFS parent path from the same kind of grid and checks
only that route at full voxel resolution before returning `Connected`. It returns an explicit
`EVoxelConnectivityResult`; a solid endpoint cell is repaired by snapping to the nearest air cell
among its 26 neighbours when possible, and reports the snap through its out flags. A missing coarse
route returns `NotConnectedAtThisResolution`: that is evidence only at the grid's resolution,
because a corridor thinner than `SampleStep` is invisible. No UObject state, cache, actor, world,
or PIE is required.
| Symbol | Role |
|--------|------|
| `FVoxelStrateMeasureSettings` / `FVoxelStrateMetrics` | Plain settings/result structs for bounded strate sampling and derived measurements; callers may override the interior margin (including zero to sample the seal), and results identify the resolved margin/Z window and deterministic component representatives. |
| `VF_MeasureStrate` | One-grid/one-flood-fill strate metrics; refuses invalid bounds or a grid over `MaxCells`. |
| `VF_AreConnected` | Coarse 6-connected BFS plus one full-resolution recheck of its recovered route, with explicit endpoint-solid, out-of-window, `CoarseLied`, `NotConnectedAtThisResolution`, and connected outcomes; the diagnostics overload returns endpoint component facts from that same grid. |
| `VF_DiagnoseConnectivity` | The same query plus each mouth's air-component size/share and exact geometric distance to the nearest cell of the other component; proximity is measured in coarse-cell coordinates and does not claim a route through solid. |

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

`MakeStrateSeed` (h:~477) is the shared pure world-seed/strate-index salt used by both
`BuildChunkCache` and the TunnelNetwork/Underwater destination landing queries.
`VF_SuggestLandingPoint` (h:~550, implemented in `VoxelCaveMorphology.cpp`) is the pure landing-site
query: nearest hash-room height at the requested XY for TunnelNetwork/Underwater, slab void midpoint
for FlatPlain/CrystalChamber, and bounded lateral searches for a horizontal lattice corridor (one
Maze cell), lower-seal/ledge-safe shaft (one ShaftSpacing), or validated blob top (one IslandSpacing).
It returns `false` when those feature-bearing conditions cannot be proven inside the supplied budget;
SurfaceWorld remains refused because its production height can depend on manager-owned biome/per-column
context. It never touches a generator, operator stack, manager, cache, or mutable state.

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
content lists, audio, `GameplayTags`(223), `bUseOperatorStack` (the OPSTACK A/B opt-in — only bites
if the archetype is in `UsesOperatorStackForChunk`'s ported list). EditConditions show/hide param
groups by generator type.

**`Public/VoxelStrateManager.h` + `.cpp`** — `UVoxelStrateManager : UObject` (h:108).
Maps depth→strate at runtime; owns passages.
- `FVoxelPassage` (h:39): endpoints, radius, type, control points.
- `FStrateSlot` (h:84): definition + chunk-Z range + index.
| Method | .cpp line | Role |
|--------|-----------|------|
| `Initialize` | 10 | Builds the stacked layout from settings+seed (fixed slots + pool sorted by soft-asset path, then unchanged Fisher-Yates shuffle), logs every **cave** slot whose operator-stack opt-in is disabled, then `GeneratePassages`. SurfaceWorld is deliberately excluded from that diagnostic because its exact-lattice T1.d path does not depend on the flag. |
| `GeneratePassages` | 247 | Deterministic passages between consecutive strates (per-strate `PassageConfig::Style` control points; auto passages retain the existing `EVoxelPassageType` default); placement and shape values are independently salted hashes of seed + boundary slot + connection index. After XY placement, the destination's pure `VF_SuggestLandingPoint` may move the lower mouth within an archetype-specific lattice budget, then the result is clamped outside the destination seal bands and interpolated through the control points; unsupported/no-footing answers warn and preserve the old random reach. |
| `EvaluateModifierSDF` | 357 | SDF of passages at a point (for carving). Per-chunk `thread_local` shortlist (`PassagesVersion`-stamped) → far chunks return `FLT_MAX` without walking `Passages`. §8.10. |
| `AnyPassageNearBox` | — | Conservative sphere-vs-AABB test of every passage's bound against a voxel box (+carve blend pad). Per TILE (ClassifyTile guard), never per voxel. |
| `FindSlotIndexForChunkZ` | 427 | Z → layout index. |
| `GetStrateAt` / `GetStrateIndex` | 443 / 455 | World-Z queries. |
| `GetLayoutVersion` | h:161 (inline) | Layout/passage generation counter (= `PassagesVersion`, bumped by every `Initialize`). Hot-path callers key `thread_local` memos on it (strate-index memo in `GetDensityWithParams`, passage shortlist) so editor rebuilds never serve stale data. |
| `GetStrateForChunk` | 466 | Chunk → definition. |
| `GetGeneratorTypeForChunk` | 476 | Chunk → generator type. |
| `UsesOperatorStackForChunk` | 559 | Chunk → should `GetDensityAt` take the operator stack? `bUseOperatorStack` on the definition **AND** archetype in the ported list — **now all 8 of 8** (Maze, FlatPlain, CrystalChamber, SurfaceWorld, VerticalShafts, FloatingIslands, TunnelNetwork, Underwater). **That list is written down here and nowhere else.** With every archetype ported the flag is now the *only* thing that decides the path, so ticking the box is no longer a no-op anywhere — it is a real switch onto the operator stack for that strate. |
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
| `VoxelForgeClassifyTileTest.cpp` | `VoxelForge.OpStack.BoxVerdictFold` | Pure-logic walk of the fold in `VoxelDensityOp.h`, case by case — including the seal-forces-AllSolid case that justifies `ClassifyBox` existing. Also the only `.cpp` that includes the op header, so the build actually sees it. |
| `VoxelForgeHeightStackTest.cpp` | `VoxelForge.OpStack.SurfaceHeightEquivalence` | The height-space stack vs `ComputeSurfaceTerrainZ`, in **altitudes**. Runs twice: defaults, then **all F20 terrain ops ON** — the load-bearing pass, since the ops are off by default and the defaults pass exercises only the structural source. Also brute-forces `MaxDisplacement` (a false bound would be a hole). Bar is bit-identity; a height delta is a visibly different world, not rounding. |
| `VoxelForgeCrossPlatformTest.cpp` | `VoxelForge.Determinism.CrossPlatformDigest` | SHAPE digest (sign of density = the world) + FIELD digest (bit-for-bit) over a fixed integer grid, plus `NearIso` bounding how many samples could flip sign. Reports rather than asserts until pinned. Run on Windows and Linux and compare. |
| `VoxelForgeOpStackSlabTest.cpp` | `VoxelForge.OpStack.SlabEquivalence` | **Phase 2's first port.** The same 5-op slab stack vs `GetSlabDensity` over 20k points, run twice — FlatPlain **and** CrystalChamber — which is what demonstrates the two archetypes really are one op. Plus window-invariance and box-verdict brute force. Compares against the reference **as it is now** (post Z-term removal), so green = pure refactor and any visual delta is attributable to §3.1 alone. |
| `VoxelForgeOpStackTunnelTest.cpp` | `VoxelForge.OpStack.TunnelNetworkSpineEquivalence` | **Stage A of the last port.** Zeroes the 13 detail-op amplitudes so the *original* takes the path stage A ported — that is what makes an incomplete stack verifiable now. Samples in **clusters** (24 chunks × 250 points), because the SDF cache rebuilds when a query leaves its box and uniform sampling would rebuild per point on both paths. Check 3 (two param sets, A/B interleaved) compares each stack **to itself alone, never to the original** — the original would fail it, see AUDIT §C2. Asserts **zero** box verdicts, which is the honest stage-A result. |
| `VoxelForgeOpStackShaftTest.cpp` | `VoxelForge.OpStack.VerticalShaftEquivalence` | The port that tests **reuse**, not fidelity: three of the five ops are Maze's, unchanged. Forces connectors + ledges on, because both are off or negligible at defaults and a resting param is an untested operator. The source and mirror now share a deterministic inner-3×3-emitted drainage tree whose parents come from rebuild-only 7×7/5×5 windows, plus the old random links. **Fixed 2026-07-29:** its `EffectOverBox` used to return `CarveOnly` because a shaft merely *existed* within a `Spacing*1.6` halo — true almost everywhere at `ShaftSpacing 55 / ShaftDensity 0.6`, hence **0 of 60** tiles. It now tests tree and random capsules (same row-major cell order ⇒ same `VoxelHash::Pair`, so symmetry of `Pair()` is not assumed), with **Z exact** and XY conservative. The sampler was also widened from ±48 voxels to ±440. Every proved tile is brute-forced over its full lattice, and the test reports rebuild-vs-hot cache cost. |
| `VoxelForgeOpStackIslandTest.cpp` | `VoxelForge.OpStack.FloatingIslandEquivalence` | The port that runs the stack **backwards** — void + fill vs rock + carve, same classes with the opposite sign. Counts interior-solid and open-void samples separately (on this archetype an aggregate "N solid" is dominated by the seal bands and says nothing about the islands). Counts `AllSolid` and `AllAir` verdicts **separately** too: `AllAir` is the one no cave archetype could ever prove, and it is the entire perf argument here. |
| `VoxelForgeOpStackMazeTest.cpp` | `VoxelForge.OpStack.MazeEquivalence` | **Phase 1's load-bearing test.** The 7-op Maze stack vs `GetMazeDensity` over 20k points (aiming for bit-identity; a side-of-iso disagreement is the hard fail), plus purity across workers and brute force on every box verdict the stack emits. Reports how many tiles the stack can prove uniform — today's `ClassifyTile` proves **zero** for any cave archetype. |
| `VoxelForgeStrateParamCoverageTest.cpp` | `VoxelForge.Determinism.StrateParamBlendCoverage` | **The X-macro guard** (added 2026-08-17). `FStrateGenerationParams::Lerp` blends the hand-written `VF_STRATE_PARAM_FIELDS` list, **not** the struct — so a field added to one and not the other compiles, tests green, and silently takes its **default** inside every Gradient/Interleaved transition band. This expands the X-macro a **third** way (after LERP and SNAP), into a name list, and diffs it against the struct's UObject reflection. Pure shape test: no fixture, no world, instant. `GExemptFieldNames` is **empty** — every reflected field is covered today, and any exemption must be written down as a decision. Stakes rise with the world composer, which intends to invent parameter sets through this same `Lerp` (`COMPOSER-NOTES.md`). |
| `VoxelForgeLayoutOrderIndependenceTest.cpp` | `VoxelForge.Determinism.LayoutOrderIndependence` | Builds a known transient soft-pointer pool, then rebuilds it in original, reversed, and swapped orders. Requires a non-empty layout and passage set, and compares every slot's definition/Z/height plus passage endpoints, radius, type, control geometry, and bounds bit-for-bit. |
| `VoxelForgePassageOpenSpaceTest.cpp` | `VoxelForge.Determinism.PassageLandsInOpenSpace` | Uses the real fixture density path to check every generated inter-strate passage whose destination query answers: a 16-point ring outside the mouth's carve/blend band has at least half its samples in destination air, and the endpoint matches the pure open-point result within the mouth's float envelope. This is a connectivity proxy, not a flood-fill proof. Reports checked passages and false/unanswerable archetypes; fails if it inspects zero passages. |
| `VoxelForgeStrateConnectivityTest.cpp` | `VoxelForge.Generation.StrateConnectivity` / `VoxelForge.Generation.StrateConnectivityRefinement` / `VoxelForge.Generation.VerticalShaftSeamFreedom` | Bounded strate metrics with density-polarity and solid-gap controls, deterministic route rechecks, and refinement sweeps. The refinement test explicitly checks margins 0/2/4/8 at step 2/radius 192, reports components/largest-component share, the `(0,0)` spine's full-window/interior vertical run and component, plus arrival/departure→spine and arrival→departure enums across 16 VerticalShafts seeds. The seam test re-evaluates cell-boundary positions after warming distinct neighbouring chunk contexts in both legacy and operator-stack paths. |

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
