# Codex task 001 — make tile-skipping observable in the running game

**Owner:** Codex (Model Luna, xHigh) · **Orchestrator:** Claude · **Branch:** `experimental`
**Status:** specified, not started

---

## Why this exists

Jahni built the world, looked at it, and said: *"I don't know if it dropped any meshing? but it looks
alright by the eye."*

He's right to be unsure — **there is no way to answer that question from inside the game.** The
plugin has **zero** stat counters (`grep INC_DWORD_STAT` → nothing). Tile-skipping is the largest
perf item in the whole plan and it is currently unobservable in production; it has only ever been
measured in an automation harness, on 40 sampled tiles.

And a visual check cannot answer it: *skipped correctly* and *skipped nothing* render identically.
This codebase has paid repeatedly for exactly that confusion — see the "coverage is a number, not a
boolean" lessons in `OPSTACK-HANDOFF.md`.

**The real prize:** with no strate opted in, cave-archetype skips must read **0**. After ticking
`bUseOperatorStack` on one `TunnelNetwork` strate and flying underground, they must become non-zero.
That is the **production-side proof of T1.d**, which does not exist today.

## The sites — there are TWO, and the second one is the one that answers the question

### Site A — the skip itself

`Source/VoxelForge/Private/VoxelWorld.cpp`, in **`AVoxelWorld::GenerateTileResult`** (~line 1501).
Trust the symbol, not the line number.

```cpp
bool bTrivialEmpty = false;
if (!bSheetTile && !bWantCapture && Generator && Mesher && Mesher->IsoLevel == 0.0f)
{
    TRACE_CPUPROFILER_EVENT_SCOPE(VoxelForge_ClassifyTile);
    bTrivialEmpty = (Generator->ClassifyTile(OriginVoxels, Step, Cells) != EVoxelTileClass::Mixed);
}

FVoxelMeshData MeshData;
if (!bTrivialEmpty)
{
    TRACE_CPUPROFILER_EVENT_SCOPE(VoxelForge_GenerateMesh);
    MeshData = bSheetTile ? Mesher->GenerateSheetMesh(...) : Mesher->GenerateMesh(...);
}
```

### Site B — where the OPERATOR STACK's verdict is produced

`Source/VoxelForge/Private/VoxelGenerator.cpp`, in **`UVoxelGenerator::ClassifyTile`**, at the **exit
of the `if (bAnyCave)` block** (~line 3009) — the last two lines of that block:

```cpp
if (bCanSolid == bCanAir) { return EVoxelTileClass::Mixed; }
return bCanSolid ? EVoxelTileClass::AllSolid : EVoxelTileClass::AllAir;
```

**Why site A alone cannot answer the question — this is the correction that makes the task
meaningful.** `ClassifyTile` has *two* independent ways to reach a non-`Mixed` verdict:

- the **hand-written** path, which predates all of this work: a chunk in a **bedrock gap** sets
  `bCanAir = false` (VoxelGenerator.cpp ~2835) and, absent a passage or the origin spine, the tile
  resolves **`AllSolid`**. Likewise the SurfaceWorld column scan. This fires with **no strate opted
  in at all**;
- the **operator-stack** path, the `if (bAnyCave)` block, which is the only thing T1.d added.

So `TilesSkippedAllSolid` at site A **will already be non-zero underground before any strate is
ticked** — the bedrock between strates guarantees it. A single lumped counter would make the
before/after unreadable, and that is exactly the "when a zero has several possible causes, give each
one its own number" lesson this project already paid for.

Site B's counters have the opposite property, and it is a strong one: `ClassifyTile` returns `Mixed`
outright at the cave branch when `UsesOperatorStackForChunk(CC)` is false (~line 2809, and again per
chunk of the box at ~2914). **With no strate opted in, the site-B counters are zero by
construction, not merely by observation** — so a non-zero reading after ticking the box cannot come
from anywhere else.

## What to build

1. **A stat group.** New header `Source/VoxelForge/Public/VoxelStats.h`:
   `DECLARE_STATS_GROUP(TEXT("VoxelForge"), STATGROUP_VoxelForge, STATCAT_Advanced);` plus
   `DECLARE_DWORD_COUNTER_STAT_EXTERN` for each counter below. `DEFINE_STAT` for each goes in **one**
   `.cpp` — put them in a new `Source/VoxelForge/Private/VoxelStats.cpp`.

