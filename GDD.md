# Game Design Document

**Status:** first consolidation, 2026-08-17. Distilled from `COMPOSER-NOTES.md`, which stays as the
working transcript (chronological, with wrong turns preserved on purpose). This document is the
opposite: **the decided state, without the journey.**

> **Where this file lives, and why it is the wrong place.** This is a design document for *the game*.
> Most of it is not about VoxelForge at all — the city, combat, the economy and seasons are all
> outside the plugin. It lives here only because the plugin folder is the only part of the project
> under version control. Move it to the project root once that becomes a repository.

**How to read this:** everything under **Decided** was settled by Jahni. Everything under **Open** has
not been. Where a design is mine rather than his, it says so. Nothing here is a proposal wearing a
decision's clothes.

---

## 1. The game, in a paragraph

A fantasy exploration and combat RPG, online-first and party-based. The setting is medieval-ish and
progresses toward better and more modern infrastructure without ever leaving fantasy. Players descend
from a city on the surface into an endless underworld to find the resources that surface has none of,
and bring them back to build that city up. Side systems: housing and farming. The underworld is
generated, shared by everyone, and replaced every season. The city is hand-built, shared by everyone,
and permanent.

## 2. The premise

**Civilization emerged from the depths.** The first strate below the surface is artificial — someone
built it — and it holds crystals with humans inside them. The game opens on one of those humans being
un-crystallized.

The surface is a sky island, or a mountain so tall nobody can descend it. Either way it is small and it
has no resources. Everything a civilization needs has to come from below.

So descending is not conquest. **It is a return.** You are going back to where you came from, and
nobody remembers why you left.

## 3. The core loop

> The surface is limited, so you must descend. The depths supply resources. Resources build the
> settlement. The settlement lets you descend further.

Every system in this document hangs off that loop, and it closes. The sky island does quiet work in
it: it explains why there is no trade with anywhere else, why the depths are the only economy, and why
going down is the civilization's whole project rather than an adventurer's hobby.

---

## 4. Vocabulary

Two words got used for the same thing early on and caused real confusion. Pinned here.

**THE SURFACE / THE CITY** — a hand-authored, ordinary Unreal level. Not voxel terrain, not generated,
not part of VoxelForge. Communal and identical for every player. **It never resets.**

**THE DEPTHS** — everything VoxelForge generates: bounded, subterranean, a stack of strates below the
city's hole. Some strates only *mimic* being outdoors. **This is what is regenerated each season.**

**A SEASON** — one generation of the depths, roughly six to twelve months, plus the settlement tier the
community earns during it.

---

## 5. The city

### Decided

The city is **communal** — the same place for everyone, not instanced — and **permanent**. The season
wipe never touches it.

It **grows across seasons**, one tier at a time: camp, village, town, city, advanced city. Players
gather resources in the depths and donate them to the mayor or equivalent; a global counter rises until
the next tier is reached. A season is the unit that advances it. The game opens at *camp*.

Because groups never meet in the depths, **the city is the entire social surface of the game.** Trade,
the retrieval guild, shared builds, and simply seeing another person all happen there.

### Design rules

**A tier grants access and convenience, never power.** Services, workshops, NPCs, a bank, fast travel,
new quests, better aesthetics — but not stronger starting gear or stat bonuses. Otherwise players
arriving in season eight begin meaningfully stronger than those in season one, the early strates
trivialize, and the difficulty curve erodes a little every season until the top of the world is
scenery. This is the same rule the depth economy already follows: **buy access and time, never
numbers.**

**The city should be a ledger.** Somebody arriving in season eight built none of it. That is either
alienating or the best first impression in the game, depending entirely on whether the city records
*who* built it — names on the aqueduct, a monument per tier listing that season's contributors. New
players then walk through evidence that real people made this place, and long-term players get
something permanent, which pairs with the titles that already survive wipes.

### Open

- Is the tier threshold a fixed number, or does it scale with the active population? A fixed number
  stalls forever with a small playerbase and is cleared in a week with a large one. *(My
  recommendation: target one tier per season and retune the threshold at each season boundary, which
  is a moment that already exists for other reasons.)*
- Where housing sits relative to the city.

---

## 6. The depths

### Decided

Bounded, but **gigantic**. Not infinite — that choice was made deliberately, and it buys a great deal.

**No bottom.** The descent does not end.

**Boss strates roughly every five levels**, and they are **intermediary, not terminal** — gates that
punctuate the descent rather than finish it. A boss is where the transport network's next stop opens.

**Regenerated every season** from a single seed shared by every player.

