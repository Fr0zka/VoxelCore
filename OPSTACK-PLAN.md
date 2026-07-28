# VoxelForge — Density Operator Stack: the plan

> **What this is:** the agreed direction for turning VoxelForge from an *archetype dispatcher* into a
> *composable density pipeline*, so new world ideas become authoring instead of C++. Written
> 2026-07-26 as a handoff for a future context — read this instead of re-deriving it.
>
> **Status (2026-07-28):** **Phases 0.5, 1 and 2 CODE-COMPLETE — 8 of 8 archetypes ported**, each
> bit-identical to its original in an equivalence test, all wired behind `bUseOperatorStack`:
> **Maze · FlatPlain · CrystalChamber · SurfaceWorld (biomes included) · VerticalShafts ·
> FloatingIslands · TunnelNetwork · Underwater.** The archetype `switch` now has a complete
> operator-stack twin, opt-in per strate.
>
> ✅ **BUILT AND GREEN, 2026-07-28 — 14 tests.** TunnelNetwork A+B bit-identical over 6000 samples
> with all twelve group-coverage probes non-zero, all four noise branches covered, 0 gate leaks, and
> C1 proved by 10 Terrace-op rooms containing 1119 samples. One open warning: the `Underwater` check
> landed 0 samples in open cave, so its bit-identity proves little — diagnosed, not guessed, in
> `OPSTACK-PROGRESS.md`'s last entry.
>
> **Not done, and it is the next real prize:** `ClassifyTile` still uses hand-written guards and does
> not consume `ClassifyBox`. That is where measured tile-skipping becomes frames.
>
> Two things came out of Phase 2 that were not in the original design: **height space**
> (`VoxelHeightOp.h`, a second operator family — some things are not another channel but another
> *space*) and **`IVoxelBiomeField`** (ops depend on a capability, never on the generator, which is
> what lets them become assets in Phase 3). Both are described in `OPSTACK-DECOMPOSITION §5`.
>
> **Known open:** generation is measurably slower on the op path (one fix landed — the column memo
> was discarding itself every chunk; virtual dispatch and the hashed lookup remain). Deferred by
> Jahni until the transition is complete. Live state and the next action live in
> [OPSTACK-PROGRESS.md](OPSTACK-PROGRESS.md) — read its last entry first. The per-archetype
> breakdown is in [OPSTACK-DECOMPOSITION.md](OPSTACK-DECOMPOSITION.md).
>
> **Read first:** `CODEMAP.md` (navigation) · `ARCHITECTURE.md §8.10` (the perf invariants this must not
> break) · `AUDIT-2026-07.md §6` (the 3D hazards this is designed to kill permanently).
>
> **Jahni's goal, verbatim (2026-07-26):** *"a world generator, of any kind, of any possibility, almost
> — any combination of ideas you could guess, could be happening."*

---

## 0. The one-paragraph version

`UVoxelGenerator::GetDensityAt` is a `switch` over 8 hardcoded `ECaveGeneratorType` values, each owning
a bespoke density function and param struct. That means (a) a new world idea costs ~6 edit sites, and
(b) **ideas cannot combine** — one archetype owns the whole voxel. The fix is to make density a **stack
of small operators**, each of which can `PrepareChunk` / `Eval` / declare **what it can do to a box**.
That last part is the keystone: it makes `ClassifyTile` generic and correct *forever*, instead of a
hand-written guard per feature (the thing that already caused one revert and is the top hazard for the
current 3D work). Staged so the game never stops working, and so the first useful step is small.

**Non-goal, stated deliberately:** this must *serve* the descent-through-strates structure, not dissolve
it. "The world is a vertical stack you dig down through, each layer its own place" is the idea of this
project — the seals, the (0,0) spine and the passages only mean something because of it. The op stack
should make each strate more surprising, not turn the world into undifferentiated composable soup.

---

## 1. Why this specific shape fits this specific codebase

Not a generic "use composition" argument — three concrete reasons:

**(a) It formalises what the code already does by hand.** Every archetype already: hoists chunk-constant
work into a `thread_local` cache (`PrepareChunk`), evaluates cheaply per voxel (`Eval`), and — in
`ClassifyTile` — has a hand-written statement of what it can do to a tile (`Bounds`). The operator model
isn't a new discipline; it's the existing discipline, named.

