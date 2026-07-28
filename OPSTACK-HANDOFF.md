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
