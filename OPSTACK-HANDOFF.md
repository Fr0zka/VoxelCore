# Handoff — VoxelForge operator stack, updated 2026-08-16 (four things queued on ONE build)

> Paste the block below into a fresh session. Everything it refers to is on disk and in git.
>
> **State:** 8 of 8 archetypes ported and green. Tile-skipping is measured **in the automation
> harness** (11 of 40 tiles proved `AllSolid` at production defaults, 14641 voxels brute-forced,
> 0 violations) and **still unobserved in the running game**. `AUDIT §C2` is **fully closed** (both
> halves — verified 2026-08-16, don't reopen). `experimental` is pushed.
>
> ## ⛔ FOUR unbuilt things are stacked on `experimental`. Build once, read four numbers.
>
> | # | commit | what to read |
> |---|---|---|
> | 1 | `e002bd4` VerticalShafts connector capsules | `Box verdicts over 60 VerticalShafts tiles` — **0 has been the number for the project's whole life**; `violations` must stay 0 |
> | 2 | `eb317d9` `stat VoxelForge`, 8 counters | baseline `TilesOpStackSolid` = 0 → tick one `TunnelNetwork` strate → **non-zero**. That is the production proof of T1.d, which has never existed |
> | 3 | `7dbdf51` `ExtraReach` × `VF_PerlinAbsBound` | Maze/VerticalShafts may prove **FEWER** tiles. **That is correct, not a regression** |
> | 4 | `eaa44bf` `Max3` radius envelope | **a NO-OP at shipped defaults is the correct result** — any moved number means the diff did more than intended |
>
> In all four: the eight equivalence tests must stay green, and `violations` must stay 0.
>
> **⚠️ 3 and 4 are CORRECTNESS fixes to box verdicts, found by auditing all 28 `EffectOverBox`
> implementations.** Both were the same mistake: *a bound taken from the parameter that reads like
> the maximum instead of the supremum of what `Eval` actually produces* — and both times a correct
> instance of the same reasoning already existed elsewhere in the same file. See
> `OPSTACK-PROGRESS.md` 2026-08-16 (e) and (f); the sound-and-checked ops are listed there so they
> are not re-audited.

---

You're picking up the VoxelForge UE5 voxel plugin on branch `experimental` (already checked out —
do not create another). I'm Jahni. The design and the history are written down so you don't
re-derive them.

## Read first, in this order

1. **`CLAUDE.md`** — project rules. **Rule #1 is absolute: never build, compile, or run the editor.**
   I build everything myself. When code is done, stop, say "ready to build", list the likely
   compile-error spots, and wait.
2. **`OPSTACK-PROGRESS.md` — THE LAST ENTRY FIRST.** Append-only log; the resume point.
3. **`OPSTACK-PLAN.md`** — the plan. **§2.6.1 is the acceptance bar** and supersedes §2.6.
4. **`OPSTACK-DECOMPOSITION.md`** — per-archetype breakdown. **§0.2** (the amplitude bound) is now
   *implemented*, not pending; §2 TunnelNetwork and §8 Underwater are history, not instructions.
5. **`AUDIT-2026-07.md`** — **§C2's SDF-cache half is FIXED (2026-07-28)**, its live-edit half
   (`OC_Chunk` / `BM_Chunk` / `FChunkBiomeCache`) is still open; §C10 is SOLVED, don't reopen;
   §C9's library half is the top open theoretical risk with 0 measured exposure.
6. **`CODEMAP.md`** — navigation. Trust symbol names over line numbers.

## How we work now — Codex writes, you orchestrate

From 2026-07-29 this project runs **in tandem with Codex (Model Luna, xHigh)**. **Codex handles most
of the coding; you orchestrate.** Concretely:

- You read the code and decide *what* to do; you write **precise specs** Codex executes; you **review
  what comes back against the real code, not against its description**; you own the docs
  (`OPSTACK-PROGRESS.md`, `CODEMAP §3`, this file) and the measurements.
