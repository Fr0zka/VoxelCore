# Handoff — VoxelForge operator stack, 2026-07-28 (end of day 3)

> Paste the block below into a fresh session. Everything it refers to is on disk and in git.

---

You're picking up an agreed refactor of the VoxelForge UE5 voxel plugin, on branch `experimental`
(already checked out — do not create another). I'm Jahni. Design and progress are written down so you
don't re-derive them.

## Read first, in this order

1. **`CLAUDE.md`** — project rules. **Rule #1 is absolute: never build, compile, or run the editor.**
   I build everything myself. When code is done, stop, say "ready to build", list the likely
   compile-error spots, and wait.
2. **`OPSTACK-PROGRESS.md` — THE LAST ENTRY FIRST.** Append-only log of what is built vs merely
   written. This is the resume point.
3. **`OPSTACK-PLAN.md`** — the plan. **§2.6.1 is the current acceptance bar** and supersedes §2.6.
4. **`OPSTACK-DECOMPOSITION.md`** — all 8 archetypes broken into ops. §2 TunnelNetwork (the one in
   progress), §0.2 the worm bound, §7 islands, §5 SurfaceWorld.
5. **`AUDIT-2026-07.md`** — §C9's library half is the top open risk. **§C10 is SOLVED — don't
   reopen.** §C1 was reopened and re-closed on 2026-07-28; §C2 has a new *suspected* item worth
   verifying before acting on.
6. **`CODEMAP.md`** — navigation. Trust symbol names over line numbers.

## Where things stand — 6 of 8 ported and wired, the 7th half-ported, 13 tests green

| Archetype | State |
|---|---|
| `Maze` | ✅ ported, bit-identical, wired |
| `FlatPlain` + `CrystalChamber` | ✅ **one op for both**, bit-identical, wired |
| `SurfaceWorld` | ✅ ported incl. **biome blending**, bit-identical, wired |
| `VerticalShafts` | ✅ ported, bit-identical, wired — 3 ops reused from Maze unchanged |
| `FloatingIslands` | ✅ ported, bit-identical, wired — the stack that runs **backwards** |
| `TunnelNetwork` | 🔶 **STAGE A of 3 done and verified.** Not wired — see below |
| `Underwater` | ❌ TunnelNetwork + a water flag; folds in at stage C |

Everything is behind `UVoxelStrateDefinition::bUseOperatorStack`; the ported list lives **only** in
`UVoxelStrateManager::UsesOperatorStackForChunk`. Un-ported archetypes ignore the flag, so ticking it
anywhere is harmless.

## TunnelNetwork is staged — read this before touching it

`GetDensityWithParams` is ~1080 lines: 13 detail modifiers, a two-region cache, worms, and a per-room
op override. Porting it whole before anything can be checked would be ~600 unverified lines on top of
~200 — the `AUDIT §P3` pattern this refactor has dodged seven times. So:

- **Stage A — DONE, bit-identical over 6000 samples.** Vertical scale · base rock · cave warp · room
  graph (+ pits + chimneys) · carve · worms · structural post. **7 ops.**
- **Stage B — NEXT.** The 13 detail modifiers of `STEP 4b–4h`, gated on `Sdf < SDFBlendRadius·3`.
  They insert between the carve and the worms, so the op-count assertion in the test must move.