Some strates are open, cathedral-sized spaces lit from within that read as outdoors. There is no sun
anywhere in the depths.

### What bounding buys

Because the depths are finite, the generator can examine an entire strate rather than sampling it. That
turns the entrance-and-exit guarantee into a **proof**, and it makes "the community has explored 34% of
this season" a real number rather than a slogan.

---

## 7. A strate

### Decided

**The primordial law: every strate has an entrance and an exit, exposed to air, and findable.** This is
the one invariant that may never fail.

**Danger is depth times a per-strate multiplier**, with a rare chance of an easier strate appearing
deep. Danger drives which creatures live there, which environmental hazards exist, and what loot drops.
It does **not** drive what *kind* of place a strate is.

**A strate's identity is unrelated to its depth.** Two seeds differ in everything — not the same
strates spawn, nor the same details. You could meet anything at any depth.

**Anomalous strates are rare.** The example Jahni gave is "dreamcore" — repetitive patterns, a strange
sense of emptiness. They should be rare enough that finding one is *puzzling*, because it does not have
its place there. They deliberately do **not** blend with their neighbours; the wrongness lives in the
seam.

**Size varies enormously** — from a crawlspace to a two-kilometre cathedral.

### Design rules

**Strangeness needs ordinary to be strange against.** If every strate is peculiar, peculiar becomes the
baseline and nothing reads as uncanny. A world should guarantee a share of grounded, plausible strates
so the strange ones land.

**A rare easy strate should be a designed type, not a low roll** — visibly calm, few creatures, a place
a descent exhales. The natural home for a safe camp or a merchant.

**The mirror of that is stronger: a rare danger spike near the surface.** A place at −300 that is
absolutely not for you yet. You find it early, it kills you, and it sits in your head for forty hours.
In a single shared world that becomes season-wide news.

**Every anomaly must be legible.** Unsignalled variance reads as bad tuning, not design. And danger in
particular must be **readable at a strate's entrance, before the player commits** — warned-and-ignored
is the best death in games; unwarned is the worst.

### Open

- Adjacency rules: what may sit next to what. Confirmed as wanted, never specified.

---

## 8. Descending

### Decided

**A party of up to six.** The descent is done together.

**Instanced per group.** Different groups never meet in the depths. Today this runs on a local server —
one player's machine or a dedicated one — fed by an external database for community information.

**Two ways back up.** Walking back through the tunnels is content, not a chore. Alternatively, once a
strate has been visited it opens a passage onward, so transport — an elevator or similar — can carry
you down, and you choose which strate to travel to.

**Transport unlocks are per-player for now.** A world-level unlock, where the first group through opens
the way for everyone, is a possible future — gated on strates being large enough that finding the next
passage is not a ten-minute errand. That is a decision to make from data, not in advance.

### Open

- **How long is a run?** Still unknown, and it constrains a great deal: strate size, how dense the
  transport stops need to be, how deep a season can plausibly reach.
- Does danger scale with party size, or are strates tuned for a nominal group?

---

## 9. Progression and the economy

### Decided

**Gear stats have a ceiling — somewhere around depth 30.** Loot found deeper has the *same* stats.

**What depth buys instead:** better natural rolls, so an item costs less material to level up; and
materials, enchantments and effects that exist nowhere else.

**Beyond the ceiling, going deeper is about pride, challenge, and seeing new things.** That is the
intent, not an accident.

**Depth-locked materials are tagged by difficulty tier, not by strate.** Their placement stays random,
so *where* a material is has to be rediscovered every season — which in a one-seed shared world turns
into a community question with a fresh answer each time. A fixed depth would be solved once and stay
solved forever.

**Trading will exist**, lightly at first, and becomes much better once the city is a real social hub.

### Design rules

**Buy access and time, never numbers.** Applied to both depth and city tier, which means the economy has
one principle rather than two competing ones.

**Guarantee what recipes depend on; leave the rest to chance.** A material a progression recipe needs
must be placed within reach, or a season ships a dead end that reads as a bug. A purely optional
material can be absent, and a season lacking one gains an identity.

**Since the depths are gigantic, "guaranteed" must mean a density, not a location.** Somewhere in an
enormous strate is unfindable. Near enough to the spine and common enough to actually meet is not.

### Open

- Where exactly the gear ceiling sits, and how fast danger climbs past it — that single rate is the
  entire endgame difficulty curve.

---

## 10. Death

### Decided

Dying in the depths leaves your **corpse, possessed**. You fight it to get your cargo back.

**The corpse copies your weapons, and you keep yours.** It is a mirror match, not a disarming — you
never return to a fight underequipped.

