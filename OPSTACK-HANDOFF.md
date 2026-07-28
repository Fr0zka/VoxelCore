# Handoff — VoxelForge operator stack, 2026-07-28 (Phase 2 complete and green)

> Paste the block below into a fresh session. Everything it refers to is on disk and in git.
>
> **State:** Phase 2 is **DONE — 8 of 8 archetypes ported, built, and green** (14 tests).
> `ClassifyTile` consumes `ClassifyBox`. One open warning, one clear next task.

---

You're picking up the VoxelForge UE5 voxel plugin on branch `experimental` (already checked out —
do not create another). I'm Jahni. The design and the history are written down so you don't
re-derive them.

## Read first, in this order

1. **`CLAUDE.md`** — project rules. **Rule #1 is absolute: never build, compile, or run the editor.**
   I build everything myself. When code is done, stop, say "ready to build", list the likely
   compile-error spots, and wait.
2. **`OPSTACK-PROGRESS.md` — THE LAST ENTRY FIRST.** Append-only log; the resume point. The last
   entry is the green build with every measured number in it.
3. **`OPSTACK-PLAN.md`** — the plan. **§2.6.1 is the acceptance bar** and supersedes §2.6.
4. **`OPSTACK-DECOMPOSITION.md`** — per-archetype breakdown. **§0.2** (the amplitude bound) is the
   live one; §2 TunnelNetwork and §8 Underwater are now history, not instructions.
5. **`AUDIT-2026-07.md`** — **§C2 has a CONFIRMED sub-item as of 2026-07-28, read it**; §C10 is
   SOLVED, don't reopen; §C9's library half is the top open theoretical risk with 0 measured
   exposure.
6. **`CODEMAP.md`** — navigation. Trust symbol names over line numbers.

## Where things stand — the transition is COMPLETE and VERIFIED

All 8 archetypes have an operator-stack twin, per-strate opt-in, each equivalence-tested **bit for
bit** against its original density function. The `switch` and the stack are now two complete,
interchangeable implementations.

| Archetype | State |
|---|---|
| `Maze` | ✅ ported, bit-identical, wired |
| `FlatPlain` + `CrystalChamber` | ✅ **one op for both**, bit-identical, wired |
| `SurfaceWorld` | ✅ ported incl. biome blending, bit-identical, wired |
| `VerticalShafts` | ✅ ported, bit-identical, wired |
| `FloatingIslands` | ✅ ported, bit-identical, wired — the stack that runs **backwards** |
| `TunnelNetwork` | ✅ **19 ops**, bit-identical incl. all 12 detail modifiers + per-room override |
| `Underwater` | ✅ same builder, second `case` — ⚠️ its check has a coverage gap, see below |

Everything sits behind `UVoxelStrateDefinition::bUseOperatorStack`; the ported list lives **only** in
`UVoxelStrateManager::UsesOperatorStackForChunk` (now all 8). **No strate asset has the box ticked**
— that is my call and I haven't made it. But the flag is no longer a no-op anywhere: ticking it now
really switches that strate onto the stack, for density *and* for tile classification.

`ClassifyTile` **consumes `ClassifyBox`** for cave archetypes (SurfaceWorld and bedrock gaps keep
their hand-written exact-lattice proofs). `GetDensityAt` and `ClassifyTile` build the stack through
the **same** factory, `VF_BuildOpStackForChunk` — a second copy would be a hole, not a bug.

## The one open warning

```
Underwater (stage C2): bit-identical across 2000 samples — 0 of them in open cave (0.0%)
```

**A green bit-identity over 2000 samples of solid rock is not evidence** — it is exactly what two
agreeing voids look like. Same failure as stage A's 1.1 % run, in a different slot, caught by a
counter written for it.

A real bug surfaced while diagnosing: the sampled chunk-Z range used `Z / CHUNK_SIZE`, and C++
integer division **truncates toward zero**. TunnelNetwork is at the top of the layout in positive Z
where truncation == floor, so it could not show there; Underwater is at the **bottom, in negative
Z**, where it shifts the upper chunk bound a notch high and the `Clamp` piles samples into the top
seal band. Fixed (`FloorDivChunk`), sampling widened 8 → 24 clusters, **and not trusted**: new
**check 5b** gives each of the three possible causes its own number and prints how to read them.

