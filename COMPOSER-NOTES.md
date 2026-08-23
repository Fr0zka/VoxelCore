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

---

## 2026-08-17 — the MEASUREMENT PASS (and how "good" ever gets judged)

### ⭐ The reframe that sets the budget: the composer runs ONCE PER SEASON, OFFLINE
Everyone shares one seed per season ⇒ there is exactly **one world per season, ever** ⇒ the composer
never runs on a player's machine and never runs at load. It runs once, on Jahni's box or a build
machine. **The problem is not "fast enough for a loading screen", it is "runs overnight."** So it can:
- generate a thousand candidate strates, measure them all, keep the best eight;
- validate at any resolution it likes;
- later run a model over the candidates at **zero runtime cost to anyone**;
- ⭐ **let Jahni review the world before publishing the season.** The system does the work; he keeps a
  veto. Costs nothing because it is all offline anyway.

**What ships is small:** the season's seed + the chosen strates' parameter vectors. Clients generate
terrain from those exactly as today. **No runtime composition anywhere.**

### The pass itself: ONE coarse grid + ONE flood fill → one feature vector → three jobs
Sample the candidate into a coarse voxel grid (a few boxes at hash-chosen XY within its Z band), flood
fill the air once, derive everything from that grid. A 4–8× downsample preserves all of it.

**Validation (is it garbage?):** air fraction · **largest connected component share** (low = swiss
cheese: sealed pockets nobody reaches = generation paid for and never seen) · walkable surface (air
above, solid below, survivable slope) · **feature scale** (median distance-transform of the air —
catches "noise fog", structures smaller than a few voxels read as static not rock) · vertical
clearance · **entrance→exit reachability** (the primordial law, as a number).

**Danger (same grid, different questions):** fall exposure (walkable surface with a long drop beside
it) · **openness** (mean distance to solid from walkable positions — sightlines and room to retreat,
which is most of what makes a fight good or "blergh") · traversal mix (walk/climb/swim) · **tortuosity**
(path length ÷ straight-line — maze-likeness).

**Rarity:** nothing new — the vector of all the above, distance from the corpus centroid.

⚠️ **Coarse sampling can LIE about connectivity.** A wall thinner than the sample spacing vanishes and
two sealed spaces look joined ⇒ a strate could pass the primordial-law check while being impassable.
The one metric where a false pass is dangerous. **Fix: coarse to FIND the route, then re-verify that
one path at full resolution.** Cheap — only the corridor you found.

