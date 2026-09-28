# VoxelForge — Review Findings (2026-06-23)

Local-LLM cross-file QUALITY review (Qwen3.6-27B @128K, codemap-guardrailed) + Claude
verification. Scope: performance, redundancy, dead code, over-complexity (not correctness bugs).
**Verdict: codebase is healthy — no perf regressions.** Items below are improvements, by appetite.

> **Reconciled against `fable-idea.md` + the perf-pass history (2026-06-23).** The big density/
> streaming perf wins are already SHIPPED and are *not* listed here (T1.a surface cache, T1.b
> grid normals, T1.c LOD0 collision, CullTiles spiral fix, region foliage, passage shortlist).
> Everything below is **net-new** (redundancy / dead code / complexity — a different axis), except
> the two notes flagged inline. Before acting on a PERF item, glance at `fable-idea.md` Part I.

Legend: ✅ verified against code · ◻️ checklist box.

> **2026-07-04 full-codebase pass (Fable 5)** — applied everything marked `[x]` below, plus NEW items
> found reviewing the un-built two-grid deco + density-volume work:
> • **Deco launch stall fix** — a pending cell already in flight from a previous build was dropped,
>   permanently stalling its region build (blank region until strate change). Now deferred + retried.
> • **Capture gating** — level-0 tiles outside the shadow window no longer pay the mesher capture
>   (quantize + 32 KB queue payload) nor pollute `CaptureCache` (`IsTileCaptureUseful`, both ends).
> • **Per-tick material pushes change-detected** — `UpdateTerrainMaterialParams` (10 vectors + 3
>   textures × every terrain MID) and the orb MPC writes now no-op on idle frames; static `FName`s.
> • **Landmark cave march bounded** — `FindLandmarkColumn` now honours `ColDepth` (same bedrock
>   early-stop as the deco march); it ran the full strate band synchronously on the game thread.
> • **`UploadDirtyTextures` now calls `EnsureTextures`** — GPU upload toggled ON at runtime works.
> Deliberately NOT done: the big behavior-preserving splits below (un-built tree; compile risk).

> **2026-07-04 perf pass 2 (per-voxel hot path, Fable 5)** — ✅ BUILT & WORKING (ticked 2026-07-27). All
> bit-identical (same hashes/math, hoisted per chunk/cell):
> • **DiffLayer snapshot API** (`HasAnyMods`/`GetModsVersion`/`GetChunkModsSnapshot`/static
>   `EvaluateMods` + `ModsVersion` atomic) — `GetDensityAt` snapshots a chunk's mods once per
>   (chunk, version) via a `thread_local` 64-slot cache: ~27 lock ops per tile task instead of
>   ~86k once any carve exists.
> • **Room shape pre-bake** — `FCachedRoom::ShapeType/ShapeA/ShapeB/ShapeR` baked in
>   `BuildChunkCache`; **`EvaluateSDFCached` signature changed** (`RoomShapeVariety` removed,
>   8 call sites updated).
> • **Strate-index memo** in `GetDensityWithParams`, keyed (chunkZ, `GetLayoutVersion()`) —
>   new inline getter on `UVoxelStrateManager`.
> • **Per-cell lattice bakes** (`thread_local`, keyed cell+seed+params): slab columns, maze open
>   edges, vertical shafts + cross-connectors, floating-island constants, disturbance
>   chasms/bridges/ridges.
> • **Worm N2 short-circuit** — skip the 2nd Perlin when N1 ≥ WormThreshold (lossless).
> **Regression fixed same day:** pass 1's skip-if-identical cache on the orb MPC writes broke the
> mini-sun lighting (MPC world instances reset behind the writer) — REVERTED, `LastOrbMPC`
> removed; never de-duplicate writes to externally resettable state. The MID-side change
> detection stays (MIDs own their values).

> **2026-07-04 batch 3 (Fable 5)** — Jahni green-lit multiple changes per build + visual deltas
> ("nothing is set in stone"). ✅ BUILT & WORKING together with pass 2 (ticked 2026-07-27):
> • **Terracing gradient Z-only** (6→2 SDF samples — see the ticked item above).
> • **Lerp X-macro** + **BakeRoomFeature dedupe** (both ticked above, bit-identical).
> • **T2.b / T2.c / T2.d and F2** shipped in the same batch — tracked in `fable-idea.md` (Tier 2
>   status, F2) and ARCHITECTURE §8.10, not repeated here.

