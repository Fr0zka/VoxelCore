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

### Open design questions for the owner
- All five strate slots resolve to `DA_Strate3`. Intended?
- Should a composer roll be allowed to overwrite an explicitly authored value?
