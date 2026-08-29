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

The three real builders, for reference:

```
TunnelNetwork (19)   rock source -> room graph (SDF) -> SdfCarve -> 12 density modifiers -> worm source -> structural
Maze (7)             rock source -> lattice corridors (SDF) -> SdfRoughness (SDF) -> SdfCarve -> structural
FloatingIslands (7)  VOID source -> island blobs (SDF) -> SdfRoughness (SDF) -> SdfFill -> structural
```

**⭐ Most ordering is MECHANICAL — derive it, do not author it.** Roughness sits on the *SDF* before
conversion in Maze and on *density* after conversion in TunnelNetwork; CODEMAP confirms they are two
different ops for exactly that reason. An op's legal position is fixed by **which channel it reads and
which it writes**.
⇒ **Every operator declares its channel reads/writes.** Dependencies form a DAG and **any topological
sort of that DAG is a legal stack** — no authored order, and it produces orderings nobody wrote down.

**⭐ Root polarity is a one-bit identity lever.** FloatingIslands is Maze's op classes rooted in a
**void** source with a **fill** instead of a rock source with a carve — and you get islands instead of
tunnels.

**Corrective ops must be FUSED, not ordered.** `FFloorBiasMod` exists only to undo what
`FCaveRoughnessMod` did to floors. Both are density-space, so channel rules permit either order.
**An operator whose only purpose is to correct another operator is not a separate operator** — fuse them
into one op with two internal phases. That removes the invalid state instead of annotating it. Fifteen
ops that compose in any legal order beat eighteen with a footnote. (Fusing with identical order and
math leaves the field bit-identical.)

**One declaration covers the rest:** an op is **additive** (small displacement, commutes freely — shuffle
at will) or **transformative** (clamps, multiplies, gates — position matters, needs explicit placement).

**The structure roll:**
`root polarity -> shape source -> conversion (follows from polarity) -> draw k modifiers legal in the
resulting channel space (k itself rolled) -> structural post appended automatically.`
With ~5 shape sources and ~15 modifiers choosing 4–8, that is **tens of thousands of structurally
distinct stacks before a single parameter is touched** — which is what carries early variety while the
corpus is small.

**⇒ Rule: vary STRUCTURE aggressively, vary PARAMETERS conservatively.** Shift the ratio as the corpus
grows.

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

### ⚠️ 6.1 THE BLOCKER — box verdicts are only correct in the stack they ship in

CODEMAP, on the lattice corridor source: *"Its `EffectOverBox` answers for the source+carve **pair**
(Phase 1 simplification) so it must be told the downstream `ExtraReach`."* The op interface says the
same in its own comment: a source writing only `Sdf` does not touch density itself, so it answers on
behalf of the converter that follows it.

Fine when a human wrote the pair. **It breaks the moment the composer assembles a combination nobody
verified**, and the failure is the silent one: a tile wrongly proved uniform means no geometry, no
collision, no error, and a player falls through the floor.

⇒ **Prerequisite before free composition is safe: every operator's `EffectOverBox` must be correct in
ISOLATION**, not correct-given-its-neighbours. That means propagating an **SDF interval** through the
box query instead of letting the source answer for the pair — which the interface comment already names
as the Phase 3 version of the contract. Not huge, but load-bearing, and it must land first.

### ⚠️ 6.2 Box-verdict bounds are proved against HAND-SET params

T1.d / `ClassifyTile` bounds are proved today against the parameters Jahni typed. **If the seed invents
parameters, every bound must hold across the whole authored RANGE, not a value.** Same silent failure
mode as above. See `voxelforge-noise-bounds` — a bound must be the SUPREMUM of what `Eval` produces, not
the parameter named "max".

### ⚠️ 6.3 `ClassifyTile` will silently delete 3D caves

It proves anything below `TerrainZ` solid and skips meshing. Any future 3D generation inside mountains
needs a guard, or the caves vanish with no error.

### ⚠️ 6.4 The primordial law has a real hole TODAY

