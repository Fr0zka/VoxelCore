# Codex task 006 — the same `Min > Max` under-bound, in `BuildChunkCache` (both density paths)

**Owner:** Codex (Model Luna, xHigh) · **Orchestrator:** Claude · **Branch:** `experimental`
**Status:** specified, not started
**Kind:** ⚠️ **correctness of a collection bound.** Third instance of this class; the worst of the three.
**Credit:** found by the Sol-High read-only audit (`AUDIT-2026-08-CODEX.md`, VF-05) and **verified
against the code** before being specified.

---

## Why this exists

`CODEX-TASK-004` fixed two cell-sweep pads that assumed the field named `Max*` was numerically the
larger one. **The same defect exists in `VoxelCaveMorphology.cpp`, and it matters more**, for three
reasons:

1. It is **`TunnelNetwork`** — the largest and most-used archetype.
2. `BuildChunkCache` is called by **both** density paths: the original `switch` *and*
   `FRoomGraphSource`, which deliberately calls it rather than transcribing it. **This is not an
   operator-stack bug — it is in the shipped original code and always has been.**
3. Its failure mode is a **window-invariance break** (`ARCHITECTURE §8.4`), not just a missing room:
   whether a room exists depends on which chunk you queried from. In a multiplayer game that means
   two peers generate different geometry from the same seed.

### The mechanism

Radii are interpolated:

```cpp
Room.RadiusXY = FMath::Lerp(Params.MinRoomRadius,   Params.MaxRoomRadius,   SizeFactor);  // ~262
const float RadA = FMath::Lerp(Params.TunnelMinRadius, Params.TunnelMaxRadius, FactorA);  // ~467
const float RadB = FMath::Lerp(Params.TunnelMinRadius, Params.TunnelMaxRadius, FactorB);  // ~468
```

`FMath::Lerp(A, B, t)` with `t ∈ [0,1]` yields anywhere in `[min(A,B), max(A,B)]` — it does **not**
require `A ≤ B`. But every bound derived from those radii reads only the `Max*` field:

| site | line | expression |
|---|---|---|
| `MaxInfluence` | ~127–130 | `FMath::Max(Params.MaxRoomRadius, Params.TunnelWarpStrength + Params.TunnelMaxRadius) + Params.SDFBlendRadius` |
| `CollectMargin` | ~152 | `2.0f * MaxTunnelLen + MaxInfluence` (inherits it — **no separate edit needed**) |
| `RoomZBuffer` | ~171 | `Params.MaxRoomRadius * Params.RoomHeightRatio` |
| `EvaluateSDF`'s `Margin` | ~871–874 | the same expression as `MaxInfluence`, duplicated |

With `MinRoomRadius > MaxRoomRadius`, rooms larger than `MaxInfluence` are generated, so a room that
can reach a chunk may sit in a cell the collect region never visited. `RoomReachesSearchBox` uses the
*actual* radius and is therefore correct — but it can only test rooms that were collected at all.

## The fix

Derive **bound-only envelopes** and use them at all four sites:

```cpp
const float RoomRadiusEnvelope   = FMath::Max(Params.MinRoomRadius,   Params.MaxRoomRadius);
const float TunnelRadiusEnvelope = FMath::Max(Params.TunnelMinRadius, Params.TunnelMaxRadius);
```

- `MaxInfluence` → `FMath::Max(RoomRadiusEnvelope, Params.TunnelWarpStrength + TunnelRadiusEnvelope) + Params.SDFBlendRadius`
- `RoomZBuffer` → `RoomRadiusEnvelope * Params.RoomHeightRatio`
- `EvaluateSDF`'s `Margin` → the same corrected expression.

⚠️ `MaxInfluence` and `EvaluateSDF`'s `Margin` are the **same formula written twice**. They must stay
identical. If a shared helper is natural here, use one — two copies of a rule that must agree is a
bug factory, and this file already has the duplication. If you introduce a helper, keep it local to
this translation unit and do not change either call site's semantics.

## ⛔ DO NOT reorder the `Lerp` endpoints

Swapping to `Lerp(min, max, t)` maps the same hash `t` to a **different radius** for the same room,
which changes generated geometry and breaks the eight bit-for-bit equivalence tests. **Only the
bounds may become conservative. The three `Lerp` calls must not be touched at all.**
This is the same rule as tasks 003 and 004, and it is the third time it applies.

## ⚠️ Invariants

1. **No `Lerp` line changes. No room/tunnel placement, hashing, or `bStore` logic changes.**
   If your diff touches `Room.RadiusXY`, `RadA`, `RadB`, or `RoomReachesSearchBox`, stop and say so.
2. The change is strictly conservative: a larger envelope collects **more** cells, never fewer.
3. Exactly one file: `Source/VoxelForge/Private/VoxelCaveMorphology.cpp`.
4. Comments are French + English; match the file. Say **why** the envelope is `max(Min, Max)` — the
   next reader must not "simplify" it back to `MaxRoomRadius`.

## Acceptance

- One file changed, a handful of lines.
- **With correctly ordered params, `max(Min, Max) == Max`, so every number is bit-identical and this
  is a NO-OP. That is the acceptance signal.** The eight equivalence tests, every box-verdict line
  and every `violations` count must be **unchanged** after the build. A moved number means the diff
  did something it should not have.
- It only changes behaviour for an asset whose range is inverted — which is precisely the case that
  was silently producing window-dependent geometry.

## Notes for the reviewer (Claude)

- Confirm all four bound sites use the envelopes, and that `CollectMargin` inherits rather than being
  edited separately.
- Confirm `MaxInfluence` and `EvaluateSDF`'s `Margin` still compute the identical expression.
- Confirm no `Lerp` argument order changed anywhere in the file.
