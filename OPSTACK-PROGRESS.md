# OPSTACK — progress log

> **APPEND ONLY. Never rewrite or reorder entries.** Write the entry for a piece of work *before*
> starting it, so an abrupt session end still leaves an accurate marker.
>
> **Entry format:** date · what · believed-true · **UNVERIFIED** (everything not yet built by Jahni) ·
> next single action.
>
> This file is how a fresh context resumes. Read the last entry first, then `OPSTACK-PLAN.md §9`.

---

## 2026-07-27 — branch created, design locked, nothing built

**What:** Branch `experimental` created from `69fa73e tmp` and checked out. `main` untouched and is the
known-good fallback world. Design finalised across `OPSTACK-PLAN.md` (incl. §2.5 op taxonomy and §2.6
acceptance bar, both added after Jahni's objection that a naive "ops" reading would just be his old
room-operations system). Kickoff prompt written to `OPSTACK-PROMPT.md`.

**Believed true:** the plugin builds and runs correctly as of this commit — everything through F20
phases 1+2 is built and working (confirmed by Jahni 2026-07-26; the "PENDING BUILD" markers still in
`fable-idea.md` / `ARCHITECTURE.md` are stale and are queue item Q3). `bEnableDensityVolume` is already
set to false. Lighting is deprecated for now. Live direction before this refactor was 3D density
generation (overhangs, caves inside mountains).

**UNVERIFIED:** nothing yet — no code has been written for this refactor.

**Known open bugs, not yet fixed, documented in `AUDIT-2026-07.md §1`:** C1 large-seed noise collapse ·
C2 three cache keys missing `LayoutVersion` · C3 `FMath::PerlinNoise2D` on the SDF hot path (dormant) ·
C4 `GetPlayerPosition` zero-vector sentinel · C5 unbounded-join-on-shutdown · C6 diff-layer scaling.

**Next single action:** Phase 0.5 — the three automation tests (density purity across worker threads,
`ClassifyTile` vs brute force, `DiffLayer` under contention). Then the Phase 1 skeleton header only.
Then STOP for a build. See `OPSTACK-PROMPT.md` → `WHAT TO DO`.

---

## 2026-07-27 — Q4 done: the design docs are in git

**What:** `.gitignore` replaced the single `!CODEMAP.md` exception with `!*.md`, and committed
`ARCHITECTURE.md`, `AUDIT-2026-07.md`, `CLAUDE.md`, `OPSTACK-PLAN.md`, `OPSTACK-PROGRESS.md`,
`OPSTACK-PROMPT.md`, `REVIEW_FINDINGS.md`, `fable-idea.md` (commit `3128852`). `CODEMAP.md.bak` stays
ignored (`.bak` doesn't match `*.md`).

**Why first, out of queue order:** every `*.md` was ignored, so a `git commit` of this progress file
would have silently recorded nothing. The whole unattended crash-safety discipline depends on this
file being committable, so it had to lead.

**Believed true / verified:** verified — `git ls-files "*.md"` now lists all nine.

**UNVERIFIED:** nothing (no C++ touched).

**Next single action:** Phase 0.5 tests + the Phase 1 skeleton header.

---

## 2026-07-27 — starting Phase 0.5 (tests) + Phase 1 skeleton header

**Plan for this batch, written before doing it** (so an abrupt end leaves a marker):

1. `Source/VoxelForge/Private/Tests/VoxelForgeDeterminismTest.cpp` — density purity, shuffled order,
   multi-threaded (`ParallelFor`), bit-equality.
2. `Source/VoxelForge/Private/Tests/VoxelForgeClassifyTileTest.cpp` — `ClassifyTile` verdict vs a
   brute-force sweep of the exact mesher lattice.
3. `Source/VoxelForge/Private/Tests/VoxelForgeDiffLayerTest.cpp` — N readers + 1 writer, monotonic
   version, no crash.
4. `Public/VoxelDensityOp.h` — the Phase 1 contract. HEADER ONLY, nothing wired into `GetDensityAt`.

**Known design constraint found while reading:** `ClassifyTile` and `GetDensityAt`'s per-chunk caches
need a live `UVoxelStrateManager`, whose only entry point is
`Initialize(UVoxelSettings*, int32)` reading `TSoftObjectPtr` pools. Tests therefore build transient
`UVoxelStrateDefinition` objects and point soft pointers at them. **If that resolve turns out not to
work headless, test 2 self-skips rather than failing** — noted here because a skip that looks like a
pass is exactly the kind of thing that hides a bug.

**UNVERIFIED:** everything below this line until Jahni builds. No test has ever been compiled in this
plugin — `Private/Tests/` is a new directory and the module has never pulled in `AutomationTest.h`.

**Next single action:** write the three test files.

---

## 2026-07-27 — batch A+B landed, BUILD GATE reached

**Commits, oldest first:**

| Commit | What | Risk |
|---|---|---|
| `3128852` | `.gitignore !*.md` + the eight design docs into git (Q4) | none |
| `6eec796` | the three Phase 0.5 tests + `VoxelForgeTestFixture.h` | new files only |
| `d41d34e` | `Public/VoxelDensityOp.h` + `EVoxelTileClass` moved to `VoxelTypes.h` | header-only |
| `73f6b26` | **AUDIT C2 fix** — five caches now key on `GetLayoutVersion()` | ⚠️ the only commit that touches a hot path |
| `b4d13e0` | regression test for C2 | new file only |

**Believed true:** `73f6b26` is behaviour-neutral at a static layout — the layout version only
moves on `Initialize`, so a running world sees the same keys it saw before, plus one `uint32`
compare per chunk-change. It changes behaviour only after a `RebuildStrates` / editor live edit,
which is the bug.

**Design decision worth not re-litigating:** `ClassifyBox` in `VoxelDensityOp.h` is deliberately
NOT source-only. `ApplyBoundarySeal` does `Max(D, SealFactor·BaseDensity)` inside its band — it
*forces* solid regardless of input, and pure direction (`FillOnly`) cannot express that. Over a box
sitting entirely in the top seal band the source says AllAir, `FillOnly` kills AllAir, both
hypotheses die → Mixed, whereas `ClassifyTile` returns AllSolid there today. Not a hole, but a
silent loss of exactly the trivial tiles T1.d exists to skip. So forcing ops reset the fold, and
ops after them still apply. The full mapping from the fold to today's `ClassifyTile` is written out
in the header.

**Deliberately NOT done, and why:** AUDIT **C1** (unbounded `SeedF`) is a one-line fix at six
sites, and `OPSTACK-PLAN §8` lists it first. I left it. It is dormant at small seeds, and applying
it changes `SeedF` from `Seed` to `hash(Seed)&0x3FFF` — i.e. it re-rolls the whole world's noise
even at today's seed, forcing a re-tune. Doing that in the same build Jahni is using to judge four
other unverified changes costs him attribution for no present-day benefit. It should land on its
own, deliberately, when he has an hour to re-tune. **It is still a real bug — do not lose it.**

**UNVERIFIED — everything above.** None of it has been compiled. In particular the test fixture's
`TSoftObjectPtr` → transient-object resolve has never run.

**Next single action:** build-free queue. Q1 (`OPSTACK-DECOMPOSITION.md`), then Q2, then Q3.

---
