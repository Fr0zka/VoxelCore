# WORK-NEXT — the queue after the read-only audit (2026-09-11)

Ordered by importance, then by size. Everything here comes from Sol's read-only audit of
`experimental` @ `aa62971`, checked against `main`.

The audit's two-sentence verdict:
> *The operator abstraction leaked into the innermost loop, and streaming treats
> "work submitted/applied" as "safe to stand on."*

**Standing rules for every item**: determinism is absolute (same build + same seed = same world on
every machine); output may change between builds; nothing is deleted to go fast; capability before
speed (player-fit 20,830 / walk-reachable 10,909 — do not drop below); scope is
`Plugins\VoxelForge` only, no links, absolute `-out=` paths under the plugin's own `Saved\`.

---

## 1. Collision-ready before player-ready  ·  CORRECTNESS  ·  medium

The player falls through the world. There is **no invariant** that a tile you occupy has collision.
`main` has no such barrier either — it was simply fast enough to hide the race.

- `apply_s` ends at mesh **submission**. RMC cooks collision **asynchronously** and we never record
  cook-complete. We have never measured when physics can actually collide.
- A tile enters `LoadedTiles` before its apply path completes (`VoxelWorld.cpp:1532`).
- Only LOD0 carries collision (`VoxelWorld.cpp:3109`), so visible coarse terrain can exist while the
  collidable tile is still queued.

**Do**: record cook-complete; gate the pawn (spawn and movement) on the supporting tile being
collidable, not merely applied.

✅ **Owner playtest, 2026-09-13: he can no longer fall through the world.** The gate uses RMC's
`SetCollisionConfig()` future as the authority (`HandleTileCollisionCookComplete`,
`IsPlayerSupportCollisionReady`).

📖 **Reading `Plugins/RealtimeMeshComponent` is permitted for this item — READ ONLY.** Find whether a
cook-complete delegate/flag exists. ⛔ No edits, no builds against it: the owner's copy carries custom
modifications and was destroyed once by an over-broad delete.

## 2. Lower the operator graph into a fused evaluator  ·  PERFORMANCE  ·  large

This is where the ~8× lives (pre-op-stack `34f06df` did 128³ in 0.130 s; we are at ~1.04 s).

A LOD0 tile samples 42,875 positions. The old path short-circuited the whole cave suite per sample
when `bNearCaveSurface` was false — roughly **70% of deep-solid samples**. We now allocate a rich
`FVoxelOpSample` per position, run each active operator across the entire block, use scalar fallbacks
for most operators, copy scratch in and out, and still call scalar `GetDensityAt` per grid point.

> *An operator whose interval intersects the surface anywhere in the block runs across the whole
> block; the old path could decide per sample that expensive detail work was irrelevant.*

**Do**: keep the authored operator graph, but **compile/lower it** into a fused evaluator that restores
per-sample short-circuiting and stops spilling `FVoxelOpSample` through memory per operator.
⚠️ Byte-identical geometry is the acceptance test. Stage it so the tree never breaks.

Note: the D-floors/terracing features are only ~17% of the penalty — not the 8×.

## 3. Cancel obsolete work, and prioritise the floor  ·  BOTH  ·  medium

Nothing aborts a tile when the player moves and it stops being desired. Work is distance-sorted from
an artificial chunk centre, not the pawn, with no collision-critical priority
(`VoxelWorld.cpp:1864`). Up to 14 workers can be busy with stale coarse tiles while the player's LOD0
floor waits.

**Do**: abort no-longer-desired tiles; sort from the true pawn position; give the occupied tile and the
next tile along the heading absolute priority.

## 4. Collapse the duplicated field semantics  ·  PERFORMANCE ARCHITECTURE  ·  medium-large  

The effective field now lives in three places: the operator stack, the classifier's parallel interval
logic, and a post-stack repair tail in `GetDensityAt` (origin floors, passage structure, passage
floors, room floors). `FRoomGraphSource` evaluates the room graph then traverses tunnels *again* for
metadata (`VoxelDensityOpStack.cpp:4234`).

The audit calls this *"the clearest sign of overengineering"*: later fixes compensating for where an
operation sits in the pipeline. It also makes interval proofs harder to trust.

**Do**: make the stack the single semantic source of truth. Likely overlaps heavily with item 2.

**Promoted 2026-09-12**: this is performance architecture, not maintenance. The classifier is a
second evaluator (interval proofs, exact certificates, and final-field validation) beside the
canonical lowered evaluator, and the runtime can validate the same tile again before meshing.
Items 1 and 2 in the current round deliberately leave this lowering work untouched; any
classifier retained later must consume the lowered representation and reusable density data.

