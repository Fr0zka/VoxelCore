# Codex task 004 — two box verdicts assume `MinRadius ≤ MaxRadius`; a third one already doesn't

**Owner:** Codex (Model Luna, xHigh) · **Orchestrator:** Claude · **Branch:** `experimental`
**Status:** specified, not started
**Kind:** ⚠️ **correctness of a box verdict.** Same class as task 003. Small fix, closes a class.

---

## Why this exists

Three operators roll a primitive radius from a hash between two designer-set params:

```cpp
Out.R   = FMath::Lerp(MinRadius,          MaxRadius,          hash01);   // FGridColumnMod   ~1129
Out.R   = FMath::Lerp(P.ShaftMinRadius,   P.ShaftMaxRadius,   hash01);   // FShaftFieldSource ~1560
Out.Rxy = FMath::Lerp(P.IslandMinRadius,  P.IslandMaxRadius,  hash01);   // FIslandBlobSource ~1850
```

`FMath::Lerp(A, B, t)` with `t ∈ [0,1]` lands anywhere in `[min(A,B), max(A,B)]` — it does **not**
require `A ≤ B`.

Each op's `EffectOverBox` then sweeps a **range of lattice cells** around the query box, padded by
the largest radius a cell could hold, and tests each rolled primitive exactly. The pad decides which
cells are *looked at at all*, so a pad smaller than the true maximum radius means **cells are never
examined**, their primitives are never tested, and the op reports `Identity` for a box that its own
`Eval` will carve or fill.

| op | pad used for the cell sweep | correct? |
|---|---|---|
| `FGridColumnMod` ~1086 | `FMath::Max(MaxRadius, 0.0f) + ColBlend` | ⛔ **exposed** |
| `FShaftFieldSource` ~1436 | `FMath::Max(P.ShaftMaxRadius, P.ConnectorRadius) + ExtraReach` | ⛔ **exposed** |
| `FIslandBlobSource` ~1803 | `const float MaxR = FMath::Max(P.IslandMinRadius, P.IslandMaxRadius);` | ✅ **already correct** |

**The third one is the point.** Someone hit this exact concern while writing the island source and
guarded it. The other two shipped without the guard. This task makes the three consistent.

## How exploitable, stated honestly

The shipped defaults are correctly ordered (`2/5`, `2/7`, `5/11`), so **nothing is broken out of the
box.** It needs a mis-ordered asset value — `ColumnMinRadius = 8, ColumnMaxRadius = 4`.

Nothing prevents that. The `UPROPERTY` metas carry `ClampMin = "1.0"`, which is a per-property
floor; Unreal has no declarative way to say "must be ≤ that other property". And `ColumnMinRadius`
is *also* settable per-room through `UVoxelTerrainOpDefinition`, so it is not only the strate asset.

What makes it worth the five lines: when it does happen, the failure is **invisible and maddening**.
`Eval` still draws the fat column perfectly, so every tile that gets meshed looks correct; only the
tiles the classifier *skipped* are missing — no geometry, no collision, in a world that otherwise
looks right.

## The fix — five lines, and one thing you must NOT do

Make each pad use the true envelope:

```cpp
// FGridColumnMod ~1086
const float Reach = FMath::Max3(MinRadius, MaxRadius, 0.0f) + ColBlend;

// FShaftFieldSource ~1436
const float Pad = FMath::Max3(P.ShaftMinRadius, P.ShaftMaxRadius, P.ConnectorRadius) + ExtraReach;
```

(Use whatever spelling is idiomatic here — check that `FMath::Max3` is already used in this codebase
before reaching for it; nested `FMath::Max` is fine and matches `FIslandBlobSource`'s existing line.)

### ⛔ DO NOT "fix it properly" by normalising the params

The tempting larger fix — swap `Min`/`Max` at resolution time so `Min ≤ Max` always — is **wrong and
will break the build's tests.** `Eval` computes `Lerp(Min, Max, t)`; swapping the endpoints maps the
same hash `t` to a *different* radius for the same cell. That changes generated geometry and breaks
the eight bit-for-bit equivalence tests against the `switch` path.

**Only the BOUND may become conservative. `Eval` stays byte-identical.** This is the same rule as
task 003 and the same reason.

## ⚠️ Invariants

1. **No `Eval`, `RollColumn`, `RollShaft`, `GetCells`, or `GetCellsAt` body may change.** If your
   diff touches one, you have the wrong site — stop and say so.
2. **Do not touch `FIslandBlobSource`.** It is already correct and is the reference for this fix.
3. The change direction is strictly conservative: a wider sweep examines *more* cells, so a verdict
   can only move from `Identity` toward `CarveOnly`/`FillOnly`, never the reverse. Do not add any
   compensating tightening.
4. Comments are French + English; match the surrounding file. Say **why** the envelope is
   `max(Min, Max)` and not `Max` — the next reader must not "simplify" it back.

## Acceptance

- `git diff --stat` shows exactly one file: `Source/VoxelForge/Private/VoxelDensityOpStack.cpp`,
  and a handful of lines.
- The three ops now agree on the pattern.
- After the build: **every box-verdict line and every equivalence test is unchanged**, because the
  shipped defaults are correctly ordered and the envelope only differs when they are not. **A change
  in any of those numbers means the diff did something it should not have.** That is this task's
  whole acceptance signal — a *no-op at defaults* is the expected, correct result.

## Notes for the reviewer (Claude)

- Confirm both pads changed and `FIslandBlobSource` did not.
- Confirm no `Lerp` argument order was touched anywhere — that is the failure mode that would look
  like a tidy-up and silently change the world.
