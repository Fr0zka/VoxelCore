# COMPOSER-NOTES.md — the world composer

**What this is:** the design of the system that *invents* the depths, plus every code finding and
hazard that constrains it. Engineering document.

**What this is NOT:** the game. The premise, the city, the economy, death, combat, seasons and the
community layer live in **[GDD.md](GDD.md)**. Where this file needs one of those, it states the
*constraint* and points there for the reasoning. Do not restate game design here — duplicated
documents drift, and then neither is authoritative.

**History:** this file was a chronological transcript of the 2026-08-17 design conversation, ~1400
lines, game design and generator design tangled together. It was distilled on the same day. **The full
original transcript is in git at commit `b781e3b`** — go there for the reasoning behind anything that
reads as bald assertion here.

---

## 1. The vision

**The system must INVENT strates and worlds** from Jahni's tagged raw content plus a seed.

⚠️ **Read this correction; it is the whole point.** The first reading of the vision was that Jahni
authors *strates* and the system arranges them. **Wrong.** His words:

> *"I'm not pretty happy to make strates forever, that's actually the opposite. If I could never make
> any new strate myself and have it do it itself would be genius."*

**The floor of authoring is RAW CONTENT + TAGS** — operators, meshes, materials, sounds, creatures,
music — tagged, with **multiple fitting tags each**. The overlaps between tags are where the system
finds combinations he never planned that still cohere; single-tag buckets cannot do that.

Everything above that floor is **composed and measured**: strates, layouts, danger, rarity, names.

**A strate becomes generated data, not an asset.** `UVoxelStrateDefinition` survives as a struct the
composer fills in; nobody opens it in the editor any more.

### This is op-stack "Phase 3", and it is REOPENED

The 2026-08-16 `OPSTACK-HANDOFF.md` says in bold *"Do not start Phase 3."* **That is superseded.**
Asked what the world was missing, Jahni described exactly Phase 3 plus a composer above it.

It is **not** a restart of the refactor. The eight operators are live, composable, and production. What
never happened is anything *composing* them except a port of the old archetype switch. **The machine
was built and never fed.**

### Ambition level

The seed **invents parameters within authored ranges** — not preset-picking. Two seeds differ in
everything: not the same strates spawn, nor the same details.

---

## 2. What the game requires of the generator

Constraints only. See [GDD.md](GDD.md) for why each exists.

| requirement | source |
|---|---|
| **Every strate has an entrance and an exit, exposed to air, and findable.** The one invariant that may never fail. | GDD §7 |
| **Danger = depth × per-strate multiplier**, rare exceptions in both directions. Drives creature, hazard and loot selection — **never** what kind of place a strate is. | GDD §7 |
| **Danger must never scale generation parameters.** It selects content and may bias which terrain ops are drawn. If depth multiplied a density parameter, strate 80 would be geometrically broken rather than hard. | GDD §9 |
| **Anomalous strates are rare and must NOT blend** with their neighbours. `EVoxelStrateTransition::Hard` already expresses this. | GDD §7 |
| **A share of grounded, plausible strates is guaranteed** — strangeness needs ordinary to be strange against. | GDD §7 |
| **Fightable space is a generation guarantee, not a hope.** Boss arena ≈ 3–4 chunks across, ordinary fight space ≈ 2, camera headroom ≈ 1 chunk (1 chunk = 32 voxels × 25 cm = 8 m). Spacious *and vertical* is the target. | GDD §12 |
| **Essential materials need a placement DENSITY, not a location** — "somewhere in a gigantic strate" is unfindable. Optional materials may be absent. | GDD §9 |
| **The depths are bounded** ⇒ a whole strate can be examined, so the entrance/exit law can be **proved** rather than sampled. | GDD §6 |
| **No bottom** ⇒ generate a deep buffer (~30 strates) offline and extend in a later batch. | GDD §6 |
| **Boss strates every ~5 slots** ⇒ the layout needs a **periodic** fixed-slot rule; `Settings->FixedStrates` is a map of absolute indices today. | GDD §6 |
| **A season freezes generation; a wipe frees it.** The composer may ship imperfect and improve every cycle. | GDD §13 |
| **The manifest must support names, quests, fast travel, material lookup and death locations.** | GDD §7, §10, §14 |

---

## 3. The composer

### 3.1 It runs ONCE PER SEASON, OFFLINE — this sets the whole budget

Everyone shares one seed per season, so there is exactly **one set of depths per season, ever**. The
composer never runs on a player's machine and never at load. It runs once, on a build box.

**The problem is not "fast enough for a loading screen" — it is "runs overnight."** So it can generate a
thousand candidate strates, measure them all, keep the best, validate at any resolution, and later run
a model over the candidates at zero runtime cost. **And Jahni reviews and vetoes the season before it
publishes** — the system does the work, he keeps the veto.

**What ships is small:** the season's seed plus the chosen strates' parameter vectors. Clients generate
terrain from those, deterministically, exactly as today. **No runtime composition anywhere.**

### 3.2 Structure: how a stack gets assembled

The current shipping builders, for reference (structural posts included; TunnelNetwork's builder is
also the Underwater builder):

```
TunnelNetwork / Underwater (20)  rock source -> room graph (SDF) -> SdfCarve -> 12 density modifiers -> worm source -> structural
FlatPlain / CrystalChamber (6)   slab-void source -> grid columns -> structural
Maze (8)                          rock source -> lattice corridors (SDF) -> SdfRoughness (SDF) -> SdfCarve -> structural
SurfaceWorld (6)                  surface column -> overhang -> structural
VerticalShafts (9)                rock source -> shaft field (SDF) -> SdfRoughness (SDF) -> SdfCarve -> ledges -> structural
FloatingIslands (8)                VOID source -> island blobs (SDF) -> SdfRoughness (SDF) -> SdfFill -> structural
```

**⭐ Most ordering is MECHANICAL — derive it, do not author it.** Roughness sits on the *SDF* before
conversion in Maze and on *density* after conversion in TunnelNetwork; CODEMAP confirms they are two
different ops for exactly that reason. An op's legal position is fixed by **which channel it reads and
which it writes**.
⇒ **Every operator declares its channel reads/writes.** Dependencies form a DAG and **any topological
sort of that DAG is a legal stack** — no authored order, and it produces orderings nobody wrote down.

**Built status — 2026-09-04:** `EVoxelOpChannel` now names the two fields actually carried by
`FVoxelOpSample` (`Density` and `Sdf`); all 29 concrete density operators declare reads, writes, and
additive/transformative behavior. `FVoxelOpStack::Add` snapshots that metadata, and
`ValidateChannelOrder` checks the resulting channel DAG without entering the voxel loop.
`VoxelForge.OpStack.ChannelDAG` builds all eight shipping stacks and validates them: **22 tests
succeeded, 0 failed, 0 not run**.

**⭐ Root polarity is a one-bit identity lever.** FloatingIslands is Maze's op classes rooted in a
**void** source with a **fill** instead of a rock source with a carve — and you get islands instead of
tunnels.

⛔ **"Corrective ops must be FUSED" — WITHDRAWN 2026-09-04. The premise was false.**

This section used to assert: *"`FFloorBiasMod` exists only to undo what `FCaveRoughnessMod` did to
floors... fuse them into one op with two internal phases."* **It does not, and they must not be fused.**

Read the math rather than the names. `FCaveRoughnessMod` adds its own displacement:
`D += TotalRough`. Eleven ops later, `FFloorBiasMod` computes an **independent** term
`B = NormZ² × FloorBias` from `Sdf`, room geometry and `WorldZ`, and adds it: `D += B`.
**It never reads or subtracts `TotalRough`.** It is not a correction to roughness at all — it acts on
the accumulated density and belongs late in the stack, exactly where it is.

⇒ **No fuse. No `corrects` dependency either** — there is nothing being corrected. The general
principle ("an operator whose only purpose is to correct another is not a separate operator") stands
as a rule for ops that genuinely are corrective; this simply was not an instance of it.

**Two lessons, both cheap here and expensive elsewhere:**
- The claim came from reading a *name* and a *comment*. Ten Density-writing ops sit between the two
  (`FCaveTerraceMod`, `FLayerLineMod`, `FRibbingMod`, `FCaveOverhangMod`, `FCaveCliffMod`,
  `FScallopMod`, `FCaveArchMod`, `FRoomColumnMod`, `FDomeMod`, `FPinchMod`) — a fact one grep would
  have shown before the design paragraph was written.