**⚠️ That commit (`ce409e7`) is written but NOT BUILT.** First action: build, run the `VoxelForge`
filter, read the `Underwater diagnosis` line. It names the cause; the fix follows from it.

## Then the one task everything is waiting on

**Make `FRoomGraphSource::EffectOverBox` answer spatially.**

TunnelNetwork proves **0 of 40** tiles today, and the test asserts that. The chain dies at the room
source, which returns `Both` with unknown amplitude before anything downstream is reached. Its room
and tunnel bounds (`FCachedRoom::CullRadiusSq`, `FCachedTunnel::BoundRadiusSq`) are **already in the
SDF cache**; what it costs is building that cache for the *queried box*, on the querying thread.

That cost is now clearly worth paying, and every other piece is already built to receive it:

- `ClassifyTile` consumes `ClassifyBox` in production, so a proved tile skips `GenerateMesh` —
  30 000+ density evaluations saved against one `BuildChunkCache`;
- the fold carries **numbers** (`MaxCarveOverBox` / `MaxFillOverBox` / `ForcedMarginOverBox`), so a
  bounded worm no longer kills `AllSolid` on rock that is solid by more than it can carve;
- the twelve detail modifiers already **inherit** the room source's verdict via `VF_NoCaveOverBox` —
  the day the source says `Identity` for a box, all twelve follow, in one place rather than thirteen.

**Keep the brute-force check.** `VoxelForge.OpStack.ClassifyTileSoundness` verifies verdicts against
`GetDensityAt` on a world where every strate opted in. A false verdict is an invisible hole: no
geometry, **no collision**, until a player falls through it.

## ⚠️ Debts that must be paid BEFORE that lands, not after

Both were introduced knowingly and are written at the exact site a reader would land on.

1. **Box bounds read STRATE params, but a per-room op can raise them.** `EffectOverBox` and the new
   amplitude bounds are computed from strate params, because a box spans many rooms. But `ApplyTo`
   writes the op's value **even where the strate's was 0**, so a room op can enable a modifier the
   strate had switched off, or give it a bigger amplitude. A box verdict on a strate with a
   terrain-op pool can therefore be **too optimistic** — the dangerous direction. Harmless while the
   room source answers `Both` (nothing is provable anyway); **not harmless the moment it doesn't.**
   Noted at `FLayerLineMod::EffectOverBox` and `FRoomGraphSource::LocalParams()`.
2. **`AUDIT §C2` is confirmed and unfixed on the `switch` path.** `GetGenerationParams` blends params
   *within* a strate (`Alpha` depends on chunk Z for `Gradient`, and on chunk XY too for
   `Interleaved`), and `Gradient` + `TransitionBlendChunks = 2` are the **defaults**. The original's
   SDF cache key has neither params nor chunk Z, so a worker evaluates the second chunk it builds
   against the first chunk's rooms — and *which* chunk came first depends on worker order, so two
   peers can diverge from the same seed. The op stack does **not** inherit it (params CRC in the
   key), and `ClassifyTile`'s new path guards against it explicitly (params must be bit-identical
   across every chunk coord the box touches). The fix on the `switch` path is a params CRC in its
   key — a live-generation change that wants a build in front of it.

## After that, in order

