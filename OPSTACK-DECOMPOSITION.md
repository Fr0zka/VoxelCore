# VoxelForge — the decomposition map

> **What this is:** every one of the 8 archetypes, read line by line, broken into the four roles of
> `OPSTACK-PLAN.md §2.5`, with each existing param traced to the op that will own it. Written
> 2026-07-27 so no later port has to re-derive it.
>
> **Read `OPSTACK-PLAN.md §2.5` first.** The whole point is that an archetype becomes
> `source + combiners + modifiers`, not one opaque op. If a port here collapses into a single
> `FMazeOp`, the refactor has failed its own test.
>
> **Status:** analysis only. No op exists. `Public/VoxelDensityOp.h` (the contract) is written; the
> `switch` in `GetDensityAt` is untouched.

---

## 0. What fell out — read this before the per-archetype sections

Three findings changed how I'd sequence the work. They are the reason this document is worth its
length.

### 0.1 ⚠️ The contract needs an SDF channel, not just a density channel

`IVoxelDensityOp::Eval` returns *density*. But look at what TunnelNetwork actually does:

```
CaveSDF = EvaluateSDFCached(rooms + tunnels)          // SDF space
CaveSDF = SmoothMin(CaveSDF, PitSDF,     Pit.BlendK)  // SDF space
CaveSDF = SmoothMin(CaveSDF, ChimneySDF, Chim.BlendK) // SDF space
→ ONE carve at the end: Density -= CarveFactor · BaseDensity · 2
```

Maze, VerticalShafts and FloatingIslands do the same shape: build an SDF from several primitives,
perturb the SDF with roughness noise, then convert once.

