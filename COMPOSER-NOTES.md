# COMPOSER-NOTES.md — running notes on the world-composer conversation

> Started 2026-08-17. Small on purpose: what we decided, what's still open, what must not be
> forgotten. Not a design doc — that comes later, if it earns it.

## The idea, in one line

**Jahni authors the vocabulary; the system writes the sentences.** He makes data assets (ops, biomes,
deco sets, set-pieces, whole handmade levels/strates); the system assembles strates and worlds out of
them from the seed, *logically*.

This is op-stack **Phase 3 + a composer layer above it**. The 2026-08-16 handoff's "do NOT start
Phase 3" is **superseded** — it closed the door on the one thing he actually wants.

## Decided

- **Ambition level (c):** the seed invents parameters within authored ranges. Not preset-picking.
- **Two seeds differ in everything** — not the same strates spawn, nor the details.
- **⭐ Primordial law:** *every strate has an entrance and an exit, exposed to air, findable.*
- **Handmade content plugs in** as stamps **and** as whole strates (e.g. a unique boss strate) —
  entries in the pool with special placement rules.
- **Build the connections first, generate around them.** Reachability by construction, not by luck.
  Precedent: the (0,0) spine is already a guaranteed vertical connector. Generate-then-check gets
  brutal at 8 strates/world.
- **Validation is 3 layers:** (1) construct-by-design so the law can't break, (2) *measurable* checks
  for accidents — air fraction, continuous walkable surface, feature size vs voxel scale, strate
  height sanity, (3) **reject-and-resample**: fold the attempt number into the hash
  (`hash(seed, strateSlot, attempt)`) so every seed still yields a valid world and peers stay in
  lockstep.
- **The AI good/bad judge is second, not first.** Not a fool idea — but the cost is *labels*, and
  Jahni is the only labeller. Layers 1–3 remove the numeric garbage; what's left is genuine taste,
  a much smaller job. The dataset then accumulates for free from seeds he plays and reacts to.
- **F4 save/load is the LOCK on the density field, not a feature.** Everything field-changing lands
  before it. The composer is the largest field-changing work that exists ⇒ far ahead of F4.
- **Acceptance criteria get stated up front and confirmed by Jahni** — never inferred. (The
  bit-identity criterion was self-imposed and cost 3 weeks; peer determinism never required matching
  an older build. Determinism = same build, all machines. Version stability = a gen-version byte.)

## Open

- What makes a **world** coherent above the strate level? (depth-driven rules? tag adjacency? an arc?)
- What is a strate *as a unit the composer picks* — pool + tags + depth bands? Fixed count per world?
- "**Findable**" ≠ "reachable". Reachable is a number; findable is placement (light, airflow,
  landmark, terrain funnelling you toward it). Whose job, and how expressed?
- Where do the authored **ranges** live, and can they be context-dependent (this op's range depends
  on what else is in the stack)?

## Hazards — do not forget

- ⚠️ **Box-verdict bounds under (c).** T1.d / `ClassifyTile` bounds are proved today against
  *hand-set* params. If the seed invents params, every bound must hold across the whole authored
  **range**, not a value. Wrong ⇒ a tile wrongly proved uniform ⇒ no geometry, no collision, no
  error, player falls through the floor. The only silent failure mode in the system.
- ⚠️ **`ClassifyTile` proves anything below `TerrainZ` solid** and skips meshing — it will silently
  delete 3D caves inside mountains unless guarded.
- ⚠️ **Validation must itself be deterministic** across every machine that runs it — including a
  future headless Linux dedicated server, where float behaviour may not match the Windows client.

---

## 2026-08-17 — the FEEL: magical, not logical

Jahni corrected the direction. He does **not** want a geologically reasoned world. He wants it to feel
**magical** — the game throwing countless worlds at the player, *of any kind, any size, any
peculiarity*. Examples he gave: an alien-looking surface strate; a **"dreamcore"** strate — repetitive
patterns, a strange sense of emptiness.

- **Adjacency rules: CONFIRMED** (he agrees with those).
- **Depth-as-meaning: UNDECIDED.** The old model was "deeper ⇒ weirder, stranger, spookier"; he is no
  longer sure that's right. Do not assume it.
- ⚠️ My earlier pitch ("coherence makes the world *readable*; depth should mean heat/pressure/danger")
  was arguing for a **different game**. Dropped.

**Reframe that survives his goal:** coherence lives **inside** a strate, not across the stack. The
strate is the unit of authorship — one complete, committed idea, thoroughly itself. Placement can be
arbitrary; the strate still feels authored.

Ideas on the table (none decided):
- **Rarity instead of depth.** Weird strates are *rare*, not *deep* — you could hit dreamcore at
  −200 m. Descent feels like drawing cards, which serves "countless worlds" better than a gradient.
- **Strangeness needs ordinary to be strange against.** Guarantee a share of grounded strates so the
  weird ones land. A rule about the **mix**, not about depth.
- **Adjacency rules become PACING rules:** no two alike in a row; a grounded strate sets up a weird
  one; special pairs (flooded over lava) earn a real transition.
- **Size and count are free variables** — a 40 m crawlspace strate vs a 2 km cathedral. Cheap to
  vary, huge effect on feel. "Any size" was explicit in his description.
- The **unique boss strate** implies a *destination without a gradient*. An end is enough; every step
  need not escalate.

**Open question left hanging:** does the world have a **bottom the player knows about**? "Boss strate"
implies an end; "countless worlds" implies endless. Those pull in opposite directions.

---

## 2026-08-17 — what the GAME is (context that reshapes the above)