## Performance
- [x] ~~**`VoxelGenerator.cpp` (terrain-op gradient)** — compute once per voxel and reuse.~~
  **RE-VERIFIED 2026-07-04: misdiagnosed.** The 6 `EvaluateSDFCached` calls in the terracing block are
  six DISTINCT sample positions (±1 on each axis) for one central-difference gradient — computed once
  per voxel already, gated to terrace-enabled rooms near surfaces. Nothing is redundantly re-evaluated.
  *Optional lossy halving:* **APPLIED 2026-07-04 (Jahni approved visual deltas).** SDFs are
  ≈unit-gradient, so `Horizontality = clamp(|SDF(Z+1)−SDF(Z−1)|/2)` — the 4 X/Y samples are gone
  (6→2 SDF evals per terrace-voxel); terraces fade slightly differently on SmoothMin'd slopes.
- [x] **`VoxelContentManager.cpp` (BuildCellSpawns)** — slope-gate cosines now precomputed per entry
  (`CosMaxSlope`/`CosMinSlope` arrays), bit-identical gating. *(2026-07-04)*
- [x] **`VoxelContentManager.cpp` (LaunchDecoTasks)** — head-index drain + one `RemoveAt(0, Head)`
  compaction (was O(N) per pop). Bonus fix: a pending cell still in flight from a previous build is now
  DEFERRED, not dropped (dropping stalled its region build forever → permanently blank region). *(2026-07-04)*
- [x] **`VoxelStrateManager.cpp`** — linear `BoundRadius` stored on `FVoxelPassage`; shortlist no longer
  Sqrt's per passage. *(2026-07-04)*

## Redundancy (factor out)
- [x] **`VoxelCaveMorphology.cpp`** — Pit/Chimney/Column baking loops → shared `BakeRoomFeature`
  hash-placement skeleton (gate/XY/radius chain) + per-type Emit lambda. Bit-identical (same salts,
  same hash order). *(2026-07-04, batch 3 — Jahni green-lit batching without intermediate builds)*
- [x] **`VoxelStrateTypes.h`** — `FStrateGenerationParams::Lerp` now expands the
  `VF_STRATE_PARAM_FIELDS` X-macro (all 74 fields verified present — no drift had happened yet).
  New struct fields MUST be added to that list. Bit-identical. *(2026-07-04, batch 3)*
- [x] **`VoxelWorld.cpp` brush methods** — Carve/Fill sphere now funnel through `ApplyModification`
  like Box/Capsule already did (one diff-layer + remesh dispatch). *(2026-07-04)*
- [x] **`VoxelContentManager.cpp`** — duplicated drain/reset loops → `DrainDecoResults()` +
  `ResetGridBuildState(FDecoGrid&)`. *(2026-07-04)*
- [x] **`EVoxelPassageType` vs `EVoxelPassageStyle`** — ✅ **CLOSED 2026-09-04 as WONTFIX**, enacting
  the 2026-08-16 judgement below, which recommended closure and was then left open by oversight. No
  code change; the two enums stay.
  Two overlapping passage-shape enums, both in
  active use (11 refs). Consider consolidating to one. *(judgment call, not dead)*
  **JUDGED 2026-08-16 — LEAVE THEM. Recommendation: close this item rather than act on it.** They
  read as duplicates from the index and are not: `EVoxelPassageType` (`VoxelStrateTypes.h` ~42) is
  the **global inter-strate bore shape** the layout generator picks (`SlopedTunnel` / `VerticalShaft`
  / helix…); `EVoxelPassageStyle` (~1741) is **per-strate descent styling** on
  `FStratePassageConfig` (`Straight` / `Worm` / `Spiral` / `Cascading`). Different owners, different
  value sets, different lifetimes. And both are `UMETA`-tagged, i.e. **serialised into Jahni's
  authored strate assets** — merging them silently rewrites saved content. That is a content-risk
  change bought for cosmetic tidiness, which is the wrong trade at any time and especially before
  the content lock.

## Dead code
- [x] **`UVoxelMarchingCubesMesher::GetDensity()`** — removed, along with `InterpolateEdge`,
  `ComputeGradientNormal` and `GradientOffset` (all dead since T1.b). *(2026-07-04)*
- [x] **Vestigial `MidPoint`** — `MidPoint`/`bHasMidPoint` fields + the unreachable debug-draw branch
  removed. *(2026-07-04)*
- [x] **Codemap-known dead/legacy** — `GetLODForChunk`, `LODToStep`, `IsChunkInRange`, `LOD0Distance`,
  `LOD1Distance`, `ContentMaxLevel`, `VoxelChunk.h` (whole file — `FVoxelTileKey` is the identity now)
  all removed 2026-07-04. `MaxLODLevel` / `DecorationActorRadiusChunks` were already replaced by the
  two-grid deco redesign (`StreamTier` / `DecorationNearRadiusChunks`).
  **CORRECTION: `GetStrateChunkZBounds` is NOT dead** — `BuildDesiredTiles` uses it for the
  strate-aware vertical clamp; struck from the dead list.
