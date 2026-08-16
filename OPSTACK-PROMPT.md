# Kickoff prompt — density operator stack refactor

*Paste the block below into a fresh context. Everything above the line is for Jahni, not the new session.*

**Branch:** already created and checked out — `experimental` (from `69fa73e tmp`). `main` is untouched.

**Why this exists:** the refactor is too big for one context. This prompt makes any fresh session able to
start, or resume at a phase boundary, without re-deriving the design.

**Working rhythm it enforces:** one large batch of code → stop → *"ready to build"* + likely compile-error
spots → Jahni builds → he pastes errors/screenshots → fix → next batch. That is deliberate; see §Autonomy.

---

```
You're picking up an agreed refactor of the VoxelForge UE5 voxel plugin, on branch `experimental`
(already checked out — do not create another). I'm Jahni. This was designed with a previous context
and written down so you don't have to re-derive it.

GOAL
Replace the hardcoded archetype `switch` in `UVoxelGenerator::GetDensityAt` with a composable
density OPERATOR STACK, so new world ideas become data-authoring instead of C++. Long-term ambition:
"a world generator of any kind, of any possibility — any combination of ideas could be happening."

READ FIRST, IN THIS ORDER (do not skip, do not skim §2.5)
1. CLAUDE.md            — project rules. Rule #1 is absolute.
2. OPSTACK-PLAN.md      — THE PLAN. §0 the summary, §2 why step 1 is small, **§2.5 the op taxonomy**,
                          §2.6 the acceptance bar, §4 the phases, §5 the invariants, §7 non-goals.
3. AUDIT-2026-07.md     — §6 (the 3D hazards) and §5 (priority list). §1 has real open bugs.
4. CODEMAP.md           — navigation. Trust symbol names over line numbers; the lines are stale.
5. ARCHITECTURE.md §8.10 — the perf invariants. Read before touching any hot path.

THE THREE WAYS YOU WILL FAIL — internalise these before writing code

(1) BUILDING. Never run a build, compile, or the editor. Jahni builds everything himself; he has the
    editor open and running it yourself just burns cost and tells you nothing. When a batch of code is
    done: STOP, say "ready to build", and list the likely compile-error spots. Then WAIT. Same for
    in-editor verification — ask for a screenshot, don't try to produce one.

(2) WRAPPING INSTEAD OF DECOMPOSING. Read OPSTACK-PLAN §2.5. Jahni already had a "room operations"
    system — ops that perturb density near an existing surface. His words: "there's a world of
    difference between a grotto strate and an open world strata." If you turn each old density
    function into one monolithic op, you have rebuilt the switch with extra steps and wasted the
    effort. Archetypes must DECOMPOSE into: FIELD SOURCES (role 1 — what makes a grotto a grotto),
    COMBINERS (role 2 — how sources merge; this is what makes ideas compose), DETAIL MODIFIERS
    (role 3 — his old ops, now one role of four), STRUCTURAL POST (role 4 — spine/seal/passage/diff,
    always appended in that order, never author-omittable).
    The test: can you author "open-world surface strate whose mountains contain a room-graph cave
    system, with floating islands in the upper void" with NO C++? If no, you built the wrong thing.

(3) BREAKING AN INVARIANT. OPSTACK-PLAN §5. In particular: window invariance (§8.4 — every op is a
    pure function of world coords + seed); the T1.a column cache is keyed (XY box, StrateKey, Seed)
    with NO ChunkZ and is shared down the whole vertical stack, so XY-pure data only; every cache key
    must include LayoutVersion (see AUDIT C2 — three existing caches get this wrong today);
    ProcessQueue stays EQueueMode::Mpsc; Epoch carries through every async path.

ACCEPTANCE BAR — read OPSTACK-PLAN §2.6 carefully, two properties get confused
- `ValidateDeterminism` = 0 delta: REQUIRED ALWAYS. Same point, different cache window/thread,
  identical float. This is self-consistency, not reproduction.
- Byte-identical to the OLD system's output: NOT required. Jahni: "not a 1/1 replica, but a possible
  very close result to what I have right now, else it won't really matter much." Recognisably the same
  kind of place at a fixed seed, judged on a screenshot. A one-time param re-tune is expected and fine.
  This relaxation is WHY you are allowed to decompose properly instead of wrapping.

WHAT TO DO — first batch (Jahni has standing permission for multiple changes per build)

A. Phase 0.5 from the plan — the three automation tests. There are currently ZERO tests in this
   plugin and nothing machine-checks the many "bit-identical" claims in the docs.
   1. Density purity: sample ~10k points, shuffle query order, re-sample, assert bit-equality. Run it
      across MULTIPLE worker threads (the existing `ValidateDeterminism` button is game-thread only and
      would miss worker-cache divergence — that's how AUDIT C2 hid).
   2. ClassifyTile soundness: for random tiles, if the verdict is AllSolid/AllAir, brute-force the
      lattice and assert every sample agrees. This is the highest-consequence function in the plugin
      and is currently validated only by reasoning; a false verdict is an invisible, collisionless hole.
   3. DiffLayer under contention: N readers + a writer, assert no crash and monotonic version.

B. Phase 1 skeleton — `Public/VoxelDensityOp.h`: `IVoxelDensityOp` (PrepareChunk / Eval /
   EffectOverBox / IsXYPure), `EVoxelOpEffect { CarveOnly, FillOnly, Both, Identity }`, the four role
   tags, `FVoxelOpContext` (carrying LayoutVersion), and the combiner enum. Header + docs only, no
   ports yet. Start with DIRECTION-only effects — no numeric intervals (OPSTACK-PLAN §2 explains why
   this alone reproduces every hand-written ClassifyTile guard, and why it makes step 1 small).

C. Then STOP and hand off for a build. Do not start porting Maze in the same batch.

If (A) fails on the current code you have found a live bug — report it, fix it, don't build on top.

FOUR THINGS AGREED WITH JAHNI 2026-07-27 THAT THE PLAN UNDER-STATES — hold these as goals

(i) THE BIGGEST PERF PRIZE IS TILE-SKIPPING FOR CAVE STRATES, and it is currently zero. Read
    `ClassifyTile`: any chunk that is neither a bedrock gap nor SurfaceWorld hits
    `return EVoxelTileClass::Mixed;  // archétype cave […] pas prouvable en v1`. So TunnelNetwork,
    Maze, VerticalShafts, FloatingIslands, FlatPlain, CrystalChamber and Underwater capture NONE of
    T1.d's win (which was 84% of gens empty, −44% worker CPU). `fable-idea` wanted this from the start
    ("for cave strates, 'no room/tunnel/passage/spine/seal/diff-layer bounds intersect' — all bounding
    data already exists") and it never happened because a bespoke prover per archetype was too much.
    `EffectOverBox` IS the generic mechanism. A room-graph source returning `Identity` when no room or
    tunnel bound reaches the box makes deep bedrock skippable for the first time. Treat this as an
    explicit deliverable of each port, not a side effect.

(ii) COMPILE THE STACK TO A FLAT TAPE, and don't wait for Phase 3 if the profile says otherwise.
    Naive per-voxel virtual dispatch is ~6 ops × ~43k samples/tile ≈ 257k indirect calls ≈ ~1 ms/tile
    of pure overhead — material against current gen cost. In `PrepareChunk`, compile the stack into a
    flat `(opcode, params)` array and run a switch over a small dense opcode set in the inner loop: no
    vtables, params cache-hot, predictable branches. Offsetting win, worth stating: ops that are
    disabled or out-of-scope are ABSENT from the tape, so the ~15 per-voxel `if (Params.X > 0)` gates
    inside `GetDensityWithParams` today become zero cost instead of one always-false branch each.
    Take an Insights capture after Phase 1; if dispatch shows up, do the tape then.

(iii) THE PERF STORY IS "≈ NEUTRAL PLUS ONE REAL WIN", NOT "FASTER". Do not oversell it in docs or
    reports. The reason to do this refactor is composition. Honest expectation: neutral per-voxel after
    the tape, meaningful gain on tiles-never-generated. If a measurement contradicts that, say so.

(iv) SHIP PRESET STACKS so authoring doesn't regress. A simple world today is "pick an enum, fill one
    struct"; after, it's "assemble 3-5 assets in the right order", which is more clicks and a new class
    of mistake (ordering is now semantic). Provide `DA_Stack_ClassicGrotto`, `DA_Stack_OpenWorld` etc.
    as starting points a strate can diverge from. `ECaveGeneratorType` is expected to disappear
    eventually — but only after every archetype is ported; it stays as the fallback path until then.

HOW TO REPORT AT EVERY STOP
- What changed, file by file.
- "Ready to build" + the specific spots likely to error (signatures, UHT, includes, template/lambda
  capture) so Jahni knows where to look.
- What he should LOOK AT in-editor afterwards, concretely, and what a pass vs fail looks like.
- What the next batch will be.
- Anything you became unsure about. Ask rather than assume — for UE API behaviour especially, ask him
  for the docs instead of guessing (DivideAndRoundDown truncating rather than flooring already cost
  build cycles once).

DISCIPLINE
- Update CODEMAP §3 rows for any new/renamed symbol; ARCHITECTURE §8 for design changes; tick phases
  in OPSTACK-PLAN.md as they land. Comments are French + English — match the surrounding file.
- Never edit Binaries/, Intermediate/, *.generated.h.
- Density sign: NEGATIVE = solid, POSITIVE = air at the mesher. #1 source of confusion.
- Commit per feature with a real message (this branch exists so you can commit freely; `main` is the
  known-good fallback). Do not push.
- If the plan turns out to be wrong for this domain, SAY SO and stop. There is an explicit
  stop-trigger on Phase 1: if it exceeds ~2 days or the source/modifier split doesn't fall out
  naturally from the existing code, revert and report rather than pushing through.

UNATTENDED OPERATION — Jahni may start you and go to sleep. Read this before your first tool call.

YOU CANNOT CHECK YOUR REMAINING BUDGET. No tool reports usage or quota. So do not try, and do not
claim to. Instead assume the harder thing: **this session can end at any moment, without warning,
mid-edit, and nobody will be watching.** Everything below follows from that.

CRASH-SAFE DISCIPLINE (non-negotiable when unattended)
1. `git commit` after every coherent unit — a file, a test, a header. Small and often. You are on
   branch `experimental`; `main` is the known-good fallback, so committing costs nothing and a
   half-finished commit is infinitely better than an uncommitted half-edit. Pushing `experimental`
   is fine and expected (it is tracked as `origin/experimental` since 2026-07-29); **never push
   `main`.**
2. Maintain `OPSTACK-PROGRESS.md` at the plugin root. APPEND (never rewrite) a dated entry per
   milestone: what you did, what you believe is true, what is UNVERIFIED (i.e. everything not yet
   built), and the single next action. Write the entry BEFORE starting the work it describes, so an
   abrupt death still leaves an accurate marker. This file is how the next context resumes.
3. Never leave a file mid-transformation across a stopping point. If you're partway through changing
   a signature and its call sites, finish all call sites or revert the change. A tree that doesn't
   compile *for a reason you documented* is fine; one that doesn't compile for an unknown reason is
   the thing that wastes Jahni's morning.
4. Don't batch a risky change with a safe one in the same commit. If a build fails he needs to know
   which half did it.

WHEN YOU REACH THE BUILD GATE — this will happen quickly, and it is not a failure
The first batch (A + B) is a few hours at most, and then you physically cannot verify anything. At
that point: STOP writing plugin code. Do NOT invent more C++ to fill the night — writing unverified
code on top of unverified code is the exact failure `AUDIT-2026-07.md §P3` documents, and doing it
unattended would be the worst version of it.

Instead work the BUILD-FREE QUEUE, in this order. All of it is genuinely useful and none of it can
break anything:

  Q1. ★ THE DECOMPOSITION MAP — the highest-value unattended task by far. Create
      `OPSTACK-DECOMPOSITION.md`: for EACH of the 8 archetypes, read its density function carefully
      and write out its proposed op breakdown — which FIELD SOURCE, which COMBINERS, which DETAIL
      MODIFIERS, and exactly which existing params migrate to which op asset (field by field, so
      nothing is silently dropped). Note per op: `IsXYPure()`, its `EffectOverBox` strategy, and any
      shared primitive two archetypes could reuse. This is hours of careful reading, has zero build
      risk, and it de-risks and speeds up every later port. Do this before anything else in the queue.
  Q2. Audit the `FStrateGenerationParams` 74 fields against Q1 and list any that no op claims — those
      are either dead or a decomposition gap. Report, don't delete.
  Q3. Tick the stale "PENDING BUILD" markers in `fable-idea.md` / `ARCHITECTURE.md` (confirmed
      resolved 2026-07-26 — everything is built and working; see `AUDIT-2026-07.md §0`).
  Q4. Fix `.gitignore` to `!*.md` — the design docs are currently untracked (`AUDIT P1`).
  Q5. STOP. Write the final `OPSTACK-PROGRESS.md` entry and a clear "good morning" summary: what's
      ready to build, exact likely error spots, what to look at in the editor, what a pass looks like.
      Then idle. Do not start Phase 2. Do not port a second archetype. Do not refactor anything not
      in the plan.

There is no prize for burning the whole night. A small, committed, well-documented, build-ready
increment plus a complete decomposition map is a genuinely good night's work.
```

---

## Autonomy — what a fresh context can and cannot do alone

**Asked:** *"possibly even, you tell me if it's possible, for you to wait for the token to replenish? to
continue it all automatically and on your own."* — and then: *"can you tell it to check for token remaining
before continuing actions? I plan to let it run as I sleep."*

**On checking the budget: no. There is no tool that reports remaining usage or quota**, so an instruction to
"check tokens first" would be unfollowable — a safety net that looks real and isn't. The prompt therefore
instructs the opposite and stronger discipline: **assume the session can die at any moment, unattended**,
and make every stopping point crash-safe (commit-per-unit, an append-only progress log written *before*
the work it describes, never a half-applied edit). That achieves the actual goal — not waking up to a
broken tree — without depending on information the session can't get.

**On resuming automatically across a limit reset: the mechanism exists, but don't use it for this, and
tokens are not the real limit.**

- A self-pacing loop is available (`/loop` with no interval, which schedules its own wakeups). Whether it
  resumes cleanly across a usage-limit reset is not something to promise — treat it as unverified.
- **The actual blocker is the build gate, not the budget.** Every meaningful step of a C++ refactor ends
  at a compile, and rule #1 is that Jahni compiles. A loop left running would therefore do exactly one
  thing: **write more unverified code on top of unverified code** — which is the precise failure pattern
  `AUDIT-2026-07.md §P3` was written about (three weeks of stacked "PENDING BUILD"). Automating it would
  deepen the problem it diagnosed.
- **What genuinely scales instead: batch size.** A single context can produce a large, coherent,
  self-contained batch before stopping. Standing permission for multiple changes per build already exists.
  So the throughput lever is "fewer, bigger handoffs", not "unattended looping".
- **What IS worth looping:** work with no build gate — documentation passes, analysis, or the separate
  web-based world-rating harness. Not this.

**So the honest shape of "handling it on its own":** a fresh context can own the *design decisions, the
code, the doc updates and the sequencing* end to end, across many sessions, resuming from
`OPSTACK-PLAN.md §9`. It cannot own *verification*. That stays with Jahni, and given that the failure
modes here are invisible holes and silent seams, that's the correct place for it.

### What an overnight run realistically produces

Setting expectations honestly, because the first batch is deliberately small:

| | |
|---|---|
| ~1–3 h | Phase 0.5 tests + Phase 1 skeleton header. **Then it hits the build gate and cannot verify anything.** |
| remaining night | The BUILD-FREE QUEUE — dominated by **Q1, the decomposition map**: all 8 archetypes read carefully and broken into source / combiners / modifiers, with every param field traced to its destination op. Genuinely hours of work, zero build risk, and it makes every later port faster. |
| morning | A committed build-ready increment, `OPSTACK-PROGRESS.md`, `OPSTACK-DECOMPOSITION.md`, and a "good morning" summary with exact likely error spots. |

**What it must NOT do overnight:** port archetypes, start Phase 2, or write more plugin C++ once the gate
is reached. The prompt says this explicitly, twice, because "fill the night with code" is the tempting
wrong answer and it reproduces the stacked-unverified-work pattern the audit was written about.

---

## Feasibility, honestly

| Phase | Effort | Risk |
|---|---|---|
| 0.5 — three tests | 1 session + 1-2 build rounds | Low. May surface a real bug (that's a win). |
| 1 — skeleton + Maze decomposed | 1-2 sessions + 2-3 build rounds | **Medium — this is the go/no-go.** |
| 2 — port remaining archetypes | 1 session each, opportunistic | Medium; `TunnelNetwork` last, it owns §8.4. |
| 3 — ops as assets, strate = op list | several sessions | Medium-high; big authoring-surface change. |

**The single genuine unknown is Phase 1**, and it's cheap to find out — one archetype, timeboxed, with an
explicit abort. If `Maze` doesn't decompose cleanly into a source + a modifier, the abstraction is wrong
for this domain and two days bought that knowledge.

**The largest hidden cost is not code, it's re-tuning.** Every ported archetype needs its params re-dialled
to look right again (§2.6 accepts this). That's Jahni's time in the editor, not a context's time writing
C++, and it is likely to dominate the schedule.
