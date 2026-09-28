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

The 2026-08-16 `Docs/archive/OPSTACK-HANDOFF.md` says in bold *"Do not start Phase 3."* **That is superseded.**
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

**What ships is small:** the season's seed plus the chosen strates' parameter vectors and structure
recipes. Clients generate
terrain from those, deterministically, exactly as today. **No composition is shipped or run in the
normal game path.** The editor-only PIE inspection button described below is a deliberate debugging
exception: it overlays one already-built slot so the owner can walk a measured candidate; it is not
cooked, does not compose a season, and is cleared by RebuildStrates.

**Tier 4c is now the build-box seam:** `VF_ComposeSeason(SeasonSeed, Settings)` generates a bounded
candidate batch, measures every candidate, rejects vacuous/fragmented/primordial-law failures, and
writes the selected Tier A spine as diffable `season_manifest.json`. The default test budget is 24
candidates at measurement step 8 and a six-slot output; production can raise the candidate count for
the overnight budget without changing the runtime contract. The manifest stores each selected seed,
recipe, complete native parameter-family vector, archetype, bounds, measured metrics, and gate facts.
Floating-point fields carry both a readable value and their exact IEEE-754 bit pattern, so a loader can
rebuild the same stack and the season test can assert bit-identical density rather than assume it.

Publishing is explicit: keep the JSON as the reviewed/diffable source, create a
`UVoxelSeasonAsset`, point `SourceManifestJson` at it, press `ImportSeasonManifestJson`, and assign the
asset to `UVoxelSettings::Season`. The primary data asset is what cooks. Schema 2 embeds a canonical
SHA-1 `content_hash`; runtime recomputes it and also compares the asset's separately imported metadata,
so a stale or edited season fails closed instead of silently falling back to the authored pool.

The selected-only review page is a descent-ordered `VoxelStratePreview` contact sheet. Each card shows
the depth, recipe, metrics, grounded/outlier/fixed selection reason, and boss-slot marker. Selection is
deliberately provisional: the current readable policy reserves 75% grounded and 25% outlier slots,
penalises near-identical neighbours, rewards new archetypes/recipes, and respects fixed absolute slots.
Jahni's preview and veto remain the decision that turns this artifact into a published season. The
composer, corpus, measurement, selection, promotion, and review writer are editor/build-box code only.
Runtime contains only manifest parsing/verification and recipe evaluation, and `WorldRadiusVoxels`
remains 0. Generated schema-2 slots have no passage/content/visual record: they therefore use
deterministic empty/default wider content and zero auto-passages (the origin spine remains). Fixed slots
may retain an authored definition path as that wider-content bag. Lateral regions remain unshippable at
the measured **4/16** cross-seam law result after the landing pass (the 9/16 figure below is the
pre-landing snapshot).

An abbreviated manifest shape is:

```json
{
  "format": "VoxelForgeSeasonManifest",
  "schema_version": 2,
  "content_hash": "0123456789abcdef0123456789abcdef01234567",
  "season": 0,
  "seed": 9320,
  "candidate_count": 24,
  "survivor_count": 19,
  "selected_count": 6,
  "strates": [
    {
      "depth_index": 0,
      "seed": 123,
      "archetype": "Maze",
      "uses_recipe": true,
      "recipe": {
        "root_polarity": 0,
        "root": { "op_class": 0, "param_block": 3 },
        "shape_source": { "op_class": 3, "param_block": 3 },
        "conversion": { "op_class": 8, "param_block": 3 },
        "structural_param_block": 3,
        "modifiers": []
      },
      "parameters": { "maze": { "LatticeSpacing": { "value": 18.0, "bits": "0x41900000" } } },
      "measured_metrics": {
        "air_fraction": { "kind": "float", "value": 0.5, "bits": "0x3F000000" },
        "largest_component_share": { "kind": "float", "value": 0.9, "bits": "0x3F666666" }
      },
      "selection_reason": "grounded; provisional 75/25 policy",
      "boss_slot": false
    }
  ]
}
```

The real file is complete; the sample is intentionally abbreviated. `FVoxelSeasonFixedStrate` also
allows a build invocation to provide a full recipe/vector for an absolute slot, while entries adapted
from today's authored `FixedStrates` retain their native vector and source asset path.

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
`FVoxelOpSample` (`Density` and `Sdf`); all 30 concrete density operators (including the lateral
parent combiner) declare reads, writes, and
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

### 3.2b Lateral regions — implemented, primordial-law gate failed (2026-09-06 post-landing audit)

The owner's “tunnel network leading to a big chamber and drops into shafts” question changes the
unit of composition: an archetype is a **region inside a strate**, not the strate's label. Tier 4d
therefore adds `FVoxelStrateRegionManifest`. The composer rolls 1–3 region records by a salted
`VoxelHash::Cell` identity. Every region carries its own native parameter vector and, for structure
rolls, its own recipe. The partition is a jittered 256-voxel XY lattice with nearest-site,
nearest-different-site Voronoi assignment. Its identity is pure `(XY, seed, absolute strate index)`;
rekeying is explicit when a candidate moves from an attempt index into a live slot.

The lateral junction mirrors the vertical transition shape but blends the result at the right
level. In a region core, one stack evaluates. Within the 24-voxel boundary band, the nearest
different region's stack evaluates too and the two **internal densities** are crossfaded with the
same linear curve used by the vertical transition (split symmetrically at the bisector). Native
parameters are never blended: `ShaftSpacing` and a slab's floor/ceiling controls do not share a
meaning. Region cores omit structural posts; the parent appends the global spine, vertical seal,
passage tube + landing/floor, and XY edge seal once, in the existing order, after the lateral result. `Density`
keeps the stack convention positive=solid and is negated only at the MC-facing boundary.

The runtime hand-off remains an explicit manifest copy, not a new runtime composer. A worker
prepares a small lattice-site halo and `(CHUNK_SIZE+3)^2` integer query grid per chunk; normal
integer voxel samples use O(1) cached region lookup, and fractional gradient probes reuse the
prepared site set. `ValidateChannelOrder` still runs for every region core and the parent, and
`WorldRadiusVoxels` remains zero during offline materialisation.

The box proof is intentionally conservative. A box is allowed to use one child only when the
centre's bounded-neighborhood nearest-site gap is known and, minus twice the XY half-diagonal, is
**strictly greater** than the blend width. If the local lattice window cannot certify that nearest
different-region site, the proof is refused. Every other box evaluates every region's `ClassifyBox`;
any `Mixed`, disagreement, boundary, or unproved band remains `Mixed`. The dedicated audit exercised 40 boxes, including 20 boxes that
actually touched more than one region, brute-forced **199,800** integer samples, and found **0
false-uniform violations**. All 20 cross-boundary verdicts were `Mixed`; no cross-boundary uniform
claim was accepted.

The performance audit at width 24 measured **10,517/65,536 = 16.0477%** band voxels over the
Z-independent XY field. With chunk preparation excluded and the partition cache hot, the latest
focused run was **338.11 ns/voxel** in an interior and **663.12 ns/voxel** in a band (**1.961×**).
That is the expected cost of evaluating a second stack, and it is an explicit §8.10 budget item;
if this
feature proceeds, a narrower band or a cheaper density junction needs an owner-approved design
decision rather than an unreported performance regression.

The primordial law is the blocker. The pre-landing audit connected **9/16 (56.25%)**. After the
landing geometry pass, the current same-fixture audit still collected **16 actual opposite-region
arrival/departure mouth pairs** and connected **4/16**; the one-region controls also connected
**4/16**. The same 16 seam cases remain **valid 16/16, non-vacuous 16/16, and largest-component
share ≥ 0.50 16/16**; including the unsnapped seam law, current full hard-gate survival is therefore
**4/16**. The landing itself is geometry-green, but it does not rescue the native region network.
The result is not ready for promotion. A future fix must be a design decision such as a guaranteed
cross-seam corridor/landing contract, not a hidden pass threshold or seed selection.

The rendered review artifact is
`Saved/ComposerPreview/LateralRegions_seed_12001/index.html` (vertical/plan images and contour
variants). It is intentionally retained even though the law test is red, so a measured seam can be
looked at rather than trusted.

### 3.2a Editor walk-through hand-off (not a runtime composer)

AVoxelWorld::ApplyComposerCandidate is the one editor-only bridge from this offline API to a live
PIE world. It calls VF_RollStrateCandidate(Corpus, ComposerSeed, ComposerCandidateIndex,
bComposerRollStructure). That wrapper delegates to the exact parameter/recipe calls exercised by
the offline composer tests, then installs the result
as a temporary density override on ComposerTargetStrateIndex. The layout slot's authored definition,
height, content, and generated passages are intentionally unchanged; this avoids calling
GeneratePassages while Initialize is half-built. The action pauses/drains generation, bumps the
manager layout version, and reuses RegenerateAllChunks so GenerationEpoch invalidates stale mesh
results and the normal streamer rebuilds the world.

The Details-panel properties are ComposerSeed, ComposerCandidateIndex, ComposerTargetStrateIndex,
and bComposerRollStructure, under Live Edit|Composer. The button is Apply Composer Candidate; it
is usable only during PIE. RebuildStrates clears the temporary override. A post-apply eight-point
density check compares the live generator with the same prepared candidate stack bit-for-bit when
no diff edits, disturbance post, or surface-biome context makes the standalone oracle incomplete.
The log line carries the recipe/archetype and the coarse metrics so it can be matched to a
contact-sheet row. This hand-off is editor-only and is not part of the shipped composer contract.

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

#### Human-scale audit and the killed tunnel-only hypothesis (2026-09-05)

The generation defaults are now derived from the body reference rather than chosen as voxel counts:
**1 voxel = 0.25 m**, the player capsule is **1.36 voxels / 0.34 m radius** and **7.04 voxels /
1.76 m tall**, and the default strate is **8 chunks / 256 voxels / 64 m**. The complete generated
`name | voxels | METRES | body/level meaning` table is emitted by `VoxelForge.Composer.Showcase` and
covers each archetype plus the shared passage/disturbance and optional transport fields.

The requested maze test was run first on the same seed/index and a mouth-fitted step-1 ROI. The
radius is the tunnel radius; the bore is twice the radius:

This is the **pre-spanning-tree radius sweep**. It remains useful as historical evidence about
mouth/fine-resolution behaviour, but it is not a connectivity result for the new topology.

| Corridor radius | Bore | Player-fit fraction | Traversable share | Minimum clearance | Arrival→departure |
|----------------:|-----:|--------------------:|------------------:|------------------:|-------------------|
| 4 vox / 1.00 m | 2.00 m | 0.009007 | 0.078358 | 1 voxel / 0.25 m | NotConnected |
| 6 vox / 1.50 m | 3.00 m | 0.014545 | 0.165932 | 1 voxel / 0.25 m | NotConnected |
| 8 vox / 2.00 m | 4.00 m | 0.012156 | 0.126354 | 1 voxel / 0.25 m | NotConnected |
| 10 vox / 2.50 m | 5.00 m | 0.009814 | 0.105611 | 1 voxel / 0.25 m | NotConnected |
| 12 vox / 3.00 m | 6.00 m | 0.008697 | 0.185039 | 1 voxel / 0.25 m | NotConnected |

The hypothesis is **killed as a single root cause**. There is no sharp or monotonic fit rise, and
the law stays disconnected at every bore. The unchanged one-voxel minimum also shows that the
failure is not explained by the nominal tunnel diameter alone: the generated field, route mouths,
floor/cap interaction, and/or resolution still need separate diagnosis.

The new defaults use separate derivations: rooms are 8-20 m diameters with a 24 m origin hub and
32 m cells; round passages are 3-4 m bores; Maze is a 16 m cell with a 6.25 m nominal bore;
slabs reserve a 22.4 m floor-to-ceiling span; shaft diameters are 4-7 m with 3 m connectors;
islands are 12-24 m diameters on 28 m cells; ordinary passages are 3-4 m bores and reach 8-24 m.
`RoomSpacing` keeps the largest room 6 m radially clear of the next cell. For Maze, the target is
3 m of usable two-abreast floor: `3 / 0.7 = 4.286 m` clear diameter, or an 8.571-voxel clear
radius; adding the proven 3.75-voxel roughness reach gives 12.321 voxels, rounded to
`CorridorRadius=12.5`. Thus the worst-case usable floor is
`0.7 × 2 × (12.5−3.75) × 0.25 = 3.0625 m`, and the roughness ratio is `3.75/12.5 = 0.300`.
The 64-voxel cell leaves `64−2×(12.5+3.75)=31.5` voxels (7.875 m) of worst-case wall
between neighbouring bores. `StrateHeightInChunks` grows from 4 to 8 because 17 m tall rooms,
20 m terrain relief, and a high cap do not share a comfortable 32 m envelope. No global multiplier
was used. The other default ratios remain **0.625** (minimum tunnel), **0.469/0.625**
(shaft/connector), **0.234** island radius (**0.484** including blend), **0.625/0.938** slab
column for floor/ceiling, and **0.047** against SurfaceWorld's 80-voxel relief scale.