**Fantasy exploration + combat RPG.** Medieval-ish, progressing toward better / more modern
infrastructure but staying inside fantasy. Side systems: **housing and farming**. Needs **bosses**,
and **fights that don't feel "blergh."**

Consequences for the composer:

- **Progression needs difficulty to correlate with position — but not by killing the magic.**
  Proposal: **draw strates freely (rarity, any kind, any size), then SORT the drawn set by their own
  danger rating.** Identity stays random; difficulty comes out ordered by construction. Depth then
  means *danger* without ever meaning *weirdness*.
- ⚠️ **Housing is the real lock on the density field — harder than F4 save/load.** A house has a
  *location* in a world that must keep existing. Once players build, the generator's output for that
  seed is frozen forever. Everything field-changing must land before housing ships.
- **Farming/housing need buildable space:** flat-ish, stable, safe, reachable, probably surface. A
  composer that only makes dramatic terrain has nowhere to put a farm. Likely: strate 0 stays the
  grounded home base.
- **F8 (ore veins) is promoted from reward-loop polish to BACKBONE.** "Progression to better
  infrastructure" is a tech tree, and a tech tree needs a supply chain. Ore quality should ride the
  same danger ordering.
- **Fights need fightable space.** Default caves are hostile to good combat — cramped, no sightlines,
  bad camera. Good fights want verticality, cover, chokepoints, room to retreat. ⇒ the composer must
  treat **arenas as a first-class output**, not hope one occurs. This is exactly why boss strates are
  handmade.

**Next open question:** is the home base fixed on the surface with the descent run as **expeditions**
(go down, come back up — cf. F14 lift/teleport anchors), or is it a **one-way descent**? Decides
whether strates are re-entered or passed through once, and shapes the entrance/exit law.

---

## 2026-08-17 — the world is a PUBLIC SHARED ARTIFACT (the biggest constraint yet)

- **Home = a hand-authored city, above ground, with a big opening inside** (the descent hub). Not
  generated. Ties to the primordial law: the city opening *is* the hand-placed entrance to strate 1.
- Natural buildable space where the world offers it; otherwise **terrain-editing tools** for players.
  Underground building: undecided, idea is alive.
- **Strates are revisitable and permanent to the player's view** — "fetch fish at the crystalline lake
  in strate X" must be a valid quest. Places are *discovered*, and **discoveries get shared online**.
- **Players share BUILDS across worlds.** Everyone's world is the same world, so a bridge one player
  builds can propagate to other players' worlds if it's liked. Community feeling of exploration.
  Jahni: "it all fails without players." Landmark **names** may propagate the same way.
- **Landmarks need names** — from a word pool or similar.

### What this forces

- ⚠️ **Version stability is now a REAL requirement, and I was too casual about it earlier.** A
  gen-version byte migrates a save file. It cannot migrate a *community's shared knowledge*. Once
  wikis, quests, videos and shared builds cite "the crystalline lake at (X,Y,Z)", the density field is
  frozen **globally and permanently**, not per-save. ⇒ **The composer must be finished before the
  world becomes public.** (This does NOT revive bit-identity to the OLD system — matching a dead
  build was always pointless. It sharpens *when* the future freeze lands.)
- **The diff layer is already the correct transport for shared builds:** position + compact
  modification records, deterministic to apply, "replicate diff events, never geometry". A shared
  build is a diff patch with an address.
- ⭐ **The composer must emit a WORLD MANIFEST, not just a density field.** A list of everything
  notable it placed — stable ID, type, position, extent, strate. Otherwise nothing can *name* the
  crystalline lake, no quest can reference it, no map can mark it, no wiki can cite it, no fast travel
  can target it. The composer already knows what it placed; it just has to write it down instead of
  discarding it. **Cheap now, enormously expensive to retrofit.**
- **Naming = two layers.** (1) A **deterministic procedural name** from a word pool, so every landmark
  has the *same* name for every player *before anyone visits it* — that is the substrate that makes
  quests and wikis possible at all. (2) An optional **community name** that propagates like builds and
  overrides the default. Beloved names win; nothing is ever nameless.
- **Shared-build hazards to design in, not bolt on:** two builds claiming the same place; a build that
  seals an entrance/exit and **violates the primordial law**; and content moderation — auto-propagating
  user-built voxel content into strangers' worlds needs a report/reject/rollback path, and shared
  builds must be individually removable client-side.

**Open fork:** does a shared build travel as **raw terrain edits** (voxel diff) or as **placed
structure pieces** (a prefab/building system)? Completely different to transmit, validate and moderate.

---

## 2026-08-17 — shared builds: BLUEPRINTS, not terrain diffs (fork resolved)

**Decided:** shared builds are **defined blueprint structures** (a parent/children blueprint family
made for construction), *not* raw terrain edits. Raw voxel edits are too hard to constrain.

