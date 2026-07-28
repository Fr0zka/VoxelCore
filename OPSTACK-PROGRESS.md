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

## 2026-07-27 — SurfaceWorld COMPLETE (biomes included). 5 of 8 archetypes ported.

All 10 tests green again. **But the two new biome checks printed nothing** — because I wrote them to
report only on failure. **That is the exact flaw I flagged twice in this session** (the
`WaterLevelRelative` early-out, the overhang window count) and then committed myself: a silent pass
is indistinguishable from a check that never ran. Both now `AddInfo` their coverage, so the next run
shows the blend actually executed over 2000 (weight, point) pairs.

### Step 2c — the density side, and SurfaceWorld is closed

- **`FSurfaceColumnSource` takes per-biome params + an owned `IVoxelBiomeField`.** Empty params ⇒
  the original path, bit-for-bit unchanged (which is why the existing tests should stay green).
- **The field is OWNED by the stack, not borrowed.** The real adapter points at `GetDensityAt`'s
  `thread_local` `CP_BiomeCtx` / `CP_BiomeCache`; the stack is itself `thread_local` and rebuilt in
  the *same* refetch block, so all three are born and die together on one thread. Making ownership
  structural beats leaving survival to a convention the next reader has to infer.
- **The overhang amp now blends across biomes** — `Lerp(Amp(PD), Amp(PN), W)` with the slope and
  threshold from the **dominant** only, exactly as `ComputeSurfaceColumn` does. Interpolating the
  *slope* would be meaningless: it is a measurement of the terrain, not a setting.
- **`FGeneratorBiomeField` lives in `VoxelGenerator.cpp`,** on the side that knows the generator.
  That is the whole point of the interface — the op sees a *capability*, never an owner, which is
  what lets it become an asset in Phase 3.
- **The biome guard is gone** from `UsesOperatorStackForChunk`.

Also cleaned up while there: the two constructors now **delegate to one body with one id counter**
instead of each initialising separately (two init paths is two places to forget a member — the first
draft already had two competing counters, one of them tagged with a high bit to avoid collision,
which is a smell rather than a design).

**5 of 8 archetypes ported:** Maze · FlatPlain · CrystalChamber · SurfaceWorld.

**UNVERIFIED:** not compiled. Likely spots: the delegating constructor; `TUniquePtr<IVoxelBiomeField>`
as a defaulted parameter in the public header; `VoxelHeightOp.h` newly included by
`VoxelDensityOpStack.h` (a public→public include); the `case` block now needing braces for its local
declarations; and `FGeneratorBiomeField` being defined before `UVoxelGenerator`'s member functions
while calling `ResolveBiomeSampleAt`.

**What to try after the build:** tick `bUseOperatorStack` on a SurfaceWorld strate **with biomes**
now. Both paths compute the same function, so expect it identical — biome borders included, which is
the case that was guarded off until now.

**Next single action:** build. Then `VerticalShafts` (§6) — it reuses `FConstantRockSource`,
`FSdfRoughnessMod` and `FSdfCarve` unchanged from Maze, so it should be the cheapest of the eight.

---

## 2026-07-27 — SurfaceWorld verified visually. And Jahni measured the thing I had only flagged.

All 10 green, biome checks now visible and correct (blend bit-exact across the whole weight sweep;
ceiling selects rather than blends at weight 1.0). Jahni: *"the opstack looks similar if not
identical to the old terrain"* — the biome case included, which is what step 2c added.

**And: *"it took a bit more time generating with the opstack."*** That is a real regression, it was
predictable, and I had flagged it as "pending" rather than fixed. Diagnosed, two compounding causes:

**1. The memo key invalidated on every chunk.** It was keyed on `InstanceId`, which changes on every
stack rebuild — i.e. every chunk. `GSurfColCache`, the path it replaced, is keyed on
`(XY box, StrateKey, Seed, LayoutVersion)` with **no ChunkZ**, deliberately *"shared down the whole
vertical strate stack"*. So a 4-chunk-tall strate recomputed **every column four times**, cliff
resamples included — and the cliff costs four extra structural samples per column.

**2. The table could not hold one chunk.** A chunk is `CHUNK_SIZE²` = 1024 columns; the table had
**256** entries. It thrashed against itself *within a single tile*, before any cross-chunk question.

