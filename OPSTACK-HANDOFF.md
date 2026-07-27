# Handoff — VoxelForge operator stack, 2026-07-27 (end of day 2)

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
4. **`OPSTACK-DECOMPOSITION.md`** — all 8 archetypes broken into ops. §5 SurfaceWorld, §6 shafts,
   §7 islands, §2 TunnelNetwork.
5. **`AUDIT-2026-07.md`** — §C9 is the top open risk (half fixed). **§C10 is SOLVED — don't reopen.**
6. **`CODEMAP.md`** — navigation. Trust symbol names over line numbers.

## Where things stand — 5 of 8 archetypes ported, 11 tests green

| Archetype | State |
|---|---|
| `Maze` | ✅ ported, bit-identical, wired |
| `FlatPlain` + `CrystalChamber` | ✅ **one op for both**, bit-identical, wired |
| `SurfaceWorld` | ✅ ported incl. **biome blending**, bit-identical, wired |
| `VerticalShafts` | ✅ ported, bit-identical, wired — **3 ops reused from Maze unchanged** |
| `FloatingIslands` | ❌ next (§7) |
| `Underwater` | ❌ (§8 — TunnelNetwork + a water flag) |
| `TunnelNetwork` | ❌ **LAST**, deliberately — owns `BuildChunkCache`'s two-region window-invariance discipline (§8.4), the most delicate code in the plugin |

Everything is behind `UVoxelStrateDefinition::bUseOperatorStack`; the ported list lives **only** in
`UVoxelStrateManager::UsesOperatorStackForChunk`. Un-ported archetypes ignore the flag, so ticking it
anywhere is harmless.

## Two things Phase 2 invented that were not in the original design

1. **Height space** (`VoxelHeightOp.h`) — a *second operator family*. SurfaceWorld's terrain ops
   (cliff/terrace/layer-lines/beach) read and write an **altitude**, not a density: no input Z,
   XY-pure per column, neither density nor SDF touched. They do not fit `IVoxelDensityOp`. §0.1 found
   density needed a second *channel*; this found terrain needs a second **space**. Bonus: a height
   stack *cannot* hold Z-dependent data because there is no Z in the signature — `AUDIT §6.3`'s
   hazard became a type error instead of a convention.
2. **`IVoxelBiomeField`** — ops depend on a *capability*, never on `UVoxelGenerator`. The adapter
   (`FGeneratorBiomeField`) lives in `VoxelGenerator.cpp`. This is what lets ops become assets in
   Phase 3; an op holding a generator pointer never could.

## What is left, in the order I'd do it

1. **`FloatingIslands`** (§7) — next port.
2. **`Underwater`** (§8) — TunnelNetwork + a flag; fold in once TunnelNetwork is done.
3. **`TunnelNetwork`** (§2) — last, and take §8.4's window-invariance discipline seriously.
4. **PERF — deliberately parked by Jahni until the transition is complete.** Generation is measurably
   slower on the op path. One cause found and fixed (the column memo was discarding itself every
   chunk). Remaining suspects in order: the hashed column lookup vs. `GSurfColCache`'s direct-indexed
   box, then per-voxel virtual dispatch. **Measure before optimising** — that is the C10 lesson.
5. **`AUDIT §C9` library half** — `FPSemantics = Precise` fixed the compiler half; `sinf`/`cosf` are
   not IEEE-754 specified, so MSVC's CRT and glibc's libm can still differ. Currently **0 samples
   within 1e-6 of the isosurface**, i.e. no measured risk. If it ever must be zero *by construction*,
   the fix is a deterministic in-house sin/cos, not a build flag. Also: run
   `CrossPlatformDigest` on Linux and compare the SHAPE digest, then pin it.
6. **VerticalShafts proves 0 of 60 tiles** (Maze 23, slabs 36-40). Pessimistic, not wrong: its
   `EffectOverBox` returns `CarveOnly` whenever any shaft is within a `Spacing*1.6` halo instead of
   testing real connector capsules. Lost CPU, never a hole.
7. **`ClassifyTile` still uses hand-written guards.** `ClassifyBox` exists and is brute-force verified
   per archetype, but **nothing consumes it in production yet**. That is where the measured
   tile-skipping (23 / 36 / 40 of 60, against today's zero) actually turns into frames — arguably the
   biggest single win still on the table.

## Hard rules that prevent real bugs

- **Density sign:** negative = solid at the mesher. Inside the op stack the convention is INTERNAL
  (**positive = solid**), negated once by the caller. The SDF channel uses standard SDF convention.
- **Never run both density paths in one world.**
- **The acceptance bar is `§2.6.1`:** *same seed ⇒ same world on every peer*. Resemblance to the
  pre-refactor world is **not** required. The equivalence tests are **port-correctness oracles**, not
  fidelity checks — keep them for that reason.
- **Every cache key includes `LayoutVersion` AND the params.** See the §C2 note and the overhang
  regression of 2026-07-27, where omitting the params silently deleted the overhang and only 1 sample
  in 20 000 crossed the isosurface.
- `ProcessQueue` stays `EQueueMode::Mpsc`; `Epoch` carries through every async path; don't "optimize"
  the `ARCHITECTURE §8.10` invariants.
- Commit per coherent unit with a real message. **Never push.** `main` is the known-good fallback.
- Update `CODEMAP §3`, `ARCHITECTURE §8`, tick `OPSTACK-PLAN`, append to `OPSTACK-PROGRESS.md`.

## Method lessons this refactor actually paid for

- **Instrument before hypothesising.** §C10 cost six builds and five refuted hypotheses, then was
  solved for free by a build setting changed for an unrelated reason. Park a question whose
  consequences are measured and benign.
- **Verify the premise before reasoning from it.** Twice, a confident chain rested on an unchecked
  assumption, and twice the check reversed the answer — C1's *documented* fix was wrong, and "C9's
  risk is gone after FPSemantics" was wrong.
- **A test that prints nothing on success is indistinguishable from one that never ran.** Report
  coverage counts, not just failures. This bit three times before it stuck.
- **A perf change can be a correctness change.** The column-memo optimisation silently deleted the
  overhang; the test suite caught it the same day. This is the concrete answer to "why all these
  tests" — the failure was invisible to inspection and produced plausible terrain.
- **When inserting a class into `VoxelDensityOpStack.cpp` / `VoxelHeightOpStack.cpp`, put it ABOVE
  the labelled end of the anonymous namespace.** Anchoring on the FACTORIES banner puts it outside,
  and the brace added with it closes nothing. Made that mistake twice; both files now say so at the
  exact line.