## 5. Verdict cache removed after moving-session audit  ·  complete

The clean moving-session audit found zero outer or nested hits: 2,109 outer probes and 19,904 nested
probes produced 15,877 stores and 15,365 replacements in the full 512-entry table. Same-binary
cache-on/cache-off request-to-ready was 0.367645/0.365978 s p95 and generation was 0.285559/0.285992
s p95, so the cache was neutral within run noise while paying the global lock and linear scans.

The global verdict cache, lock, entry table, invalidation path, cache-only profiler fields, and
deliberate cache probe are removed. The proof classifier remains; item 4 is still deliberately untouched.

## 6. Three small, sharp ones  ·  tiny

- **The zero-position guard.** Streaming only starts when `GetPlayerPosition()` is not exactly
  `FVector::ZeroVector` (`VoxelWorld.cpp:1167`, `:1359`) — a pawn at the world origin is
  indistinguishable from "no pawn", and **the owner spawns at (0,0)**.
- **`ApplyMeshToTile` has no success return** (`VoxelWorld.cpp:3064`). If runtime-mesh init fails the
  tile stays recorded as loaded and never retries — a permanent hole.
- **Whole-tile uniform verdicts are trusted without exact validation.** Block verdicts get checked
  against the full grid when meshing; an outer AllAir/AllSolid verdict has no equivalent safety net.

## 7. Deferred, with reasons
- **Coarse-first LOD refinement** — blocked until coarse tiles can carry collision, or item 1 lands.
- **GPU evaluation** — right hardware, but bit-exactness across vendors/drivers is unproven and
  determinism is absolute. Item 2 is the honest prerequisite either way.
- **The origin spawn room** — the owner considers it a design mistake that should not exist. Design
  conversation, not a fix.

---

## Outer-classifier A/B — measured 2026-09-12 (nested refinement OFF in every mode)

Clean, diagnostics off, headless game path. Static = 343 LOD0 samples, two runs per mode; moving =
160 m at 8 m/s, 1,274 LOD0 samples. Density calls = validation + mesher.

| mode | ready p95 static | gen p95 static | worker CPU static | density calls static | ready p95 moving | gen p95 moving | density calls moving |
|---|---:|---:|---:|---:|---:|---:|---:|
| 0 no outer classifier | **182.4-182.7 ms** | **124.9-127.0 ms** | **184.9-193.5 s** | **18.05 M** | **198.9 ms** | **129.9 ms** | **57.7 M** |
| 1 current (validate, discard) | 189.7-194.8 ms | 139.1-140.6 ms | 201.2-202.3 s | 21.89 M | 208.0 ms | 143.4 ms | 69.6 M |
| 2 validation-grid reuse | 190.0-194.7 ms | 140.3-141.3 ms | 201.2-201.3 s | 21.89 M | 210.7 ms | 143.3 ms | 69.6 M |

**Verdict: the outer classifier is net-negative on the game path.** Mode 0 is better on every latency
figure and does ~18% fewer density calls. It cannot remove geometry, since it skips nothing.

**Mode 2 cannot win as designed.** It reuses the validation grid only when a uniform verdict is
*disproved* (`VoxelWorld.cpp` ~4012), and validation disproved none: 0 reuse tiles in every run. Mixed
tiles, the ones that actually need a grid, never run validation, so there is nothing to reuse.

**Done 2026-09-13, owner approved:** `GVoxelForgeOuterClassifierMode` now defaults to 0, and mode 2
is deleted along with its reuse grid, its mesher parameter and its counter. Mode 1 is still
reachable with `voxel.OuterClassifierMode=1` as an A/B instrument. Verified:
- export OBJ SHA-256 unchanged (`b3e5f4c3..9b377`)
- a run without the override prints `outer_classifier_mode=0 validation_density_calls=0`
- **841/841 tiles have identical triangle counts between mode 0 and mode 1**. The 394 tiles mode 1
  skipped as uniform produce 0 triangles in mode 0, so the strate band cut was not hiding geometry
  behind the classifier.

⚠️ **Correction to `f204413`.** Its message says the nested classifier's 3.5x "cannot be reproduced".
That was my measurement error: I passed `-voxel.UseBlockEarlyOut=1` (the STREAMING console variable)
to the export commandlet, which reads its own `-blockearlyout=` argument, so both of my runs had
nested refinement off. Re-run with the right flag: `-blockearlyout=0` 0.553 / 0.572 s,
`-blockearlyout=1` 2.036 / 2.060 s, which is **3.6x**, with byte-identical geometry. The 3.5x was real,
and disabling nested refinement in streaming (`f204413`) was worth it.