The architecture sanity check uses [Epic's Unreal level-blockout guidance](https://dev.epicgames.com/documentation/en-us/unreal-engine/designer-01-project-setup-and-level-blockout-in-unreal-engine) (player-sized reference, 2-3 m halls and 3-4 m heights as starting guidance), the [2010 ADA Standards](https://www.ada.gov/law-and-regs/design-standards/2010-stds/) (915 mm clear walking width and larger passing/turning spaces), and [NPS Lehman Cave dimensions](https://www.nps.gov/grba/learn/nature/lehman-caves-dimensions.htm) (real chambers ranging from passages to 20 m-plus vertical volumes). These are sanity anchors, not claims that one universal ratio makes a good cave.

### ✅ Tier 5 status — promotion is built and measured (2026-09-05)

Promotion is now a real corpus input, not a claim about a future approval UI. A surviving editor
candidate becomes a `FVoxelStratePromotableRecord`: complete native parameter vector (all six family
blocks), recipe, archetype tag, measured metric summary, season/seed/candidate identity, input corpus
hash, and the three gate results. `VF_SaveStratePromotedRecords` writes a sorted, pretty JSON store
(`schema_version=2`) beside one JSON season manifest per run. `FVoxelStrateCorpus` loads those records
alongside project assets and C++ defaults, tags each member `project`, `default`, or `promoted`, and
prints the provenance counts in the membership report. The stored metric is evidence for review only:
the world-specific verifier rebuilds the recipe and remeasures it on every load, and a failed fresh
non-vacuous, largest-component, or primordial arrival→departure gate cannot enter the corpus.

The provisional policy is deliberately explicit. It accepts only records that pass all three gates,
requires a normalized measured-metric distance of **at least 0.20** from every usable existing member
and already accepted record, and caps a season at **6** promotions. Distance is Euclidean over
`(air fraction, largest-component share, walkable fraction, median feature scale / 256 voxels,
median vertical clearance / 64 voxels)`. The 0.20 threshold is a review-sized novelty floor: it is
roughly a 20-point move in one bounded fraction, or a smaller combined move across dimensions, while
the physical normalizers make feature/clearance changes comparable. It is provisional, not the
owner's definition of good. One authored asset was measured as a valid but all-air window in this
fixture (`DA_Strate2`), so it is retained as measured-but-vacuous and uses conservative exact-parameter
identity fallback rather than a false metric distance; the other **11/12** base members participate
in the measured novelty space.

The five-season simulation used 24 structure candidates per season, the fixed sample window
(`step=8`, `radius=256`, `WorldRadiusVoxels=0`), and the same fresh verifier on every reload:

| Season | Input corpus hash | Corpus after | project/default/promoted | survivors / 24 | survival | measured spread | promoted | rejected near-dup / hash / cap |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | `9dbcea85` | 15 | 4 / 8 / 3 | 5 | 20.8% | 0.869214 | 3 | 2 / 0 / 0 |
| 2 | `8b624d80` | 20 | 4 / 8 / 8 | 13 | 54.2% | 0.849043 | 5 | 8 / 0 / 0 |
| 3 | `cfbd74ec` | 20 | 4 / 8 / 8 | 13 | 54.2% | 0.849043 | 0 | 13 / 0 / 0 |
| 4 | `cfbd74ec` | 21 | 4 / 8 / 9 | 11 | 45.8% | 0.916302 | 1 | 10 / 0 / 0 |
| 5 | `c108a276` | 21 | 4 / 8 / 9 | 12 | 50.0% | 0.916302 | 0 | 12 / 0 / 0 |

The curve does **not** show a monotonic survival improvement: `20.8%, 54.2%, 54.2%, 45.8%,
50.0%`. Measured spread also dips and plateaus, but ends wider: **0.869214 → 0.916302
(+0.047089)**. This five-season run therefore found no collapse toward a mean, but it is not evidence
that collapse cannot happen at a different seed or policy. The current recommendation is to keep the
novelty floor and add a small, explicitly reviewable outlier quota in a later policy revision—promote
some distant survivors even when their quality score is merely acceptable—rather than tune this run
to look smoother. The owner still needs the preview before defining “good”; promotion is the dilution
mechanism, not that missing judgment.

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

The complete 253-descriptor spread table is emitted by the automation test, including authored and
synthetic operation-default sample counts, min/max/mean/standard deviation, excluded/reflected
status, and clamp metadata. The current run has **212 zero-range rows** and **41 rows with real
spread**. The shape-spread rows below are retained from the prior report; the newly measured
terrain-detail spread is listed explicitly after the liveness experiment.

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

The 34 terrain-detail fields now have the following effective spread. `N` is shown as
`authored+op-defaults=effective`; the TunnelNetwork group has 3 authored vectors and Underwater
has 1. The default seed is present only for the 23 live fields, one matching operation type per
field. The values are the test's population statistics (the operation shape defaults happen to be
identical to the FStrate defaults, so only activation fields have non-zero spread here).

| Field | Tunnel N | Tunnel min..max | Tunnel mean | Tunnel SD | Underwater N | Underwater min..max | Underwater mean | Underwater SD | Result |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---|
| TerraceStepHeight | 3+1=4 | 0..5 | 1.25 | 2.16506351 | 1+1=2 | 0..5 | 2.5 | 2.5 | LIVE |
| TerraceHardness | 3+1=4 | 0.5..0.5 | 0.5 | 0 | 1+1=2 | 0.5..0.5 | 0.5 | 0 | LIVE |
| TerraceNoiseDisplacement | 3+1=4 | 0.5..0.5 | 0.5 | 0 | 1+1=2 | 0.5..0.5 | 0.5 | 0 | LIVE |
| LayerLineSpacing | 3+1=4 | 0..4 | 1 | 1.73205081 | 1+1=2 | 0..4 | 2 | 2 | LIVE |
| LayerLineDepth | 3+1=4 | 0.300000012..0.300000012 | 0.300000012 | 0 | 1+1=2 | 0.300000012..0.300000012 | 0.300000012 | 0 | LIVE |
| OverhangStrength | 3+1=4 | 0..0.5 | 0.125 | 0.216506351 | 1+1=2 | 0..0.5 | 0.25 | 0.25 | LIVE |
| OverhangDepth | 3+1=4 | 5..5 | 5 | 0 | 1+1=2 | 5..5 | 5 | 0 | LIVE |
| OverhangFrequency | 3+1=4 | 0.0599999987..0.0599999987 | 0.0599999987 | 0 | 1+1=2 | 0.0599999987..0.0599999987 | 0.0599999987 | 0 | LIVE |
| RibbingSpacing | 3+1=4 | 0..3 | 0.75 | 1.29903811 | 1+1=2 | 0..3 | 1.5 | 1.5 | LIVE |
| RibbingDepth | 3+1=4 | 0.400000006..0.400000006 | 0.400000006 | 0 | 1+1=2 | 0.400000006..0.400000006 | 0.400000006 | 0 | LIVE |
| CliffStrength | 3+1=4 | 0..0.5 | 0.125 | 0.216506351 | 1+1=2 | 0..0.5 | 0.25 | 0.25 | LIVE |
| ScallopStrength | 3+1=4 | 0..0.5 | 0.125 | 0.216506351 | 1+1=2 | 0..0.5 | 0.25 | 0.25 | LIVE |
| ScallopFrequency | 3+1=4 | 0.100000001..0.100000001 | 0.100000001 | 0 | 1+1=2 | 0.100000001..0.100000001 | 0.100000001 | 0 | LIVE |
| ArchDensity | 3+1=4 | 0..0.100000001 | 0.0250000004 | 0.0433012708 | 1+1=2 | 0..0.100000001 | 0.0500000007 | 0.0500000007 | LIVE |
| ArchMinRadius | 3+1=4 | 3..3 | 3 | 0 | 1+1=2 | 3..3 | 3 | 0 | LIVE |
| ArchMaxRadius | 3+1=4 | 6..6 | 6 | 0 | 1+1=2 | 6..6 | 6 | 0 | LIVE |
| ColumnDensity | 3+0=3 | 0..0 | 0 | 0 | 1+0=1 | 0..0 | 0 | 0 | DEAD |
| ColumnMinRadius | 3+0=3 | 2..2 | 2 | 0 | 1+0=1 | 2..2 | 2 | 0 | DEAD |
| ColumnMaxRadius | 3+0=3 | 5..5 | 5 | 0 | 1+0=1 | 5..5 | 5 | 0 | DEAD |
| PitDensity | 3+0=3 | 0..0 | 0 | 0 | 1+0=1 | 0..0 | 0 | 0 | DEAD |
| PitMinRadius | 3+0=3 | 4..4 | 4 | 0 | 1+0=1 | 4..4 | 4 | 0 | DEAD |
| PitMaxRadius | 3+0=3 | 10..10 | 10 | 0 | 1+0=1 | 10..10 | 10 | 0 | DEAD |
| PitDepth | 3+0=3 | 25..25 | 25 | 0 | 1+0=1 | 25..25 | 25 | 0 | DEAD |
| ChimneyDensity | 3+0=3 | 0..0 | 0 | 0 | 1+0=1 | 0..0 | 0 | 0 | DEAD |
| ChimneyMinRadius | 3+0=3 | 2..2 | 2 | 0 | 1+0=1 | 2..2 | 2 | 0 | DEAD |
| ChimneyMaxRadius | 3+0=3 | 5..5 | 5 | 0 | 1+0=1 | 5..5 | 5 | 0 | DEAD |
| ChimneyHeight | 3+0=3 | 20..20 | 20 | 0 | 1+0=1 | 20..20 | 20 | 0 | DEAD |
| DomeDensity | 3+1=4 | 0..0.150000006 | 0.0375000015 | 0.0649519079 | 1+1=2 | 0..0.150000006 | 0.075000003 | 0.075000003 | LIVE |
| DomeMinRadius | 3+1=4 | 8..8 | 8 | 0 | 1+1=2 | 8..8 | 8 | 0 | LIVE |
| DomeMaxRadius | 3+1=4 | 15..15 | 15 | 0 | 1+1=2 | 15..15 | 15 | 0 | LIVE |
| DomeHeightRatio | 3+1=4 | 0.800000012..0.800000012 | 0.800000012 | 0 | 1+1=2 | 0.800000012..0.800000012 | 0.800000012 | 0 | LIVE |
| PinchDensity | 3+1=4 | 0..0.150000006 | 0.0375000015 | 0.0649519079 | 1+1=2 | 0..0.150000006 | 0.075000003 | 0.075000003 | LIVE |
| PinchStrength | 3+1=4 | 5..5 | 5 | 0 | 1+1=2 | 5..5 | 5 | 0 | LIVE |
| PinchLength | 3+1=4 | 12..12 | 12 | 0 | 1+1=2 | 12..12 | 12 | 0 | LIVE |

The first pass excluded all **34** terrain-detail fields. That was an unmeasured structural claim,
so `VoxelForge.Composer.TerrainDetailLiveness` now tests it directly. For both the legacy and
operator-stack `GetDensityAt` paths, it uses a fixed **4,096-point** lattice (16×16×16, 4-voxel
spacing, the TunnelNetwork fixture), clears the terrain-operation pool, records a baseline, then
changes only one operation group to substantial values. The two paths produced bit-identical
results. The aggregate verdict is **9 live groups / 23 live fields** and **3 dead groups / 11 dead
fields**:

| Operation group | Changed samples / 4,096 | Sign changes | Sum abs delta | Max abs delta | Verdict |
|---|---:|---:|---:|---:|---|
| Terrace | 1,314 | 40 | 2,177.12156 | 5.09695816 | LIVE |
| LayerLines | 532 | 8 | 163.082606 | 0.64941144 | LIVE |
| Overhang | 569 | 12 | 598.018486 | 4.41974068 | LIVE |
| Ribbing | 933 | 6 | 213.929947 | 1.49233627 | LIVE |
| Cliff | 391 | 3 | 372.185603 | 2.80882835 | LIVE |
| Scallop | 38 | 0 | 5.78068841 | 1.01755953 | LIVE |
| Arch | 418 | 190 | 7,327.12817 | 36.0000019 | LIVE |
| Column | 0 | 0 | 0 | 0 | DEAD |
| Pit | 0 | 0 | 0 | 0 | DEAD |
| Chimney | 0 | 0 | 0 | 0 | DEAD |
| Dome | 227 | 44 | 3,262.50304 | 24.0000019 | LIVE |
| Pinch | 226 | 22 | 1,217.39489 | 16.3533421 | LIVE |

The result is identical for both paths, so each row above represents two measured rows. The
baseline mean was `-3.86253725` for every group. Variant means were, respectively, `-3.8647801`,
`-3.82272216`, `-4.00853786`, `-3.91476624`, `-3.86411419`, `-3.86112595`, `-5.6513869`,
`-3.86253725`, `-3.86253725`, `-3.86253725`, `-3.06602772`, and `-4.1597528` in the table's
order. The live fields are therefore removed from the exclusion list and are now eligible for the
roll. Only the 11 direct Column/Pit/Chimney transport fields plus the two runtime Z bounds remain
excluded.

The dead-field call chain is concrete. `UVoxelStrateManager::BuildParamsFromDefinition`
(`Private/VoxelStrateManager.cpp:1399-1404`) returns only `Definition->GenerationParams`; it does
not merge terrain-op assets. `VoxelGenerator::GetDensityAt` (`Private/VoxelGenerator.cpp:1099-1109`)
gets the active definition's `TerrainOperations` pool and passes it to
`VoxelCaveMorphology::BuildChunkCache`. The cache applies the selected
`UVoxelTerrainOpDefinition` at `Private/VoxelCaveMorphology.cpp:1741`, then consumes
`PitDensity`/shape at `:1744-1764`, `ChimneyDensity`/shape at `:1767-1787`, and
`ColumnDensity`/shape at `:1790-1803`. Sampling uses the resulting `SDFCache.Pits` and
`SDFCache.Chimneys` at `Private/VoxelGenerator.cpp:1147-1206` and `SDFCache.Columns` at
`:1730-1752`; it does not read the direct strata Column/Pit/Chimney slots. The project contains
exactly **1** `UVoxelTerrainOpDefinition` asset, `Content/VoxelForge/Blocks/NewDataAsset.uasset`,
so a second asset corpus would have one member and no useful spread.

The composer now seeds each of the 23 live fields with **one matching per-operation default** from
the `UVoxelTerrainOpDefinition` header (applied through `ApplyTo`, with no duplicated constants),
then adds any authored strate values. The current TunnelNetwork group has 3 authored samples + 1
operation-default sample; Underwater has 1 + 1. The dead fields receive no synthetic seed and stay
excluded. The complete test table exposes `authored`, `op-defaults`, and effective `samples`.

