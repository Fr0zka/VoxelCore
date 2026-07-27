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

## 2026-07-27 — build-free queue done. END OF UNATTENDED SESSION.

**What (queue items, all zero-build-risk):**

- **Q1 — `OPSTACK-DECOMPOSITION.md`** (commit `c188ee8`). All 8 archetypes read line by line and
  broken into source / combiners / modifiers / structural post, with every `FStrateGenerationParams`
  field traced to its destination op. Three findings that change sequencing are in its §0.
- **Q2 — the param audit** is §9 of that same file. Every field is claimed except
  `WaterLevelRelative`, which is a render/water property misfiled in the density struct (and `Lerp`'d
  across strate boundaries, where a water plane arguably shouldn't be). Reported, not deleted.
- **Q3 — stale markers ticked** (commit `831ee2f`): `fable-idea` F20 phases 1/2 + F18,
  `REVIEW_FINDINGS` perf pass 2 + batch 3, `ARCHITECTURE`'s biome full-param redesign. NOT ticked:
  `ARCHITECTURE`'s F6 master material graph — its C++ half is built but the graph is editor-side
  work that is genuinely still open.
- **Q4 — `.gitignore`** (commit `3128852`, done first out of order because progress-log commits
  depended on it).
- CODEMAP §3 rows for every new/moved symbol; `OPSTACK-PLAN` phase markers ticked.

**The three findings, so they are not lost if only this file is read:**

1. **The op contract probably needs an SDF channel as well as a density channel.** Rooms, pits and
   chimneys are `SmoothMin`'d in SDF space before a *single* carve, and three of the four SDF
   archetypes add roughness to the SDF, not to density. A single-channel `Eval` can only overwrite —
   which is also why cross-source `SmoothUnion` (*"a maze inside a mountain that looks like it
   belongs"*) is not expressible without it. **Decide before porting Maze**; it is far cheaper now
   than after four ports. Not applied — it is Jahni's call.
2. **Worm tunnels are why TunnelNetwork can never skip a tile.** A fielded 3D-noise carve with no
   bounds forces `CarveOnly` everywhere, killing `AllSolid` for the most-used archetype. Its
   amplitude is trivially capped by `WormStrength`, so ~10 lines of scalar bound recovers deep-rock
   skipping. Suggests one numeric bound belongs in Phase 2, not Phase 3 as the plan has it.
3. **Disturbances already carry lattice bounds `ClassifyTile` discards** (it only tests
   `ChasmDensity > 0` strate-wide). A win available to SurfaceWorld independently of everything else.

**Believed true:** the working tree is a coherent, committed state. Nine commits on `experimental`,
`main` untouched.

**UNVERIFIED — the whole C++ batch.** Nothing has been compiled. Specifically at risk:
`Private/Tests/` is a new directory, the module has never included `AutomationTest.h`, and the test
fixture's `TSoftObjectPtr` → transient-`UVoxelStrateDefinition` resolve has never run.

**Next single action: BUILD.** Then fix what the tests say, then answer
`OPSTACK-DECOMPOSITION.md §11`, then port Maze. Do not write more plugin C++ before the build.

---

## 2026-07-27 — correction to the 3rd entry above

The entry *"starting Phase 0.5"* says the `ClassifyTile` test **self-skips** if the fixture's
`TSoftObjectPtr` resolve fails headless. That was the plan; it is **not** what was written. All four
tests call `AddError(World.WhyInvalid())` and FAIL, with a message that says explicitly it is a
fixture failure and not a density bug. Failing is the right behaviour — a skip that reads as a pass
is exactly what hides bugs — but the earlier entry describes code that does not exist, so it is
corrected here rather than edited (this log is append-only).

---

## 2026-07-27 — starting Phase 1: the Maze port, OFF the hot path

**Jahni said continue and build later, delegating the open design calls. Two decided, with reasons:**

**(1) `Eval` becomes two-channel** — `FVoxelOpSample { Density, Sdf }` — per
`OPSTACK-DECOMPOSITION.md §0.1`. Maze itself forces the question: its roughness perturbs the **SDF**
(`MazeSDF += noise·Rough`), not the density. Applied to density instead, the same noise scales with
the local gradient and is a visibly different effect. Single-channel could not port Maze faithfully,
never mind compose two sources with `SmoothMin` later. Cost: one float.

**(2) The stack's density channel is INTERNAL convention (positive = SOLID), negated once by the
caller.** This **reverses what `VoxelDensityOp.h` said yesterday** (it specified MC). Reason: every
existing archetype body is written in internal convention and negates on `return`. Porting in MC
would mean flipping the sign of every line at transcription time — on the plugin's documented #1
source of confusion. Internal makes each port a literal transcription instead. `ApplyDisturbances`
and the diff layer genuinely are MC-space, but they live in `GetDensityAt` *after* the archetype
today and are NOT in this stack, so the question is deferred, not dodged.

**The shape of this batch, and why it is not "unverified code on unverified code":** the ops, the
stack and the Maze port are **all new files**, plus one mechanical extraction. **`GetDensityAt` and
`ClassifyTile` are NOT touched** — nothing in the running game can change. The port is validated by
a test that runs the op stack against `GetMazeDensity` over thousands of points and asserts
bit-equality, so Phase 1's real question ("does the source/modifier split fall out naturally?") gets
an empirical answer instead of an opinion. Wiring the stack into `GetDensityAt` waits for the build.

**Files:** `Public/VoxelDensityPrimitives.h` (spine/seal/passage lifted out of `VoxelGenerator.cpp`
so ops and the generator share ONE copy) · `Public/VoxelDensityOpStack.h` +
`Private/VoxelDensityOpStack.cpp` · `Private/Tests/VoxelForgeOpStackMazeTest.cpp` ·
edits to `VoxelDensityOp.h` and `VoxelGenerator.cpp` (include the primitives, delete the local copies).

**UNVERIFIED:** all of it, plus everything from the previous batch.

**Next single action:** write those files, then STOP.

---

## 2026-07-27 — Phase 1 done (written, not built). Maze DOES decompose.

**The Phase 1 question is answered.** `OPSTACK-PLAN §4`'s stop-trigger asked whether the
source/modifier split falls out naturally from the existing code. It does — Maze becomes seven ops
with no contortion, and three of them are already shared with other archetypes:

```
FConstantRockSource      ← also TunnelNetwork's and VerticalShafts' first line
FLatticeCorridorSource   ← Maze only (role 1: what makes a maze a maze)
FSdfRoughnessMod         ← also VerticalShafts, FloatingIslands
FSdfCarveOp              ← the same six lines currently copied in three archetypes
FOriginSpineOp ─┐
FBoundarySealOp ├─ identical in all six density functions
FPassageCarveOp ┘
```

No revert, no stop-trigger. Commit `4c53d3b`.

**What is deliberately NOT wired:** `GetDensityAt` and `ClassifyTile` are untouched, so nothing in a
running world can change. `OPSTACK-PLAN §4` Phase 1 step 3 ("GetDensityAt gains one branch") is
**held back until the build is green** — wiring an uncompiled stack into the hot path would be
exactly the stacked-unverified-work pattern `AUDIT §P3` documents. The port is validated instead by
`VoxelForge.OpStack.MazeEquivalence`.

**Two contract decisions taken (Jahni delegated them):** two-channel `Eval`, and INTERNAL sign
convention inside the stack. Both are argued in the previous entry and in the commit message. The
second **reverses** what `VoxelDensityOp.h` said on 2026-07-26 — if a later context finds MC-in-stack
written anywhere, that text is stale.

**One pre-existing hairline bug found and NOT silently patched:** at the inner edge of a seal band,
`1 - Dist/Thickness` can round to exactly `0.0f`, so `SealFactor·BaseDensity` is 0, internal density
lands on 0, and the mesher's `D >= IsoLevel` counts that point as AIR. Today's `ClassifyTile`
excludes those z from column testing and can therefore emit `AllSolid` over them. It needs the
archetype to produce air at exactly that z, so the window is hairline — but a false `AllSolid` is a
hole. The **new** seal op keeps a 1-voxel safety margin before it forces. The old path is untouched;
`VoxelForge.Determinism.ClassifyTileSoundness` would catch it if it ever fires.

**UNVERIFIED:** everything, still. Nothing has been compiled.

**Next single action: BUILD.** Then, in order: fix what the tests report → read
`MazeEquivalence`'s two numbers (how many samples differ, and how many tiles the stack can prove
uniform) → only then wire the stack into `GetDensityAt` behind a per-strate opt-in.

---

## 2026-07-27 — FIRST GREEN BUILD. Six tests ran. Five passed.

**The plugin compiles and the tests execute.** Results
(`VoxelM/Saved/Automation/Automation2026.07.27-02.36.57.csv`):

| Test | Result | What it means |
|---|---|---|
| `ClassifyTileSoundness` | ✅ | 600 tiles: 471 Mixed, 76 AllSolid, 53 AllAir. 24 brute-forced against the exact mesher lattice, **zero holes**. T1.d's soundness is machine-checked for the first time. |
| `DensityPurity` | ✅ | 10k points, shuffled order, multi-threaded, with and without carves — **bit-identical throughout**. No cache-key bug of the AUDIT C2 family survives in the density path. |
| `LiveEditInvalidation` | ✅ | 64/64 probes moved after a live edit. The C2 fix works. |
| `BoxVerdictFold` | ✅ | The op-stack fold's logic, case by case. |
| `MazeEquivalence` | ✅ (with warning) | see below |
| `DiffLayerContention` | ❌ | **my test was wrong, not the plugin** — see below |

### The two numbers that mattered

**454 of 20000 Maze samples differ, largest |delta| 1.907e-06, and ZERO land on the opposite side
of the isosurface.** 1.907e-06 is exactly one ULP at a float of magnitude 16 — i.e. the ports are
*geometrically identical*: not one triangle would move. §2.6 accepts this. Leading hypothesis for
the residue, now fixed and awaiting a re-run: the original routes the noise coordinates through an
`FVector` (double in UE5) before casting back to float, so it rounds float→double→float, while the
op passed floats straight through. Under `/fp:fast` those round in different places. The op now
reproduces the detour deliberately, with a comment saying not to "simplify" it. **If the next run
still shows drift, the next candidate is FMA contraction differing across translation units.**

**23 of 60 Maze tiles proved uniform.** Today's `ClassifyTile` proves **zero** for Maze — every cave
archetype falls through to `"pas prouvable en v1"`. That is ~38% of tiles becoming skippable for an
archetype that has never skipped one, and it is the first hard evidence for the perf half of the
whole refactor.

### The failure was mine

`Expected 'every carve was recorded' to be 400, but it was 3200.` `GetTotalModificationCount()` sums
**stored entries**, not operations — a stroke is filed under every chunk its AABB overlaps, and 400
radius-6 spheres straddling chunk corners store 8 entries each. The concurrency the test actually
exists to check all passed: **7 reader threads, 28.7 million read rounds against 760 writes and 6
`Clear()`s, no crash, monotonic version, clean state afterwards.** Assertion rewritten to compare
the stored count against the fan-out `ApplyModification` itself reported, which is a stronger check.

Worth noting for AUDIT C6: that getter is the right metric for the diff-layer scaling wall (stored
entries are what grow without bound) and the wrong name for it.

**UNVERIFIED:** the three fixes in this entry (the diff-layer assertion, the `FVector` rounding
detour, and the `FVoxelOpStack` move-only/dllexport fix that made the build pass) have not been
re-run.

**Next single action:** rebuild, re-run, and check whether `MazeEquivalence` now reports 0 differing
samples. Then wire the stack into `GetDensityAt` behind a per-strate opt-in — Phase 1 step 3, which
was deliberately held back until the build went green. It now has.

---

## 2026-07-27 (afternoon) — ALL SIX TESTS GREEN. One hypothesis killed.

Run at 12:02 today (in `Saved/Logs/VoxelM.log`; the CSV export was taken later).

| Test | Result |
|---|---|
| `ClassifyTileSoundness` | ✅ |
| `DensityPurity` | ✅ |
| `DiffLayerContention` | ✅ **now passes** — the assertion fix was right; 33.3M read rounds |
| `LiveEditInvalidation` | ✅ 64/64 probes moved |
| `BoxVerdictFold` | ✅ |
| `MazeEquivalence` | ✅ (same warning) |

**Phase 0.5's gate is met.** All three original tests plus the two op-stack ones are green on the
current code, so the op stack is no longer being built on unverified ground.

### The `FVector` rounding hypothesis was WRONG — and the way it was wrong is informative

The previous entry predicted the fix would take `MazeEquivalence` from 454 differing samples to 0.
The re-run returned **exactly 454 samples, exactly `1.90734863e-06`, at exactly `(-23, 55, -660)`** —
bit-for-bit the same result. The `float → double → float` detour is a no-op, which is what
`/fp:precise` semantics say it should be. Hypothesis eliminated cleanly; the detour is harmless and
stays (it costs nothing and documents the original's shape), but it is **not** the cause.

**Stopped guessing, added a bisect** to `MazeEquivalence`: it re-runs the comparison four times,
disabling roughness → seal → spine → passages, on **both sides**, and reports which stage's removal
makes it bit-exact. One run now answers a question two guesses failed to.

**Standing hypothesis, to be confirmed or killed by that bisect:** the residue is compiler
float-contraction across translation units (`/fp:fast` lets the same expression reassociate
differently in `VoxelGenerator.cpp` and `VoxelDensityOpStack.cpp`), worth ~1 ULP. It fits the
~2% hit rate: **only voxels inside the narrow SDF blend shell have an unsaturated carve factor** —
everywhere else `Carve` is exactly 0 or exactly 1 and both paths agree bit for bit. If that is
confirmed, **bit-identity is not achievable in principle for these ports**, and the standard for
every later archetype becomes "zero isosurface crossings", not "zero differing floats". That is a
conclusion worth having explicitly rather than re-deriving per port.

**UNVERIFIED:** the bisect itself.

**Next single action:** re-run `MazeEquivalence` and read the bisect table. Then Phase 1 step 3 —
wire the stack into `GetDensityAt` behind a per-strate opt-in.

---

## 2026-07-27 (afternoon) — the residue is the COMPILER. Measured, not guessed.

**Bisect result:**

```
roughness off                  ->  151 / 5000 differ (max |delta| 1.907e-06)
roughness + seal off           ->  155 / 5000 differ (max |delta| 9.537e-07)
roughness + seal + spine off   ->  126 / 5000 differ (max |delta| 9.537e-07)
corridors + carve ONLY         ->  126 / 5000 differ (max |delta| 9.537e-07)
```

The residue survives every stage removal, down to **constant rock + capsule SDF + carve** — code
that is a character-for-character transcription. So it is not in anything the decomposition added.

**Cause, from the engine source rather than from memory** (`VCToolChain.cs`):

```csharp
case FPSemanticsMode.Default: // Default is imprecise FP semantics.
case FPSemanticsMode.Imprecise: Arguments.Add("/fp:fast"); break;
```

UBT's own doc for that mode: *"FP math isn't IEEE-754 compliant: the compiler is allowed to transform
math expressions in ways that might result in differently rounded results."* The plugin sets no
override, so identical source in `VoxelGenerator.cpp` and `VoxelDensityOpStack.cpp` may legitimately
reassociate differently — worth about 1 ULP.

Two prior hypotheses were wrong (the `FVector` round-trip, then "check the roughness window / carve
blend"). The bisect cost one build and settled it. **Noted as a working lesson: on a numeric
discrepancy, bisect before hypothesising a third time.**

**Why exactly ~2.3% of samples:** `Blend - Sdf` catastrophically cancels at the edge of the blend
shell, amplifying a 1-ULP SDF difference into a 1-ULP density difference. Outside that thin shell
`Carve` is exactly 0 or exactly 1 and both paths agree bit for bit.

### Consequences recorded

1. **`OPSTACK-PLAN §2.6`** — bit-identity is not achievable in principle for these ports, at any
   level of care. The operational bar for every remaining archetype, now encoded in the test:
   **hard-fail on isosurface crossings · tolerate ULP-scale deltas · warn on anything larger**
   (that last one is real port drift, and the test no longer cries wolf about the floor).
2. **`AUDIT-2026-07.md §C9` (new)** — the part that matters more than the port: `ARCHITECTURE §9.1`'s
   multiplayer model is "replicate the seed, every peer regenerates identically", and under
   `/fp:fast` that holds **only between bit-identical binaries**. Same build, same platform: fine
   (`DensityPurity` proves it). Windows client + Linux dedicated server both regenerating
   authoritative geometry: a real desync source, presenting as rare unreproducible geometry-only
   divergence. The knob is `FPSemantics = FPSemanticsMode.Precise` in `VoxelForge.Build.cs`, and
   **it should not be turned speculatively** — it blocks the vectorisation T2.a was chasing, on the
   plugin's hot loop, for an unmeasured cost. Decision needs a profile and a confirmed
   cross-platform requirement.

**UNVERIFIED:** the test's new ULP-tolerance branch (expect `MazeEquivalence` to report the same 454
samples as INFO rather than WARNING next run).

**Next single action:** Phase 1 step 3 — wire the stack into `GetDensityAt` behind a per-strate
opt-in. Phase 1's question is fully answered: Maze decomposes cleanly, the stack is
window-invariant, and it proves 23/60 tiles uniform where `ClassifyTile` proves zero.

---

## 2026-07-27 — /fp:fast hypothesis DEAD. Third wrong guess. Switching to instrumentation.

**Jahni built with `FPSemantics = FPSemanticsMode.Precise` and got a BYTE-IDENTICAL result:** same
454 samples, same `1.90734863e-06`, same `(-23, 55, -660)`. A different float model producing
identical output is not "the same rounding error twice" — it is proof that **rounding is not the
cause at all.** The residue is a real, deterministic LOGIC difference somewhere in a transcription I
have read three times and believe to be identical.

**Track record on this one discrepancy, recorded because the pattern matters more than the bug:**

| # | Hypothesis | Killed by |
|---|---|---|
| 1 | `FVector` float→double→float round-trip | re-run returned the identical result |
| 2 | "check the roughness window / carve blend / octave count" | the bisect: residue survives to `corridors + carve ONLY` |
| 3 | `/fp:fast` cross-TU reassociation | `/fp:precise` build returned the identical result |

Three hypotheses, all plausible, all reasoned from *what could explain it* rather than from
measurement. Each cost a build cycle. **The lesson is not "be smarter", it is "instrument earlier":**
the bisect (measurement) produced more information in one run than two hypotheses did in three.

**Corrected in the docs:** `AUDIT-2026-07.md §C9` and `OPSTACK-PLAN §2.6` both assert the `/fp:fast`
story as the explanation for the residue. **That specific claim is now falsified and must be walked
back** — see the next entry. (The *separate* C9 finding, that UBT's FP default differs by toolchain
and the MP model assumes bit-reproducible terrain, still stands on its own: it was read out of
`VCToolChain.cs` / `ClangToolChain.cs`, not inferred from this test.)

**What changed in code:** `FVoxelOpStack::EvalSample` now exposes the full `FVoxelOpSample`, and
`MazeEquivalence` dumps the worst point in raw hex — both densities, the stack's internal SDF, and
the carve factor reconstructed from each side. That last one localises the divergence: identical
recovered carve with differing density ⇒ the fault is after the conversion; differing carve ⇒ it is
in the SDF (lattice edges or `VoxelSDF::Capsule`) or in `SmoothStep01`.

**UNVERIFIED:** the instrumentation itself.

**Next single action:** re-run `MazeEquivalence` and read the WORST-POINT DUMP. Phase 1 step 3
(wiring the stack into `GetDensityAt`) is deliberately **paused** until this is understood — it is a
small unexplained numeric difference, and those do not get smaller when you build on them.

---

## 2026-07-27 — CORRECTION: hypothesis 3 was never tested. My error, not Jahni's.

**`FPSemantics = FPSemanticsMode.Precise` was set in `Source/VoxelM/VoxelM.Build.cs` — the GAME
module.** `FPSemantics` is a `ModuleRules` property, i.e. **per module**. Every line of density code
lives in the `VoxelForge` module, which was still compiling `/fp:fast`. The float model never
changed for the code under test.

**So the previous entry is wrong and is retracted:** `/fp:fast` is NOT eliminated. The run that
"reproduced the residue under precise semantics" reproduced it under *fast* semantics, which is
exactly what it should have done and proves nothing.

**The failure mode, stated plainly because it is the third time in one day:** I accepted "it's with
fpsemantics precise" as fact and reasoned a confident conclusion from it — one paragraph after
writing that the lesson was to instrument rather than assume. The check took one `grep` and I did it
only after Jahni suggested it. **Verify the premise before reasoning from it, especially when the
premise is what makes the conclusion interesting.**

**Consequently:** the notes in the previous entry saying `AUDIT §C9` and `OPSTACK-PLAN §2.6` are
falsified are themselves withdrawn. Those documents' `/fp:fast` explanation is back to *plausible
and now genuinely about to be tested*. Nothing in them needs changing yet.

**What changed:** `FPSemantics = FPSemanticsMode.Precise` added to **`VoxelForge.Build.cs`**, clearly
marked as a temporary experiment with removal instructions and a read-the-result guide.

`VoxelM.Build.cs` can keep or drop its copy — it is harmless either way and irrelevant to this test.

**One build now answers everything**, because the WORST-POINT DUMP instrumentation is already in:

| Result | Meaning | Next |
|---|---|---|
| 454 → **0** | the FP model WAS the cause | remove the line; decide separately whether precise is worth its cost on the hot path (needs a profile) |
| 454 → **454** | real logic difference | remove the line; read the dump — identical recovered carve ⇒ fault after the conversion, differing carve ⇒ fault in the SDF or `SmoothStep01` |

**UNVERIFIED:** everything about this experiment.

**Next single action:** rebuild, re-run `MazeEquivalence`, read the two numbers. Phase 1 step 3 stays
paused.

---

## 2026-07-27 — FPSemantics on VoxelForge does not build. Reverted; measuring a safer way.

**What happened:** setting `FPSemantics` on the VoxelForge module broke the build with ~30 errors —
`UMaterialInterface`, `USoundBase`, `TSubclassOf<AActor>`, `APawn`, `ENABLE_DRAW_DEBUG` all
"undefined type". **None of them are FP-related.**

**Why:** UBT can only share a precompiled header between modules whose **compile environments
match**. Changing `FPSemantics` changed VoxelForge's environment, so it lost eligibility for the
engine's shared PCH — and with it ~30 includes the plugin has always been getting for free.

**Genuine latent finding, worth its own item some day:** several public headers use engine types they
never include (`VoxelBiomeDefinition.h`, `VoxelStrateDefinition.h`, `VoxelSettings.h`,
`VoxelStrateTypes.h`, `VoxelContentManager.h`, `VoxelDensityVolume.h`, and `VoxelWorld.cpp`). The
plugin compiles today only because the shared PCH supplies them. UE has been moving away from
implicit shared-PCH includes for years, so this will need doing eventually — **but not inside an
unrelated diagnostic**, which is why it was reverted rather than chased.

**Reverted**, with the reason written into `VoxelForge.Build.cs` so nobody retries it blind.

### The FP question, answered without touching build settings

`MazeEquivalence` now compiles a **verbatim copy of the Maze core into the TEST's translation unit**
and compares three implementations of the same source:

```
A = GetMazeDensity        (VoxelGenerator.cpp TU)
B = the operator stack    (VoxelDensityOpStack.cpp TU)
C = MazeCoreVerbatim      (the test's own TU)
```

- **A != C** ⇒ identical source, different TU, different result ⇒ the compiler, not the port.
  Nothing to fix; record it and move on.
- **A == C, B != C** ⇒ the source IS stable across TUs ⇒ the operator stack differs for a **logic**
  reason, and it is in `FLatticeCorridorSource` or `FSdfCarveOp`.

Duplicating code is normally a fault; here it is the only instrument that answers the question,
because three careful readings all concluded "identical" and the test disagrees. It is marked
diagnostic-only and comes out once the answer is in.

**UNVERIFIED:** everything in this entry.

**Next single action:** rebuild (normal incremental now — the Build.cs change is reverted) and read
the THREE-WAY block. Phase 1 step 3 still paused.

---

## 2026-07-27 — THREE-WAY VERDICT: the fault is MINE, in the operator stack.

```
A generator TU  vs  B opstack TU : 126 differ
A generator TU  vs  C test TU    : 0 differ      <-- identical source, different TU, SAME result
B opstack TU    vs  C test TU    : 126 differ
```

**A == C settles it: the source is stable across translation units.** So the compiler was never the
cause, and the operator stack differs for a **logic** reason. Fourth hypothesis dead — but this one
points at code I own, which is the first time the answer has been actionable.

**Correction to walk back in the docs** (not yet done — do it once the cause is known, so it is
corrected with the right explanation rather than twice):
- `OPSTACK-PLAN §2.6`'s green note claims bit-identity is unachievable because of `/fp:fast`.
  **False.** `A == C` proves identical source reproduces exactly across TUs here.
- `AUDIT-2026-07.md §C9`'s *first* consequence ("refactors cannot be bit-identical") is likewise
  false and must go. **C9's second half stands** — UBT's FP default genuinely differs by toolchain,
  read straight out of `VCToolChain.cs` / `ClangToolChain.cs`, and the MP model does assume
  bit-reproducible terrain. That half was never inferred from this test.
- The test's own INFO text ("this is the expected floor... /fp:fast") is wrong for the same reason
  and gets rewritten with the real cause.

**Also learned, and worth keeping:** setting `FPSemantics` on VoxelForge costs the module the
engine's shared PCH and exposes ~30 missing includes across seven files. Recorded in `Build.cs`.

### Where the fault is NOT

Read line by line against the verbatim copy, all identical: the ctor's `FMath::Max` clamps, the
cell `FloorToInt`, `NodeCenter`, `EdgeOpen`'s hashes and salts, the `{-1,0}³` sweep and its add
order, the capsule loop, the `FMath::Min` fold, the carve's clamp/smoothstep/subtract, and the four
structural-post no-ops. Three readings said "identical" and the measurement disagrees, so **reading
is not going to find it** — hence more instrument, less staring.

### The instrument now in place

`MazeCoreVerbatim` optionally returns its **SDF** and edge count, and the three-way compares the SDF
channels directly instead of inferring from densities:

- **SDF identical, density differs** ⇒ fault is in `FSdfCarveOp`.
- **SDF differs** ⇒ fault is in `FLatticeCorridorSource` (edge set or capsule fold).

It also reports the split across all 126 mismatches, and dumps the first one with raw hex plus the
verbatim edge count — so if the edge SETS differ (a cache-key bug) that shows up as a count mismatch
immediately.

**UNVERIFIED:** the instrumentation.

**Next single action:** rebuild, read `FIRST B-vs-C MISMATCH`. It names the file to open.

---

## 2026-07-27 — LOCALISED to the carve. Testing the right variable this time.

**The diagnostic pinned it exactly:**

```
SDF   stack -1.76393199 [0xBFE1C886]   verbatim -1.76393199 [0xBFE1C886]   IDENTICAL
MC    stack  7.83939362 [0x40FADC50]   verbatim  7.83939266 [0x40FADC4E]   2 ULP apart
across all mismatches: SDF differs 0, SDF identical but density differs 126
```

So the lattice, the hashes, the edge set and `VoxelSDF::Capsule` are all **exactly right** — 126 of
126. The entire difference is in `FSdfCarveOp`, whose expression is character-identical to the
original and whose inputs (`Sdf`, `Blend` 2.0, `BaseDensity` 8) are bit-identical.

**Identical inputs + identical expression + different output ⇒ the arithmetic is being *evaluated*
differently.** And `SmoothStep01` is `x * x * (3.0f - 2.0f * x)` — `3.0f - 2.0f*x` is exactly the
shape MSVC fuses into an FMA, which is one rounding instead of two: **~1 ULP.**

**Why the three-way missed it — worth recording, because it is a reasoning error, not a coding one.**
`A` (GetMazeDensity) and `C` (the verbatim copy) are both straight-line, inlined code. `B` goes
through a **virtual** `IVoxelDensityOp` call, so `FSdfCarveOp::Eval` is compiled out-of-line and can
get a different contraction decision. The three-way tested *"does the translation-unit boundary
change the result?"* — it does not — but the real variable is *"does the optimisation context change
the result?"*. **I designed a clean experiment for the wrong variable, and then believed its answer.**
Hypothesis 3 was not wrong about the mechanism (`/fp:fast` contraction); it was wrong about the test.

**The experiment now added** isolates exactly that variable: the same carve expression, in the same
translation unit, once `FORCEINLINE` and once `FORCENOINLINE`.

- **inlined != FORCENOINLINE** ⇒ FP contraction confirmed. **The port has no bug** — the operator
  stack is arithmetically correct and the residue is unavoidable wherever an op is a virtual call.
  Then: correct the docs with the *real* reason, accept the ULP floor, move on to step 3.
- **inlined == FORCENOINLINE** ⇒ contraction is not it, and there is a real logic bug in
  `FSdfCarveOp` that has now survived four readings.

**UNVERIFIED:** the experiment.

**Next single action:** rebuild, read `INLINING EXPERIMENT`. Either way the answer is final — the
inputs are proven bit-identical, so only the evaluation can differ.

---

## 2026-07-27 — the variable is COMPILE-TIME CONSTANT vs RUNTIME DATA. One line left to confirm.

**The inlining experiment partitioned the measurements perfectly, just not the way it was framed:**

```
inlined carve  !=  FORCENOINLINE carve : 0          <-- inlining is NOT the variable
FORCENOINLINE  ==  operator stack      : 5000/5000  <-- test-TU carve == other-TU op, ALWAYS
inlined        ==  verbatim            : 4874/5000  <-- 126 differ, IN THE SAME TU
```

The test-TU carve matches the operator stack **in a different TU** perfectly, yet disagrees with the
verbatim **in its own TU**. So neither the TU boundary nor inlining is the variable. Sorting the five
implementations by the one remaining difference:

| Implementation | `Blend` is | Group |
|---|---|---|
| `GetMazeDensity` (A) | `const float Blend = 2.0f` | **compile-time constant** |
| `MazeCoreVerbatim` (C) | `const float Blend = 2.0f` | **compile-time constant** |
| `FSdfCarveOp` (B) | a class member | **runtime data** |
| `CarveInlined` | a parameter | **runtime data** |
| `CarveNoInline` | a parameter | **runtime data** |

A == C. B == CarveInlined == CarveNoInline. The two groups differ. **Every single observation from
today fits that split, and nothing else does.**

**Mechanism:** under `/fp:fast`, folding `Blend * 2.0f` to the literal `4.0f` at compile time enables
a contraction in `SmoothStep01`'s `3.0f - 2.0f*x` — one rounding instead of two — that the runtime
form cannot get. ~1 ULP.

**Why this matters far beyond the bug:** an operator's parameters are **data by design** — that is
the entire point of the refactor. They can never go back to being compile-time literals. So this
ULP-level difference is **inherent and permanent** for every archetype port, and no amount of care in
transcription will remove it. That is the real, precise reason bit-identity is unachievable here —
not the vague `/fp:fast` hand-wave I put in the docs earlier, which happened to name the right
compiler flag for the wrong reason.

**Confirming line added:** `CarveConstBlend` — identical to `CarveInlined` except `Blend` is a
compile-time constant. Predicted: matches the verbatim 5000/5000, differs from the runtime form on
exactly 126.

**UNVERIFIED:** that prediction.

**Next single action:** rebuild, read `CARVE VARIABLE ISOLATION`. If it lands as predicted: correct
`OPSTACK-PLAN §2.6`, `AUDIT §C9` and the test's INFO text with the real reason, delete the diagnostic
scaffolding, and **resume Phase 1 step 3** — the port is proven correct (SDF exact on 126/126, only
the final rounding differs, 0 isosurface crossings).

---

## 2026-07-27 — hypothesis 5 dead too. One unambiguous check left, then I stop chasing.

```
CONST-Blend   ==  verbatim      : 4874 / 5000
CONST-Blend   !=  runtime-Blend : 0
```

`Blend`'s constness is **not** the variable: the const and runtime carve forms are bit-identical to
each other, and both miss the verbatim on the same 126. Five hypotheses, five dead.

**Worse, one of the numbers I reasoned from was circular.** `FORCENOINLINE == operator stack :
5000/5000` cannot fail by construction — it feeds `S.Sdf` to a carve and compares against the density
the stack computed *from that same `S.Sdf`*. It measures nothing. I read it as corroboration.

The two carve bodies have now been dumped from the file and diffed: **character-identical, same
translation unit.** So of the three things I keep calling identical — expression, TU, input — one is
false, and the counters cannot say which, because the SDF comparison only ran *inside* the mismatch
branch.

**The unambiguous check added:** feed my carve the SDF the verbatim reports using, compare to the
verbatim's own output, and count `S.Sdf != VerbSdf` **directly, with no enclosing condition**.

- `Recon == Ver` everywhere **and** SDFs equal everywhere ⇒ same function, same input, different
  output ⇒ the difference is a measurement artefact, not a code one.
- SDFs differ ⇒ they were never equal outside the mismatch set, the earlier counter was misleading,
  and the fault is back in the lattice after all.

### Proportion check — this is the last build I would spend on it

**The port is already verified on every axis that affects the game:** SDF bit-exact on 126/126, the
lattice/hashes/`Capsule` exactly right, **0 isosurface crossings out of 20000**, geometry identical,
window-invariant across threads, and every box verdict survives brute force. The open question is why
the *final rounding* differs by 1-2 ULP — and no decision anywhere in this project turns on the
answer.

**So: if this check doesn't resolve it, accept and move to Phase 1 step 3.** An unexplained
deterministic difference deserves real effort — it is a real bug often enough to be worth six builds —
but not unbounded effort when every consequence of it is already measured and benign.

**UNVERIFIED:** the check.

**Next single action:** rebuild, read `UNAMBIGUOUS DISCRIMINATOR`. Then either fix or accept, and in
both cases correct `OPSTACK-PLAN §2.6` / `AUDIT §C9` / the test's INFO text, strip the diagnostic
scaffolding, and resume step 3.

---

## 2026-07-27 — ULP residue PARKED by decision. Phase 1 closed. Moving to step 3.

**Jahni's call, and the right one:** pin it and move on. Six builds spent; the information stopped
being worth the cost.

**The final run did close it as far as it can be closed:**

```
my carve(verbatim's own SDF) == verbatim : 4874 / 5000
stack SDF  !=  verbatim SDF              : 0      (counted directly, no condition)
```

SDFs identical **everywhere**, not just among mismatches. Two character-identical carve
implementations, in the **same translation unit**, fed a **provably identical** input, differ by
1 ULP on 126/5000. For deterministic code that is only possible if they compile to different
instruction sequences — which is exactly what `/fp:fast` permits based on surrounding context, with
no single isolable axis. **Hypothesis 3 was right about the mechanism and wrong about every clean
variable I proposed for it**, which is why four carefully-designed isolation tests all came back
negative.

The one experiment that would settle it (`FPSemantics = Precise` on this module) is blocked behind
the shared-PCH / IWYU debt. Worth clearing on its own terms; not worth clearing to chase 1 ULP.

**Recorded as `AUDIT-2026-07.md §C10`** with the full refutation table, so the next context cannot
re-derive the same five hypotheses at a build each. `OPSTACK-PLAN §2.6` and `§C9` corrected — my
earlier `/fp:fast`-across-TUs explanation was wrong and is gone.

**Scaffolding stripped** from `MazeEquivalence`: the verbatim copy, the three-way, the bisect, the
inlining/constness experiments and the worst-point dump are all out. What remains is the permanent
value: the equivalence check with ULP grading, the window-invariance check, and the box-verdict
brute force.

### Phase 1 is closed. What it proved

- Maze decomposes into **seven** ops with no contortion; three are already shared with other archetypes.
- The **SDF is bit-exact** — lattice, hashes, `Capsule` all correct.
- **Zero isosurface crossings** — geometrically identical to the original.
- **Window-invariant** across query order and worker threads.
- **Every box verdict survives brute force**, and **23 of 60 tiles prove uniform** where
  `ClassifyTile` proves zero for any cave archetype. That is the perf case, measured.

**Next single action:** Phase 1 step 3 — wire the stack into `GetDensityAt` behind a per-strate
opt-in, so a Maze strate can be A/B-switched in the editor and judged on a screenshot (§2.6's bar).

---

## 2026-07-27 — Phase 1 step 3: the stack is WIRED IN, behind a per-strate opt-in.

**`OPSTACK-PLAN §4` Phase 1 step 3 done.** `GetDensityAt` gains exactly one branch, as the plan
specified, and both systems now coexist.

**Files:**
- `VoxelStrateDefinition.h` — `bool bUseOperatorStack` (EditAnywhere, "Use Operator Stack
  (experimental)"). The A/B switch §2.6's acceptance bar needs: flip it, regenerate, judge the
  screenshot.
- `VoxelStrateManager.{h,cpp}` — `UsesOperatorStackForChunk()`. **The ported-archetype list lives
  here and nowhere else**, so an unported archetype ignores the flag and falls back to the switch.
  Ticking the box on any strate is therefore harmless today; only `Maze` changes behaviour.
- `VoxelGenerator.cpp` — `CP_OpStack` / `CP_UseOpStack` built in the SAME refetch block as the
  params (so the chunk+`LayoutVersion` key already covers it, no new invalidation logic), plus one
  `if (CP_UseOpStack)` on the dispatch.

**Cost on the hot path: one bool test per voxel.** The stack is built per chunk, never per voxel —
the same cadence as the existing param refetch. `ApplyDisturbances` and the diff layer are
deliberately left OUTSIDE the stack and run once for both paths, exactly as before, so the tail of
the pipeline is untouched.

**Defensive choice worth noting:** if `UsesOperatorStackForChunk` ever returns true for an archetype
with no builder, the code clears the flag and falls back to the switch rather than generating an
empty stack. A world that is *unported* is recoverable; a world that is *wrong* is not.

**UNVERIFIED:** not compiled. Likely error spots: the `else switch` form in `GetDensityAt`,
`FVoxelOpStack` as a `thread_local` (it is move-only — move-assign from a temporary is used to
reset it), and the new include in `VoxelGenerator.cpp`.

**What to look at in the editor:** set a Maze strate's `bUseOperatorStack`, regenerate, and compare
against the same seed with it off. **Pass = recognisably the same maze** — same corridor scale, same
connectivity, same feel. That is §2.6's bar, and it is the last thing Phase 1 needs.

**Next single action:** build, then the visual A/B. After that, Phase 2 — the port order in
`OPSTACK-DECOMPOSITION §10.4` starts with FlatPlain + CrystalChamber collapsing into one op.

---

## 2026-07-27 — session handoff written. 39 UE skills installed.

**`OPSTACK-HANDOFF.md`** added at the plugin root: a pasteable resume prompt for a fresh session.
Read order, exact current state, the immediate next action, the hard rules, the open items, and the
method lesson from this session.

**39 Unreal skills installed.** Jahni dropped a UE skills library into `.claude/skills/`, but nested
as `.claude/skills/core/<name>/SKILL.md` — two levels deep, where Claude Code discovers skills one
level deep at `.claude/skills/<name>/SKILL.md`. None were being loaded. Flattened (39 SKILL.md, 124
reference files, all frontmatter valid, folder names already matched `name:`); `core/category.md`
left in place as documentation. Confirmed loading.

⚠️ **They are untracked**: `.gitignore` starts with `*`, and git cannot re-include a file whose parent
directory is excluded, so nothing under `.claude/` can be tracked without un-ignoring the directory
itself. Same shape as AUDIT P1. Flagged to Jahni, not actioned — vendoring a reference library into
the plugin repo is his call.

**Immediately relevant to open work:** `module-and-build-system` documents `PCHUsage` / shared PCH /
IWYU — i.e. the exact mechanism that blocked §C10's settling experiment. That skill was in the repo,
undiscovered, while it was worked out the slow way. Worth reading before clearing the IWYU debt.

**Next single action unchanged:** build Phase 1 step 3, then the visual A/B on a Maze strate.

---

## 2026-07-27 — fresh context: two gaps closed in the step-3 wiring BEFORE the build.

Resumed from `OPSTACK-HANDOFF.md`. Read the step-3 diff against the test path instead of taking
"not compiled" as "nothing to check first" — the symbols all line up (`BuildMazeStack`'s five params
match, `Seed`/`OriginSpineRadius` are generator members, `FVoxelOpContext` comes in transitively via
`VoxelDensityOp.h`), but **the production path and the test path did not agree on two things.**

**1. `PrepareChunk` was never called in production.** The test calls it (`…MazeTest.cpp:133`);
`GetDensityAt` did not. All seven concrete `PrepareChunk` bodies are empty today, so this changes
**nothing** now — which is exactly why it was worth fixing before it could bite. The first op that
hoists real per-chunk work would have been **green in test and silently wrong in game**, and that
class of bug is expensive to find precisely because the test says yes. `GetDensityAt` now builds an
`FVoxelOpContext` (chunk, seed, layout version, strate Z bounds) in the same refetch block and calls
`PrepareChunk` on it. `Step` stays 1 — `GetDensityAt` genuinely does not know the mesher's sampling
step (T2.b contract); noted rather than guessed.

**2. The degenerate-strate early-out had no counterpart.** `GetMazeDensity` opens with
`if (StrateHeight <= 0.0f) return 1.0f;` — air. The stack has no such early-out **by design** (the
test asserts this and refuses to run on a degenerate strate). Unguarded, a zero-height Maze strate
would give **air on one path and whatever the spine/seal ops make of a zero-height band on the
other**. `GetDensityAt` now falls back to the `switch` in that case, so the reference behaviour is
the behaviour. Reachability is not the point — the archetype guard exists, so its port needs one.

**Docs corrected, since both were now actively false:**
- `VoxelDensityOpStack.h`'s banner still said "NOTHING HERE FEEDS THE GAME". It does feed the game
  now, behind the opt-in. Rewritten to say exactly what is wired (`GetDensityAt`) and what is not
  (`ClassifyTile` — still hand-written guards, that is Phase 2), plus the §C10 "never compare the
  two paths" rule at the point of use.
- `CODEMAP §3.2d` had the same stale claim; `§3` gained rows for `UsesOperatorStackForChunk` and
  `bUseOperatorStack`, and the `BuildMazeStack` row now carries the degenerate-strate precondition.

**UNVERIFIED:** still not compiled — that is Jahni's call and it is the immediate next action.
Error spots unchanged, plus one: `FVoxelOpContext` is aggregate-initialised field-by-field, so a
field rename would show up here.

**Next single action unchanged:** build, then the visual A/B on a Maze strate (§2.6's bar —
recognisably the same maze, judged on a screenshot). Then Phase 2, starting with the §3.1 question.

---

## 2026-07-27 — PHASE 1 CLOSED (visual A/B passed). Phase 2 opened: the slab collapse.

**Phase 1's acceptance bar is met.** Jahni built step 3, ticked `bUseOperatorStack` on the Maze
strate and compared: *"it's hard to see with our current maze (which is simple in architecture) but
seems like it's pretty similar, if not entirely similar."* That is §2.6's bar — recognisably the
same maze — and it is worth being precise about how much weight it carries.

**The screenshot is the weakest evidence Phase 1 has, and that is fine, because it was never
carrying the argument.** A simple maze is a poor visual discriminator: "hard to tell apart" is
*exactly* what the measurements already predicted, since **0 of 20 000 samples cross the
isosurface** — no triangle can move. The A/B's job was to catch the class of error the numbers
cannot see (wrong params reaching the stack, wrong strate, wrong wiring), and it did that. The
geometric claim rests on the numbers, and always did.

### Phase 2, first port: FlatPlain + CrystalChamber → ONE op

**§3.1 answered by Jahni: the Z term can go.** So it is gone, and this is the change that makes the
rest worth doing.

**Two separate changes landed together, deliberately, and the test is what keeps them attributable:**

1. **The design change** — `GetSlabDensity`'s floor and ceiling noise lost their Z terms
   (`WorldZ * FF * 0.05f` → `0.0f`; `WorldZ * CF * 0.08f + 3000.0f` → `3000.0f`, keeping the
   decorrelation offset). The world **re-tunes once**: a different slice of the noise field means a
   different floor/ceiling shape. Not a degradation — a different draw.
2. **The refactor** — the now-XY-pure function ported to `FSlabVoidSource` + `FGridColumnMod`,
   plus the three structural ops. Five ops.

`SlabEquivalence` compares the stack against `GetSlabDensity` **as it is now**, so: green ⇒ the port
is a pure refactor ⇒ **any visual delta is attributable to the Z-term removal and nothing else.**
That is why both could go in one build without losing the ability to say which one caused what —
the attribution comes from the test, not from the build order.

**Why this port matters more than its size:** `BuildSlabStack` has **no branch on archetype**,
because `GetSlabDensity` never had one either — CrystalChamber IS FlatPlain with a bigger
`CeilingRoughness`. The test runs the identical battery on both slots, so "two archetypes are one
op" is demonstrated rather than asserted. **8 archetypes → 7.**

**And the perf claim, which is what §3.1 was really about:** `FSlabVoidSource::ClassifyBox` is
**exact and needs no sampling**. `VoxelNoise::FBM` is contractually `[-1,1]`, so both surfaces live
in Z bands with known bounds — a tile below `FloorZ - FloorAmp` is provably solid, a tile strictly
between the bands is provably air. A slab strate is mostly solid rock below its floor, so this
should prove a large fraction of tiles. `ClassifyTile` proves **zero** today. The test prints the
count per archetype; that number is the whole return on the Z term.

`FGridColumnMod` returning `Identity` when no column reaches the box is what lets the source's
`AllAir` survive the fold — otherwise columns would kill every air verdict in the strate.

**UNVERIFIED: none of this is compiled.** Likely error spots, in order:
- `VoxelForgeOpStackSlabTest.cpp` is new — check it is picked up by the module's build.
- The lambda `RunForSlot` captures `World`/`Gen` by reference and calls `AddError`/`TestEqual` on
  the test instance; `TestEqual`'s name argument is built with `*FString::Printf(...)`.
- `FGridColumnMod::GetCells` returns a reference to a `thread_local` — intentional (same pattern as
  `FLatticeCorridorSource::GetCellEdges`), but it is `const` while mutating the thread_local.
- `static constexpr float ColBlend` used inside `FMath::Max`/comparisons — may need a definition
  under older MSVC ODR rules if it is ever odr-used.
- `FSlabGenerationParams` must be complete in `VoxelDensityOpStack.h` (it comes via
  `VoxelStrateTypes.h`, already included).

**Next single action:** build, run `VoxelForge.OpStack.SlabEquivalence`, and **read the two
"proved uniform" numbers** — they are the measured payoff of §3.1. Then tick `bUseOperatorStack` on
a FlatPlain or CrystalChamber strate for the visual A/B. Expect the floor/ceiling shape to have
changed from the Z-term removal; the question is whether it still reads as the same *kind* of place.

---

## 2026-07-27 — SlabEquivalence GREEN. The §3.1 payoff is measured: 36 and 40 of 60 tiles.

```
FlatPlain      box verdicts over 60 tiles: 36 proved uniform, 24 Mixed
CrystalChamber box verdicts over 60 tiles: 40 proved uniform, 20 Mixed
both: 52 of 20000 samples differ, ALL at ULP scale, 0 cross the isosurface
```

**This is the number the whole §3.1 question was about, and it is better than Maze's.** Maze proved
23 of 60; the slab archetypes prove **36 and 40** — 60-67 % of tiles, where `ClassifyTile` proves
**zero** for these two today. The reason is structural rather than lucky: a slab strate is mostly
solid rock below its floor, and now that both surfaces are XY-pure their Z bands are known exactly,
so "this tile is entirely below the floor band" is a comparison rather than a sample.

**The ULP residue is the known C10 floor** — 52/20000, 0 isosurface crossings, worst delta 2⁻¹⁸
(exactly 1 ULP at a density magnitude of ~32). **Not investigated, on purpose.** Same shape as C10:
deterministic, ULP-scale, zero consequence, and six builds were already spent proving that shape is
not worth chasing.

### The output exposed a real weakness in my own test — fixed

Both archetypes reported **the same 52 and the same worst delta**. The explanation is in the
fixture: `FTestWorld::Build` sets only `GeneratorType`, so **FlatPlain and CrystalChamber both get
DEFAULT `FSlabGenerationParams`.** They are the same configuration at two depths.

So the test's claim #2 was overstated. It demonstrated "the slab stack works at two depths", not
"one op serves two archetypes with different defaults" — **`CeilingRoughness`, the one field that
actually distinguishes CrystalChamber, was never varied.** The differing tile counts (36 vs 40)
come from the two slots' Z ranges, not from the archetypes differing.

**A third pass added: `CrystalChamber(tuned)`** — `CeilingRoughness` 6 → 20, plus a rougher floor
and 3× the columns. It varies what actually matters, and it is deliberately the **worst case for
`ClassifyBox`**: a large `CeilingRoughness` widens the ceiling band and makes the
`Max(CeilZ - noise, FloorSurface + 2)` clamp far more likely to bind, which is exactly where a
false verdict — a HOLE — would appear. The default params were too gentle to stress that bound.

**UNVERIFIED:** the third pass. Its box-verdict brute force is the part that matters; expect fewer
tiles proved uniform than the gentle passes (wider bands ⇒ more Mixed), and **zero** unsound
verdicts. If `NumUnsound > 0` here, the `CeilHi` bound in `FSlabVoidSource::ClassifyBox` is the
first suspect, not the noise contract.

**Next single action:** rebuild, confirm the tuned pass is green, then the visual A/B on a FlatPlain
or CrystalChamber strate. The floor/ceiling shape WILL differ from before (the Z-term removal); the
question is whether it still reads as the same kind of place. Then `SurfaceWorld`
(OPSTACK-DECOMPOSITION §5) — biggest payoff, most care.

---

## 2026-07-27 — the tuned pass cried wolf. The TEST was wrong, not the port.

```
CrystalChamber(tuned): 121/20000 differ, 6 exceed the bound, worst |delta| 1.71661377e-05
                       0 cross the isosurface;  box verdicts 32/60 proved, 0 unsound
```

**The port is fine. The yardstick was wrong, and it was wrong in a way that only shows up under
large amplitudes — which is precisely why the tuned pass was worth adding.**

The old bound was `16 · max(|Old|, 1) · FLT_EPSILON` — ULPs measured on the **output density**.
But the density is `min(Z - Floor, Ceil - Z)`, so **near the isosurface the output tends to 0 while
the intermediates (surfaces, world Z, noise amplitudes) are in the HUNDREDS.** A rounding born at
scale ~400 was being judged against a yardstick of scale 1: 400× too tight, and tightest exactly
where the test looks hardest.

**Measured, not assumed:**

| | roughness | worst \|delta\| | in ULP of \|Z\| |
|---|---|---|---|
| gentle passes | ceil 6, floor 4 | 3.81e-06 | ~0.08 |
| tuned pass | ceil 20, floor 9 | 1.72e-05 | **0.345** |

Amplitudes went up ×2.25–3.33, the deltas went up **×4.5**, and the worst one is **sub-ULP at the
scale it is born in**. Error proportional to amplitude is the signature of ordinary rounding, not of
a wrong transcription — a wrong offset or a missing `abs()` would move the surface by **voxels**,
four orders of magnitude above this, not by a factor of four.

**Fixed:** the bound now scales with the magnitude the error is born in (`max(|Old|, |Z|, strate Z
bounds)`), and — more importantly — **the warning now prints the discriminator instead of just the
alarm**: the density at the offending sample and the delta expressed in ULPs of the working scale.
A few ULP with a near-zero density is cancellation; thousands of ULP is drift. The next context
reads that off the message instead of re-deriving it at a build apiece.

The test stays discriminating: real drift is 4 orders of magnitude above the new bound.

### The box verdicts held under the worst case — that is the result that mattered

32 of 60 proved uniform with **0 unsound**, under tripled ceiling roughness and 3× the columns.
That was the specific thing the tuned pass existed to attack (a wide ceiling band makes the
`Max(CeilZ - noise, FloorSurface + 2)` clamp bind, which is where a false verdict would be a HOLE),
and the bound survived it. Fewer tiles proved than the gentle passes (32 vs 36/40), which is correct
— wider bands mean more genuinely Mixed tiles.

### Visual A/B — and a content finding worth more than the A/B

Jahni: *"visually, crystal and plain are identical, same for when opting on or off from the
opstack."*

**Opt-in on/off identical = the port is confirmed a pure refactor.** Note this is the expected
result and my earlier framing was sloppy: **both** paths compute the post-§3.1 function, so the
Z-term change is invisible in the A/B by construction. It is only visible against the world as it
looked *before* this build.

**FlatPlain and CrystalChamber rendering identical is the real finding.** They share
`FSlabGenerationParams` and nothing in the content distinguishes them — **the enum promised a
difference the data never delivered**, in the shipped world exactly as in the test fixture. So the
merge loses no distinction; it reveals there was none. Recorded in `OPSTACK-DECOMPOSITION §3`.
Making a crystal chamber look like one is now a **params** job (`CeilingRoughness` 6 → ~20), which
is precisely the outcome the refactor is for.

**UNVERIFIED:** the corrected bound and the new warning text.

**Next single action:** rebuild; the tuned pass should drop to INFO with all three passes reporting
sub-ULP-of-scale deltas. Then `SurfaceWorld` (§5) — biggest payoff, most care: the T1.a column cache
and the exact-lattice `ClassifyTile` bound must both survive the port.

---

## 2026-07-27 — the acceptance bar changed, and it invalidated the audit's own fix for C1.

**Jahni:** *"I do not need your work to be identical or near identical to what I had before, only
having it 99.99% at worst reproducible if two people share the same seed, since everyone rebuilds it
on multiplayer."*

The bar is **peer agreement in the present**, not fidelity to the past. Recorded as
`OPSTACK-PLAN §2.6.1`, which supersedes §2.6's "recognisably the same place".

**Four consequences, all recorded where they will be found:**

1. **`§C10` closed permanently, not parked.** It measures old-path vs new-path agreement, and the
   two paths never coexist in a shipped world. No requirement depends on it.
2. **The equivalence tests keep their value for a different reason** — they are **port-correctness**
   checks (a transcription slip is a real bug), not fidelity checks. Hard-fail on isosurface
   crossings stays; the ULP grading is now diagnostic only.
3. **`§C9` promoted to the top open risk.** "Two people share a seed" is exactly what `/fp:fast`
   weakens across toolchains, and a **Linux dedicated server** generating collision/nav against
   Windows clients compiles the same density code under *opposite* float models. The fix
   (`FPSemantics = Precise`) is blocked behind the IWYU debt, which now has a real justification
   rather than a tidiness one.
4. **`§C1` is unblocked** — it was deferred *only* because it re-rolls the world's noise.

### ⛔ And then C1's documented fix turned out to be wrong

Before applying the one-liner the audit has carried since it was written, I did its arithmetic:

```
proposed:  SeedF bounded to 16383, multiplier * 97.7 KEPT
           max coord term = 1.6e6  ->  ULP = 0.19  ->  9.5x the ~0.02/voxel step
```

**It bounds the seed but not the offset, and the multiplier is where the magnitude comes from.** The
fix would have made the bug less catastrophic while leaving it live for mid-range seeds — and, worse,
**closed the ticket**. Cost to discover after applying: one build plus a world re-roll.

**The real fix is to delete the multipliers.** The `· 7.3f … · 97.7f` factors exist only to
decorrelate the ~40 noise sites from each other; that is a hashing job, and hashing does it better:
`VoxelHash::SeedOffset(Seed, Site)` returns a **site-salted, bounded, final-units** offset. Bounding
`SeedF` alone would also funnel every seed through 16384 offsets *shared by all sites*, so two
colliding seeds would give identical noise everywhere; per-site salting means two seeds must collide
at **all ~40 sites**, which is never.

**And the op stack has already inherited the bug** — `FSlabVoidSource::SeedF` is a faithful
transcription of `const float SeedF = (float)Seed`. It must be fixed in **both** paths in one change
or the equivalence tests will correctly scream. **Every further archetype port copies it again**,
which is the argument for doing it before `SurfaceWorld` rather than after.

Corrected in `AUDIT §C1` with the arithmetic, so the wrong fix cannot be applied later from the doc.

**UNVERIFIED:** nothing new in code this entry — docs only. The C1 fix itself is NOT written; it
needs Jahni's go-ahead on scope (~40 sites, re-rolls every world, one build).

**Next single action — Jahni's call between two:**
- **`§C1`** (recommended first): ~40 mechanical sites + the op stack, one build, kills the
  highest-severity latent bug before more ports copy it.
- **`SurfaceWorld`** (`OPSTACK-DECOMPOSITION §5`): the biggest op-stack payoff, most care.

---

## 2026-07-27 — C9 FIXED AT THE CAUSE: FPSemantics = Precise, IWYU debt cleared.

**Jahni's call, and the right one:** *"I believe we could put FPSemantics to precise? Clearing up
anything that makes us not use it, then skip on the cross-platform same result for now and continue
on the rework of the opstack."* Fix the cause rather than measure the symptom — if both toolchains
compile IEEE-754 with no contraction, they agree **by construction** and there is nothing left to
compare.

**Verified in the UE 5.7 source rather than assumed** (`E:\Program Files\Epic Games\UE_5.7`):

| | `Default` | `Precise` |
|---|---|---|
| `VCToolChain.cs:1264` (Windows/MSVC) | `/fp:fast` | **`/fp:precise`** |
| `ClangToolChain.cs:712` (Linux/Mac) | `-ffp-contract=off` | **`-ffp-contract=off`** |

So `Default` really did mean **opposite** float models per platform, and `Precise` collapses them
onto the same one. `FPSemantics` is a `ModuleRules` property (`ModuleRules.cs:777`), so per-module
is the right granularity.

### The IWYU debt, cleared

Losing the shared PCH is what the debt was hiding behind. All uses turned out to be pointers,
`TWeakObjectPtr` or `TSubclassOf` parameters, so **forward declarations suffice** — only the
templates and macros needed real includes:

| Header | Added |
|---|---|
| `VoxelBiomeDefinition.h` | `class UMaterialInterface;` |
| `VoxelSettings.h` | `class UMaterialInterface;` |
| `VoxelStrateDefinition.h` | `Templates/SubclassOf.h` + `UMaterialInterface`, `USoundBase`, `AActor` |
| `VoxelStrateTypes.h` | `Templates/SubclassOf.h` + `AActor` |
| `VoxelContentManager.h` | `Templates/SubclassOf.h` + `AActor` |
| `VoxelAtmosphereManager.h` | `class AActor;` |
| `VoxelDensityVolume.h` | **`DrawDebugHelpers.h`** + `class AActor;` |

**`VoxelDensityVolume.h` was the one worth catching.** It uses `ENABLE_DRAW_DEBUG` in an `#if`, and
an **undefined macro in an `#if` is silently 0** — so without the include the debug block would have
vanished without a single warning, rather than failing the build. Include paths verified against the
engine tree (`Engine/Public/DrawDebugHelpers.h`, `CoreUObject/Public/Templates/SubclassOf.h`), not
guessed.

**Expect a residual tail.** The shared PCH hid these for years and only a real build enumerates them
all. Any further "undefined type" error from this build is IWYU, **not** the float setting — the fix
is to add the include, never to revert `FPSemantics`. That instruction is now written into
`VoxelForge.Build.cs` where the next person will hit it.

### Also landed: the cross-platform instrument (kept, not blocking)

`VoxelForge.Determinism.CrossPlatformDigest` — two FNV-1a digests over a fixed integer grid (no RNG,
so the sample set cannot itself diverge):
- **SHAPE** — the sign of density only. This is all the mesher reads, so it *is* the world: same
  shape digest ⇒ same cavities, same walls, same collision. Jahni's "99.99% reproducible", literally.
- **FIELD** — every float bit. Differing while SHAPE matches ⇒ sub-voxel vertex wobble, harmless.

Plus `NearIso`: how many samples sit within 1e-4 of the isosurface, i.e. how many could *possibly*
flip sign under a float-model change. That **bounds** the risk instead of assuming it. Digests are
reported, not asserted, until pinned — pinning before the platforms agree would just carve the
divergence into the test. Per Jahni, the cross-platform comparison is deferred; the test costs
nothing to leave in and becomes a permanent regression guard the day someone runs it on Linux.

**UNVERIFIED:** none of this is compiled. **Expect a perf regression** — `/fp:precise` forbids the
reassociation and FMA contraction `/fp:fast` allowed, on a noise-heavy hot path. Worth measuring
against `ARCHITECTURE §8.10` rather than assuming it is small.

**Next single action:** build. Then, per Jahni, back to the opstack — `SurfaceWorld`
(`OPSTACK-DECOMPOSITION §5`), the biggest payoff and the most care: the T1.a column cache and the
exact-lattice `ClassifyTile` bound must both survive the port. `§C1` (bounded seed offsets) stays
open and still wants doing before too many more archetypes copy it.

---

## 2026-07-27 — the IWYU tail was exactly one site.

`VoxelWorld.cpp:526` — `PC->GetPawn()->GetActorLocation()` needs **`APawn` complete**, and `Casts.h`
only forward-declares it. Added `GameFramework/Pawn.h`, plus `GameFramework/PlayerController.h`
which was only complete *transitively* — the same fragility this whole change exists to remove.

**My scan missed it because I only audited `Public/`.** The shared PCH served `.cpp` files too, and
`APawn` was named in `Build.cs`'s original error list — I read that list and still scanned only the
headers. Everything else in the module compiled, so this is the whole tail: one site, one build.

**UNVERIFIED:** the fix.

**Next single action:** rebuild. Then watch the perf number — `/fp:precise` on a noise-heavy hot
path, checked against `ARCHITECTURE §8.10`. Then back to the opstack: `SurfaceWorld` (§5).

---

## 2026-07-27 — ✅ C10 SOLVED (it WAS /fp:fast). SurfaceWorld opened: height space needed its own family.

**Maze and Slab are now BIT-IDENTICAL to their originals.** `FPSemantics = Precise`, set for
cross-platform play, dissolved the ULP residue.

**So hypothesis 3 had the right mechanism all along, and every experiment built on it was doomed by
construction.** Under `/fp:fast` the compiler reassociates and contracts *by surrounding context*,
with **no single isolable axis** — which is exactly why five carefully-designed one-variable tests
all came back negative while the difference stayed. There was no variable to find. Removing the
*permission* removed the difference.

**The lesson is not the one I expected.** Nobody solved C10; C9 got fixed for an unrelated
requirement and C10 fell out of it. Six more builds of bisecting source would have found nothing,
because the answer was a **build setting nobody was looking at**. Parking a question whose every
consequence is measured and benign was right on its own terms *and* right in hindsight — the
information was not obtainable along the path I was on, at any price. Recorded in `AUDIT §C10`.

**Immediate consequence:** the equivalence tests are now much sharper instruments. Any diff at all is
a real finding rather than noise to grade. The ULP machinery stays as a float-model regression alarm.

### SurfaceWorld, step 1 of 2 — and it forced an architectural decision

`§5` says the height ops *"operate on Z values in the column, not on density"* and then lists them as
children of `FHeightfieldSource`. Writing them made the consequence unavoidable: **they do not fit
`IVoxelDensityOp` at all.** No input Z (they produce one), XY-pure per column rather than per voxel,
and they write neither density nor SDF. The two ways to force them in were a per-voxel third channel
for what is a **column** property, or one opaque op — `§2.5`'s named failure mode.

**So height space got its own contract:** `VoxelHeightOp.h` — `FVoxelHeightSample` (`Height` +
`Relief`), `IVoxelHeightOp`, `FVoxelHeightStack`, and five ops in `VoxelHeightOpStack.cpp`.
`Relief` is the original's `M`: produced by the structural source, consumed by the terrace gate.
Same shape of lesson as `§0.1` — that one found density needed a second *channel*; this found
terrain needs a second *space*.

**A property the type system now gives for free:** a height stack **cannot** hold Z-dependent data,
because there is no Z in the signature to put there. `AUDIT §6.3` warns that Z-dependent data
smuggled into `FSurfaceColumn` silently corrupts every chunk in the vertical stack and that
`ValidateDeterminism` would not catch it. That hazard is now a type error instead of a convention.

**Deliberately staged.** This step touches **nothing** on the density path — no `FHeightfieldSource`
adapter, no `FSkyCapSource`, no `FOverhangShelfMod`, no column cache, no wiring. If height space had
not decomposed cleanly, that would show up here for the price of one test rather than after building
the adapter, the cache integration and the dispatch on top of it. Same method Phase 1 used on
density, applied to the question Phase 2 actually raised.

**The test runs twice, and the second pass is the one that matters:** the F20 terrain ops are **off
by default**, so a defaults-only run would exercise the structural source and leave all four
modifiers — i.e. everything new — untested. The second pass turns them all on. It also brute-forces
`MaxDisplacement` against observed movement, because a false bound would later be a hole.
`WaterLevelRelative` must be set for that pass or `FBeachHeightMod` early-outs and the fifth op is
never touched — a green test that measured nothing.

**Also:** `ComputeSurfaceTerrainZ` moved from private to public on `UVoxelGenerator` (same
justification as `GetSlabDensity` / `GetMazeDensity` — exposed for isolated tests). Its old private
declaration was removed; two declarations of one member would not compile.

**UNVERIFIED:** none of this is compiled. Likely error spots: the new `.cpp`/`.h` pair being picked
up; `FVoxelHeightStack` as a move-only local; the non-owning `const IVoxelHeightOp**` out-param in
`MakeStructuralHeightSource` and its `static_cast` back down in `MakeCliffHeightMod`; `FVector2D`
members being double in UE5 (cast at every use); and the `ComputeSurfaceTerrainZ` access move.

**Next single action:** build + run `SurfaceHeightEquivalence`. If green, step 2 — `FHeightfieldSource`
(the adapter that turns a column into density, preserving the T1.a cache), `FSkyCapSource`,
`FOverhangShelfMod` (the one genuinely 3D op here), then biome blending and the wiring. `§C1`
(bounded seed offsets) still open.

---

## 2026-07-27 — height stack GREEN (bit-identical, both passes). Step 2a + a real bug in my own §3.1 work.

```
SurfaceWorld(defaults):            bit-identical across 20000 samples
SurfaceWorld(all terrain ops on):  bit-identical across 20000 samples
MaxDisplacement: claims 16.300, worst observed 4.441 (27% of claim)
```

**The second op family was the right call** — height space decomposes as cleanly as density did,
including with all four F20 terrain ops on. `MaxDisplacement` is loose (27% used) because it sums
each op's independent maximum and they never peak at the same XY. **Left loose deliberately:** a
loose bound only costs CPU when the heightfield eventually gets a `ClassifyBox`; a tight-but-wrong
one is a hole in the world.

### ⚠️ `FSlabVoidSource::IsXYPure()` was `true`, and that was WRONG

Found while writing `FSurfaceColumnSource` and having to decide the same flag.

The contract is *"**`Eval`** does not depend on Z"*. `FSlabVoidSource::Eval` computes
`min(Z - floor, ceil - Z)` — Z-dependent in the most direct way possible. **§3.1 made the SURFACES
XY-pure; the DENSITY never was and cannot be** — it is a distance to a surface. I conflated the two
while writing the very operator that quotes the warning against doing so.

**Latent only because nothing reads the flag yet** — and step 2b is exactly where it would have gone
live: a generic T1.a column cache keyed on (XY box, StrateKey, Seed) with **no ChunkZ** would have
shared one density value down the entire vertical chunk stack. `AUDIT §6.3` says this corrupts every
chunk silently and that `ValidateDeterminism` would not catch it, because it samples along an X
boundary. Fixed, with the distinction written at the site.

**This is the clearest argument yet for the height-space split:** the thing that is XY-pure is the
HEIGHT, and in `VoxelHeightOp.h` it lives in a type with no Z to get wrong. The bug is unrepresentable
there.

### Step 2a — the bridge into density space

- `FSkyCapHeightSource` — the ceiling is an **altitude**, so it belongs in height space, not in
  density space as `§5` had it. Same category slip as the terrain ops; the subtraction happens later,
  in the combine. It gets tested window-invariance and type-enforced XY purity for free.
- `FSurfaceColumnSource` — consumes both height stacks, produces
  `Density = max(TerrainZ - Z, Z - CeilSurf)`. `IsXYPure() = false`, correctly this time.
- `BuildSurfaceStack` — source + 3 structural. **No per-column memo inside the op, on purpose:**
  T1.a already exists one level up in `GetDensityAt`, and a second cache key is a second thing to get
  wrong in exactly the way described above. Step 2b reuses the existing cache rather than inventing a
  second one.

**What step 2a does NOT cover, and the test now says so at the top:** the **overhang** (found while
reading: `GetSurfaceDensity` passes `OverhangAmp = 0`, so it computes none — the only reference is
the *cached* path, `ComputeSurfaceColumn`) and **biome blending** (weight 0 here; it is the `Mask`
combiner and `§5`'s Phase 3 prototype). **Do not wire SurfaceWorld into a world with biomes or
overhangs until 2b**, because nothing currently in the tests would say it is wrong.

**UNVERIFIED:** step 2a is not compiled. Likely spots: `FVoxelHeightStack` as a member of
`FSurfaceColumnSource` (move-only member ⇒ the enclosing op is move-only too, which is fine since it
lives behind `TUniquePtr`); the new `VoxelHeightOp.h` include in `VoxelDensityOpStack.cpp`; and
`Gen->GetSurfaceDensity` taking two param refs plus a weight.

**Next single action:** build. Then step 2b — the overhang op (per-column gate + per-voxel union,
referenced against the cached path), biome blending as the `Mask` combiner, and the wiring that
reuses `GSurfColCache`. `§C1` still open.

---

## 2026-07-27 — step 2b: the overhang, the one op that could NOT live in height space.

Step 2a came back green on all four counts, including the density-side bridge
(`FSurfaceColumnSource` bit-identical to `GetSurfaceDensity` over 20 000 samples).

**`FOverhangShelfMod` is the boundary case that justifies where the two spaces were split.** Its
uphill reach *grows with altitude* (`Frac = (Z - TerrainZ) / OverhangHeight`), so it is essentially
Z-dependent — it is the only SurfaceWorld op that could not have gone into `VoxelHeightOp.h`. The
line between the two spaces falls where the code changes nature, not where it was convenient.

**The per-column problem, and how it is solved.** The overhang needs `TerrainZ` plus a per-column
gate (`OverhangAmp`, `DirX`, `DirY`) that the source computes. Three ways to get it, two bad:
- recompute the height stack per voxel — correct but pays the cliff's four resamples per lip voxel;
- add a third channel to `FVoxelOpSample` — a per-voxel slot for a **column** property, and
  archetype-specific pollution of a shared contract (`§11` has this open, unresolved);
- **chosen:** the source memoises the column and the overhang reads it, same as `cliff → structural`.

**The memo key is the part worth getting right.** Keyed on `(InstanceId, X, Y)` where `InstanceId`
comes from a monotonic atomic counter — **not** on `this`. A freed stack and a newly allocated one
can share an address; a counter that never goes backwards cannot collide. Since the stack evaluates
every Z of a column at the same XY, the hit rate is ~1, so this also recovers the per-column reuse
without inventing a second cross-chunk cache.

**Two functions exposed** (`ComputeSurfaceColumn`, `SurfaceDensityFromColumn`) because they are the
**only** oracle for the overhang — `GetSurfaceDensity` passes `OverhangAmp = 0` and computes none.
Their private declarations were removed; two declarations of one member will not compile.

**The test's third pass is written to avoid measuring nothing.** A uniform Z draw over the whole
strate would almost never land in the overhang window, so the test would pass green having never run
the op — the same trap as `WaterLevelRelative` in the height pass. Half the samples are now placed
*inside* the window deliberately, the count is reported, and it warns if it is zero.

**Still missing before wiring: BIOME BLENDING.** The ground is evaluated for the dominant biome and
lerped toward the neighbour — the `Mask` combiner, and `§5` calls it the Phase 3 prototype. **Do not
tick `bUseOperatorStack` on a SurfaceWorld strate with biomes until then.**

**UNVERIFIED:** step 2b is not compiled. Likely spots: `std::atomic` include; `HFractal3D` newly
added to the density TU; `MakeUnique<FSurfaceColumnSource>` then `MoveTemp` into the stack while
keeping a raw pointer; and the two newly-public generator methods.

**Next single action:** build. Then biome blending as the `Mask` combiner + the wiring that reuses
`GSurfColCache`. `§C1` still open.

---

## 2026-07-27 — SurfaceWorld WIRED (no biomes yet). And a perf trap caught before it shipped.

All six checks green, **9399 samples deliberately inside the overhang window** — the op was genuinely
exercised rather than skipped.

### ⚠️ The one-entry column memo would have been a disaster, not a slowdown

`FSurfaceColumnSource`'s memo held a single entry. That is correct **only if the caller walks a whole
Z column before changing XY** — and the mesher promises nothing of the sort. If it iterates X first
within a Z slice, *every* voxel misses and the full height stack re-runs per voxel, **including the
cliff's four structural resamples**. On the most expensive archetype in the plugin that is an order
of magnitude, not a few percent.

It would also have been invisible in the tests: they sample random XY, where a one-entry memo and a
256-entry one behave identically. **The tests could not have caught this; only reading the access
pattern could.**

Replaced with a **direct-mapped 256-entry table**, `thread_local`, hashed on the XY bit patterns,
with the **full key compared on hit** — a collision can only cost a recompute, never return the
wrong column. Robust to any iteration order the mesher chooses.

### Wired, with the biome guard in one place

`UsesOperatorStackForChunk` now returns true for `SurfaceWorld` **only when the strate has no
biomes**. The original evaluates the dominant biome and interpolates the *heights* toward the
neighbour across the border band; the stack evaluates one param set. Without the `Mask` combiner a
biome strate would not shift subtly — it would get **a hard seam at every biome border**.

The guard lives in `UsesOperatorStackForChunk`, beside the archetype list, so "can this strate take
the stack?" stays one question asked in one place. `GetDensityAt` carries a second, defensive check
on `CP_BiomeCtx.IsValid()`: if the two ever disagree it falls back to the `switch`, because an
unported world is recoverable and a wrong one is not.

**UNVERIFIED:** not compiled. Likely spots: the `FSlot` struct + `thread_local` array inside a const
method; `return S.C` (the previous `return C` referred to a name now scoped inside the `if`); the
new `SurfaceWorld` case in `GetDensityAt`'s op-stack switch.

**What to try in the editor after the build:** tick `bUseOperatorStack` on a **biome-less**
SurfaceWorld strate and compare. Both paths compute the same function, so this is a wiring check,
not a look change — expect it identical. **A biome strate will silently ignore the flag**, by design.

**Next single action:** build, then the visual A/B. After that the remaining SurfaceWorld work is
the `Mask` combiner (biome blending, §5's Phase 3 prototype) and integrating `GSurfColCache` so the
stack path reuses the existing box cache rather than only its own table. `§C1` still open.

---

## 2026-07-27 — AUDIT §C1 FIXED. 85 noise sites, mechanically, in both paths at once.

SurfaceWorld's wiring came back fine, so I took `§C1` next — **not a detour any more**: the op stack
had already inherited the bug three times, and every remaining port (`VerticalShafts`,
`FloatingIslands`, `TunnelNetwork`) would copy it again. Fixing it now means those get written
correctly instead of needing a follow-up pass.

**The fix, and why the documented one was wrong.** The audit proposed bounding `SeedF` to 16383
while keeping the `· 97.7f` multiplier — which still reaches 1.6e6, where the ULP is 0.19, **9.5× the
per-voxel step**. Less spectacular, still broken, ticket closed. The multiplier is the problem.

`VoxelHash::SeedOffset(Seed, SiteKey)` inverts the roles: **the multiplier no longer decorrelates by
amplifying — it IDENTIFIES the site, and the hash decorrelates.** Output is already in final units,
bounded to [0, 16383], so the ULP is 0.002 = 10 % of a voxel step. Site-salted, so two seeds must
collide at **all ~50 sites** to give the same world, rather than sharing one global bucket.

**Why the 85-site edit was safe to do without compiling:** the transformation is a pure regex —
`SeedF * K.Kf` → `VoxelHash::SeedOffset(SeedU, K.Kf)` — and **the literal stays visible at the call
site**, so every line can still be eye-checked against the original. Applied to all three files in
one pass, so the archetype `switch` and the ported ops changed *identically*; if they had not, the
three equivalence tests would say so loudly. 62 + 7 + 16 sites, 0 left behind, plus 2 bare `+ SeedF`
worm sites handled by hand (site key `1.0f`).

### ⚠️ The new test exists because the equivalence tests are structurally blind here

`VoxelForge.Determinism.LargeSeedSurvives` — seeds 1337, 1e5, 1e7, 2e9; asserts the heightfield still
produces ≥ 50 distinct heights over 400 samples.

**The equivalence tests could never have caught C1.** They compare the op stack against the archetype
switch, and both read the *same* faulty expression — so at a large seed both collapse **identically**:
bit-identical, green, and both perfectly flat. An oracle that shares the implementation's bug cannot
see the bug. This test compares nothing to nothing; it asserts a **property** — the terrain must vary.
That distinction is worth keeping in mind for the ports still to come.

The fixture's own comment said the small seed was a *workaround* for C1; corrected, since the reason
is now just message comparability.

**UNVERIFIED:** not compiled. Likely spots: a missed `SeedU` declaration in one of the 9 functions
(the awk sweep found none, but it is a heuristic); `VoxelCaveMorphology.h` newly included in
`VoxelHeightOpStack.cpp`; and the `uint32`/`float` swap on two op members.

**⚠️ EXPECT EVERY WORLD TO LOOK DIFFERENT.** This re-rolls every noise offset in the plugin. That is
the intended consequence and it is covered by §2.6.1 — nothing depends on the old shapes any more.
The three equivalence tests should stay green (both paths changed together); the visual is new.

**Next single action:** build, run the full `VoxelForge` filter — six tests now. Then back to the
op stack: the `Mask` combiner (biome blending) to finish SurfaceWorld, or `VerticalShafts` next.

---

## 2026-07-27 — all 9 tests green. C1 PROVEN fixed. And C9 is only HALF fixed.

Full `VoxelForge` filter, exported run `Saved/Automation/Automation2026.07.27-16.35.18.csv`:

```
ClassifyTileSoundness · CrossPlatformDigest · DensityPurity · DiffLayerContention
LargeSeedSurvives · LiveEditInvalidation · BoxVerdictFold · MazeEquivalence
SlabEquivalence · SurfaceHeightEquivalence          — all Success
```

**`§C1` is proven fixed, by property rather than by comparison:**

```
Seed 1337        : 400 distinct heights / 400 samples, range 20.95 voxels
Seed 100 000     : 399 / 400, range 23.46
Seed 10 000 000  : 399 / 400, range 22.89
Seed 2 000 000 000 : 400 / 400, range 23.42
```

Seed 2e9 — what `FMath::Rand()` produces — now generates a live world. Before the fix that seed
gave a *constant* field. Note the three equivalence tests stayed green through an 85-site rewrite,
which is exactly the port-correctness value they were kept for.

### ⚠️ The digest warning was right for the WRONG reason — and I nearly dismissed it

`NearIso: 2 of 115000 samples within 1e-4 of the isosurface` fired a warning whose text blamed the
`/fp:fast`-vs-precise split. That split is fixed, so my first instinct was "stale warning, soften the
text". **I checked instead of assuming, and the risk is real via a different mechanism.**

**`sinf`/`cosf` are not specified by IEEE-754.** `FPSemantics = Precise` makes MSVC and Clang agree on
*expression evaluation*; it says nothing about the math library. MSVC's CRT and glibc's libm may
legitimately differ by ~1 ULP — and `FMath::Sin`/`Cos` are all over the density path: layer lines
(`VoxelGenerator.cpp:2275`, `VoxelHeightOpStack.cpp:232`), ribs, room placement, rotations.

**So `§C9` is half closed:** the compiler half by construction, the **library half still open**. No
build flag can fix the second — two libm implementations cannot be made to agree by a compiler
setting. The fix, if it is ever needed, is a deterministic in-house `sin`/`cos` in the density path
(one more world re-tune).

**The measurement was also over-stating things ~100×.** A single 1e-4 band is far too wide for a
libm-scale delta (~1e-6 absolute on densities of magnitude ~10). Replaced with a three-band profile
(1e-4 / 1e-5 / 1e-6); only the tight band raises a warning, because only it corresponds to a delta
that could actually flip a sign. Recorded in `AUDIT §C9`.

**UNVERIFIED:** the reworded digest test.

**Next single action:** build (quick — one test file changed). Then, per Jahni: finish SurfaceWorld
(the `Mask` combiner = biome blending, `§5`'s Phase 3 prototype), then `VerticalShafts` (`§6`).

---

## 2026-07-27 — the `Mask` combiner (biome blending) in height space. §5's Phase 3 prototype.

All 10 tests green on the previous build, and the reworded digest confirms the point that mattered:

```
NearIso profile over 115000 samples: 2 within 1e-4, 0 within 1e-5, 0 within 1e-6
```

**Zero in the tight band** — no sampled voxel sits close enough to the isosurface for a libm
difference to flip its side. The residual `§C9` library half is real in principle and, on this grid,
carries no measured risk. The wide band's "2" was the ~100× over-statement, exactly as predicted.

### The design decision worth recording: `IVoxelBiomeField`

Biome blending needs to ask "which biome is at this XY?", and the real answer is a warped Voronoi
with a per-chunk cache living on `UVoxelGenerator`. **The op must not hold a `UVoxelGenerator*`** —
Phase 3 wants operators to become *assets*, and an op that owns a generator pointer never can.

So the op depends on `IVoxelBiomeField`, a two-line interface returning `(dominant, neighbour,
weight)`. The adapter that knows the generator stays on the generator's side. This is the same move
as `cliff → structural source`: depend on the *capability*, not on the owner.

### The combiner itself

`FBiomeBlendHeightSource` — **one complete height stack per biome**, heights lerped in the border
band. Each biome's stack computes its **own** relief `M` and gates its **own** terrace with it,
which is exactly what the original does (two independent full `ComputeSurfaceTerrainZ` calls, only
the OUTPUTS blended). Blending *heights* rather than *params* is what keeps borders continuous
across any param difference — interpolating params would drag a terrace through intermediate states
that mean nothing.

**The ceiling SELECTS instead of blending**, because the original takes the dominant biome's ceiling
alone. Reproduced as-is rather than "improved": a blended sky cap would change the world's
silhouette, and a port is not where that gets decided.

### Tested against a synthetic field, on purpose

The real Voronoi resolver has its own coverage; a synthetic field lets the weight sweep 0 → 1
**continuously**, which is where an inverted lerp (`1-w` for `w`) hides — it looks right in the
middle and wrong only at the ends. Two deliberately dissimilar biomes, five weights × 400 points,
bit-exact against `FMath::Lerp` of the two full stacks. Plus a check that the ceiling still returns
the dominant's at weight 1.0, which is the case a "blend everything" refactor would silently break.

**UNVERIFIED:** not compiled. Likely spots: the local `class FFixedWeightField` declared inside a
function body and used as an interface; `TArray<FVoxelHeightStack>` (move-only element type — needs
`MoveTemp` on insert, which it has).

**Next single action:** build. Then the density-side integration — `FSurfaceColumnSource` takes the
per-biome params + field, blends the overhang amp (`Lerp(Amp(PD), Amp(PN), W)` with slope from PD),
and the generator-side adapter wraps `ResolveBiomeSampleAt`. That drops the biome guard in
`UsesOperatorStackForChunk` and finishes SurfaceWorld. Then `VerticalShafts` (§6), which reuses
`FConstantRockSource` + `FSdfRoughnessMod` + `FSdfCarve` and should be the cheapest port yet.

---