## Room-cache work, 2026-09-13 (`2990009`, `2de603c`, `cfad3d1`)
Player-fit memo, then the tile-sized room cache gated to LOD3+ (fused) / LOD4+ (op-stack). Against
`2990009`, static worker CPU is 48 -> 39 s and moving 125 -> 114 s, with identical geometry.

⚠️ **Correction to `cfad3d1`.** Its message says absolute numbers drift because "the machine is in
daytime use." That was my unverified guess, and it is false: the owner had not used the PC all day.
The real signal is systematic. The switch-off baseline came out at 58 s in two rounds hours apart,
against 48 s at `2990009`. So something left ON with `voxel.TileCacheWindow=0` costs ~10 s, and LOD0
generation p50 is ~4% slower than at `2990009` (73 -> 77 ms static). Queued: attribute it with
in-tree switches (first suspect: `voxel.SpatialIndex` auto-on at fused LOD1-2), never by running old
commits.

**Resolved 2026-09-13 (`ab4bbe3` + the switch sweep):** not the spatial index; every `SpatialIndex`
setting is within ~1%. `TileCacheWindow=0` at HEAD is a different configuration from `2990009`, not
the old path. It still thrashes at L3/L4 (p95 ~250 ms), now at 54 s. The DEFAULT beats `2990009` on
every metric: static worker 48 -> 34 s, moving 125 -> 101 s, LOD0 generation p50 73 -> 66 ms static
and 70 -> 62 ms moving. The "LOD0 ~4% slower" worry is gone. `SpatialIndex` now defaults to 1
(marginally best, within noise). Per-tile identity: 0/841 tiles differ against traces recorded
before the always-on LOD0 changes.

**Next LOD0 target, found by the sampler's caller aggregation:** the "unknown" ~13.8% LOD0 leaf is
`ucrtbase.dll!_finite`, the non-inlined CRT call behind `FMath::IsFinite`. Its callers are the tunnel
chain, passage landing/floor checks and `GetDensityAt`. An inline bit test is exactly the same
predicate, so it changes no field.

## State after 2026-09-14 (HEAD `7a972f5`)

**In-game generation since the start of 2026-09-13**, headless game path (static = 841 tiles;
moving = 160 m at 8 m/s):
```
                                   worker CPU static   LOD0 generation p50 static
c65b015  classifier off            ~184 s              ~75 ms
2990009  player-fit memo             48 s                73 ms
cfad3d1  tile cache window (LOD3+)   39 s                77 ms
ab4bbe3  field-exact LOD0 overhead   33 s                66 ms
5489b5e  inline exact IsFinite       ~31-34 s            ~62-66 ms
7a972f5  (current)                   ~34 s               ~66 ms
```
The later rounds' absolute figures come from slightly different harness settings. Compare within a
round.

**Measured, not guessed: what each component costs** (`6ccf371`, remove-one ablation, static worker
saving): tunnel SDF 11.4% (and it owns the LOD0 p95 tail), cave warp 10.3%, passage carving 10.1%,
landing posts 9.9%, structural posts 5.6%, worms 2.4%, everything else <= 2.2%. Ablations change
the field, so these mix computation cost with the cost of the geometry each component creates.

**Tried and not kept:**
- the exact worm block skip: 4% skip rate, neutral, off by default;
- value-noise / lattice worms: no gain, removed;
- exact culling (SmoothMin cutoff, tile candidates, post early-outs): the build was 23% slower even
  with every switch off, parked on local branch `wip/exact-cull-20260914`. Only the tunnel-core
  ablation gate fix was ported.

**Measurement lessons**, recorded because each cost a round:
- line-level sampler attribution of inlined code is a hint; confirm it with an ablation;
- a switch-off A/B inside a new build cannot see always-on cost; always compare the final default
  build against the committed tip's DLL, with a fixed command line;
- launch Unreal serially (it is a GUI exe that returns at once), and reap crash reporters.

**Next candidates:** a cheaper cave warp (field-changing: coarse warp lattice); the tunnel SDF (field-
changing merge of the warped SDF and the world-space core); a real per-tile candidate filter
designed to cost nothing per sample; the cold read-only audit's findings.

### Open design questions for the owner
- All five strate slots resolve to `DA_Strate3`. Intended?
- Should a composer roll be allowed to overwrite an explicitly authored value?