**This is the same tool as F1** (`fable-idea.md`'s top-pick 2D world-preview): the measurement pass
with a visualisation on top. Build the measurement, get the preview nearly free — and it's needed
anyway for reviewing a season before publishing.

### Judging "good" — Jahni is right to doubt, but the judge is the CORPUS, not a model
Measurements catch **broken**, not **boring**. The gap is narrower than it looks because *good* never
has to be defined — only **collected**:
- **Rate at the STRATE level, not the world level.** Twenty candidates, thumbs up/down = twenty labels
  in minutes; a hundred in an evening. Approved ones join the corpus and become blend parents.
  **This loop works with ZERO machine learning.** A model only makes it faster, later.
- **Player behaviour in a live season** — lingering, revisiting, rushing past. Free telemetry, but
  careful: time spent can mean *fascinating* or *lost and furious*.
- ⭐ **Where players choose to SNAPSHOT THEIR HOME.** An unambiguous "I love this place", no UI, no
  prompt, ungameable. The strongest quality signal in the design, and it costs nothing.

⚠️ **Known risk: convergence to a comfortable middle.** Promote only what you already liked, keep
blending near the corpus, and the system slowly stops surprising you. Same failure mode as the
build-vote pool, same counter: keep extrapolating past the corpus, and keep some unrated weird
candidates in circulation.

---

## 2026-08-17 — STACK ASSEMBLY: how the composer builds an op stack

### What the three real builders actually look like (read from `VoxelDensityOpStack.cpp`)
```
TunnelNetwork (19)   rock source → room graph (SDF) → SdfCarve → 12 density modifiers → worm source → structural
Maze (7)             rock source → lattice corridors (SDF) → SdfRoughness (SDF) → SdfCarve → structural
FloatingIslands (7)  VOID source → island blobs (SDF) → SdfRoughness (SDF) → SdfFill → structural
```

### ⭐ 1. Most ordering is MECHANICAL, not aesthetic — derive it, don't author it
Roughness sits on the **SDF** before conversion in Maze, and on **density** after conversion in
TunnelNetwork — CODEMAP confirms they are literally two different ops for that reason. An op's legal
position is fixed by **which channel it reads and which it writes**.
⇒ **Every operator declares its channel reads/writes.** Dependencies then form a DAG and **any
topological sort of that DAG is a legal stack.** No authored order, no judgement, and it generates
orderings no human ever wrote down.

### ⭐ 2. Root polarity is a one-bit identity lever
FloatingIslands is the same op classes as Maze but rooted in a **void** source with a **fill** instead
of a rock source with a carve — and you get islands instead of tunnels. One bit flips the entire
character of a strate.

### 3. `FFloorBiasMod` — delete the problem instead of documenting it
FloorBias exists purely to undo what `FCaveRoughnessMod` did to floors. Both are density-space, so
channel rules permit either order, but semantically one must follow the other.
⇒ **An operator whose only purpose is to correct another operator is not a separate operator. FUSE
them** into one op with two internal phases. Removes the invalid state rather than annotating it, and
leaves the rest genuinely free to shuffle. *Fifteen ops that compose in any legal order beat eighteen
with a footnote.* (Fusing with identical order and math leaves the world bit-identical.)

### 4. One small declaration covers what's left
Each op is **additive** (small displacement, commutes freely with peers ⇒ shuffle at will) or
**transformative** (clamps, multiplies, gates ⇒ position matters, needs explicit placement).

### 5. The structure roll
`root polarity → shape source → conversion (follows from polarity) → draw k modifiers legal in the
resulting channel space (k itself rolled) → structural post appended automatically.`
With ~5 shape sources and ~15 modifiers choosing 4–8, that is **tens of thousands of structurally
distinct stacks before a single parameter is touched** — which is what carries early variety while the
corpus is small.

### ⚠️ 6. THE BLOCKER — box verdicts are only correct *in the stack they ship in*
CODEMAP, on the lattice corridor source: *"Its `EffectOverBox` answers for the source+carve **pair**
(Phase 1 simplification) so it must be told the downstream `ExtraReach`."* The op interface says the
same in its own comment: a source that writes only `Sdf` doesn't touch density itself, so it answers
on behalf of the converter that follows it.

That is fine when a human wrote the pair. It **breaks the moment the composer assembles a combination
nobody verified**, and the failure is the silent one: a tile wrongly proved uniform ⇒ no geometry, no
collision, no error, player falls through the floor.

⇒ **Prerequisite before free composition is safe: every operator's `EffectOverBox` must be correct in
ISOLATION**, not correct-given-its-neighbours. That means propagating an **SDF interval** through the
box query instead of letting the source answer for the pair — which the interface comment already
names as the Phase 3 version of the contract. Not huge, but load-bearing, and it must land first.

### ✏️ Correction to yesterday's note
The X-macro / struct diff said *"the only struct scalar missing from the macro is `Alpha`, which is the
blend weight and correctly excluded."* **`Alpha` is not a field at all** — it is the `float Alpha`
parameter of `FStrateGenerationParams::Lerp` (line 1033), which the regex caught by accident.
**The macro and the struct are in PERFECT sync, zero exceptions.** Now guarded by a test — see below.

---

## 2026-08-17 — the X-macro guard (built, green)

`Source/VoxelForge/Private/Tests/VoxelForgeStrateParamCoverageTest.cpp` →
**`VoxelForge.Determinism.StrateParamBlendCoverage`**. Built clean first try (Jahni, 2026-08-17).
Expands `VF_STRATE_PARAM_FIELDS` a **third** way (after LERP and SNAP) into a name list and diffs it
against `FStrateGenerationParams`' UObject reflection. No fixture, no world — a shape test.
`GExemptFieldNames` is **empty**: every reflected field is covered, and an exemption must be written
down as a decision. Matters more under the composer, which invents parameter sets through that same
`Lerp` — a missing field would be **constant across every invented strate** with nothing to notice it
by. CODEMAP §3.12 row added.

---

## 2026-08-17 — ⚠️ THE PRIMORDIAL LAW HAS A REAL HOLE (verified in code)

### What passages do today
`UVoxelStrateManager::GeneratePassages` places every inter-strate passage as:
`FRandomStream Rng(CachedSeed ^ 0x50A55A6E)` → **random angle** around the (0,0) spine → **random
distance** in the config range → **random reach** into each strate. Endpoints are `(PX, PY, TopZ)`
and `(PX, PY, BottomZ)`. Carving is a structural-post invariant (`VF_ApplyPassageCarving`), so the
tube itself IS air by construction.

### ⚠️ The hole
**Nothing consults the destination strate's actual cave layout.** The tube is air, but whether its
lower mouth *joins the lower strate's connected space* is pure luck. **A player can descend a passage
and arrive in a sealed bubble** — legal geometry, air all the way, and nowhere to go. The primordial
law is currently a hope, not a guarantee. It gets worse under invented strates, where nobody has
eyeballed the layout.

Also: that `FRandomStream` walks **sequentially across all strates**, so changing an early strate's
connection count shifts **every later passage**. Same order-dependence as the strate pool
⇒ use `hash(seed, strateIndex, connIndex)` instead.

### ⭐ Fix 1 — don't let the passage hope; make the strate OFFER a landing site
Add to the field-source contract something like **`SuggestOpenPoint(WorldX, WorldY) → optional Z`**.
Every source already knows where its own air is:
| source | open point |
|---|---|
| `FRoomGraphSource` | nearest room centre (rooms are hash-placed ⇒ cheap to find) |
| `MakeSlabVoidSource` | anywhere in the void band |
| `FIslandBlobSource` | just above an island top |
| `FSurfaceColumnSource` | just above `TerrainZ` |
| `MakeLatticeCorridorSource` | the nearest lattice node |

Passage placement becomes: pick XY → **ask the lower strate's source for an open Z near it** → aim
there. Deterministic, cheap, and it keeps working for **invented** strates because any new source must
answer. Nice inversion: the strate offers the landing site instead of the passage gambling.

### Fix 2 — verification as the net (reuses the measurement pass)
The measurement pass already flood-fills the air. So: **does the upper mouth's air component reach the
lower mouth's?** If not, re-roll the passage (`hash(..., attempt)`). Coarse to find, full resolution to
confirm the one corridor — as established for the connectivity metric.

### Fix 3 — FINDABILITY, and most of it is data, not code
✅ **`ELandmarkAnchor::PassageMouth` already exists** — landmarks can anchor at passage mouths, and
`FStrateLandmark` already carries a **Light-Orb block**. A glowing landmark at every passage mouth is
authorable **today, with no code at all.**

But *findable* means being drawn there from a distance, not marked once you arrive. Three layers,
cheapest first:
- **Light.** A glow down a tunnel is the strongest pull in a cave. Already supported.
- **Sound.** An emitter with a long attenuation radius at the mouth. Works **around corners**, which
  light does not — in a cave you hear the draft before you see the hole. (Ties to F9 audio.)
- ⭐ **Make the mouth a PLACE, not a hole.** Fix 1 already lands the passage *in a room*; make that
  room distinctive — bigger, lit, decorated. Places are memorable, cracks in walls are not. This is
  the one that actually makes it findable on the second visit, which is what quests need.
- *Parked (strongest, most invasive):* bias the cave network to converge toward the mouth, so simply
  following tunnels tends to lead there.

---

## 2026-08-17 — RULING + BUILD ORDER

**Jahni's ruling:** guaranteeing an open landing place is **needed**. And the passage mouth should be
**a real landmark — a special place**, so it *shows* and is *less dismissed*. Not merely a lit marker.
Also: *"not all to be implemented in a row."*

### The sequencing principle
⭐ **Every tier must be worth doing on its own, before the next one exists.** Nothing gets built purely
as machinery for a later tier. (This is the direct antidote to the 2026-07/08 refactor, where three
weeks of correct work produced a world unchanged by a single voxel.)

### Tier 0 — safety nets, true regardless of the composer
- ✅ **X-macro guard test** — DONE, built green 2026-08-17.
- **Order-independent placement:** replace the sequential `FRandomStream` in `Initialize` and
  `GeneratePassages` with `hash(seed, slotIndex[, connIndex, attempt])`. *Standalone value:* Jahni can
  reorder or tidy the strate pool asset **without changing every world**. Small, isolated.

### Tier 1 — the primordial law (a BUG FIX, not composer work)
- **`SuggestOpenPoint(X, Y) → optional Z`** on field sources; aim passages at it.
- **Passage mouth becomes a room + a distinctive landmark** (per the ruling).
*Standalone value:* fixes a **live hole in the shipping game** — today a player can descend into a
sealed pocket. Worth doing **even if the composer never happens**, and it makes the current world
better immediately.

### Tier 2 — the measurement pass
Coarse sample + one flood fill + the feature vector (air fraction, largest component, walkable surface,
feature scale, clearance, reachability, fall exposure, openness, traversal mix, tortuosity).
*Standalone value:* it **is** F1, `fable-idea.md`'s top-pick world-preview tool — Jahni can finally see
what parameters do instead of tuning blind. It also verifies Tier 1 (does the mouth actually connect?).

### Tier 3 — op-system prerequisites for free composition
- **Channel read/write declarations** on every op ⇒ ordering derives from a DAG.
- **Fuse corrective ops** (`FFloorBiasMod` into `FCaveRoughnessMod`) — an op that only repairs another
  op is not an op.
- ⚠️ **`EffectOverBox` correct in ISOLATION** (SDF interval propagation instead of "the source answers
  for the pair"). **This is the gate on free composition** — without it, a novel stack can wrongly
  prove a tile uniform ⇒ no geometry, no collision, no error.
*Standalone value:* the fuse and the isolation fix are both correctness improvements to the code that
ships today.

### Tier 4 — the composer proper
Structure roll (root polarity → shape source → k modifiers) · parameter roll (blend the corpus via
`Lerp`) · reject-and-resample driven by Tier 2 · the **offline season pipeline** with Jahni's review
and veto.

### Tier 5 — the long game
**Promotion** (good strates rejoin the corpus) · theme/tag draws for materials, creatures, audio ·
eventually the model. All of it optional, all of it compounding.

---

## 2026-08-17 — WORLD SHAPE + TRAVERSAL (the two holes Codex found, now closed)

Codex read `COMPOSER-NOTES.md` cold, in isolation, and correctly identified the project, the goal
(the **corrected** one — the system invents strates, Jahni supplies vocabulary), and the intended
feel. It found all three marked reversals and no unmarked one. Two of its five "could not determine"
items were real holes; Jahni closed them:

### Decided
- ⭐ **The world has NO BOTTOM.** Endless descent.
- **Boss strates are INTERMEDIARY** — roughly every 5 strates. Bosses punctuate, they do not terminate.
- **Return trip, two ways:** walk back up through the tunnels (that is content, not a chore), **or**
  once a strate has been visited it opens a passage onward, so transport (elevator/other) can carry you
  down, **choosing which strate to travel to**.
- **Unlock is LOCAL (per player) for now.** Base scope = a **local server** fed by **Jahni's database**
  for community information. **Global/world-level unlock is a possible future** — gated on strates
  being large enough that finding the next passage is not "a 10 minute endeavour of searching".

### Consequences
- ⚠️ **No bottom breaks "generate the whole world offline at season start."** Fix: pre-generate a
  **deep buffer** (~30 strates) offline; extend in another offline batch if players approach its end.
  A season probably never exhausts it, and the **review/veto step survives intact**.
- ⚠️ **Danger needs a ceiling.** `danger = f(depth)` with no bottom grows forever ⇒ everything eventually
  unsurvivable. It must plateau or approach an asymptote. Open question that shapes the whole curve:
  **does strate 40 differ from strate 30 in danger, or only in what KIND of place it is?**
- **"Every 5th slot is a boss" needs a PERIODIC fixed-slot rule.** `Settings->FixedStrates` is a
  `TMap<int32, …>` of absolute indices today — the composer needs `slot % N` as well.
- The boss is therefore a **GATE**, which is where the transport network naturally hangs (the route past
  strate 10 opens when the boss at 10 falls). Gives the descent a rhythm: five strates, then a wall.
- **"Select which strate to travel to" is a menu — and that menu is the WORLD MANIFEST** (names, danger,
  discovered state). Third time the manifest has paid for itself.
- ⭐ **The global-vs-local call is MEASURABLE, not a guess.** The measurement pass already computes
  entrance→exit path length and tortuosity; a live season gives the real number by telemetry. Decide it
  from data later. (Same principle as everything else here: measure, don't declare.)

### ⚠️ Architecture consequence — the community layer must be OPTIONAL AT RUNTIME
Local server + external database means a DB outage, a network hiccup or maintenance must **degrade**,
never break. The world is generated from the seed and needs no DB at all. So: **procedural names are
the fallback** when community names cannot be fetched (the deterministic name always exists by
construction), shared builds simply do not appear, votes queue locally and sync later. Cheap now,
genuinely painful to retrofit once a hundred call sites assume the fetch succeeded.

### ⚠️ Clarification — elevator unlocks DIE WITH THE WIPE
The wipe is world-only and "everything else survives", **but the unlock network is world-scoped**: it
references this season's strates, and next season strate 7 is a different place. Carrying it forward
would also skip the new season's frontier, which is the point of wiping. **Named here before a player
discovers it the hard way.**

---

## 2026-08-17 — DEPTH ECONOMY + material placement (and the manifest is confirmed a NEED)

### Decided
- **Gear stats have a CEILING (~depth 30).** Past it, loot has the *same* stats but better natural
  **rolls** (Trove-style stat-quality %) ⇒ **less material to level up**. Depth buys **less grind and
  unique access, never bigger numbers.**
- **Past the ceiling, going deeper is about pride, challenge, and seeing new things.** Confirmed
  intent, not an accident.
- **Trading will exist** — lightly at first, better once the surface becomes a social hub.
- ⭐ **Depth-locked materials are tagged by TIER, not by strate** — e.g. a `post difficulty content`
  tag — **so placement stays random.**
- ✅ **The world manifest is confirmed a NEED**, not a proposal.

### Why the tier tag is better than a fixed depth (Jahni's call, and it is the stronger one)
A fixed depth is **solved once and stays solved forever**. A random placement inside a difficulty tier
must be **found again every wipe**, and in a one-seed shared world the answer spreads. *"Where is X this
season?"* becomes a renewable community question — exactly what the shared-knowledge design wants, at
zero extra cost.

### Consequences
- **Guarantee what recipes depend on; leave the rest to chance.** One flag on the material: *essential*
  ⇒ the composer must place it somewhere in the generated buffer; *optional* ⇒ the dice decide, and a
  season lacking it gets an identity ("the one with no X"). Essential-but-absent is a dead end nobody
  can fix.
- ⭐ **MANIFEST ≠ PLAYER KNOWLEDGE.** The manifest holds ground truth (the composer placed it, and
  quests/validation/fast-travel need it). **Discovery state is a separate overlay**, per player. Easy
  now; awkward to unpick if the client is handed the whole manifest up front.
- ⚠️ **You cannot hide the manifest anyway** — the client generates terrain from the seed, so players
  hold the generator and can enumerate it offline. Not a leak to prevent; a fact to design around, as
  Minecraft does with seed maps. And it barely matters here: community knowledge spreading **is** the
  intent, datamining is only a faster route to it, and **the wipe is what makes it not matter** — a map
  of season 4 is worthless in season 5.
- ⚠️ **The danger growth rate past the gear ceiling IS the endgame difficulty curve.** Below 30 gear and
  danger climb together; above it gear is capped and danger is not, so skill/strategy/consumables are
  the only levers left. That single number decides where the wall sits. Everything else is content;
  that is tuning.
- **Reward converges while danger does not** — a roll-quality % asymptotes at 100. Deliberate: the deep
  game is sustained by scarcity and pride, not progression.
- **Trading is a community-layer feature** ⇒ it must degrade gracefully when the database is
  unreachable, like shared builds and community names.

---

## 2026-08-17 — THE WORLD MANIFEST: design

### ⭐ It is TWO things, not one
**Tier A — the SPINE. Finite, tiny, actually stored.** Strate slots + passages. ~30 strates plus a few
passages each ⇒ ~100 entries, kilobytes. **This already exists** as `StrateLayout` + the passage list,
computed once in `UVoxelStrateManager::Initialize`. It is the world's skeleton.

**Tier B — the FIELD. Infinite in XY, therefore NEVER stored.** Landmarks, rooms, material deposits,
build sites. The world does not end sideways, so this cannot be a table — it is a **function**: give it
a region, it enumerates what is there. Deterministic, hash-based, **zero bytes**.

⇒ That split is what makes "must not be expensive" true rather than aspirational.

### ⭐ The trick: THE ID ENCODES THE POSITION
The hard requirement is resolving an ID **back** to a place (a saved quest target, a fast-travel
destination, a wiki citation) — normally a lookup table, which is exactly what we cannot have.
Landmarks are placed on a **hash lattice**, so build the ID from **lattice cell + type + slot index
within the cell**. Then **ID → position is ARITHMETIC**: no table, no database, no storage. A quest
stores `strate 4, cell (−128, 73), type Lake, index 2` and resolves it instantly, forever, on any
machine, offline.

**Names fall out of the same thing:** `hash(id, seed)` → word pool. Every entry in an infinite world
has a stable, globally identical name that nobody stored and nobody generated in advance — including
entries no player will ever visit. (This is the substrate that makes quests and wikis possible; see the
two-layer naming note, community names override on top.)

### What earns an entry
**One test: can a system need to ask "where is it?" or "what is it called?"** If yes, it is an entry.
✅ strates · passage mouths · landmarks · notable rooms · material deposits · build sites · boss arenas ·
the (0,0) spine. ❌ individual rocks · every tunnel segment · terrain that merely emerged from noise.

**Underneath that — ADDRESSABLE vs NOTABLE.** Everything hash-placed is *addressable* for free (it has
an ID by construction). **Notability is a filter on top, and it is exactly what the measurement pass
produces**: biggest chamber in a region, the one with a lake, the one at a passage mouth. "Notable
rooms" needs no new placement logic, only a threshold.

### Discovery sits ON TOP, never inside
A player's discovered set is a set of **IDs** — small integers, cheap to store, cheap to sync. Community
discovery is the union. **The manifest never changes; only who has seen what.** (Confirms MANIFEST ≠
PLAYER KNOWLEDGE from the depth-economy note.)

### ⚠️ Two cautions
- **The hash functions become part of the FROZEN SEASON CONTRACT.** Change how a lattice cell hashes and
  every quest target in that season moves. Fine — worlds are frozen within a season — but these
  functions are **not implementation details to tidy mid-season**.
- **Tier B is cheap, not FREE.** Enumerating rooms in a region needs that region's room-placement pass.
  Fine for a beacon or nearby markers; a full-continent map view would hurt. ⇒ **LOD the query**: coarse
  regions return only high-notability entries — which is what a zoomed-out map wants anyway.

---

## 2026-08-17 — NEAR-INFINITE STRATES: three consequences

Jahni: some strates (surface-like especially) are **near-infinite in XY** — storage is impossible.
Confirms the Tier A / Tier B split. But it also quietly weakens three things already decided:

### ⭐ 1. The (0,0) SPINE is what makes an infinite world navigable — state it as a law
In a bounded cave you eventually stumble onto the way down. In an infinite surface strate you never
will — the exit is a point in an endless plane. But `GeneratePassages` already places passages at a
random **angle + distance from the origin**, within a configured range, so **passages cluster near the
spine by construction.**
⇒ **The descent is anchored to the spine; infinity is OPTIONAL content.** A lost player walks back to
the spine and finds the way down. Everything outward is exploration you choose.
⚠️ This is currently an **accident of how the code happens to work**, not a stated invariant. It should
be written down as a law alongside the entrance/exit one.

### 2. The measurement pass must sample WHERE PLAYERS GO
An infinite strate cannot be measured, only sampled — and a box a million units out measures a place
nobody will ever stand in. ⇒ **validate the neighbourhood of the spine and the passage mouths**, the
load-bearing part. A strate may be beautiful at the origin and mush ten km out; that is genuinely fine.
Also: report measurements as **distributions (median + spread), not single values** — regions really
will differ, especially once biomes vary the surface. A mean pretending to describe infinity is a lie.

### ⚠️ 3. CORRECTION — essential materials need a DENSITY, not a location
Earlier note said the composer must place an essential material "somewhere in the buffer". **In an
infinite strate "somewhere" is unfindable** — it could be a thousand km out. The guarantee must be a
**placement density within the reachable band**: near enough to the spine, common enough to actually
meet. Otherwise a season ships a recipe nobody can complete and it reads as a bug, not as scarcity.
Conveniently this is measurable: *expected number of deposits within N of the spine* is a number the
composer can check.

---

## 2026-08-17 — the CITY is a plain UE level (no seam problem after all)

**Correction to my "authored city meets generated surface" worry:** the hand-authored city is a
**normal Unreal level**, not voxel terrain. VoxelForge sits **under the hole in the city**, or
possibly **instantiated in a whole separate level** to be free of the city's constraints.
⇒ **There is no authored/generated seam.** They are not the same kind of thing.

### Open / to confirm
- ❓ **Is the voxel world therefore entirely SUBTERRANEAN?** If the real outdoors is the city level,
  then `SurfaceWorld` is not a surface — it is a cathedral-sized underground space that *reads* as
  outdoors (consistent with the existing design where surface/sky strates are enclosed, lit from
  within by skylight + gem-star ceiling, never by a sun). **Asked, not assumed.**
- ⚠️ **Separate level vs sublevel is a real tradeoff, worth settling before both exist.** A distinct map
  gives the voxel world its own world settings, lighting, kill-Z and streaming, free of the city's —
  genuinely valuable. **The cost is that the descent becomes a LOAD rather than a walk.** For a game
  whose core verb is descending, a seamless transition through the hole would feel far better, and UE
  can do it with both as sublevels of one world — but then the voxel world inherits the city's world
  settings, which is exactly the constraint being escaped. *"Free of constraints"* and *"you can walk
  down into it"* are in tension.
- **Infinite vs bounded world is UNDECIDED.** Jahni: *"I might not make infinite world also, but for now
  I might, so I prefer saying it."*
  ⇒ **Keep designing for INFINITE**: it degrades gracefully to bounded, while a bounded-assuming design
  (stored manifest, complete map, exhaustive validation) breaks the moment the world is not.
  ⇒ But note bounded is a **lever, not merely a limit** — it would buy complete measurement instead of
  sampling, a real map, and much easier findability. A live option to pull deliberately one day.

---

## 2026-08-17 — BOUNDED world, SUBTERRANEAN confirmed, and the descent shaft

### Decided
- ⭐ **The world is BOUNDED.** Still gigantic maps, but finite.
- ✅ **The voxel world is entirely SUBTERRANEAN** — everything is under the surface; some strates only
  *mimic* being surface-like. (The real outdoors is the city's UE level.)
- **Transition:** likely a **master level** holding shared elements (lighting etc.), switching when the
  player enters the hole, with a transition playing. **The elevator can be as long as needed** — but
  Jahni intends to place the **first strate well below** so travel time covers generation naturally.

### What BOUNDED buys
- ⭐ **The primordial law becomes PROVABLE, not sampled.** The composer can flood-fill an *entire*
  strate at coarse resolution, so entrance→exit reachability is a **proof**, not a probability. Same
  for largest-connected-component and walkable surface — true statistics, not estimates from boxes.
  For the one law Jahni called primordial, that is the difference between a check and a guarantee.
- **Budget note:** 8 km × 8 km at 4 m sampling ≈ 200 M cells (too many); at 16 m ≈ 3 M (nothing).
  Resolution scales with map size. Aliasing caveat still applies — **coarse to find the route, full
  resolution to confirm the one corridor.**
- **"Explored" becomes a real number** — a finite volume means *"the community has explored 34 % of
  season 5"*. A genuine collective progress bar for a shared world, free once bounded.

### ⚠️ NEW REQUIREMENT — an XY world-edge seal, which does not exist
Verified: `VF_ApplyBoundarySeal(float& Density, float WorldZ, float StrateTopZ, float StrateBottomZ,
float Thickness, float BaseDensity)` is **purely VERTICAL** — it seals each strate's ceiling and floor
so nothing punches through except passages. There is **no horizontal equivalent**, because until now
the world had no sides. A bounded world does.
⇒ Build it as a **fourth structural invariant** alongside spine → seal → passage: a **forcing** op,
appended automatically, not author-omittable. Two reasons that shape is right: an invented strate
cannot forget it, and because forcing ops prove `AllSolid` in their band, **the world's outer shell
becomes free to skip** in tile classification.

### The transition, and ⭐ the descent shaft as the game's front door
- **Distance gives a FIXED time budget, not an adaptive one** — fall speed × depth is the same
  wall-clock on every machine, so a slow disk still lands in ungenerated space. **An elevator can
  stall for a ready signal; a free fall cannot.** If it's a drop, size it for the worst machine.
- ⭐ **Whatever sits between the hole and the first strate is the highest-traffic space in the game** —
  every player, every session, forever. It is currently an *accident* (just the gap streaming needed).
  It should be authored deliberately, because it is already doing several jobs:
  - **sets the tone** for everything below (a featureless drop reads as a loading screen; a worked
    shaft with old machinery and lights going into the dark reads as "people have been here");
  - **it is the elevator lobby** ⇒ the natural home of the manifest-driven **strate-select menu**, as a
    real place rather than a floating panel;
  - ⭐ **it is where depth becomes legible** — wall markings, and **the community's depth record for the
    season marked on the shaft**, passed on the way down. The season's collective arc made physical, in
    the one place everybody looks;
  - later: shops, NPCs, surface-side infrastructure.
- Costs nothing architecturally (city-level authored geometry, or the top of the spine) but turns a
  technical necessity into the game's threshold.

---

## 2026-08-17 — ACTOR-SPACE sweep (built, working)

**Why:** the city's hole is not at world (0,0), so the `AVoxelWorld` actor must be placed under it.
Jahni: *"we'll have it to be fully actor relative, just in case."*

**What was actually true before:** the plugin was **half** actor-relative. Correct already —
`GetBiomeAtWorldLocation`, `GetVoxelSurfaceHeightAt`, `UpdateDecorations`, `UpdateLandmarks`,
`UpdateWater`, `UVoxelDensityVolume::Update`, the passage debug draw, the clipmap material. **Not**
correct — the streaming centre, the streaming anchors, all six carve/fill entry points,
`GetStrateAtPosition`, and `UVoxelAtmosphereManager::UpdateForPlayer` (whose own comment *documented*
the assumption: *"actor at origin/identity"*). It only ever worked because the actor sat at the origin.

⚠️ **The trap avoided:** the content managers already convert internally, so converting `PlayerLastPos`
at the source would have **double-converted four call sites**. The fix had to be per-boundary, not at
the top. Found only by reading each consumer — the estimate before that read was wrong in both
directions.

**Landed:**
- `AVoxelWorld::WorldToLocalCm` / `WorldToLocalVoxel` / `LocalVoxelToWorld` (BlueprintPure) — declared
  as **the only sanctioned boundary** between Unreal coordinates and voxel coordinates.
- ⭐ **A grep-checkable invariant, which is the real deliverable:** *no `/ VOXEL_SIZE` applied to a
  parameter named `World*` outside those three functions.* The risk was never a missed site among the
  ones we found — it is the **next** one nobody notices.
- 11 call sites converted (counted before and after; every old pattern now zero).
- ⚠️ **Translation-only guard in `BeginPlay`** — tiles, `ClassifyBox`, the clipmap window, culling and
  streaming distance all assume **world-axis-aligned** boxes. A rotation or non-unit scale breaks that
  *structurally*, not gracefully. Logs an Error naming the offending rotation/scale.
- CODEMAP §3.5 and §6 document the convention.

**Nice property:** with the actor at the origin every conversion is identity, so this is a **no-op**
until the actor moves. Verify by building, confirming nothing changed, *then* moving the actor.
Jahni built it: **"seems to work fine."**
