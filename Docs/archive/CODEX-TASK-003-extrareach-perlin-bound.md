# Codex task 003 — three `ExtraReach` formulas use an FBM bound this file already proved wrong

**Owner:** Codex (Model Luna, xHigh) · **Orchestrator:** Claude · **Branch:** `experimental`
**Status:** specified, not started
**Kind:** ⚠️ **correctness of a box verdict** — the class of bug that deletes collision. Not a perf task.

---

## Why this exists

`VoxelDensityOpStack.cpp` contains a rigorous, written derivation that `|Perlin3D| ≤ 1.5`, exposes it
as `PerlinAbsBound` (line ~2336), and uses it correctly for the tunnel warp dilation (~2452). The
comment there is explicit that the loose "~[-1,1]" figure from the noise header is **not** to be
relied on, and `OPSTACK-HANDOFF.md` records the standard: *a bound in a box verdict must be PROVED,
not observed — over-estimating costs CPU, under-estimating deletes collision.*

**Three `ExtraReach` formulas in the same file silently assume `sup|FBM| ≤ 1.0`.** Each carries the
comment "FBM ∈ [-1,1]", which is exactly the claim the file disproves 1800 lines earlier.

And `VoxelNoise::FBM` **is normalised** — it returns `Total / MaxValue` where `MaxValue = Σ Amp`
(`VoxelNoise.h` ~272). So `sup|FBM| = sup|Perlin3D|` exactly: **1.5, not 1.0.** The octave sum
neither amplifies nor attenuates the bound.

### What that costs, per archetype, at the shipped defaults

`Identity` from these sources means "no primitive within `ExtraReach` of the box", i.e. `Sdf ≥
ExtraReach` throughout. Roughness then does `Sdf += FBM · VOXEL_NOISE_SCALE · Strength`
(`FSdfRoughnessMod::Eval`), so worst case `Sdf' ≥ ExtraReach − B·1.25·|Roughness|`. Soundness
requires `Sdf'` to stay at or above the downstream carve/fill threshold.

| archetype | `ExtraReach` at defaults | downstream threshold | needs `B ≤` | verdict at `B = 1.5` |
|---|---|---|---|---|
| **VerticalShafts** (`Rough 3.0`) | `1.25·3 + 2 + 1` = **6.75** | carve blend **2.0** | **1.27** | ⛔ **UNSOUND** (margin −0.875) |
| **Maze** (`Rough 2.0`) | `1.25·2 + 2 + 1` = **5.5** | carve blend **2.0** | **1.40** | ⛔ **UNSOUND** (margin −0.25) |
| **FloatingIslands** (`Rough 4.0`, `K 5.0`) | `1.25·4 + 2·5 + 1` = **16.0** | fill `K` **5.0** (+ `K/6` SmoothMin dip) | **2.03** | ✅ sound — but only because `K` is large. Sound by parameter luck, not by construction. |

Break-even roughness for the two carve archetypes is `|Rough| ≤ 1.6`; they ship at 3.0 and 2.0.

**How alarmed to be, stated honestly.** No strate has `bUseOperatorStack` ticked, so nothing in the
running game is affected today. The brute-force tile scans report 0 violations — but they *sample*,
and they were sampling against a shaft source that proved **zero** tiles until `e002bd4`, so the
shaft path has never been exercised at all. The empirical sup of this Perlin is estimated at
~1.0–1.1, which is *below* the 1.27 the shafts need — which is why nothing has been seen yet, and
also why the margin is uncomfortably thin. The bug is that the verdict rests on an unproved bound,
which is the thing this codebase has already decided it does not do.

## The fix