1. **PERF — unparked.** The op path is measurably slower. One cause found and fixed (the column memo
   discarded itself every chunk). Remaining suspects in order: the hashed column lookup vs
   `GSurfColCache`'s direct-indexed box, then per-voxel virtual dispatch. Also measured and stated:
   the gate is now tested twelve times per voxel instead of once (stage B5's deliberate trade).
   **Measure before optimising** — that is the §C10 lesson.
2. **`VerticalShafts` proves 0 of 60 tiles.** Pessimistic, not wrong: `EffectOverBox` returns
   `CarveOnly` whenever any shaft is within a `Spacing*1.6` halo instead of testing real connector
   capsules. Lost CPU, never a hole.
3. **`AUDIT §C9` library half** — `sinf`/`cosf` are not IEEE-754 specified, so MSVC's CRT and glibc's
   libm can differ. Currently **0 samples within 1e-6 of the isosurface**, i.e. no measured risk. Run
   `CrossPlatformDigest` on Linux, compare the SHAPE digest, pin it. The real fix if ever needed is a
   deterministic in-house sin/cos.
4. **Phase 3 — ops as data assets.** A design conversation, not a transcription. Don't start it
   unprompted. What makes it possible is already in place: ops depend on capabilities
   (`IVoxelBiomeField`), never on `UVoxelGenerator`.

## Hard rules that prevent real bugs

- **Density sign:** negative = solid at the mesher. Inside the op stack the convention is INTERNAL
  (**positive = solid**), negated once by the caller. The SDF channel uses standard SDF convention.
- **Never run both density paths in one world.** **Comparing them is legitimate** — §C10 is closed
  since `FPSemantics = Precise`, and all eight equivalence tests compare bit for bit. They are
  **port-correctness oracles**, not fidelity checks: §2.6.1 requires *same seed ⇒ same world on every
  peer*, not resemblance to the pre-refactor world.
- **Every cache key includes `LayoutVersion` AND the params.** See §C2 and the overhang regression of
  2026-07-27, where omitting the params silently deleted the overhang and only 1 sample in 20 000
  crossed the isosurface.
- `ProcessQueue` stays `EQueueMode::Mpsc`; `Epoch` carries through every async path; don't "optimize"
  the `ARCHITECTURE §8.10` invariants.
- Commit per coherent unit with a real message. **Never push.** `main` is the known-good fallback.
- Update `CODEMAP §3`, `ARCHITECTURE §8`, tick `OPSTACK-PLAN`, append to `OPSTACK-PROGRESS.md`.
- **When inserting a class into `VoxelDensityOpStack.cpp` / `VoxelHeightOpStack.cpp`, put it ABOVE
  the labelled end of the anonymous namespace.** Anchoring on the FACTORIES banner puts it outside,
  and the brace added with it closes nothing. Made that mistake twice; both files say so at the
  exact line.

## Method lessons this refactor actually paid for

Ordered by how much they cost.

- **Instrument before hypothesising.** §C10 cost six builds and five refuted hypotheses, then was
  solved for free by a build setting changed for an unrelated reason. Park a question whose
  consequences are measured and benign.
- **Verify the premise before reasoning from it.** Five times now a confident chain rested on an
  unchecked assumption and the check reversed it: C1's *documented* fix was wrong; "C9's risk is gone
  after FPSemantics" was wrong; "C1 is closed, 0 sites left behind" was wrong (the sweep matched a
  *spelling*); "PitDensity enables pits" was wrong; "there are 13 detail modifiers" was wrong (twelve,
  and only eleven read the per-room copy). **A grep over a spelling is evidence about the spelling.**
- **Read the code, not the comment.** The cliff modifier's comment promises a sampled Z±1 gradient;
  the code samples nothing and uses a Z-stretched Perlin it *calls* `VertGrad`. Ported as written —
  and written down, so nobody "fixes" it from the comment.
- **A perf change can be a correctness change.** The column-memo optimisation silently deleted the
  overhang; the tests caught it the same day. Invisible to inspection, and it produced plausible
  terrain.
- **Coverage is a number, not a boolean.** Four related traps, each of which produced a green run
  that proved almost nothing:
  - *A test that prints nothing on success is indistinguishable from one that never ran.*
  - *A guard that only trips at zero notices absence, it does not measure coverage.* Use fractions.
  - *A success message that **asserts** coverage instead of reporting it reads as evidence while
    measuring nothing.*
  - *A check can be vacuous as well as a counter.* "Nothing leaked" is worthless unless something
    happened — so the gate check also reports how many samples move when the modifiers are zeroed.
- **Enabling a feature is not evidence it fired — ask the structure, not the output.** Setting
  `PitDensity` did nothing (wrong struct). Diffing two stacks with/without the op pool would have
  *lied* (the pool is not in the SDF cache key, so both share the `thread_local` cache). What worked:
  call `BuildChunkCache` and look at `Pits.Num()`. **Prefer the check that can fail for exactly one
  reason** — and when a zero has three possible causes, give each one its own number.
- **An oracle that shares the defect under test proves nothing.** The stale-cache check compares each
  stack against *itself evaluated alone*, never against the original — which keys its SDF cache
  without the params and would fail it.
- **One definition, not two kept in sync.** `VF_BuildOpStackForChunk` exists because a tile skipped on
  the verdict of a stack that is not the one producing its density is a hole. A "keep these in sync"
  comment would not have been enough.
