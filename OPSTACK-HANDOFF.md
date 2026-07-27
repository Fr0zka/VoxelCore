# Handoff — VoxelForge operator stack, 2026-07-27

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
3. **`OPSTACK-PLAN.md`** — the plan. §2.5 op taxonomy, §2.6 acceptance bar, §4 phases, §5 invariants.
4. **`OPSTACK-DECOMPOSITION.md`** — all 8 archetypes broken into ops, every param traced. §10.4 is
   the port order, §11 the open questions.
5. **`AUDIT-2026-07.md` §C10** — read before touching the Maze equivalence test. Six builds were
   spent there; it is parked deliberately.
6. **`CODEMAP.md`** — navigation. Trust symbol names over line numbers.

## Where things stand

**Phase 0.5 — DONE, green.** Five automation tests, all passing (Session Frontend → Automation,
filter `VoxelForge`): `DensityPurity`, `ClassifyTileSoundness`, `DiffLayerContention`,
`LiveEditInvalidation`, `OpStack.BoxVerdictFold`, `OpStack.MazeEquivalence`. They found and fixed a
real bug (AUDIT C2 — five caches missing `LayoutVersion`).

**Phase 1 — DONE and verified.** `Maze` decomposes into **seven** ops with no contortion:
`ConstantRock → LatticeCorridor → SdfRoughness → SdfCarve → Spine → Seal → Passage`. Measured:
SDF reproduced **bit for bit**, **zero isosurface crossings** out of 20 000, window-invariant across
worker threads, every box verdict survives brute force, and **23 of 60 tiles proved uniform** where
`ClassifyTile` proves zero for any cave archetype. That last number is the perf case for the whole
refactor, measured rather than argued.

**Phase 1 step 3 — WRITTEN, NOT COMPILED.** The stack is wired into `GetDensityAt` behind
`UVoxelStrateDefinition::bUseOperatorStack`, with the ported-archetype list living only in
`UVoxelStrateManager::UsesOperatorStackForChunk()`. **This is the immediate next thing to build.**

### The immediate next action

1. I build. Likely error spots: the `else switch` form in `GetDensityAt`, `FVoxelOpStack` as a
   `thread_local` (it is move-only, reset by move-assigning a temporary), the new include.
2. Then the **visual A/B**, which is the last thing Phase 1 needs: tick `bUseOperatorStack` on a Maze
   strate, regenerate, compare against the same seed with it off. **Pass = recognisably the same
   maze** (same corridor scale, connectivity, feel). That is `OPSTACK-PLAN §2.6`'s bar — judged on a
   screenshot, not a diff.
3. Then **Phase 2**, starting with the cheapest win: `FlatPlain` + `CrystalChamber` collapse into one
   op (`OPSTACK-DECOMPOSITION §3`). Ask me §3.1 first — whether the slab floor noise's Z term is
   intentional; if it can go, both archetypes become XY-pure and start skipping tiles.

## Hard rules that prevent real bugs

- **Density sign:** negative = solid, positive = air *at the mesher*. Inside the op stack the
  convention is INTERNAL (**positive = solid**), negated once by the caller. The SDF channel uses
  standard SDF convention, so `min()` means opposite things on the two channels. This is the #1
  source of confusion in the plugin — `VoxelDensityOp.h` explains it.
- **Never run both density paths in one world**, and never compare them for equality (§C10).
- Every cache key includes `LayoutVersion`; `ProcessQueue` stays `EQueueMode::Mpsc`; `Epoch` carries
  through every async path; don't "optimize" the `ARCHITECTURE §8.10` perf invariants.
- Never edit `Binaries/`, `Intermediate/`, `*.generated.h`. Comments are French + English — match the
  surrounding file.
- Commit per coherent unit with a real message. Never push. `main` is the known-good fallback.
- Update `CODEMAP §3` for new/renamed symbols, `ARCHITECTURE §8` for design changes, tick phases in
  `OPSTACK-PLAN.md`, and append to `OPSTACK-PROGRESS.md`.

## ⚠️ The lesson from this session — it cost six builds

Chasing a 1-ULP difference in the Maze port, I proposed **five** hypotheses and every one was refuted
by measurement: the `FVector` round-trip, a transcription slip, `/fp:fast` across translation units,
inlining context, and compile-time-constant vs runtime parameters. Each was plausible. Each cost a
build. What actually produced information was instrumenting — a bisect, a three-way comparison, a
variable-isolation harness.

Worse, twice I reasoned confidently from a premise I hadn't checked (a build flag that had landed in
the wrong module; a comparison that was circular by construction and could not fail).

**So: on any numeric discrepancy, instrument before hypothesising, and verify the premise before
reasoning from it.** And know when to stop — Jahni called it, correctly, once the port was proven
correct on every axis that affects the game and only the last rounding was unexplained.

## New this session: 39 Unreal skills are installed

`.claude/skills/<name>/SKILL.md` — they were nested one level too deep and invisible; now flattened
and loading. Directly relevant: `automation-and-testing`, `coding-standards`,
`module-and-build-system` (covers `PCHUsage` / shared PCH / IWYU — the exact thing that blocked
§C10's settling experiment), `game-thread-performance`, `memory-and-gc`, `navigating-engine-source`.
**Use them** — the one that would have explained the shared-PCH failure was sitting in the repo,
undiscovered, while I worked it out the slow way.

## Known open items, deliberately not done

- **AUDIT C1** — unbounded `SeedF`, one line at six sites. Dormant at small seeds; applying it
  re-rolls the world's noise and forces a re-tune, so it wants its own build. **Still a real bug.**
- **IWYU debt** — seven headers use engine types they never include; the plugin only compiles because
  of the shared PCH. Blocks `FPSemantics` experiments. Worth clearing on its own terms.
- **AUDIT C10** — the parked ULP residue. Read it before reopening; five hypotheses are already
  refuted in a table there.
- `OPSTACK-DECOMPOSITION §11` — five design questions for me, most importantly whether the two-channel
  `Eval` (already implemented) stays.