- **Hand Codex the INVARIANT, not just the task.** This codebase's traps are invisible in a diff —
  density sign, `Identity` meaning `Sdf ≥ T` (below), cache keys needing params + `LayoutVersion`,
  inserting classes above the anonymous-namespace end marker. A spec that omits these gets code that
  compiles and deletes collision.
- `CODEX-TASK-*.md` at the plugin root are the specs. Each carries a **Why**, the **exact site**, the
  **invariants**, an **acceptance** section, and **notes for the reviewer**. Write the next one the
  same way.
- Unchanged: **never build** (Jahni does), and a plausible patch is not a verified one until a
  measurement says so.

## Where things stand

All 8 archetypes have an operator-stack twin, per-strate opt-in, each equivalence-tested **bit for
bit** against its original density function. The `switch` and the stack are two complete,
interchangeable implementations.

Everything sits behind `UVoxelStrateDefinition::bUseOperatorStack`; the ported list lives **only** in
`UVoxelStrateManager::UsesOperatorStackForChunk` (all 8). **No strate asset has the box ticked** —
that is my call and I still haven't made it. `GetDensityAt` and `ClassifyTile` build the stack
through the **same** factory, `VF_BuildOpStackForChunk` — a second copy would be a hole, not a bug.

### ✅ T1.d — the tile-skipping prize — is real and measured **in the harness** (not yet in the game)

`FRoomGraphSource::EffectOverBox` answers **spatially**. The result, brute-forced voxel by voxel:

```
[production defaults]  11 of 40 tiles proved AllSolid — 14641 voxels checked, 0 violations
[dense fixture]         0 of 40                       — correct, and structurally inevitable
```

One function's verdict is inherited by `FSdfConvertOp`, the twelve detail modifiers (via
`VF_NoCaveOverBox`) **and** `FWormFieldSource` — fourteen operators from one place. That is what the
C1 wiring was built for.

### ⚠️⚠️ THE ONE INVARIANT THAT CAN DELETE COLLISION — read before touching any op

**`FRoomGraphSource::EffectOverBox` returning `Identity` now means `Sdf ≥ T`, NOT `Sdf == FLT_MAX`**,
where `T = max(3·SDFBlendRadius, WormNetworkRange)`. That is sound only because all three consumers
of the SDF channel were read one by one:

| consumer | threshold |
|---|---|
| `FSdfConvertOp::Eval` | `Sdf >= Blend`, and the tunnel stack passes `MakeSdfCarve(P.SDFBlendRadius, …)` ⇒ **K** |
| the twelve modifiers | `VF_NearCaveSurface` ⇒ **3K** |
| `FWormFieldSource::Eval` | `CaveSDF >= WormNetworkRange` ⇒ **WormNetworkRange** |

**Any new consumer of `InOut.Sdf` must have a threshold ≤ `T`, or be added to that `max`.** An op
reading `Sdf < 100` would see false `Identity` verdicts and produce tiles with no geometry **and no
collision**. The warning is written at the site you land on when you add one.

(The `−K` slack covers *any* number of primitives because `SmoothMin`'s penalty is exactly zero once
`|A−B| ≥ K`, so the running minimum saturates at `K` below the smallest term. Without that
observation the slack would scale with the ~88 tunnels in a cache and the criterion would be dead.)

## First actions — one build to read, two tasks to hand Codex

### (a) Hand Codex `CODEX-TASK-001-tile-skip-stats.md`, then `-002-` — 001 first, they chain

Everything in this refactor has been proved in an automation harness on 40 sampled tiles, and
**nothing has ever been observed in the running game.** Task 001 adds a `stat VoxelForge` group with
`TilesClassified / TilesSkippedAllSolid / TilesSkippedAllAir / TilesMeshed`, **plus
`TilesOpStackSolid / TilesOpStackAir` at a second site**. Task 002 adds `ColumnMemoHit / Miss` to the
same group; it needs 001's header to exist, and both should land in one build.