`UVoxelStrateManager::GeneratePassages` places every inter-strate passage as: `FRandomStream(CachedSeed
^ 0x50A55A6E)` → random angle around the (0,0) spine → random distance in the config range → random
reach into each strate. **Nothing consults the destination strate's cave layout.** Carving is a
structural-post invariant so the tube *is* air — but whether its lower mouth joins the lower strate's
connected space is pure luck. **A player can descend a passage and arrive in a sealed pocket.**

**Fix — make the strate OFFER a landing site, don't let the passage gamble.** Add to the field-source
contract something like `SuggestOpenPoint(WorldX, WorldY) -> optional Z`. Every source already knows
where its own air is: `FRoomGraphSource` the nearest room centre (hash-placed, cheap to find),
`MakeSlabVoidSource` anywhere in the void band, `FIslandBlobSource` just above an island top,
`FSurfaceColumnSource` just above `TerrainZ`, `MakeLatticeCorridorSource` the nearest lattice node.
Passage placement becomes: pick XY, ask the strate below for an open Z near it, aim there. Deterministic,
cheap, and it keeps working for invented strates because any new source must answer.

Then the measurement pass flood fill is the net: does the upper mouth's air component reach the lower
mouth's? If not, re-roll with the attempt folded into the hash.

### 6.5 Findability is mostly DATA, not code

`ELandmarkAnchor::PassageMouth` already exists — landmarks can anchor at passage mouths — and
`FStrateLandmark` already carries a Light-Orb block. **A glowing landmark at every passage mouth is
authorable today with no code at all.** Beyond that, cheapest first: light (a glow down a tunnel is the
strongest pull in a cave); sound (works around corners, which light does not — you hear the draft before
you see the hole); and ⭐ **make the mouth a PLACE, not a hole** — the `SuggestOpenPoint` fix already
lands the passage in a room, so make that room distinctive. Cracks in walls are forgettable; rooms are
landmarks, and that is what makes it findable on the *second* visit, which quests need.

### ⚠️ 6.6 The layout depends on the pool's CONTENTS *and its ORDER*

`Initialize` Fisher-Yates shuffles `Settings->StratePool` with `FRandomStream(WorldSeed)`, then cycles it
(`PoolCursor % Num`) when `TotalStrates` exceeds the pool. So **reordering the pool asset in the editor
changes every world**, and a pool smaller than the strate count repeats entries in a fixed order.

⇒ Use `hash(seed, slotIndex[, attempt])` instead, over a pool sorted by a stable key. The pool becomes a
**set** rather than a sequence: tidying the asset stops changing worlds, and each slot's roll becomes
independent of its neighbours'. Adding or removing a strate type still changes worlds — unavoidable, and
the reason content lands at wipes.

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
- **Order-independent placement** (§6.6). *Standalone value:* the strate pool asset can be reordered or
  tidied without changing every world. Small and isolated.

### Tier 1 — the primordial law (a BUG FIX, not composer work)
- **`SuggestOpenPoint` on field sources; passages aim at it** (§6.4).
- **The passage mouth becomes a room plus a distinctive landmark** (§6.5).

*Standalone value:* fixes a **live hole in the shipping game** — today a player can descend into a
sealed pocket. Worth doing even if the composer never happens.

### Tier 2 — the measurement pass (§3.4)
*Standalone value:* it **is** F1, `fable-idea.md`'s top-pick world-preview tool. Jahni can finally see
what parameters do instead of tuning blind. It also verifies Tier 1.

### Tier 3 — op-system prerequisites for free composition
- **Channel read/write declarations** on every op, so ordering derives from a DAG (§3.2).
- **Fuse corrective ops** (`FFloorBiasMod` into `FCaveRoughnessMod`).
- ⚠️ **`EffectOverBox` correct in ISOLATION** (§6.1) — the gate on everything after.
- **The XY edge seal** (§6.7).

*Standalone value:* the fuse and the isolation fix are both correctness improvements to shipping code.

### Tier 4 — the composer proper
Structure roll (§3.2) · parameter roll (§3.3) · reject-and-resample driven by Tier 2 (§3.5) · the
**offline season pipeline** with Jahni's review and veto (§3.1).

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