Recommendation for the dead trio: do **not** build an asset corpus for the one current asset. The
right future composer abstraction is direct terrain-op parameter structs/configurations, seeded by
the per-operation defaults and any authored op values, with the rolled op payload carried alongside
`FVoxelStrateRollInfo` and injected into the transient candidate's terrain-op pool. That requires a
small candidate/materialisation API and a populated-pool liveness/box test; simply jittering the
current direct FStrate slots would remain a silent no-op. This task rolls every field that the
existing direct strata path can actually move and leaves the dead trio neutral until that API exists.

The bool policy is explicit: `bTunnelsFlowTowardOrigin` is inherited from the dominant parent.
`OriginRoomMaxConnections` and `RoughnessNoiseType` retain `Lerp`'s existing SNAP behavior and
are not jittered. In this editor build clamp metadata was available while the table was built; it
is **not promised in a cooked runtime**, so the commandlet/cook-time bake remains owed.

The controlled 64-candidate run (step 4 sweep, radius 256, max 8,000,000 cells, fixture seed 1337)
kept the existing corpus hash/RNG stream while enabling the live detail fields. It produced
**59/64 survivors (92.2%)**: 60 non-vacuous candidates, 60 with largest component share ≥ 0.50,
63 exact unsnapped arrival→departure law passes, and no roll failures. The complete 64-row table
is emitted by `VoxelForge.Composer.ParameterRoll`; each row reports archetype, weighted parents,
air fraction, largest share, walkable fraction, feature scale, and arrival→departure verdict.
Feature scale was **0→376** (the prior `190ff38` run was **4→376**), while walkable fraction was
**0.000000→0.388506** (prior **0.000→0.389**). Thus detail variation widened the feature-scale
range only at the low end by four voxels; it did not increase the upper scale or the walkable
spread. The 17 TunnelNetwork/Underwater candidates all carried at least one non-zero live detail
activation, and each of Terrace/LayerLines/Overhang/Ribbing/Cliff/Scallop/Arch/Dome/Pinch appeared
non-zero in all 17. The measurement is therefore not broadly sensitive to these detail fields in
this 64-candidate corpus, which is an empirical result, not a reason to remove the live knobs. The
controlled run took **113.317 s** total (**0.015 s** corpus load, **113.300 s** roll/measurement).

The §6.2 check remains non-vacuous: **1,228 boxes proved**, **1,634,468 voxels checked**, and
**0 violations**. Its verdict mix was 1,332 Mixed, 553 AllSolid, and 675 AllAir; no candidate
violated the box law. The box law used unit step and brute-forced every voxel of each uniform box,
while the candidate sweep retained step 4. The test asserts zero violations and keeps the law open
to future changed candidates rather than treating this clean run as a license to tune the survival
rate.

### Season-zero bootstrap follow-up (2026-09-04)

The homogeneous-field trap is now handled explicitly. A field whose measured corpus range is at
or below `1e-6` takes a **bootstrap** jitter of ±25% of its own magnitude, with a one-native-unit
floor for values near zero; fields with useful spread still take the real ±15% of `max-min` rule.
The safety clamps remain a final guard and never become roll ranges. `FVoxelStrateRollInfo` reports
both the number of bootstrap applications and the distinct `archetype:field` names. The comment in
the implementation makes the lifecycle explicit: this is a bootstrap for a starved season-zero
corpus, and Tier 5 promotion should retire it as the corpus grows.

The follow-up 64-roll measurement applied the bootstrap **1,422 times across 168 distinct fields**.
It did **not** broaden the measured distributions beyond the previous run: feature scale stayed
**0→376 voxels** and walkable fraction was **0.000000→0.286766**, below the previous **0.388506**
maximum. The bootstrap is therefore active, but these two measurements are insensitive to the
changed fields in this sample; this is more useful than pretending the new knob created visible
world variety. The run had **38/64 survivors**, 60 non-vacuous candidates, 55 meeting the largest
component threshold, and 42 exact law passes; those counts are observations, not tuning targets.

The wider structure-roll jitter remained sound: **659 boxes proved**, **877,129 voxels checked**,
and **0 violations** (1,901 Mixed, 266 AllSolid, 393 AllAir). The parameter-roll box check likewise
proved **1,204 boxes**, checked **1,602,524 voxels**, and found **0 violations** (1,356 Mixed, 608
AllSolid, 596 AllAir). A violation would reopen §6.2; this run did not.

### Corpus-free measurement (2026-09-04)

The proposed alternative to copying correlations from examples was measured directly. The structure
recipe is shared by all three arms, so only parameter provenance changes:

* **Corpus blend (today):** the existing exact-archetype weighted parent blend, ±15% spread jitter,
  and final metadata clamps.
* **Naive uniform:** every live scalar is independently uniform in the same finite corpus-free
  envelope. No authored vector, default vector, blend, or corpus spread is read.
* **Constraint-sampled:** independent geometry quantities are drawn; dependent quantities are
  calculated from the relations below, then the same envelope is applied as a final safety net.

There is an important wording issue in Part A: most fields have only `ClampMin` and no finite
`ClampMax` (`VoxelStrateTypes.h` has 135 minimums and 52 maximums across 216 UPROPERTYs), while
the native tunnel fields have no reflected metadata. “Uniform within clamps” is therefore undefined
for much of the parameter space. For this measurement, `VF_GetCorpusFreeRange` declares finite
H-relative operational envelopes for those fields; it does not use a corpus value or a default value
as a range. The exact envelope is in `VoxelStrateComposer.cpp:1087-1278`, and both corpus-free arms
use it so the comparison is fair. This makes the naive result conditional on an explicit finite
support, which is a spec caveat rather than a hidden tuning target.

#### Declared relations

These are sufficient conservative relations for the experiment, derived from the existing generator
equations. They are not claimed to be invariants of every existing authored asset; the audit below
checks that question rather than assuming it.

| relation | declaration used by `ConstraintSampled` | code that supplies the geometry/math |
|---|---|---|
| Room placement, overlap, and isolation | Let `S=RoomSpacing`, `D=RoomDensity`, and `R=MaxRoomRadius`. Draw target area coverage `C = πR²D/S²` in `0.08..0.18`; derive `R = S*sqrt(C/(πD))`; require `C∈[0.02,0.35]` and `R≤S/2`. The coverage interval is an explicit experiment support for the room occupancy equation, not a generator hard limit. | `VoxelCaveMorphology.cpp:1305-1326` rolls room existence, jitter, and radius; `VoxelCaveMorphology.cpp:1319-1320` gives the `[0.15,0.85]` cell jitter. Implemented/validated at `VoxelStrateComposer.cpp:1522-1540` and `:3094-3104`. |
| Tunnel reach | Neighboring-room horizontal jitter can be `1.7S` in each axis, hence `1.7S√2`; vertical center separation can be `2*R*RoomHeightRatio`. Derive `MaxTunnelLength` as at least the Euclidean sum of those bounds. This matches the generator’s distance gate. | `VoxelCaveMorphology.cpp:1388` and `:1502` reject out-of-reach links; implemented/validated at `VoxelStrateComposer.cpp:1557-1564` and `:3114-3121`. |
| Room vertical fit | `BoundarySealThickness + max(MaxRoomRadius,OriginRoomRadius)*RoomHeightRatio < H/2`, because room centers are restricted to the seal-free interval and the room Z radius is `Radius*RoomHeightRatio`. | `VoxelCaveMorphology.cpp:137-140` and `:248-250` establish the room/seal buffer; validated at `VoxelStrateComposer.cpp:3106-3112`. |
| Slab floor/ceiling | Worst floor is `H*FloorRelativeHeight + 1.25*FloorRoughness`; worst ceiling is `H*CeilingRelativeHeight - 1.25*CeilingRoughness`. Derive both relative heights from seal + clearance and require a two-voxel void. | `VoxelCaveMorphology.cpp:329-362` and `VoxelGenerator.cpp:2175-2212`; implemented/validated at `VoxelStrateComposer.cpp:1602-1626` and `:3143-3185`. |
| Maze corridor | For a 25 cm voxel, derive `3/0.7 = 4.286 m` clear diameter for two-abreast floor, hence an 8.571-voxel clear radius. Add the proven `SurfaceRoughness*VOXEL_NOISE_SCALE*1.5 = 3.75` voxels and round to `CorridorRadius=12.5`; require `CellSize=64` so the worst-case bore gap remains `64−2*(12.5+3.75)=31.5` voxels. | `VoxelCaveMorphology.cpp` bounds the lattice and applies the roughness-safe landing; `VoxelGenerator.cpp` applies roughness and subtracts `2*BaseDensity`; implemented/validated at `VoxelStrateComposer.cpp:1629-1642` and `:3188-3216`. |
| Surface ground/cap and height features | The structural height is bounded by `ElevationRange*(1+MountainStrength)+2*1.25*SurfaceRoughness`. Add conservative budgets for cliff displacement (`CliffStrength*CliffSharpness` times that spread), terrace step, layer depth, and beach width. Derive `BaseGroundRelative` above the bottom seal and `CeilingRelative` below the top seal with cap roughness, undulation, ridge, overhang height, and two-voxel headroom. Cliff is sampled as a scalar and reduced only when this derived height budget cannot contain it. | `VoxelGenerator.cpp:2470-2517` is the structural height equation; `:2537-2557` is the cliff remap; `:2607-2654` is the cap; `:2670-2686` caps overhang height. Implemented/validated at `VoxelStrateComposer.cpp:1645-1724` and `:3219-3269`. |
| Vertical shafts and ledges | Require `ShaftMaxRadius≤ShaftSpacing/2`; require `ShaftMinRadius > 1.5*1.25*SurfaceRoughness + 0.25`; derive `ConnectorRadius > 1.5*1.25*SurfaceRoughness + 1`; derive ledge spacing greater than `2*(LedgeDepth + floor-clearance)`. | `VoxelCaveMorphology.cpp:628-654` moves landings out of roughness/ledge bands; `VoxelGenerator.cpp:3944-3954` derives tree roughness reach and `:4209-4214` applies ledges; implemented/validated at `VoxelStrateComposer.cpp:1730-1755` and `:3272-3310`. |
| Floating islands | Require `IslandMaxRadius≤IslandSpacing/2`; derive island radius and thickness so `BoundarySealThickness + max(0.2R,ThicknessRatio*R) < H/2`; require the minimum radius to exceed roughness plus SDF blend. | `VoxelCaveMorphology.cpp:852-935` computes island radius, underside, warp, and blend pad; `VoxelGenerator.cpp:4268-4344` starts with void and fills the island; implemented/validated at `VoxelStrateComposer.cpp:1757-1780` and `:3313-3355`. |
| Density sign/carve strength | Every solid-starting family samples `BaseDensity>0`; tunnel worm strength is derived above base density, and the maze/shaft/island paths use the existing ±`2*BaseDensity` carve/fill convention. | `VoxelGenerator.cpp:1059`, `:2085`, `:2359-2432`, `:3944-3954`, `:4200`, and `:4272`; the relation checks are in `VoxelStrateComposer.cpp:3029-3355`. |

Bools and enums are categorical, not arithmetic constraints. `bTunnelsFlowTowardOrigin` is a fair
50/50 draw; `OriginRoomMaxConnections` is an integer draw in the declared `0..12` support; and
`RoughnessNoiseType` is a uniform draw over all four enum values. Reflected boolean fields in the
sibling families are also fair 50/50. There are currently no non-tunnel reflected enums. The
category stream is separate from the scalar stream so neither arm’s scalar coupling changes the
categorical policy. The sampler is deterministic in `(archetype, seed, index, mode, H)`, uses no
retained RNG state, and clamps only after dependent equations (`VoxelStrateComposer.cpp:1782-1841`).

#### The actual three-way result

The requested **256×3** run was started and stopped after roughly six minutes without reaching an
aggregate report. A 64×3 attempt (including a repeat after law traversal was made conditional on
passing the fragmentation gate) also exceeded roughly 27 minutes without an aggregate report. Those
partial runs are not data. The completed equal-arm run therefore used **16 candidates per arm**;
all three arms ran the same 16 structure recipes, step-4 measurement over radius 256, `MaxCells=
8,000,000`, and one fixed arrival/departure pair. Criterion precedence is **vacuous → fragmented
(`LargestComponentShare < 0.50`) → law failed**. Law is intentionally skipped for fragmented
candidates because the earlier criterion already decides the candidate; `law skipped` is reported
separately. The completed test took **22.995 s** internally (**22.980 s** in roll/measurement).

| approach | survival | vacuous | fragmented | law failed | air range | walkable range | feature scale range |
|---|---:|---:|---:|---:|---:|---:|---:|
| corpus blend (today) | 13/16 (81.2%) | 0 | 1 | 2 | 0.003291..0.999878 | 0.000000..0.318692 | 4..332 |
| naive uniform | 7/16 (43.8%) | 3 | 3 | 3 | 0.003471..1.000000 | 0.000000..0.163436 | 0..152 |
| constraint-sampled | 7/16 (43.8%) | 3 | 5 | 1 | 0.002636..1.000000 | 0.000000..0.220178 | 0..52 |

The intermediate gates were corpus `16 non-vacuous / 15 largest-enough / 13 law passes`, naive
`13 / 10 / 7`, and constrained `13 / 8 / 7`. Observed law failures were 2, 3, and 1
respectively; the other candidates that did not reach the law were the reported fragmented cases.
No roll, build, measurement, stack-count, channel-order, or context failure occurred.

The constrained survivors are valid but not as good as the corpus survivors in this run:

| approach | survivor count | air range (mean) | walkable range (mean) | feature scale range (mean) |
|---|---:|---:|---:|---:|
| corpus blend (today) | 13 | 0.022004..0.999878 (0.801809) | 0.000000..0.318692 (0.066071) | 4..332 (92.307693) |
| naive uniform | 7 | 0.542744..0.996999 (0.856300) | 0.000068..0.044617 (0.012566) | 12..152 (64.571426) |
| constraint-sampled | 7 | 0.801857..0.976726 (0.886178) | 0.000237..0.047382 (0.012851) | 16..52 (31.428572) |