Its deliverable is a **before/after that constitutes the production proof of T1.d**: after ticking
`bUseOperatorStack` on one `TunnelNetwork` strate and flying the same route, **`TilesOpStackSolid`
must go non-zero**. The spec carries the invariants — most importantly that `bTrivialEmpty` decides
whether a tile has **collision**, and that `GenerateTileResult` runs on **worker threads** so a plain
`static int32++` is a data race.

⚠️ **Corrected 2026-08-16 — the earlier version of this bar was unmeasurable.** It said
`TilesSkippedAllSolid` would read 0 underground with no strate opted in. It will not: `ClassifyTile`
also proves `AllSolid` on its **hand-written** path (a bedrock-gap chunk sets `bCanAir = false`,
`VoxelGenerator.cpp` ~2835), which fires with nothing ticked at all. That is the same "~84 %" caveat
below, which the old bar quoted and then contradicted. The op-stack-only counters are zero **by
construction** — the cave branch returns `Mixed` at `UsesOperatorStackForChunk` — so they are the
ones that prove anything.

Interim answer if Jahni wants it before that lands: **Unreal Insights already shows this.** The trace
scopes `VoxelForge_ClassifyTile` and `VoxelForge_GenerateMesh` exist at the site; a skipped tile is a
`ClassifyTile` with no `GenerateMesh` after it. ⚠️ But ~84 % of tiles were *already* being rejected by
the hand-written SurfaceWorld/bedrock paths long before this work, so surface skips will drown the
cave ones — you must be **underground in an opted-in `TunnelNetwork` strate** for the number to mean
anything.

### (b) Build `e002bd4` (VerticalShafts) and read ONE line

Everything before it is built and green.

> Build, run the `VoxelForge` filter, and read
> **`Box verdicts over 60 VerticalShafts tiles`**.
>
> **0 was the number for the whole project's life.** Its `EffectOverBox` used to return `CarveOnly`
> because a shaft merely *existed* within a `Spacing*1.6` halo — true almost everywhere at
> `ShaftSpacing 55 / ShaftDensity 0.6`. It now rebuilds the connectors the way `GetCells` does and
> tests the real capsules, with **Z exact** and XY conservative.
>
> - **Non-zero, and `violations` still 0** ⇒ it worked; record it and move on.
> - **Still 0** ⇒ the warning in that test names what to check **first**: `ExtraReach` inflates both
>   remaining tests, so compare it against `ShaftMaxRadius` before touching either test. **Do not
>   re-derive from scratch** — that is exactly what cost three rounds on TunnelNetwork.

## Then, in order