- **Stage C.** The per-room op override (`§2`'s option (a)), the `Underwater` water flag, and only
  then does `UsesOperatorStackForChunk` return true for either.

**Why stage A is verifiable while incomplete:** every detail modifier is amplitude-gated, and
`FStrateGenerationParams` already defaults all of them to zero. Zeroing `SurfaceRoughness` (the one
exception) sends the *original* down exactly the path stage A ported. Stage B's test will do the
reverse — turn them on one group at a time.

**⚠️ The decision that must not be undone:** `FRoomGraphSource` **CALLS** `BuildChunkCache` /
`EvaluateSDFCached`; it does not transcribe them. That is where `ARCHITECTURE §8.4`'s two-region
window-invariance discipline lives, and a transcription would *fork* it — with the fork "validated"
by a test that compares it to the original. Only the ~60 lines of glue are transcribed.

## Three things Phase 2 settled that were not in the original design

1. **Height space** (`VoxelHeightOp.h`) — a *second operator family*. SurfaceWorld's terrain ops read
   and write an **altitude**, not a density: no input Z, XY-pure per column. They do not fit
   `IVoxelDensityOp`. §0.1 found density needed a second *channel*; this found terrain needs a second
   **space**. Bonus: a height stack *cannot* hold Z-dependent data, because there is no Z in the
   signature — `AUDIT §6.3`'s hazard became a type error instead of a convention.
2. **`IVoxelBiomeField`** — ops depend on a *capability*, never on `UVoxelGenerator`. The adapter
   (`FGeneratorBiomeField`) lives in `VoxelGenerator.cpp`. This is what lets ops become assets in
   Phase 3; an op holding a generator pointer never could.
3. **`FRAME` ops are RETIRED — porting all three candidates killed the idea.** `CaveWarp`'s scope is
   exactly one operator (pits/chimneys explicitly read *unwarped* coords, the thing §2 called the
   fiddliest in the decomposition — inside one op it evaporates). `VerticalScale` is `Z / Scale`, a
   one-line pure function. The island warp was already local. **Zero frames from three candidates:**
   not missing infrastructure, one idea seen three times from a distance. Marked retired in §1.

## What is left, in the order I'd do it

1. **Stage B** — the 13 detail modifiers. The bulk of the remaining lines.
2. **Stage C** — per-room op override + `Underwater` + flip both on in `UsesOperatorStackForChunk`.
   **The override is NOT optional polish:** pits, chimneys and columns exist *only* through a
   per-room `UVoxelTerrainOpDefinition` (`BuildChunkCache` opens its bake with
   `if (!CR.RoomOp) continue;` and then reads a *fresh* param struct). No override ⇒ no pits, ever.
3. **The worm amplitude cap (`DECOMPOSITION §0.2`)** — the largest single perf item in the plan.
   TunnelNetwork proves **0 of 40** tiles today because a fielded-noise carve has no spatial bound.
   But its amplitude is bounded and trivial (`t ∈ [0,1]`, `Mask ∈ [0,1]` ⇒ at most `WormStrength`
   toward air), and that bound already has a home in `FWormFieldSource::MaxCarveAmplitude()`. It
   needs a fold that carries **numbers**, not just directions.
4. **`ClassifyTile` still uses hand-written guards.** `ClassifyBox` is brute-force verified per
   archetype but **nothing consumes it in production**. That is where measured tile-skipping turns
   into frames — arguably the biggest single win still on the table.
5. **PERF — parked by Jahni until the transition is complete.** The op path is measurably slower.
   One cause found and fixed (the column memo discarded itself every chunk). Remaining suspects in
   order: the hashed column lookup vs `GSurfColCache`'s direct-indexed box, then per-voxel virtual
   dispatch. **Measure before optimising** — that is the C10 lesson.
6. **`AUDIT §C9` library half** — `sinf`/`cosf` are not IEEE-754 specified, so MSVC's CRT and glibc's
   libm can differ. Currently **0 samples within 1e-6 of the isosurface**, i.e. no measured risk. The
   real fix, if ever needed, is a deterministic in-house sin/cos. Also: run `CrossPlatformDigest` on
   Linux, compare the SHAPE digest, pin it.
7. **`AUDIT §C2`, suspected** — `GetGenerationParams` blends params across Gradient transitions, so
   two chunk Zs inside one strate may hold different params with the same XY box, strate index and
   seed ⇒ the SDF cache never rebuilds ⇒ the lower chunk gets the upper chunk's rooms. **Verify the
   premise before acting** (does Gradient blending actually vary within a strate?). The op stack does
   not inherit it — its key folds in a params CRC.
8. **VerticalShafts proves 0 of 60 tiles.** Pessimistic, not wrong: `EffectOverBox` returns
   `CarveOnly` whenever any shaft is within a `Spacing*1.6` halo instead of testing real connector
   capsules. Lost CPU, never a hole.

## ⚠️ IF YOU ARE RUNNING UNATTENDED — read this section before starting

Jahni may start this session and leave, then have you shut the PC down when you're done.

**You cannot build, and nobody will build for you during this run.** Rule #1 still holds absolutely.
Everything you write stays unverified until he returns.

**That is his explicit, repeated decision, not an oversight — do not re-litigate it and do not
quietly scale the work down.** Verbatim: *"I'm fine with unverified work, if committed, I can always
manually go back and try — I want him to work towards the full op stack completeness."* So the goal
of an unattended run is **to finish the transition**, not to stop somewhere safe. Take the queue
below as far as it goes.

Two things make that reasonable rather than reckless, and they are worth knowing:

1. **Committing per group IS the safety net.** He reverts by sha. That is why the one-commit-per-group
   rule is the one thing not to bend.
2. **Reaching 8 of 8 does not change his world by itself.** `UsesOperatorStackForChunk` returns false
   unless a strate has `bUseOperatorStack` ticked, so adding `TunnelNetwork` and `Underwater` to the
   ported list is inert until *he* ticks a box. The flag flip is safe to include; pointing a live
   world at it stays his call.

### The rules that replace "build after each step"

1. **One commit per group below. Never bundle two.** Jahni reverts by group, and a failed build must
   bisect to a group rather than to 700 lines. Name the group explicitly in the subject line.
2. **Every commit leaves the tree coherent.** No half-written op, no dangling brace, no factory
   declared without a definition. Assume the next thing that happens is a compile.
3. **Transcribe literally. Do not improve anything.** With no test feedback, a "cleanup" is an
   unfalsifiable guess. Keep the original's operation order, its constants, its `FVector`
   round-trips, its odd `FMath::Max` floors. If something looks wrong, **write it down in the
   progress log and port it as-is** — that is how `§C1` and the `MinDivisor` split were both found
   without breaking anything.
4. **Update the test in the same commit as the code it covers**, including its coverage counters. Do
   not leave "I'll add the test after" — after is a build away.

### The work queue, in order. Run it to the end — **8 of 8 is the finish line.**

- **B1 — Surface roughness** (`STEP 4b`, ~130 lines). Density-space variant: a *different op* from
  `FSdfRoughnessMod` (quadratic fade, anti-fill clamp, 4 noise types). See `DECOMPOSITION §1`.
- **B2 — Terrace · LayerLines · Ribbing** (~150 lines). The three "sedimentary" remaps.
- **B3 — Overhang · Cliff · Scallop · Arch** (~210 lines).
- **B4 — Columns · Domes · Pinch · FloorBias** (~190 lines). Columns read `SDFCache.Columns`, so this
  group needs a Column op in the test's room pool — and the stage-A guard that *errors* on
  `TotalColumns > 0` must move to this commit, not before it.
- **B5 — the gate itself.** All of B1–B4 live inside `if (bNearCaveSurface)`, i.e.
  `Sdf < SDFBlendRadius·3 && Sdf < FLT_MAX`. Decide deliberately whether that is one scoping wrapper
  or a repeated early-out in each op, write down which and why, and make the test prove a voxel
  *outside* the gate is untouched.

After each group, extend the test: turn that group's amplitudes ON in `EnableTunnelFeatures`, add a
coverage counter proving the group actually fired (**not** just that its param is non-zero — see the
pit/chimney lesson below), and move the op-count assertion.

**⚠️ The subtlety that lets B precede C.** Inside `if (bNearCaveSurface)` the original *shadows*
`Params` with `LocalTerrainParams` — a copy with the nearest room's terrain op applied — and **all 13
modifiers read the shadowed copy**. Porting them against strate-level params is therefore equivalent
*only while no room carries a detail-type op*. The test's pool is Pit/Chimney only and `ApplyTo(Pit)`
writes just the four pit fields, so the two are identical today. **Keep the pool that way through
B1–B5**; C1 is what makes a `Terrace` op in the pool legal. (B4's Column op is safe: `STEP 4d` reads
`SDFCache.Columns` from the bake, never the shadowed params.)

- **C1 — the per-room op override.** `§2` calls this the piece with "no clean home" and offers two
  options. **Take option (a), and note that the mechanism is already established in this codebase —
  do not invent a scoping predicate.** Three ops already read state owned by an upstream op via a
  non-owning pointer handed over at build time: `FOverhangShelfMod` ← `FSurfaceColumnSource`,
  `FShaftLedgeMod` ← `FShaftFieldSource`. Do the same here:

  `FRoomGraphSource` gains `const FStrateGenerationParams& LocalParamsAt(x, y, z) const`, which
  reproduces the original exactly — take the voxel's `NearestRoomIdx` (it already computes it), apply
  that room's `RoomOp` onto a `thread_local` copy of the params, return it. Every detail op holds a
  `const FRoomGraphSource*` and reads its fields from that copy instead of from its own.

  This is the pit/chimney resolution a second time: the difficulty in `§2` came from assuming each
  modifier must own its params. Let one op own the shared state and the rest read it, and the problem
  disappears. **Transcribe the per-voxel copy as-is** — yes, the original copies a ~74-field struct
  per voxel inside the gate; note it in the log as a perf item and change nothing.

  Acceptance: put a `Terrace` op in the test's room pool alongside Pit/Chimney and require
  bit-identity. That is the check B could not make.
- **C2 — `Underwater`.** Nearly free: `§8` establishes there is **no density difference at all** —
  `GetDensityAt` routes it to `GetDensityWithParams` and `WaterLevelRelative` is consumed by the
  render-side water system, not by density. So it is the TunnelNetwork stack, reached from a second
  `case`. Add the wiring case and an equivalence test that samples the `Underwater` slot. If you find
  a real density difference, **stop and write it down** — that would contradict `§8` and is worth
  more as a finding than as code.
- **C3 — 8 of 8.** Add `TunnelNetwork` and `Underwater` to `UsesOperatorStackForChunk`'s ported list.
  Update `CODEMAP §3.2d` and its `UsesOperatorStackForChunk` row, tick `OPSTACK-PLAN` Phase 2, and
  write the closing `OPSTACK-PROGRESS` entry: the archetype `switch` now has a complete operator-stack
  twin, per-strate opt-in, every one equivalence-tested.

### After 8 of 8 — keep going, in this order

1. **Settle `AUDIT §C2`'s suspected item.** Does `GetGenerationParams` actually vary within one strate
   across a Gradient transition? Answerable by *reading* `GetGenerationParams` and
   `FStrateGenerationParams::Lerp` — no build. Turn the suspicion into a yes or a no with the
   evidence, and correct the audit either way. Cheap, and it closes an open question honestly.
2. **The worm amplitude cap (`DECOMPOSITION §0.2`).** The largest single perf item in the plan, and
   the reason TunnelNetwork proves 0 of 40 tiles. It needs the fold to carry a **number**, not just a
   direction: add a numeric carve/fill amplitude alongside `EVoxelOpEffect`, have
   `FWormFieldSource::MaxCarveAmplitude()` (already written, already unused) feed it, and let
   `AllSolid` survive when the rock is solid by more than the sum of every remaining carve.
   **⚠️ This changes the fold contract that all 13 tests rest on — its own commit, nothing else in
   it,** and `VoxelForge.OpStack.BoxVerdictFold` must be extended in the same commit.
3. **Make `ClassifyTile` consume `ClassifyBox`.** Verified per archetype, still unconsumed in
   production — this is where measured tile-skipping (Maze 23/60, slabs 36–40/60) becomes frames.
   A false verdict is an invisible hole, so keep the brute-force check in every archetype's test.
4. **Re-read your own diffs against the original**, group by group, as a reviewer rather than an
   author. On a run with no builds, a second reading is the only oracle you have. Do this rather than
   starting anything new when the queue runs out.

### Still not yours to do, even unattended

- **Do NOT build, do not launch the editor, do not push.** Unchanged, not negotiable.
- **Do NOT tick `bUseOperatorStack` on any strate asset.** C3 makes the archetypes *available*;
  pointing a live world at an unverified path stays Jahni's decision.
- **Do NOT start Phase 3** (ops as data assets). It is a design conversation, not a transcription.

### The commit messages ARE the ledger

**Jahni's words: "just commit with explicit name what changed, I'll revert if needed."** So there is
no separate bookkeeping — the commit history is the record, and it has one job: let him revert a
single group without touching the others.

That makes the subject line load-bearing. Name the group and the scope, not the intent:

```
feat(opstack B2): port Terrace, LayerLines, Ribbing detail modifiers
feat(opstack B4): port Columns, Domes, Pinch, FloorBias + column op in the test pool
feat(opstack C1): per-room op override — detail ops read LocalParamsAt from the room source
feat(opstack C3): TunnelNetwork + Underwater in the ported list — 8 of 8
```

Body: what was transcribed, anything that looked wrong and was ported as-is anyway, and what breaks
first if the group is wrong. One group per commit — that rule exists *because* of the revert
workflow, so it is the one thing not to bend.

**Close the run with one `OPSTACK-PROGRESS` entry** listing every commit made, in order, with a
one-line "what this touches" each, and the test filter to run (`VoxelForge`). Jahni's first action on
return is a single build; that entry is what makes it efficient. Then shut down if he asked you to.

## Hard rules that prevent real bugs

- **Density sign:** negative = solid at the mesher. Inside the op stack the convention is INTERNAL
  (**positive = solid**), negated once by the caller. The SDF channel uses standard SDF convention.
- **Never run both density paths in one world.**
- **The acceptance bar is `§2.6.1`:** *same seed ⇒ same world on every peer*. Resemblance to the
  pre-refactor world is **not** required. The equivalence tests are **port-correctness oracles**, not
  fidelity checks — keep them for that reason.
- **Every cache key includes `LayoutVersion` AND the params.** See `§C2` and the overhang regression
  of 2026-07-27, where omitting the params silently deleted the overhang and only 1 sample in 20 000
  crossed the isosurface.
- `ProcessQueue` stays `EQueueMode::Mpsc`; `Epoch` carries through every async path; don't "optimize"
  the `ARCHITECTURE §8.10` invariants.
- Commit per coherent unit with a real message. **Never push.** `main` is the known-good fallback.
- Update `CODEMAP §3`, `ARCHITECTURE §8`, tick `OPSTACK-PLAN`, append to `OPSTACK-PROGRESS.md`.
- **When inserting a class into `VoxelDensityOpStack.cpp` / `VoxelHeightOpStack.cpp`, put it ABOVE
  the labelled end of the anonymous namespace.** Anchoring on the FACTORIES banner puts it outside,
  and the brace added with it closes nothing. Made that mistake twice; both files now say so at the
  exact line.

## Method lessons this refactor actually paid for

Ordered by how much they cost.

- **Instrument before hypothesising.** §C10 cost six builds and five refuted hypotheses, then was
  solved for free by a build setting changed for an unrelated reason. Park a question whose
  consequences are measured and benign.
- **Verify the premise before reasoning from it.** Four times now, a confident chain rested on an
  unchecked assumption and the check reversed it: C1's *documented* fix was wrong; "C9's risk is gone
  after FPSemantics" was wrong; "C1 is closed, 0 sites left behind" was wrong (the sweep matched a
  *spelling*, `SeedF * K`, and the survivor spelled it `(float)S * K`); "PitDensity enables pits" was
  wrong. **A grep over a spelling is evidence about the spelling.**