- [x] **2026-09 cleanup (`cleanup/2026-09`)** — the far sheet ring runtime (`GenerateSheetMesh` + its
  streaming path) and ~40 unreferenced functions/members/types removed (each proven by a search of
  `Source/`, `Tools/`, `Config/` and build files; see that branch's `refactor(dead-code)` commit).
  Reflected (`UPROPERTY`/`UFUNCTION`/`UENUM`…) and enum candidates were NOT removed — see
  *Owner decisions* below.

## Owner decisions — unused but kept (2026-09 cleanup)
Nothing in `Source/` reads these, but assets, Blueprints, tools or tests may reference them by name,
so removing them is the owner's call.
- [ ] `UVoxelSettings::bFarSheetRing` / `FarSheetSpanLevels` — runtime always passes `false` since the
  sheet ring was removed; still logged in the startup config line, written as `far_sheet_ring` in the
  tile-hash dump, and toggled by `voxel.TestFarSheetRing` (all kept because `Tools/VoxelForgeTest.ps1`
  and the clipmap tests use them). `VoxelClipmapDesiredTiles`' sheet branch is test-only now.
- [ ] `UVoxelSettings::CeilingViewMultiplier` / `CeilingBandChunks` — never read.
- [ ] `UVoxelSettings::DensityVolumeMaxTasks` — only printed in the startup config log (fill runs on its
  own thread).
- [ ] `UVoxelStrateDefinition::WaterColor`, `Creatures`, `AmbientSound`, `Music`, `MusicVolume` and the
  `FStrateCreature` / `FStrateAmbientActor` spawn fields (`SpawnChancePerChunk`, `MaxPerStrate`,
  `SpawnWeight`) — authored data with no consuming system yet (F9 audio, creatures).
- [ ] `UVoxelBiomeDefinition::WaterMaterial` — never read; the water plane uses the strate's
  `WaterMaterial` only.
- [ ] Plain C++ enums with no user: `EVoxelFace` (`VoxelTypes.h`), `EVoxelSeasonSelectionReason`
  (`VoxelSeasonManifest.h`), `EVoxelOpCombine` (`VoxelDensityOp.h` — its comment carries the
  two-channel sign convention; keep that text if the enum goes).
- [ ] `IVoxelHeightOp::MaxDisplacement` / `FVoxelHeightStack::MaxTotalDisplacement` — no caller; kept as
  the documented hook for a future heightfield `ClassifyBox`.

## Over-complexity (behavior-preserving splits, optional)

> ⛔ **REASSESSED 2026-08-16 — do not pick these up as filler work.** They were written before the
> operator-stack refactor existed, and two of them are now actively counter-productive rather than
> merely optional. Read the reason before ticking anything here.

- [ ] ~~`GetDensityWithParams` (~600-1000 L)~~ → **DON'T.** ⛔ Two independent reasons. (1) The
  operator stack is *replacing* this function archetype by archetype — splitting it produces code
  that gets deleted, and churns the eight equivalence tests that compare the stack against it **bit
  for bit**. (2) It is the hottest path in the plugin and carries the `ARCHITECTURE §8.10`
  invariants (`thread_local` box-valid caches, two-pass MC loop, SSE noise). The one time a
  "behaviour-preserving" change was made here it silently deleted the overhang and only 1 sample in
  20 000 crossed the isosurface — *a perf change can be a correctness change*. Revisit only once the
  `switch` path is retired for good.
- [ ] ~~`BuildChunkCache` (~450 L)~~ → **DON'T, same reason.** `FRoomGraphSource` deliberately
  **calls** `BuildChunkCache`/`EvaluateSDFCached` instead of transcribing them, precisely so there is
  one definition. Restructuring it now forks the thing that was kept unforked on purpose.
- [ ] `GenerateMesh` (~250 L) → `PrecalcDensityGrid`/`MarchCells`/`GenerateSkirts`.
  ⚠️ Still genuinely optional, but the two-pass loop is an `§8.10` invariant — a split must not
  merge the passes, and the Z-outermost pre-sample order is load-bearing for every column cache
  downstream (see the column-memo work of 2026-08-16). Low value, non-zero risk.
- [ ] `GetGenerationParams` (~180 L) → extract `ApplyBoundaryTransition(...)`. **The safest of the
  six** — pure params math, no caches, and `AUDIT §C2` already forced a close reading of both
  Gradient arms. If any of these is ever worth doing, it is this one.
- [ ] `GeneratePassages` (~150 L) → `ComputePlacement`/`BuildControlChain`/`ComputeBounds`
- [ ] `BuildCellSpawns` (~150 L) → `FindSurfaceCrossings`/`PlaceDecorationsAtCrossings`

---
*Full method notes + raw per-pass output: `E:\LocalLLM\reviews\QUALITY_VoxelForge.md` (+ `QUALITY_pass1/2/3`).*