**(b) The 8 archetypes are ~6 functions wearing costumes.** `FlatPlain` and `CrystalChamber` are literally
the same function with different defaults; `Underwater` is `TunnelNetwork` + a water flag. Decomposed,
they're roughly **15 orthogonal primitives** (floor surface, ceiling surface, hash cylinders, room SDF
graph, tunnel capsules, worm noise, lattice corridors, heightfield stack, island blobs, boundary seal,
spine carve, passage carve, disturbances, surface ops, diff layer). 15 primitives that combine covers
vastly more than 8 that don't.

**(c) It kills the recurring hazard permanently.** T1.d guards are currently written per feature
(`AnyPassageNearBox`, the spine circle test, chasm/bridge flags, `HasAnyModInChunkRange`, phase-2's
`OverhangMargin`). Every new 3D feature needs one, forgetting one is a hole, and one was already
forgotten badly enough to revert T1.d v1 on 2026-06-26. Under this model a new op **cannot ship without
answering the question**, and a test catches it if the answer is wrong.

---

## 2. The keystone insight — start with DIRECTION, not intervals

The full version of `Bounds` returns a numeric interval. **Don't start there.** Almost every existing
operator is *one-directional*: it only ever carves, or only ever fills.

```cpp
enum class EVoxelOpEffect : uint8
{
    CarveOnly,   // can only move density toward AIR   → kills the AllSolid hypothesis
    FillOnly,    // can only move density toward SOLID → kills the AllAir  hypothesis
    Both,        // unconstrained
    Identity     // provably no effect on this box (the early-out that makes it fast)
};

virtual EVoxelOpEffect EffectOverBox(const FBox& VoxelBox, const FVoxelOpContext& Ctx) const;
```

**This alone reproduces every hand-written guard in `ClassifyTile` today, generically.** Look at the
current code: "passages ⇒ `bCanSolid = false`" *is* `CarveOnly`. "bridges/ridges ⇒ `bCanAir = false`"
*is* `FillOnly`. "no passage near this box ⇒ skip" *is* `Identity`.

So **Phase 1 ships with no numeric bounds at all** and already gets the whole safety property. Numeric
intervals are a later tightening for gen-cost, not a correctness prerequisite. This is what turns a scary
refactor into a small first step — and it's the thing to remember if this plan ever feels too big.

### Every existing primitive already has an obvious answer

Filled in here so a future context doesn't have to re-derive it:

| Primitive | Effect | Identity test (cheap) | Numeric bound (later) |
|---|---|---|---|
| `ApplyBoundarySeal` | **FillOnly** (`FMath::Max`) | box misses both seal bands | `+[0, BaseDensity]` |
| `ApplyOriginSpine` | **CarveOnly** | circle-vs-box XY, + Z outside interior | `−[0, Base*2 + Seal]` |
| `ApplyPassageCarving` | **CarveOnly** | `AnyPassageNearBox` — **already written** | `−[0, …]` via `AirTarget` |
| Disturbance chasms | **CarveOnly** | `ChasmDensity == 0`, or lattice miss | `+[0, Solid]` toward air |
| Disturbance bridges/ridges | **FillOnly** | density == 0, or lattice miss | `−[0, Solid]` |
| F20 overhang | **FillOnly**, banded | outside `(TerrainZ, TerrainZ+Height]` — **already written** | union ⇒ `max` only |
| Room / tunnel / column SDF | Both | bounding sphere vs box — **already written** | **Lipschitz-1**: `SDF ∈ [SDF(c) − r, SDF(c) + r]`, `r` = box half-diagonal |
| fBm / Ridged term `k·N(...)` | Both | `k == 0` | `±\|k\|` — `FBM` returns `[-1,1]` by construction (`Total/MaxValue`) |
| Heightfield (`TerrainZ − Z`) | Both | — | `[minT − Zmax, maxT − Zmin]`; **or sample the lattice exactly** (see below) |
| Diff layer | Both | `HasAnyModInChunkRange` — **already written** | `±max\|Strength\|` over mods in range |

Two things to notice:

- **Five of these already exist as code.** The work is mostly *moving* guards, not inventing them.
- **`Bounds` is allowed to be exact-by-sampling, not just analytic.** Today's `ClassifyTile` gets the
  tightest possible SurfaceWorld verdict by evaluating `ComputeSurfaceColumn` **on the mesher's exact
  lattice** — same functions, same floats, so the verdict is exact rather than estimated. That must
  survive. The contract is "conservative", not "closed-form": an op may sample to answer.