**If each primitive becomes a density op with its own carve, the `SmoothMin` junctions are lost** —
a pit would meet its room at a hard seam instead of the organic blend the code deliberately builds
(the comment at the pit block says exactly this: *"SmoothMin at the pit-to-room junction creates the
same organic transition as tunnel-to-room (no hard seam at PitTopZ)"*). Roughness is worse: three of
the four archetypes add noise to the **SDF**, which displaces the surface; adding the same noise to
**density** scales with the local gradient and is a different effect.

So ops need two channels: `float Density` and `float Sdf`, with an explicit `SdfToDensity` op that
converts. Sketch:

```cpp
struct FVoxelOpSample { float Density; float Sdf; };   // Sdf = FLT_MAX ⇒ "no surface nearby"
virtual void Eval(float X, float Y, float Z, FVoxelOpSample& InOut) const = 0;
```

**This is the one part of `OPSTACK-PLAN §3` I think is wrong as written, and it matters far beyond
fidelity:** SDF-space `SmoothMin` between two *different* sources is precisely how "a room graph
carved into a mountain" produces an organic junction rather than one field punching a hole in the
other. The single-channel contract can only ever overwrite. **Recommend adopting the two-channel
`Eval` before porting Maze** — it is cheaper now than after four ports.

*(Left as a recommendation, not a change: the header as committed is single-channel, and this is
Jahni's call.)*

### 0.2 ⚠️ Worm tunnels are why TunnelNetwork can never skip a tile — and the fix is a number

`AUDIT §6.2` frames "placed vs fielded" as a question about *future* 3D caves. It is already a live
cost. Worms are a pure 3D-noise threshold carve with no bounds:

```
if (WormStrength > 0 && WormThreshold > 0) { ... Density -= t · WormStrength · NetworkMask; }
```

Unbounded in space ⇒ `EffectOverBox` = `CarveOnly` **everywhere** ⇒ `AllSolid` is dead for every
tile of every strate with worms enabled. Direction alone cannot recover it.

**But the amplitude is bounded and trivially known:** `t ∈ [0,1]`, `NetworkMask ∈ [0,1]`, so the worm
op can move density toward air by at most `WormStrength`. If the stack so far is provably solid by
more than the sum of every remaining op's max carve, the box is still `AllSolid`.

So the first numeric interval bound worth writing is not the SDF Lipschitz bound the plan reaches
for — it is **a scalar amplitude cap on the fielded-noise carves**. Roughly ten lines, and it is
what unlocks deep-rock skipping for the plugin's most-used archetype. `OPSTACK-PLAN §4` defers all
numeric bounds to Phase 3; on this evidence one of them belongs in Phase 2.

### 0.3 Disturbances already have bounds that `ClassifyTile` throws away

Today: `if (D.ChasmDensity > 0) bCanSolid = false;` — strate-wide, for the whole tile.

But chasms/bridges/ridges are hash-placed on an XY lattice of known spacing and radius, and the
per-voxel code already builds the 3×3 candidate list. A box that no candidate reaches is `Identity`.
This is a **pure win available to SurfaceWorld today**, independent of everything else in this plan:
any surface strate that enables chasms currently loses `AllSolid` on every tile in it.

---

## 1. The shared primitive library

The 8 archetypes are ~6 functions in costumes (`OPSTACK-PLAN §1b`). Decomposed, here is what
actually repeats. **Reuse count is the payoff metric** — anything used once is suspicious.

### Role 1 — FIELD SOURCES

| Op | What it lays down | Used by | `IsXYPure` | `ClassifyBox` / `EffectOverBox` |
|---|---|---|---|---|
| `FConstantRockSource` | `Density = BaseDensity` (solid) | TunnelNetwork, Maze, VerticalShafts, **bedrock gaps** | ✅ trivially | `ClassifyBox` → **AllSolid**, always. Free, exact. |
| `FConstantVoidSource` | `Density = −BaseDensity` (air) | FloatingIslands | ✅ | `ClassifyBox` → **AllAir**, always. |
| `FSlabVoidSource` | floor surface + ceiling surface, `Density = −min(z−floor, ceil−z)` | FlatPlain, CrystalChamber | ❌ *see §3.1* | `ClassifyBox` by sampling floor/ceiling over the box's XY corners — **exact, cheap, and a new skip win** |
| `FHeightfieldSource` | `max(TerrainZ − z, z − CeilSurf)` | SurfaceWorld | ✅ (the column) | `ClassifyBox` by sampling columns on the mesher's exact lattice — **this is today's `TestColumn`, moved verbatim** |
| `FRoomGraphSource` | rooms + tunnels (+ pits + chimneys) SDF | TunnelNetwork | ❌ | `Identity` when no room/tunnel bound reaches the box — **`FCachedRoom`/`FCachedTunnel` already carry the bounds** |
| `FLatticeCorridorSource` | 3D lattice capsule corridors | Maze | ❌ | `Identity` when no open edge's capsule bound reaches the box |
| `FShaftFieldSource` | vertical cylinders + horizontal connectors | VerticalShafts | ❌ (cylinders are XY-pure; connectors are not) | `Identity` when no shaft cell reaches the box in XY |
| `FIslandBlobSource` | tapered flat-topped blobs | FloatingIslands | ❌ | `Identity` when no island bound reaches the box |
| `FWormFieldSource` | 3D-noise threshold carve | TunnelNetwork | ❌ | **`CarveOnly` always** — see §0.2. The one unbounded source. |

Note how much `Identity` is available and unused: **six of nine sources can prove themselves absent
from most of the volume**, and today none of them do.

### Role 2 — COMBINERS

`Replace` · `Union`(min) · `Subtract`(max) · `SmoothUnion`/`SmoothSubtract` (reuse
`VoxelSDF::SmoothMin/Max`) · `Add` · `Mask`. Plus, from §0.1, the conversion op:

| Op | Meaning |
|---|---|
| `FSdfCarve(blend)` | `SDF < blend` ⇒ `Density -= smoothstep(...)·BaseDensity·2`. **The exact same six lines appear in TunnelNetwork, Maze and VerticalShafts.** |
| `FSdfFill(blend)` | the `+=` mirror. FloatingIslands. |

That one op deduplicates three copies of the carve formula and one of the fill.

### Role 3 — DETAIL MODIFIERS

Everything here is gated on being near a surface, and everything here already exists.

| Op | Space | Used by | Notes |
|---|---|---|---|
| `FSurfaceRoughnessMod` | **SDF** *or* **density** — two variants, see below | all 4 SDF archetypes | the single biggest reuse in the plugin |
| `FTerraceMod` (cave) | density | TunnelNetwork | SDF-gradient orientation gate (2 extra SDF evals) |
| `FLayerLineMod` | density | TunnelNetwork | sine along Z, cubed |
| `FRibbingMod` | density | TunnelNetwork | sine along Z, squared, `+=` |
| `FCaveOverhangMod` | density | TunnelNetwork | low-Z-frequency fBm, positive lobe only |
| `FCaveCliffMod` | density | TunnelNetwork | noise-modulated vertical gradient |
| `FScallopMod` | density | TunnelNetwork | cellular noise |
| `FArchMod` | density | TunnelNetwork | room-relative |
| `FRoomColumnMod` | density | TunnelNetwork | room-relative, pre-baked in `BuildChunkCache` |
| `FDomeMod` | density | TunnelNetwork | room-relative |
| `FPinchMod` | density | TunnelNetwork | room-relative |
| `FFloorBiasMod` | density | TunnelNetwork | only inside cave air |
| `FPitMod` / `FChimneyMod` | **SDF** | TunnelNetwork | `SmoothMin`'d into the cave SDF — §0.1's motivating case |
| `FGridColumnMod` | density | FlatPlain, CrystalChamber | world-grid cylinders — **different op** from `FRoomColumnMod` |
| `FShaftLedgeMod` | density | VerticalShafts | banded shelves, half-sided |
| `FChasmMod` / `FBridgeMod` / `FRidgeMod` | density (MC) | **all archetypes** (disturbances) | see §0.3 |

**The two roughness variants, because this is the trap:**

- **SDF variant** (Maze, VerticalShafts, FloatingIslands): `Sdf += fBm(x·k, y·k, z·k)·SCALE·Rough`.
  Raw, no fade, no clamp, fixed frequency baked into the call site (`0.12`, `0.1`, `0.08`).
- **Density variant** (TunnelNetwork): two octave sets (main + fine×3), optional domain warp,
  four noise types, a `min(…, 0)` clamp so roughness can never re-fill definite air, and a
  quadratic fade by distance from surface.

They are *not* the same op with different params. Port them as one op with a `Space` enum and let
the SDF variant's fixed frequencies become real params — that alone is a small authoring win, and
`OPSTACK-PLAN §2.6` explicitly permits the re-tune.

### Role 4 — STRUCTURAL POST (fixed order, appended by the compiler, never author-omittable)

| # | Op | Today | Effect over box |
|---|---|---|---|
| 1 | `FOriginSpineOp` | `ApplyOriginSpine` | `CarveOnly`; `Identity` when the XY circle (R + 3) misses the box, or Z is outside the interior |
| 2 | `FBoundarySealOp` | `ApplyBoundarySeal` | **`ClassifyBox` → AllSolid inside its band** (forcing — see `VoxelDensityOp.h`); `Identity` when the box misses both bands |
| 3 | `FPassageCarveOp` | `ApplyPassageCarving` | `CarveOnly`; `Identity` via `AnyPassageNearBox` — **already written** |
| 4 | `FDiffLayerOp` | the diff block in `GetDensityAt` | `Both` when mods intersect; `Identity` via `HasAnyModInChunkRange` — **already written** |

The order is load-bearing and is the order the code already uses: spine carves the interior only,
the seal then re-solidifies its bands (the spine deliberately never touches them), passages punch
through everything including the seal, and the player wins last.

### A fifth thing the plan doesn't name: FRAME OPS

Two archetypes transform the *query coordinates* rather than the field:

- `VerticalScale` — `EffectiveZ = WorldZ / VerticalScale` (TunnelNetwork), stretches everything below it.
- `CaveWarpStrength/Frequency` — domain-warps the coords the SDF is evaluated at, *but not* the
  coords roughness/terrain-ops/columns use. The comment is emphatic about why: *"terracing stays
  horizontal, columns stay vertical"*.

So a frame op has **scope** — it applies to some ops below it and not others. Modelling that as
"push frame / pop frame" markers in the tape is straightforward; modelling it as a per-op flag is
not, because the same op can appear inside and outside a frame. **Decide this before the tape
format is fixed.** It also has a `EffectOverBox` consequence: any op under a warp frame must inflate
its box by the warp amplitude before answering — the existing code already does exactly this
(`Expansion = CaveWarpStrength + 2`).

---

## 2. TunnelNetwork  *(and Underwater — identical rock, a water flag)*

`GetDensityWithParams`, ~1080 lines, the biggest single function in the plugin. **Port LAST** — it
owns `BuildChunkCache`'s two-region window-invariance discipline (§8.4), the most delicate code here.

```
FRAME  VerticalScale                          (Z pre-divide, wraps everything below)
  ├─ FConstantRockSource            Replace    BaseDensity
  ├─ FRAME CaveWarp                            (SDF queries only — NOT the ops below the frame)
  │    ├─ FRoomGraphSource          → Sdf      rooms + tunnels, cached per chunk
  │    ├─ FPitMod                   → Sdf      SmoothMin, unwarped coords ⚠️
  │    └─ FChimneyMod               → Sdf      SmoothMin, unwarped coords ⚠️
  ├─ FSdfCarve(SDFBlendRadius)      Subtract
  ├─ [gate: bNearCaveSurface = Sdf < SDFBlendRadius·3]
  │    ├─ FSurfaceRoughnessMod      Add        density-space variant
  │    ├─ ── per-room op override ──           ⚠️ see below
  │    ├─ FTerraceMod               Add
  │    ├─ FLayerLineMod             Subtract
  │    ├─ FRibbingMod               Add
  │    ├─ FCaveOverhangMod          Add
  │    ├─ FCaveCliffMod             Add
  │    ├─ FScallopMod               Subtract
  │    ├─ FArchMod                  Add
  │    ├─ FRoomColumnMod            Add
  │    ├─ FDomeMod                  Subtract
  │    ├─ FPinchMod                 Add
  │    └─ FFloorBiasMod             Add
  ├─ FWormFieldSource               Subtract   ⚠️ ungated, unbounded — §0.2
  └─ [structural post ×4]
```

⚠️ **Pits and chimneys are evaluated at UNWARPED coordinates while rooms are evaluated at warped
ones**, and then `SmoothMin`'d together. The comment justifies it (pit anchors come from unwarped
room centres). Under a frame model this means pits/chimneys must sit *outside* the warp frame while
still writing the same SDF channel. That is expressible, but it is the single fiddliest thing in the
whole decomposition — **budget for it and do not discover it during the port.**

⚠️ **The per-room terrain-op override has no clean home.** Today: `NearestRoomIdx` picks a room,
that room's hash-rolled `UVoxelTerrainOpDefinition` is applied onto a *copy of the whole param
struct*, and the copy shadows `Params` for the rest of the function. In an op stack there is no
"whole param struct" to overwrite. Two options:

- **(a) Scope by room.** Each modifier gains an optional "only inside room N's influence" predicate,
  and the room-graph source publishes the nearest-room index per voxel as stack state. Faithful,
  and it generalises to "this op only inside this region", which is the `Mask` combiner already in
  the plan.
- **(b) Drop per-room ops**, make modifiers strate-wide, and recover variety with `Mask` on a
  hash field. Much simpler, visibly different world.

**(a) is right** — per-room variety is a real feature and (b) would flatten it — but it is the piece
that could blow the Phase-1 timebox if attempted early. It is also the *only* consumer of
`NearestRoomIdx`, so it can be deferred: port TunnelNetwork's geometry first with strate-wide ops,
add room scoping after.

**Tile-skipping prize:** currently zero. After the port, with §0.2's amplitude bound and the room
bounds already in `FCachedRoom`, deep bedrock below a tunnel network becomes provably `AllSolid`.
This is the largest single perf item in the whole plan.

---

## 3. FlatPlain and CrystalChamber

**These are one op with two default sets** — `OPSTACK-PLAN §4` calls this the first real win, and
reading the code confirms it: `GetSlabDensity` is called for both types with no branch on which.
CrystalChamber is FlatPlain with a bigger `CeilingRoughness`.

```
FSlabVoidSource                Replace    floor surface + ceiling surface → void field
FGridColumnMod                 Add        world-grid jittered cylinders, infinite height
[structural post ×4]
```

That is the entire archetype. Two of the eight collapse into one, and the ceiling's `abs(noise)`
(formations hang down only, never punch up) is a two-line flag on the source.

> **Observed in-editor 2026-07-27, and it settles the question:** Jahni reports FlatPlain and
> CrystalChamber render **identical** in the live world. They should — they share
> `FSlabGenerationParams`, and nothing in the content sets them apart. **The enum promised a
> difference the data never delivered**, in the shipped world as well as in the test fixture.
> So the merge does not lose a distinction; it *reveals* that there was none. Making CrystalChamber
> look like a crystal chamber is a **params** job — raise `CeilingRoughness` (6 → ~20, what
> `SlabEquivalence`'s tuned pass uses) and drop `CeilingRelativeHeight` a little. That is authoring,
> which is exactly the outcome the whole refactor is aiming at.

### 3.1 ✅ RESOLVED 2026-07-27 — Jahni: the Z term can go. Removed.

**Decision:** the Z term was not intentional character. It is gone from `GetSlabDensity` (both
surfaces), `FSlabVoidSource` is XY-pure, and FlatPlain + CrystalChamber are ported and wired.

**What that bought, and what it cost:**
- `IsXYPure() == true` ⇒ the T1.a column-cache treatment becomes available generically.
- An **exact `ClassifyBox` with no sampling**: `VoxelNoise::FBM`'s contract is `[-1,1]`, so both
  surfaces live in Z bands with known bounds — a tile entirely below the floor band is provably
  solid, a tile strictly between the bands is provably air. These two archetypes proved **zero**
  tiles before. `VoxelForge.OpStack.SlabEquivalence` reports the count.
- **Cost: the world re-tunes once.** Dropping the term samples a different slice of the noise
  field, so floor and ceiling shapes change (they do not degrade). Covered by §2.6's explicit
  permission to re-tune.

The original finding, kept because it explains why the answer mattered:

`FSlabVoidSource` is **not XY-pure, and probably should be.** Both surfaces sample noise with a
small Z term:

```cpp
FloorNoise: FractalNoise3D(x·FF, y·FF, WorldZ·FF·0.05f)   // ← Z
CeilNoise:  FractalNoise3D(x·CF, y·CF, WorldZ·CF·0.08f)   // ← Z
```

A "floor surface height" that depends on the altitude you sample it from is geometrically odd — the
floor is at a different height depending on which voxel asks. In practice the coefficient is tiny so
it reads as a subtle vertical smear rather than a bug, and it is deterministic, so nothing is broken.
But it blocks the T1.a column-cache treatment and it makes an exact `ClassifyBox` more awkward
(the surface must be sampled per Z rather than per column).

**Question for Jahni: was the `·0.05f` Z term intentional character, or a leftover from copying the
3D-noise call signature?** If it can go, `FSlabVoidSource` becomes XY-pure, gets the column cache
for free, and gets an exact box classification — which means FlatPlain and CrystalChamber start
skipping trivial tiles, which they never have. That is a large win for a one-character change, so
it is worth asking rather than assuming either way.

**Answered: it can go.** See the resolution above.

---

## 4. Maze — **the Phase 1 port**

The plan picks Maze, and the code justifies the pick completely. ~100 lines, and it decomposes
without any of the awkwardness elsewhere.

```
FConstantRockSource            Replace    BaseDensity
FLatticeCorridorSource         → Sdf      capsules over open lattice edges
FSurfaceRoughnessMod           → Sdf      SDF-space variant, frequency 0.12 (hardcoded today)
FSdfCarve(blend = 2.0)         Subtract
[structural post ×4]
```

**Why it is the right first port, beyond size:** edge identity is `hash(lower node, axis)`, so two
adjacent chunks *cannot* disagree. No `BuildChunkCache`, no COLLECT/STORE region, no window-invariance
risk at all (`AUDIT §6.4` names this the pattern to prefer). If the source/modifier split does not
fall out here, it will not fall out anywhere, and that is exactly what the stop-trigger is for.

**`EffectOverBox`:** the open-edge set reachable from a box is the same `{-1,0}³` node sweep the
per-voxel code already does, at cell granularity. Capsule bound = `CorridorRadius + roughness
amplitude + blend`. `Identity` when no open edge's capsule reaches the box — which for a sparse
`BranchProbability` is most of the volume. **Maze currently skips zero tiles; this is its first.**

**The proof `OPSTACK-PLAN §4` asks for:** once `FLatticeCorridorSource` exists, drop it into a
`SurfaceWorld` strate under the terrain and confirm you get a maze inside a mountain with no C++.
Note this requires §0.1's SDF channel to look *good* (smooth junctions where corridors meet rock);
it works but reads harsh without it.

---

## 5. SurfaceWorld — biggest payoff, most care

Three XY-pure functions plus a cheap per-voxel combine. The column cache (T1.a) and the exact-lattice
`ClassifyTile` bound must both survive the port — they are the two most valuable pieces of
engineering in the file.

```
FHeightfieldSource             Replace    ← the whole column pipeline, XY-pure
  ├─ FStructuralHeightField                continents + mountains + detail, under a warp frame
  ├─ FCliffHeightMod                       slope-gated steepening (4 structural resamples)
  ├─ FTerraceHeightMod                     relief-gated plateaus
  ├─ FLayerLineHeightMod                   sine bands
  └─ FBeachHeightMod                       flatten toward the water line
FSkyCapSource                  Subtract   ceiling: warp + signed swell + downward-only hang
FOverhangShelfMod              Union      ⚠️ per-voxel, NOT XY-pure — the one 3D op here
[structural post ×4]
```

> ### ⚠️ RESOLVED 2026-07-27 — the height ops needed a SECOND OP FAMILY, not a sub-list
>
> This section says the height ops *"operate on Z values in the column, not on density"* and then
> lists them as children of `FHeightfieldSource`. Writing them made the consequence unavoidable:
> **they do not fit `IVoxelDensityOp` at all.** Its signature is `Eval(x, y, z, FVoxelOpSample&)` —
> per voxel, density + SDF. A height op has **no input Z** (it produces one), is XY-pure (once per
> column), and writes neither channel.
>
> The two ways to force it were both bad: a per-voxel third channel for what is a **column**
> property, or collapsing all five into one opaque op — `OPSTACK-PLAN §2.5`'s explicit failure mode.
>
> **So height space got its own contract: `VoxelHeightOp.h`** (`FVoxelHeightSample` with
> `Height` + `Relief`, `IVoxelHeightOp`, `FVoxelHeightStack`). Same lesson as `§0.1`, one step
> further: §0.1 found that density needed a second *channel*; this found that terrain needs a second
> *space*. Verified by `VoxelForge.OpStack.SurfaceHeightEquivalence` before anything was built on
> top of it — deliberately, so a wrong answer would have cost one test rather than a whole port.
>
> **Bonus the type system gives for free:** a height stack cannot contain Z-dependent data, because
> there is no Z in the signature to put there. `AUDIT §6.3` warns that Z-dependent data smuggled into
> `FSurfaceColumn` silently corrupts every chunk in the vertical stack and that `ValidateDeterminism`
> would not catch it. Here the *type* forbids it rather than a convention.

**Critical distinction the port must preserve:** the height ops (`FCliffHeightMod` and friends)
operate on **Z values in the column**, not on density. They are XY-pure and belong in
`PrepareChunk`/the column cache. `FOverhangShelfMod` operates per voxel and re-samples the
structural heightfield at a shifted XY. Mixing those two up puts Z-dependent data in `FSurfaceColumn`,
which `AUDIT §6.3` warns silently corrupts every chunk in the vertical stack — **and
`ValidateDeterminism`, which samples along an X boundary, would not catch it.**

This is the archetype where `IsXYPure()` earns its place in the contract: it turns an implicit
convention that has to be remembered into a declaration the compiler routes on.

**Biome blending:** the heightfield is evaluated for the dominant biome and its nearest neighbour and
the two *heights* are lerped. In stack terms that is the `Mask` combiner with a biome-weight field —
which is exactly the mechanism `OPSTACK-PLAN §4 Phase 3` wants for unifying strates and biomes. So
SurfaceWorld's existing biome blend is the prototype for the whole Phase 3 idea, and porting it is
how that gets validated.

---

## 6. VerticalShafts

```
FConstantRockSource            Replace
FShaftFieldSource              → Sdf      infinite cylinders (XY-only) + hash-gated connectors
FSurfaceRoughnessMod           → Sdf      SDF variant, frequency 0.1
FSdfCarve(blend = 2.0)         Subtract
FShaftLedgeMod                 Union      banded shelves, +X/+Y half only so a climb path remains
[structural post ×4]
```

Nearly identical in shape to Maze — same `source → roughness → carve` spine, different primitive.
That similarity is the evidence the abstraction is real: two archetypes that look unrelated in the
`switch` are the same three ops with a different source.

**Split worth making:** the shafts are XY-pure infinite cylinders; the connectors are not. Two ops
(`FShaftColumnSource` XY-pure + `FShaftConnectorSource`) let the cylinder half get the column-cache
treatment and answer `ClassifyBox` exactly in XY. Keeping them as one op forfeits that.

---

## 7. FloatingIslands

The only archetype whose source is **air**, which is what makes it a good composition test.

```
FConstantVoidSource            Replace    −BaseDensity (open void)
FRAME IslandWarp                          XY domain warp (lobed outlines, amplitude ~0.35·meanR)
  └─ FIslandBlobSource         → Sdf      tapered flat-top blobs, SmoothMin'd together
FSurfaceRoughnessMod           → Sdf      SDF variant, frequency 0.08, 4 octaves
FSdfFill(SDFBlendRadius)       Union
[structural post ×4]
```

Note the warp here is applied to the **query** (`WX`,`WY` computed once per voxel and shared by all
nearby islands) exactly like TunnelNetwork's cave warp — same frame concept, third instance. Three
uses is enough to make frames a first-class part of the model rather than a special case.

**`FIslandBlobSource` is the cleanest `Identity` opportunity in the plugin:** islands are hash-placed
with a known XY radius and explicit `TopZ`/`BotZ`. A box that no island's AABB reaches is provably
untouched — and since a floating-island strate is *mostly* empty void, that is most tiles. Combined
with `FConstantVoidSource`'s `ClassifyBox → AllAir`, a FloatingIslands strate could go from skipping
zero tiles to skipping the large majority of them.

---

## 8. Underwater

`GetDensityAt` routes `Underwater` to `GetDensityWithParams` with a comment: *"Underwater shares
tunnel rock (water table is a render-side overlay)."* There is **no density difference at all**.

So `Underwater` is not an archetype, it is TunnelNetwork plus `WaterLevelRelative` consumed by the
water render system. When `ECaveGeneratorType` finally disappears, this one vanishes for free — it
never needed to exist as a generator type.

---

## 9. Q2 — the param audit: every field, and who claims it

`FStrateGenerationParams` via the `VF_STRATE_PARAM_FIELDS` X-macro. **Every field is claimed except
where flagged.**

| Field(s) | Destination op |
|---|---|
| `BaseDensity` | **stack-global** — read by every source and all four structural-post ops. Not any single op's param. |
| `VerticalScale` | `FRAME VerticalScale` |
| `WormFrequency`, `WormHorizontalBias`, `WormThreshold`, `WormStrength`, `WormNetworkRange` | `FWormFieldSource` |
| `RoomSpacing`, `RoomDensity`, `MinRoomRadius`, `MaxRoomRadius`, `RoomHeightRatio`, `RoomShapeVariety`, `RoomFloorCutMin`, `RoomFloorCutMax`, `FloorReliefStrength`, `FloorReliefFrequency` | `FRoomGraphSource` |
| `OriginRoomRadius`, `OriginRoomMaxConnections` | `FRoomGraphSource` — ⚠️ but see §10.3, they couple to the spine |
| `TunnelMinRadius`, `TunnelMaxRadius`, `TunnelDensity`, `MaxTunnelLength`, `TunnelWarpStrength`, `TunnelHorizontalBias`, `bTunnelsFlowTowardOrigin`, `TunnelEndpointZOffset` | `FRoomGraphSource` |
| `SDFBlendRadius` | `FSdfCarve` / `FSdfFill` — **shared**, also the `bNearCaveSurface` gate width |
| `CaveWarpStrength`, `CaveWarpFrequency` | `FRAME CaveWarp` |
| `SurfaceRoughness`, `RoughnessFrequency`, `RoughnessNoiseType`, `DomainWarpStrength`, `DomainWarpFrequency` | `FSurfaceRoughnessMod` (density variant) |
| `BoundarySealThickness` | `FBoundarySealOp` — and read by the spine, disturbances, shaft connectors, island spread |
| `StrateTopWorldZ`, `StrateBottomWorldZ` | **`FVoxelOpContext`, not op params.** Runtime-injected, never author-set. |
| `FloorBias` | `FFloorBiasMod` |
| `TerraceStepHeight`, `TerraceHardness`, `TerraceNoiseDisplacement` | `FTerraceMod` |
| `LayerLineSpacing`, `LayerLineDepth` | `FLayerLineMod` |
| `OverhangStrength`, `OverhangDepth`, `OverhangFrequency` | `FCaveOverhangMod` ⚠️ name collision, §9.1 |
| `RibbingSpacing`, `RibbingDepth` | `FRibbingMod` |
| `CliffStrength` | `FCaveCliffMod` ⚠️ name collision, §9.1 |
| `ScallopStrength`, `ScallopFrequency` | `FScallopMod` |
| `ArchDensity`, `ArchMinRadius`, `ArchMaxRadius` | `FArchMod` |
| `ColumnDensity`, `ColumnMinRadius`, `ColumnMaxRadius` | `FRoomColumnMod` ⚠️ name collision, §9.1 |
| `PitDensity`, `PitMinRadius`, `PitMaxRadius`, `PitDepth` | `FPitMod` |
| `ChimneyDensity`, `ChimneyMinRadius`, `ChimneyMaxRadius`, `ChimneyHeight` | `FChimneyMod` |
| `DomeDensity`, `DomeMinRadius`, `DomeMaxRadius`, `DomeHeightRatio` | `FDomeMod` |
| `PinchDensity`, `PinchStrength`, `PinchLength` | `FPinchMod` |
| **`WaterLevelRelative`** | ⚠️ **claimed by no density op.** See §9.2. |

### 9.1 Three names mean different things in different structs

`FStrateGenerationParams` and `FSurfaceGenerationParams` both define `CliffStrength`,
`OverhangStrength`/`OverhangFrequency`, `TerraceHardness` and `ColumnDensity`-family fields — and
they are **genuinely different operations**:

| Name | in `FStrateGenerationParams` (cave) | in `FSurfaceGenerationParams` (surface) |
|---|---|---|
| `CliffStrength` | noise-modulated vertical density gradient near a cave wall | slope-gated steepening of a **height value** |
| `OverhangStrength` | fBm lobe adding rock into a cave | F20 warped-terrain union making a real 3D shelf |
| `TerraceHardness` | staircase edge width on a cave wall | plateau/riser ratio of a **height** quantiser |
| `ColumnDensity` | room-anchored columns | *(slab struct)* world-grid cylinders |

Today the type system keeps them apart. Once ops are data assets in one list, **nothing does** —
`DA_Op_Cliff` would be ambiguous. Name them for what they operate on from day one:
`FCaveWallCliffMod` vs `FHeightSlopeCliffMod`, `FCaveShelfMod` vs `FTerrainOverhangMod`. Cheap now,
a rename with authored assets in the field later.

### 9.2 The one unclaimed field: `WaterLevelRelative`

It lives in `FStrateGenerationParams` (and is `Lerp`'d at every strate boundary along with the
density params), but **nothing in the density path reads it.** Its consumers are
`GetWaterLevelWorldZForChunk` (the water render system) and `ComputeSurfaceTerrainZ`'s beach
flattening — which reads the *surface* struct's copy, not this one.

Not dead, but misfiled: it is a *content/render* property riding in a *density* struct, and it is
being interpolated across strate boundaries where a water plane should probably be a hard property
of one strate. **Report, don't delete** — but when ops become assets it should move to the strate
itself rather than to any op.

### 9.3 Fields that are context, not params

`StrateTopWorldZ` / `StrateBottomWorldZ` are runtime-injected by
`UVoxelStrateManager::Get*ParamsForChunk`, never author-set, and every archetype's first line is a
degenerate check on them. They belong in `FVoxelOpContext` (where the header already puts them) and
should be *removed* from the per-op param structs so an author cannot see or set them. That deletes
a whole class of "I set the Z bounds and nothing happened".

---

## 10. Ordering rules the compiler must enforce

### 10.1 Structural post is appended, always, in order
`FOriginSpineOp` → `FBoundarySealOp` → `FPassageCarveOp` → `FDiffLayerOp`. Verified identical in all
six density functions. An author cannot omit, reorder or insert between them.

### 10.2 Disturbances run after the archetype, before the diff layer
`ApplyDisturbances` is called in `GetDensityAt` *after* the archetype function returns (so after that
function's own spine/seal/passages) and *before* the diff layer. So the true global order is:

```
[archetype stack] → spine → seal → passages → disturbances → diff layer
```

Disturbances self-limit to the seal interior (`if (Z <= InnerBot || Z >= InnerTop) return;`), which
is why running them after the seal is safe. **Preserve this or bridges will punch through seals.**

### 10.3 The spine and the origin room are two systems aimed at the same place
`ApplyOriginSpine` carves an unconditional column at XY (0,0) in every strate; `OriginRoomRadius`
makes the room graph put a big room there too; `bOpenSurfaceEntry` opens a shaft from above. Three
mechanisms, one location, and `AUDIT C8` already flags an unchecked invariant between
`OriginRoomRadius` and the COLLECT margin. When these become ops, the coupling becomes visible and
should be either unified or documented — right now it works by everyone independently agreeing that
(0,0) is special.

### 10.4 Recommended port order (unchanged from the plan, now with reasons from the code)

1. **Maze** — cleanest split, no connectivity decision, cheapest mistake. §4.
2. **FlatPlain + CrystalChamber** — two archetypes → one op. Ask §3.1 first.
3. **FloatingIslands** — the biggest `Identity` win, and the first air-source stack.
4. **VerticalShafts** — same spine as Maze, validates the source-swap claim.
5. **SurfaceWorld** — biggest payoff; the column cache and exact-lattice bound must survive.
6. **TunnelNetwork** — last. §8.4, the warp/pit coordinate split, and the per-room op override.
7. **Underwater** — falls out of 6 for free.

---

## 11. Open questions for Jahni

Ranked by how much they change the work.

1. **Two-channel `Eval` (density + SDF)?** §0.1. Changes the contract. Cheapest to decide now.
   My recommendation: yes — without it, cross-source `SmoothMin` is impossible and "a maze inside a
   mountain" reads as a hole punched in rock rather than a cave that belongs there.
2. **Is `FSlabVoidSource`'s Z-term intentional?** §3.1. One character; unlocks XY-purity, the column
   cache and exact tile classification for two archetypes.
3. **Per-room terrain ops: scope-by-room (faithful) or strate-wide + `Mask` (simpler, flatter)?**
   §2. Recommend scope-by-room, but *after* the geometry port, not during it.
4. **Amplitude bound on the worm carve in Phase 2 rather than Phase 3?** §0.2. It is the difference
   between TunnelNetwork skipping tiles and never skipping tiles.
5. **Rename the colliding cave/surface op names before any asset is authored?** §9.1.

---

*Written 2026-07-27 by Opus 5, unattended, as build-free queue item Q1. Companion to
`OPSTACK-PLAN.md` (the plan) and `OPSTACK-PROGRESS.md` (what is actually built).*