1. **PERF — the biggest open item, and now AIMED (2026-08-16). Read this before touching it.**
   The op path is measurably slower; one cause was already found and fixed (the column memo
   discarded itself every chunk). Three things were worked out since, all still **unmeasured**:

   - **The A/B needs no new code.** `VoxelForge_ClassifyTile` and `VoxelForge_GenerateMesh` already
     exist, and the world is deterministic, so two Insights traces — flag off, then on, same seed and
     route — are a clean before/after.
   - **But it is unreadable without task 001.** With the stack on, tiles get *skipped*, so
     `GenerateMesh` runs fewer times; a total conflates "cheaper per tile" with "fewer tiles" and
     those pull opposite ways. `TilesMeshed` is the denominator. **⇒ 001 is a PREREQUISITE here, not
     a parallel item.** Order: 001 → traces → attribution → fix.
   - **The suspects don't share an archetype**, so measure one ticked strate at a time.
     *SurfaceWorld* = the hashed column memo. *TunnelNetwork* = **19 virtual calls per voxel**
     (16 from `BuildTunnelNetworkStack` + 3 from `AppendStructuralPost`), plus the known 12× gate
     re-test (stage B5's deliberate trade).

   `CODEX-TASK-002` tests the SurfaceWorld suspect and **fixes nothing on purpose** — the derivation
   says the direct-mapped 4096-slot table evicts ~25 % of columns *every Z plane* (the mesher
   pre-samples **Z-outermost**), for a derived ~9× on column work. **Derived, not measured.**
   **Measure before optimising** — the §C10 lesson, re-learned the hard way last session.
2. **The warp squeeze — PARKED with its ceiling measured, my recommendation is leave it.** The
   `WARP SHARE` line says over half the remaining blocking is the query-box dilation, not geometry
   (production: rooms 0.9 → 0.4, tunnels 2.4 → 1.1 with the dilation zeroed). The only remaining
   route is proving `sup|Perlin3D|` down from the proved **1.5** toward its apparent ~1.0–1.1, worth
   ~27 % of the dilation. Spot-checking a grid is **not** a proof and a wrong sup is a hole.
   **A negative result is already recorded so nobody repeats it:** bounding the warp *locally*
   (evaluate at the box centre, shift, dilate by the variation) is **worse** — a rigorous per-axis
   Lipschitz bound is `4·1.875 + 1 = 8.5` per unit cell, and `8.5 × 0.206` (the half-box in noise
   units) `= 1.75` exceeds the global range bound of 1.5.
3. **`AUDIT §C9` library half** — `sinf`/`cosf` are not IEEE-754 specified, so MSVC's CRT and glibc's
   libm can differ. Currently **0 samples within 1e-6 of the isosurface**, i.e. no measured risk. Run
   `CrossPlatformDigest` on Linux, compare the SHAPE digest, pin it. The real fix if ever needed is a
   deterministic in-house sin/cos.
4. ~~**`AUDIT §C2`'s remaining half**~~ — **✅ CLOSED, verified 2026-08-16. Do not re-open, and do
   not spec a fix for it — I nearly did.** `OC_Chunk`, `BM_Chunk` and `TC_BiomeCache` all carry a
   layout-version guard (`OC_Version` / `BM_Version` / `TC_SeenVersion`), `FChunkBiomeCache` has an
   explicit `Invalidate()` that all four `thread_local` instances call on a version change, and the
   only other two instances in the tree are **function-local**, so they cannot go stale. Recorded in
   `AUDIT-2026-07.md §C2`. The live-edit half was fixed at the same time as the determinism half;
   only this list was stale.
5. **Phase 3 — ops as data assets.** A design conversation, not a transcription. Don't start it
   unprompted. What makes it possible is already in place: ops depend on capabilities
   (`IVoxelBiomeField`), never on `UVoxelGenerator`.

## Debts — status changed, read this before acting on the old text

1. **"Box bounds read STRATE params but a per-room op can raise them" — DORMANT, not urgent.**
   Checked rather than paid, and the check reversed the premise: when the source proves `Identity`
   the twelve modifiers are `Identity` **soundly** (their `bNearCaveSurface` gate never opens, so no
   room op can enable anything), and when it answers `Both` it supplies no `MaxCarveOverBox`, so the
   default `FLT_MAX` kills every hypothesis regardless of what the modifiers claim. **It goes live
   the day `FRoomGraphSource` gains a `MaxCarveOverBox`** — bounding the converter's `2·BaseDensity`
   would make the modifiers' own numbers matter for the first time. Written at the site.
2. **`AUDIT §C2` — FIXED on the `switch` path.** `GetDensityWithParams` now takes **required**
   `ParamsFingerprint` + `LayoutVersion`. Required, not defaulted, so a caller that forgets fails to
   compile. The CRC is taken **once per chunk** where the params memo already lives (`CP_TunnelFP`) —
   a `MemCrc32` per voxel on the hottest path would have been a real regression. Note the audit's own
   suggested alternative ("add chunk Z to the key") is both insufficient (`Interleaved` makes `Alpha`
   depend on chunk **XY** too) and destructive (chunk XY is deliberately absent so `WorldX ± 1`
   gradient probes don't thrash the box — `ARCHITECTURE §8.10`).

## Hard rules that prevent real bugs

- **Density sign:** negative = solid at the mesher. Inside the op stack the convention is INTERNAL
  (**positive = solid**), negated once by the caller. The SDF channel uses standard SDF convention.
- **`Identity` from the room source means `Sdf ≥ T`.** See the boxed invariant above. This is the
  single most dangerous thing in the current code.
- **Never run both density paths in one world.** **Comparing them is legitimate** — §C10 is closed
  since `FPSemantics = Precise`, and all eight equivalence tests compare bit for bit. They are
  **port-correctness oracles**, not fidelity checks: §2.6.1 requires *same seed ⇒ same world on every
  peer*, not resemblance to the pre-refactor world.
- **Every cache key includes `LayoutVersion` AND the params.** See §C2 and the overhang regression of
  2026-07-27, where omitting the params silently deleted the overhang and only 1 sample in 20 000
  crossed the isosurface.
- **A bound in a box verdict must be PROVED, not observed.** `|Perlin3D| ≤ 1.5` is derived from
  `GradDot`'s two-distinct-axes form and the per-axis weighted bound of 0.5 — *not* from the header's
  "~[-1,1]". Over-estimating costs CPU; under-estimating deletes collision.
  **⚠️ USE `VF_PerlinAbsBound` — it is file-scope in `VoxelDensityOpStack.cpp` and it is the ONLY
  copy. Never write a bare `1.0` for a noise amplitude in a reach.** `VoxelNoise::FBM` **normalises**
  (`return Total / MaxValue`), so `sup|FBM| = sup|Perlin3D|` **exactly** — the octave sum neither
  amplifies nor attenuates it, and an `FBM`-driven reach needs the same 1.5. This rule was written
  *before* three `ExtraReach` formulas were found violating it (2026-08-16, fixed in `7dbdf51`):
  VerticalShafts and Maze were unsound at their shipped defaults, FloatingIslands sound only because
  its `SDFBlendRadius` happens to be large. **A rule stated in a doc is not a rule enforced in code** —
  when you add a reach, grep for `VF_PerlinAbsBound` and use it.
- `ProcessQueue` stays `EQueueMode::Mpsc`; `Epoch` carries through every async path; don't "optimize"
  the `ARCHITECTURE §8.10` invariants.
- Commit per coherent unit with a real message. **`experimental` is pushed and tracked
  (`origin/experimental`, since 2026-07-29) — keep it in sync. NEVER push `main`**, which stays the
  known-good fallback at the commit it has always been. ⚠️ A pushed commit here is **not** a
  "verified green" marker: the branch carries unbuilt work by design, and only `OPSTACK-PROGRESS.md`
  says what was actually built.
- Update `CODEMAP §3`, `ARCHITECTURE §8`, tick `OPSTACK-PLAN`, append to `OPSTACK-PROGRESS.md`.
- **When inserting a class into `VoxelDensityOpStack.cpp` / `VoxelHeightOpStack.cpp`, put it ABOVE
  the labelled end of the anonymous namespace.** Anchoring on the FACTORIES banner puts it outside,
  and the brace added with it closes nothing. Made that mistake twice; both files say so.
- **Match the codebase's spelling of engine macros.** `KINDA_SMALL_NUMBER`, not
  `UE_KINDA_SMALL_NUMBER` — the plugin uses the unprefixed form everywhere.

## Method lessons this refactor actually paid for

Ordered by how much they cost.

- **⭐ Instrument what you ASSUMED, not just what you changed.** This is the expensive one, learned
  over four rounds in one session. The warp dilation — `CaveWarpStrength · VOXEL_NOISE_SCALE ·
  PerlinAbsBound`, a constant chosen in the first commit — inflated a 10-voxel tile into a 50-voxel
  query box, **125× the volume**. Four separate tightenings (the worm, the columns, the sampler, the
  tunnel disjunction) were each individually correct and each landed *around* that untouched term.
  The tunnel fix, predicted "an order of magnitude", delivered 25 % — **and the instrument said so,
  and I credited the tunnels.** *When a fix under-delivers against its predicted size, suspect the
  constant you never measured.*
- **Instrument before hypothesising.** §C10 cost six builds and five refuted hypotheses. In this
  session the attribution line (`AllSolid killed by: …`) was written after *two* wrong guesses and
  immediately named a third operator nobody had looked at. **A diagnostic that lists candidate causes
  without measuring them is still a guess wearing rigour** — my "either the tiles straddle cave or
  the source isn't reaching Identity" warning offered two causes and both were wrong.
- **Verify the premise before reasoning from it.** Six times now a confident chain rested on an
  unchecked assumption and the check reversed it. Latest three: `RoomSpacing` was **42** (the fixture
  overrides it) while I did three rounds of arithmetic with the header default of 80 — *the number
  was printing in the report I kept quoting*; "the plugin bets on `|Perlin3D| ≤ 0.8`" was wrong (the
  cache **rebuilds** when the warped query leaves the box, so that expansion is a perf heuristic);
  and the per-room-op debt "must be paid first" was wrong (it is dormant). **Include the premises you
  are confident enough about not to look up — especially a default, when a fixture exists whose whole
  job is overriding defaults.**
- **A sampler must cover at least one period of what it samples.** The tunnel test drew tile XY from
  **±32 voxels** with `RoomSpacing 80` and a guaranteed origin room at (0,0) — it measured the spine
  hub and called it the world. The shaft test had the identical bug (±48 against `ShaftSpacing 55`).
  Both now print their own extent **in units of the pattern's period**.
- **A test fixture tuned for coverage can be antagonistic to the thing you are measuring.**
  `EnableTunnelFeatures` densifies (`RoomSpacing` 80→42, `RoomDensity` 0.35→0.85) so the equivalence
  check isn't comparing solid rock to solid rock — and at that density the room cull radius *equals*
  the lattice spacing, so **no box can ever be proved**. `0 proved` there is the correct answer. The
  box verdict is therefore measured on **both** densities, and the dense run must stay at 0.
- **Diagnostics report THIS run; history goes in the log.** The test output had accumulated hardcoded
  numbers from previous runs beside live ones ("32 of 34 tiles" printed while the live figure was 21
  of 28). Unreadable, and self-inflicted.
- **Read the code, not the comment.** The cliff modifier's comment promises a sampled Z±1 gradient;
  the code samples nothing and uses a Z-stretched Perlin it *calls* `VertGrad`. Ported as written —
  and written down, so nobody "fixes" it from the comment.
- **A perf change can be a correctness change.** The column-memo optimisation silently deleted the
  overhang; the tests caught it the same day. Invisible to inspection, and it produced plausible
  terrain.
- **Coverage is a number, not a boolean.** Four related traps, each producing a green run that proved
  almost nothing:
  - *A test that prints nothing on success is indistinguishable from one that never ran.*
  - *A guard that only trips at zero notices absence, it does not measure coverage.* Use fractions.
  - *A success message that **asserts** coverage instead of reporting it reads as evidence while
    measuring nothing.*
  - *A check can be vacuous as well as a counter.* "Nothing leaked" is worthless unless something
    happened.
- **Enabling a feature is not evidence it fired — ask the structure, not the output.** Setting
  `PitDensity` did nothing (wrong struct). **Prefer the check that can fail for exactly one reason**
  — and when a zero has several possible causes, give each one its own number.
- **An oracle that shares the defect under test proves nothing.** The stale-cache check compares each
  stack against *itself evaluated alone*. (Since §C2 was fixed, the test call sites now pass a real
  params fingerprint, so the original no longer shares the defect either.)
- **One definition, not two kept in sync.** `VF_BuildOpStackForChunk` exists because a tile skipped on
  the verdict of a stack that is not the one producing its density is a hole. The same reasoning is
  why `GetLastRoomBoxDiagnostic` **reads back** what the operator computed instead of letting the
  test re-derive the criterion, and why the two-density tile scan is one lambda called twice.
- **Don't assert a number you want to improve.** Check 4 asserted `0 proved` — honest when written,
  and it would have forbidden the entire T1.d gain. What it asserts now is that **no proved tile is
  wrong** (brute force, every voxel); the proved count is *reported*.
