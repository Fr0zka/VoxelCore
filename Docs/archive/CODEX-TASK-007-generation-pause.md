# Codex task 007 — VF-01: never mutate layout/passages while workers read them

**Owner:** Codex (Model Luna, xHigh) · **Orchestrator:** Claude · **Branch:** `experimental`
**Status:** specified, not started
**Kind:** ⚠️ **crash class (use-after-free).** Highest-severity item found on 2026-08-16.
**Origin:** Sol-High audit VF-01, **independently confirmed by reading** before this spec was written.

---

## The defect

`UVoxelStrateManager::Initialize` does `StrateLayout.Empty()` and `Passages.Empty()` + `Passages.Add()`
— it **frees and reallocates** both arrays. There is **no lock, no barrier, no drain** in that file.

Meanwhile those same arrays are read **on mesher worker threads**:

| reader | access |
|---|---|
| `AnyPassageNearBox` (`VoxelStrateManager.cpp:460`) | range-`for` over `Passages` |
| `EvaluateModifierSDF` | indexes `Passages[...]` |
| `FindSlotIndexForChunkZ` | iterates `StrateLayout` |

all reached from `GetDensityAt` / `ClassifyTile` inside chunk tasks.

`RegenerateAllChunks()` bumps the epoch **after** `Initialize`, so previous-epoch workers are live
*during* the mutation. **The epoch rejects a finished result; it cannot make a read of a freed
allocation safe.**

**Precedent in this very codebase:** `DiffLayer.ChunkMods` is read on mesher workers and written on
the game thread, and all access now holds `ModsLock` — added after a real carve-vs-stream access
violation. `StrateLayout` / `Passages` are the same shape with no guard.

Four `Initialize` call sites:

| line | function | dangerous? |
|---|---|---|
| 145 | `RebuildStrates` | **yes** |
| 309 | `OnObjectModifiedInEditor` | **yes — fires automatically on a strate asset edit while streaming** |
| 417 | `BeginPlay` | **no** — no tasks exist yet. **Leave it alone.** |
| 2091 | `ChangeSeed` | **yes** (also writes `Generator`'s `Seed` / `OriginSpineRadius`) |

## Why THIS fix and not the other two

Rejected deliberately — do not "improve" the design into either of these:

- **An `FRWLock` around the two arrays** (the `ModsLock` shape) would put a **read lock on the
  per-voxel hot path** — `EvaluateModifierSDF` and `FindSlotIndexForChunkZ` run ~43k times per tile.
  There is an open, unmeasured perf regression under active investigation (`CODEX-TASK-001/002`);
  adding hot-path lock traffic now would **contaminate the very measurement those tasks exist to
  take.** Correct, but the worst possible timing.
- **An immutable generation snapshot** (Sol's suggestion) is the right long-term architecture and a
  real refactor of `UVoxelStrateManager`'s whole API surface. Too large to improvise, and it belongs
  in a design conversation.

**The drain has zero hot-path cost**, reuses machinery already proven in `EndPlay`, and its only
cost — a brief stall — lands exclusively on **human-initiated editor actions** (asset edit, rebuild,
seed change). It never occurs during play.

## What to build

### 1. A pause flag distinct from shutdown

Add to `AVoxelWorld`: `std::atomic<bool> bGenerationPaused{false};`

⚠️ **Do NOT reuse `bShuttingDown` for this.** It would work mechanically, but it means "we are tearing
down" and a future reader would be misled about lifetime. Introduce a small helper used at the
existing gate points:

```cpp
FORCEINLINE bool ShouldAbortWork() const
{
    return bShuttingDown.load(std::memory_order_relaxed)
        || bGenerationPaused.load(std::memory_order_relaxed);
}
```

Route the **existing** checks through it — the submission gate (`VoxelWorld.cpp:638`) and the
in-task checks (`:1467`, `:1474`). **Do not add new check points**; do not change what those sites do
when the check is true.

### 2. An RAII scoped pause, modelled on `EndPlay`'s drain

`EndPlay` (`:323–334`) already implements this exact pattern: raise the gate, then spin until
`ActiveTaskCount` reaches 0. Mirror it.

```
FScopedGenerationPause guard(this);
if (!guard.Acquired()) { /* log error, DO NOT mutate, return */ }
```

- **Ctor:** set `bGenerationPaused = true`, then wait for **both** `AVoxelWorld::ActiveTaskCount == 0`
  **and** the decoration tasks to finish. Decoration tasks are counted by the file-static
  `GActiveDecoTasks` in `VoxelContentManager.cpp` and already drained by `NotifyShutdown` (`:65–80`) —
  add a small public drain/wait accessor on `UVoxelContentManager` rather than exposing the counter.
- **Dtor:** always clear `bGenerationPaused`, including on the failure path.

### 3. ⚠️ FAIL SAFE — this is the most important line in the spec

If the deadline expires with tasks still running: **DO NOT MUTATE.** Log an error naming the
function, clear the flag, and return, leaving the world in its previous consistent state. The user
can retry the edit.

**Mutating anyway is what the bug already does.** A timeout that proceeds is not a fix. The three
dangerous call sites must each be structured so the `Initialize` call is *unreachable* unless the
pause was acquired.

Use a generous deadline (≥ 5 s) and log at `Error` when it expires — a silent skip would look like
the edit simply didn't apply.

### 4. Wrap the three call sites

`RebuildStrates`, `OnObjectModifiedInEditor`, `ChangeSeed`. The pause must cover **all** the mutation,
including `ChangeSeed`'s writes to the generator's `Seed` / `OriginSpineRadius`, and it must be
released **before** `RegenerateAllChunks()` so regeneration can submit work. **`BeginPlay` is not
wrapped.**

## ⚠️ Invariants

1. **No density, mesher, or geometry code may change.** This must not move one bit of generated
   terrain. If your diff touches `VoxelGenerator.cpp`, `VoxelDensityOpStack.cpp`,
   `VoxelCaveMorphology.cpp` or `VoxelMarchingCubesMesher.cpp`, stop — wrong site.
2. **No deadlock.** The pause is taken on the **game thread**. Verify by reading that chunk tasks
   never block on the game thread (they read the generator and `Enqueue` to an MPSC queue, which is
   non-blocking) — so a drain is bounded. **State in your report that you checked this**, and if you
   find any worker path that waits on the game thread, STOP and report it instead of proceeding.
3. **`ProcessQueue` stays `EQueueMode::Mpsc`.** Do not touch it.
4. **Do not change `EndPlay`.** Its 3-second timeout is a separate, deliberate decision
   (audit VF-02) and is Jahni's call, not part of this task.
5. **Carry the `Epoch`** through anything you touch; do not reorder the existing epoch bump relative
   to `RegenerateAllChunks`.
6. Comments are French + English; match the surrounding file.

## Acceptance

- `stat`/gameplay unchanged; **generated terrain bit-identical** (the equivalence tests and every
  box-verdict number must be untouched — this change cannot reach them).
- Editing a strate asset while the world streams: brief stall, then the edit applies. **No crash.**
- The failure path is reachable and honest: if the drain times out, an `Error` log names the function
  and the world keeps its previous state.
- `git diff --stat` should list `VoxelWorld.cpp`, `VoxelWorld.h`, and `VoxelContentManager.{h,cpp}`
  for the drain accessor. Nothing else.

## Notes for the reviewer (Claude)

- Confirm the three dangerous sites cannot reach `Initialize` when the pause was not acquired, and
  that `BeginPlay` is untouched.
- Confirm the dtor clears the flag on **every** path including early return.
- Confirm `ShouldAbortWork` replaced the existing checks rather than adding new ones, and that
  `bShuttingDown`'s own semantics are unchanged.
- Confirm the deco drain is included — chunk tasks alone are not the whole reader set.