**Vote system (Jahni's):** votes drive a priority list. Unvoted / negatively voted ⇒ back of the
queue. Sufficiently negative ⇒ **invalidated for sharing, never pulled again.** Where two builds
compete for the same zone, higher votes ⇒ better chance to appear.

### Why blueprints are the right call beyond moderation
- **Payload is tiny:** blueprint class ID + transform + variation params. Bytes, not a voxel diff.
- **Validatable without simulating voxels** — bounds are known, so "does it seal a passage / violate
  the primordial law" is a cheap check.
- ⭐ **It survives content updates.** Change the blueprint asset and every placed instance updates.
  A baked voxel diff can never be improved after the fact.

### Open design issues on the vote loop
- ⚠️ **Cold start / rich-get-richer.** A build must be *seen* to be voted on; if appearance is ranked
  by votes, new builds never appear ⇒ never get votes. Reserve a fraction of appearances for
  new/unvoted builds (explore vs exploit).
- **Rank by confidence, not raw count** — otherwise an old build with 1000 votes permanently outranks
  a better new one with 10. Some **time decay** too, or the top calcifies and the world stops changing.
- **"Never pulled again" is a hard delete** — needs volume thresholds, not just ratio, so a small
  group can't bury a build. Keep it soft/reversible server-side even if permanent to clients.
- Blueprint construction shrinks the crude-content surface but does not remove it (pieces can still be
  arranged). Report + rollback path still required.

### ⭐ Proposal: build SLOTS, not free coordinates
The composer marks **candidate build sites** in the world manifest (type, extent, strate — a bridge
site across a chasm, a shrine site at a passage mouth, a camp site on a ledge). Shared builds then
compete for *slots*, not arbitrary positions. Placement becomes valid by construction, "the zone" gets
a concrete definition, and conflict resolution is just "who wins this slot."

### ⚠️ Tension to resolve: canon vs roll
Earlier: the world is the *same* for everyone, so a wiki can cite it. But vote-weighted "more chances
to appear" implies each player **rolls** which builds they get. Both can't be fully true.
Proposed split: **terrain and landmarks are canon (identical for all); shared builds are a roll.**
The wiki cites natural features; builds are the variable garnish.
Also: **pin a build once a player has seen it** — the world may gain builds over time, but shouldn't
silently lose one you've come to know (or built your house next to).
**Player claims beat shared builds** — a shared build must never roll into player-owned ground.

---

## 2026-08-17 — SEASONS: worlds are wiped (this dissolves the freeze problem)

**Decided:**
- The **base generation** — everything defined at generation time — **is** the shared world, identical
  for everyone. **Shared builds are a LAYER ON TOP** that adds to it. (Confirms the canon/roll split.)
- **New and unvoted builds must be rolled in deliberately**, to give new players and new builds a
  chance. Confirmed.
- ⭐ **A world is STATIC once published, until it is WIPED** — perhaps every 6–12 months — to make
  room for generation changes and other evolution.

### What seasons change (important — supersedes my earlier warning)
- ⚠️ I said two messages ago that once the world goes public the density field is frozen **globally and
  permanently**, so the composer had to be *finished* first. **With wipes that is false, and it's the
  best news in this conversation.** Generation is frozen only **within a season**. Every wipe is a free
  window to change noise, ops, bounds, archetypes, anything.
- ⇒ The composer can ship **imperfect and improve every cycle**. No need to get it right before contact
  with players. Bit-identity anxiety is now dead permanently — worlds are *expected* to differ by
  version, by design.
- ⇒ **A world = seed + generator version + publish date.** Stamp the generator version into the
  published world; that stamp is the whole of "version stability" now.
- ⇒ Concrete unblock: **T2.a (SIMD noise)** and anything else that changes generator output was parked
  in `fable-idea.md` because it shifts seeds. At a wipe boundary that cost is **zero**. Same for the
  large-seed noise-collapse fix in `AUDIT-2026-07.md`.

### ⭐ Proposal: the BLUEPRINT LIBRARY is what survives a wipe
Your house dies with the world; your **design** doesn't. If shared builds are blueprint structures,
the corpus of player creations can persist **across** seasons — so the vote/priority pool grows richer
every cycle instead of resetting, and returning players carry something forward into a fresh world.
That makes the wipe survivable for the housing/farming players, who otherwise lose the most.

**Knowledge dying at the wipe is a FEATURE.** Re-discovery is the loop; a wiped world resets the
"everything is already solved and on the wiki" state that eventually kills exploration games. The
community re-explores together, which is the exact feeling Jahni is after. The **hand-authored city**
stays constant across seasons — the anchor of familiarity.

### Open
- **What else survives a wipe?** Account progression / unlocks / recipes / reputation / cosmetics?
  Nothing? (Pure reset is brutal on housing + farming players.)
- **One world at a time globally, or several shards on different seeds?**
- **Does singleplayer/offline get a permanent world**, or do wipes apply there too? (Many seasonal
  games run a permanent realm alongside the seasonal one.)

---

## 2026-08-17 — wipe scope, housing, and the vote pool

**Decided:**
- **The wipe is world-only.** Everything else survives (account progression etc.).
- **The vote/build pool does NOT survive a seed change** — nothing would be placed correctly. It is
  wiped, with **rewards for the top builders** (titles etc.), tiered by population (10/50/100/1000).
- **Housing → "instanced worlds":** custom personal worlds that are never wiped. Jahni finds this
  *slightly sad* — it cuts the player out of the strates and the shared world. Underground building
  still undecided.
- **Everyone shares one seed**, until the game is big enough to offer "reboots" (shards).
- **Singleplayer** was never the intent (online-first), but may become an option.

### Refinement: placement dies, DESIGN needn't
A blueprint build has two separable parts: **where it sits** (chasm-specific, absolutely world-bound —
Jahni is right that this cannot survive) and **the design itself** (pieces in relative arrangement,
placement-independent). So:
- **Shared/competitive pool: WIPED.** And the strongest argument isn't placement — placement *could*
  be re-matched to new slots by fit. It's **fairness**: carrying the corpus forward means season 2
  opens already full of season 1's voted builds, and every new player competes against an entrenched
  corpus forever. A fresh pool gives each season a fresh creative economy.
- **Personal library: KEPT (private).** A player keeps their own designs, so they can rebuild a
  beloved house in the new world and re-submit it to the new pool. **The loss is the location, not the
  work** — that is what makes a wipe emotionally survivable.
- **Titles carrying across** is what lets *reputation* survive when the builds don't — a season ending
  rather than a deletion. Optional knob: a small roll-boost for proven builders, which also softens
  the cold-start problem (watch that it doesn't recreate rich-get-richer).

### ⭐ The "slightly sad" instanced world may not be necessary
**The hand-authored city is already wipe-proof** — it isn't generated, so a generation change can't
invalidate it. Put persistent housing **there** (a district / plots / interiors, city-adjacent
authored farmland) and it survives every wipe with **no instancing at all**, keeping players inside
the shared world and visible to each other at the social hub. Instancing then shrinks to the standard,
small MMO problem of interiors/plots rather than whole private worlds.

### ⚠️ HAZARD — a never-wiped *generated* instanced world reintroduces the freeze problem
If a personal world is generated by generator v3 and is never wiped, then v3 must be kept alive
**forever** — every old code path, every old bound, pinned per world — or that player's world breaks.
That is the "support every version forever" trap, in a side corner, undoing exactly what seasons just
bought. Avoid by making persistent personal space **non-generated** (authored plot / city district),
or by regenerating it each season and **re-placing the blueprints** (possible precisely because
blueprint builds are placement-independent).

---

## 2026-08-17 — housing: bake the plot (and why a MESH is the wrong bake)

**Constraints from Jahni:**
- **The social hub is later** — infrastructure + difficulty. Not now.
- ⚠️ **The surface is very limited, on purpose** — story-wise it is *why* players descend for space and
  resources. ⇒ **housing everyone on the surface does not scale**, so the "put housing in the
  hand-authored city" idea above does not hold as the general answer.
- **His proposal:** bake the player's generated instance into a **mesh**, so the plot no longer depends
  on the generation system. Players can switch or destroy their house anytime; otherwise it's safe.

**That solves the version-pinning hazard properly** — a baked region needs no generator, so no old
generator has to be kept alive. Right instinct.

### ⭐ Refinement: snapshot the FIELD, not the mesh
Bake the sampled **density + material IDs** for the bounded plot region, not triangles:
- **A mesh can't be dug.** Housing/farming ship alongside player terrain-editing tools; a baked mesh
  kills that inside the one place players care most about.
- **A mesh freezes the plot against every future mesher/material improvement** (Transvoxel, normals,
  surface-class polygroups, F6 masks). That just moves the museum problem from the generator to the
  mesher. A field snapshot re-meshes with whatever the current mesher is.
- Density fields **compress very well** — most of a plot is uniformly solid or uniformly air.
- ⚠️ **The plot must be BOUNDED** or storage scales with ambition.

Architecturally this is: **a per-region override source in the generation path** — "this region has a
stored field, read it instead of evaluating the op stack."

### ⭐ Same mechanism ships hand-authored content
A hand-made boss strate or a distributed handmade level **is** a stored field region. The override
source Jahni needs for houses is the same one he needs for authored levels/stamps. **One feature, two
uses — build it once.**

### On the wipe, for a plot
Simplest split: **the house travels, the ground doesn't.** Blueprints are placement-independent, so
they re-place in the new world; the terrain regenerates. Avoids having to blend a baked plot into
fresh generated terrain (seams at the plot edge) — which would otherwise need a reserved "homestead
slot" plus a blend skirt. Keep that harder option in reserve, don't build it first.

---

## 2026-08-17 — housing: SETTLED

- **Personal housing = an instanced level**, not a place in the shared world. Their own tiny world;
  never wiped, never re-placed.
- ⭐ **The plot's ground is a snapshot of land the player CHOSE** in the shared world and claimed.
  Their home is *from* somewhere. After a wipe it is the last surviving fragment of a world that no
  longer exists — the wipe becomes meaningful rather than lossy, and the descent gains a second motive
  beyond resources: finding somewhere worth living.
- **One template level, populated from save data** — a stored field region + a list of blueprint
  transforms. **Not** one `.umap` per player (cooked assets can't be made at runtime).
- **Invites needed** — houses can be shared/visited.
- **Time-based growth computed from elapsed real time on entry**, not ticked (the instance isn't
  loaded while the player is away).

⇒ **Concrete plugin work implied:** a **stored-field source** for the voxel world (read a saved region
instead of evaluating the op stack) **+ its save format**. Same feature ships hand-authored levels /
boss strates. Bounded region size is the one thing to fix early.

---

## STILL AWAITING JAHNI'S RULING (my proposals — do NOT treat as decided)

1. **World manifest** — the composer emits a list of everything notable it placed (stable ID, type,
   position, extent, strate). Needed for landmark names, quest references, maps, fast travel, and
   build slots. Biggest architectural consequence of anything proposed here.
2. **Danger rating + sort** — draw strates freely (any kind, any size, any rarity), then order the
   drawn set by their own danger rating, so depth means *difficulty* without ever meaning *weirdness*.
3. **The mix rule** — guarantee a share of grounded/plausible strates so the strange ones land.
   Strangeness needs ordinary to be strange against.
4. **Build slots** — the composer marks candidate build sites; shared builds compete for *slots*
   rather than free coordinates.

---

## 2026-08-17 — RULINGS from Jahni on the four proposals

### 1. World manifest — ✅ ACCEPTED, "must not be expensive"
**Make it free by inverting it: the manifest is not an export, it is the PLAN.**
The composer's layout pass runs *first* — pick strates, place connections, place landmarks/rooms/
arenas — and that decision list **is** the manifest. The density field is generated *from* it. So it
is never derived by scanning terrain (which is what would be expensive and unreliable).
- **Only explicitly placed decisions go in** — landmarks, rooms/POIs, lakes, arenas, entrances/exits,
  build sites. **Not** emergent terrain features (every rock, every ledge).
- Rough size: ~8 strates × 50–200 entries × ~32–64 B ⇒ **~100 KB per world**. Computed once, identical
  for every player.
- **Names are computed, not stored:** `hash(entryID, seed)` → word pool. Zero storage.
- Must be computable **without generating terrain** — it is upstream of the field, not downstream.

### 2. Danger rating — ✅ ACCEPTED. Drives **mobs, environmental hazards, and loot**.
Refinements proposed:
- **Danger is a RANGE per strate archetype, rolled per world** ⇒ one dreamcore asset yields a gentle
  dreamcore *and* a terrifying one. Multiplies variety with no extra authoring, and serves "two seeds
  differ in everything."
- A strate asset therefore specifies its **theme/identity**, not its inhabitants; mob/hazard/loot pools
  are selected by **danger × theme tags**. Strates become reusable across the whole curve.
- **Ore quality rides the danger tier** ⇒ F8's supply chain and the difficulty curve are one axis.
- ⚠️ **Sort with jitter, not strictly.** Perfectly monotonic escalation reads flat; an occasional
  easier strate after a hard one gives relief and pacing.

### 3. The mix rule — ✅ ACCEPTED, sharpened by Jahni:
**dreamcore should be QUITE RARE, so players are puzzled when they find it "because it doesn't have
its place there."**
⇒ The effect is *not-belonging*, which is stronger than rarity alone:
- **Anomalous strates must NOT be blended in.** Normal neighbours get a coherent transition; an
  anomaly gets a **hard cut** — the wrongness lives in the seam. The adjacency system needs a flag for
  "this strate deliberately does not reconcile with its neighbours."
- Rare enough that a player may go a whole season without seeing one ⇒ in a single shared world, an
  anomaly becomes **community news** ("there's a dreamcore strate at −1400 this season"). Rarity plus
  one shared seed turns it into an event.

### 4. Build slots — ⚠️ "not bad but needs refining". Refinement:
- **Slots do not restrict building.** Players build anywhere valid. A slot is the **qualification for
  auto-propagation**: build in a slot and your build is eligible to spread to everyone; build freely
  and it is still yours, still visitable, still shareable by invite — just not auto-distributed.
  Resolves freedom vs safe validation without taking anything away.
- **Slots are DERIVED from the manifest, not separately authored** — a chasm entry implies a crossing,
  a passage mouth implies a marker/shrine, a ledge implies a camp. No new authoring burden.
- **A slot = position + shape constraint** (span, clearance, orientation) so blueprint fit is a
  mechanical check, which is what makes propagation safe.

---

## 2026-08-17 — danger: Z-MULTIPLIER, not a sort (Jahni's model — adopted, it beats mine)

**Decided:** no sorting pass. **Danger = a function of depth (Z) × the strate's own multiplier**, with
a **rare chance of a less-difficult strate appearing deep**.

**Why it's better than my sort:**
- **Local, not global.** Danger is computed per strate from its Z — no whole-world ordering pass, and a
  strate's danger is knowable without knowing the rest of the world's strate list.
- **Fully decouples identity from placement.** Any strate can appear at any depth; depth supplies the
  danger. That serves "any world, any kind, anywhere" better than sorting ever could — sorting
  implicitly binds a strate's identity to a position in the curve.
- **The rare exception is a designed feature, not the fudge-factor jitter I proposed.**
- ⭐ **It makes danger ANOMALIES possible in both directions**, which sorting structurally cannot:
  - **Respite strate** — unexpectedly calm, deep. Don't leave it as "lower numbers": make it a
    *designed type*. Quiet, visibly calmer, fewer mobs — a sanctuary. Natural home for a safe camp,
    a merchant, a landmark, a breather in the descent.
  - **Danger spike near the surface** — the "something is very wrong here" beat. Players find it early,
    die, and come back for it later. Pairs perfectly with the anomaly/dreamcore concept (out of place
    in *difficulty*, not only in *look*), and in one shared world it becomes season-wide news.

**Caveats to build in:**
- ⚠️ **Clamp the multiplier range so the depth term dominates on average**, or the curve stops being
  reliable and progression breaks. Rare exceptions are a feature; a noisy curve is a bug.
- ⚠️ **The exception must be LEGIBLE.** Unsignalled difficulty variance reads as bad tuning, not
  design. Each anomaly needs a tell.
- ⚠️ **Danger must be readable at a strate's ENTRANCE**, before the player commits. A surprise easy
  strate is a gift; a surprise deadly one without warning is just an unfair death. Give the entrance a
  broadcast — visual/audio/NPC warning/UI readout fed from the manifest. Warned-and-ignored is
  memorable; unwarned is unfair.
- Danger becomes a **manifest property** of each strate, so quests, UI and loot tables all read it from
  one place.
- The **mix rule (grounded vs anomalous) is independent of this** and still stands.

---

## 2026-08-17 — WHAT THE CODE ACTUALLY SAYS (read pass, no changes made)

### ⭐ The composer already exists in embryo: `UVoxelStrateManager::Initialize`
`Private/VoxelStrateManager.cpp:29`, roughly 100 lines. It already does the job's skeleton:
1. loads `Settings->StratePool` (soft ptrs) and **Fisher-Yates shuffles it** with
   `FRandomStream(WorldSeed)`;
2. walks `TotalStrates` slots, taking `Settings->FixedStrates[i]` where pinned, else cycling the
   shuffled pool (`ShuffledPool[PoolCursor % Num]`);
3. computes each slot's Z range from `Definition->StrateHeightInChunks`, leaving
   `InterStrateGapChunks` of bedrock between;
4. calls `GeneratePassages` — deterministic passages between consecutive strates.

**This runs BEFORE any terrain and its result (`StrateLayout`) is what the density field is derived
from.** That is exactly the "manifest is the plan, not an export" architecture — it already exists.
The composer is an *expansion of this function*, not a new system.

### ⭐ The manifest can cost ZERO storage — it needs to be ENUMERABLE, not stored
`FStrateLandmark` placement (`VoxelContentManager.cpp:1263 UpdateLandmarks`) is **hash-lattice**:
cell = `SpacingChunks` chunks, position = lattice point + hash jitter
(`VoxelHash::Mix(H ^ ...)`), plus a `PassageMouth` anchor mode that enumerates the finite passage
list. The file's own comment: *"cost scales with the NUMBER of landmarks in range, not the area."*
⇒ "what notable things are in this region?" is already a **pure deterministic function**, answerable
without generating terrain and without streaming. So the manifest is an **index computed on demand**,
not a baked table. That fully answers Jahni's "it must not be expensive."

### ✅ Already exists — do NOT redesign these
- **`EVoxelStrateTransition::Hard`** (per strate, on its lower boundary) **is** the "anomaly must not
  blend" mechanism. Already authored per strate. Gradient / Hard / Interleaved.
- **Height already varies 1–256 chunks** (~8 m to ~2 km). "Any size" is supported today.
- **`GameplayTags` on `UVoxelStrateDefinition`** — the tag vocabulary for adjacency rules and
  mob/hazard/loot selection is already there, and the module already depends on GameplayTags.
- **`UVoxelTerrainOpDefinition`** — ops ARE already data assets (12 types: Terrace, LayerLines,
  Ribbing, Cliff, Scallop, Overhang, Arch, Column, Pit, Chimney, Dome, Pinch), referenced from the
  strate's `TerrainOperations`, merged by `BuildParamsFromDefinition`.
- **Landmarks** already carry `AnchorMode`, `SpacingChunks`, `JitterFraction`, `SpawnProbability`,
  `ExclusionRadiusChunks`, `Priority` — F7's placement machinery, built. **This is the seed of build
  slots**: slots are landmarks of a "build site" kind.

### ❌ Genuinely missing for the composer
- **No rarity.** The pool is a flat `TArray` shuffled uniformly. Dreamcore-should-be-rare needs a
  weighted entry (`{Def, Weight, Tags, allowed depth band…}`), not a bare array.
- **Pool CYCLES** (`PoolCursor % Num`) when `TotalStrates` > pool size ⇒ repeats in a fixed order.
  "Two seeds differ in everything" needs a weighted draw instead.
- **No danger concept anywhere** in the strate definition or settings. Needs the Z-multiplier model.
- **No adjacency rules** — nothing prevents two alike in a row, nothing expresses "sets up" pairs.
- ⭐ **The op COMPOSITION is hardcoded C++**: `BuildTunnelNetworkStack` / `BuildMazeStack` /
  `BuildSurfaceStack` … one builder per archetype, in `VoxelDensityOpStack.cpp` (252 KB). The *ops*
  are composable; the *composition* is not data. **This is exactly the Phase 3 gap — the machine was
  built and never fed.** Confirmed against the code, not inferred.

### ⚠️ Constraints discovered
- **Layout = f(seed, pool CONTENTS, pool ORDER).** `FRandomStream` + Fisher-Yates over
  `Settings->StratePool` means **adding or reordering one strate in the pool changes every world**.
  ⇒ new strate types can only land **at a wipe**. Consistent with the season model, but it must be
  stated: mid-season content additions cannot touch the pool.
- ⚠️ **`FStrateGenerationParams` is a giant flat param bag** with an X-macro (`VF_STRATE_PARAM_FIELDS`)
  driving its `Lerp` blend. Ambition level **(c)** wants **authored ranges per parameter**, and there
  is no natural home for a per-field range in a flat struct of that size. **This is the main
  engineering obstacle to (c)** and the next thing worth designing.

---

## 2026-08-17 — the pool entry, and DERIVING distribution instead of authoring it

Jahni's objection to the pool-entry proposal (weight / max / min per entry): that is still
**hand-authoring the world's composition**. He wants the game to produce its own worlds.
Real distinction: he authors the **vocabulary**; he does not want to author the **distribution**.

### ⭐ The unifying move: MEASURE each assembled strate, don't declare its numbers
Reuse the coarse validation pass already required for garbage detection (low-res sample of the strate,
once at world creation: air fraction, walkable surface, feature size, connectivity). It produces
*numbers about the place* — use them for everything, not just yes/no. **One pass, three uses.**

| Was going to be authored | Derived instead |
|---|---|
| **Weight (rarity)** | **Outlier-ness**: distance from the pool's centroid in measurement space. Dreamcore is rare *because it is unlike everything else*. Self-correcting — author five dreamcore-ish strates and they stop being outliers, which is the right answer. |
| **Danger multiplier** | **Measured**: verticality (fall distance), openness (sightlines / room to retreat), traversal difficulty (climb/swim share), darkness. Discovered from what the strate generates. |
| **Max per world** | **Weight decay on draw** — an entry's weight drops sharply each time it's picked. Rare things don't repeat; common things recur without dominating. No per-entry number. |
| **Min per world (the mix rule)** | **One GLOBAL dial**: "a world's total strangeness may not exceed X." One knob for the whole game instead of one per strate. |
| Some descriptive **tags** | Measurable too — flooded = water volume, vertical = aspect ratio, open = mean free space. |

### Honest limits (stated to Jahni)
- **Intent can never be derived** — which strate is the boss's, which is fixed at the top. Authored,
  always, and rightly.
- **A measurement can be wrong about how a place FEELS** (measures ordinary, plays unsettling).
  ⇒ **derived by default, with an optional per-entry override that is normally empty.** The escape
  hatch exists without being an obligation on every strate.

⇒ Jahni's remaining authoring job: **make strates, and turn two or three global dials.**

**Through-line worth noting:** this is the same answer as the AI-judge discussion — *measure, don't
declare* — and it reuses the same pass. The design is cohering on one mechanism.

**Still on the table from the earlier proposal:** adjacency rules as a small tag-rule list in settings
(no two alike adjacent / `Anomaly` needs a `Grounded` neighbour above / max run length), the draw must
never fail (fallback entry), and making the draw order-independent — `hash(seed, slotIndex, attempt)`
over a pool sorted by a stable key, so reordering the asset in the editor stops changing every world.

---

## 2026-08-17 — ⛔ CORRECTION: Jahni does NOT want to author strates either

The line above — *"Jahni's remaining authoring job: make strates, and turn two or three global
dials"* — is **WRONG**. His words: *"I'm not pretty happy to make strates forever, that's actually
the opposite. If I could never make any new strate myself and have it do it itself would be genius."*

**⇒ The system must INVENT strates, not arrange authored ones.** I had the composer one level too
shallow for this entire conversation. Re-derive from here.

### What that means architecturally
- **A strate stops being an asset and becomes generated data.** `UVoxelStrateDefinition` survives as a
  struct (the generator needs its fields) but nobody fills it in the editor — the **composer** fills
  it: draws operators, rolls parameters, assigns materials/content, and hands the generator a strate
  that has never existed before and won't exist after the wipe.
- The **strate pool / weights / min / max** design above is therefore obsolete as stated. Rarity,
  danger and mix must be properties of *invented* strates, measured after assembly (the measurement
  idea survives the correction — it is *more* necessary now, not less).

### ⭐ The grammar for inventing a stack ALREADY EXISTS in code
`EVoxelOpRole` (in `Public/VoxelDensityOp.h`): **`FieldSource`** (what the field IS) ·
**`Combiner`** (how fields merge) · **`DetailModifier`** · **`StructuralPost`** (spine → seal →
passage, appended automatically, never author-omittable). A valid stack already has a defined shape:
one source, some modifiers, structural post last. "Invent a strate" = draw a source, draw modifiers,
roll parameters, in an order the roles already constrain. **The constraint system is sitting there
unused.**

### Where the hard part moved
- **Coherence was free when a human authored the strate. It is not free now.** A maze op + an overhang
  op + a flooded op at incompatible scales = mush. This is the garbage problem at full strength, and
  it is why the **AI-judge idea makes much more sense in this light** — it was never about judging
  hand-made content, it is about judging *invented* content, in a space too large to eyeball.
- **Dressing must be coherent too** — palette, mood, decorations, creatures, audio. Drawn independently
  you get crystal formations in a lava flow. ⇒ the composer draws a **theme** first, then pulls
  operators, materials, decorations, creatures and audio carrying that theme's tags.
- ⚠️ **The RANGE problem is no longer "at some point" — it is the centre of the design.** The ranges
  *are* the space the system invents inside.

### Open question put to Jahni
**Where is the floor of authoring?** Proposed: the **raw content** — operators, meshes, materials,
sounds, creatures — plus their tags. Those are the atoms; everything above (strates, worlds, layout,
danger, rarity) is composed and measured. Every operator added then *multiplies* with every other one
instead of adding one more place. **Or does he want the system to invent operators too?**

---

## 2026-08-17 — floor CONFIRMED; op invention parked

**Floor of authoring = raw content + tags.** Operators, meshes, materials, sounds, creatures, music —
tagged. Jahni accepts this ("I'd need to be thorough and tag every enemy, music and whatnot").
Nothing above that (strates, worlds, layout, danger, rarity) is authored.

### Inventing operators — parked, but not impossible
- ❌ **New KINDS of operator cannot be invented** ("warp the domain by noise", "carve capsules along a
  spline"). Those are ideas expressed as code ⇒ program synthesis. Bad bet, not where the value is.
- ✅ **Discovering good COMBINATIONS and promoting them into named reusable units** is real and
  buildable: a sub-stack that measures well becomes one building block. From the composer's view that
  *is* a new operator. **Compounds across seasons** — season 1's discoveries are season 2's vocabulary.
- ⚠️ **Do not chase it early.** ~20 ops with rolled parameters already spans a space vastly larger than
  a hundred seasons could explore. **The scarce resource is judgment, not vocabulary** — an hour on
  measurement/validation/the model beats an hour widening a space we use a rounding error of.

### Why the tagging burden is smaller than it sounds
- **Per-ASSET, not per-place.** Tag a bat once ⇒ every world, every season, forever. Cost scales with
  content, not with number of places. *This is the actual payoff of inventing strates rather than
  authoring them.*
- **GameplayTags are hierarchical and already a plugin dependency** — `Theme.Volcanic` reaches every
  volcanic context with no enumeration.
- **Some coherence needs no tags:** palette can be sampled from a material's dominant colour and
  clustered. A decoration whose colours sit in the strate's family fits, unstated.
- **Lazy:** untagged content is simply never drawn. Ship with 20 enemies and 3 tracks; add more
  whenever. Nothing breaks, the pool just gets richer.

**NEXT:** the range problem (now central, not a footnote).

---

## 2026-08-17 — THE RANGE PROBLEM (designed; verified against the code)

### Verified facts (not remembered — checked)
- `FStrateGenerationParams` has **73 scalar fields** (+1 bool). **Not hundreds** — tractable.
- ✅ **The X-macro is IN SYNC.** `VF_STRATE_PARAM_FIELDS` covers 75 entries; the only struct scalar
  missing from it is `Alpha`, which *is* the blend weight and is correctly excluded. CODEMAP's warned
  trap (add a field, forget the macro, blends silently reset it to default) **has not bitten**.
  ⇒ `FStrateGenerationParams::Lerp` already blends the whole parameter set, and is already load-bearing
  at strate boundaries. **Add a test asserting macro ≡ struct so this stays true.**
- `VoxelStrateTypes.h`: **135 `ClampMin`, 52 `ClampMax`** across 216 UPROPERTYs. Most fields have no
  upper bound. Paired min/max fields exist (`MinRoomRadius`/`MaxRoomRadius`, `TunnelMin/MaxRadius`,
  `RoomFloorCutMin/Max`) and some fields carry relational `EditCondition`s.

### Why rolling parameters fails (the reason to state plainly)
The coherent region of a 73-dimensional space is a vanishingly thin sheet; uniform sampling in a box
never lands on it. The problem is not wrong ranges — it is that **independent rolling destroys the
CORRELATIONS** that make a parameter set coherent (room spacing ↔ room radius ↔ tunnel length ↔ strate
height). This is very likely what Jahni was sensing when he reached for an AI judge.

### ⭐ The design: don't roll parameters — BLEND KNOWN-GOOD ONES
1. **Corpus** = known-good parameter vectors. Seeded by Jahni's existing hand-authored strate assets.
2. **Roll** = pick 2–3 parents (weighted), **blend with random weights**, then jitter.
3. **Reuse `FStrateGenerationParams::Lerp`** — it already does exactly this, over exactly this field
   set, and the world working already tests it. Build nothing.
4. **Constraint satisfaction is FREE:** a convex combination preserves every relation that holds in
   both parents (`Min ≤ Max` stays true). Correlations survive because they were never broken.
5. ⭐ **Ranges come from the CORPUS SPREAD** — per field, how much it varies across existing strates —
   **not from authored numbers and not from the clamps.** Clamps are "don't type nonsense" limits, not
   "this makes a good world" limits. *Measure, don't declare*, again.
6. **Clamps = hard safety net only.** Never the roll range.

### Escaping the convex hull
Blending alone only reaches *between* existing points. Escapes: jitter slightly past the corpus;
deliberate **extrapolation** past a parent away from the centroid (riskier ⇒ validate harder); and
⭐ **PROMOTION** — every invented strate that measures well and survives players joins the corpus.
Season 1 blends ~6 strates; season 5 blends 200 discovered ones. **The system's imagination grows from
its own history, and the game gets stranger as it ages.** (Third appearance of this mechanism today:
discovered ops, discovered strates, discovered parameter regions. It is the spine of the design.)

### ⚠️ Honest caveats
- **UPROPERTY metadata is editor-only** — `ClampMin`/`ClampMax` do not exist in a packaged build.
  ⇒ bake them at cook time via a small editor commandlet walking the struct by reflection. Small,
  real, one-time work.
- **Clamp coverage is partial** (52 of ~73 have no max). The net has holes; don't lean on it.
- **A few fields are not tunables** — `StrateTopWorldZ` / `StrateBottomWorldZ` are runtime Z bounds.
  Needs an explicit **exclusion list** for rolling.
- **Bools can't be blended** — roll feature toggles by probability, or inherit from the dominant parent.
- ⚠️ **The corpus is small today** (~6 strates), so blending alone will not feel like "countless
  worlds" in season 1. ⇒ **early variety must come from STRUCTURE, not parameters** — *which* operators
  are in the stack at all. 20 ops choose 5 is already thousands of distinct stacks.
  **Rule: vary structure aggressively, vary parameters conservatively — shift the ratio as the corpus
  grows.**

---

## 2026-08-17 — theme coherence narrowed; findability first step

- **Thematic mismatch is mostly FINE** — Jahni: a lava flow *could* have crystal formations, it was a
  bad example. ⇒ my "coherent dressing" worry was overstated. **The real garbage is GEOMETRIC**:
  operators at incompatible *scales* producing mush. Smaller and more mechanical than feared.
- ⭐ **Multiple fitting tags per asset is the mechanism, not a compromise.** One tag per thing = rigid
  buckets. Several tags = a graph with overlaps, and **the overlaps are where the system finds
  combinations Jahni didn't plan that still hang together** — which is the "magical" effect he wants.
  He describes assets; themes emerge from co-occurrence rather than being assigned.
- **Jahni's expected first big job:** create themes and tag content with multiple fitting tags.
- **Findability, first step:** a **subtle beacon glow**, initially for debugging. Note it is *not*
  throwaway — the debug beacon **is** the real solution in programmer art. The placement logic is
  identical; only delivery changes later (light spilling from the passage, a draft, a shift in
  ambience). Build the ugly version; ship a reskin.