- **A perf change can be a correctness change.** The column-memo optimisation silently deleted the
  overhang; the tests caught it the same day. The failure was invisible to inspection and produced
  plausible terrain.
- **Coverage is a number, not a boolean.** Three related traps, each of which produced a green run
  that proved almost nothing:
  - *A test that prints nothing on success is indistinguishable from one that never ran.* Report
    counts, not just failures.
  - *A guard that only trips at zero does not measure coverage, it notices absence.* A run with 65 of
    6000 samples in open cave (1.1 %) passed a `== 0` guard silently. Use fractions.
  - *A success message that **asserts** coverage instead of reporting it reads as evidence while
    measuring nothing.* One said "pits and chimneys exercised" through a whole run in which zero pits
    existed.
- **Enabling a feature is not evidence it fired — ask the structure, not the output.** Setting
  `PitDensity` did nothing (wrong struct). Diffing two stacks with/without the op pool would have
  *lied* (the pool is not in the SDF cache key, so both share the `thread_local` cache). What worked:
  call `BuildChunkCache` and look at `Pits.Num()`. Prefer the check that can fail for exactly one
  reason.
- **An oracle that shares the defect under test proves nothing.** The stale-cache check was going to
  compare two interleaved param sets against the original — which keys its SDF cache without the
  params and would have failed. Compare against the thing itself evaluated alone instead.