**Fixed:** `PrepareChunk` now derives a **shared** `ColumnKey` from `(StrateBottomWorldZ,
LayoutVersion, Seed)` — the same identity `GSurfColCache` uses — and the table is 4096 entries
(~150 KB/worker, in line with `GSurfColCache`'s 6 × 59 KB). The memo is `thread_local`, so it
already survived stack rebuilds; **only the key was throwing the contents away.**

**Why sharing across chunk Z is sound:** heights are XY-pure *by type* (the whole point of
`VoxelHeightOp.h` — there is no Z in the signature), and the biome field is documented
Z-independent (*"ZERO Z dependence: the climate/Voronoi fields are pure-XY"*). That is precisely the
justification `GSurfColCache` already rests on.

**Un-prepared safety:** `ColumnKey` starts at `InstanceId` rather than 0, because slots initialise to
`Key = 0` and a zero key would falsely hit the pristine slot at (0,0). Without `PrepareChunk` you
get per-instance caching — less sharing, still correct. Degrade, never lie.

**⚠️ This may not close the gap entirely, and I am not claiming it does.** Virtual dispatch (5 ops
per voxel) and the hashed lookup versus the original's direct-indexed box both remain. Those are
smaller than a 4× column recompute, but "smaller" is a guess until measured. **The next generation
timing is the measurement** — if it is still slower, the remaining suspects in order are: the box
cache's direct indexing vs. my hash, then per-voxel virtual calls.

**UNVERIFIED:** not compiled.

**Next single action:** build, regenerate, and compare generation time against the switch path with
the flag off. Then `VerticalShafts` (§6).

---

## 2026-07-27 — VerticalShafts ported. 5 of 8, and operator reuse is now MEASURED.

**The port that tests the thesis rather than the fidelity.** Every previous port asked "does the
decomposition reproduce the original?". This one asks **"do operators actually get reused across
archetypes?"** — which is `OPSTACK-PLAN §2.5`'s claim and the only reason to do this refactor rather
than tidy the `switch`.

**Three of the five ops are Maze's, reused without a line changed:** `ConstantRock`, `SdfRoughness`,
`SdfCarve`. In the `switch`, `GetMazeDensity` and `GetVerticalShaftDensity` are two ~100-line
functions with nothing visibly in common. As operators they are the **same three ops with a
different source and different tuning** — frequency 0.1 instead of 0.12, window `rough + 4` instead
of `R + rough + 2`. If the test is bit-identical, reuse stops being an intention and becomes a
measurement.

**Two new ops:** `FShaftFieldSource` (infinite cylinders + hash-gated connectors → SDF) and
`FShaftLedgeMod` (banded shelves on the +X/+Y half only, so the shaft stays climbable).

**Deviation from `§6`, stated:** it suggested splitting the source in two (XY-pure cylinders +
connectors) so the cylinder half could get an exact XY box verdict. Kept as one op, because the
connectors derive from the *same* 3×3 roll as the shafts and the ledge mod needs the shaft list
anyway — splitting would mean rolling twice or sharing a cache between two ops. What is forfeited is
the exact verdict on the cylinder half alone; what is kept is a conservative `EffectOverBox` that
tests circles *and* connector reach. Revisit if the profile says it matters.

**One subtlety worth flagging:** `FShaftLedgeMod` gates on `InOut.Sdf < 0` — the SDF **after**
roughness, as the pile left it. Re-deriving the SDF there would give the pre-roughness value and
shift every ledge. The op reads the channel rather than recomputing, which is exactly what the
two-channel `FVoxelOpSample` is for.

**Compile fix on the way in:** `FCells` was declared at the bottom of the class but returned by
functions above it. Member *bodies* are deferred; *return types* are not — C4430 plus an unreadable
cascade from a trivial cause. Moved up beside `FShaft`/`FConn`, with a note.

**Ported: Maze · FlatPlain · CrystalChamber · SurfaceWorld (biomes incl.) · VerticalShafts — 5 of 8.**
Remaining: `FloatingIslands` (§7), `Underwater` (§8, TunnelNetwork + a flag), and `TunnelNetwork`
(§2) **last**, because it owns `BuildChunkCache`'s two-region window-invariance discipline (§8.4),
the most delicate code in the plugin.

**UNVERIFIED:** the shaft port is not compiled past the `FCells` fix.

**Next single action:** build, run the `VoxelForge` filter — 11 tests now, the new one is
`VoxelForge.OpStack.VerticalShaftEquivalence`. Then `FloatingIslands`. **Perf is deliberately parked
until the transition is complete** (Jahni's call); the open item is that the op path is slower, with
virtual dispatch and the hashed column lookup as the remaining suspects.

---

## 2026-07-27 — VerticalShafts bit-identical. And the perf fix had introduced a REAL bug.

**`VerticalShaftEquivalence`: bit-identical over 20 000 samples, 966 inside a shaft.** So the
cylinders, connectors, roughness, carve and ledges all ran, and **operator reuse across archetypes is
now measured rather than intended** — three of the five ops are Maze's, unchanged.

### ⚠️ `SurfaceHeightEquivalence` FAILED, and it was my own perf fix

```
Overhang: 69 of 20000 samples differ (largest |delta| 5.84); 1 crosses the isosurface
```

**Cause: the `ColumnKey` I introduced in `f3faa3b` did not include the params.** It was
`hash(StrateBottomWorldZ, LayoutVersion, Seed)`. The test builds two stacks in the **same** strate,
same layout version, same seed, differing only in overhang settings — identical keys, so the second
stack read the first's cached columns, which had been computed with `OverhangAmp = 0`. The overhang
silently vanished wherever a column was already cached.

**This is not a test artefact.** It is precisely the weakness the codebase already documents for
`GSurfColCache` (the extended `AUDIT §C2` note in `VoxelGenerator.cpp`): *"StrateKey is
round(StrateBottomWorldZ), so a live edit that changes terrain params WITHOUT moving the strate
leaves the key unchanged and serves stale columns."* Production merely **masks** it, because
`RebuildStrates` bumps `LayoutVersion`. My key inherited the same hole, and the test found it in one
build.

**Fixed** by folding a `FCrc::MemCrc32` fingerprint of the params (plus every per-biome param set)
into the key. `FSurfaceGenerationParams` is verified pure POD — no `TArray`, `FString` or pointer —
so a memory CRC **cannot** produce a false hit; at worst padding causes a false *miss*, i.e. a
recompute. Erring toward CPU cost rather than toward a wrong column.

**Worth stating plainly: a perf optimisation introduced a correctness bug, and the test suite caught
it the same day.** That is the clearest answer yet to "what are all these tests for" — the failure
was invisible to inspection, produced *plausible* terrain, and only one of 20 000 samples actually
crossed the isosurface.

### Known, not fixed: VerticalShafts proves 0 of 60 tiles

Maze proves 23, the slabs 36-40, shafts **zero**. Expected from the choice recorded last commit: my
`EffectOverBox` returns `CarveOnly` if *any* shaft exists within a `Spacing*1.6` halo, rather than
testing the actual connector capsules. At default `ShaftDensity` almost every box has a shaft in that
halo, so `AllSolid` is always killed and nothing is provable. **Correct but pessimistic** — a false
verdict would be a hole, this is only lost CPU. The improvement is to test real connector capsules
instead of "a shaft exists nearby", which is `§6`'s split argument arriving through the back door.

**UNVERIFIED:** the fingerprint fix.

**Next single action:** build, confirm `SurfaceHeightEquivalence` is green again (all 11 tests).
Then `FloatingIslands` (§7) → `Underwater` (§8) → `TunnelNetwork` (§2, **last**). Perf still parked.

---

## 2026-07-27 — END OF DAY 2. All 11 tests green. 5 of 8 archetypes ported.

The params fingerprint fixed the overhang regression; the full `VoxelForge` filter is green.

**Ported and wired, all bit-identical to their originals:** Maze · FlatPlain · CrystalChamber ·
SurfaceWorld (biomes included) · VerticalShafts.

**Remaining ports:** `FloatingIslands` (§7) → `Underwater` (§8) → `TunnelNetwork` (§2, **last**).

**Open items, none blocking:**
- **Perf** — op path is slower; parked by Jahni until the transition is complete. One cause fixed.
- **`§C9` library half** — `sinf`/`cosf` not IEEE-754; 0 samples measured at risk. Run the digest on
  Linux and pin it when the platforms agree.
- **VerticalShafts proves 0 of 60 tiles** — pessimistic `EffectOverBox`, not a hole.
- **`ClassifyTile` still hand-written** — `ClassifyBox` is verified but unconsumed. This is where the
  measured tile-skipping becomes actual frames, and it is arguably the biggest win left.

`OPSTACK-HANDOFF.md` rewritten for a fresh context: current state, what is left in order, the two
things Phase 2 invented (height space, `IVoxelBiomeField`), and the method lessons that were paid for
in build cycles.

---

## 2026-07-28 — FloatingIslands ported. 6 of 8. The stack runs BACKWARDS, and §C1 was NOT closed.

**The portage that tests the AXIS, not the fidelity.** `VerticalShafts` measured reuse *by identity*
— three of Maze's ops, not a line changed. This one measures something stronger and riskier for the
abstraction: **reuse by INVERSION**.

The four archetypes ported so far all start from ROC and CARVE. FloatingIslands starts from the VOID
and FILLS. If the abstract axis chosen back in §0.1 — the *sign* of the internal density — is the
right one, then both ends of the pile must be the same operators negated:

```
FConstantFieldSource(+Base)   ←→   FConstantFieldSource(-Base)      ClassifyBox: AllSolid ←→ AllAir
FSdfConvertOp(Sign = -1)      ←→   FSdfConvertOp(Sign = +1)         carve         ←→ fill
FSdfRoughnessMod              ←→   FSdfRoughnessMod                 4ᵉ archétype, inchangé
```

And it holds: **the only new operator in this port is the island blob.** 7 ops total.

Two classes were merged rather than duplicated (`FConstantRockSource` → `FConstantFieldSource`,
`FSdfCarveOp` → `FSdfConvertOp`), each with two factories so the *authoring* vocabulary keeps saying
"rock"/"void" and "carve"/"fill". Multiplying by ±1 is exact in IEEE-754, so the three green ports
are bit-for-bit untouched — that claim is load-bearing and the next run tests it.

### `ClassifyBox` can say **AllAir** for the first time

No cave archetype has ever proved "all air"; `FConstantVoidSource` can, trivially and exactly, and a
floating-island strate is *by construction* mostly empty. That is `OPSTACK-DECOMPOSITION §7`'s claim,
and the test counts AllSolid and AllAir **separately** — an aggregate "N proved" would have hidden
precisely the number that matters. If AllAir comes back 0, the stack is still sound and the perf
argument simply did not fire (the VerticalShafts situation); the test says so out loud rather than
looking green.

### ⚠️ The bound is ONE-SIDED, and assuming otherwise would have been a hole

`Sdf ≥ WorldZ − TopSurf` bounds an island from above. **There is no bound from below:** under an
island the SDF degenerates to ≈ `DistXY`, so a hairline thread of matter hangs down the axis to the
strate floor. Rejecting a box for sitting below an island would be a hole in the original's own
geometry. Only the top rejects.

Second trap, caught by doing the arithmetic rather than eyeballing it: the domain warp displaces X
and Y with **two independent** noise samples, so the point moves along the diagonal — the pad needs
`WarpAmp·√2`, not `WarpAmp`. A 1× pad is wrong by 41 % exactly where both noises saturate together:
rare, plausible-looking, and effectively unreachable by random testing.

### ⚠️⚠️ `AUDIT §C1` was reported closed on 2026-07-27. It was one site short.

Found by reading `GetFloatingIslandDensity` line by line to port it:

```cpp
const float WX = WorldX + FractalNoise3D(FVector(WorldX * 0.04f + (float)S * 0.0007f, ...));
```

**The sweep matched `SeedF * K`; this site spells it `(float)S * K`.** A textual sweep finds a
spelling, not a bug. And the property test could not compensate — `LargeSeedSurvives` asserts the
*heightfield* still varies, while this site perturbs an *island outline*: at seed 2e9 the term hits
~1.4e6, ULP 0.125 against a 0.04 voxel step, so the warp flattens and every island snaps back to a
perfect circle. Cosmetic, not catastrophic, which is exactly why nothing screamed for two days.

Fixed in **both** paths in one pass so `FloatingIslandEquivalence` stays a valid oracle. Recorded in
`AUDIT §C1` with the sharp edge that came with it: `SeedOffset` quantises its site key by ×100, so
`0.0007f` lands on site **0** — unique today (every other key is ≥ 0.19), silently collidable
tomorrow.

**This is the third time this session that a confident premise failed a check.** C1's *documented*
fix was wrong; "C9 is gone after FPSemantics" was wrong; now "C1 is closed, 0 left behind" was wrong.
The pattern is stable enough to plan around: **a claim about the code is evidence about whatever was
actually examined, and nothing else.**

### Deviation from `§7`, stated

`§7` sketched a `FRAME IslandWarp` wrapping the source. The warp stays **inside** the op. Frames are
worth building at the *second* real user, and two of the three (TunnelNetwork's cave warp, its tunnel
warp) are not ported. Designing an abstraction against one example is what this refactor has avoided
throughout — `IVoxelBiomeField` exists because a second, concrete need appeared. Revisit at
TunnelNetwork.

Also carried over from the shaft port: the 3×3 memo key includes `BoundarySealThickness`, **which the
original omits** although `SpreadZ` reads it. Same family as `§C2` and as the overhang regression of
2026-07-27. Adding a field to a cache key can only cost a recompute; leaving one out costs a wrong
world, invisibly.

**Ported: Maze · FlatPlain · CrystalChamber · SurfaceWorld (biomes incl.) · VerticalShafts ·
FloatingIslands — 6 of 8.** The two remaining are really one: `Underwater` *is* TunnelNetwork plus
`WaterLevelRelative` (§8), so the `switch` loses both cases in a single port.

**UNVERIFIED:** nothing here is compiled. Likely spots, in order: the two class renames
(`FConstantRockSource` / `FSdfCarveOp` no longer exist — every reference should go through a factory,
but a missed one is a clean C2065); `FFloatingIslandParams` reaching `VoxelDensityOpStack.cpp`
(it comes via `VoxelStrateTypes.h`, already included, so this should be free); the nested `FCells`
declared before its returning functions (the `FShaftFieldSource` C4430 trap, avoided deliberately);
and `MakeUnique<FIslandBlobSource>` being called from the factory namespace, which is fine only
because the class sits above the end-of-anonymous-namespace line.

**Next single action:** build, run the `VoxelForge` filter — **12 tests** now, the new one is
`VoxelForge.OpStack.FloatingIslandEquivalence`. Watch three numbers in its output: samples inside
island rock (0 ⇒ the equivalence proved only that two voids agree), the AllAir verdict count (0 ⇒ the
perf argument did not fire), and of course the diff count.

**⚠️ EXPECT ISLAND SILHOUETTES TO CHANGE** wherever the seed is large — that is the C1 fix, it is
intended, and §2.6.1 covers it.

Then `Underwater` + `TunnelNetwork` (§8 / §2, **last**, with §8.4's window-invariance discipline).
Perf still parked by Jahni until the transition is complete.

---

## 2026-07-28 — 12 tests green. TunnelNetwork STAGE A (of three) written.

Jahni: *"everything's green."* FloatingIslands is bit-identical and wired; 6 of 8 ported.
**Worth asking for explicitly next run:** the three coverage numbers that test prints — samples
inside island rock, the AllAir verdict count, and the diff count. Green with zero samples inside
island rock would mean the equivalence proved that two empty voids agree.

### The last archetype, staged deliberately

`GetDensityWithParams` is ~1080 lines, 13 detail modifiers, a two-region cache and a per-room op
override. Porting all of it before anything can be verified would be ~600 unverified lines on top of
~200 unverified ones — the `AUDIT §P3` pattern this refactor has dodged six times.

**The way in:** every detail modifier is amplitude-gated, and `FStrateGenerationParams` already
leaves all of them at **zero** by default (`BuildParamsFromDefinition` stopped merging them globally
— they arrive as per-room ops). One exception, `SurfaceRoughness = 5`. So zeroing those amplitudes
sends the ORIGINAL down exactly the path stage A ported, and stage A is verifiable **today**, bit for
bit, against the real function. Same discipline as the height-stack test's "defaults, then all ops
ON", taken in the other direction.

- **Stage A (this commit):** vertical scale · base rock · cave warp · room graph (+ pits + chimneys)
  · carve · worms · structural post. 6 ops.
- **Stage B:** the 13 detail modifiers, gated on `Sdf < SDFBlendRadius·3`.
- **Stage C:** the per-room op override (`§2`'s option (a)), the `Underwater` water flag, and only
  then does `UsesOperatorStackForChunk` return true for either.

### ⚠️ The one decision that matters: the op CALLS `BuildChunkCache`, it does not transcribe it

Every other port is a literal transcription. This one must not be. `BuildChunkCache` carries the
two-region window-invariance discipline (`ARCHITECTURE §8.4`) and is the most delicate code in the
plugin; transcribing it would **fork** it — two copies of one invariant, drifting, with the copy
"validated" by a test that compares it to the original. What *is* transcribed is the ~60 lines of
glue around it (strate-index memo, search-box key, warp, pit/chimney loops).

### `FRAME` ops are retired, and porting is what retired them

`§2` described two nested frames (`VerticalScale`, `CaveWarp`) and `§7` a third (the island warp).
Porting all three dissolved all three:

- **`CaveWarp` wraps exactly ONE operator.** Pits and chimneys explicitly read *unwarped* coords.
  A transform whose scope is one op is a local variable, not a frame.
- **`VerticalScale` is `Z / Scale`** — a pure function of a scalar and a param, one line wherever it
  is needed. A frame would add a channel to avoid a division.
- **The island warp** was already kept local for the same reason.

**Zero frames out of three candidates.** It was never missing infrastructure; it was the same thing
seen three times from a distance. Recorded in the builder rather than left as a permanent TODO.

Also found: TunnelNetwork's carve divides by `Max(SDFBlendRadius·2, 1)` where Maze/Shafts/Islands
divide by `Blend·2`. The formulas diverge once `Blend·2 < 1`, so `FSdfConvertOp` gained an explicit
`MinDivisor` instead of letting the two look interchangeable. `Max(x, 0) == x` for positive Blend, so
the green ports are untouched.

### ⚠️ A test that compares against the original would have measured the ORIGINAL'S bug

Check 3 (two stacks, different params, evaluated A/B/A/B at the same point) was written comparing
both against `GetDensityWithParams`. **It would have failed** — and not because of the port. The
original's SDF cache key is `(XY box, strate, seed)` with **no params**, so under interleaving it
serves B the rooms it built for A. Comparing to it there measures its staleness, not my operator.
Rewritten to compare each stack against **itself evaluated alone**: an oracle that does not share the
defect under test.

That in turn raises a **suspicion, filed as a suspicion**: `GetGenerationParams` blends params across
Gradient transitions, so two chunk Zs inside one strate can hold different params with the same XY
box, strate index and seed ⇒ no rebuild ⇒ the lower chunk gets the upper chunk's rooms. In
production, with no live edit needed. Recorded in `AUDIT §C2` **with the check that would confirm
it**, because the whole chain rests on "Gradient blending actually varies within a strate", which I
have not verified. The port does not inherit it — the params fingerprint forces the rebuild.

### What stage A deliberately does NOT prove

Box verdicts: **zero proved, and the test asserts zero.** `FRoomGraphSource` answers `Both` (its
bounds are in the SDF cache, which it would have to build for the queried box — worth doing only
once `ClassifyTile` actually consumes `ClassifyBox`), and `FWormFieldSource` answers `CarveOnly`
*everywhere*, because a fielded-noise carve has no spatial bound. That is `§0.2`'s point, and the
amplitude cap that fixes it (`t ∈ [0,1]`, `Mask ∈ [0,1]` ⇒ at most `WormStrength` toward air) now has
a home in `FWormFieldSource::MaxCarveAmplitude()`, waiting for a fold that carries numbers.

**UNVERIFIED:** not compiled. Likely spots: `FStrateTerrainOpEntry` / `UVoxelStrateDefinition` newly
reachable from `VoxelDensityOpStack.cpp` (added the include); `MakeSdfCarve`'s new defaulted third
parameter (declared in the header, so the three existing call sites still compile); `FCrc` needing
`Misc/Crc.h` (it comes via `CoreMinimal.h`, and `FSurfaceColumnSource` already uses it in this file);
and the test's `GetGenerationParams` signature.

**Next single action:** build, run the filter — **13 tests**, the new one is
`VoxelForge.OpStack.TunnelNetworkSpineEquivalence`. Then stage B (the 13 modifiers).

---

## 2026-07-28 — Stage A is bit-identical. And its own counters say the test was thin.

```
TunnelNetwork STAGE A: bit-identical across 6000 samples in 24 chunks
                       (65 in open cave, 5636 in rock)
Params-fingerprint   : 3 of 400 probe points genuinely differ, 0 served wrong
Box verdicts         : 0 proved, 40 Mixed          ← asserted, expected
Op count             : expected 6, got 7           ← FAIL
```

**The port is correct.** Vertical scale, cave warp, the shared `BuildChunkCache`, pits and chimneys
at unwarped coords, the floored carve divisor and the worm network mask all reproduce the original
bit for bit. The only failure was **my arithmetic in the assertion**: 4 ops + 3 structural = 7, and I
wrote 6. Fixed.

### The two numbers that matter are the ones that passed

**65 of 6000 samples in open cave — 1.1 %.** Green, and mostly comparing solid rock to solid rock:
the carve, the pits, the chimneys and the worm carve only run near the network. At `RoomSpacing = 80`
/ `RoomDensity = 0.35` a 32-voxel chunk usually contains no cave at all.

**3 of 400 probe points differ between the two param sets — 0.75 %.** So the stale-cache check asked
its question three times and answered "not stale" 397 times about points that could never have
revealed staleness.

Both counters exist precisely to say this, and both said it. **Both guards, however, only fired at
ZERO** — so the run was green with the coverage of a much smaller test. That is the same lesson as
"a test that prints nothing on success is indistinguishable from one that never ran", one notch
finer: *a coverage guard that only trips at zero does not measure coverage, it only notices absence.*
Both are now **fraction thresholds** (≥ 10 % cave, ≥ 5 % differing probes) and both print a
percentage.

Fixes applied: `RoomSpacing 80 → 42`, `RoomDensity 0.35 → 0.85`.

### New check 3b — did the pits and chimneys actually fire?

`§2` calls those two loops the fiddliest thing in the whole decomposition (unwarped coords
`SmoothMin`'d into the warped room SDF). Setting `PitDensity = 0.55` proves nothing: `SDFCache.Pits`
can still come back empty and the test stays green. So the new check builds the same stack with
`PitDensity = ChimneyDensity = 0` and counts the points that MOVE. Zero ⇒ those loops contributed
nothing and the equivalence says nothing about them, whatever colour the test is.

Same move as the FloatingIslands "samples inside island rock" counter, applied to a sub-feature
instead of a whole archetype: **enabling a feature is not evidence it fired.**

**UNVERIFIED:** the strengthened test is not compiled. Only the test file changed; the operators are
untouched and already proven bit-identical.

**Next single action:** re-run the filter (13 tests). Expect stage A green with a cave fraction in
the tens of percent, a fingerprint check in the double digits, and a non-zero pit/chimney count.
Then stage B — the 13 detail modifiers.

---

## 2026-07-28 — the coverage fixes landed, and check 3b caught a real gap on its first run.

```
Cave coverage        : 21.0 %      (was 1.1 %)   ✅
Params-fingerprint   : 383/400 = 95.8 % differ   (was 0.75 %)   ✅
Equivalence          : bit-identical, 6000 samples, 1259 in open cave   ✅
Box verdicts         : 0 proved, 40 Mixed        (asserted)
Pit/chimney          : 0 of 1500 points move     ❌ ← the new check, earning its keep immediately
```

### Pits and chimneys never ran, and the params said they would

`BuildChunkCache` bakes them inside `if (!CR.RoomOp) continue;`, then reads **`OpParams`** — a
*fresh* `FStrateGenerationParams` with only that room's terrain op applied. So
`FStrateGenerationParams::PitDensity`, which the test set to 0.55, **is never read by the bake at
all**. Pits, chimneys and columns exist *only* through a `UVoxelTerrainOpDefinition` rolled per room,
and the fixture's definitions have an empty `TerrainOperations`. Zero pits, every run.

**Not a product bug, and worth stating so I don't "fix" it later:** those fields carry no `UPROPERTY`
on `FStrateGenerationParams`. It is a transport struct written by `ApplyTo`, not a settings surface —
nothing in the editor offers Jahni a `PitDensity` that quietly does nothing. My reading was wrong,
not the code.

**Consequence for stage C:** the per-room op override is not an optional refinement. It is the *only*
path by which pits, chimneys and columns exist at all.

### The fix took three attempts, and the second one was the instructive failure

1. ~~Zero `PitDensity` on the params~~ — the fields nothing reads.
2. ~~Build a second stack with an empty op pool and diff the densities~~ — **would have silently
   lied.** The op pool is *not* in the SDF cache key (in production `LayoutVersion` covers pool
   edits, which is why the original gets away with it), so two stacks differing only by the pool
   would share the same `thread_local` cache and produce identical output — reported as "pits
   contribute nothing", for the second time, for a third reason.
3. **Ask the bake what it baked.** The suspected cause was an empty `SDFCache.Pits`; that is a
   structure the test can simply *look at*. `BuildChunkCache` over six search boxes, then assert
   rooms > 0, pits > 0, chimneys > 0. No cache key involved, no ordering to respect, and it fails
   for exactly one reason.

The test now attaches a real two-entry op pool (Pit 0.5 / Chimney 0.5, so every room draws one).
**Safe at stage A on a non-obvious ground:** the per-room override applies the op to a copy of the
params that drives the 13 unported detail modifiers — but `ApplyTo(Pit)` writes only the four pit
fields, so no modifier wakes. A `Terrace` op there *would* break stage A, which is precisely what
stage B will add. There is also a guard that errors out if the bake ever produces **columns**, since
stage A has not ported `STEP 4d`.

### And a message that lied for a whole run

The green equivalence line said *"Exercised: … pits and chimneys at UNWARPED coords"*. It was false —
zero pits existed. A success message that asserts coverage instead of reporting it is the same
failure as a guard that only trips at zero: it reads as evidence while measuring nothing. Reworded to
point at the bake-coverage number rather than claim the coverage.

**UNVERIFIED:** test file only; operators untouched and already proven bit-identical.

**Next single action:** re-run. Expect non-zero pits and chimneys, zero columns, and the equivalence
still bit-identical — *that* run is the one where stage A is genuinely covered. Then stage B.

---

## 2026-07-28 — STAGE A GENUINELY COVERED. 13 tests green. Handoff rewritten.

```
TunnelNetworkSpineEquivalence ......................... Success
  Equivalence   : bit-identical, 6000 samples, 1431 in open cave
  Cave coverage : 23.9 %                     (floor 10 %)
  Bake coverage : 49 rooms, 56 pits, 28 chimneys, 0 columns
  Fingerprint   : 383/400 = 95.8 % differ, 0 served wrong
  Box verdicts  : 0 proved, 40 Mixed         (asserted — §0.2's worm bound)
```

**This is the run where stage A means something.** The previous green one did not: it compared
mostly solid rock to solid rock, its stale-cache check asked its question three times out of 400, and
its pit/chimney loops ran on an empty list while the success message claimed otherwise. Same code,
same colour, three different strengths of evidence — which is the whole argument for printing
coverage numbers instead of pass/fail.

56 pits and 28 chimneys now exist in the bake, so the two loops `§2` calls the fiddliest thing in the
decomposition (unwarped coords `SmoothMin`'d into the warped room SDF) were exercised by 6000
bit-identical samples. 0 columns confirms `STEP 4d` stayed dormant, as stage A requires.

`OPSTACK-HANDOFF.md` rewritten for a fresh context: the three-stage TunnelNetwork plan and why stage
A is verifiable while incomplete, the `FRoomGraphSource` calls-not-transcribes rule, `FRAME` ops
recorded as retired, and the method lessons regrouped — notably the three coverage traps and
"enabling a feature is not evidence it fired".

**Next single action:** stage B — the 13 detail modifiers of `STEP 4b–4h`, inserted between the carve
and the worms. The test's op-count assertion moves, and its `EnableTunnelFeatures` starts turning the
amplitudes back ON one group at a time (a `Terrace` op in the room pool will be the first thing to
break stage A's equivalence, by design).

---

## 2026-07-28 — **8 OF 8.** TunnelNetwork + Underwater ported. Unattended run, 8 commits, zero builds.

Jahni started this session and left. Rule #1 held: nothing was compiled, nothing was launched,
nothing was pushed. **Everything below is written and committed but UNVERIFIED.** He reverts by sha,
which is why the one-commit-per-group rule was the one thing not bent.

```
6e29cbf  B1  surface roughness (4b) + VoxelNoise::Cellular3D shared out of VoxelGenerator.cpp
ab1a996  B2  Terrace · LayerLines · Ribbing (4c) + FRoomGraphSource::FState hoisted out of Eval
6ec6009  B3  cave Overhang · Cliff · Scallop · Arch (4c)
b063d43  B4  Columns (4d) · Domes (4g) · Pinch (4h) · FloorBias + Column op in the test pool
8a303cc  B5  the gate itself — test only, proves a voxel outside it is bit-identically untouched
9591088  C1  per-room op override — eleven detail ops read FRoomGraphSource::LocalParams()
e479fcd  C2  Underwater wired to the same builder + its own equivalence check
(this)   C3  UsesOperatorStackForChunk returns true for both — 8 of 8 — + CODEMAP / PLAN / PROGRESS
```

**Test filter: `VoxelForge`. Still 13 tests** — no test file was added; the TunnelNetwork test grew
five new checks. `VoxelForge.OpStack.TunnelNetworkSpineEquivalence` is the one to read.

### What the archetype switch looks like now

`ConstantRock → RoomGraph(warp + pits + chimneys) → SdfCarve → Roughness → Terrace → LayerLines →
Ribbing → Overhang → Cliff → Scallop → Arch → RoomColumn → Dome → Pinch → FloorBias → Worms →
[spine → seal → passage]` — **19 ops, one builder, two archetypes.** Every one of the eight
archetypes now has an operator-stack twin, per-strate opt-in, each equivalence-tested bit for bit
against its original function.

⚠️ `UsesOperatorStackForChunk` returning true for TunnelNetwork/Underwater **changes nothing by
itself** — it still requires `bUseOperatorStack` ticked on a strate asset, which is Jahni's call and
was deliberately not done. What *has* changed: the flag is no longer a no-op anywhere. Ticking it on
any strate now really switches that strate onto the operator stack.

### Five things found by reading the original, ported as-is, and worth knowing

Each of these looked like a bug or an oversight. None was fixed, because with no test feedback a
"cleanup" is an unfalsifiable guess — and three of them are load-bearing for the look of the world.

1. **There are TWELVE detail modifiers, not thirteen** — and only **ELEVEN** read the per-room param
   copy. The `const FStrateGenerationParams& Params = LocalTerrainParams;` shadow is declared *inside*
   the `if (bNearCaveSurface)` block, which begins **after** STEP 4b, so **surface roughness reads
   strate params**. Both numbers were carried wrong in the handoff and both matter for C1.
2. **The cliff modifier's comment describes code that does not exist.** It promises "sample density at
   Z±1 and compute the vertical gradient"; the code samples nothing and uses a Perlin with 3× Z
   frequency as a proxy it calls `VertGrad`. Multiplying by `CaveSDF` still gives it the right sign
   either side of the surface, which is why it produces steeper faces at all.
3. **Terrace's two SDF gradient probes query unwarped X/Y and raw Z**, while the field they probe was
   evaluated at warped coordinates and effective Z — and they exclude pits and chimneys. The probe
   does not sample the field whose slope it measures.
4. **`ColumnDensity` on `FStrateGenerationParams` is read by nothing.** Neither the bake (which reads
   a fresh `OpParams` with only the room's op applied) nor the per-voxel loop. Columns exist *only*
   through a `Column` terrain-op asset in the strate's pool. Same shape as the `PitDensity` finding,
   and the reason B4's coverage had to be an assertion about the bake rather than a params probe.
5. **The per-voxel ~74-field param copy** of the override is transcribed as-is. Real perf item.

### Two decisions this run made that are not reversible by taste

**B5 — the gate is a repeated early-out per operator, not a scoping container.** The stack is a flat
list that `ClassifyBox` folds op by op; a container would have to re-implement `VF_FoldOp` and would
hide its children from the fold, and an op that only exists inside a container cannot become a Phase-3
asset. The cost is stated rather than hidden: the original tests once and skips twelve, the stack
tests twelve times. Predictable branches, but measure before optimising — that is the §C10 lesson.

**C1 — one op owns the shared state, eleven read it.** `DECOMPOSITION §2` called the per-room override
"the piece with no clean home" and proposed a per-modifier scoping predicate. The difficulty came
entirely from assuming each modifier must *own* its params. `FRoomGraphSource::LocalParams()` publishes
the strate params with the nearest room's op applied, memoised once per voxel (invalidated at the top
of every `Eval`, before any early-out), and the modifiers read it. **No scoping predicate was
invented** — this is the third use of a pattern already in the file (`FOverhangShelfMod` ←
`FSurfaceColumnSource`, `FShaftLedgeMod` ← `FShaftFieldSource`). Same shape as the pit/chimney
resolution: the problem was in the framing, not the code.

### ⚠️ One thing C1 broke that nothing consumes yet — fix before `ClassifyTile` does

`EffectOverBox` still answers from **strate** params, because a box spans many rooms and a per-voxel
copy has no meaning there. But `ApplyTo` writes the op's value even where the strate's was `0`, so a
room op can **enable a modifier the strate had switched off**. On a strate with a terrain-op pool, a
box verdict can therefore be too optimistic. Harmless today — nothing consumes `ClassifyBox` in
production — and **it must be fixed before `ClassifyTile` starts to.** Noted at
`FLayerLineMod::EffectOverBox`, which is where a reader would land.

### The coverage discipline, extended — and why the checks grew faster than the code

Every group added an operator *and* a way to be told it did nothing. The test now reports:

- **group coverage** (check 1b): rebuild the stack with one group off, count moved samples. **Zero is
  an error, not a warning.** Legitimate as a stack-diff here — unlike for the op pool — because these
  are params, and the params CRC *is* in the SDF cache key.
- **the noise switch** (1c): all four roughness noise types × warp on/off. The main equivalence only
  ever takes the FBM branch; three of four cases were otherwise untested.
- **the gate** (1d): outside-gate samples must be bit-identical to a modifier-free stack, *and* the
  inside-gate move count must be non-zero — otherwise "nothing leaked" only means "nothing happened".
- **the bake** (3b): rooms, pits, chimneys and now **columns**, which is the only sound way to cover an
  operator that has no parameter. The stage-A guard that errored on `TotalColumns > 0` is inverted.
- **the override** (3c): how many baked rooms drew a `Terrace` op, and how many samples land inside
  one of those rooms. Both non-zero, or check 1's green means only that two paths agree where the
  override never applies — which is what stage B already proved.
- **Underwater** (5): 2000 samples in the Underwater *slot*, which is a different strate index, hence
  a different bake seed and a different entry in the strate-index memo — something six chunks of slot
  0 cannot exercise.

That is the pit lesson applied a fourth and fifth time: **enabling a feature is not evidence it
fired, and the check that proves it must be able to fail for exactly one reason.**

### What breaks first, per group, if a group is wrong

- **B1** — diffs concentrated in open cave within `|SDF| < SurfaceRoughness·2`. If instead only check
  1c fails, exactly one branch of the noise switch is implicated and nothing else.
- **B2** — diffs near horizontal surfaces (terrace) or in thin Z bands (lines/ribs). If the *whole*
  equivalence collapses everywhere, suspect the `FState` hoist, not the three new ops.
- **B3** — a band `|SDF| < range`, or inside open cave for the arch.
- **B4** — dome/pinch near room centres and perimeters; a column error is a ring of diffs at fixed XY
  through a whole room height.
- **C1** — diffs confined to the subset of rooms that drew `Terrace`, with terrace-shaped Z banding,
  while check 3c still reports non-zero coverage.
- **C2** — if the TunnelNetwork equivalence is green and only this fails, **the operators are fine**
  and the finding is a real density difference between Underwater and TunnelNetwork. That would
  contradict `DECOMPOSITION §8` and is worth more written down than patched.

### Likely compile-error spots, in order

1. `VoxelNoise::Cellular3D` in `VoxelCaveMorphology.h` — it reopens `namespace VoxelNoise` in a header
   that does not include `VoxelNoise.h`. Legal, but it is the newest structural change.
2. `#include "VoxelTerrainOpDefinition.h"` newly needed in `VoxelDensityOpStack.cpp` for `ApplyTo`
   (added). `FCachedRoom::RoomOp` was only forward-declared before.
3. The five modifier constructors that gained a `const FRoomGraphSource*` parameter — a missed call
   site is a clean argument-count error at `BuildTunnelNetworkStack`.
4. `FRoomGraphSource::FState` is declared *after* `PrepareChunk` and used inside `Eval` further down;
   in-class member functions see the whole class, so this is fine, but it is the `C4430` shape that
   bit the shaft port.
5. The test's `EVoxelTerrainOpType::Terrace` / `Column` and `FTestWorld::SlotUnderwater` — all exist,
   all newly referenced from this file.

### Next single action

Build, run the `VoxelForge` filter, and read three numbers before anything else: the **group-coverage
percentages** (1b), the **Terrace-room sample count** (3c), and the **inside-gate move count** (1d).
A green run with a zero in any of those is a green run that proved much less than it looks.

Then, in the order agreed: settle `AUDIT §C2`'s suspected item (answerable by reading
`GetGenerationParams` and `FStrateGenerationParams::Lerp` — no build needed), the worm amplitude cap
(`DECOMPOSITION §0.2`, its own commit, extends `BoxVerdictFold`), and then make `ClassifyTile` consume
`ClassifyBox`. Perf stays parked until Jahni unparks it.

---

## 2026-07-28 — after 8 of 8: the fold carries numbers, and `ClassifyTile` finally consumes `ClassifyBox`

Same unattended run, continued past the transition. Four more commits, still zero builds.

```
64d0e11  AUDIT §C2's suspected item settled — CONFIRMED, and it is the default config (docs only)
353d504  the box fold carries NUMBERS — amplitude bounds alongside EVoxelOpEffect  (§0.2)
239c037  the twelve detail modifiers inherit the room source's box verdict
6a8390f  ClassifyTile consumes ClassifyBox for cave archetypes — the T1.d prize
```

**Test filter: `VoxelForge`. Now 14 tests** — the new one is
`VoxelForge.OpStack.ClassifyTileSoundness`.

### `AUDIT §C2` — the suspicion was right, and it needed no build to settle

Four links, each read in the source rather than inferred: `Alpha` depends on chunk Z *inside* a
slot; `Lerp` blends every room-placement field; `Gradient` + `TransitionBlendChunks = 2` are the
**defaults**; and the strate index is per-slot, so the cache key cannot notice. A worker that builds
`(X,Y,Z1)` then `(X,Y,Z2)` in one strate evaluates the second chunk against the first chunk's rooms.

The part that matters more than the seam: **the result depends on which chunk that worker happened
to build first** — a window-invariance break, and in multiplayer two peers can generate different
geometry from the same seed. Not fixed here: the fix is a params CRC in the *original* path's SDF
cache key, a live-generation change that wants a build in front of it.

**Why nothing caught it:** the fixture sets `TransitionType = Hard` on every strate on purpose. The
one configuration the tests never build is the default one.

### The numeric fold, and the thing `§0.2` does not say out loud

`§0.2` frames the worm as *the* blocker: a fielded carve has no spatial bound, so it answers
`CarveOnly` on every box and kills `AllSolid` everywhere. True — and the amplitude fix is exactly as
described (`t ∈ [0,1]`, `Mask ∈ [0,1]` ⇒ at most `WormStrength`). But porting it surfaced a second
half that was missing:

1. **The subtraction had no first term.** Nothing declared *how solid* the rock was. Hence
   `ForcedMarginOverBox`, which `FConstantFieldSource` answers exactly (`|Value|`).
2. **The twelve detail modifiers were the bigger drag, and not because of their amplitudes.** They
   are all gated on the SDF the room source writes, so where the source proves no cave reaches the
   box they are **Identity**, not "bounded" — yet each declared `Both`/`FillOnly`/`CarveOnly` and
   killed the hypothesis just as hard. They already held the pointer since C1; they simply were not
   asking. `VF_NoCaveOverBox` fixes twelve declarations in one place.

Backwards compatibility is structural, not promised: `FLT_MAX` and `0` are the defaults, so an op
that overrides nothing subtracts `FLT_MAX` from a margin of `0` and dies exactly as before.

### `ClassifyTile` — the prize, and the only change on this run that could make a hole

Where it used to `return Mixed` without a call for cave archetypes, it now builds the strate's stack
and folds `ClassifyBox`. **SurfaceWorld and bedrock gaps keep their hand-written proofs** — an
exact-lattice column test beats any box bound, so the stack has nothing to offer there.

**The load-bearing decision:** `GetDensityAt`'s build switch was extracted into
`VF_BuildOpStackForChunk` and both callers now use it. A second copy would be the worst bug
available in this file — a tile skipped on the verdict of a stack that is not the one producing its
density *is* a hole. A "keep these in sync" comment would not have been enough.

Six guards, all failing to `Mixed`. The one that matters is **params bit-identical across every
chunk coord the box touches**, and it exists *because of* the `§C2` finding two commits earlier: a
tile straddling a blend band genuinely cannot be represented by one stack. The others: opt-in true on
every chunk (not just the triggering one), one cave slot per tile, no mixed cave/surface/gap tile, a
27-chunk-coord cap, and the disturbances folded by hand since `§10.2` leaves them outside the stack.

**A safety property worth recording, because it was checked rather than assumed:** no box query in
the whole operator library touches per-worker memo state — `EffectOverBox` and `ClassifyBox` are
pure in every op. So the classifier building and folding its own stack cannot clobber the caches the
density path depends on.

**What it buys:** Maze, FlatPlain/CrystalChamber, VerticalShafts and FloatingIslands can prove tiles
in production. TunnelNetwork and Underwater still prove nothing — their chain dies at
`FRoomGraphSource`, which answers `Both`. Making it answer spatially means building the SDF cache for
the queried box, which is *now worth it*: a skipped tile saves 30k+ density evaluations, and both
the amplitude fold and the modifiers' Identity inheritance are already in place to receive it.
**That is the single highest-value thing left.**

### Fixture: a cross-test hazard that was hidden by an accident

`FTestWorld::Build` gains `bUseOperatorStack` (default false), and every test world now gets a
**process-unique `LayoutVersion`**. `PassagesVersion` is per-instance and starts at 0, so two
`FTestWorld`s both reported `1` — and `GetDensityAt`'s per-chunk caches are keyed on
`(ChunkCoord, LayoutVersion)`. One world could be served the previous world's params *and its
`CP_UseOpStack` flag*. Invisible for as long as every world agreed the flag was false; the first
world that ticks it removes the coincidence, in both directions.

### The reviewer pass, done as the last step

Re-read every ported operator against the original block, as a reviewer rather than the author.
**No transcription error found.** What the pass did confirm, and what is worth knowing:

- operator order in `BuildTunnelNetworkStack` matches the original **line for line** (roughness →
  terrace → lines → ribs → overhang → cliff → scallop → arch → columns → domes → pinch → floor bias);
- `EffectiveZ` is recomputed per operator instead of once — same expression, same operands, so
  bit-identical;
- the terrace's `SDFBlendRadius` comes from strate params in the port and from the *shadowed* copy in
  the original, which are equal because no `ApplyTo` writes that field;
- `LocalParams()` is materialised lazily by the first modifier that asks, which reproduces the
  original's cost profile (one struct copy per voxel **inside the gate**, none in deep rock).

### Next single action

Unchanged: build, run the `VoxelForge` filter, read the coverage numbers before the colours. Then
the two numbers new to this batch — `VoxelForge.OpStack.ClassifyTileSoundness`'s **count of tiles
actually brute-forced** (zero would mean it proved nothing about the new wiring, and it errors on
that), and the verdict counts compared against `VoxelForge.Determinism.ClassifyTileSoundness`, whose
difference *is* the T1.d gain.

After that: make `FRoomGraphSource::EffectOverBox` answer spatially. Everything else is now waiting
on it, and the two commits above were built to receive it.
