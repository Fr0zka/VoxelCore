# Codex task 005 — three box-verdict tile scans sample less than one lattice period

**Owner:** Codex (Model Luna, xHigh) · **Orchestrator:** Claude · **Branch:** `experimental`
**Status:** specified, not started
**Kind:** test coverage. **Zero risk to the game — no non-test file may change.**

---

## Why this exists

This project has already found and fixed this exact defect **twice**:

> *"A sampler must cover at least one period of what it samples. The tunnel test drew tile XY from
> ±32 voxels with `RoomSpacing 80` — it measured the spine hub and called it the world. The shaft
> test had the identical bug (±48 against `ShaftSpacing 55`)."* — `OPSTACK-HANDOFF.md`

Both were fixed to `SpanCells = 55` ⇒ **±440 voxels**, and both now print their own extent in units
of the pattern's period so it cannot silently regress.

**Three box-verdict tile scans were never fixed**, because the fix was applied where the bug was
noticed rather than to the class. All three still use the original `Rng.RandRange(-6, 6) * Extent`
with `Step = 1, Cells = 8` ⇒ `Extent = 8` ⇒ **half-extent ±48 voxels**:

| test | half-extent | lattice period (default, **unchanged by the fixture**) | coverage |
|---|---|---|---|
| `VoxelForgeOpStackIslandTest.cpp` ~244 | ±48 | `IslandSpacing` **95** | **0.51 periods** ⛔ worse than either bug already fixed |
| `VoxelForgeOpStackSlabTest.cpp` ~289 | ±48 | `ColumnSpacing` **60** | **0.80 periods** ⛔ |
| `VoxelForgeOpStackMazeTest.cpp` ~276 | ±48 | `CellSize` **40** | 1.20 periods ⚠️ marginal |

Verified: none of `EnableIslandFeatures` / the slab tuning / the maze setup overrides the spacing, so
the header defaults are what these tests actually run against.

**Why it matters right now, specifically.** Two commits just changed the box verdicts these very
tests are supposed to guard — `7dbdf51` (the `ExtraReach` bound, which touches **islands**, maze and
shafts) and `eaa44bf` (the radius envelope, which touches **slab columns** and shafts). The tests
that would catch a mistake in those changes currently sample about half a lattice cell.

## The rule the two fixed tests already encode

`Extent = Step * Cells = 8` voxels, so `SpanVoxels = SpanCells * 8`. To get **8 periods** of
half-extent you set:

> **`SpanCells` = the lattice spacing** (`55` for `ShaftSpacing 55` — that is where the shaft test's
> `55` comes from, and it is not a coincidence).

Apply the same:

| test | `SpanCells` | resulting half-extent | periods |
|---|---|---|---|
| Island | `95` | ±760 | 8.0 |
| Slab | `60` | ±480 | 8.0 |
| Maze | `40` | ±320 | 8.0 |

## What to build

For each of the three tests, mirror **exactly** what `VoxelForgeOpStackShaftTest.cpp` (~212) does:

1. Hoist `const int32 SpanCells` and `const int32 SpanVoxels = SpanCells * 8;` **outside the tile
   loop** — the report needs them and `Extent` is loop-local. (That scoping slip has already happened
   once in this file family; the shaft test's comment records it.)
2. Draw `Rng.RandRange(-SpanCells, SpanCells) * Extent` for X and Y. **Leave the Z draw exactly as
   it is** — it is clamped to the strate slot and is not part of this defect.
3. Extend the existing report line to print `SpanVoxels`, the live ratio
   `(float)SpanVoxels / FMath::Max(<the spacing param>, 1.0f)`, and the spacing itself — so the
   extent is stated in units of the pattern's own period and a future narrowing is visible.

## ⚠️ Invariants

1. **This is NOT "widen until it passes."** The comment already in the tunnel test says it best and
   the same reasoning applies here: *every proved tile is still brute-forced voxel by voxel below, so
   a wider sampler that produced a FALSE verdict fails exactly as before. We are changing what the
   measurement **looks at**, not what it **demands**.* Do not touch the brute-force loop, its
   tolerance, or any `AddError`.
2. **Do not adjust an assertion to accommodate a moved number.** Widening will change the proved /
   Mixed counts — that is the point. If an existing assertion would now fail, **report it and stop**;
   do not retune it. ("Don't assert a number you want to improve" is a written lesson here.)
3. **No file outside `Source/VoxelForge/Private/Tests/` may change.** `git diff --stat` must list
   only those three test files.
4. Do not change `Step`, `Cells`, the tile count (`60`), or the RNG seeds — a changed seed makes the
   before/after incomparable, and comparability is the whole point of touching this now.
5. Comments are French + English; match the surrounding file.

## Acceptance

- Three test files changed, nothing else.
- Each of the three now prints its extent **and** that extent in periods of its own spacing param.
- After the build, each ratio line reads **≥ 8 periods**.
- The proved counts will move. **That is expected.** What must NOT move: `violations` / `NumUnsound`
  stays **0** in all three. If it becomes non-zero, the wider sampler has found a genuine hole that
  the narrow one was hiding — which would be this task paying for itself immediately, and must be
  reported loudly rather than tuned away.

## Notes for the reviewer (Claude)

- Confirm `SpanCells`/`SpanVoxels` are outside the tile loop in all three.
- Confirm the Z draw is untouched.
- Confirm the ratio is computed **live** from the params struct, not hardcoded — a hardcoded "8.0
  periods" in a format string would be a success message that asserts coverage while measuring
  nothing, which is a named failure mode in this project.