- The old parenthetical *"fusing with identical order and math leaves the field bit-identical"* was
  also wrong: reordering across ten accumulating ops cannot be bit-identical, because IEEE-754
  addition is not commutative. ⚠️ But do not let that argument decide anything on its own —
  **bit-identity across builds was never required** (see §8: peer determinism is *same build, all
  machines*). Had the fuse been semantically right, a slightly changed field would have been an
  acceptable price. It was the *semantics* that killed it, not the bits.

**One declaration covers the rest:** an op is **additive** (small displacement, commutes freely — shuffle
at will) or **transformative** (clamps, multiplies, gates — position matters, needs explicit placement).
“Commutes” here is algebraic: IEEE-754 float accumulation is not generally bitwise commutative, so a
shuffle still needs the equivalence gate.

**The structure roll:**
`root polarity -> shape source -> conversion (follows from polarity) -> draw k modifiers legal in the
resulting channel space (k itself rolled) -> structural post appended automatically.`
With ~5 shape sources and ~15 modifiers choosing 4–8, that is **tens of thousands of structurally
distinct stacks before a single parameter is touched** — which is what carries early variety while the
corpus is small.

**⇒ Rule: vary STRUCTURE aggressively, vary PARAMETERS conservatively.** Shift the ratio as the corpus
grows.

**Tier 4b implementation (2026-09-04):** `FVoxelOpStackRecipe` is the shipped-manifest shape: it
stores the one-bit `RootPolarity`, root/source/conversion `EVoxelStrateOpClass` IDs, an ordered list
of modifier IDs, and an `EVoxelStrateParamBlock` on every entry. The four structural posts are
deliberately not a field in the recipe. `VF_BuildStackFromRecipe` appends spine, vertical seal,
passage carving, and XY edge seal in that fixed order, so an invented strate cannot omit primordial
law. The conversion ID is checked against polarity rather than rolled independently.

The roller's modifier catalogue is a deterministic array of declared ops, not a compatibility list.
It asks each op for its channel reads/writes, additive contract, required resources, and provided
resources, then draws 4–8 unique entries from the legal remainder. The channel declarations exposed
a real missing dependency: `FCaveTerraceMod` and the other room-relative cave modifiers use the
`FRoomGraphSource` room cache through a non-owning pointer, even though their channel masks only said
`Density`. `FShaftLedgeMod` similarly consumes shaft state, and `FOverhangShelfMod` consumes the
surface-column state. `RequiredResources` / `ProvidedResources` now declare and validate those
dependencies (RoomGeometry, ShaftGeometry, SurfaceColumn); the roller cannot place such an op before
its provider. `FCaveRoughnessMod` and `FWormFieldSource` were checked and do not require room state:
they read only their declared channels and parameters.

The parameter decision is deliberately visible: structure and parameter rolls are separate. Each
candidate rolls the six native family blocks independently from the exact-archetype corpus using the
Tier 4a conservative blend/jitter procedure. An op consumes the block named on its recipe entry;
structural posts consume the shape source's family block. There is no unsafe 73-field cross-family
blend. The current `SurfaceWorld` block is still rolled and carried even when a structure has no
surface-column op, so the recipe remains a complete family-parameter record.

The focused 64-candidate run produced 64 distinct recipes, 0 invalid recipes, and 0 box-verdict
violations. It checked 650,859 voxels across 489 proved boxes. Survival was 46/64 (71.9%) under the
same non-vacuous, largest-share ≥ 0.50, exact `Connected` criteria as Tier 4a; that lower rate is an
expected consequence of aggressive structural exploration, not a tuning target. The test also
rerolls every recipe and rebuilt stack, checks `WorldRadiusVoxels == 0`, and feeds the novel stack
through the offline Tier 2 sampler; no runtime generation path calls this API. The final focused
test took **76.466 s**. The complete `VoxelForge` namespace then passed **24/24 tests (0 failed,
0 not run)** in **339.797 s**.

### 3.3 Parameters: the range problem

**Verified facts, not remembered:** `FStrateGenerationParams` has **73 scalar fields** (+1 bool) — not
hundreds. The `VF_STRATE_PARAM_FIELDS` X-macro covers **all of them** (`Alpha` is the `Lerp` function's
parameter, not a field). `VoxelStrateTypes.h` carries 135 `ClampMin` and 52 `ClampMax` across 216
UPROPERTYs, so most fields have no upper bound. Paired min/max fields exist
(`MinRoomRadius`/`MaxRoomRadius`, `TunnelMin/MaxRadius`, `RoomFloorCutMin/Max`).

**Why rolling parameters fails, stated plainly:** the coherent region of a 73-dimensional space is a
vanishingly thin sheet, and uniform sampling in a box essentially never lands on it. The problem is not
wrong ranges — **independent rolling destroys the CORRELATIONS** that make a parameter set coherent
(room spacing ↔ room radius ↔ tunnel length ↔ strate height).

**⭐ The design: do not roll parameters — BLEND KNOWN-GOOD ONES.**

1. **Corpus** = known-good parameter vectors, seeded by the existing hand-authored strate assets.
2. **Roll** = pick 2–3 parents (weighted), blend with random weights, then jitter.
3. **Reuse `FStrateGenerationParams::Lerp`** — it already does exactly this, over exactly this field
   set, and the world working already tests it. Build nothing.
4. **Constraint satisfaction is FREE:** a convex combination preserves every relation that holds in both
   parents (`Min <= Max` stays true). The correlations survive because they were never broken.
5. ⭐ **Ranges come from the CORPUS SPREAD** — per field, how much it varies across existing strates —
   **not from authored numbers, and not from the clamps.** Clamps are "don't type nonsense" limits, not
   "this makes a good world" limits. *Measure, don't declare.*
6. **Clamps are the hard safety net only**, never the roll range.

**Escaping the convex hull:** blending alone only reaches *between* existing points. Jitter slightly
past the corpus; extrapolate deliberately past a parent away from the centroid (riskier, validate
harder); and — the real one — **PROMOTION**: every invented strate that measures well and survives
players joins the corpus. Season 1 blends ~6 strates; season 5 blends 200 discovered ones.

**⚠️ Caveats:** UPROPERTY metadata is editor-only, so the clamp net must be **baked at cook time** by a
small commandlet walking the struct by reflection. Clamp coverage is partial. A few fields are not
tunables at all (`StrateTopWorldZ` / `StrateBottomWorldZ` are runtime Z bounds) and need an explicit
exclusion list. Bools cannot be blended — roll them by probability or inherit from the dominant parent.

### ✅ Tier 4a status — parameter roll built (2026-09-04)

`Public/VoxelStrateComposer.h` + `Private/VoxelStrateComposer.cpp` now implement the corpus and
the deterministic roll. `Private/Tests/VoxelForgeComposerParameterRollTest.cpp` is the offline
measurement harness. The roll uses the native family blend (`FStrateGenerationParams::Lerp` for
the tunnel family), chooses 2–3 weighted parents **inside one exact archetype group**, applies
jitter of **±15% of each field's measured corpus range (`max-min`)**, then applies reflected
`ClampMin`/`ClampMax` metadata. It does not use clamps as distribution ranges, does not touch
generation, and leaves `WorldRadiusVoxels` at its default 0. The sibling native parameter structs
are carried in the same corpus entry, so a `SurfaceWorld` vector is never blended with a
`TunnelNetwork` vector merely because both happen to be stored in a strate asset.

The original one-vector result was caused by the old loader following `DA_Settings`' fixed-strate
and pool references. An audit of those references finds **one unique path**,
`/Game/VoxelForge/DA_Strate3.DA_Strate3`, whose archetype is `TunnelNetwork`; that is why the
previous corpus only loaded one asset. The current loader instead asks the Asset Registry for every
project `UVoxelStrateDefinition`, sorts and de-duplicates the results, accepts `/Game` packages,
and ignores `Saved/Autosaves` and `Saved/Cooked` copies. The scan found **4 project assets**, with
no saved-copy, outside-project, duplicate, or unresolved records:

