# Codex task 002 — is the op-stack column memo thrashing? Count, don't guess.

**Owner:** Codex (Model Luna, xHigh) · **Orchestrator:** Claude · **Branch:** `experimental`
**Status:** specified, not started
**Depends on:** `CODEX-TASK-001` — this adds counters to the **same** `stat VoxelForge` group.
Do 001 first; this task assumes `VoxelStats.h` already exists.

---

## Why this exists

The operator-stack density path is measurably slower than the `switch` it replaces, and the cause
has never been attributed. There are three standing suspects. **This task measures the first one,
and does not fix anything.** That order is deliberate: `AUDIT §C10` cost six builds and five refuted
hypotheses by reasoning first, and the last session re-learned it.

### The hypothesis, derived from the code

`FSurfaceColumnSource::GetColumn` (`VoxelDensityOpStack.cpp` ~615) memoises a computed column in a
**direct-mapped, 4096-entry `thread_local` table**, indexed by a hash of the two XY floats:

```cpp
struct FSlot { uint64 Key; float X, Y; FColumn C; };   // 40 bytes
thread_local FSlot Slots[4096] = {};                   // 160 KB per worker
const uint32 Idx = ((HX * 0x9E3779B9u) ^ (HY * 0x85EBCA6Bu)) >> 20;   // [0,4095]
```

The original path, `GSurfColCache` in `GetDensityAt`, is instead a **direct-indexed box**:
`CI = (IY - Box.BaseY) * Dim + (IX - Box.BaseX)`, with a `Computed[CI]` flag. **No hash, therefore
no collisions, therefore every column is computed exactly once.**

Now the sampling order, which is the premise that makes this bite. `FVoxelMarchingCubesMesher`
pre-samples with **Z as the OUTERMOST loop** (`VoxelMarchingCubesMesher.cpp` ~226):

```cpp
for (int32 gz = GzLo; gz <= GzHi; gz++)
    for (int32 gy = -1; gy <= GridDim; gy++)
        for (int32 gx = -1; gx <= GridDim; gx++)
            Generator->GetDensityAt(...);
```

So the mesher sweeps a **whole XY plane at every Z level**. Every column in the tile is revisited
once per Z plane — roughly 34 times.

The table's own comment sized it for this: *"A chunk is CHUNK_SIZE² columns (1024), so the first
draft's 256 entries could not even hold one chunk and thrashed inside a single tile. 4096 covers
four chunks."* **That reasoning has a gap.** A direct-mapped table does not need to be full to
evict — it needs two live keys to collide. At ~1156 columns per plane in 4096 slots (load factor
0.28), the expected number of columns sharing a slot with another is **~285, about 25 %**. Those
columns evict each other, miss again on the next Z plane, and recompute the **entire height stack** —
structural source, cliff (four structural resamples), terrace, layer-line, beach, and the ceiling
stack — every single plane.

Order of magnitude if that is right: ~1156 column computations on the original path versus
~1156 + 285 × 34 ≈ **10 000** on the op path. Roughly **9×** the column work, on the plugin's most
expensive archetype.

**That is a derivation, not a measurement, and it is exactly the kind of confident chain this
project has watched reverse six times.** Hence: count first.

## What to build — two counters, no behaviour change

Add to the existing `stat VoxelForge` group from task 001:

| counter | incremented when |
|---|---|
| `ColumnMemoHit` | `GetColumn` found a live entry (the `if` body did **not** run) |
| `ColumnMemoMiss` | `GetColumn` recomputed (the `if` body ran) |

That is the entire change. Two `INC_DWORD_STAT` calls inside `FSurfaceColumnSource::GetColumn`,
around the existing `if (S.Key != ColumnKey || S.X != WorldX || S.Y != WorldY)`.

## ⚠️ Invariants

1. **Change nothing else in `GetColumn`.** Not the table size, not the hash, not the key comparison.
   The point of this task is to produce a number that decides whether the fix is worth writing; a
   change bundled in makes the number unattributable. **Do not "obviously improve" the table while
   you are in there** — if the fix ships in the same build as the instrument, we learn nothing, and
   this project has a written lesson about exactly that.
2. **`GetColumn` is `const` and runs on mesher workers.** `INC_DWORD_STAT`, never a `static int32++`
   — same rule as task 001, same reason.
3. **The key comparison is load-bearing and must stay complete.** `S.Key != ColumnKey || S.X != X ||
   S.Y != Y` — the full key is compared on every touch precisely so a hash collision can only cost a
   recompute and never return **someone else's column**. Do not shorten it to feed a counter.
4. **Zero cost when `STATS == 0`.** No value computed outside the macros.
5. `FCaveCliffMod` and the overhang read this same memo through
   `Column->GetColumn(...)` (~line 864). They are legitimate traffic and must be counted, not
   excluded — they are part of why a miss is expensive.

## Acceptance — the prediction is now numeric (tightened 2026-08-16 from the real grid dimensions)

The hand-wavy "20–30 %" band this section used to carry has been replaced by an arithmetic
prediction, because the inputs are all statically knowable and were read out of the source:

- `CHUNK_SIZE = 32`, `CellsPerAxis = 32`, `GridDim = 33`, and the pre-sample loops run
  `g ∈ [-1, GridDim]` per axis ⇒ **35 × 35 = 1225 distinct columns per tile**, over **35 Z planes**
  (the mesher's own buffer comment, "35³ floats", confirms the dimension).
- 1225 keys in 4096 slots is a load factor of **0.299**. Expected slots holding exactly one key
  `= 4096 · np(1-p)^(n-1) ≈ 908`, so **~317 columns (25.9 %) share a slot with another** and evict
  each other on every plane.
- ⇒ op path ≈ `1225 + 34 × 317` ≈ **12 000** column computations per tile.
  Original path (`GSurfColCache`, direct-indexed, `Computed[CI]` persists) = **1225**.
  **≈ 9.8×.**

### How to read the result

⚠️ **Compare the RATIO OF THE TWO HYPOTHESES, not an absolute percentage.** The overhang and cliff
modifiers call `GetColumn` again at the same XY (~line 864); every extra consumer adds **hits** and
no misses, so it inflates the denominator and drags the miss *rate* down without changing the
verdict. What does not move is the ~10× gap between the two outcomes.

| observation | verdict |
|---|---|
| misses ≈ **8–10×** the hit-path baseline (single-consumer: ~28 % of lookups) | **CONFIRMED** — the table evicts on collision every Z plane. The fix gets its own task, with this run as its "before". |
| misses ≈ **1 per distinct column** (single-consumer: ~3 %, hit rate ≥ 97 %) | **hypothesis WRONG.** The table behaves like the box, the ~9.8× does not exist, and the perf cost is suspect 2 (19 virtual calls per voxel). |

*A negative result here is a real result.* It retires the most-suspected cause and is worth the build
either way; it must be written into `OPSTACK-PROGRESS.md`, not quietly dropped.

Report **both raw numbers**, never the ratio alone — a ratio cannot distinguish "few lookups" from
"many", and the absolute miss count is what the fix would be reducing.

## Notes for the reviewer (Claude)

- Confirm the miss counter sits inside the `if`, and the hit counter in an `else` — not computed
  from a subtraction, which would silently agree with itself.
- Confirm `GetColumn`'s early-out path (if any is added later) cannot skip both counters.
- Confirm nothing else in `VoxelDensityOpStack.cpp` changed. `git diff --stat` should show one file
  and a handful of lines.