2. **Six per-frame counters** (`DWORD_COUNTER`, so `stat VoxelForge` shows a rate, not a total):

   | counter | site | incremented when |
   |---|---|---|
   | `TilesClassified` | A | the classifier gate was entered (the `if` above ran `ClassifyTile`) |
   | `TilesSkippedAllSolid` | A | verdict was `AllSolid` |
   | `TilesSkippedAllAir` | A | verdict was `AllAir` |
   | `TilesMeshed` | A | `GenerateMesh` / `GenerateSheetMesh` actually ran |
   | `TilesOpStackSolid` | B | the `bAnyCave` block returned `AllSolid` |
   | `TilesOpStackAir` | B | the `bAnyCave` block returned `AllAir` |

   Splitting solid from air is the point, not decoration: **cave archetypes prove `AllSolid`**.
   Splitting site B from site A is the whole deliverable — see "Why site A alone cannot answer the
   question" above. `TilesOpStackSolid ≤ TilesSkippedAllSolid` always, and the difference is the
   pre-existing bedrock/surface skipping.

   At site B, increment on the `return` line only — **not** before the
   `if (bCanSolid == bCanAir) return Mixed;` guard, which is where the block bails out with no
   verdict.

3. To get the verdict you need it as a value, not a bool. Changing
   `bTrivialEmpty = (Classify(...) != Mixed)` into a stored `EVoxelTileClass Verdict = Classify(...)`
   followed by `bTrivialEmpty = (Verdict != Mixed)` is **fine and expected**.

## ⚠️ Invariants — a violation here is not a bug, it is a hole

1. **DO NOT change `bTrivialEmpty`'s value or the control flow.** That bool decides whether a tile
   gets geometry **and collision**. A wrong value is invisible until a player falls through the
   floor. Refactor the expression, never the condition.
2. **DO NOT touch the gate `!bSheetTile && !bWantCapture && Generator && Mesher && Mesher->IsoLevel
   == 0.0f`.** Every clause is load-bearing and documented in the comment above it — sheet tiles have
   no marching cubes, capture tiles need the grid even when uniform, and the verdicts assume the MC
   iso is exactly zero.
3. **Thread safety: this runs on WORKERS.** `GenerateTileResult` is called from the async ChunkGen
   task *and* the synchronous carve path. Use the `INC_DWORD_STAT` family, which is per-thread-packet
   safe. **A plain `static int32` counter, even `++` on an `int32`, is a data race — do not.**
4. **Zero cost when stats are compiled out.** The `INC_DWORD_STAT` macros already vanish when
   `STATS == 0`. Do not wrap them in an `if` that survives, and do not compute anything solely to
   feed a counter outside the macro.
5. **No new includes in a public header beyond `Stats/Stats.h`**; the plugin follows IWYU and the
   include debt was cleared deliberately (`AUDIT §C9` work).
6. **At site B, do not touch `ClassifyTile`'s control flow either — and do not add an early
   `return`.** That function is a chain of conservative guards that all **fail to `Mixed`**; every
   `return` in it is load-bearing. Add the counter to the existing `return` expression's statement,
   nothing else. `ClassifyTile` is `const` and runs on the same workers as site A, so the same
   `INC_DWORD_STAT`-not-`static int32` rule applies.

## Acceptance

- Editor, `stat VoxelForge` on screen, fly around: the numbers move.
- **`TilesClassified == TilesSkippedAllSolid + TilesSkippedAllAir + TilesMeshed`** for tiles that
  entered the gate. (Tiles that fail the gate are meshed without being classified, so `TilesMeshed`
  is legitimately larger than the classified total — say so in a comment rather than "fixing" it.)
- **`TilesOpStackSolid ≤ TilesSkippedAllSolid`** and **`TilesOpStackAir ≤ TilesSkippedAllAir`**,
  always. A violation means site B is counting a verdict that site A did not act on.
- With **no strate opted in**, flying underground through a `TunnelNetwork` strate:
  - `TilesSkippedAllSolid` is expected to be **non-zero** — that is the pre-existing bedrock/surface
    skipping, not a bug, and it is why the lumped counter cannot be the deliverable;
  - `TilesOpStackSolid` and `TilesOpStackAir` are **0**. This is the baseline, and it must be
    *observed* before the next step even though it is guaranteed by the flag gate.
- Tick `bUseOperatorStack` on **one** `TunnelNetwork` strate, fly the same route:
  **`TilesOpStackSolid` becomes non-zero.** ← this is the deliverable, and it is the first
  production-side evidence T1.d has ever had.

## Notes for the reviewer (Claude)

- Check the verdict refactor byte-for-byte against the original condition. `!= Mixed` is the whole
  contract.
- Check the counters are `DWORD_COUNTER` (per-frame) and not `DWORD_ACCUMULATOR`.
- Confirm no counter is incremented outside the gate in a way that double-counts the carve path,
  which calls `GenerateTileResult` synchronously from the game thread.
- Site B: confirm the increment sits **after** the `bCanSolid == bCanAir` bail-out, and that the two
  counters follow `bCanSolid` the same way the returned enum does — a swapped pair reads as a
  plausible result and proves the wrong thing.
- The automation tests call `ClassifyTile` directly; they will move the site-B counters. Harmless,
  but do not let a test-only path become the only thing that moves them.
