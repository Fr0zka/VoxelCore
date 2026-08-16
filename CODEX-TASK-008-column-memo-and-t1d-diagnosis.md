# Codex task 008 — (A) fix the measured column-memo thrash, (B) diagnose why T1.d never fires in game

**Owner:** Codex (Model Luna, xHigh) · **Orchestrator:** Claude · **Branch:** `experimental`
**Status:** specified, not started

Two independent changes in different subsystems, deliberately bundled into one build because their
signals cannot contaminate each other: (A) is SurfaceWorld column caching, (B) is a counter in the
cave branch of `ClassifyTile`.

---

# PART A — replace the hashed column memo with a direct-indexed box

## This is now MEASURED, not suspected

`stat VoxelForge` in the running game, SurfaceWorld-dominated flight:

```
Column Memo Hits    avg 102,300.84
Column Memo Misses  avg  17,683.60      → miss rate 14.7%
```

Predicted **14.0%** if the table thrashes, **1.4%** if it does not. It thrashes. Second confirmation
from a different statistic: 17,683 misses ÷ 2.13 tiles meshed = **~8,300 column recomputes per
tile**, where a healthy cache does ~1,225 — **6.8×**.

**The cause.** `FSurfaceColumnSource::GetColumn` (`VoxelDensityOpStack.cpp` ~615) uses a
**direct-mapped, 4096-entry hashed** table. A direct-mapped table evicts on *collision*, not on
fullness: 1225 columns per tile in 4096 slots is a load factor of 0.30, at which ~317 columns (26%)
share a slot and evict each other — **on every one of the ~35 Z planes**, because the mesher
pre-samples Z-outermost (`VoxelMarchingCubesMesher.cpp` ~226). Each miss recomputes the entire height
stack: structural source, cliff (four structural resamples), terrace, layer-line, beach, ceiling.

## The fix — copy the scheme that already works, one file away

`GSurfColCache` / `FSurfaceColumnBox` in `VoxelGenerator.cpp` (~152, and its use at ~737) is the
original path's solution to the identical problem: a **direct-indexed box** —
`CI = (IY - Box.BaseY) * Dim + (IX - Box.BaseX)` with a `Computed[CI]` flag — centred on the first
sample and rebuilt when a query leaves it. **No hash ⇒ no collisions ⇒ every column computed exactly
once.** Read it before writing; you are porting a proven scheme, not inventing one.

Apply the same structure inside `FSurfaceColumnSource`, keeping it `thread_local`.

## ⚠️ The invariant that must not be lost in the port

The current memo's validity check is `S.Key != ColumnKey || S.X != WorldX || S.Y != WorldY`, and
`ColumnKey` is built in `PrepareChunk` from **strate + layout version + seed + `ParamsFingerprint`**.

**`ParamsFingerprint` is load-bearing and its absence was a real shipped bug** — without it, two
stacks of the same strate with different params shared columns, the overhang silently vanished, and
only 69 of 20000 samples showed it. The comment at the site records this. **The new box's validity
key must still contain all four**, or you reopen a fixed bug. (Note `GSurfColCache` itself keys on
`(box XY, StrateKey, Seed, LayoutVersion)` **without** the fingerprint — do **not** copy that part;
it is the weakness the op-stack memo deliberately closed.)

Also keep the full XY comparison semantics: a lookup must never return a column computed for a
different XY. With a direct-indexed box that is structural (the index *is* the XY), but the box
bounds check must be exact.

## Keep the counters

`ColumnMemoHit` / `ColumnMemoMiss` must keep working, incremented on the same meaning (miss = a
column was recomputed). They are the before/after instrument.

## Invariants

1. **Density must not move by one bit.** This changes *caching*, never a computed value. The eight
   equivalence tests — especially `SurfaceHeightEquivalence` and its overhang section — must stay
   bit-identical. If your diff changes what `TerrainStack.EvalHeight` / `CeilingStack.EvalHeight`
   compute, or the overhang gate maths, you have the wrong site.
2. Both consumers keep working: the source (`~858`) and the overhang (`~923`, via
   `Column->GetColumn`). The overhang **must** see the same column the source did — that is
   "by construction rather than by convention", and the current code says so.