**Only you can claim your loot.** No player can take it.

**A corpse run is a detour, not an evening — ten minutes at worst.**

**An NPC tells you where your corpse was last seen**, for a very small fee.

**Possible later:** dead players appear in other groups' instances, and killing such a corpse earns
gold from a retrieving guild — never that player's loot.

### Design rules

**Place the corpse; do not make it walk to you.** On death it should appear at a landmark near the
nearest passage or transport stop, and wander only within that neighbourhood. That turns the ten-minute
cap from a hope into a guarantee regardless of where you actually died.

**Charge for precision, not for existence.** The general area — "last seen near the Sink" — should be
free. Pay only for the exact position or live tracking. Otherwise the fee is not a decision, and worse,
a broke player after a bad run cannot recover their cargo at all.

**Copied weapons must never be lootable.** If the corpse drops a copy of your sword you have built an
item duplicator and it will be found within a week. Copied gear vanishes with the corpse; only cargo
drops.

**The corpse should be a combat-shaped version of you, not a literal copy.** A support build's literal
mirror heals itself and cannot be beaten, for reasons that have nothing to do with skill.

**Give the player a deliberate edge.** A perfect mirror is a coin flip, and you often arrive having
already spent your consumables. You have potions and can retreat; it does not and cannot. That is what
makes the fight winnable through preparation rather than luck.

**The bounty hunter and the player retrieval guild are one system with two front ends** — players are
the fast, uncertain path; the NPC is the slow, guaranteed one. Either way the cargo returns to you and
the retriever is paid by the guild, not out of your pocket.

### Open

- What happens if you die while fighting your corpse — one corpse that absorbs the last, or do they
  stack?

---

## 11. Housing and farming

### Decided

**Housing is an instanced personal level** — your own small world, never wiped, never re-placed.

**Its ground is a snapshot of land you chose** in the depths and claimed. Your home is *from* somewhere.
After the season wipe it is the last surviving fragment of a world that no longer exists.

**One template level populated from save data** — a stored terrain region plus a list of placed
structures. Not one map per player.

**Invites**, because houses can be shared and visited.

**Anything that grows is computed from elapsed real time on entry**, not ticked — the instance is not
loaded while you are away.

### Open

- Whether players can build underground in the depths proper. The idea is alive, undecided.
- Whether the surface offers natural buildable space, or whether terrain-editing tools cover it.

---

## 12. The verbs

### Digging — decided

Digging is for **breaking walls that hide something behind them**, **uncovering half-buried ruins**,
**small shortcuts within a strate**, and **terraforming for builds**. It is a reveal-and-shape verb.

It is **not** a movement verb, and it is not the primary way through the world.

Descending by digging is possible but effortful: each strate's floor and ceiling are sealed, and the
seal must be dug through. Passages and transport are the fast path, by design.

### Combat — decided

MMORPG-like and dynamic: ranged and melee, magical and physical, stuns, roots, mutes, area effects,
targeted area effects, damage over time.

**Party of six. No friendly fire** — an area effect in a tunnel is simply strong, and that is accepted.

**First person must remain possible.**

### Design rules

**Telegraph the source, not the floor.** A circle painted on the ground only works if you can see the
ground, which is exactly what first person removes — you cannot see a marker under your own feet or
behind you. If the enemy visibly winds up instead, the threat is read off the creature. That works in
both perspectives, it survives lumpy voxel terrain where ground decals project badly, and reading an
animation is a better thing to ask of a player than reading a decal.

**Supporting both perspectives means owing both sets of constraints** — full third-person camera
clearance *and* source-readable telegraphs.

**Fights need space, and the generator must guarantee it.** Cramped tunnels are the worst possible
arena: no room to dodge a telegraph, no sightlines, no retreat, and with six players' effects in an
eight-metre corridor nobody can see anything. Rough targets, in the generator's own units of 8 m
chunks: a boss arena wants 3 to 4 chunks across, an ordinary fight space about 2, and roughly 1 chunk
of headroom for the camera.

**Verticality is what caves can offer that a flat arena cannot.** MMO fights are overwhelmingly flat. A
fight across ledges and drops is genuinely different, and it is the reason to fight underground at all.

### Open

- Creatures. Nothing decided. Danger and theme tags will select them, but *what they are* is unwritten,
  and they are most of what makes a strate feel like a place rather than a shape.

---

## 13. The season

### Decided

The depths are wiped and regenerated roughly every six to twelve months.

**Survives a wipe:** the city and its tier, account progression, titles, personal blueprints, housing
instances.