| Asset | Archetype |
|---|---|
| `DA_Strate1` | `SurfaceWorld` |
| `DA_Strate2` | `FloatingIslands` |
| `DA_Strate3` | `TunnelNetwork` |
| `DA_Strate4` | `TunnelNetwork` |

The hand-authored C++ defaults in `VoxelStrateTypes.h` are also corpus members: one for each of the
8 supported archetypes (`TunnelNetwork`, `FlatPlain`, `CrystalChamber`, `Maze`, `SurfaceWorld`,
`VerticalShafts`, `FloatingIslands`, `Underwater`). The fed corpus is therefore **12 members** in
**8 exact-archetype groups**. Group membership is `TunnelNetwork=3`, `FlatPlain=1`,
`CrystalChamber=1`, `Maze=1`, `SurfaceWorld=2`, `VerticalShafts=1`, `FloatingIslands=2`, and
`Underwater=1`.

The complete 253-descriptor spread table is emitted by the automation test, including sample count,
min/max/mean/standard deviation, excluded/reflected status, and clamp metadata. The 230 zero-range
rows are intentionally omitted below; these are the **23 rows with real spread** (float formatting
is the test's measured output):

| Archetype | Field | N | Min | Max | Mean | StdDev |
|---|---|---:|---:|---:|---:|---:|
| TunnelNetwork | MinRoomRadius | 3 | 10 | 25 | 15 | 7.07106781 |
| TunnelNetwork | MaxRoomRadius | 3 | 30 | 125 | 61.6666667 | 44.7834295 |
| TunnelNetwork | OriginRoomRadius | 3 | 20 | 120 | 53.3333333 | 47.1404521 |
| TunnelNetwork | TunnelMinRadius | 3 | 3 | 9 | 5 | 2.82842712 |
| TunnelNetwork | TunnelMaxRadius | 3 | 7 | 15 | 9.66666667 | 3.77123617 |
| SurfaceWorld | BaseGroundRelative | 2 | 0.200000003 | 0.300000012 | 0.250000007 | 0.0500000045 |
| SurfaceWorld | ElevationRange | 2 | 60 | 200 | 130 | 70 |
| SurfaceWorld | ContinentFrequency | 2 | 0.00100000005 | 0.00600000005 | 0.00350000005 | 0.0025 |
| SurfaceWorld | MountainStrength | 2 | 0.5 | 1 | 0.75 | 0.25 |
| SurfaceWorld | MountainFrequency | 2 | 0.00200000009 | 0.0120000001 | 0.0070000001 | 0.005 |
| SurfaceWorld | SurfaceRoughness | 2 | 2 | 3 | 2.5 | 0.5 |
| SurfaceWorld | ReliefStrength | 2 | 0 | 0.699999988 | 0.349999994 | 0.349999994 |
| SurfaceWorld | ReliefContrast | 2 | 1.60000002 | 2 | 1.80000001 | 0.199999988 |
| SurfaceWorld | TerraceHeight | 2 | 12 | 20 | 16 | 4 |
| SurfaceWorld | BeachWidth | 2 | 8 | 200 | 104 | 96 |
| FloatingIslands | IslandSpacing | 2 | 95 | 1000 | 547.5 | 452.5 |
| FloatingIslands | IslandDensity | 2 | 0.100000001 | 0.5 | 0.300000001 | 0.199999999 |
| FloatingIslands | IslandMinRadius | 2 | 18 | 50 | 34 | 16 |
| FloatingIslands | IslandMaxRadius | 2 | 42 | 500 | 271 | 229 |
| FloatingIslands | ThicknessRatio | 2 | 0.100000001 | 0.699999988 | 0.399999995 | 0.299999993 |
| FloatingIslands | VerticalJitter | 2 | 0.200000003 | 0.600000024 | 0.400000013 | 0.20000001 |
| FloatingIslands | TopFlatten | 2 | 0.600000024 | 0.800000012 | 0.700000018 | 0.099999994 |
| FloatingIslands | SurfaceRoughness | 2 | 0 | 4 | 2 | 2 |

The reflection/use audit found **36 excluded fields**: the 34 terrain-op transport fields
`TerraceStepHeight`, `TerraceHardness`, `TerraceNoiseDisplacement`, `LayerLineSpacing`,
`LayerLineDepth`, `OverhangStrength`, `OverhangDepth`, `OverhangFrequency`, `RibbingSpacing`,
`RibbingDepth`, `CliffStrength`, `ScallopStrength`, `ScallopFrequency`, `ArchDensity`,
`ArchMinRadius`, `ArchMaxRadius`, `ColumnDensity`, `ColumnMinRadius`, `ColumnMaxRadius`,
`PitDensity`, `PitMinRadius`, `PitMaxRadius`, `PitDepth`, `ChimneyDensity`, `ChimneyMinRadius`,
`ChimneyMaxRadius`, `ChimneyHeight`, `DomeDensity`, `DomeMinRadius`, `DomeMaxRadius`,
`DomeHeightRatio`, `PinchDensity`, `PinchStrength`, `PinchLength` — populated per room by
`UVoxelTerrainOpDefinition` — plus the manager-owned runtime bounds `StrateTopWorldZ` and
`StrateBottomWorldZ`. The tunnel transport fields are measured directly from the native
`FStrateGenerationParams` X-macro even though they are not reflected `UPROPERTY`s; their values are
reported but deliberately not jittered. Same-named fields in sibling structs are not automatically
excluded: the exclusion applies to the tunnel transport slots whose generation source is the
terrain-op pool. The test prints the complete field-by-field table, including excluded fields,
reflection status, and clamp metadata.

The terrain-field source is now traced in code. `VoxelStrateManager.cpp` loads the active strate's
`TerrainOperations` soft-object pool, and `VoxelGenerator.cpp` passes that pool to
`VoxelCaveMorphology::BuildChunkCache`. For each nearest room, `FVoxelTerrainOpDefinition::ApplyTo`
overlays the selected operation onto a local copy (`FStrateGenerationParams LocalTerrainParams =
Params`) before the room operator runs. Thus the 34 values are transported through the strate
params, but their authored cave-detail values come from `UVoxelTerrainOpDefinition` assets selected
by the active strate's terrain-operation pool. Rolling those slots in the strate composer is not a
useful way to roll cave detail: selected room operations overwrite them. It is not a literal
absolute no-op, because the local base copy is still used where no operation overwrites a slot.
The correct future design is a **second corpus** over terrain-op assets plus the active pool's
membership/weights; this task does not build it. The project currently has **1**
`UVoxelTerrainOpDefinition` asset (the Asset Registry count used by the test).

The bool policy is explicit: `bTunnelsFlowTowardOrigin` is inherited from the dominant parent.
`OriginRoomMaxConnections` and `RoughnessNoiseType` retain `Lerp`'s existing SNAP behavior and
are not jittered. In this editor build clamp metadata was available while the table was built; it
is **not promised in a cooked runtime**, so the commandlet/cook-time bake remains owed.

The fed 64-candidate run (step 4 sweep, radius 256, max 8,000,000 cells, fixture seed 1337)
produced **59/64 survivors (92.2%)**. There were 60 non-vacuous candidates, 60 with largest
component share ≥ 0.50, 63 exact unsnapped arrival→departure law passes, and no roll failures.
The complete 64-row table is emitted by `VoxelForge.Composer.ParameterRoll`; each row reports
archetype, weighted parents, air fraction, largest share, walkable fraction, feature scale, and
arrival→departure verdict. The current run took **114.845 s** total (**0.014 s** corpus load,
**114.827 s** roll/measurement).

The §6.2 check is no longer vacuous: **1,224 boxes proved**, **1,629,144 voxels checked**,
**0 violations**. Its verdict mix was 1,336 Mixed, 549 AllSolid, and 675 AllAir; the law check
reported no violating candidate. The box law used unit step and brute-forced every voxel of each
uniform box, while the candidate sweep retained step 4. The test asserts zero violations and keeps
the law open to future changed candidates rather than treating this clean run as a license to tune
the survival rate.

### 3.4 The measurement pass — one grid, one flood fill, three jobs

Sample a candidate strate into a coarse voxel grid, flood-fill the air **once**, derive everything from
that single grid. A 4–8× downsample preserves all of it. Because the depths are bounded, an entire
strate can be covered rather than sampled.

**Validation — is it garbage?**
air fraction · **largest connected component share** (low = swiss cheese: sealed pockets nobody reaches,
generation paid for and never seen) · walkable surface (air above, solid below, survivable slope) ·
**feature scale** (median distance-transform of the air — catches "noise fog", structures smaller than a
few voxels reading as static rather than rock) · vertical clearance · **entrance→exit reachability** ·
**fightable-space count** (GDD §12 dimensions).

**Danger — same grid, different questions.**
fall exposure · **openness** (mean distance to solid from walkable positions — sightlines and room to
retreat, which is most of what makes a fight good or bad) · traversal mix (walk/climb/swim) ·
**tortuosity** (path length ÷ straight line = maze-likeness).

**Rarity — nothing new.** The vector of all the above; distance from the corpus centroid. **Outliers are
rare because they are outliers**, so a dreamcore strate is rare without anyone typing a weight.
Self-correcting: author five dreamcore-ish strates and they stop being outliers, which is the right
answer.

⚠️ **Coarse sampling can LIE about connectivity.** A wall thinner than the sample spacing vanishes and
two sealed spaces look joined, so a strate could pass the primordial-law check while being impassable.
The one metric where a **false pass is dangerous**. **Fix: coarse to FIND the route, then re-verify that
one path at full resolution** — cheap, because only one corridor is verified.

⚠️ **Sample where players actually go** — near the spine and the passage mouths. A box measured far out
describes a place nobody will stand in. Report **distributions (median + spread), not single values**.

**This is also F1.** `fable-idea.md`'s top-pick 2D world-preview tool is this pass with a visualisation
on top. Build the measurement, get the preview nearly free — and it is needed anyway to review a season
before publishing.

### 3.5 Validation in three layers, and how "good" is ever judged

1. **Construct by design** so the law cannot break — build the connections FIRST and generate around
   them. Reachability by construction, not by luck. Generate-then-check gets brutal at 8 strates each.
2. **Measure the accidents** (§3.4). Most garbage is numerically bad, not aesthetically bad.
3. **Reject and resample** — fold the attempt index into the hash (`hash(seed, slot, attempt)`) so every
   seed still yields a valid world and peers stay in lockstep.

**Judging "good" — the judge is the CORPUS, not a model.** Measurements catch *broken*, not *boring*.
But *good* never has to be defined, only **collected**:

- **Rate at the STRATE level, not the world level.** Twenty candidates thumbs up/down is twenty labels
  in minutes. Approved ones join the corpus and become blend parents. **This loop works with zero
  machine learning**; a model only makes it faster, later.
- **Player behaviour in a live season** — lingering, revisiting, rushing past. Free telemetry, but time
  spent somewhere can mean *fascinating* or *lost and furious*.
- ⭐ **Where players choose to snapshot their home** (GDD §11). An unambiguous "I love this place", with
  no UI, no prompt, and no way to game it. The strongest quality signal in the design, and it costs
  nothing to collect.

⚠️ **Known risk: convergence to a comfortable middle.** Promote only what you already liked, keep
blending near the corpus, and the system slowly stops surprising you. Same failure mode as the
build-vote pool, same counter: keep extrapolating past the corpus, and keep some unrated weird
candidates in circulation.

---

## 4. The world manifest

### ⭐ It is TWO things, not one

**Tier A — the SPINE. Finite, tiny, actually stored.** Strate slots and passages. ~30 strates plus a few
passages each is roughly 100 entries — kilobytes. **This already exists** as `StrateLayout` plus the
passage list, computed once in `UVoxelStrateManager::Initialize`.

**Tier B — the FIELD. Effectively unbounded in XY, therefore NEVER stored.** Landmarks, rooms, material
deposits, build sites. It is a **function**: give it a region, it enumerates what is there.
Deterministic, hash-based, **zero bytes**.

That split is what makes "the manifest must not be expensive" true rather than aspirational.

### ⭐ The trick: THE ID ENCODES THE POSITION

Resolving an ID *back* to a place — a saved quest target, a fast-travel destination, a wiki citation —
would normally need a lookup table, which is exactly what we cannot have. Landmarks are placed on a
**hash lattice**, so build the ID from **lattice cell + type + slot index within the cell**. Then
**ID to position is arithmetic**: no table, no database, no storage, resolvable offline on any machine
forever.

**Names fall out of the same thing:** `hash(id, seed)` indexes the word pools. Every entry has a stable,
globally identical name that nobody stored and nobody generated in advance — including entries no player
will ever visit. That is the substrate that makes quests and wikis possible at all.

### What earns an entry

**One test: can a system need to ask "where is it?" or "what is it called?"** If yes, it is an entry.
Yes: strates, passage mouths, landmarks, notable rooms, material deposits, build sites, boss arenas, the
(0,0) spine. No: individual rocks, every tunnel segment, terrain that merely emerged from noise.

**ADDRESSABLE vs NOTABLE.** Everything hash-placed is *addressable* for free — it has an ID by
construction. **Notability is a filter on top, and it is exactly what the measurement pass produces**:
the biggest chamber in a region, the one with a lake, the one at a passage mouth. No new placement
logic, only a threshold.

### Discovery sits ON TOP, never inside

A player's discovered set is a set of **IDs** — small integers, cheap to store and sync. Community
discovery is the union. **The manifest never changes; only who has seen what.**

⚠️ **You cannot hide it anyway.** Clients generate terrain from the seed, so players hold the generator
and can enumerate the manifest offline. Not a leak to prevent but a fact to design around, as Minecraft
does with seed maps. It barely matters here: community knowledge spreading *is* the intent, and a map of
season 4 is worthless in season 5.

### ⚠️ Two cautions

- **The hash functions become part of the FROZEN SEASON CONTRACT.** Change how a lattice cell hashes and
  every quest target in that season moves. These are not implementation details to tidy mid-season.
- **Tier B is cheap, not FREE.** Enumerating rooms in a region needs that region's placement pass. Fine
  for a beacon or nearby markers; a full-map view would hurt. **LOD the query** — coarse regions return
  only high-notability entries, which is what a zoomed-out map wants anyway.

---

## 5. Naming

**Failure mode to avoid: slot-filling.** *"The Crystal Cavern of Whispers."* Players learn the template
in ten minutes; after that every name is noise and, worse, names stop **distinguishing** places, which
defeats the point of having them.

1. ⭐ **Derive names from the MEASUREMENTS, not from a bag.** A vast open chamber draws vast-open words,
   a flooded one draws water words, a tight vertical shaft draws shaft words. The name then *fits* the
   place, and a player who hears "the Sink" and later finds a huge drain-like chamber gets a small
   satisfying click.
2. **Several competing GRAMMARS, not one template.** Compound (Blackreach, Stonefall); descriptive
   definite (The Long Dark, The Hollow); possessive (Varen's Fall — somebody was here and it went
   badly); functional (Shaft Nine — implies industry and records); opaque, from an invented phonology so
   it reads as a language rather than keyboard mash. **Five grammars read as history. One reads as a
   generator.**
3. **Name SCARCITY.** Only *notable* entries get a name; everything else stays addressable and anonymous.
   Scarcity is what makes a named place feel like a place — a threshold, not a feature.
4. **Names must survive contact with a WIKI.** Short, pronounceable on sight, no apostrophes or unusual
   glyphs. **A name nobody can spell cannot become community knowledge**, which is the only reason names
   exist in a shared world.

**Two layers:** the procedural name is the substrate — it exists before anyone visits. A community name
may override it and propagates like a shared build. Nothing is ever nameless, and beloved names win.

⛔ **Parked:** varying the naming language by depth (deeper = older, stranger tongue). Liked, but it is
still a gradient, and gradients are what Jahni rejected.

---

## 6. Code findings and hazards

All verified against the source on 2026-08-17 unless marked otherwise.

### ✅ 6.1 THE BLOCKER — `EffectOverBox` correct in isolation (resolved 2026-09-04)

The source+converter coupling is removed. `FVoxelBoxHypotheses` now carries an
`FVoxelBoxSdfInterval`; each SDF writer publishes only the interval for its own field, and each
converter/detail operator folds that interval through its own formula. `FVoxelOpStack::VF_FoldOp`
propagates the interval in stack order, so an operator cannot borrow a neighbour's reach or verdict.

The interval is deliberately a proof, not a likelihood: invalid, non-finite, or unimplemented
bounds become unknown and therefore cost a skip rather than manufacture `AllSolid`/`AllAir`.
Bounds use the supremum of `Eval` over the authored parameter range, including `sup|FBM| = 1.5`,
the `max(Min, Max)` radius envelope, warp·√2 where two axes are independent, and the SmoothMin
slack. The existing fold carries the interval; no parallel box-query path was needed.

The isolation deliverable is an unverified combination: `ConstantVoid → LatticeCorridorSource →
SdfRoughness → SdfFill`, which no shipping builder emits. Across 256 boxes it proved 250 `AllAir`
and 6 `Mixed`; **332,750 voxels checked, 0 violations**. The shipping archetype scans likewise
reported zero violations. This resolves §6.1 for the current operators, with one ongoing interface
rule: every future SDF writer must implement interval propagation or accept `Mixed` by default.

### ⚠️ 6.2 Box-verdict bounds are proved against HAND-SET params

T1.d / `ClassifyTile` bounds are proved today against the parameters Jahni typed. **If the seed invents
parameters, every bound must hold across the whole authored RANGE, not a value.** Same silent failure
mode as above. See `voxelforge-noise-bounds` — a bound must be the SUPREMUM of what `Eval` produces, not
the parameter named "max".

### ⚠️ 6.3 `ClassifyTile` will silently delete 3D caves

It proves anything below `TerrainZ` solid and skips meshing. Any future 3D generation inside mountains
needs a guard, or the caves vanish with no error.

### 🔨 6.4 The primordial law hole — bounded destination landing

`UVoxelStrateManager::GeneratePassages` places every inter-strate passage as independently salted
hashes of `(seed, upper-strate index, connection index)` → random angle around the (0,0) spine →
random distance in the config range → random reach into each strate. **Nothing consults the
destination strate's cave layout.** Carving is a structural-post invariant so the tube *is* air — but
whether its lower mouth joins the lower strate's connected space is pure luck. **A player can descend
a passage and arrive in a sealed pocket.**

**Fix — make the strate OFFER a landing site, don't let the passage gamble.** The source contract is
`SuggestLandingPoint(DesiredX, DesiredY, MaxLateralSnap) -> optional FVector`. Every source knows where
its own air is: `FRoomGraphSource` keeps the nearest room's vertical placement at the requested XY,
`MakeSlabVoidSource` answers in the void band, `FIslandBlobSource` finds a blob top, and the lattice/
shaft sources search their placement grid. Sparse sources may move laterally, but only within one
source spacing/cell; if that bounded search has no footing, falling back is the honest answer.
Passage placement remains deterministic and cheap, and the slanted control-point chain is bounded too.

Then the measurement pass flood fill is the net: does the upper mouth's air component reach the lower
mouth's? If not, re-roll with the attempt folded into the hash.

### 6.5 Findability is mostly DATA, not code

`ELandmarkAnchor::PassageMouth` already exists — landmarks can anchor at passage mouths — and
`FStrateLandmark` already carries a Light-Orb block. **A glowing landmark at every passage mouth is
authorable today with no code at all.** Beyond that, cheapest first: light (a glow down a tunnel is the
strongest pull in a cave); sound (works around corners, which light does not — you hear the draft before
you see the hole); and ⭐ **make the mouth a PLACE, not a hole** — the `SuggestLandingPoint` fix already
lands the passage in a room, so make that room distinctive. Cracks in walls are forgettable; rooms are
landmarks, and that is what makes it findable on the *second* visit, which quests need.

### ✅ 6.6 The layout depends on the pool's CONTENTS, not its ORDER (BUILT + TESTS GREEN)

Before Tier 0, `Initialize` Fisher-Yates shuffled `Settings->StratePool` in editor order with
`FRandomStream(WorldSeed)`, then cycled it (`PoolCursor % Num`) when `TotalStrates` exceeded the pool.
So **reordering the pool asset in the editor changed every world**, and a pool smaller than the strate
count repeated entries in a fixed order.

⇒ **Tier 0 is now implemented:** load the pool, sort it by the stable soft-asset path, then keep the
existing Fisher-Yates shuffle and cycling algorithm. The pool is a **set** rather than an editor-ordered
sequence: tidying the asset stops changing worlds. Adding or removing a strate type still changes
worlds — unavoidable, and the reason content lands at wipes. A per-slot hash would be a different
sampling algorithm and is intentionally deferred.

### ⚠️ 6.7 A bounded world needs an XY EDGE SEAL, which does not exist

Verified: `VF_ApplyBoundarySeal(float& Density, float WorldZ, float StrateTopZ, float StrateBottomZ,
float Thickness, float BaseDensity)` is **purely VERTICAL** — it seals each strate's ceiling and floor so
nothing punches through except passages. There is no horizontal equivalent, because until the depths were
bounded the world had no sides.

⇒ Build it as a **fourth structural invariant** alongside spine → seal → passage: a **forcing** op,
appended automatically, not author-omittable. An invented strate then cannot forget it — and because
forcing ops prove `AllSolid` in their band, **the outer shell becomes free to skip** in tile
classification.

### 6.8 The (0,0) spine, exactly

`VF_ApplyOriginSpine` carves a guaranteed-open vertical column at actor-local XY (0,0), through every
strate, archetype-independent, appended automatically. **It carves only the strate INTERIOR and leaves
the seals intact on purpose** — its own comment: *"so the player must still dig through to descend"*. It
provides a clean landing space, not a continuous open shaft.

⇒ The hand-authored descent shaft is the **above-ground continuation of the spine**. Nothing new to
build. The city's hole must sit above the VoxelWorld actor's origin (see §6.9), and the shaft's diameter
should match the spine's `Radius` or the join will visibly step.

### ✅ 6.9 Actor space — DONE (built and confirmed off-origin, 2026-08-17)

The field is authored in **actor space**: `(0,0)` is the `AVoxelWorld` actor's origin, not Unreal's world
origin. The plugin used to be only *half* actor-relative — biome/deco/water/density-volume/debug
converted; the **streaming centre**, the **anchors**, all **six carve/fill entry points**,
`GetStrateAtPosition` and `UVoxelAtmosphereManager::UpdateForPlayer` did not. It worked only because the
actor sat at the origin.

`WorldToLocalCm` / `WorldToLocalVoxel` / `LocalVoxelToWorld` are now **the only sanctioned boundary**
between Unreal coordinates and voxel coordinates, and the real deliverable is the invariant beside them:
**no `/ VOXEL_SIZE` applied to a parameter named `World*` outside those three functions.** A grep can
check that — and the risk was never a missed site among the ones found, it is the next one nobody
notices. ⚠️ **Translation only**: rotation or non-unit scale breaks world-axis-aligned box maths
structurally (tiles, `ClassifyBox`, the clipmap window, culling); `BeginPlay` logs an Error.

⚠️ **The trap avoided, worth remembering:** the content managers already converted internally, so
converting `PlayerLastPos` once at the source would have **double-converted four call sites**. The fix
had to be per-boundary. Only visible after reading each consumer.

### ✅ 6.10 The X-macro guard — DONE (built green, 2026-08-17)

`VoxelForge.Determinism.StrateParamBlendCoverage` expands `VF_STRATE_PARAM_FIELDS` a third way, into a
name list, and diffs it against the struct's reflection. `GExemptFieldNames` is empty on purpose. Guards
the fact §3.3 depends on: `Lerp` blends the macro list, not the struct.

### ⚠️ 6.11 Client-side meshing makes determinism a RUNTIME requirement

The host owns collision; every client meshes its own view, which is only safe because every client
generates the same field. Two clients producing different floats means they disagree about where walls
are, and the host's collision is authoritative — so one player hits an invisible wall where another sees
a passage. ⇒ `VoxelForge.Determinism.CrossPlatformDigest` currently **reports rather than asserts**. This
architecture promotes it to **must-be-green**.

*(Not a hazard: six players do not multiply streaming. `RegisterStreamingAnchor` defaults to a 3×3×2 box
= 18 level-0 tiles per remote player, against a clipmap of hundreds across LODs. `CollisionOnly` skips
rendering only — density, marching cubes and the collision cook still run, but at 18 tiles each that is
small. What does scale with party size is **carve-driven re-meshing**, which is bursty.)*

---

## 7. Build order

**The sequencing principle: every tier must be worth doing on its own, before the next one exists.**
Nothing gets built purely as machinery for a later tier. This is the direct antidote to the 2026-07/08
refactor, where three weeks of correct work produced a world unchanged by a single voxel.

### Tier 0 — safety nets, true regardless of the composer
- ✅ **X-macro guard test** — DONE, green.
- ✅ **Actor-space sweep** — DONE, confirmed off-origin.
- ✅ **Order-independent placement** (§6.6) — DONE. Built clean, `VoxelForge.Determinism.LayoutOrderIndependence` green. *Standalone value:* the strate pool asset can be reordered or
  tidied without changing every world. Small and isolated.

### Tier 1 — the primordial law (a BUG FIX, not composer work)
- ✅ **`SuggestLandingPoint` on field sources; passages aim at it** (§6.4) — DONE, **both mouths**.
  ⚠️ It shipped HALF BUILT for weeks: only the LOWER mouth was ever aimed. See §14.3 — the ring proxy
  could not see it, and only the Tier 2 connectivity measurement could.
- ⬜ **The passage mouth becomes a room plus a distinctive landmark** (§6.5) — not started.
- ⬜ **`SurfaceWorld` still refuses the query** — it needs manager-resolved biome context, which
  cannot be answered from a pure free function. Open.

*Standalone value:* fixes a **live hole in the shipping game** — a player could descend into a
sealed pocket. Worth doing even if the composer never happens. **It was real**: see §14.

### Tier 2 — the measurement pass (§3.4) — ✅ BUILT (2026-09-03), see §13
`VoxelStrateMeasure.h/.cpp` + `VoxelForge.Generation.StrateConnectivity` / `...Refinement`.
*Standalone value:* it **is** F1, `fable-idea.md`'s top-pick world-preview tool — the measurement
half. The visualisation on top is not built yet.

**It paid for itself immediately**: it found the `VerticalShafts` primordial-law violation (§14),
which no amount of playing was likely to surface, and it caught three of its own artifacts before
they became "findings" (§13).

⬜ Remaining: the *preview* (F1's visualisation), and the danger/rarity metric families from §3.4
(fall exposure, openness, traversal mix, tortuosity, corpus-centroid distance).

### Tier 3 — op-system prerequisites for free composition
- ✅ **Channel read/write declarations** on every op, plus the stack DAG validator (§3.2; built
  2026-09-04).
- ⏸ **Fuse corrective ops** (`FFloorBiasMod` into `FCaveRoughnessMod`) — blocked by the ten
  intervening `Density` readers/writers under the bit-identity requirement; see §3.2.
- ✅ **`EffectOverBox` correct in ISOLATION** (§6.1; built and brute-force verified 2026-09-04).
- **The XY edge seal** (§6.7).

*Standalone value:* the isolation fix is a correctness improvement to shipping code. The fuse remains
an open semantic decision; the Part 0 analysis above concludes that FloorBias is not a correction of
roughness and should stay late in the stack.

### Tier 4 — the composer proper
- ✅ **Parameter roll (§3.3, Tier 4a)** — corpus spread + deterministic blend/jitter implementation
  and the 64-candidate measurement pass are built; the first run exposed a one-vector settings corpus
  and a vacuous cave box-verdict scan, both recorded above.
- ✅ **Structure roll (§3.2, Tier 4b)** — serialisable recipes, root polarity, declaration-derived
  resource-safe modifier legality, mandatory posts, 64-candidate measurement, and direct box-verdict
  brute force are built; focused result is 46/64 survival with 0 invalid recipes and 0 violations.
- ⬜ Reject-and-resample driven by Tier 2 (§3.5) · the **offline season pipeline** with Jahni's review
  and veto (§3.1).

### Tier 5 — the long game
**Promotion** (good strates rejoin the corpus) · theme and tag draws for materials, creatures and audio ·
eventually the model. All optional, all compounding.

---

## 8. Corrections that must not be re-derived

Kept because each cost real time or would otherwise be repeated.

- ⛔ **"Do not start Phase 3" is SUPERSEDED.** `OPSTACK-HANDOFF.md` still says it; believe this file.
- ⛔ **Jahni does not want to author strates either.** The composer must invent them. An earlier reading
  had him authoring strates while the system arranged them — one level too shallow, and it shaped a
  whole afternoon of wrong design.
- ⛔ **Peer determinism never required matching the OLD system.** Same build, all machines — that is all
  it ever meant. The self-imposed bit-identity criterion cost three weeks. Version stability across
  builds is a *separate* property, and seasons make it a non-issue.
- ⛔ **Digging is a reveal-and-shape verb, not a movement verb.** An earlier pitch made it traversal
  (the Deep Rock model). That is not this game.
- ⛔ **Six players do not multiply streaming.** Raised as a 6× cost; wrong (§6.11).
- ⛔ **Depth must not mean "weirder".** An early pitch argued for geological coherence and depth-driven
  strangeness. That is a different game — see GDD §7.
- ⛔ **`Alpha` is not a struct field**, it is `Lerp`'s parameter. An earlier note claimed the X-macro had
  one exception; it has none.
- **State acceptance criteria up front and have Jahni confirm them.** Criteria chosen alone drift
  toward whatever can be self-verified, which is reliably the wrong one.

---

## 9. Open — generator side only

Game-design questions live in **[GDD.md](GDD.md) §16**. These are the ones that block generator work:

1. **Adjacency rules between strates** — confirmed wanted, never specified. Proposed shape: a small
   tag-rule list in settings (no two alike in a row; `Anomaly` needs a `Grounded` neighbour above; max
   run length), with the draw falling back to a designated safe entry so it can **never fail**.
2. **How the mix rule is expressed** — a global "total strangeness" cap was proposed; not ruled on.
3. **How theme coherence is drawn** — operators, materials, decorations, creatures and audio pulled as a
   compatible set. Jahni expects his first big job to be creating themes and tagging content with
   multiple fitting tags.
4. **Where the corpus lives** and how promotion is recorded between seasons.
5. **Whether op discovery** (promoting good sub-stacks into named reusable units) is ever worth it.
   Parked: the existing parameter space is already vastly larger than a hundred seasons could explore.
   **The scarce resource is judgment, not vocabulary.**

---

## 10. Verification log

**2026-08-17 — full suite run headless, first time this session.** Built `VoxelMEditor Win64
Development` clean (no errors, no warnings), then:

```
UnrealEditor-Cmd.exe VoxelM.uproject -ExecCmds="Automation RunTests VoxelForge;Quit"
                     -unattended -nopause -nosplash -NullRHI -ReportExportPath=...
```

**Result: 16 succeeded, 0 failed, 0 not run, 0 succeeded-with-warnings.** 25.7 s.

Tests: BoxVerdictFold · ClassifyTileSoundness (×2) · CrossPlatformDigest · DensityPurity ·
DiffLayerContention · FloatingIslandEquivalence · LargeSeedSurvives · **LayoutOrderIndependence** ·
LiveEditInvalidation · MazeEquivalence · SlabEquivalence · **StrateParamBlendCoverage** ·
SurfaceHeightEquivalence · TunnelNetworkSpineEquivalence · VerticalShaftEquivalence.

**2026-09-04 — Tier 3b validation run.** The specified UE 5.7 `VoxelMEditor Win64 Development`
build succeeded (14 actions). The full headless suite reported **22 succeeded, 0 failed, 0 not run,
0 succeeded-with-warnings**, including `VoxelForge.OpStack.ChannelDAG`; the existing equivalence,
determinism, box-verdict, and connectivity checks stayed green. The 4b→4h fuse was deliberately
not applied after the intervening-channel audit above.

**⭐ This closes the pending-verification thread opened by `OPSTACK-HANDOFF.md`.** That file flagged
commits `4d33321` (Sol's boundary fold) and `91585ea` as *built but NOT re-verified*, with two
acceptance conditions. Both now met:
- **`violations` = 0** everywhere. Measured: TunnelNetwork at production defaults **12 of 40 tiles
  proved AllSolid, 15 972 voxels brute-forced, 0 violations**; VerticalShafts **32 of 60 proved, 0
  violations**; dense fixture 0 proved / 40 Mixed, 0 violations.
- **All equivalence tests green** — Maze, Slab, TunnelNetworkSpine, VerticalShaft, FloatingIsland,
  SurfaceHeight.

⇒ **No revert of `4d33321` is needed.** The 39 % tile-skip win stands, verified rather than assumed.

⚠️ **Read the PROVED counts as measurements, never as contracts.** What is asserted is that none of
them is *wrong* — a false verdict leaves no geometry and no collision behind it.

---

## 11. Tier 1 status — passages aim at real open space (bounded full-point query; build not run)

**Updated 2026-08-29.** `VF_SuggestLandingPoint` (in `VoxelCaveMorphology`) is a **pure free function**
of (archetype, params, seed, desired XY, lateral budget) — it touches no operator stack, no
`UVoxelStrateManager`, no cache. That matters: `GeneratePassages` runs inside `Initialize`, when the
layout is half-built and `LayoutVersion` is mid-flight, so constructing anything holding a manager
pointer there is a re-entrancy trap.

`GeneratePassages` now asks the **destination** strate for a full open/footing point near the chosen XY.
Maze, VerticalShafts, and FloatingIslands may snap to a nearby lattice site, shaft, or island top;
the lower mouth uses the returned X/Y/Z, clamped strictly inside the interior (never into a seal band).
Where the archetype cannot prove footing inside its bound, **today's random reach and original XY are
preserved unchanged**.

### ⚠️ Coverage is source-dependent and the number is the point
The failing pre-fix measurement was:

> *7 inter-strate passages, 3 checked, 0 footing checks, 38/48 room/slab ring samples air, 4
> query-false (4 unique archetypes; 1 unsupported, 3 supported-but-no-point).*

The Tier 1 query now searches laterally for `Maze`, `VerticalShafts`, and `FloatingIslands` landing
sites within one configured placement period. `SurfaceWorld` remains intentionally refused.
The extended test uses vertical source footing brackets for those three instead of applying the
room/slab circumference ring to thin or void-dominated geometry. The post-fix measurement is deliberately
not claimed here because this change does not build or run the Unreal automation test.

### Seed contracts, do not mix them
- **TunnelNetwork / Underwater** → `VoxelCaveMorphology::MakeStrateSeed` (the strate's own room seed).
- **Slab / Maze / VerticalShafts / FloatingIslands** → the generator **world** seed, which their
  source placement/noise uses.
- **SurfaceWorld** → no answer yet; its world seed alone does not identify the manager-resolved
  biome/per-column production context.

Seeding the query from the passage salt hash — which my spec originally asked for — would inspect a
cave graph that **does not exist** and return confident answers about imaginary rooms. Codex caught it.

### The test is a proxy, and says so
The first version sampled `GetDensityAt` at the mouth and asserted air — **vacuous**, because
`VF_ApplyPassageCarving` is a structural-post invariant, so the tube is air *at its own mouth by
construction*. It passed whether the aiming worked or not. Replaced with a **16-point lateral ring** at
`MouthRadius + 2 × PassageBlend` (~14 voxels at defaults), requiring ≥ 8/16 air for room/slab
destinations. A mouth in a real room has open space around it; a mouth in bedrock has only the tube it
dug itself. Maze, VerticalShafts, and FloatingIslands use a manager-free source density bracket:
air at the landing and solid below it. A large void ring is not evidence of island footing.
⚠️ **It is a proxy for connectivity, not a proof.** The proof is a flood fill — Tier 2.

### ⚠️ A forked placement envelope, guarded by that test
`VF_FindNearestHashRoomLandingPoint` re-derives the room vertical placement envelope that `BuildChunkCache` also
computes. Calling the per-chunk cache builder from `Initialize` is impractical, so this is a deliberate
second copy. **Both sites now carry a PLACEMENT CONTRACT comment naming the other**, and the ring test
is the only thing standing between a placement change and silent bedrock passages.

---

## 12. Tier 1 FINAL — and the 92.8% defect a green suite hid

**Built clean, suite green: 17 succeeded, 0 warnings, 0 failed (2026-08-30).**

### ⚠️ Correct the record: commit `1ab8c0c` overstated what worked
It reported *"3 checked, 48/48 ring samples air"* and read as Tier 1 working. A million-seed sweep later
showed the room query left the landing point **OUTSIDE the selected room 92.795% of the time**. The
fixture's 48/48 was **luck**, not correctness — seven sampled passages cannot see a systemic defect.

**Cause, and it was my instruction:** I specced `MaxLateralSnap = 0` for rooms and slabs — *"preserving
existing placement, do not regress them, they are green today."* The room path found the nearest room,
then returned the **caller's XY** with that room's Z. It located a room and aimed beside it. That is the
exact defect I correctly identified for Maze/VerticalShafts/FloatingIslands and then explicitly exempted
rooms from.

### The fix: return the ROOM CENTRE
`VF_FindNearestHashRoomLandingPoint` already computed the room's XY and discarded it. It now returns it.
- **A room's centre is inside its own room by construction** — no SDF evaluation, no warp handling
  needed. It is also the point with the largest margin against the production cave warp (~1.5 voxels of
  displacement: decisive at a room's edge, irrelevant at its centre). Simpler *and* more robust than the
  warped-SDF inside-proof I had been specifying.
- **Caves get a real budget** (`RoomSpacing`): a room is an XY-**sparse** target, exactly like a maze
  corridor. **Slabs correctly keep 0** — a slab's void band is XY-continuous, so the requested XY is
  already inside it.

### ⚠️ OPEN TRADE-OFF: the room budget is tight
Final measurement: *7 passages, 5 checked, 3 footing checks, **32/32** room/slab ring samples air,
2 query-false (1 unsupported, 1 supported-but-no-point)* — and **TunnelNetwork and Underwater declined
entirely at this seed.**

With `RoomDensity < 1` not every cell holds a room, so the nearest is often 1.5–2 cells away and one
`RoomSpacing` refuses. Cave passages are back on random reach — *honest*, but not landing.

**~2× spacing would likely restore cave coverage, at the cost of passages drifting further from the
(0,0) spine — the property that makes them findable.** Deliberately NOT tuned: "raise the number until
coverage looks better" is the move rejected twice this session. **Jahni's call.**

### A guard was softened, and why that is not threshold-tuning
`PassageLandsInOpenSpace` asserted that all four room/slab archetypes must answer *within the fixture's
7 passages*. That asserts a property of the **sample**, not the code — and it failed on a **fix**, since
a supported query is *allowed* to decline on budget. Now reported via `AddInfo`.
**Still hard failures:** vacuity (zero passages / zero checks / no footing exercised), every per-mouth
ring and footing assertion, endpoint-matches-query, and SurfaceWorld unexpectedly answering.
Same discipline the codebase applies to box-verdict PROVED counts: *read the count as a measurement,
assert only that nothing it reports is wrong.*

### Process notes worth keeping
- ⚠️ **`codex exec` began hanging reliably** — three runs, 0 bytes written, process alive, at
  `max`/`xhigh`/`high`. A tiny `low`-effort probe worked, Jahni's interactive CLI works, and quota was
  confirmed fine. Cause unresolved; the room fix was completed by hand instead.
- ⚠️ **I waited 4.5 hours on a hung background task** because I treated "no notification" as "still
  working." Checking the output file's size costs nothing. `CodexGuidance.md` states the rule from the
  other side: *never wait silently; report if blocked more than two minutes.*
- ⚠️ **A stream edit (`perl`) corrupted `VoxelStrateManager.cpp`** — a skip-until-terminator ran past
  its block because a second `UE_LOG(LogTemp, Warning` existed earlier in the file, deleting ~178 lines
  and an entire function. Git recovered it. **Use targeted read-then-edit on source files; verify line
  count and brace balance after any structural edit.**
- **Do not run builds while a Codex task is active** — `CodexGuidance.md` §Collaboration: one agent per
  working tree.

---

## 13. Tier 2 BUILT — and what measuring the world actually found (2026-09-03)

**The measurement pass exists, is falsifiable, and earned itself on the first run by finding a live
gameplay bug that six months of playing might not have surfaced.**

`VoxelStrateMeasure.h/.cpp` — read-only, deterministic, headless. One coarse grid, one 6-connected
air flood fill, every metric derived from that same grid: air fraction, component count, largest
component share, walkable fraction, median feature scale, median vertical clearance, and connectivity
between two points. `VoxelForge.Generation.StrateConnectivity` (fast) and
`...StrateConnectivityRefinement` (the slow sweeps).

### ⭐ The single most important habit this produced
**Every reported number states the volume it was measured in.** `ResolvedMarginVoxels`, `SampledMinZ`,
`SampledMaxZ` are on the metrics struct and printed. This exists because the first version did not
have it, and the consequence is below.

### ⚠️ Three artifacts that looked exactly like findings

Each of these was, briefly, believed. Each was wrong. The pattern is identical every time: **a number
produced by a method nobody had questioned.**

1. **"FlatPlain is a 99.95% empty void."** The interior window excluded a whole chunk (32 voxels) at
   each strate boundary to avoid a 4-voxel seal band — 8x too much. On 4-chunk strates that discarded
   *half of every strate, floor included*. Walkable fraction was reading 0.00044; with the window
   derived from the seal that actually exists (`2 x BoundarySealThickness`), it is 0.0578. **132x.**
   *My spec's error.*

2. **"5 of 15 passage endpoints are disconnected."** The test's `LargestComponentAnchor()` did not find
   the largest component — it returned a hardcoded guess near the (0,0) spine. And the question itself
   was wrong: *"connected to the largest component"* is not the primordial law. A strate can be 57
   disconnected shafts and be perfectly good, provided the shaft you arrive in is the shaft you leave
   from. Corrected: **13 of 15 connected**, and the real question is `arrival → departure`.
   *My spec's error, twice over.*

3. **"7 of 16 seeds fail the law."** `VF_AreConnected` took the shortest coarse BFS route, re-walked it
   at full resolution, and on hitting a wall returned false and gave up — never looking for another
   way round. Of those failures, only 2 were `NOT_CONNECTED`; **7 were `COARSE_LIED`**, i.e. one
   candidate route was blocked. A blocked route is not a disconnected pair. *My verifier's error.*

⇒ **Rule: when a measurement reports something alarming, suspect the measurement first.** It was the
method three times out of three. The cost of checking is minutes; the cost of not checking was nearly
a rewrite of generation that was not broken.

### The guard is two-directional now, and the directions are NOT symmetric
- **False POSITIVE** (coarse says joined, reality is walled) → *dangerous*: no geometry, no collision,
  a player falls through the floor. `FullResolutionPathIsAir` re-walks the recovered route at
  one-voxel spacing. It fires: **2 of 21 routes** on the first run were coarse false positives.
- **False NEGATIVE** (a corridor thinner than `SampleStep` vanishes) → *merely wrong*, but wrong in the
  direction that sends you "fixing" healthy code. Named explicitly as
  `NotConnectedAtThisResolution` so a bare negative can never be read as proof.

⇒ **`CoarseLiedBudgetExhausted` means UNKNOWN and must never be folded into "disconnected".**

### Settling a finding: refinement with the confound held fixed
`VerticalShafts` was declared a real bug only after step 4→2→1 (7.34M cells), radius 256→192→128, and
margin 8→4→2→0 (margin 0 includes the seal, which can only *add* solid, so it cannot invent a route) —
with **step and radius varied independently** so neither could explain the result. Then 16 seeds,
because a single fixture cannot distinguish one unlucky world from a broken archetype. See
[[tests-must-be-falsifiable]]; a 7-sample fixture already hid a defect that fired 92.8% of the time.

---

## 14. The VerticalShafts bug, end to end

**Symptom:** descend into a `VerticalShafts` strate and you cannot reach its exit. 15 of 16 seeds.

**It was three bugs wearing one coat.**

### 14.1 The spine was not the backbone there
The (0,0) spine (`OriginSpineRadius` 14) holds 97–100% of the air and is the largest component in
**7 of 8** archetypes. In `VerticalShafts` it punched through as an isolated column holding **9.6%**,
never the largest. Fixed: the spine now participates as a *connector endpoint* — **not** as a carving
shaft; `VF_ApplyOriginSpine` remains sole owner — and is excluded from ledge placement.
Necessary, **not sufficient**: it only joins its own 3x3 neighbourhood, and mouths land 60–200 voxels out.

### 14.2 ⭐ The shaft graph sat below its percolation threshold
The tell was a *plateau*: the largest component sat at **~0.22 in every variant** — roughness 0,
connector radius 8, baseline. Roughness changed the component *count* (it manufactures noise pockets)
but never the giant component's share.

`ShaftDensity` 0.6 sites, `CrossConnectChance` 0.35 bonds, and a `1.6`-cell pair cutoff that silently
drops many neighbour pairs once jitter spreads shafts 1.7 (orthogonal) to 2.4 (diagonal) cells apart.

⛔ **Do NOT fix this by raising those three numbers.** Percolation is a **phase transition**: tuning
toward it buys a world connected at one seed and shattered at the next — which *is* the 1-in-16
behaviour. A phase transition is not a knob.

✅ **Fixed structurally:** a **drainage tree**. Every shaft emits a deterministic connector to the
nearest shaft *strictly closer to the origin*, so chains drain inward and terminate at the spine. The
probabilistic connectors stay as texture and loops — the tree guarantees you can get out, the noise
keeps it from reading as a diagram.

  - seed 1337: **30 components / .220 largest → 17 / .975**
  - 16 seeds: spine is the largest component **16/16** (was 8/16)

⚠️ **Seam hazard, and the reason for the 7x7 window.** A shaft's parent computed from a 3x3 window
would differ between chunks, so the connector would exist in one and not the other — invisible in the
metrics, very visible in game. **Collect over 7x7, emit only for the inner 3x3**, whose 5x5 candidate
windows are then fully contained. Guarded by `VoxelForge.Generation.VerticalShaftSeamFreedom`
(154 probes, 0 mismatches). The 49 hash rolls happen on **cache rebuild, never per voxel**
(1.634 us rebuild, 0.100 us hot call).

⚠️ Connector radius must exceed the **proven roughness supremum**
(`SurfaceRoughness x VOXEL_NOISE_SCALE x 1.5` = 5.625 at defaults; `sup|FBM|` is **1.5, not 1.0** —
see [[voxelforge-noise-bounds]]). A guaranteed connector that noise can close is not a guarantee.

### 14.3 ⭐ Tier 1 was only ever HALF BUILT
`VF_SuggestLandingPoint` was called **once per passage, for the LOWER mouth only**. The upper mouth
fell out of the control-point chain with no landing query at all. **Tier 1 fixed where a player
ARRIVES and never touched where they DEPART** — and the measurement said so exactly:
`departure → spine` was **0 connected across all 16 seeds**.

**The ring proxy could never have caught this.** It checks each mouth for open space individually, and
an unaimed mouth still has open space around it *because the passage tube carved some*. Only a
connectivity measurement could see it. This is the strongest argument in the whole project for Tier 2
existing at all.

✅ Fixed: each mouth queries **its own** strate independently — which is precisely what keeps the
layout a **set, not a sequence**. Passage `i` never reads passage `i-1`, so
`LayoutOrderIndependence` stays green. ⛔ The "obvious" fix (chain the mouths) would break it.

  - `departure → spine`: **0 → 9** of 16 · `arrival → departure`: **4 → 7** of 16

**Status: not yet 16/16.** The remaining gap is mostly `COARSE_LIED`, i.e. §13's verifier defect, not
the world. Fix the measurement before touching generation again.

---

## 15. Working notes that cost something

- ✅ **The `codex exec` hang was stdin.** It prints `Reading additional input from stdin...` and blocks
  forever on an inherited pipe. **Always `< /dev/null`.** Cost 4.5 h once. Read the banner's
  `reasoning effort:` line back after every launch — Jahni asks for `max`.
- **A killed agent leaves unvalidated edits.** A session teardown killed Codex mid-task; the edits were
  complete and correct but had never been built. **Build and run them yourself rather than assume** —
  a clean-looking diff is not a passing test.
- **Codex has found six real errors in my specs across nine tasks.** Asking for disagreement in every
  spec is the highest-value line in them. Three of today's corrections came from it, not me.
- **Acceptance must be the measured property, never "the code was written."** Twice a task returned
  "built, and it does not meet the criterion — here is the number." Both times that honesty pointed
  straight at the real cause.