3. Sizing: state in a comment how many columns a tile needs (a 35×35 grid = 1225) and size the box so
   one tile fits without eviction, as `FSurfaceColumnBox` does.
4. `thread_local` stays; no shared mutable state across workers.

## Acceptance

- `ColumnMemoMiss` drops roughly **10×**; miss rate goes from **14.7% → ~1.5%**.
- The eight equivalence tests stay green and bit-identical.
- If the miss rate does **not** fall, say so plainly — a fix that does not move its own instrument is
  a failed fix, not a partial one.

---

# PART B — name which guard stops T1.d in the running game

## The problem

`Tiles Operator Stack Solid` / `Air` **never appeared** in `stat VoxelForge`, while the harness proves
11 of 40 tiles at production defaults. Rows only render in frames where a counter fires, so site B is
never reached in game. The cave branch of `UVoxelGenerator::ClassifyTile` has **13 `return
EVoxelTileClass::Mixed` paths** and we cannot tell which one fires.

## What to build

Add DWORD counters to the existing `stat VoxelForge` group (`VoxelStats.h` / `.cpp`) that attribute
the bail, grouped by *reason* rather than one per line:

| counter | fires when |
|---|---|
| `CaveBailNotOpStack` | `UsesOperatorStackForChunk` is false (either the initial check or the per-chunk sweep) |
| `CaveBailMixedContent` | `bAnyNonCave` — the tile also touches a gap or SurfaceWorld chunk — or a second cave slot, or out-of-layout |
| `CaveBailParams` | the params `Memcmp` disagreed across the box, the archetype differed, or `NumChunkCoords > 27` |
| `CaveBailStackVerdict` | the stack built fine but `ClassifyBox` returned `Mixed` |
| `CaveBailDisturbance` | the final `bCanSolid == bCanAir` after disturbances |
| `CaveBailNoStack` | `VF_BuildOpStackForChunk` returned false |

Increment **exactly one** per bail, immediately before the `return`. Together with the existing
`TilesOpStackSolid` / `TilesOpStackAir`, one underground flight then names the cause outright.

## Invariants

1. **DO NOT change any control flow, condition, or return value in `ClassifyTile`.** Every `return`
   there is a conservative guard that fails to `Mixed`; a wrong verdict leaves a tile with no
   geometry and no collision. Add counters beside the existing returns and nothing else.
2. `ClassifyTile` is `const` and runs on **worker threads** — `INC_DWORD_STAT` only, never a
   `static int32++`. It routes through `FThreadStats::AddMessage` (per-thread packets), which is why
   it is safe.
3. Zero cost when `STATS == 0`: compute nothing outside the macros.
4. Do not touch the non-cave parts of `ClassifyTile` (gap / SurfaceWorld / column scan).

## Acceptance

Fly underground in a TunnelNetwork strate: exactly one bail counter should dominate, or
`TilesOpStackSolid` should finally appear. Either outcome is a result.

---

# Shared rules

- **NEVER build, compile or run the editor or the tests.** Stop when the code is written.
- **Do not `git commit`, `git push`, `git checkout`, `git stash`, `git restore`.** Uncommitted work
  in the tree must survive.
- Comments are French + English; match the surrounding file.
- Macro spelling: `KINDA_SMALL_NUMBER`, not the `UE_`-prefixed form.
- When inserting anything into `VoxelDensityOpStack.cpp`, put it **above the labelled end of the
  anonymous namespace** — anchoring on the FACTORIES banner puts it outside and the brace closes
  nothing. This mistake has been made twice in that file.

## Report

1. The diff for Part A and Part B separately.
2. `git diff --stat`.
3. Explicit confirmation that: no height-stack or overhang maths changed; the new column key still
   contains strate + layout + seed + `ParamsFingerprint`; `ClassifyTile`'s control flow and return
   values are untouched; exactly one bail counter fires per bail path.
4. Likely compile-error spots, specifically.
5. Anything in this spec that contradicts the code — **stop and say so rather than guessing.**
