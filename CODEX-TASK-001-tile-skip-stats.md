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

## The site — do not go looking, it is one place

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

## What to build

1. **A stat group.** New header `Source/VoxelForge/Public/VoxelStats.h`:
   `DECLARE_STATS_GROUP(TEXT("VoxelForge"), STATGROUP_VoxelForge, STATCAT_Advanced);` plus
   `DECLARE_DWORD_COUNTER_STAT_EXTERN` for each counter below. `DEFINE_STAT` for each goes in **one**
   `.cpp` — put them in a new `Source/VoxelForge/Private/VoxelStats.cpp`.

2. **Four per-frame counters** (`DWORD_COUNTER`, so `stat VoxelForge` shows a rate, not a total):

   | counter | incremented when |
   |---|---|
   | `TilesClassified` | the classifier gate was entered (the `if` above ran `ClassifyTile`) |
   | `TilesSkippedAllSolid` | verdict was `AllSolid` |
   | `TilesSkippedAllAir` | verdict was `AllAir` |
   | `TilesMeshed` | `GenerateMesh` / `GenerateSheetMesh` actually ran |

   Splitting solid from air is the point, not decoration: **cave archetypes prove `AllSolid`**, so
   that counter is the one that answers "did the op-stack work do anything in the real game".

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

## Acceptance

- Editor, `stat VoxelForge` on screen, fly around: the numbers move.
- **`TilesClassified == TilesSkippedAllSolid + TilesSkippedAllAir + TilesMeshed`** for tiles that
  entered the gate. (Tiles that fail the gate are meshed without being classified, so `TilesMeshed`
  is legitimately larger than the classified total — say so in a comment rather than "fixing" it.)
- With **no strate opted in**: flying underground through a `TunnelNetwork` strate,
  `TilesSkippedAllSolid` stays **0**. That is the baseline and it must be observed *before* the
  next step, or the next step proves nothing.
- Tick `bUseOperatorStack` on **one** `TunnelNetwork` strate, fly the same route:
  `TilesSkippedAllSolid` becomes **non-zero**. ← this is the deliverable.

## Notes for the reviewer (Claude)

- Check the verdict refactor byte-for-byte against the original condition. `!= Mixed` is the whole
  contract.
- Check the counters are `DWORD_COUNTER` (per-frame) and not `DWORD_ACCUMULATOR`.
- Confirm no counter is incremented outside the gate in a way that double-counts the carve path,
  which calls `GenerateTileResult` synchronously from the game thread.