**Dies with the wipe:** the depths, the shared build pool, the transport unlock network.

### Why this is good rather than merely tolerable

The world is impermanent and the civilization is not. *We lost our maps, but we built a town.* Season
eight arrives in a place season three made, which gives a season's end a meaning beyond freshness.

It also solves problems that would otherwise be permanent. Every long-lived exploration game eventually
dies of its own wiki — everything is solved, every secret is mapped, nothing is left to find. A wipe
resets that, and the whole community re-explores together. It equally means a datamined map is worthless
one season later, so there is nothing to defend against.

And it frees the generator: because generation is only frozen *within* a season, it can be changed at
every boundary. The system never has to be finished before it meets players.

---

## 14. The community layer

### Decided

**One seed for everyone.** Every player's depths are the same depths, which is what makes shared
knowledge — wikis, guides, "the crystalline lake is in strate four" — possible at all.

**Players share builds**, as defined blueprint structures rather than raw terrain edits. Votes rank
them; well-liked builds appear in more players' worlds; disliked ones fall to the back of the queue and
sufficiently disliked ones are never shown again. **New and unvoted builds must be deliberately rolled
in**, or nothing new is ever seen and the system dies quietly.

**The build pool does not survive a season change** — nothing would be placed correctly. Top
contributors are rewarded with titles, tiered by population.

**Landmarks have names**, generated so that every player sees the same name for the same place before
anyone has ever visited it.

### Design rules

**The community layer must degrade gracefully.** The depths are generated from a seed and need no
database at all. If the database is unreachable, procedural names stand in for community ones, shared
builds simply do not appear, and votes queue locally. That is cheap now and painful to retrofit once a
hundred call sites assume the fetch succeeded.

**A build placed anywhere is yours; a build placed in a spot the world advertised is eligible to
spread.** That keeps building free while keeping automatic distribution safe.

**Names must survive contact with a wiki** — short, pronounceable, typeable. A name nobody can spell
cannot become community knowledge, which is the only reason names exist here.

**Auto-distributing player content needs a report, reject and rollback path**, and shared builds must be
individually removable by the player receiving them.

---

## 15. The opening

### Decided

A cinematic: civilization emerged from the depths. The first artificial strate holds crystals with
humans inside. One of them is un-crystallized — you.

You discover a **base camp**, and are told you could be an adventurer who braves the depths.

Then you descend to **strate 2 — the first generated strate.**

### Why this is worth more than it looks

The first strate is hand-made, and the rest are generated. That is normally a production compromise
players must be prevented from noticing. Here the hand-made strate is **hand-made in the fiction too** —
artificial because someone built it, with the natural world lying beneath. A player who senses that
strate 1 feels different from strate 2 is reading the story correctly rather than spotting the seam.

That is rare, and it is worth protecting in every later decision.

---

## 16. Open questions, collected

Ordered roughly by how much they block other work.

1. **Is the descent fun?** Unverified, and undesignable — it needs a crude build and an evening of
   playing it. Everything in this document assumes the answer is yes.
2. **How long is a run?** Constrains strate size, transport density, and how deep a season reaches.
3. **Creatures.** Nothing decided. Blocks content, not the generator.
4. **The backend.** Local server plus an external database, per-group instances, moderation, shared
   builds. Entirely unspecified and carrying a lot of weight.
5. **Does danger scale with party size**, or are strates tuned for a nominal group?
6. **The tier threshold** — fixed, or scaled to population?
7. **Adjacency rules** between strates. Wanted, never specified.
8. **Underground building** — undecided.
9. **Where the gear ceiling sits**, and how fast danger climbs past it.

## 17. Deliberately deferred

- **Digging noise attracting creatures.** Liked, parked. Still the cheapest way to price digging in
  safety rather than in resources, if it is ever wanted.
- **Naming languages varying by depth** — older, stranger tongues deeper down. Liked, parked as still
  being a gradient.
- **World-level transport unlocks**, where the community opens the descent together. Gated on strates
  being big enough for the discovery to be worth something.
- **Shards** — more than one seed at a time. Only if the game grows enough to need them.
- **Singleplayer.** Never the intent; may become an option.

---

## Where the rest lives

- **`COMPOSER-NOTES.md`** — the working transcript this was distilled from, including the generator
  design (the composer, the world manifest, the measurement pass, stack assembly) and the reasoning
  behind everything above.
- **`ARCHITECTURE.md`**, **`CODEMAP.md`** — how VoxelForge actually works.
- **`fable-idea.md`** — the older ranked feature and performance roadmap. Predates most of this.