The constraint arm tied naive survival at 7/16, traded three naive law failures for five
fragmentation failures, and produced a higher mean air fraction but dramatically lower walkable and
feature-scale ranges than the corpus survivors. With only seven survivors in each corpus-free arm,
this is evidence of a quality gap, not a claim about a stable population distribution; it is already
enough to reject “technically valid means equivalent.”

#### Box-law and authored-corpus audit

Every arm performed 40 box probes per candidate. Non-`Mixed` verdicts were brute-forced over the
same unit lattice: **0 violations** in all arms.

| approach | Mixed | AllSolid | AllAir | proved | voxels checked | violations |
|---|---:|---:|---:|---:|---:|---:|
| corpus blend (today) | 444 | 69 | 127 | 196 | 260,876 | 0 |
| naive uniform | 330 | 168 | 142 | 310 | 412,610 | 0 |
| constraint-sampled | 378 | 90 | 172 | 262 | 348,722 | 0 |
| **total** | **1,152** | **327** | **441** | **768** | **673,486** | **0** |

The constraint sampler produced **0 declared-parameter violations** and stopped-on-violation count
0. `WorldRadiusVoxels` remained **0**. The shared structure recipe had 0 determinism failures;
each arm had 0 parameter-roll determinism failures, 0 invalid stack counts, 0 channel-order
failures, and 0 context violations.

The same declared relations were audited against all 12 current corpus vectors. **3/12 violate**
the conservative experiment relations:

* `DA_Strate3` (`TunnelNetwork`, H=128): room area coverage `2.68447`, above the declared `0.35`
  upper support.
* `Default_SurfaceWorld` (`SurfaceWorld`, H=128): conservative ground lower bound `-15.35`,
  below the bottom seal `4`.
* `Default_VerticalShafts` (`VerticalShafts`, H=128): minimum shaft radius `5`, below the roughness
  margin `5.875`.

These are findings about the proposed sufficient relations, not proof that those authored worlds
are visibly broken: two are deliberately worst-case bounds and the first is an occupancy heuristic.
The corpus is living outside the new constraint envelope in three places, so the envelope cannot be
silently promoted to a universal validator without either weakening it or changing those assets.

#### Verdict

The corpus is still load-bearing for quality. Keep the corpus-blend arm in production and use the
constraint sampler as a corpus-free exploratory/bootstrap arm whose passing candidates can enter
Tier 5 promotion. Do not replace the production arm with naive uniform rolling. The data does not
justify dropping the handmade vectors today; it supports keeping them while promotion gradually
dilutes their share. A future larger sample is warranted after optimizing the fixed law/box pass,
but it should be treated as confirmation, not as permission to tune these observed counts.

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
on top. The measurement hand-off and deterministic preview are now built; they are needed anyway to
review a season before publishing.

### ✅ F1 measurement + preview built (2026-09-04)

`FVoxelStrateSampleGrid` is an opt-in export of both the exact `Air` polarity and the scalar `Density`
values already sampled by `VF_MeasureStrate` / `VF_MeasureStrateWithSampler`. The editor/automation
renderer consumes that buffer and never calls a density sampler, so each candidate's metrics and images
come from one window and one sample pass. The filled-cell view is paired with a marching-squares-style
`density = 0` contour from the scalar field. The contour is not an anti-aliased or blurred coarse image:
it is a legibility aid alongside the honest sampled-cell evidence.

The step-4 measurement and 64-card contact sheet are unchanged in scope. Each card now shows the coarse
filled and contour XZ/XY pairs. `VoxelForge.Composer.StructureRoll` writes them, plus an optional fine
pass, to:

`Saved/ComposerPreview/structure_seed_0_corpus_9dbcea85_step_4_radius_256_fine_step_1_fine_radius_64/`

The fine pass is explicit and bounded: the historical composer selected the 42 coarse survivors, sampled
step 1 in a radius-64 (128×128 XY) ROI, and refused before allocation when `MaxCells=2,000,000` would be
exceeded. The default is radius 64 because it stays around the observed multi-million-cell budget while exposing
voxel-scale detail; radius 128 is already roughly 7.1–7.5M cells and radius 256 exceeds the 8M coarse
cap. The public settings let another editor/automation caller choose step, radius, centre, margin, and
cap, or the writer can be called for an explicitly selected list. Fine filenames, captions, and each
card's text include the sample step and exact resolved XY/Z window, so they cannot be mistaken for the
coarse render. A refused fine render is shown as a refusal with its reason rather than silently omitted.

The fine ROI is now centred on each survivor's deterministic `LargestComponentPoint` from the coarse
air flood fill and is labelled **“largest open space”** on the page. It is not the middle of the
strate and not the world origin. In the final 42-survivor run, the origin-centred baseline had **6/42**
uniform fine plan projections (and 6/42 complete two-view cards); the largest-open-space pass had
**3/42** in both counts. The reduction is real but not total: a largest open space can itself be
larger than a radius-64 ROI, so a uniform card is not proof that the strate is empty.

The vertical image is an XZ section through the sampled cell nearest the strate-window centre Y. The
plan image scans every sampled Z layer, keeps layers containing both air and solid, and chooses the
one with the largest number of XY solid/air boundary transitions; ties prefer more mixed cells, more
walkable cells, then the layer nearest the vertical centre. If no mixed layer exists it chooses the
most solid fallback layer, so a slab does not silently produce a blank mid-height sheet. The focused
run's first candidate was **128×48** for coarse vertical, **128×130** for fine vertical, and **128×148**
for both plan images; dimensions vary with the resolved Z window. Both views use dark solid, light air,
orange walkable cells, a turquoise scalar contour, and a one-chunk footer scale bar. The page states
**one chunk = 32 voxels = 8 m**, the density sign (`> 0` is air), every exact window, the 512-pixel cap
(including a 20-pixel footer), and that larger source grids aggregate already-sampled cells rather than
resampling. It also carries the required caveat: a 2D apparent join is not a 3D connectivity proof;
the separate arrival→departure verdict is the connectivity check.

The final run measured **0.300 s** for the coarse render, **133.272 s** for the origin-plus-centred
fine scalar samples, and **1.113 s** for their filled/contour rendering. Peak fine `Air` + `Density`
capture allocation was **9,338,880 bytes** and the peak single RGBA raster was **75,776 bytes**. The
full StructureRoll test completed in **243.649 s**, with 64 coarse filled pairs, 64 coarse contour
pairs, 42 fine filled pairs, 42 fine contour pairs, and 0 refusals.

### ✅ Morning archetype showcase + walkable floor-area audit (2026-09-05)

The owner-facing review artifact is Saved/VoxelForge/Showcase/index.html. It is generated only by
the editor automation test VoxelForge.Composer.Showcase; opening it is offline and has no runtime
generation hook. It contains one alphabetized card for each native archetype, with the exact four PIE
hand-off values, recipe, coarse step-4 filled/contour XZ+XY views, and a separate step-1 filled/contour
ROI centred on that card's coarse LargestComponentPoint. The fine ROI is bounded at radius 64 and
MaxCells=4,000,000; a refusal or blank ROI is written as text rather than represented by an empty
image. The earlier bounded four-chunk run rendered all eight fine views; the final production-height
run is recorded below because its larger window produced **7** fine renders, **1** refusal, and **1**
blank diagnostic card.

The Part A table adds three measurements to the original volumetric walkability ratio:

* WalkableFloorAreaFraction = distinct sampled XY columns containing at least one walkable cell /
  the sampled XY footprint. It is an area/footprint proxy, not a continuous world-area integral.
* MedianVerticalClearance is the lower median of the sampled air run above walkable cells, reported
  in voxels. Both the walkability predicate and this median use the stated sample step.
* LargestWalkableSurfaceShare is the largest 4-neighbour connected component of the projected
  walkable columns / all walkable columns. It is a 2D surface-continuity indicator, not a proof that
  floors at different Z values connect in 3D; the separate exact unsnapped arrival→departure route
  check remains the traversal gate.

The showcase selection exhausts seeds **0** and **7331**, candidate indices **0–63**, and excludes
multi-region rolls while VF_LateralRegionsAreShippable() is false. The hard gates are non-vacuous,
largest air component share ≥ **0.50**, and exact unsnapped arrival→departure connectivity. Among
those survivors the deterministic score is floor_area + 0.25*clamp(clearance/64,0,1) +
0.10*largest_surface; floor area is primary, clearance and projected continuity are tie-breakers.
The fixture and PIE hand-off use the project generator seed **0**, bComposerRollStructure=false,
and leave the existing slot layout, passage endpoints, placement, and WorldRadiusVoxels=0 intact.

The following is the **pre-scale baseline** captured before the 2026-09-05 human-scale defaults
(the post-scale diagnostic table is below). Each card prints its own exact derived-margin Z window;
common XY window
is [-256,256) × [-256,256) voxels, step **4**, HeadroomCells=2, MaxCells=8,000,000):

| Archetype | walkable fraction | floor area fraction | median clearance | largest surface share |
|-----------|------------------:|--------------------:|-----------------:|----------------------:|
| CrystalChamber | 0.059105 | 0.999878 | 68 | 1.000000 |
| FlatPlain | 0.056107 | 0.999878 | 72 | 1.000000 |
| FloatingIslands | 0.018869 | 0.400146 | 36 | 0.678005 |
| Maze | 0.286020 | 0.290466 | 8 | 0.990544 |
| SurfaceWorld | 0.064233 | 0.937378 | 64 | 0.993619 |
| TunnelNetwork | 0.155047 | 0.208374 | 20 | 0.463972 |
| Underwater | 0.228995 | 0.206055 | 12 | 0.331161 |
| VerticalShafts | 0.108252 | 0.113770 | 12 | 0.144850 |

The result is mixed, and that is the finding. CrystalChamber, FlatPlain, and SurfaceWorld retain
roughly **0.94–1.00** walkable XY footprint while their volumetric walkable ratios are only
**0.056–0.064**: the original ratio was primarily measuring corridor occupancy and was the wrong
quality target for broad slab/chamber floors. Maze is genuinely corridor-rich: its ratio and floor
area are both about **0.29**, and **0.991** of its projected walkable surface is in the largest
component. Conversely, TunnelNetwork, Underwater, and VerticalShafts have only **0.114–0.208** floor
area and **0.145–0.464** largest-surface share; FloatingIslands is intermediate (**0.400** area,
**0.678** largest share). Those are real fragmentation/ledge findings, not just a strict-volume
artifact. All selected cards pass the separate mouth-to-mouth route check, so “walkable area” still
does not mean every walkable patch is on the traversable route.