1. **Hoist `PerlinAbsBound` to file scope and rename it `VF_PerlinAbsBound`**, so there is **one**
   definition rather than a class-static plus three implicit `1.0`s. Keep the existing derivation
   comment with it — it is the justification, not decoration.

   **Naming, resolved:** the file-scope helpers in this file are all `VF_`-prefixed
   (`VF_NearCaveSurface`, `VF_DistPointSegment`, `VF_NoCaveOverBox`), so a file-scope constant takes
   the same prefix. That means this is a **rename**, not just a move:
   - delete the `static constexpr float PerlinAbsBound = 1.5f;` class-static inside `FRoomGraphSource`
     (~2336), moving its whole derivation comment with it;
   - **update `FRoomGraphSource`'s own use at ~2452** (`P.CaveWarpStrength * VOXEL_NOISE_SCALE *
     PerlinAbsBound`) to the new name. This is the one place where the warp dilation is computed and
     it must keep computing the identical value — the rename must not change its arithmetic.
   - after the edit, `grep -n "PerlinAbsBound" ` must show **only** `VF_PerlinAbsBound` occurrences.
2. **Multiply the roughness term by it in all three `ExtraReach` formulas** (~4106 VerticalShafts,
   ~4215 FloatingIslands, ~4247 Maze):

   ```cpp
   // before
   FMath::Abs(P.SurfaceRoughness) * VOXEL_NOISE_SCALE
   // after
   FMath::Abs(P.SurfaceRoughness) * VOXEL_NOISE_SCALE * VF_PerlinAbsBound
   ```
3. **Fix the three comments.** Each says "FBM ∈ [-1,1]". Replace with the real statement: `FBM` is
   normalised (`Total / MaxValue`), so `sup|FBM| = sup|Perlin3D| =` the proved `PerlinAbsBound`.
   A comment that states a refuted bound is how this happened in the first place.

## ⚠️ Invariants

1. **This must not change density by one bit.** `ExtraReach` is read **only** inside
   `EffectOverBox` (verified: every other occurrence is a comment or the `float ExtraReach;` member
   declaration — no `Eval`, no `GetCells`). The eight equivalence tests compare `Eval` bit for bit
   and must stay green. **If you find yourself editing an `Eval`, stop — you have the wrong site.**
2. **The change direction is strictly conservative**: larger `ExtraReach` ⇒ more `CarveOnly`, fewer
   `Identity` ⇒ *fewer* tiles proved uniform. It can only cost CPU, never open a hole. Do not
   "balance" it by tightening something else in the same edit.
3. **Anonymous-namespace placement.** Put the hoisted constant **above the labelled end of the
   anonymous namespace**, not anchored on the FACTORIES banner — anchoring there puts it outside and
   the brace added with it closes nothing. This mistake has been made twice in this file and the
   file says so.
4. **`FRoomGraphSource`'s warp dilation changes NAME ONLY.** It already uses the bound correctly and
   is the reference implementation for this fix; the value it computes must be bit-identical after
   the rename. Do not alter its formula, its `√2` factor, or anything else in that function.
5. Comments are French + English; match the surrounding file.

## Acceptance

- `git diff --stat` shows **one** file: `Source/VoxelForge/Private/VoxelDensityOpStack.cpp`.
- All three `ExtraReach` definitions include the bound; no fourth site exists (`BuildTunnelNetworkStack`
  has no `ExtraReach` — it uses `PerlinAbsBound` directly for the warp).
- No `Eval` body changed.
- After the build: the eight equivalence tests stay green (density unchanged), and the box-verdict
  lines for **Maze** and **VerticalShafts** may report *fewer* proved tiles than before. **A drop
  there is the expected, correct outcome, not a regression** — it is the cost of a sound bound.
  Record the before/after in `OPSTACK-PROGRESS.md`.

## Notes for the reviewer (Claude)

- Confirm the constant is genuinely at file scope inside the anonymous namespace and that the
  class-static is gone, not shadowed — two definitions that can drift is the failure this fixes.
- Confirm all three call sites got it. Two out of three is worse than none, because it looks done.
- Confirm no `Eval`, `GetCells`, or `GetCellsAt` body appears in the diff.