### Prior art (30 minutes, worth it before Phase 1)

- [Keeter, *Massively Parallel Rendering of Complex Closed-Form Implicit Surfaces*](https://dl.acm.org/doi/10.1145/3386569.3392429) + [fidget](https://github.com/mkeeter/fidget) — interval arithmetic per expression node to prune empty regions. The industrial version of this idea.
- [Barbier et al., *Lipschitz Pruning: Hierarchical Simplification of Primitive-Based SDFs* (CGF 2025)](https://onlinelibrary.wiley.com/doi/10.1111/cgf.70057) — **the better fit for us**: bounds each primitive's range of influence and treats primitives as **black boxes**, where full interval arithmetic needs interval semantics defined for every node. Our ops are SDFs (Lipschitz by construction) and bounded-amplitude fBm. Black-box bounds; don't build a node-level IA engine.

---

## 2.5 ⚠️ The op TAXONOMY — and why this is NOT the old room-ops system

**Jahni's objection, 2026-07-27, and it is the correct one:** *"ops structure — which, let's all be honest,
was what I had before, 'room operations' which would modify stuff, so I sure hope your idea is not that.
There's a world of difference between a grotto strate and an open world strata."*

He is right, and a fresh context **must** internalise this or it will build something useless.

`UVoxelTerrainOpDefinition` today can only **perturb density near a surface that already exists**, inside
a fixed archetype (`Terrace`, `LayerLines`, `Ribbing`, `Cliff`, `Scallop`, `Overhang`, `Arch`, `Column`,
`Pit`, `Chimney`, `Dome`, `Pinch` — all applied near cave walls where `bNearCaveSurface`). It cannot turn a
grotto into an open world, because it never decides **what the field IS**. That decision lives in the
`switch` in `GetDensityAt`, which is exactly the thing we are removing.

So the op stack has **four ROLES**, and the old system only had role 3:

### Role 1 — FIELD SOURCES (the new thing; this is the "world of difference")
Produce a density field *from nothing*. **This is what makes a grotto a grotto and an open world an open
world.** Each of today's archetypes is fundamentally one of these:

| Source | Today's archetype |
|---|---|
| Heightfield ground + sky-cap ceiling | `SurfaceWorld` |
| Room-graph SDF (hash rooms + tunnel capsules) | `TunnelNetwork` |
| Floor/ceiling slab void | `FlatPlain`, `CrystalChamber` |
| 3D lattice corridors | `Maze` |
| Full-height shafts + connectors | `VerticalShafts` |
| Suspended island blobs in open void | `FloatingIslands` |

A source is a first-class op with the same three-method contract. **A "strate archetype" therefore stops
being an enum and becomes `source + combiners + modifiers`.** Two strates differ in their SOURCE first,
their modifiers second.

### Role 2 — COMBINERS (how sources merge; this is what makes ideas compose)
`Replace` · `Union`(min) · `Subtract`(max) · `SmoothUnion`/`SmoothSubtract` (reuse `VoxelSDF::SmoothMin/Max`)
· `Mask` (scale the next op by a field: biome weight, slope gate, relief, depth).

This is the role that buys the ambition. *Floating islands **inside** a grotto. A maze **beneath** an open
world's ground. A room-graph carved **into** a mountain.* None of those are expressible today at any price.

### Role 3 — DETAIL MODIFIERS (the old system, demoted to one role of four)
Roughness, terrace, layer lines, ribbing, scallop, cliff, overhang, domes, pinch. **All of today's
`UVoxelTerrainOpDefinition` types land here, essentially unchanged.** They keep working; they stop being
the whole story.

### Role 4 — STRUCTURAL POST (fixed order, non-negotiable, runs last)
`ApplyOriginSpine` → `ApplyBoundarySeal` → `ApplyPassageCarving` → diff layer.

These are **world invariants**, not creative choices: descent must stay possible, seals must hold, passages
must punch through anything, player edits win. They are ops for uniformity but the stack compiler must
always append them, in this order, regardless of authoring. **An author must not be able to omit them.**

### Plus: SCOPING
Any op may be gated by a region predicate — Z band, XY region, biome index, slope range, depth. Scoping is
what lets one strate hold several sources without them fighting, and it's the mechanism that unifies
"strate" and "biome" into one concept in Phase 3.

> **The test for whether this refactor was worth doing:** can you author *"an open-world surface strate
> whose mountains contain a room-graph cave system, with floating islands in the upper void"* **without
> writing C++?** If no, it collapsed back into the old system and something went wrong.

---

## 2.6 The acceptance bar — "very close", NOT byte-identical

**Jahni, 2026-07-27:** *"I want not a 1/1 replica of the current strates with the current system, but a
possible very close result to what I have right now, else it won't really matter much."*

Two different properties get confused here. Keep them apart:

| Property | Required? | Meaning |
|---|---|---|
| **`ValidateDeterminism` = 0** | **YES, ALWAYS** | the same world point sampled twice, from different cache windows/threads, returns the identical float. This is window invariance (§8.4) — self-consistency. Non-negotiable, it's what prevents seams and MP divergence. |
| **Byte-identical to the OLD system's output** | **NO** | matching the pre-refactor world float-for-float. |

**This is a deliberate relaxation of what an earlier draft of this plan demanded, and it matters
strategically:** if bit-identity were required, the cheap path would be to wrap each old density function
as one monolithic op — 8 opaque ops that don't compose, i.e. **the switch with extra steps and zero
gain.** Releasing that constraint is what permits *real* decomposition into the primitives in §2.5.

> **✅ CONFIRMED THE HARD WAY, 2026-07-27.** The Maze port reproduces `GetMazeDensity`'s **SDF bit for
> bit**, and its final density to within 1-2 ULP on ~2% of samples, with **zero isosurface
> crossings** — geometrically identical, not one triangle moved. The exact origin of that last
> rounding was chased through five measured-and-refuted hypotheses and then **parked by decision**;
> the full evidence is in `AUDIT-2026-07.md §C10`. **Read C10 before ever reopening it.**
>
> **The operational bar for every remaining archetype port, encoded in
> `VoxelForge.OpStack.MazeEquivalence`:** hard-fail on any isosurface crossing (that moves geometry);
> tolerate ULP-scale deltas (the accepted floor); warn on anything larger (that is real port drift).
>
> **And the rule that came out of it:** never run the archetype `switch` and the operator stack in the
> same world, and never compare their outputs for equality — a half-migrated strate would seam. Not a
> client-desync risk (the field is proven bit-pure within a binary); the cross-platform concern is
> `§C9`.

**The bar instead:** for each ported archetype, an authored op stack must reproduce the *character* of the
old one — same scale, same navigability, same feel, recognisably the same kind of place. Judged by Jahni
on a screenshot at a fixed seed, not by a diff. Expect and accept a one-time re-tune, exactly as the T2.a
SIMD noise switch required.

---

### 2.6.1 ⚠️ RELAXED FURTHER, 2026-07-27 — resemblance to the old world is NOT a requirement at all

**Jahni, verbatim:** *"I do not need your work to be identical or near identical to what I had
before, only having it 99.99% at worst reproducible if two people share the same seed, since
everyone rebuilds it on multiplayer."*

**This replaces the "recognisably the same place" bar above.** The requirement is not fidelity to the
past — it is **agreement between peers in the present**. Restated as the only two properties that
now matter:

| Property | Required? | Enforced by |
|---|---|---|
| **Same seed ⇒ same world, on every peer** | **YES — this is the whole bar** | `DensityPurity` within a binary; **`§C9`** across binaries/platforms |
| Resemblance to the pre-refactor world | **NO** | nothing; freely re-tunable |
| Bit-identity with the archetype `switch` | **NO** | nothing; never compare them |

**What this changes, concretely:**

1. **`§C10` is closed, not parked.** It measures old-path vs new-path agreement, and the two paths
   will never both exist in a shipped world. The residue cannot affect anything Jahni requires.
2. **The equivalence tests keep their value, but for a different reason.** They are no longer
   *fidelity* checks; they are **port-correctness** checks — a transcription slip is still a real
   bug, and comparing against the old function is the cheapest way to catch one. Read them that way.
   The hard-fail (isosurface crossing) stays; the ULP grading is now diagnostic only.
3. **`§C9` is promoted from a footnote to THE risk.** "Two people share a seed" is exactly the
   guarantee `/fp:fast` weakens across toolchains, and a Linux dedicated server generating collision
   geometry against Windows clients is the concrete case.
4. **Changes that re-roll the world's noise are no longer expensive.** `§C1` in particular was
   deferred *only* because it forces a re-tune. That objection is gone.

---

## 3. The contract

```cpp
// Chunk-constant inputs. Mirrors what the thread_local CP_* block resolves today.
struct FVoxelOpContext
{
    FIntVector ChunkCoord;
    int32      Step;              // LOD step — ops may cheapen themselves (see §7)
    uint32     Seed;
    uint32     LayoutVersion;     // ⚠️ see AUDIT C2 — every cache key MUST include this
    float      StrateTopWorldZ, StrateBottomWorldZ;
    const FBiomeContext* Biome;   // null = strate has no biome field
};

class IVoxelDensityOp
{
public:
    // Hoist all chunk-constant work here (room lists, biome grids, column caches, lattice bakes).
    // Called once per chunk per worker. This is where today's thread_local caches move to.
    virtual void PrepareChunk(const FVoxelOpContext& Ctx) = 0;

    // Per-voxel. InDensity = what the stack produced so far, MC convention (negative = solid).
    virtual float Eval(float X, float Y, float Z, float InDensity) const = 0;

    // CONSERVATIVE. Phase 1: direction only. Phase 3: add a numeric interval overload.
    // Returning Both is always SAFE (costs CPU); returning the wrong one is a HOLE.
    virtual EVoxelOpEffect EffectOverBox(const FBox& VoxelBox, const FVoxelOpContext& Ctx) const = 0;

    // Declares whether Eval depends on Z. XY-pure ops get the T1.a column-cache treatment
    // generically instead of SurfaceWorld having a bespoke one.
    virtual bool IsXYPure() const { return false; }
};
```

**Composition semantics** — keep the vocabulary small, and reuse what exists (`VoxelSDF::SmoothMin` /
`SmoothMax`):

| Mode | Meaning |
|---|---|
| `Replace` | ignore `InDensity` (stack roots: heightfield, base density) |
| `Union` (min) | add solid — bridges, islands, columns |
| `Subtract` (max) | carve air — rooms, tunnels, passages, spine |
| `SmoothUnion/Subtract` | the same with `SmoothMin/Max(k)` — organic junctions |
| `Add` | scalar accumulate — noise/roughness terms |
| `Mask` | scale the *next* op by a field (biome weight, slope gate, relief) |

`Mask` is what buys most of the expressiveness: "this op, but only in high-relief regions / only on
steep slopes / only in this biome" becomes composition rather than a bespoke gate inside each op.

---

## 4. Order of work

### Phase 0 — THIS WEEK. Ship the 3D caves. Refactor nothing.

The current feature (caves inside mountains, volumetric generation) ships as-is with a **hand-written
`ClassifyTile` guard** — see `AUDIT-2026-07.md §6.1` for why it's mandatory (without it the caves are
never meshed: no geometry, no collision, invisible until you fall through them).

**One constraint only:** write the guard as a standalone function in the shape of the future contract —

```cpp
// Not a member of anything yet. Just the right shape.
EVoxelOpEffect CaveSystemEffectOverBox(const FBox& VoxelBox, const FVoxelOpContext& Ctx);
```

Same work, zero architectural commitment, and it establishes the pattern.

**Also decide here (see `AUDIT §6.2`): placed, not fielded.** Hash-located cave systems with bounds
(like `FCachedRoom`/`FCachedTunnel`) let the guard prove most of the mountain still solid and keep
T1.d's 44% worker-CPU win. A global 3D noise field makes every deep tile `Mixed` and hands that back.

> ✅ **Gate:** caves render and collide when approached through solid rock, and worker CPU hasn't
> visibly regressed.

---

### Phase 0.5 — The safety net. ~1 day. Do this before Phase 1, not after.
> **✅ WRITTEN 2026-07-27 — ⏳ NOT YET COMPILED OR RUN.** Four tests in `Private/Tests/` (the three
> below, plus a live-edit regression test for `AUDIT C2`). The gate below is NOT met until Jahni
> builds and runs them. See `OPSTACK-PROGRESS.md`.

Three automation tests (`Source/VoxelForge/Private/Tests/`, `IMPLEMENT_SIMPLE_AUTOMATION_TEST`).
There are currently **zero tests**, and nothing machine-checks the dozens of "bit-identical" claims in
the docs.

1. **Density purity** — sample 10k points, shuffle query order, re-sample, assert bit-equality. Catches
   every cache-key bug including `AUDIT C2`. Run it across **multiple worker threads**, because
   `ValidateDeterminism` runs on the game thread and would miss worker-cache divergence.
2. **`ClassifyTile` soundness** — for random tiles, if the verdict is `AllSolid`/`AllAir`, brute-force
   the lattice and assert every sample agrees. **This is the highest-consequence function in the plugin
   and it is currently validated only by reasoning.**
3. **`DiffLayer` under contention** — N readers + a writer; assert no crash, monotonic version.

> ✅ **Gate:** all three green on the current code. If #1 or #2 fails, you've found a live bug — fix it
> before building on top.

---

### Phase 1 — The pivot. Port ONE archetype. Timebox it.

**Pick `Maze`.** ~100 lines, no cross-chunk connectivity decision, trivial bound (corridor SDF is
Lipschitz-1 off a lattice), and it's the least-used archetype so a mistake is cheap.

1. ✅ **DONE 2026-07-27 (uncompiled):** `IVoxelDensityOp` + `EVoxelOpEffect` + `FVoxelOpContext` +
   the four role tags + the box-verdict fold, in `Public/VoxelDensityOp.h`. **One addition beyond
   this spec:** `ClassifyBox` is not source-only — forcing ops (the boundary seal inside its band)
   overwrite the input, which pure direction cannot express. Rationale in the header.
   **One open question the decomposition raised:** `Eval` probably needs an SDF channel as well as
   a density channel — see `OPSTACK-DECOMPOSITION.md §0.1`. Decide before porting Maze.
2. **DECOMPOSE, don't wrap** (§2.5, §2.6). Maze becomes a stack, not one op:
   `FLatticeCorridorSource` (role 1 — the capsule field off the 3D lattice) → `Subtract` →
   `FSurfaceRoughnessMod` (role 3 — the existing `SurfaceRoughness` perturbation) → then the four
   structural-post ops appended automatically. **If it comes out as a single `FMazeOp`, the refactor
   has failed its own test** — that's the switch with extra steps.
3. `GetDensityAt` gains **one** branch: strate has an op stack ⇒ run it; else fall through to today's
   switch. **Both systems coexist**, indefinitely if needed.
4. `ClassifyTile` gains a generic path (`EffectOverBox` folded over the stack) used only by ported strates.

> ✅ **Gate:** `ValidateDeterminism` = 0 delta (§2.6 — always), Phase 0.5 tests green, and the Maze world
> is **recognisably the same place** at a fixed seed — same corridor scale, same connectivity, same feel.
> Judged on a screenshot, not a diff. A one-time re-tune of the params is expected and fine.
>
> ✅ **The real proof, and the one that answers Jahni's objection:** once `FLatticeCorridorSource` exists,
> **drop it into a `SurfaceWorld` strate underneath the terrain** and confirm you get a maze inside a
> mountain with no C++ written. If that works, the architecture is doing the thing it was built for.
>
> 🛑 **Stop-and-reconsider trigger:** if Phase 1 exceeds ~2 days, or the source/modifier split doesn't fall
> out naturally from the existing code, the abstraction is wrong for this domain. **Say so plainly, revert,
> and report** — do NOT push through and port a second archetype to prove a point.

---

### Phase 2 — Port opportunistically. No big bang.

Port each archetype **the next time a feature makes you open it anyway**. The switch shrinks on its own.
Suggested order when there's a free choice — cheapest and least risky first:

✅ `Maze` (P1) → ✅ `FlatPlain`/`CrystalChamber` (one op, two default sets — the first real win: two
archetypes collapse into one; **done**, `BuildSlabStack`, 8 archetypes → 7) → ✅ `SurfaceWorld`
(**done**, incl. biomes — needed a whole second op family, `VoxelHeightOp.h`; biggest payoff, biggest
care: the T1.a column cache and the exact-lattice `ClassifyTile` bound both survived) →
✅ `VerticalShafts` (**done**, 3 ops reused from Maze unchanged) →
✅ `FloatingIslands` (**done**, `BuildFloatingIslandStack` — the stack that runs **backwards**: void
source + fill instead of rock source + carve, the *same* classes with the opposite sign; only the
blob source is new) → ✅ `TunnelNetwork` + `Underwater` (**done**, one builder for both —
`BuildTunnelNetworkStack`, 19 ops).

**8 of 8 ported.** The last two were really one: `Underwater` *is* TunnelNetwork plus
`WaterLevelRelative` (§8, re-verified before relying on it), so the switch lost its last two cases in
a single port.

TunnelNetwork was ~1080 lines and was taken in **three stages, each verifiable on its own** rather
than as ~600 unverified lines on top of ~200 (the `AUDIT §P3` pattern):
* **A** — SDF spine: vertical scale, base rock, cave warp, room graph (+ pits + chimneys), carve,
  worms, structural post. Verifiable *while incomplete* because every detail modifier is
  amplitude-gated and defaults to zero, so zeroing them sends the ORIGINAL down exactly stage A's path.
* **B** — the twelve detail modifiers of `STEP 4b–4h`, one group per commit, each with a coverage
  probe that proves the group actually moved something (`B1` roughness, `B2` terrace/lines/ribs,
  `B3` overhang/cliff/scallop/arch, `B4` columns/domes/pinch/floor-bias, `B5` the gate itself).
* **C** — the per-room op override (`§2`'s option (a), and it needed no scoping predicate: one op
  owns the state, eleven read it), `Underwater`, and the flag flip.

⚠️ **`FRoomGraphSource` CALLS `BuildChunkCache`/`EvaluateSDFCached`; it does not transcribe them.**
That is where §8.4's two-region window-invariance discipline lives, and a copy would fork it — with
the fork "validated" by a test that compares it to the original.

Along the way, `FStrateGenerationParams`' 74 fields decompose into per-op structs, which retires the
`VF_STRATE_PARAM_FIELDS` X-macro drift problem for free.

---

### Phase 3 — The payoff. Ops become data.

1. `UVoxelDensityOpDefinition : UPrimaryDataAsset` — one asset per op, mirroring today's
   `UVoxelTerrainOpDefinition` (which is *already* the right authoring shape: an asset + weight +
   probability in an ordered list).
2. **A strate becomes "a Z range + an ordered op list"** instead of "an enum + a param bag".
3. **A biome becomes "an XY region predicate + an op list" — the same mechanism.** Today these are two
   unrelated systems (`ECaveGeneratorType` dispatch vs the biome field with its `bOverrideTerrain`
   special case that only works for `SurfaceWorld`). Unifying them is where the expressiveness comes
   from: any op, scoped by any region, in any combination.
4. Numeric interval bounds where profiling shows `Both` is costing real gen time.
5. **Only if needed:** compile the per-chunk stack into a flat opcode tape and interpret with a switch,
   killing per-voxel virtual dispatch (~43k samples/tile × N ops). Standard technique. **Don't do this
   pre-emptively** — measure first; the noise is likely still dominant.

---

## 5. Invariants this must not break

Non-negotiable. Each has a documented reason and, mostly, a scar.

- **Window invariance (§8.4).** Every op stays a pure function of world coords + seed. Any op with a
  *connectivity decision over a neighbourhood* inherits `BuildChunkCache`'s two-region COLLECT/STORE
  discipline. **Prefer the `Maze` pattern** (pure hash of `(lower node, axis)` — adjacent chunks cannot
  disagree, no cache, no COLLECT region) unless connectivity is a gameplay requirement.
- **T1.a column cache Z-independence.** `FSurfaceColumn` data is keyed `(XY box, StrateKey, Seed)` with
  **no ChunkZ** and shared down the whole vertical stack. XY-pure data goes in the column cache;
  Z-dependent evaluation happens per voxel. `IsXYPure()` exists to make this explicit instead of implicit.
- **Cache keys include `LayoutVersion`** (`AUDIT C2` — today's `CP_Chunk`/`OC_Chunk`/`BM_Chunk` don't,
  and serve stale params after a live edit). `FVoxelOpContext` carries it so a new op can't forget.
- **`ProcessQueue` stays `EQueueMode::Mpsc`**; ops are read-only on workers; `Epoch` carries through
  every async path.
- **The two-pass MC loop, margin ring, and `thread_local` grid reuse** (§8.10) are untouched by all of
  this — the op stack lives *below* `GetDensityAt`, the mesher never knows.

---

## 6. Risks, and how each one is detected

| Risk | Detection |
|---|---|
| A wrong `EffectOverBox` ⇒ **a hole** | Phase 0.5 test #2 (brute-force vs verdict) — the load-bearing one |
| A cache key missing an input ⇒ **seams** | Phase 0.5 test #1 (shuffled order, multi-threaded) |
| Per-voxel dispatch cost | Insights `VoxelForge_GenerateMesh` before/after each port; tape compile if it bites |
| Abstraction is wrong for this domain | The Phase 1 stop-trigger — one archetype, timeboxed, revert cheaply |
| Scope creep into a node-graph editor | See §7 |

---

## 7. Explicitly NOT doing

- **No node-graph editor.** An ordered `TArray` of op assets is 90% of the value. A graph UI is a
  separate project, years later, and chasing it is how generative systems die.
- **No GPU density.** Same reasons as `fable-idea` Part I: readback latency, CPU collision, and
  cross-GPU float determinism is fatal for "replicate the seed, regenerate identically on every peer"
  (`ARCHITECTURE §9.1`).
- **No node-level interval arithmetic engine.** Black-box Lipschitz/amplitude bounds per op (§2).
- **No big-bang port.** If more than one archetype is mid-port at any time, stop.
- **No dissolving the strate structure.** See §0.

---

## 8. Independent fixes — do these regardless, they get worse with time

From `AUDIT-2026-07.md §5`. None depend on this plan; all become harder inside it.

1. **Bound `SeedF`** (`AUDIT C1`) — `const float SeedF = (float)(VoxelHash::Mix((uint32)Seed) & 0x3FFF);`
   at all 6 definition sites. Large seeds currently collapse noise terms to constants. One world re-tune.
   **Do it before tuning 3D caves against a seed you might later randomise.**
2. ✅ **DONE 2026-07-27 (uncompiled)** — `GetLayoutVersion()` added to `CP_Chunk` / `OC_Chunk` /
   `BM_Chunk`, **plus two the audit missed**: `TC_BiomeCache` in `ClassifyTile`, and the
   `GSurfColCache` box key (whose `StrateKey` is `round(StrateBottomWorldZ)`, so a live edit that
   changes terrain params without moving the strate served stale columns — the most visible form of
   the bug). `FChunkBiomeCache::Invalidate()` added, since a validity BOX says nothing about the
   `FBiomeContext` its cells were classified against. (`AUDIT C2`.)
3. **`GetPlayerPosition` no-player flag** (`AUDIT C4`) — `(0,0,0)` is the designed spine landing and
   currently stalls all streaming.
4. **Unbounded joins on shutdown** (`AUDIT C5`).
5. ✅ **DONE 2026-07-27** — `!*.md` in `.gitignore`; all nine design docs are tracked (commit `3128852`).
6. ✅ **DONE** — the work lives on branch `experimental`; `main` is the known-good fallback.

---

## 9. Resume here

**Next action (2026-07-27): BUILD.** Phase 0.5's four tests and the Phase 1 skeleton header are
committed and unverified. Nothing else should be written until they compile and the tests are green
— writing unverified code on top of unverified code is the exact pattern `AUDIT §P3` documents.

After the build, in order: (1) fix whatever the tests report, (2) answer the five questions in
`OPSTACK-DECOMPOSITION.md §11` — especially the SDF channel, which changes the contract and is
cheapest to decide before any port, (3) port `Maze` per `OPSTACK-DECOMPOSITION.md §4`.

`AUDIT C1` (unbounded `SeedF`) is deliberately still open — see the progress log for why it was held
back rather than forgotten.

When picking this up cold: read §0, §2 (the direction-only insight — that's what makes step 1 small),
and §4. The rest is reference. If the plan feels too big, re-read §2: **Phase 1 needs no numeric bounds
at all**, and the first real win is two archetypes collapsing into one.

---

*Changelog — 2026-07-26: written by Opus 5 after a full-tree audit, at Jahni's request, as a durable
handoff so a future context doesn't re-derive it. Companion to `AUDIT-2026-07.md`.*