The web sanity check did not produce a defensible universal “good” floor-to-volume threshold. The
official NPS descriptions instead support a heterogeneous target: solution caves grow along connected
fractures and streams, passages branch and join, and caves can contain multiple levels, narrow
walking passages, rooms, and vertical pits. NPS's Lehman Cave dimensions give a useful scale example
(the Talus Room is reported as 90 ft wide × 376 ft long, with 21,511 sq ft of floor area and 113 ft
floor-to-ceiling), not a universal ratio. The official Deep Rock Galactic cave-design note likewise
describes rooms connected by narrow winding tunnels, while the Team Cherry map-design interview
emphasizes interconnected spaces and backtracking. References: NPS Solution Caves
(https://www.nps.gov/subjects/caves/solution-caves.htm), NPS Mammoth Cave morphology
(https://home.nps.gov/maca/learn/nature/how-mammoth-cave-formed.htm), NPS Lehman Cave dimensions
(https://www.nps.gov/grba/learn/nature/lehman-caves-dimensions.htm), Deep Rock Galactic procedural
cave design
(https://store.steampowered.com/news/posts/?appgroupname=Deep+Rock+Galactic&appids=548430&enddate=1729500950&feed=steam_community_announcements),
and Team Cherry map design (https://www.pcgamer.com/how-to-design-a-great-metroidvania-map/).

#### Historical post-scale measurement before the Maze spanning-tree correction (2026-09-05)

The new defaults are applied in `VoxelStrateTypes.h`; the production definition, season defaults, and
the owner-facing showcase use `StrateHeightInChunks=8` (64 m). The bounded test fixture remains at
four chunks so the broad regression suite stays finite; this is a test-budget distinction, not a
second production default. The final focused run is
`Saved/AutomationReports/WorldScaleShowcaseProductionFinal/index.json`, with 256 roll evaluations,
82 eligible single-region evaluations, 174 lateral-region evaluations skipped while that gate is off,
and a **0/8** complete hard-gate survivor set. It still wrote fresh diagnostic cards: 7 fine renders,
1 refusal, and 1 blank card.

The before/after values below are deliberately labelled as candidate measurements, not paired
experiments: the before rows are the selected pre-scale candidates from the baseline report, while
the after rows are the best measured post-scale candidates from the production-height search. Floor
area is the coarse 128 m × 128 m projected-footprint proxy (16,384 m² maximum); the ratio is the
candidate's reported roughness-to-feature ratio. A zero or unresolved player law is a result, not a
substituted value.

| Archetype | Before: fit / traversable / law | Before: floor m² / median clearance m / roughness ratio | After: fit / traversable / law | After: floor m² / median clearance m / roughness ratio |
|---|---|---|---|---|
| CrystalChamber | 0.011745 / 0.810378 / NotConnectedAtThisResolution | 16,382 / 17 / 1.813 floor, 1.796 ceiling | 0.002699 / 0.017288 / NotConnectedAtThisResolution | 16,384 / 35 / 0.847 |
| FlatPlain | 0.003938 / 0.053571 / NotConnectedAtThisResolution | 16,382 / 18 / 1.840 floor, 2.308 ceiling | 0.000000 / 0.000000 / unresolved | 16,343 / 43 / 1.092 |
| FloatingIslands | 0.000000 / 0.000000 / StartCellNotPlayerFit | 6,556 / 9 / 0.025 | 0.000000 / 0.000000 / StartCellNotPlayerFit | 4,577 / 29 / 0.283 |
| Maze | 0.010523 / 0.136646 / NotConnectedAtThisResolution | 4,759 / 2 / 0.938 | 0.016851 / 0.058757 / NotConnectedAtThisResolution | 6,115 / 3 / 0.423 |
| SurfaceWorld | 0.001027 / 0.029412 / NotConnectedAtThisResolution | 15,359 / 16 / 0.032 | 0.000627 / 0.008415 / NotConnectedAtThisResolution | 16,384 / 42 / 0.038 |
| TunnelNetwork | 0.004439 / 0.145833 / NotConnectedAtThisResolution | 3,414 / 5 / 1.436 | 0.001197 / 0.612245 / NotConnectedAtThisResolution | 6,153 / 9 / 0.383 |
| Underwater | 0.005251 / 0.160000 / NotConnectedAtThisResolution | 3,376 / 3 / 2.212 | 0.000000 / 0.000000 / OutOfWindow | 21 / 17 / 0.683 |
| VerticalShafts | 0.007458 / 0.233577 / NotConnectedAtThisResolution | 1,864 / 3 / 0.731 shaft, 1.302 connector | 0.009883 / 0.083045 / NotConnectedAtThisResolution | 2,718 / 4 / 0.622 |

The interpreted outcome is mixed. Nominal dimensions and median clearances grew where they should,
and TunnelNetwork's best measured candidate's traversable share rose substantially, but the global law
did not become connected and broad rooms can still have very low player-fit fractions. Maze improved
only modestly and remains disconnected. This is consistent with the radius sweep being a killed
single-cause hypothesis: width alone did not cure the route/mouth/resolution failures.

The direct default roughness arithmetic is stable and independent of which rolled candidate won:

| Archetype / feature | Arithmetic | Ratio |
|---|---|---:|
| TunnelNetwork, Underwater tunnel | reach `= 2 × 1.25 × 1.5 = 3.75 vox`; `3.75 / 6` minimum radius | 0.625 |
| Maze corridor | `3.75 / 8` | 0.469 |
| FlatPlain, CrystalChamber floor / ceiling columns | `7.50 / 12` and `11.25 / 12` using mean 12-voxel column radius | 0.625 / 0.938 |
| VerticalShafts shaft / connector | `3.75 / 8` and `3.75 / 6` | 0.469 / 0.625 |
| FloatingIslands | `5.625 / 24`; including 6-voxel SDF blend, `(5.625+6)/24` | 0.234 / 0.484 |
| SurfaceWorld relief | `3.75 / 80` terrain-relief scale | 0.047 |

The default derivations are not a global multiplier:

* Body and target anchors: `1 voxel = 0.25 m`; the capsule is `1.36 vox` radius and `7.04 vox`
  tall. A 2.5 m no-duck corridor is 10 voxels; the 3-4 m fight target is 12-16 voxels. An 8-15 m
  room is 32-60 voxels across, and a 20 m cathedral anchor is 80 voxels across. Epic's official
  Unreal blockout guidance uses a player-sized reference and gives roughly 2-3 m halls and 3-4 m
  heights as starting guidance; the [2010 ADA Standards](https://www.ada.gov/law-and-regs/design-standards/2010-stds/)
  provide a 915 mm accessible clear-width lower bound, not a combat-space target.
* TunnelNetwork/Underwater: room radii `16..40 vox = 4..10 m`, so diameters are 8-20 m; the
  origin radius `48 vox = 12 m` gives a 24 m, three-chunk hub. `RoomSpacing=128 vox = 32 m` leaves
  `64-40=24 vox = 6 m` radial margin around the largest room. `TunnelMin/MaxRadius=6..8` gives
  3-4 m bores. `RoomHeightRatio=0.85` makes the largest room's vertical diameter
  `2×40×0.85×0.25=17 m`; `H=8×32=256 vox=64 m` leaves enough vertical budget for seals,
  passages, and a high room. `MaxTunnelLength=360 vox=90 m` exceeds the worst jittered neighbour
  distance with margin.
* FlatPlain/CrystalChamber: floor `0.25×256=64 vox=16 m`, ceiling `0.60×256=153.6 vox=38.4 m`,
  leaving `22.4 m` before roughness. Columns are `8..16 vox` radii (4-8 m diameters) on 96-voxel
  (24 m) cells. The two names intentionally retain the same slab generator and parameter block.
* Maze: `CellSize=64 vox=16 m`; `CorridorRadius=12.5 vox=3.125 m`, hence a 6.25 m nominal bore.
  The 3.75-voxel proven roughness reach leaves an 8.75-voxel effective radius and
  `0.7×(2×8.75×0.25)=3.0625 m` of usable floor; the worst-case neighbouring-bore wall is
  `64−2×16.25=31.5 vox=7.875 m`.
* SurfaceWorld: `ElevationRange=80 vox=20 m`, base ground `0.25×256=16 m`, warp `48 vox=12 m`,
  sky cap `0.95×256=243.2 vox=60.8 m`; terrace/layer/beach/overhang values are separately
  2 m / 2 m / 3 m / 4 m reach + 6 m height optional features.
* VerticalShafts: spacing `80 vox=20 m`, shaft radii `8..14 vox` (4-7 m diameters), connector
  radius `6 vox` (3 m bore), ledge spacing `32 vox=8 m`, ledge depth `4 vox=1 m`.
* FloatingIslands: spacing `112 vox=28 m`, radii `24..48 vox` (12-24 m diameters); the largest
  leaves `56-48=8 vox=2 m` centre-to-edge radial margin. Thickness is independent:
  `48×0.60=28.8 vox=7.2 m` below the centre and `48×0.20=9.6 vox=2.4 m` above it.
* Shared passages and disturbances: mouth/mid radii `8/6 vox` (4/3 m bores), reach `32..96 vox`
  (8-24 m), spine distance `64..192 vox` (16-48 m), wander/spiral `24 vox=6 m`, cascade ledge
  `8 vox=2 m`; chasm spacing/radius `192/24 vox` (48 m / 6 m), bridge spacing/radius `128/8`
  (32 m / 2 m), and ridge spacing/height/thickness `128/32/8` (32 m / 8 m / 2 m). Optional
  transport fields are also audited: arch radius `6..12` (1.5-3 m), column `4..8` (1-2 m), pit
  `8..16` plus depth `32` (2-4 m plus 8 m), chimney `4..8` plus height `32` (1-2 m plus 8 m),
  dome `16..32` (4-8 m), pinch length `24` (6 m), overhang depth `8` (2 m), and the inactive
  layer/rib depths `0.3/0.4` (7.5/10 cm). The exact generated row-by-row audit includes nominal
  periods for every frequency field.

The 64 m height is deliberate rather than accidental: the GDD boss-space anchor is approximately
3-4 chunks (24-32 m), so the extra vertical envelope is needed to combine that room scale with
seals and vertical traversal. The NPS [Lehman Cave dimensions](https://www.nps.gov/grba/learn/nature/lehman-caves-dimensions.htm)
also support heterogeneous natural volumes rather than one fixed room ratio.

#### Equivalence, box safety, and cost after the default move

The full report `Saved/AutomationReports/WorldScaleFullFinal3/index.json` recorded 28 successes,
2 warning-only tests, 0 failures, and 0 not-run tests in 1,704.010 s. The warnings are known scope
findings: lateral regions remain gated at 9/16 cross-region law, and TunnelNetwork op-stack
sampling has only 240/6,000 samples outside its gate. Neither is a box-soundness violation.

All legacy/stack equivalence tests remained bit-identical to their shared reference paths, including
Maze, both slab archetypes, SurfaceWorld, TunnelNetwork stage A+B, VerticalShafts, and FloatingIslands.
`WorldRadiusVoxels` remained 0, lateral regions stayed gated, density sign remained correct, and every
box brute-force audit reported **0 violations**. Current representative box counts are: ParameterRoll
1,149 proved / 1,529,319 lattice voxels; StructureRoll 715 / 951,665; corpus-free arms 185 / 246,235,
260 / 346,060, and 299 / 397,969; TunnelNetwork production op-stack 7/40 proved (7 AllSolid),
Underwater 34/40, Maze 127/256 plus 250/256 novel lattice boxes, FlatPlain 27/60, CrystalChamber
39/60, tuned CrystalChamber 29/60, VerticalShafts 31/60, and FloatingIslands 15/60 AllAir.
Every listed count has zero brute-force violations.

The pristine pre-scale player-fit showcase did not execute the op-stack classification suite, so a
pure old-default tile-count delta does not exist. The nearest instrumented pre-final checkpoint had
the operator-stack 600-tile scan at **521 Mixed / 23 AllSolid / 56 AllAir**; the final world-scale
scan is **518 / 71 / 11**. Uniform proofs therefore did not collapse (79 → 82), although the
AllAir/AllSolid mix moved. The final non-stack scan is **522 / 22 / 56**. This is a real measured
classification result, but it must not be misrepresented as a pristine causal A/B run. The
production-height showcase itself cost about **1,709.7 s** and produced 7 fine renders, one refusal,
and one blank diagnostic card; its artifact is `Saved/VoxelForge/Showcase/index.html`.

The corrected owner-facing artifact is therefore at `Saved/VoxelForge/Showcase/index.html` (fresh
report: `Saved/AutomationReports/WorldScaleShowcaseProductionFinal/index.json`). It contains the new
metre audit and the rejected-but-useful diagnostic cards; the zero hard-gate survivor set is
intentionally visible rather than hidden by a fallback candidate.

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
candidates in circulation. The first five-season promotion measurement did **not** collapse: mean
normalized spread ended 0.047089 wider than it started, but survival was not monotonic. That is one
seed and one provisional policy, not a dispensation from the risk; the next policy should reserve a
reviewable outlier quota instead of assuming survivor promotion preserves surprise.

---

## 4. The world manifest

### ⭐ It is TWO things, not one

**Tier A — the SPINE. Finite, tiny, actually stored.** Strate slots and passages. ~30 strates plus a few
passages each is roughly 100 entries — kilobytes. The runtime manager already computes `StrateLayout`
and its passage list once in `UVoxelStrateManager::Initialize`; Tier 4c now adds the first reviewable
serialized Tier A artifact, `season_manifest.json`, containing the selected ordered strate spine and
the data required to reproduce each selected density field exactly.

The current Tier 4c manifest deliberately serializes the chosen strates, not the manager's generated
inter-strate passage geometry or the full material/creature/content placement payload. The season test
therefore proves each stored strate's density round trip; passage serialization and the wider content
manifest remain a follow-up before this file can be called the complete world manifest described here.

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
shaft sources search their placement grid. Sparse sources may move laterally, but only within a
bounded local source neighbourhood (one cell for Maze, two shaft spacings at the default
VerticalShafts density, one spacing for islands); if that search has no footing, falling back is the
honest answer.
Passage placement remains deterministic and cheap, and the slanted control-point chain is bounded too.

Then the measurement pass flood fill is the net: does the upper mouth's air component reach the lower
mouth's? If not, re-roll with the attempt folded into the hash.

### ✅ 6.5 The passage mouth becomes a LANDING; the landmark remains DATA — BUILT

`ELandmarkAnchor::PassageMouth` already exists — landmarks can anchor at passage mouths — and
`FStrateLandmark` already carries a Light-Orb block. **A glowing landmark at every passage mouth is
authorable today with no code at all.** The geometry half is now built: every end of every inter-strate
passage is a deterministic `FVoxelPassageLanding` — a rounded room, a hard flat support floor, a
doorway tangent to the tube, and a floor-backed connector to the guaranteed `(0,0)` root network.
The content half is deliberately not built here; landmark meshes, lights, sound, and placement
policy belong to the content system. The landing supplies the stable, walkable, deterministic
`PassageMouth` anchor that content placement needs, without inventing landmark geometry here.

The dimensions derive from the capsule and the existing turning-floor rule: player diameter
`2×0.34/0.25 = 2.72` voxels; floor width `3/0.25 = 12` voxels; capsule height
`2×0.88/0.25 = 7.04` voxels; and 1 m headroom adds 4 voxels. The room uses
`HalfWidth=max(7, MouthRadius+2)`, so its one-voxel-inset support floor is at least 12 voxels /
3 m wide while the authored room is at least 14 voxels / 3.5 m wide. Its height is
`max(12, 7.04+4, 2×MouthRadius+2)`; with the stock 6-voxel mouth, that is 14 voxels / 3.5 m
high and 8 voxels / 2 m half-width (4 m full width).
The floor is at `StandingPoint.Z−0.5`, three voxels / 0.75 m thick, and the tube centreline meets
the room at `FloorZ+MouthRadius`, so the lower tube tangent is the floor rather than a hole with a
step. The connector is 5 voxels / 1.25 m radius (2.5 m clear width), 12 voxels / 3 m clear height,
with a 4.5-voxel support inset; the room, not the transit leg, owns the turn floor. At the root it
joins the finite origin landing room, not a vertical hub. The default inter-strate tunnel is a
two-leg switchback with a named 15° floor-gradient limit; the connector uses the same limit. The
final-density audit checks the room floor and the capsule at every tunnel step. This is deliberately
well below the movement CDO's roughly 44.8° scramble ceiling.

The join is guaranteed rather than likely because a source query is only a local capsule proof — it
does not flood-fill the live graph. `GeneratePassages` therefore adds the same deterministic
connector for answered and refused source queries, ending at the finite landing room around `(0,0)`.
That room is the explicit walkable root network; it has a floor and a common interior level, and no
continuous vertical spine is carved between strates. It is generated from the passage seed and is
included in the conservative passage bounds. Its structural
post order is `origin spine → vertical seal → passage tube + landing/floor → XY edge seal`; the
landing is clamped inside the vertical seal, the floor backstop runs before the final XY edge seal,
and the edge seal wins at the world rim. `ClassifyTile` and the op-stack both treat a landing floor
as bidirectional geometry, so a proved `AllAir` box cannot erase the support.

The stable room/floor/door/connector anchor is exactly the hook the landmark content system needs:
it is walkable, known, deterministic, and available through the passage-mouth anchor without
inventing a second placement search. Beyond that, cheapest first: light (a glow down a tunnel is
the strongest pull in a cave) and sound (works around corners, which light does not — you hear the
draft before you see the hole). Rooms are what make the mouth findable on the *second* visit, which
quests need.

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

`VF_ApplyOriginSpine` no longer carves a full-height cylinder. It builds one finite, rounded landing
room at actor-local XY (0,0) in each strate, with a flat support floor. The old cylinder guaranteed
open air but supplied no surface to stand on except at its bottom; that was both an unusable hub and
an accidental fall shaft. The room is archetype-independent, appended automatically, and sized by
the §6.5 body arithmetic. It is clamped inside the strate's vertical seal interval and never crosses
the XY edge seal.

The seals between strates remain solid. A lower room is therefore not an open shaft destination: it
becomes reachable when progression opens the inter-strate passage, and the player arrives on that
strate's room floor. `bOpenSurfaceEntry` is the one explicit exception: when enabled, strate 0 may
open its top seal down to the top room ceiling as the city's above-ground continuation. It does not
open any lower seal or bore the rooms together.

The base inter-strate connection is a small walkable tunnel. Its default is a deterministic
two-leg switchback, with a named 15° maximum floor gradient, level landing aprons, and a floor-backed
capsule-clear tube. A 32 m vertical drop needs `32/tan(15°) = 119.4 m` of horizontal run before
aprons and turn transitions, so a single straight ramp would be implausibly long; the switchback is
an explicit design choice. `VerticalShaft`, `SpiralDescent`, `CascadingDrops`, and `CrackCrevice`
remain available as exotic descriptors, but they do not receive the default walkability guarantee.

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
- ✅ **The passage mouth becomes a room plus a distinctive landmark hook** (§6.5) — BUILT for the
  geometry: both ends get deterministic walkable landings, floors, and root-network connectors.
  Landmark content itself remains intentionally with the content system.
- ⬜ **`SurfaceWorld` still refuses the query** — it needs manager-resolved biome context, which
  cannot be answered from a pure free function. Open.

*Standalone value:* fixes a **live hole in the shipping game** — a player could descend into a
sealed pocket. Worth doing even if the composer never happens. **It was real**: see §14.

### Tier 2 — the measurement pass (§3.4) — ✅ BUILT (2026-09-03), see §13
`VoxelStrateMeasure.h/.cpp` + `VoxelForge.Generation.StrateConnectivity` / `...Refinement`.
*Standalone value:* it **is** F1, `fable-idea.md`'s top-pick world-preview tool — the measurement
half plus the deterministic editor/automation visualisation described in §3.4.

**It paid for itself immediately**: it found the `VerticalShafts` primordial-law violation (§14),
which no amount of playing was likely to surface, and it caught three of its own artifacts before
they became "findings" (§13).

⬜ Remaining: the danger/rarity metric families from §3.4 (fall exposure, openness, traversal mix,
tortuosity, corpus-centroid distance).

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
  and the 64-candidate measurement pass are built; the bootstrap follow-up is 38/64 survival with
  0 box-verdict violations (the first-run corpus/vacuity findings remain recorded above).
- ✅ **Structure roll (§3.2, Tier 4b)** — serialisable recipes, root polarity, declaration-derived
  resource-safe modifier legality, mandatory posts, 64-candidate measurement, and direct box-verdict
  brute force are built; the bootstrap follow-up is 42/64 survival with 0 invalid recipes and 0
  violations, plus 64 rendered preview pairs and an index.html contact sheet. The fine survivor ROI
  is centred on `LargestComponentPoint` and labelled largest open space; the final before/after
  blank-plan count is 6/42 → 3/42.
- ✅ **Season pipeline (§3.1, §3.5, §4, Tier 4c)** — `VF_ComposeSeason` generates and measures a
  bounded candidate batch, rejects vacuous/fragmented/primordial-law failures, and writes a Tier A
  JSON spine plus a selected-only descent review page using `VoxelStratePreview`. The explicit policy
  is provisional: default 75% grounded / 25% outlier, adjacent-similarity penalty, archetype/recipe
  variety bonuses, and fixed absolute slots with boss markers every five non-terminal slots. The
  focused artifact run was **24 generated / 19 hard-gate survivors / 6 selected / 18 rejected or
  unselected**: 4 vacuous, 1 primordial-law budget failure, and 13 policy-not-selected. The loader
  rebuild test compared **393,216 density samples bit-identically**, and the same seed emitted
  byte-identical JSON. This closes the offline artifact and veto seam; full passage/content manifest
  serialization is still open as noted in §4.

### Tier 5 — the long game
- ✅ **Promotion** (good strates rejoin the corpus) — JSON promotable records and season manifests,
  project/default/promoted provenance, fresh-load re-verification of the three gates, measured
  near-duplicate rejection, per-season cap, and a deterministic five-season simulation. The run ended
  at 21 corpus members (4 project, 8 default, 9 promoted); spread widened overall but survival was
  non-monotonic. The owner still needs to define “good” from the preview; promotion does not invent
  that judgment.
- ⬜ Theme and tag draws for materials, creatures, and audio · eventually the model. All optional,
  all compounding.

---

## 8. Corrections that must not be re-derived

Kept because each cost real time or would otherwise be repeated.

- ⛔ **"Do not start Phase 3" is SUPERSEDED.** `Docs/archive/OPSTACK-HANDOFF.md` still says it; believe this file.
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
4. ✅ **Where the corpus lives** and how promotion is recorded between seasons: the cumulative store is
   `Saved/ComposerPromotion/seed_0_step_8_radius_256/promoted_strates.json` in the simulation, with
   `season_01_manifest.json` … `season_05_manifest.json` alongside it; the default runtime/editor
   location is `Saved/VoxelForge/StrateCorpus/promoted_strates.json`.
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

**⭐ This closes the pending-verification thread opened by `Docs/archive/OPSTACK-HANDOFF.md`.** That file flagged
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

**2026-09-05 — Tier 5 + fine-ROI validation.** The specified UE 5.7 `VoxelMEditor Win64
Development` build succeeded. The full headless `VoxelForge` namespace reported **27 succeeded,
0 failed, 0 not run, 0 succeeded-with-warnings** in **613.277 s**. The focused promotion test was
green in **63.556 s** and the focused StructureRoll test was green in **243.649 s**. Promotion ended
with 9 cumulative records, fresh-load stale-metric replacement, `WorldRadiusVoxels=0`, and the table
in §3.3. The StructureRoll preview measured fine blank plan/card counts **6/42 → 3/42** and rendered
42 fine filled pairs plus 42 contour pairs with 0 refusals. No existing generation/equivalence test
failed.

**2026-09-05 — Tier 4c season validation.** The specified UE 5.7 `VoxelMEditor Win64 Development`
build succeeded after adding the season manifest and review seam. The focused
`VoxelForge.Composer.Season` test passed, then the full headless `VoxelForge` namespace reported
**28 succeeded, 0 failed, 0 not run, 0 succeeded-with-warnings**. The season artifact used 24
generated candidates, 19 hard-gate survivors, and selected 6; it recorded 4 vacuous rejections, 1
primordial-law budget rejection, and 13 provisional-policy non-selections. Reloading the JSON and
rebuilding every selected stack compared **393,216 density samples bit-identically**; composing the
same seed twice emitted byte-identical JSON. The selected-only descent review was written beside the
manifest. The run also kept all existing equivalence, determinism, box-verdict, and connectivity
tests green.

**2026-09-05 — Tier 4d lateral-region audit.** The specified build succeeded after adding the
per-chunk partition cache, density combiner, conservative box fold, season round-trip fields, and
the editor preview hand-off. The dedicated test intentionally remains red on the primordial-law
gate: it collected **16 opposite-region mouth pairs** and connected **9/16 (56.25%)**; the same
region-zero controls connected **11/16**. This is evidence, not a tuned threshold. The box audit
covered **40** boxes (**20 cross-boundary**), brute-forced **199,800** integer samples, and found
**0** false-uniform violations; all cross-boundary verdicts were `Mixed`. At blend width **24**,
**10,517/65,536 = 16.0477%** of sampled XY voxels were in a band; cached interior timing was
**338.11 ns/voxel**, band timing **663.12 ns/voxel**, or **1.961×**. The multi-region preview was
written to `Saved/ComposerPreview/LateralRegions_seed_12001/index.html`. On those same 16 seam
cases, survival before the law gate was **valid 16/16, non-vacuous 16/16, largest-component share
≥ 0.50 16/16**, and full hard-gate survival was **9/16**. No corridor, threshold change, or
successful-seed filter was introduced; lateral regions are not ready for promotion until the seam
law has a design-level fix.

---

## 11. Tier 1 status — passages aim at player-fit source space (pure local stencil; foundation for §6.5)

**Updated 2026-09-06.** `VF_SuggestLandingPoint` (in `VoxelCaveMorphology`) is a **pure free function**
of (archetype, params, seed, desired XY, lateral budget) — it touches no operator stack, no
`UVoxelStrateManager`, no cache. That matters: `GeneratePassages` runs inside `Initialize`, when the
layout is half-built and `LayoutVersion` is mid-flight, so constructing anything holding a manager
pointer there is a re-entrancy trap.

`GeneratePassages` now asks **each mouth's own strate**, independently, for a player-fit pose near
the chosen XY. The pure query reconstructs the exact default capsule stencil used by
`VoxelStrateMeasure`: support coverage within max step height, a walkable support patch, and a clear
capsule volume, with positive density meaning air and non-positive density meaning solid. It samples
only a stateless source evaluator — no manager pointer, live layout, operator stack, cache, or
measurement flood fill enters the query.

The candidate is deliberately a feature core rather than arbitrary air: a room floor core,
requested XY in a slab, a horizontal Maze edge, a VerticalShafts shaft core with a deterministic
ledge-side offset above its lower seal, or a FloatingIslands top surface. The local stencil proves a legal pose, not that the pose belongs to a
large connected component; that question remains in the measurement pass. When no legal local pose
exists inside the bounded source neighbourhood, **the random reach and original XY are preserved
unchanged**.

This section is the source-selection contract, not the finished mouth geometry. §6.5 now wraps every
accepted or refused result in a structural landing: the source result chooses a deterministic anchor
when available, while the manager supplies the room, flat floor, doorway, and guaranteed root-network
connector that make the anchor a place to stand. The source query remains pure and manager-free.

### ⚠️ Coverage is source-dependent and the number is the point
The failing pre-fix measurement was:

> *7 inter-strate passages, 3 checked, 0 footing checks, 38/48 room/slab ring samples air, 4
> query-false (4 unique archetypes; 1 unsupported, 3 supported-but-no-point).*

The Tier 1 query now searches laterally for `Maze`, `VerticalShafts`, and `FloatingIslands` feature
cores within one configured placement period, and for cave rooms within one `RoomSpacing`.
`SurfaceWorld` remains intentionally refused.
The extended test uses vertical source footing brackets for those three instead of applying the
room/slab circumference ring to thin or void-dominated geometry. The post-fix measurement is not
claimed as a connectivity proof: the focused fixture now reports 7 passages, 3 upper and 3 lower
player-fit queries answered, 8/14 mouths falling back, 0/0 room/slab ring samples air, and all 3
answered thin-source footing checks valid. The eight-card step-1 measurement is recorded in §12.1;
it is the source of truth for the remaining component gaps.

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
⚠️ **It is a proxy for connectivity, not a proof.** The pure query itself is even narrower: it can
prove a local capsule pose but cannot prove a large component without reading generated layout
state. The proof is a flood fill — Tier 2.

### ⚠️ A forked placement envelope, guarded by that test
`VF_FindNearestHashRoomLandingPoint` re-derives the room vertical placement envelope that `BuildChunkCache` also
computes. Calling the per-chunk cache builder from `Initialize` is impractical, so this is a deliberate
second copy. **Both sites now carry a PLACEMENT CONTRACT comment naming the other**, and the ring test
is the only thing standing between a placement change and silent bedrock passages.

---

## 12. Tier 1 checkpoint — and the 92.8% defect a green suite hid

**Historical checkpoint: built clean, suite green: 17 succeeded, 0 warnings, 0 failed
(2026-08-30).** The room-centre correction below remains useful history, but it did not prove a
player-sized landing or a large connected component.

### 12.1 2026-09-06 player-fit source-query pass (historical, before landing geometry)

The next correction keeps the pure/re-entrant boundary intact and replaces the query's open-air
claim with a local player-pose claim. Both the upper and lower mouths query their own source
independently. The query mirrors the measurement stencil (capsule volume, support patch, step
height, and walkable normal) against a stateless archetype source evaluator. It does not run a
flood fill, so it cannot honestly promise that the two mouths share a large fit component.

The final eight-archetype before/after table below records the measured arrival/departure component
sizes and voxel gaps. A remaining gap is a generation-connectivity finding, not a reason to widen
the landing budget.

| Archetype | Before: fit / components / largest / arrival / departure / gap / law | After: fit / components / largest / arrival / departure / gap / law |
|---|---|---|
| CrystalChamber | 655 / 197 / 24 / 1 / 1 / 47.529 / NotConnectedAtThisResolution | 1,360 / 62 / 717 / 270 / 717 / 19.131 / NotConnectedAtThisResolution |
| FlatPlain | 1,116 / 45 / 259 / 1 / 1 / 48.104 / NotConnectedAtThisResolution | 2,136 / 2 / 2,135 / 2,135 / 2,135 / 0.000 / Connected |
| FloatingIslands | 4 / 2 / 3 / 1 / 1 / 0.000 / Connected | 0 / 0 / 0 / 0 / 0 / -1.000 / StartCellNotPlayerFit |
| Maze | 2,801 / 309 / 176 / 43 / 81 / 206.630 / NotConnectedAtThisResolution | 5,759 / 82 / 1,217 / 30 / 1,217 / 121.598 / NotConnectedAtThisResolution |
| SurfaceWorld | 713 / 520 / 6 / 1 / 1 / 110.648 / NotConnectedAtThisResolution | 355 / 223 / 9 / 3 / 1 / 96.073 / NotConnectedAtThisResolution |
| TunnelNetwork | 2,748 / 467 / 1,292 / 2 / 1 / 164.405 / NotConnectedAtThisResolution | 100 / 1 / 100 / 100 / 100 / 0.000 / CoarseLiedBudgetExhausted |
| Underwater | 337 / 86 / 35 / 3 / 11 / 142.499 / NotConnectedAtThisResolution | 342 / 13 / 194 / 95 / 194 / 115.265 / NotConnectedAtThisResolution |
| VerticalShafts | 1,156 / 112 / 96 / 3 / 36 / 94.000 / NotConnectedAtThisResolution | 4,669 / 36 / 619 / 14 / 81 / 104.890 / NotConnectedAtThisResolution |

The restricted law tally is **1/8 connected before and 1/8 after**; no after result is a connected
component of four cells or fewer. The FloatingIslands `StartCellNotPlayerFit` result is the live
world after the mouth carve: the pure source query found a top pose, but the structural passage
carve removes the support at that exact endpoint. That is a generation/carve integration issue,
not evidence that the query should accept open air. TunnelNetwork's single 100-cell component is
the coarse retry budget being exhausted, not a connected-law pass.

The query-answer fixture still intentionally refuses `SurfaceWorld`. In this seed, the sparse room
and slab sources can also decline when no room/core passes the local stencil inside their lateral
budget; that preserves the original placement rather than inventing a point. The pure query has
no component-size oracle, so its honest large-space substitute is a guaranteed feature core:
room centre, slab floor at requested XY, horizontal Maze edge, shaft ledge-side pose, or island
top interior. A generation-side connection/room-mouth guarantee is the smallest alternative if
the measured law must become connected.

The old VerticalShafts refinement assertion that every mouth sit on the exact shaft axis was too
strong for this task: an infinite open cylinder has no support surface at its axis. It is now an
executable **feature-core** invariant (with the drainage-tree path still audited from the resolved
axis), while the landing query proves the actual ledge-side capsule pose. The required UE 5.7 build
passed; the full `VoxelForge` namespace finished with **29 successes, 2 known warnings, 0 failures,
0 not-run** in 2,388.35 s. The warnings remain the gated lateral-region 9/16 law and the known
TunnelNetwork op-stack outside-gate sample count; box violations stayed zero.

### 12.2 2026-09-06 Maze spanning-tree correction (historical, before landing geometry)

The Maze lattice no longer uses independent per-edge hash gating. Each non-origin cell chooses one
parent from the non-zero coordinate axes and steps one unit toward the origin. The parent reduces
`|x|+|y|+|z|` by exactly one, so the undirected parent edges form one infinite rooted tree: no cycle
can exist, and following parents reaches `(0,0,0)` from every cell. A canonical lower node plus
axis edge is emitted when either endpoint selects the other. Optional loop edges use capped hash
rolls (`0.18 × BranchProbability` horizontally and `0.10 × Verticality` vertically); they add
visual choice without carrying the connectivity guarantee.

This is locally computable. The evaluator's exact window is the eight lower nodes in
`{-1,0}³` (a 2×2×2 node window); the adjacent `+1` endpoint is one of the two endpoints of each
canonical edge, so its parent decision is still inside that same bounded window. The source and
op-stack cache use the identical predicate. No wide collect was needed, and no parent decision
depends on the stored window or chunk origin. `WorldRadiusVoxels` remains `0`; lateral regions
remain gated off.

The focused topology audit sampled **64 seeds** over a centred radius-10 graph (**9,261 cells per
seed**). With loops disabled, every graph had exactly `N−1` parent edges and **0 edge-count or
disconnected-cell violations**. With default loop settings, disconnected cells were also **0**.
The default sample added **103,297** optional loop edges in total. Repeating the same seed produced
bit-identical edge keys across **20,000** checks.

The output is measurably unlike the old cubic grid. Across **216,000 interior cells** and all
64 seeds, the degree distribution was `d0=0, d1=34,371 (15.9%), d2=79,046, d3=68,545,
d4=27,965, d5=5,531, d6=542`, with mean degree **2.504**. Maximal degree-2 corridor runs were
`n=464,360`, minimum **1**, median **1**, p90 **3**, maximum **18**. The cubic-grid control was
`d6=100%`, `d1=0%`, mean **6.000**. The tree therefore supplies real dead ends, varied runs,
and non-uniform junctions even before the optional loops are considered.

The seam test `VoxelForge.Generation.MazeSeamFreedom` passed **42/42 boundary probes** with
**0 legacy mismatches and 0 operator mismatches**. It explicitly records the 2×2×2 local window
and “no wide collect” contract.

The player-fit law is still not connected, and was not tuned toward. The pre-tree baseline at the
same style of fine measurement was **5,759 fit cells / 82 components / largest 1,217 / arrival 30 /
departure 1,217 / gap 121.598 voxels**, `NotConnectedAtThisResolution`. The new spanning-tree
showcase candidate (seed `0`, candidate `57`) measured **2,453 / 72 / 473 / arrival 100 /
departure 415 / gap 149.730 voxels**, also `NotConnectedAtThisResolution`; its coarse lattice
verdict was `Connected`. This separates the proven corridor-graph law from the remaining physical
player-fit problem: the gap is at the passage mouths/fine floor-resolution interaction (and must
not be “fixed” by raising edge probability), not evidence that the lattice graph is probabilistically
fragmented. The other archetypes retain their own independent fit findings.

The 3 m two-abreast radius derivation is explicit: `3/0.7 = 4.286 m` clear diameter, or an
8.571-voxel clear radius at 0.25 m/voxel. Add the proven roughness reach
`2×1.25×1.5 = 3.75 voxels`, giving 12.321 voxels; round up to `CorridorRadius=12.5`.
The effective floor diameter is `2×(12.5−3.75)×0.25 = 4.375 m`, so usable floor is
`0.7×4.375 = 3.0625 m`. With `CellSize=64`, worst-case neighbouring-bore wall is
`64−2×(12.5+3.75)=31.5 voxels = 7.875 m`.

Box audits stayed sound after adding guaranteed air: the existing Maze box audit proved **104
uniform boxes**, classified **152 Mixed**, sampled **138,424 voxels**, and found **0 violations**;
the novel fill audit covered **240 boxes** (`0 AllSolid`, `240 AllAir`) plus **16 Mixed** cases,
**319,440 voxels**, and **0 violations**. The focused audit measured **1.436 μs** per forced cache
rebuild and **0.150 μs/call** for 20,000 hot evaluations; the final namespace run measured
**1.130 μs** and **0.145 μs/call**, respectively, on the validation machine. Parent/loop work stays
in the thread-local rebuild.

The regenerated owner-facing artifact is
`E:/Projet Unreal/VoxelM/Saved/VoxelForge/Showcase/index.html`; the current Maze card is
`candidate_1003_plan.png` (with matching `candidate_1003_vertical.png` and fine ROI variants).
The landing pass rendered **8 fine views**, refused **0**, and produced **0 blank** cards. The
showcase's diagnostic player-fit gate still found **0/8 hard-gate survivors**; the cards are kept
as walkable geometry previews rather than pretending that unresolved inter-landing law is solved.

The final full namespace verification is `Saved/AutomationReports/VoxelForgeFullMazeTreeFinal3/index.json`:
**32 tests, 30 clean successes, 2 warning-bearing successes, 0 failures, 0 not-run** in **2,523.978 s**.
The two warnings are the pre-existing gated lateral-region result and the known TunnelNetwork
outside-gate sample count; neither is a Maze or box-soundness failure.

### 12.3 2026-09-06 §6.5 landing geometry pass

> Historical snapshot. This section describes the pre-§6.8 origin-spine correction (including the
> annular root hub and the 44° connector law). The current finite landing-room and walkable-tunnel
> measurements are recorded in §12.4 below.

The mouth is now a place rather than the end of a bore. `GeneratePassages` builds both ends of all
seven inter-strate passages as `FVoxelPassageLanding` descriptors. Each descriptor contains a
rounded room, a three-voxel support slab, a tube doorway at the floor tangent, and an explicit
flat-floor connector to the deterministic `(0,0)` origin-root network. The source query still does
the local archetype selection; the connector is present for both answered and refused queries
because a local capsule proof is not a component proof.

The body arithmetic is: diameter `2×0.34/0.25 = 2.72` voxels; minimum turn floor `3/0.25 = 12`
voxels; capsule height `2×0.88/0.25 = 7.04` voxels; and one metre of headroom `=4` voxels.
`HalfWidth=max(7,MouthRadius+2)` leaves a one-voxel-inset flat support floor at least 12 voxels /
3 m across and an authored room at least 14 voxels / 3.5 m across. Height is
`max(12,7.04+4,2×MouthRadius+2)`; stock mouth radius 8 therefore produces an 18-voxel / 4.5 m
room. The tube centreline enters at `FloorZ+MouthRadius`, so its lower tangent is the room floor.
The connector is 5 voxels / 1.25 m radius (2.5 m clear width), 12 voxels / 3 m clear height, and
a 4.5-voxel support inset. It joins the common per-strate annular hub outside the origin spine;
the direct ramp is used only when it stays ≤44°, otherwise a deterministic level dog-leg precedes
the final ramp. All 14 landing floors passed the final-density slope bracket; worst measured room
gradient was `0.058998`, below the explicit 44° limit `tan(44°)=0.965689`, and the worst
analytic connector gradient was `0.522406` (walkable limit `0.965689`). The per-end values were recorded by
`VoxelForge.Determinism.PassageLandsInOpenSpace` without per-sample logging.

The focused landing audit reported **14/14** floor/slope passes, **14/14** explicit root-network
connectors, **0** seal violations, and **0** geometry/floor failures. The targeted tile audit saw
**14** floor boxes, **0** uniform proofs (`AllSolid=0`, `AllAir=0`), and **0** brute-force verdict
violations. The connector itself is a guaranteed walkable branch to the structural root; the
separate player-fit law below correctly remains an empirical test of the native network between
the two landing branches, especially when their floor heights differ.
The same audit measured the passage shortlist at **0.665 μs** per forced rebuild and **0.646 μs**
per hot call on the validation machine; source queries and landing construction remain outside the
voxel loop.

The support strip stops at the configured origin-spine radius; this preserves the existing vertical
air column rather than capping it with a landing floor. The connector still reaches the spine
boundary, and the full-resolution direct-walk assertion remains enabled.

Final player-fit component sizes from the fitted-window audit were:

The table below is the fixed H3 fitted-window identity used by `ScaleDiagnosis`. Separately, the
specific 31.9M-cell whole-strate Maze comparison cited by §6.5 changed from the supplied
**172-cell arrival / 4-cell departure** pocket to **1,235 / 2,938** after the landing. That is a
substantial improvement at both mouths, but it still has a **71.896-voxel** gap and remains
`NotConnectedAtThisResolution`.

| Archetype | Before: fit / components / largest / arrival / departure / gap / law | After: fit / components / largest / arrival / departure / gap / law |
|---|---|---|
| CrystalChamber | 655 / 197 / 24 / 1 / 1 / 47.529 / NotConnectedAtThisResolution | 1,575 / 90 / 286 / 286 / 274 / 155.042 / NotConnectedAtThisResolution |
| FlatPlain | 1,116 / 45 / 259 / 1 / 1 / 48.104 / NotConnectedAtThisResolution | 2,817 / 11 / 2,130 / 268 / 273 / 155.042 / NotConnectedAtThisResolution |
| FloatingIslands | 4 / 2 / 3 / 1 / 1 / 0.000 / Connected | 72 / 1 / 72 / 72 / 72 / 0.000 / Connected |
| Maze | 2,801 / 309 / 176 / 43 / 81 / 206.630 / NotConnectedAtThisResolution | 12,384 / 340 / 3,210 / 3,210 / 3,210 / 0.000 / Connected |
| SurfaceWorld | 713 / 520 / 6 / 1 / 1 / 110.648 / NotConnectedAtThisResolution | 2,256 / 297 / 286 / 286 / 268 / 164.250 / NotConnectedAtThisResolution |
| TunnelNetwork | 2,748 / 467 / 1,292 / 2 / 1 / 164.405 / NotConnectedAtThisResolution | 6,683 / 227 / 3,841 / 3,841 / 269 / 155.042 / NotConnectedAtThisResolution |
| Underwater | 337 / 86 / 35 / 3 / 11 / 142.499 / NotConnectedAtThisResolution | 5,780 / 117 / 3,838 / 3,838 / 267 / 155.042 / NotConnectedAtThisResolution |
| VerticalShafts | 1,156 / 112 / 96 / 3 / 36 / 94.000 / NotConnectedAtThisResolution | refused: fitted origin-inclusive grid exceeded `MaxCells` |

Thus the restricted player-fit tally is **1/8 before** and **2/7 after** of the measured archetypes,
with **0** after connections through four cells and **1/8 refused** (VerticalShafts). Maze's
origin-inclusive fitted window is connected, but its independent whole-strate 31,881,528-cell
step-1 window is not: **119,479 fit cells / 2,948 components / largest 11,418 / arrival 1,235 /
departure 2,938 / gap 71.896**, so this is not a cropped-window victory. The hard independent
VerticalShafts air-network refinement is a different, larger check and passed **16/16** fitted
arrival→departure seeds. The remaining player-fit misses are therefore between substantial
landing components and are not being hidden by a relaxed assertion or tuned toward a component
count.

The final UE 5.7 validation was the requested editor build (4 actions, succeeded; 11.59 s), followed by
`VoxelForgeLandingFinal5`: **32 tests, 30 clean successes, 2 warning-bearing successes, 0 failures,
0 not-run** in **3,607.746 s**. `Composer.Promotion` passed with its strict existing gates; no
assertion was weakened. The two warning-bearing tests are the gated lateral-region audit (**4/16**)
and the known TunnelNetwork op-stack outside-gate sample count; landing, connectivity, seal,
The namespace's showcase child evaluated **256** rolls (**82** eligible single-region, **174**
multi-region skipped while the lateral gate is off) and regenerated
`E:/Projet Unreal/VoxelM/Saved/VoxelForge/Showcase/index.html` with **7 fine renders, 1 refusal,
0 blanks**.

The landing audit's per-end final-density floor gradients (lower / upper where present) were:
TunnelNetwork **0.044387**; FlatPlain **0.001678 / 0.058998**; CrystalChamber
**0.001434 / 0.006241**; Maze **0.004608 / 0.020844**; SurfaceWorld **0.012390 / 0.012970**;
VerticalShafts **0.000061 / 0.018677**; FloatingIslands **0.005249 / 0.028687**; Underwater
**0.000061 / 0.000061**. All 14 passed against the explicit 44° walkable gradient limit
**0.965689** (`tan(44°)`).
The corresponding analytic connector gradients were **0.183831, 0.149547, 0.522406, 0.285965,
0.037782, 0.113750, 0.069551, 0.215350, 0.189820, 0.201735, 0.132101, 0.157017, 0.011850,
0.028576** (14/14; worst **0.522406**, limit **0.965689**).

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

### 12.4 2026-09-08 §6.8 finite origin landings and walkable base descent

This is the current geometry and measurement record. `VF_ApplyOriginSpine` now creates one finite
rounded room at `(0,0)` in every strate. The room is top-anchored inside the safe interval
`[StrateBottomZ+Seal, StrateTopZ-Seal]`: `FloorZ = InnerTop - RoomHeight - 4`, with a 4-voxel
(`1 m`) seal-safety margin. The three-voxel (`0.75 m`) support slab and the final-density floor
backstop are inside the room; neither vertical seal is breached, and no lower room is connected by
a default vertical bore. Only `bOpenSurfaceEntry` may add the top-strate continuation from above.

The dimensions reuse the §6.5 body arithmetic at `0.25 m/voxel`: player radius `0.34 m = 1.36`
voxels, diameter `0.68 m = 2.72` voxels, capsule height `1.76 m = 7.04` voxels, and one metre
headroom `=4` voxels. The shared formulas are `HalfWidth=max(7,Radius+2)` and
`RoomHeight=max(12,7.04+4,2×Radius+2)`. Thus the authored default origin radius `14` gives a
16-voxel half-width (`4 m` half-width, `8 m` full width) and a 30-voxel (`7.5 m`) room height;
the stock passage mouth radius `6` gives an 8-voxel half-width and a 14-voxel (`3.5 m`) room
height. The one-voxel floor inset leaves at least the 3 m turning floor required by §6.5, with
space to stand, turn, and leave.

The default inter-strate connection is `EVoxelPassageType::SlopedTunnel`, authored as a deterministic
two-leg switchback. It is a single-file tube: 6-voxel mouth radius (`3 m` diameter), 5-voxel waist
radius (`2.5 m` diameter), and a nominal `2.5 m` clear height. Level aprons and turn transitions
are 17 and 6 voxels respectively in the stock configuration (`4.25 m` and `1.5 m`). The named
`WalkableTunnelMaxGradientDegrees=15°` contract (`tan(15°)=0.267949`) is deliberately far below the
movement system's roughly 44° scramble ceiling: 44° is not a sensible walking default for a
1.76 m-tall player in a small tunnel. For a 32 m drop, the required horizontal floor run is
`32/tan(15°)=119.43 m` (`477.70` voxels), or about `59.7 m` per switchback leg before adding the
level aprons and turn transitions. A straight gentle ramp would therefore be implausibly long;
the switchback is an explicit design decision. `VerticalShaft`, `SpiralDescent`, `CascadingDrops`,
and `CrackCrevice` remain in the enum for future exotic rolls, but the first three cannot be
continuously walkable as currently authored and the crack has no player-fit guarantee.

The focused final-density audit passed **7 passages / 42 segments**, with **5,916/5,916** floor
samples and **5,916/5,916** player-capsule samples. Gradient, floor, and capsule failures were all
zero; worst geometric gradient was `0.267758`, worst authored-route gradient `0.267793`, both below
`0.267949`. All **14/14** passage ends passed their floor test, all **14/14** explicit root joins
were present, seal violations were **0**, and geometry/floor failures were **0**. The cross-section
gradient diagnostic (`13.966`) is not the route contract; it reflects overlapping tapered SDF
cross-sections and is not counted as a walkability failure. Landing box checks saw **14** boxes,
zero uniform proofs (`AllSolid=0`, `AllAir=0`), and **0** violations. The per-chunk shortlist cache
timed at **1.259 μs** per forced rebuild and **1.243 μs** per hot same-chunk call on the validation
machine; there is no per-voxel cache rebuild and no per-sample logging.

The commandlet sweep used `-run=VoxelForgeExplore -modes=walk`, 8 archetypes × 8 seeds, target slot
4, six 4-chunk strates (`4×32×0.25=32 m` each), `WorldRadiusVoxels=0`, and schema-2 JSON. All
64 commandlets exited 0 and wrote valid JSON. `Connected` is the exact player-fit arrival→departure
law; `Exact` additionally excludes start/goal snapping.

| Archetype | Connected / exact | Origin fit / reachable | Arrival probes fit / in arrival component | Departure probes fit / in arrival component |
|---|---:|---:|---:|---:|
| CrystalChamber | 0 / 0 | 8 / 2 | 40/40 / 40/40 | 40/40 / 0/40 |
| FlatPlain | 0 / 0 | 8 / 2 | 40/40 / 40/40 | 40/40 / 0/40 |
| FloatingIslands | 3 / 2 | 8 / 1 | 35/40 / 35/40 | 35/40 / 10/40 |
| Maze | 0 / 0 | 7 / 0 | 40/40 / 40/40 | 40/40 / 0/40 |
| SurfaceWorld | 0 / 0 | 8 / 0 | 40/40 / 40/40 | 40/40 / 0/40 |
| TunnelNetwork | 0 / 0 | 8 / 1 | 40/40 / 40/40 | 40/40 / 0/40 |
| Underwater | 0 / 0 | 8 / 1 | 40/40 / 40/40 | 40/40 / 0/40 |
| VerticalShafts | 3 / 3 | 8 / 0 | 40/40 / 40/40 | 40/40 / 15/40 |
| **Total** | **6 / 5** | **63 / 7** | **315/320 / 315/320** | **315/320 / 25/320** |

The five-point room probe is the centre plus ±2 voxels in X/Y. Therefore 63/64 runs had every
arrival-room probe both player-fit and in the arrival component; all 315 arrival probes that fit
were in that component. The departure room had player-fit probes in 63/64 runs, but only 25/320
departure probes were in the arrival component (5/64 runs had all five there). The player-fit law
is **6/64 connected (5/64 exact; 58/64 not connected at this resolution)**. This is not tuned away:
the remaining failures are the separate intra-strate connectivity problem, not a tunnel gradient
or floor failure.

The origin-inclusive commandlet window changed the supplied baseline from **0/64 origin-reachable**
and **17/64 with any origin player-fit cell** to **7/64 reachable** and **63/64 with any fit cell**.
The no-fit case is Maze seed 7. The origin measurement is intentionally reported separately from
the mouth-sized route window; `WorldRadiusVoxels=0` means the report is a bounded fitted window, not
a claim about a finite whole-world volume.

Fresh commandlet walk timings were **0.653 s minimum, 13.411 s mean, 34.265 s maximum** across the
64 cases. The full Editor automation gate after the change was **32/32 successful, 0 failed**;
`LayoutOrderIndependence`, the box verdict tests, and all op-stack equivalence tests were green.

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

---

## 16. Player-fit route-window audit — the origin is not the missing flip (2026-09-06)

This pass tested the proposed sixth measurement artifact directly on the fixed Maze identity:
composer seed `0`, candidate index `50`, fourth one-based fixture slot (`SlotMaze == 3`). The
player-fit stencil remained `SampleStep=1`; every row below names its exact XY/Z window and cell
budget. `NotConnectedAtThisResolution` is a result of that stated grid, not a claim that the
infinite Maze graph is disconnected.

### 16.1 Part A — three Maze windows

| window | exact window / grid | MaxCells | fit cells | components | largest | arrival / departure | gap (voxels) | law |
|---|---|---:|---:|---:|---:|---:|---:|---|
| current mouth-AABB + margin | `X[-84.1,168.7) Y[39.5,53.3) Z[-1175,-937)`, `253×14×238`, `842,996` cells, step 1 | 4,000,000 | 2,041 | 58 | 510 | 510 / 294 | 254.370 | `NotConnectedAtThisResolution` |
| origin-inclusive mouth-AABB + margin | `X[-84.1,168.7) Y[0.0,53.3) Z[-1175,-937)`, `253×54×238`, `3,251,556` cells, step 1 | 4,000,000 | 12,384 | 340 | 3,210 | 3,210 / 3,210 | 0.000 | `Connected` |
| whole-strate centered origin cap window | `X[-183,183) Y[-183,183) Z[-1175,-937)`, `366×366×238`, `31,881,528` cells, step 1 | 32,000,000 | 119,479 | 2,948 | 11,418 | 1,235 / 2,938 | 71.896 | `NotConnectedAtThisResolution` |

The origin-inclusive box **did** flip this candidate to `Connected`, but the larger centered
whole-strate window did not. The result is therefore a useful local join diagnosis, not a claim that
the native Maze strate is connected end-to-end: the 31.9M-cell measurement is the controlling
whole-strate result. The whole-strate probe used an explicit 32M cap so it could remain a real
step-1 player-fit measurement; the production fine-law cap remains 4M. A step-2 player-fit probe
is not equivalent here: the player-fit API intentionally refuses every `SampleStep != 1` because
the capsule stencil cannot be resolved at that resolution.

### 16.2 Part B — route-aware window policy

The measurement now has an explicit `bIncludeOriginInCoverWindow` flag. It expands the fitted mouth
AABB to include `(0,0)` **before** the `MaxCells` check. If that route window is over the cap, the
measurement refuses with `The requested sample grid exceeds MaxCells; measurement refused.` It does
not shrink back to a mouth-only box.

The policy used by the eight-row law is:

- origin-inclusive mouth-AABB: `Maze`, `VerticalShafts`, `TunnelNetwork`, and `Underwater`;
- mouth-AABB plus capsule margin: `CrystalChamber`, `FlatPlain`, `SurfaceWorld`, and
  `FloatingIslands`.

The first group is origin-rooted by its actual native topology: Maze parent chains, VerticalShafts
drainage-tree chains, and the origin-flowing room graph shared by TunnelNetwork/Underwater. The
landing pass also supplies an explicit structural root hub for every archetype, but this diagnostic
keeps that hub separate from the native player-fit law so it cannot hide a broken archetype network.

### 16.3 Part C — corrected eight-row law

These are the fixed seed-0 identities in the scale diagnosis. All valid rows are step 1. The
window column is part of the result, not an omitted implementation detail.

| archetype | player-fit window (`X/Y/Z`, grid, cells) | fit cells | components | largest | arrival / departure | gap (voxels) | law |
|---|---|---:|---:|---:|---:|---:|---|
| CrystalChamber | mouth-AABB + margin; `X[-178.9,-152.8) Y[-38.2,29.1) Z[-537,-295)`, `27×68×242`, `444,312` | 1,575 | 90 | 286 | 286 / 274 | 155.042 | `NotConnectedAtThisResolution` |
| FlatPlain | mouth-AABB + margin; `X[-178.9,-152.8) Y[-38.2,29.1) Z[-534,-298)`, `27×68×236`, `433,296` | 2,817 | 11 | 2,130 | 268 / 273 | 155.042 | `NotConnectedAtThisResolution` |
| FloatingIslands | mouth-AABB + margin; `X[-143.5,-135.5) Y[121.8,129.8) Z[-537,-295)`, `8×8×242`, `15,488` | 72 | 1 | 72 | 72 / 72 | 0.000 | `Connected` |
| Maze | origin-inclusive mouth-AABB + margin; `X[-84.1,168.7) Y[0.0,53.3) Z[-1175,-937)`, `253×54×238`, `3,251,556` | 12,384 | 340 | 3,210 | 3,210 / 3,210 | 0.000 | `Connected` |
| SurfaceWorld | mouth-AABB + margin; `X[-133.7,-76.1) Y[-68.8,47.5) Z[-1495,-1257)`, `58×117×238`, `1,615,068` | 2,256 | 297 | 286 | 286 / 268 | 164.250 | `NotConnectedAtThisResolution` |
| TunnelNetwork | origin-inclusive mouth-AABB + margin; `X[-178.9,0.0) Y[-38.2,29.1) Z[-536,-296)`, `179×68×240`, `2,921,280` | 6,683 | 227 | 3,841 | 3,841 / 269 | 155.042 | `NotConnectedAtThisResolution` |
| Underwater | origin-inclusive mouth-AABB + margin; `X[-178.9,0.0) Y[-38.2,29.1) Z[-535,-297)`, `179×68×238`, `2,896,936` | 5,780 | 117 | 3,838 | 3,838 / 267 | 155.042 | `NotConnectedAtThisResolution` |
| VerticalShafts | origin-inclusive mouth-AABB + margin; **refused before allocation** because the required grid exceeds `MaxCells=4,000,000` | — | — | — | — | — | `REFUSED(MaxCells)` |

The valid-row tally is **2/7 Connected**, with **1/8 refused** and **0** unknown results. Neither
connected row is a ≤4-cell pocket: `after_connected_through_4_cells=0`.

### 16.4 Plain verdict

The remaining failure is not solved by treating a local origin-inclusive window as the whole strate:
that Maze window is connected, while the full 32M-cell step-1 centered window remains disconnected.
The proven spanning-tree graph and the player-fit volume are different claims. The landing branches
are substantial and explicit-root-connected, but the native player-fitting route between them still
fails at whole-strate scale. That is a genuine between-landings/archetype-network finding, not a
measurement crop, a tiny landing pocket, or a weakened assertion.
